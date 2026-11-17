// Flashpoint PS5 - the app's screens: library grid, entry page, setup.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Same look as the web UI: Flash authoring-tool timeline colours, pages of
// results as timeline frames with the red playhead, motion-tween lavender for
// focus and shape-tween green for "downloaded".

#pragma once

#include "core/input.hpp"
#include "fp/backend.hpp"
#include "fp/images.hpp"
#include "gfx/draw_list.hpp"
#include "ui/fonts.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace hui::fp
{

struct Fonts
{
    ui::FontRef regular; // Atkinson Hyperlegible
    ui::FontRef bold;    // Atkinson Hyperlegible Bold
    ui::FontRef pixel;   // Silkscreen: timeline frame numbers
};

class App
{
  public:
    App(const Fonts &fonts, Backend &backend, Images &images);

    void update(const InputFrame &input, float dt);
    void draw(gfx::DrawList &list) const;

    // For the PC snapshot tool.
    bool idle() const;

  private:
    enum Tab : int
    {
        kLibrary = 0,
        kPlayed,
        kInstalled,
        kSetup,
        kTabCount,
    };

    struct ListState
    {
        SearchQuery query;
        int page = 1;
        long long total = 0;
        std::vector<Game> games;
        int focus = 0;
        bool loading = false;
        bool loaded = false;
        bool failed = false;
        std::uint64_t token = 0;
        std::set<int> seen;
    };

    struct Entry
    {
        bool open = false;
        bool loading = false;
        GameDetail detail;
        int focus = 0;  // 0: Play, 1..n: additional apps
        bool playing = false;
        std::uint64_t token = 0;
        std::string status;
        bool status_error = false;
        double progress = -1.0; // 0..1 while downloading/unpacking
        float poll_timer = 0.0f;
        std::string play_add_app;
    };

    // input
    void update_list(const InputFrame &input, float dt);
    void update_entry(const InputFrame &input);
    void update_setup(const InputFrame &input);
    void switch_tab(int delta);
    void load(int tab);
    void flip(int delta);
    void open_entry(const Game &game);
    void start_play(const std::string &add_app);
    void handle_play(const PlayReply &reply, std::uint64_t token);
    void toast(const std::string &message);

    // drawing
    void draw_bar(gfx::DrawList &list) const;
    void draw_link_state(gfx::DrawList &list) const;
    void draw_list(gfx::DrawList &list) const;
    void draw_timeline(gfx::DrawList &list, const ListState &state, int pages) const;
    void draw_tile(gfx::DrawList &list, const Game &game, const gfx::Rect &r, bool focused) const;
    void draw_entry(gfx::DrawList &list) const;
    void draw_setup(gfx::DrawList &list) const;
    void draw_toast(gfx::DrawList &list) const;
    void draw_glyph(gfx::DrawList &list, const char *which, float x, float cy) const;
    float draw_hint(gfx::DrawList &list, const char *glyph, const std::string &label, float x,
                    float baseline) const;
    void draw_image(gfx::DrawList &list, const std::string &rel, const gfx::Rect &r) const;
    void draw_message(gfx::DrawList &list, const std::string &title, const std::string &body,
                      float top) const;

    int pages(const ListState &state) const;

    const Fonts &fonts_;
    Backend &backend_;
    Images &images_;

    float clock_ = 0.0f;
    int tab_ = kLibrary;
    ListState lists_[3];
    Entry entry_;
    int setup_focus_ = 0;
    bool keyboard_ = false;
    int pending_page_ = 0;
    float flip_timer_ = 0.0f;
    std::string toast_;
    float toast_timer_ = 0.0f;
    float jobs_timer_ = 0.0f;
    std::vector<Job> jobs_;
    Link last_link_ = Link::connecting;
    bool stop_armed_ = false;
};

} // namespace hui::fp
