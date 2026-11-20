// Flashpoint PS5 - application entry point.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opens the display, controller and audio, starts (or finds) the Flashpoint
// background service, then every frame: read input, update, draw, present.

#include "core/input.hpp"
#include "core/save_file.hpp"
#include "fp/app.hpp"
#include "fp/audio.hpp"
#include "fp/backend.hpp"
#include "fp/hid.hpp"
#include "fp/images.hpp"
#include "fp/services.hpp"
#include "gfx/draw_list.hpp"
#include "gfx/font.hpp"
#include "gfx/renderer.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "ui/fonts.hpp"

#include <GL/glcorearb.h>

#include <cstdint>
#include <span>
#include <string>

namespace
{

constexpr const char *kAssets = "/app0/assets";
// A title's sandbox has no /data: settings live in its own storage.
constexpr const char *kSettings = "/download0/fpps5/settings.json";

bool load_font(hui::gfx::Renderer &renderer, const char *name, hui::gfx::Font *font, hui::ui::FontRef *ref)
{
    std::string data;
    const std::string path = std::string(kAssets) + "/fonts/" + name;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        hui::sys::log("[FP] font %s failed: %s", name, font->error().c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

} // namespace

int main()
{
    using namespace hui;
    sys::log("[FP] entry");

    ps5::Display display;
    if (!display.open(1920, 1080))
    {
        sys::log("[FP] fatal: display open failed");
        sys::park();
    }

    gfx::Renderer renderer;
    gfx::Font regular;
    gfx::Font bold;
    gfx::Font pixel;
    fp::Fonts fonts;
    if (!renderer.init() || !load_font(renderer, "atkinson-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, "atkinson-bold.huifont", &bold, &fonts.bold) ||
        !load_font(renderer, "silkscreen.huifont", &pixel, &fonts.pixel))
    {
        sys::log("[FP] fatal: renderer init failed");
        sys::park();
    }

    ps5::Pad pad;
    pad.open();
    InputTracker tracker;
    fp::services::init();
    fp::audio::init(kAssets);

    fp::Backend backend(std::string(kAssets) + "/payload/flashpoint-ps5.elf");
    fp::Images images(backend);
    fp::App app(fonts, backend, images, kSettings);
    backend.connect();

    gfx::DrawList list;
    PadSample samples[64];
    PadSample last_pad;
    bool hid_open = false;
    std::uint64_t frames = 0;
    std::int64_t last = sys::monotonic_us();
    for (;;)
    {
        const std::int64_t now = sys::monotonic_us();
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last) / 1e6f;
        last = now;
        if (dt > 0.05f)
            dt = 0.05f;
        const std::size_t count = pad.read(samples);
        if (count > 0)
            last_pad = samples[count - 1];
        const InputFrame input =
            tracker.update(std::span<const PadSample>(samples, count), static_cast<std::uint64_t>(now));
        const fp::hid::Frame hid = hid_open ? fp::hid::read() : fp::hid::Frame{};

        app.update(input, last_pad, hid, dt);

        list.clear();
        app.draw(list);
        renderer.begin();
        renderer.draw(list);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, display.width(), display.height());
        if (!display.swap())
        {
            sys::log("[FP] fatal: swap failed frame=%llu error=%s", static_cast<unsigned long long>(frames),
                     ps5::egl_error_name(display.last_error()));
            sys::park();
        }
        if (++frames == 1)
        {
            const bool hidden = sys::hide_splash_screen();
            sys::log("[FP] ready splash_hidden=%d", hidden ? 1 : 0);
        }
        else if (frames == 30 && app.begin_usb())
        {
            // after the first frames, with a guard: a crash here turns them off
            hid_open = fp::hid::open(pad.user());
            app.end_usb(hid_open);
            sys::log("[FP] usb keyboard/mouse %s", hid_open ? "open" : "unavailable");
        }
    }
}
