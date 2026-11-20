// Flashpoint PS5 - audio output through sceAudioOut (as Ruffle Flash PS5 does).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fp/audio.hpp"
#include "platform/ps5/system.hpp"

extern "C"
{
    int sceAudioOutInit(void);
    int sceAudioOutOpen(int user, int type, int index, unsigned length, unsigned rate, unsigned format);
    int sceAudioOutOutput(int handle, const void *buffer);
}

namespace hui::fp::audio_out
{

namespace
{
int g_handle = -1;
constexpr int kSystemUser = 0xFF;
constexpr int kPortMain = 0;
constexpr unsigned kS16Stereo = 1;
constexpr int kAlreadyInitialized = static_cast<int>(0x8026000e);
} // namespace

bool open()
{
    const int init = sceAudioOutInit();
    if (init < 0 && init != kAlreadyInitialized)
    {
        sys::log("[FP] sceAudioOutInit=0x%08x", static_cast<unsigned>(init));
        return false;
    }
    g_handle = sceAudioOutOpen(kSystemUser, kPortMain, 0, 256, 48000, kS16Stereo);
    if (g_handle < 0)
    {
        sys::log("[FP] sceAudioOutOpen=0x%08x", static_cast<unsigned>(g_handle));
        return false;
    }
    return true;
}

void write(const short *frames)
{
    if (g_handle >= 0)
        sceAudioOutOutput(g_handle, frames);
}

} // namespace hui::fp::audio_out
