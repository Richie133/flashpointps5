// Flashpoint PS5 - system libraries the app doesn't import directly.
// The approach follows Ruffle Flash PS5 (GPL-3.0).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/modules.hpp"

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

namespace hui::ps5
{

namespace
{

// The handle of a loaded module whose name contains `module`, or -1.
int find_module(const char *module)
{
    static int handles[512];
    std::size_t count = 0;
    if (sceKernelGetModuleList(handles, 512, &count) != 0)
        return -1;
    // SceKernelModuleInfo's size differs between systems: try a few.
    static const std::uint64_t sizes[] = {0x160, 0x1a8, 0x1b0, 0x158, 0x200};
    for (std::size_t i = 0; i < count && i < 512; ++i)
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

} // namespace

int load_module(std::uint16_t sysmodule, const char *name)
{
    const int rc = sceSysmoduleLoadModule(sysmodule);
    int handle = find_module(name);
    if (handle < 0)
    {
        char path[96];
        std::snprintf(path, sizeof(path), "/system/common/lib/%s.sprx", name);
        handle = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
    }
    sys::log("[FP] %s: sysmodule 0x%x, module %x", name, static_cast<unsigned>(rc), static_cast<unsigned>(handle));
    return handle < 0 ? -1 : handle;
}

void *module_symbol(int handle, const char *symbol)
{
    void *address = nullptr;
    if (handle < 0 || sceKernelDlsym(handle, symbol, &address) != 0)
        return nullptr;
    return address;
}

} // namespace hui::ps5
