// Flashpoint PS5 - headless PC run of the app: scripted input, PNG pictures.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// usage: fp_snapshots <assets dir> <output dir>
//
// Runs the same App, Backend and Images code as the console build through
// Mesa's surfaceless EGL (llvmpipe). The Backend talks to whatever answers on
// 127.0.0.1:22600, normally the PC build of the service (payload/, make -f
// Makefile.pc), so the pictures show real search results and images.

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
#include "gfx/gl_program.hpp"
#include "gfx/renderer.hpp"
#include "ui/fonts.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace hui::fp::hid
{
void hid_host_push(const Frame &f);
}

namespace
{

bool open_context()
{
    auto get_platform_display =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display = get_platform_display != nullptr
                             ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
                             : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API))
        return false;
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION,       4, EGL_CONTEXT_MINOR_VERSION, 5,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    return context != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

bool load_font(hui::gfx::Renderer &renderer, const std::string &path, hui::gfx::Font *font, hui::ui::FontRef *ref)
{
    std::string data;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        std::fprintf(stderr, "cannot load font %s\n", path.c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

// A step: a button, a tab, pointer motion or typing; then run `settle`
// seconds (and until the app is idle); then maybe save a picture.
struct Step
{
    const char *press; // button name, "tab:N", "move:x,y", "click", "wheel:N", "type:text"
    float settle;
    const char *picture;
};

const Step kTour[] = {
    {"tab:0", 2.0f, "02-home"},
    {"tab:1", 3.0f, "03-arcade"},
    {"right", 0.15f, nullptr},
    {"right", 0.15f, nullptr},
    {"down", 1.5f, "04-arcade-selected"},
    {"square", 0.4f, "05-sort-menu"},
    {"down", 0.1f, nullptr},
    {"cross", 2.5f, "06-sorted-by-date"},
    {"square", 0.2f, nullptr},
    {"up", 0.1f, nullptr},
    {"cross", 2.0f, nullptr},
    {"left", 0.1f, nullptr},
    {"left", 0.1f, nullptr},
    {"left", 0.4f, "07-sidebar"},
    {"down", 0.1f, nullptr},
    {"down", 0.1f, nullptr},
    {"down", 0.1f, nullptr},
    {"down", 0.1f, nullptr},
    {"cross", 2.5f, "08-platform-filter"},
    {"move:814,360", 0.6f, "09-pointer-hover"},
    {"click", 0.1f, nullptr},
    {"click", 4.0f, "10-double-click-play"},
    {"type:age", 2.5f, "11-keyboard-search"},
    {"tab:2", 3.0f, "12-theatre"},
    {"tab:3", 3.5f, "13-web"},
    {"move:640,180", 0.2f, nullptr},
    {"click", 1.5f, "14-web-selected"},
    // Flash Museum: the 18th sidebar row is its Games list
    {"move:180,878", 0.2f, nullptr},
    {"click", 3.0f, "14b-web-flash-museum"},
    {"wheel:-6", 1.0f, "14c-web-sidebar-scrolled"},
    {"tab:4", 1.5f, "15-logs"},
    {"tab:5", 0.8f, "16-config"},
    {"tab:6", 0.8f, "17-about"},
};

hui::InputFrame press(const std::string &n)
{
    using hui::Action;
    using hui::Direction;
    hui::InputFrame f;
    f.connected = true;
    const auto bit = [&](Action a) { f.pressed |= hui::action_bit(a); };
    if (n == "up")
        f.nav = Direction::up;
    else if (n == "down")
        f.nav = Direction::down;
    else if (n == "left")
        f.nav = Direction::left;
    else if (n == "right")
        f.nav = Direction::right;
    else if (n == "cross")
        bit(Action::confirm);
    else if (n == "circle")
        bit(Action::back);
    else if (n == "triangle")
        bit(Action::north);
    else if (n == "square")
        bit(Action::west);
    else if (n == "options")
        bit(Action::menu);
    return f;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <assets dir> <output dir>\n", argv[0]);
        return 2;
    }
    const std::string assets = argv[1];
    const std::string output = argv[2];
    const int width = 1920, height = 1080;
    if (!open_context())
    {
        std::fprintf(stderr, "no EGL context (Mesa surfaceless)\n");
        return 1;
    }
    hui::gfx::set_glsl_prefix("#version 450 core\n");
    hui::gfx::Renderer renderer;
    hui::gfx::Font regular, bold, pixel;
    hui::fp::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, assets + "/fonts/atkinson-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, assets + "/fonts/atkinson-bold.huifont", &bold, &fonts.bold) ||
        !load_font(renderer, assets + "/fonts/silkscreen.huifont", &pixel, &fonts.pixel))
        return 1;

    GLuint framebuffer = 0, color = 0;
    glGenFramebuffers(1, &framebuffer);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 1;
    hui::save::ensure_directory(output);
    std::remove((output + "/settings.json").c_str());

    hui::fp::services::init();
    hui::fp::audio::init(assets);
    hui::fp::Backend backend(assets + "/payload/flashpoint-ps5.elf");
    hui::fp::Images images(backend);
    hui::fp::App app(fonts, backend, images, output + "/settings.json");
    backend.connect();

    hui::gfx::DrawList list;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    stbi_flip_vertically_on_write(1);
    constexpr float kDt = 1.0f / 60.0f;
    hui::PadSample pad;
    const auto frame = [&](const hui::InputFrame &input) {
        app.update(input, pad, hui::fp::hid::read(), kDt);
        std::this_thread::sleep_for(std::chrono::microseconds(16667));
    };
    const auto picture = [&](const std::string &name) {
        list.clear();
        app.draw(list);
        renderer.begin();
        renderer.draw(list);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        renderer.present(framebuffer, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (std::size_t i = 3; i < pixels.size(); i += 4)
            pixels[i] = 255;
        const std::string path = output + "/" + name + ".png";
        const bool ok = stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4) != 0;
        std::fprintf(stderr, "wrote %s ok=%d\n", path.c_str(), ok ? 1 : 0);
        return ok;
    };
    const auto settle = [&](float seconds) {
        const int frames = static_cast<int>(seconds * 60.0f);
        for (int i = 0; i < frames || (!app.idle() && i < 60 * 20); ++i)
        {
            frame(hui::InputFrame{});
            if (i % 20 == 0)
            {
                list.clear();
                app.draw(list); // asks for images
            }
        }
    };

    // the intro, as it plays
    for (int i = 0; i < 80; ++i)
        frame(hui::InputFrame{});
    picture("01-intro-mark");
    for (int i = 0; i < 100; ++i)
        frame(hui::InputFrame{});
    picture("01-intro-name");
    for (int i = 0; i < 160; ++i)
        frame(hui::InputFrame{});
    picture("01-intro-tagline");

    // wait for the service
    for (int i = 0; i < 60 * 30; ++i)
    {
        frame(hui::InputFrame{});
        const hui::fp::Link link = backend.link();
        if (link != hui::fp::Link::connecting && link != hui::fp::Link::starting)
            break;
    }
    if (backend.link() != hui::fp::Link::ready)
    {
        app.open_tab(0);
        settle(0.5f);
        return picture("00-no-service") ? 0 : 1;
    }

    const auto run = [&](float seconds) {
        for (int i = 0; i < static_cast<int>(seconds * 60.0f); ++i)
        {
            frame(hui::InputFrame{});
            if (i % 20 == 0)
            {
                list.clear();
                app.draw(list);
            }
        }
    };
    const char *tour = std::getenv("FP_TOUR");
    if (tour && std::strcmp(tour, "sync") == 0)
    {
        // a PS5 with no library yet: it downloads itself
        app.open_tab(0);
        run(2.0f);
        bool ok = picture("20-sync-home");
        app.open_tab(1);
        run(2.5f);
        ok = picture("21-sync-arcade") && ok;
        run(8.0f);
        ok = picture("22-synced-arcade") && ok;
        app.open_tab(0);
        run(1.5f);
        return picture("23-synced-home") && ok ? 0 : 1;
    }

    if (tour && std::strcmp(tour, "themes") == 0)
    {
        // every theme on the Arcade (Config's first row cycles them), then
        // the app's own keyboard (run with FP_NO_IME=1)
        const auto tap = [&](const char *button) {
            frame(press(button));
            run(0.15f);
        };
        const char *const names[] = {"flashpoint", "y8", "daylight", "midnight", "ember", "terminal"};
        bool ok = true;
        for (const char *name : names)
        {
            app.open_tab(1);
            run(2.5f);
            ok = picture(std::string("30-theme-") + name) && ok;
            app.open_tab(5);
            run(0.3f);
            if (std::strcmp(name, "y8") == 0)
                ok = picture("31-theme-y8-config") && ok;
            tap("cross");
        }
        app.open_tab(1);
        run(1.5f);
        tap("triangle");
        for (const char *b : {"right", "right", "cross", "down", "cross", "right", "cross"})
            tap(b);
        ok = picture("32-own-keyboard") && ok;
        tap("options");
        run(2.5f);
        ok = picture("33-own-keyboard-searched") && ok;
        return ok ? 0 : 1;
    }

    bool ok = true;
    int px = 960, py = 540;
    for (const Step &step : kTour)
    {
        const std::string p = step.press;
        hui::fp::hid::Frame f;
        if (p.rfind("tab:", 0) == 0)
            app.open_tab(std::atoi(p.c_str() + 4));
        else if (p.rfind("move:", 0) == 0)
        {
            int x = 0, y = 0;
            std::sscanf(p.c_str() + 5, "%d,%d", &x, &y);
            f.dx = x - px;
            f.dy = y - py;
            px = x;
            py = y;
            hui::fp::hid::hid_host_push(f);
            frame(hui::InputFrame{});
        }
        else if (p == "click")
        {
            f.clicked = hui::fp::hid::kMouseLeft;
            hui::fp::hid::hid_host_push(f);
            frame(hui::InputFrame{});
        }
        else if (p.rfind("wheel:", 0) == 0) // notches, negative = down
        {
            f.wheel = std::atoi(p.c_str() + 6);
            hui::fp::hid::hid_host_push(f);
            frame(hui::InputFrame{});
        }
        else if (p.rfind("type:", 0) == 0)
        {
            for (char c : p.substr(5))
                f.text.push_back(c);
            hui::fp::hid::hid_host_push(f);
            frame(hui::InputFrame{});
        }
        else
        {
            frame(press(p));
        }
        settle(step.settle);
        if (step.picture != nullptr)
            ok = picture(step.picture) && ok;
    }
    return ok ? 0 : 1;
}
