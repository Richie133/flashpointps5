// Flashpoint PS5 - USB keyboard and mouse on the console.
// libSceKeyboard and libSceMouse aren't loaded for a title. As Ruffle Flash
// PS5 does, they come in through their sysmodule IDs (0x0106, 0x00a9) and
// the functions are then looked up, so the eboot imports nothing from them
// and starts on any system.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fp/hid.hpp"
#include "fp/hid_records.hpp"
#include "platform/ps5/system.hpp"

#include <cstdio>
#include <cstring>

extern "C"
{
    int sceSysmoduleLoadModule(std::uint16_t id);
    int sceKernelGetModuleList(int *handles, std::size_t count, std::size_t *actual);
    int sceKernelGetModuleInfo(int handle, void *info);
    int sceKernelDlsym(int handle, const char *symbol, void **address);
    int sceKernelLoadStartModule(const char *path, std::size_t argc, const void *argv, std::uint32_t flags,
                                 void *opt, int *result);
}

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

// The handle of a loaded module whose name contains `module`, or -1.
int find_module(const char *module)
{
    static int handles[512];
    std::size_t count = 0;
    if (sceKernelGetModuleList(handles, 512, &count) != 0)
        return -1;
    // SceKernelModuleInfo's size differs between systems: try a few.
    static const std::uint64_t sizes[] = {0x160, 0x1a8, 0x1b0, 0x158, 0x200};
    for (std::size_t i = 0; i < count; ++i)
    {
        for (std::uint64_t size : sizes)
        {
            struct
            {
                std::uint64_t size;
                char name[256];
                std::uint8_t rest[0x400];
            } info;
            std::memset(&info, 0, sizeof(info));
            info.size = size;
            if (sceKernelGetModuleInfo(handles[i], &info) != 0)
                continue;
            if (std::strstr(info.name, module))
                return handles[i];
            break;
        }
    }
    return -1;
}

bool load(Api &api)
{
    const int rc = sceSysmoduleLoadModule(api.sysmodule);
    int handle = find_module(api.module);
    if (handle < 0)
    {
        char path[96];
        std::snprintf(path, sizeof(path), "/system/common/lib/%s.sprx", api.module);
        handle = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
    }
    if (handle < 0)
    {
        sys::log("[FP] %s: sysmodule 0x%x, module not found", api.what, static_cast<unsigned>(rc));
        return false;
    }
    void *fns[3] = {nullptr, nullptr, nullptr};
    for (int i = 0; i < 3; ++i)
        sceKernelDlsym(handle, api.names[i], &fns[i]);
    api.init = reinterpret_cast<int (*)(void)>(fns[0]);
    api.open = reinterpret_cast<int (*)(int, int, int, const void *)>(fns[1]);
    api.read = reinterpret_cast<int (*)(int, void *, int)>(fns[2]);
    const bool ok = api.init && api.open && api.read;
    sys::log("[FP] %s: module 0x%x, functions %s", api.what, static_cast<unsigned>(handle), ok ? "found" : "missing");
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
