// Flashpoint PS5 - PC stand-in for audio output: keeps time, plays nothing.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fp/audio.hpp"

#include <chrono>
#include <thread>

namespace hui::fp::audio_out
{

bool open() { return true; }

void write(const short *)
{
    std::this_thread::sleep_for(std::chrono::microseconds(256 * 1000000 / 48000));
}

} // namespace hui::fp::audio_out
