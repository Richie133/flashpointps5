// Flashpoint PS5 - libSceKeyboard / libSceMouse record layouts (from Ruffle
// Flash PS5's hid.rs, from the device-tested ps5-native-gamepad-input-research
// notes; GPL-3.0) and the shared record tracker.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fp/hid.hpp"

#include <cstdint>
#include <vector>

namespace hui::fp::hid
{

struct KeyboardRecord
{
    std::uint64_t timestamp;
    std::uint8_t intercepted;
    std::uint8_t reserved0[7];
    std::uint8_t connected;
    std::uint8_t reserved1[3];
    std::int32_t length;
    std::uint32_t leds;
    std::uint32_t modifiers;
    std::uint16_t keycodes[16];
    std::uint8_t reserved2[32];
};
static_assert(sizeof(KeyboardRecord) == 96);

struct MouseRecord
{
    std::uint64_t timestamp;
    std::uint8_t connected;
    std::uint8_t reserved0[3];
    std::uint32_t buttons;
    std::int32_t x;
    std::int32_t y;
    std::int32_t wheel;
    std::int32_t tilt;
    std::uint8_t reserved1[8];
};
static_assert(sizeof(MouseRecord) == 40);

struct Port
{
    int handle = -1;
    bool connected = false;
    std::vector<std::uint16_t> held;
    std::uint32_t buttons = 0;
};

class Tracker
{
  public:
    std::vector<Port> keyboards_, mice_;

    void keyboard_record(const KeyboardRecord &r, Port &port, Frame &frame);
    void mouse_record(const MouseRecord &r, Port &port, Frame &frame);
    void finish(Frame &frame, double now);

  private:
    void press(std::uint16_t usage, Frame &frame);
    bool shift_ = false, caps_ = false;
    std::uint16_t repeat_key_ = 0;
    double repeat_at_ = 0, now_ = 0;
};

} // namespace hui::fp::hid
