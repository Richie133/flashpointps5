// Flashpoint PS5 - console services the app uses: the on-screen keyboard and
// the web browser. src/platform/ps5/services.cpp on the console,
// host/services_host.cpp on a PC.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace hui::fp::services
{

void init();

// Shows the system keyboard. False when it can't open.
bool keyboard_open(const std::string &title, const std::string &text);

enum class KeyboardState
{
    idle,
    open,
    done,      // text holds what was typed
    cancelled,
};
// Call every frame while it's open.
KeyboardState keyboard_poll(std::string *text);

// Opens the PS5 web browser at url (HTML5 games).
bool open_browser(const std::string &url);

} // namespace hui::fp::services
