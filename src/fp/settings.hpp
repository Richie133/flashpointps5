// Flashpoint PS5 - the app's settings, kept in the title's own storage.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace hui::fp
{

struct Settings
{
    bool intro = true;          // intro with music at start
    float music = 0.8f;         // 0..1
    float sounds = 0.7f;        // 0..1
    bool ps5_only = true;       // hide entries with no PS5 player
    bool show_extreme = false;  // Flashpoint's extreme entries
    bool web_adult = false;     // entries the websites mark NSFW
    int tile_size = 1;          // 0 small, 1 medium, 2 large
    bool usb_input = true;      // USB keyboard and mouse
    bool usb_trying = false;    // set while they start: a crash there turns them off
    std::string sort = "title"; // title, dateAdded, developer, releaseDate, random
    bool desc = false;

    static Settings load(const std::string &path);
    void save(const std::string &path) const;
};

} // namespace hui::fp
