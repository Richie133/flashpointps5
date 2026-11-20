// Flashpoint PS5 - PC stand-in for the USB keyboard and mouse: the snapshot
// tool feeds pointer motion and typing through hid_host_push().
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fp/hid.hpp"

#include <mutex>

namespace hui::fp::hid
{

namespace
{
std::mutex g_lock;
Frame g_pending;
} // namespace

bool open(int) { return true; }

Frame read()
{
    std::lock_guard<std::mutex> guard(g_lock);
    Frame f = g_pending;
    g_pending = Frame{};
    f.mouse = true;
    f.keyboard = true;
    return f;
}

void hid_host_push(const Frame &f)
{
    std::lock_guard<std::mutex> guard(g_lock);
    g_pending.dx += f.dx;
    g_pending.dy += f.dy;
    g_pending.wheel += f.wheel;
    g_pending.buttons |= f.buttons;
    g_pending.clicked |= f.clicked;
    g_pending.pressed.insert(g_pending.pressed.end(), f.pressed.begin(), f.pressed.end());
    g_pending.text.insert(g_pending.text.end(), f.text.begin(), f.text.end());
}

} // namespace hui::fp::hid
