// Flashpoint PS5 - headless PC run of the app: scripted input, PNG pictures.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// usage: fp_snapshots <assets dir> <output dir>
//
// Runs the same App, Backend and Images code as the console build through
// Mesa's surfaceless EGL (llvmpipe). The Backend talks to whatever answers on
// 127.0.0.1:22600, normally the PC build of the service (payload/, make -f
// Makefile.pc), so the pictures show real search results and images. Without
// a service it shows the "start your ELF loader" screen.

#include "core/input.hpp"
#include "core/save_file.hpp"
#include "fp/app.hpp"
#include "fp/backend.hpp"
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

namespace
{

bool open_context()
{
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display =
        get_platform_display != nullptr
            ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
            : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) ||
        !eglBindAPI(EGL_OPENGL_API))
        return false;
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION,
                                 4,
                                 EGL_CONTEXT_MINOR_VERSION,
                                 5,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                 EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                 EGL_NONE};
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    return context != EGL_NO_CONTEXT &&
           eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

bool load_font(hui::gfx::Renderer &renderer, const std::string &path, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
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

struct Step
{
    const char *press;   // button for one frame, or nullptr
    float settle;        // seconds to keep running afterwards (and until idle)
    const char *picture; // PNG name, or nullptr
};

// One tour of every screen.
const Step kTour[] = {
    {nullptr, 2.5f, "01-library"},
    {"right", 0.1f, nullptr},
    {"right", 0.1f, nullptr},
    {"down", 1.0f, "02-library-focus"},
    {"r2", 2.0f, "03-library-page2"},
    {"l2", 1.5f, nullptr},
    {"up", 0.1f, nullptr},
    {"left", 0.1f, nullptr},
    {"left", 0.1f, nullptr},
    {"cross", 2.0f, "04-entry"},
    {"cross", 4.0f, "05-entry-played"},
    {"circle", 0.5f, nullptr},
    {"triangle", 2.5f, "06-search"},
    {"cross", 2.0f, "07-search-entry"},
    {"circle", 0.4f, nullptr},
    {"circle", 1.5f, nullptr},
    {"square", 2.5f, "08-everything"},
    {"square", 1.0f, nullptr},
    {"r1", 2.0f, "09-played"},
    {"r1", 2.0f, "10-installed"},
    {"r1", 1.0f, "11-setup"},
};

hui::InputFrame press(const char *name)
{
    using hui::Action;
    using hui::Direction;
    hui::InputFrame f;
    f.connected = true;
    if (name == nullptr)
        return f;
    const std::string n = name;
    const auto bit = [&](Action a) { f.pressed |= hui::action_bit(a); };
    if (n == "up")
        f.nav = Direction::up, bit(Action::up);
    else if (n == "down")
        f.nav = Direction::down, bit(Action::down);
    else if (n == "left")
        f.nav = Direction::left, bit(Action::left);
    else if (n == "right")
        f.nav = Direction::right, bit(Action::right);
    else if (n == "cross")
        bit(Action::confirm);
    else if (n == "circle")
        bit(Action::back);
    else if (n == "triangle")
        bit(Action::north);
    else if (n == "square")
        bit(Action::west);
    else if (n == "l1")
        bit(Action::page_prev);
    else if (n == "r1")
        bit(Action::page_next);
    else if (n == "l2")
        bit(Action::jump_prev);
    else if (n == "r2")
        bit(Action::jump_next);
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
    const int width = 1920;
    const int height = 1080;
    if (!open_context())
    {
        std::fprintf(stderr, "no EGL context (Mesa surfaceless)\n");
        return 1;
    }
    hui::gfx::set_glsl_prefix("#version 450 core\n");
    hui::gfx::Renderer renderer;
    hui::gfx::Font regular;
    hui::gfx::Font bold;
    hui::gfx::Font pixel;
    hui::fp::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, assets + "/fonts/atkinson-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, assets + "/fonts/atkinson-bold.huifont", &bold, &fonts.bold) ||
        !load_font(renderer, assets + "/fonts/silkscreen.huifont", &pixel, &fonts.pixel))
        return 1;

    GLuint framebuffer = 0;
    GLuint color = 0;
    glGenFramebuffers(1, &framebuffer);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 1;
    hui::save::ensure_directory(output);

    hui::fp::services::init();
    hui::fp::Backend backend(assets + "/payload/flashpoint-ps5.elf");
    hui::fp::Images images(backend);
    hui::fp::App app(fonts, backend, images);
    backend.connect();
    if (std::getenv("FP_KEYBOARD_TEXT") == nullptr)
        setenv("FP_KEYBOARD_TEXT", "orbit", 1);

    hui::gfx::DrawList list;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    stbi_flip_vertically_on_write(1);
    constexpr float kDt = 1.0f / 60.0f;
    const auto frame = [&](const hui::InputFrame &input) {
        app.update(input, kDt);
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
        std::fprintf(stderr, "wrote %s ok=%d shapes=%zu draws=%zu\n", path.c_str(), ok ? 1 : 0,
                     renderer.last_instances(), renderer.last_draw_calls());
        return ok;
    };

    // Wait for the service (or for the app to give up on it).
    for (int i = 0; i < 60 * 30; ++i)
    {
        frame(press(nullptr));
        const hui::fp::Link link = backend.link();
        if (link != hui::fp::Link::connecting && link != hui::fp::Link::starting)
            break;
    }
    if (backend.link() != hui::fp::Link::ready)
    {
        for (int i = 0; i < 30; ++i)
            frame(press(nullptr));
        return picture("00-no-service") ? 0 : 1;
    }

    bool ok = true;
    for (const Step &step : kTour)
    {
        frame(press(step.press));
        const int settle = static_cast<int>(step.settle * 60.0f);
        for (int i = 0; i < settle || (!app.idle() && i < 60 * 20); ++i)
        {
            frame(press(nullptr));
            if (i % 20 == 0)
            {
                // images upload on the main thread inside update(); drawing
                // also asks for them, so draw now and then without saving
                list.clear();
                app.draw(list);
            }
        }
        if (step.picture != nullptr)
            ok = picture(step.picture) && ok;
    }
    return ok ? 0 : 1;
}
