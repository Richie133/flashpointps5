// Flashpoint PS5 - the app's settings as a small JSON file.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fp/settings.hpp"

#include "core/save_file.hpp"
#include "fp/json.hpp"

#include <algorithm>
#include <cstdio>

namespace hui::fp
{

Settings Settings::load(const std::string &path)
{
    Settings s;
    std::string text;
    JsonValue root;
    if (!save::read_file(path, &text) || !parse_json(text, root) || !root.is_object())
        return s;
    const auto flag = [&](const char *key, bool &out) {
        if (root[key].is_bool())
            out = root[key].as_bool();
    };
    const auto number = [&](const char *key, float &out) {
        if (root[key].is_number())
            out = std::clamp(static_cast<float>(root[key].as_number()), 0.0f, 1.0f);
    };
    flag("intro", s.intro);
    number("music", s.music);
    number("sounds", s.sounds);
    flag("ps5Only", s.ps5_only);
    flag("showExtreme", s.show_extreme);
    flag("webAdult", s.web_adult);
    flag("usbInput", s.usb_input);
    flag("usbTrying", s.usb_trying);
    flag("desc", s.desc);
    if (root["tileSize"].is_number())
        s.tile_size = std::clamp(static_cast<int>(root["tileSize"].as_number()), 0, 2);
    if (root["sort"].is_string())
        s.sort = root["sort"].as_string();
    return s;
}

void Settings::save(const std::string &path) const
{
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"intro\":%s,\"music\":%.2f,\"sounds\":%.2f,\"ps5Only\":%s,\"showExtreme\":%s,"
                  "\"webAdult\":%s,\"tileSize\":%d,\"usbInput\":%s,\"usbTrying\":%s,\"desc\":%s,\"sort\":",
                  intro ? "true" : "false", music, sounds, ps5_only ? "true" : "false",
                  show_extreme ? "true" : "false", web_adult ? "true" : "false", tile_size,
                  usb_input ? "true" : "false", usb_trying ? "true" : "false", desc ? "true" : "false");
    std::string out = buf;
    append_json_string(out, sort);
    out += "}\n";
    const std::size_t slash = path.rfind('/');
    if (slash != std::string::npos)
        save::ensure_directory(path.substr(0, slash));
    save::write_atomic(path, out);
}

} // namespace hui::fp
