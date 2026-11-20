// Flashpoint PS5 - keyboard and mouse records to key presses, text and motion.
// Record layouts and handling follow Ruffle Flash PS5's src/hid.rs (GPL-3.0),
// which is tested on the console.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fp/hid.hpp"
#include "fp/hid_records.hpp"

#include <algorithm>

namespace hui::fp::hid
{

namespace
{

// The character a usage types on a US keyboard, or 0.
char char_for(std::uint16_t usage, bool shift, bool caps)
{
    if (usage >= 0x04 && usage <= 0x1D)
    {
        const char c = static_cast<char>('a' + (usage - 0x04));
        return (shift != caps) ? static_cast<char>(c - 32) : c;
    }
    static const char digits[] = "1234567890";
    static const char shifted[] = "!@#$%^&*()";
    if (usage >= 0x1E && usage <= 0x27)
        return shift ? shifted[usage - 0x1E] : digits[usage - 0x1E];
    switch (usage)
    {
    case 0x2C: return ' ';
    case 0x2D: return shift ? '_' : '-';
    case 0x2E: return shift ? '+' : '=';
    case 0x2F: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    default: return 0;
    }
}

bool is_modifier(std::uint16_t usage) { return usage >= 0xE0 && usage <= 0xE7; }

} // namespace

void Tracker::keyboard_record(const KeyboardRecord &r, Port &port, Frame &frame)
{
    if (r.connected != port.connected)
        port.connected = r.connected != 0;
    std::vector<std::uint16_t> now;
    if (r.connected && !r.intercepted)
    {
        for (int bit = 0; bit < 8; ++bit)
        {
            if (r.modifiers & (1u << bit))
                now.push_back(static_cast<std::uint16_t>(0xE0 + bit));
        }
        for (std::uint16_t k : r.keycodes)
        {
            if (k > 3 && std::find(now.begin(), now.end(), k) == now.end())
                now.push_back(k);
        }
        caps_ = (r.leds & 2) != 0;
    }
    for (std::uint16_t k : now)
    {
        if (std::find(port.held.begin(), port.held.end(), k) == port.held.end() && !is_modifier(k))
            press(k, frame);
    }
    port.held = std::move(now);
}

void Tracker::press(std::uint16_t usage, Frame &frame)
{
    frame.pressed.push_back(usage);
    if (const char c = char_for(usage, shift_, caps_))
        frame.text.push_back(c);
    repeat_key_ = usage;
    repeat_at_ = now_ + 0.45;
}

void Tracker::finish(Frame &frame, double now)
{
    now_ = now;
    shift_ = false;
    bool held_repeat = false;
    for (const Port &p : keyboards_)
    {
        for (std::uint16_t k : p.held)
        {
            shift_ |= k == 0xE1 || k == 0xE5;
            held_repeat |= k == repeat_key_;
        }
    }
    if (!held_repeat)
        repeat_key_ = 0;
    if (repeat_key_ && now >= repeat_at_)
    {
        press(repeat_key_, frame);
        repeat_at_ = now + 0.035;
    }
    frame.shift = shift_;
    frame.keyboard = std::any_of(keyboards_.begin(), keyboards_.end(), [](const Port &p) { return p.connected; });
    frame.mouse = std::any_of(mice_.begin(), mice_.end(), [](const Port &p) { return p.connected; });
}

void Tracker::mouse_record(const MouseRecord &r, Port &port, Frame &frame)
{
    port.connected = r.connected != 0;
    if (!r.connected || (r.buttons & 0x80000000u))
    {
        port.buttons = 0;
        return;
    }
    frame.dx += r.x;
    frame.dy += r.y;
    frame.wheel += r.wheel;
    const std::uint32_t b = r.buttons & 0x1f;
    frame.clicked |= b & ~port.buttons;
    port.buttons = b;
    frame.buttons |= b;
}

} // namespace hui::fp::hid
