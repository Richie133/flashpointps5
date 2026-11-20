// Flashpoint PS5 - music and interface sounds: a small mixer on its own thread.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fp/audio.hpp"

#include "core/save_file.hpp"
#include "platform/ps5/system.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace hui::fp::audio
{

namespace
{

constexpr int kRate = 48000;
constexpr int kGrain = 256;

struct Clip
{
    std::vector<std::int16_t> samples; // interleaved
    int channels = 1;
    std::size_t frames() const { return channels ? samples.size() / static_cast<std::size_t>(channels) : 0; }
};

struct Voice
{
    const Clip *clip = nullptr;
    std::size_t at = 0;
    float gain = 1.0f;
};

struct State
{
    std::mutex lock;
    std::array<Clip, static_cast<std::size_t>(Sfx::count)> sfx;
    Clip music;
    std::vector<Voice> voices;
    bool music_on = false;
    std::size_t music_at = 0;
    float music_fade = 1.0f;
    float music_fade_step = 0.0f;
    float music_volume = 0.8f;
    float sfx_volume = 0.8f;
    bool ready = false;
};

State &state()
{
    static State s;
    return s;
}

std::uint32_t le32(const unsigned char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t le16(const unsigned char *p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

// 16-bit PCM WAV, 48 kHz, mono or stereo (what tools/make-audio.py writes).
bool load_wav(const std::string &path, Clip *clip)
{
    std::string data;
    if (!save::read_file(path, &data) || data.size() < 44)
        return false;
    const auto *p = reinterpret_cast<const unsigned char *>(data.data());
    if (std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0)
        return false;
    std::size_t at = 12;
    int channels = 0;
    while (at + 8 <= data.size())
    {
        const std::uint32_t size = le32(p + at + 4);
        if (std::memcmp(p + at, "fmt ", 4) == 0 && size >= 16)
        {
            const int format = le16(p + at + 8);
            channels = le16(p + at + 10);
            const std::uint32_t rate = le32(p + at + 12);
            const int bits = le16(p + at + 22);
            if (format != 1 || bits != 16 || rate != kRate || (channels != 1 && channels != 2))
                return false;
        }
        else if (std::memcmp(p + at, "data", 4) == 0 && channels)
        {
            const std::size_t bytes = std::min<std::size_t>(size, data.size() - at - 8);
            clip->channels = channels;
            clip->samples.resize(bytes / 2);
            std::memcpy(clip->samples.data(), p + at + 8, clip->samples.size() * 2);
            return true;
        }
        at += 8 + size + (size & 1);
    }
    return false;
}

void mix_thread()
{
    State &s = state();
    std::vector<float> acc(kGrain * 2);
    std::vector<short> out(kGrain * 2);
    for (;;)
    {
        std::fill(acc.begin(), acc.end(), 0.0f);
        {
            std::lock_guard<std::mutex> guard(s.lock);
            if (s.music_on)
            {
                const Clip &m = s.music;
                for (int i = 0; i < kGrain && s.music_on; ++i)
                {
                    if (s.music_at >= m.frames())
                    {
                        s.music_on = false;
                        break;
                    }
                    const float g = s.music_volume * s.music_fade / 32768.0f;
                    const std::size_t base = s.music_at * static_cast<std::size_t>(m.channels);
                    const float l = m.samples[base];
                    const float r = m.channels == 2 ? m.samples[base + 1] : l;
                    acc[i * 2] += l * g;
                    acc[i * 2 + 1] += r * g;
                    ++s.music_at;
                    if (s.music_fade_step > 0.0f)
                    {
                        s.music_fade -= s.music_fade_step;
                        if (s.music_fade <= 0.0f)
                            s.music_on = false;
                    }
                }
            }
            for (Voice &v : s.voices)
            {
                const Clip &c = *v.clip;
                const float g = v.gain * s.sfx_volume / 32768.0f;
                for (int i = 0; i < kGrain && v.at < c.frames(); ++i, ++v.at)
                {
                    const std::size_t base = v.at * static_cast<std::size_t>(c.channels);
                    const float l = c.samples[base];
                    const float r = c.channels == 2 ? c.samples[base + 1] : l;
                    acc[i * 2] += l * g;
                    acc[i * 2 + 1] += r * g;
                }
            }
            s.voices.erase(std::remove_if(s.voices.begin(), s.voices.end(),
                                          [](const Voice &v) { return v.at >= v.clip->frames(); }),
                           s.voices.end());
        }
        for (std::size_t i = 0; i < acc.size(); ++i)
        {
            const float v = std::clamp(acc[i], -1.0f, 1.0f);
            out[i] = static_cast<short>(v * 32767.0f);
        }
        audio_out::write(out.data());
    }
}

const char *const kNames[] = {"move", "select", "back", "tab", "play", "error", "open"};

} // namespace

bool init(const std::string &assets)
{
    State &s = state();
    int loaded = 0;
    for (int i = 0; i < static_cast<int>(Sfx::count); ++i)
        loaded += load_wav(assets + "/audio/" + kNames[i] + ".wav", &s.sfx[static_cast<std::size_t>(i)]);
    const bool music = load_wav(assets + "/audio/intro.wav", &s.music);
    if (!audio_out::open())
    {
        sys::log("[FP] audio: no output");
        return false;
    }
    sys::log("[FP] audio: %d sounds, music %d", loaded, music ? 1 : 0);
    s.ready = true;
    std::thread(mix_thread).detach();
    return true;
}

void play(Sfx sfx)
{
    State &s = state();
    if (!s.ready)
        return;
    const Clip &clip = s.sfx[static_cast<std::size_t>(sfx)];
    if (clip.samples.empty())
        return;
    std::lock_guard<std::mutex> guard(s.lock);
    // the same sound restarting (fast scrolling) replaces itself
    for (Voice &v : s.voices)
    {
        if (v.clip == &clip && v.at < clip.frames() / 3)
        {
            v.at = 0;
            return;
        }
    }
    if (s.voices.size() < 12)
        s.voices.push_back(Voice{&clip, 0, sfx == Sfx::move ? 0.55f : 1.0f});
}

void play_music()
{
    State &s = state();
    if (!s.ready || s.music.samples.empty())
        return;
    std::lock_guard<std::mutex> guard(s.lock);
    s.music_at = 0;
    s.music_fade = 1.0f;
    s.music_fade_step = 0.0f;
    s.music_on = true;
}

void stop_music(float fade_seconds)
{
    State &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    if (fade_seconds <= 0.0f)
        s.music_on = false;
    else
        s.music_fade_step = 1.0f / (fade_seconds * kRate);
}

float music_position()
{
    State &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    return s.music_on ? static_cast<float>(s.music_at) / kRate : -1.0f;
}

void set_volume(float music, float sounds)
{
    State &s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    s.music_volume = std::clamp(music, 0.0f, 1.0f);
    s.sfx_volume = std::clamp(sounds, 0.0f, 1.0f);
}

} // namespace hui::fp::audio
