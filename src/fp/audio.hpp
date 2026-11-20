// Flashpoint PS5 - music and interface sounds.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace hui::fp::audio
{

enum class Sfx
{
    move,
    select,
    back,
    tab,
    play,
    error,
    open,
    count,
};

// Loads the WAV files from <assets>/audio and starts the output thread.
// Without audio output everything below does nothing.
bool init(const std::string &assets);

void play(Sfx sfx);

// The intro music, from the start.
void play_music();
void stop_music(float fade_seconds);
// Seconds into the music, or a negative number when it isn't playing.
float music_position();

// 0..1 each.
void set_volume(float music, float sounds);

} // namespace hui::fp::audio

namespace hui::fp::audio_out
{
// Platform side: a 48 kHz stereo 16-bit stream in blocks of 256 frames.
bool open();
// Blocks until the block can be queued.
void write(const short *frames);
} // namespace hui::fp::audio_out
