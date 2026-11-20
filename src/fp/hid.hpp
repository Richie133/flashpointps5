// Flashpoint PS5 - USB keyboard and mouse plugged into the PS5.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace hui::fp::hid
{

constexpr std::uint32_t kMouseLeft = 1;
constexpr std::uint32_t kMouseRight = 2;
constexpr std::uint32_t kMouseMiddle = 4;

// What the keyboard and mouse did since the last read.
struct Frame
{
    std::vector<std::uint16_t> pressed; // USB HID usages, with auto-repeat
    std::vector<char> text;             // characters typed (US layout)
    int dx = 0, dy = 0, wheel = 0;
    std::uint32_t buttons = 0, clicked = 0;
    bool shift = false;
    bool keyboard = false, mouse = false; // connected
};

// Loads libSceKeyboard and libSceMouse (sysmodules, looked up at run time,
// so a PS5 without them still starts the app). user: the foreground user.
bool open(int user);
Frame read();

// HID usages used by the app.
namespace key
{
constexpr std::uint16_t kEnter = 0x28, kEscape = 0x29, kBackspace = 0x2A, kTab = 0x2B, kSpace = 0x2C;
constexpr std::uint16_t kDelete = 0x4C, kRight = 0x4F, kLeft = 0x50, kDown = 0x51, kUp = 0x52;
constexpr std::uint16_t kPageUp = 0x4B, kPageDown = 0x4E, kHome = 0x4A, kEnd = 0x4D, kF1 = 0x3A;
} // namespace key

} // namespace hui::fp::hid
