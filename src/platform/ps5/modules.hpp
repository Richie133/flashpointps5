// Flashpoint PS5 - system libraries the app doesn't import directly.
// Some libraries (the keyboard dialog, USB keyboard and mouse) aren't loaded
// when a homebrew title starts, so a direct import of one of their
// functions is left at address 0 and calling it crashes. They're loaded
// through their sysmodule IDs instead and their functions looked up.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace hui::ps5
{

// Loads the library behind a sysmodule ID ("libSceImeDialog" names it in
// the module list) and returns its handle, or -1.
int load_module(std::uint16_t sysmodule, const char *name);

// A function of a loaded library, or nullptr.
void *module_symbol(int handle, const char *symbol);

} // namespace hui::ps5
