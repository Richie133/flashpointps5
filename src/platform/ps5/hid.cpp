// Flashpoint PS5 - USB keyboard and mouse on the console.
// libSceKeyboard and libSceMouse aren't loaded for a title. As Ruffle Flash
// PS5 does, they come in through their sysmodule IDs (0x0106, 0x00a9) and
// the functions are then looked up, so the eboot imports nothing from them
// and starts on any system.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fp/hid.hpp"
#include "fp/hid_records.hpp"
#include "platform/ps5/modules.hpp"
#include "platform/ps5/system.hpp"

#include <cstring>

namespace hui::fp::hid
{

namespace
{

struct Api
{
    const char *what;
    std::uint16_t sysmodule;
    const char *module;
    const char *names[3];
    int (*init)(void) = nullptr;
    int (*open)(int, int, int, const void *) = nullptr;
    int (*read)(int, void *, int) = nullptr;
};

Api g_apis[2] = {
    {"keyboard", 0x0106, "libSceKeyboard", {"sceKeyboardInit", "sceKeyboardOpen", "sceKeyboardRead"}},
    {"mouse", 0x00a9, "libSceMouse", {"sceMouseInit", "sceMouseOpen", "sceMouseRead"}},
};

Tracker g_tracker;

bool load(Api &api)
{
    const int handle = ps5::load_module(api.sysmodule, api.module);
    if (handle < 0)
    {
        sys::log("[FP] %s: module not found", api.what);
        return false;
    }
    api.init = reinterpret_cast<int (*)(void)>(ps5::module_symbol(handle, api.names[0]));
    api.open = reinterpret_cast<int (*)(int, int, int, const void *)>(ps5::module_symbol(handle, api.names[1]));
    api.read = reinterpret_cast<int (*)(int, void *, int)>(ps5::module_symbol(handle, api.names[2]));
    const bool ok = api.init && api.open && api.read;
    sys::log("[FP] %s: functions %s", api.what, ok ? "found" : "missing");
    return ok;
}

} // namespace

bool open(int user)
{
    bool any = false;
    for (int which = 0; which < 2; ++which)
    {
        Api &api = g_apis[which];
        if (user < 0 || !load(api))
            continue;
        api.init();
        auto &ports = which == 0 ? g_tracker.keyboards_ : g_tracker.mice_;
        // one USB receiver used keyboard 1, mouse 0: open every index taken
        for (int index = 0; index < 4; ++index)
        {
            const std::uint8_t param[8] = {};
            const int handle = api.open(user, 0, index, param);
            if (handle >= 0)
            {
                Port port;
                port.handle = handle;
                ports.push_back(port);
            }
            else if (index > 1)
            {
                break;
            }
        }
        sys::log("[FP] %s: %zu ports", api.what, ports.size());
        any |= !ports.empty();
    }
    return any;
}

Frame read()
{
    Frame frame;
    if (g_apis[0].read)
    {
        static KeyboardRecord records[16];
        for (Port &port : g_tracker.keyboards_)
        {
            const int n = g_apis[0].read(port.handle, records, 16);
            for (int i = 0; i < n && i < 16; ++i)
                g_tracker.keyboard_record(records[i], port, frame);
        }
    }
    if (g_apis[1].read)
    {
        static MouseRecord records[64];
        for (Port &port : g_tracker.mice_)
        {
            const int n = g_apis[1].read(port.handle, records, 64);
            for (int i = 0; i < n && i < 64; ++i)
                g_tracker.mouse_record(records[i], port, frame);
            if (n <= 0)
                frame.buttons |= port.buttons;
        }
    }
    g_tracker.finish(frame, static_cast<double>(sys::monotonic_us()) / 1e6);
    return frame;
}

} // namespace hui::fp::hid
