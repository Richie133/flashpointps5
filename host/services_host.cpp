// Flashpoint PS5 - PC stand-ins for the console services (snapshots, tests).
// The keyboard "types" the FP_KEYBOARD_TEXT environment variable.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fp/services.hpp"

#include <cstdio>
#include <cstdlib>

namespace hui::fp::services
{

namespace
{
bool g_open = false;
int g_frames = 0;
} // namespace

void init()
{
}

bool keyboard_open(const std::string &, const std::string &)
{
    g_open = true;
    g_frames = 0;
    return true;
}

KeyboardState keyboard_poll(std::string *text)
{
    if (!g_open)
        return KeyboardState::idle;
    if (++g_frames < 3)
        return KeyboardState::open;
    g_open = false;
    const char *typed = std::getenv("FP_KEYBOARD_TEXT");
    if (typed == nullptr)
        return KeyboardState::cancelled;
    *text = typed;
    return KeyboardState::done;
}

bool open_browser(const std::string &url)
{
    std::fprintf(stderr, "[FP] would open the browser at %s\n", url.c_str());
    return true;
}

} // namespace hui::fp::services
