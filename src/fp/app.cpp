// Flashpoint PS5 - the app's screens: library grid, entry page, setup.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fp/app.hpp"

#include "fp/services.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hui::fp
{

namespace
{

using gfx::Align;
using gfx::Color;
using gfx::Rect;

// The palette of the web UI (www/app.css).
const Color kStage = Color::rgb(0x2b2f3a);
const Color kFrames = Color::rgb(0x353a47);
const Color kCell = Color::rgb(0x404657);
const Color kGrid = Color::rgb(0x4b5263);
const Color kPaper = Color::rgb(0xf3f4f7);
const Color kMuted = Color::rgb(0xb3b8c6);
const Color kTween = Color::rgb(0xb8b2f2);
const Color kShape = Color::rgb(0xa6e3a0);
const Color kPlayhead = Color::rgb(0xdd0428);
const Color kWarn = Color::rgb(0xffd27a);

constexpr float kMargin = 72.0f;
constexpr float kBarHeight = 96.0f;
constexpr int kCols = 7;
constexpr int kRows = 3;
constexpr int kPerPage = kCols * kRows;
constexpr float kTileW = 230.0f;
constexpr float kTileH = 232.0f;
constexpr float kThumbH = 150.0f;
constexpr float kGapX = 24.0f;
constexpr float kGapY = 20.0f;
constexpr float kGridTop = 294.0f;
constexpr int kTimelineCells = 50;

const char *const kTabNames[] = {"Library", "Played", "On this PS5", "Setup"};

std::string logo_rel(const std::string &id)
{
    if (id.size() < 4)
        return {};
    return "Logos/" + id.substr(0, 2) + "/" + id.substr(2, 2) + "/" + id + ".png";
}

std::string shot_rel(const std::string &id)
{
    if (id.size() < 4)
        return {};
    return "Screenshots/" + id.substr(0, 2) + "/" + id.substr(2, 2) + "/" + id + ".png";
}

std::string with_commas(long long n)
{
    std::string digits = std::to_string(n < 0 ? -n : n);
    std::string out;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it)
    {
        if (count && count % 3 == 0)
            out.insert(out.begin(), ',');
        out.insert(out.begin(), *it);
        ++count;
    }
    return n < 0 ? "-" + out : out;
}

std::string megabytes(long long bytes)
{
    char buf[32];
    if (bytes >= 1048576)
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / 1048576.0);
    else
        std::snprintf(buf, sizeof(buf), "%lld KB", std::max(1LL, bytes / 1024));
    return buf;
}

std::string kind_label(const std::string &platform, const std::string &library)
{
    std::string p = platform;
    for (std::size_t at = 0; (at = p.find("; ", at)) != std::string::npos;)
        p.replace(at, 2, ", ");
    return p + (library == "theatre" ? " animation" : " game");
}

// The Flashpoint bolt (www/img/logo.svg), as two polygons in a 0..1 box.
void draw_bolt(gfx::DrawList &list, float x, float y, float size, Color color)
{
    static const float a[] = {29.651f, 141.56f, 72.462f, 128.367f, 15.202f, 95.126f,
                              59.733f, 128.018f, 32.663f, 135.942f};
    static const float b[] = {11.995f, 97.353f, 32.174f, 122.455f, 17.61f, 151.955f,
                              27.186f, 149.004f, 38.636f, 122.679f, 39.134f, 129.587f,
                              50.994f, 126.523f};
    const float ox = 11.995f, oy = 95.126f, span = 60.5f;
    const auto place = [&](const float *src, int n) {
        float out[16];
        for (int i = 0; i < n; ++i)
        {
            out[i * 2] = x + (src[i * 2] - ox) / span * size;
            out[i * 2 + 1] = y + (src[i * 2 + 1] - oy) / span * size;
        }
        list.polygon(out, n, color);
    };
    place(a, 5);
    place(b, 7);
}

} // namespace

App::App(const Fonts &fonts, Backend &backend, Images &images)
    : fonts_(fonts), backend_(backend), images_(images)
{
    lists_[kPlayed].query.played = true;
    lists_[kPlayed].query.ps5_only = false;
    lists_[kInstalled].query.installed = true;
    lists_[kInstalled].query.ps5_only = false;
    for (ListState &state : lists_)
        state.query.limit = kPerPage;
}

bool App::idle() const
{
    if (backend_.link() == Link::connecting || backend_.link() == Link::starting)
        return false;
    if (tab_ < kSetup && lists_[tab_].loading)
        return false;
    return !entry_.loading;
}

int App::pages(const ListState &state) const
{
    return std::max(1, static_cast<int>((state.total + kPerPage - 1) / kPerPage));
}

void App::toast(const std::string &message)
{
    toast_ = message;
    toast_timer_ = 6.0f;
}

void App::load(int tab)
{
    if (tab >= kSetup || backend_.link() != Link::ready)
        return;
    ListState &state = lists_[tab];
    state.query.offset = (state.page - 1) * kPerPage;
    state.loading = true;
    state.failed = false;
    const std::uint64_t token = ++state.token;
    images_.cancel_pending();
    backend_.search(state.query, [this, tab, token](SearchResult result) {
        ListState &s = lists_[tab];
        if (token != s.token)
            return;
        s.loading = false;
        s.loaded = true;
        s.failed = !result.ok;
        if (!result.ok)
            return;
        s.total = result.total;
        s.games = std::move(result.games);
        s.seen.insert(s.page);
        s.focus = std::min(s.focus, std::max(0, static_cast<int>(s.games.size()) - 1));
    });
}

void App::switch_tab(int delta)
{
    tab_ = (tab_ + delta + kTabCount) % kTabCount;
    stop_armed_ = false;
    pending_page_ = 0;
    if (tab_ < kSetup)
    {
        // played/downloaded lists change while you play: refresh them
        if (!lists_[tab_].loaded || tab_ != kLibrary)
            load(tab_);
    }
}

void App::flip(int delta)
{
    ListState &state = lists_[tab_];
    const int base = pending_page_ ? pending_page_ : state.page;
    const int next = std::clamp(base + delta, 1, pages(state));
    if (next == state.page && pending_page_ == 0)
        return;
    pending_page_ = next;
    flip_timer_ = 0.28f; // holding the trigger scrubs; load when it rests
}

void App::update(const InputFrame &input, float dt)
{
    clock_ += dt;
    backend_.pump();
    images_.pump();
    if (toast_timer_ > 0.0f)
        toast_timer_ -= dt;

    const Link link = backend_.link();
    if (link != last_link_)
    {
        last_link_ = link;
        if (link == Link::ready)
            load(kLibrary);
    }
    if (link != Link::ready)
    {
        if ((link == Link::no_loader || link == Link::failed || link == Link::no_payload) &&
            input.is_pressed(Action::confirm))
            backend_.connect();
        return;
    }

    jobs_timer_ -= dt;
    if (jobs_timer_ <= 0.0f)
    {
        jobs_timer_ = 2.0f;
        backend_.jobs([this](std::vector<Job> jobs) { jobs_ = std::move(jobs); });
    }

    if (keyboard_)
    {
        std::string text;
        switch (services::keyboard_poll(&text))
        {
        case services::KeyboardState::done:
            keyboard_ = false;
            lists_[kLibrary].query.text = text;
            lists_[kLibrary].page = 1;
            lists_[kLibrary].focus = 0;
            lists_[kLibrary].seen.clear();
            tab_ = kLibrary;
            load(kLibrary);
            break;
        case services::KeyboardState::open:
            break;
        default:
            keyboard_ = false;
        }
        return;
    }

    if (entry_.open)
    {
        update_entry(input);
        if (entry_.open && entry_.playing)
        {
            entry_.poll_timer -= dt;
            if (entry_.poll_timer <= 0.0f && entry_.poll_timer > -1000.0f)
            {
                entry_.poll_timer = -10000.0f; // waiting for the answer
                const std::uint64_t token = entry_.token;
                backend_.play(entry_.detail.id, entry_.play_add_app,
                              [this, token](PlayReply reply) { handle_play(reply, token); });
            }
        }
        return;
    }

    if (input.is_pressed(Action::page_prev))
        switch_tab(-1);
    else if (input.is_pressed(Action::page_next))
        switch_tab(1);

    if (tab_ == kSetup)
        update_setup(input);
    else
        update_list(input, dt);
}

void App::update_list(const InputFrame &input, float dt)
{
    ListState &state = lists_[tab_];
    if (pending_page_)
    {
        flip_timer_ -= dt;
        if (flip_timer_ <= 0.0f)
        {
            state.page = pending_page_;
            pending_page_ = 0;
            load(tab_);
        }
    }
    if (input.is_pressed(Action::jump_prev))
        flip(-1);
    if (input.is_pressed(Action::jump_next))
        flip(1);

    if (tab_ == kLibrary)
    {
        if (input.is_pressed(Action::north))
        {
            keyboard_ = services::keyboard_open("Search Flashpoint", state.query.text);
            if (!keyboard_)
                toast("The keyboard didn't open.");
            return;
        }
        if (input.is_pressed(Action::west))
        {
            state.query.ps5_only = !state.query.ps5_only;
            state.page = 1;
            state.seen.clear();
            load(tab_);
            return;
        }
        if (input.is_pressed(Action::menu))
        {
            std::string &lib = state.query.library;
            lib = lib.empty() ? "arcade" : lib == "arcade" ? "theatre" : "";
            state.page = 1;
            state.seen.clear();
            load(tab_);
            return;
        }
        if (input.is_pressed(Action::back) && !state.query.text.empty())
        {
            state.query.text.clear();
            state.page = 1;
            state.seen.clear();
            load(tab_);
            return;
        }
    }

    const int n = static_cast<int>(state.games.size());
    if (n == 0 || state.loading)
        return;
    int f = state.focus;
    const int col = f % kCols;
    switch (input.nav)
    {
    case Direction::left:
        if (col > 0)
            --f;
        break;
    case Direction::right:
        if (col < kCols - 1 && f + 1 < n)
            ++f;
        break;
    case Direction::up:
        if (f >= kCols)
            f -= kCols;
        else if (state.page > 1 && !input.nav_repeat)
        {
            state.focus = std::min(n - 1, (kRows - 1) * kCols + col);
            flip(-1);
            return;
        }
        break;
    case Direction::down:
        if (f + kCols < n)
            f += kCols;
        else if (f / kCols < (n - 1) / kCols)
            f = n - 1;
        else if (state.page < pages(state) && !input.nav_repeat)
        {
            state.focus = col;
            flip(1);
            return;
        }
        break;
    default:
        break;
    }
    state.focus = f;
    if (input.is_pressed(Action::confirm) && f < n)
        open_entry(state.games[static_cast<std::size_t>(f)]);
}

void App::open_entry(const Game &game)
{
    const std::uint64_t serial = entry_.token; // stale answers must never match
    entry_ = Entry{};
    entry_.token = serial;
    entry_.open = true;
    entry_.loading = true;
    entry_.detail.id = game.id;
    entry_.detail.title = game.title;
    entry_.detail.developer = game.developer;
    entry_.detail.platform = game.platform;
    entry_.detail.library = game.library;
    const std::uint64_t token = ++entry_.token;
    backend_.game(game.id, [this, token](GameDetail detail) {
        if (!entry_.open || token != entry_.token)
            return;
        entry_.loading = false;
        if (!detail.ok)
        {
            entry_.status = "This entry didn't load.";
            entry_.status_error = true;
            return;
        }
        entry_.detail = std::move(detail);
        if (!entry_.detail.playable)
            entry_.status = entry_.detail.reason;
        else if (entry_.detail.kind == "flash" && entry_.detail.ruffle_mode == "export")
            entry_.status = "Copies the game into Ruffle Flash's library and opens the app.";
        else
            entry_.status = entry_.detail.reason;
    });
}

void App::update_entry(const InputFrame &input)
{
    if (input.is_pressed(Action::back))
    {
        entry_.open = false;
        ++entry_.token;
        return;
    }
    if (entry_.loading)
        return;
    const int count = 1 + static_cast<int>(entry_.detail.add_apps.size());
    if (input.nav == Direction::up)
        entry_.focus = std::max(0, entry_.focus - 1);
    else if (input.nav == Direction::down)
        entry_.focus = std::min(count - 1, entry_.focus + 1);
    if (input.is_pressed(Action::confirm) && !entry_.playing)
    {
        if (entry_.focus == 0 && !entry_.detail.playable)
            return;
        start_play(entry_.focus == 0
                       ? std::string()
                       : entry_.detail.add_apps[static_cast<std::size_t>(entry_.focus - 1)].id);
    }
}

void App::start_play(const std::string &add_app)
{
    entry_.playing = true;
    entry_.play_add_app = add_app;
    entry_.status = "Getting the game ready...";
    entry_.status_error = false;
    entry_.progress = -1.0;
    entry_.poll_timer = 0.0f;
    ++entry_.token;
}

void App::handle_play(const PlayReply &reply, std::uint64_t token)
{
    if (!entry_.open || token != entry_.token)
        return;
    if (reply.status == "pending")
    {
        const Job *job = nullptr;
        for (const Job &j : reply.jobs)
        {
            if (j.game_id == entry_.detail.id)
                job = &j;
        }
        entry_.progress = job && job->total > 0 ? static_cast<double>(job->done) / job->total : 0.0;
        const std::string state = job ? job->state : "queued";
        if (state == "downloading" && job && job->total > 0)
            entry_.status = "Downloading " + megabytes(job->done) + " of " + megabytes(job->total);
        else if (state == "extracting")
            entry_.status = "Unpacking";
        else if (state == "verifying")
            entry_.status = "Checking the download";
        else
            entry_.status = "Waiting to download";
        entry_.poll_timer = 0.8f;
        return;
    }
    entry_.playing = false;
    entry_.progress = -1.0;
    entry_.status_error = false;
    if (reply.status == "open")
    {
        entry_.status = "Opening the game in the PS5 browser...";
        if (!services::open_browser(reply.url))
        {
            entry_.status = "The browser didn't open.";
            entry_.status_error = true;
        }
        return;
    }
    entry_.status = reply.message;
    if (reply.status == "launched")
        toast(reply.message);
    else if (reply.status != "message")
        entry_.status_error = true;
}

void App::update_setup(const InputFrame &input)
{
    if (input.nav == Direction::left)
        setup_focus_ = 0;
    else if (input.nav == Direction::right)
        setup_focus_ = 1;
    if (!input.is_pressed(Action::confirm))
        return;
    if (setup_focus_ == 0)
    {
        backend_.refresh_info([this] { toast("Reconnected to the Flashpoint service."); });
        stop_armed_ = false;
        return;
    }
    if (!stop_armed_)
    {
        stop_armed_ = true;
        return;
    }
    stop_armed_ = false;
    backend_.stop_service([this](bool ok) {
        toast(ok ? "The Flashpoint service stopped. It starts again with the app."
                 : "The service didn't answer.");
        if (ok)
            backend_.connect();
    });
}

// ------------------------------------------------------------------ drawing

void App::draw(gfx::DrawList &list) const
{
    list.rounded_rect({0, 0, 1920, 1080}, 0, kStage);
    draw_bar(list);
    if (backend_.link() != Link::ready)
        draw_link_state(list);
    else if (entry_.open)
        draw_entry(list);
    else if (tab_ == kSetup)
        draw_setup(list);
    else
        draw_list(list);
    draw_toast(list);
}

void App::draw_glyph(gfx::DrawList &list, const char *which, float x, float cy) const
{
    const std::string w = which;
    const float s = 30.0f;
    if (w == "cross" || w == "circle" || w == "triangle" || w == "square")
    {
        list.circle(x + s / 2, cy, s / 2, kCell);
        const float c = x + s / 2;
        if (w == "cross")
        {
            list.line(c - 6, cy - 6, c + 6, cy + 6, 2.5f, kPaper);
            list.line(c - 6, cy + 6, c + 6, cy - 6, 2.5f, kPaper);
        }
        else if (w == "circle")
        {
            list.ring(c, cy, 7.0f, 2.5f, kPaper);
        }
        else if (w == "triangle")
        {
            list.line(c, cy - 7, c - 7, cy + 5, 2.5f, kPaper);
            list.line(c - 7, cy + 5, c + 7, cy + 5, 2.5f, kPaper);
            list.line(c + 7, cy + 5, c, cy - 7, 2.5f, kPaper);
        }
        else
        {
            list.bordered_rect({c - 6, cy - 6, 12, 12}, 1, kCell, 2.5f, kPaper);
        }
        return;
    }
    const float tw = fonts_.bold.measure(which, 16);
    list.bordered_rect({x, cy - 14, tw + 16, 28}, 6, kCell, 1.0f, kGrid);
    ui::text(list, fonts_.bold, which, x + 8, cy + 6, 16, kPaper);
}

float App::draw_hint(gfx::DrawList &list, const char *glyph, const std::string &label, float x,
                     float baseline) const
{
    const std::string g = glyph;
    const bool shape = g == "cross" || g == "circle" || g == "triangle" || g == "square";
    const float gw = shape ? 30.0f : fonts_.bold.measure(glyph, 16) + 16;
    draw_glyph(list, glyph, x, baseline - 8);
    x += gw + 10;
    ui::text(list, fonts_.regular, label, x, baseline, 22, kMuted);
    return x + fonts_.regular.measure(label, 22) + 30;
}

void App::draw_bar(gfx::DrawList &list) const
{
    list.rounded_rect({0, 0, 1920, kBarHeight}, 0, kFrames);
    list.rounded_rect({0, kBarHeight - 1, 1920, 1}, 0, kGrid);
    draw_bolt(list, kMargin, 26, 46, kPlayhead);
    float x = kMargin + 60;
    x += ui::text(list, fonts_.bold, "Flashpoint", x, 62, 34, kPaper);
    ui::text(list, fonts_.regular, " PS5", x, 62, 34, kMuted);

    float tx = 470;
    draw_glyph(list, "L1", tx, 48);
    tx += 56;
    for (int i = 0; i < kTabCount; ++i)
    {
        const float w = fonts_.bold.measure(kTabNames[i], 26) + 40;
        const bool on = i == tab_;
        if (on)
        {
            list.rounded_rect({tx, 20, w, 56}, 8, kCell);
            list.rounded_rect({tx, 72, w, 4}, 2, kPlayhead);
        }
        ui::text(list, fonts_.bold, kTabNames[i], tx + 20, 58, 26, on ? kPaper : kMuted);
        tx += w + 8;
    }
    draw_glyph(list, "R1", tx + 8, 48);

    // download badge
    for (const Job &job : jobs_)
    {
        if (job.state == "done" || job.state == "error")
            continue;
        char pct[16] = "";
        if (job.total > 0)
            std::snprintf(pct, sizeof(pct), " %d%%", static_cast<int>(job.done * 100 / job.total));
        const std::string line =
            (job.state == "extracting" ? std::string("Unpacking") : std::string("Downloading")) + pct;
        ui::text(list, fonts_.bold, fonts_.bold.font->fit(job.title, 22, 420), 1920 - kMargin, 44, 22,
                 kPaper, Align::right);
        ui::text(list, fonts_.regular, line, 1920 - kMargin, 72, 20, kMuted, Align::right);
        break;
    }
}

void App::draw_message(gfx::DrawList &list, const std::string &title, const std::string &body,
                       float top) const
{
    const Rect box{kMargin, top, 1100, 0};
    ui::text(list, fonts_.bold, title, box.x, top + 50, 44, kPaper);
    ui::paragraph(list, fonts_.regular, body, box.x, top + 110, 28, box.w, 42, kMuted);
}

void App::draw_link_state(gfx::DrawList &list) const
{
    const float top = 260;
    switch (backend_.link())
    {
    case Link::connecting:
    case Link::starting:
    {
        list.arc(kMargin + 30, top + 30, 26, 6, clock_ * 5.0f, 4.2f, kTween);
        ui::text(list, fonts_.bold, "Starting the Flashpoint service", kMargin + 90, top + 44, 40, kPaper);
        ui::paragraph(list, fonts_.regular,
                      "It runs in the background, reads your Flashpoint folder and downloads games "
                      "when you play them.",
                      kMargin + 90, top + 100, 28, 1100, 42, kMuted);
        break;
    }
    case Link::no_loader:
        draw_message(list, "Start your ELF loader first",
                     "Flashpoint PS5's background service is a payload. Nothing is listening on port "
                     "9021: start your jailbreak's ELF loader (etaHEN, or kstuff with elfldr), then "
                     "press Cross to try again.",
                     top);
        draw_hint(list, "cross", "Try again", kMargin, top + 250);
        break;
    case Link::no_payload:
        draw_message(list, "The app is incomplete",
                     "assets/payload/flashpoint-ps5.elf is missing from the app folder. Copy the whole "
                     "PPSA72386 folder from the release zip again.",
                     top);
        draw_hint(list, "cross", "Try again", kMargin, top + 250);
        break;
    case Link::failed:
        draw_message(list, "The service didn't answer",
                     "It was sent to the ELF loader but never came up. Its log is "
                     "/data/fpps5/fpps5.log. Press Cross to try again.",
                     top);
        draw_hint(list, "cross", "Try again", kMargin, top + 250);
        break;
    case Link::ready:
        break;
    }
}

void App::draw_image(gfx::DrawList &list, const std::string &rel, const Rect &r) const
{
    list.rounded_rect(r, 6, kCell);
    const Texture tex = rel.empty() ? Texture{} : images_.get(rel);
    if (tex.id == 0)
    {
        draw_bolt(list, r.cx() - r.h * 0.18f, r.cy() - r.h * 0.18f, r.h * 0.36f, kGrid);
        return;
    }
    const float scale = std::min(r.w / static_cast<float>(tex.width), r.h / static_cast<float>(tex.height));
    const float w = tex.width * scale, h = tex.height * scale;
    list.image(tex.id, {r.cx() - w / 2, r.cy() - h / 2, w, h}, gfx::kFullUv, Color::rgb(0xffffff), 4);
}

void App::draw_tile(gfx::DrawList &list, const Game &game, const Rect &r, bool focused) const
{
    if (focused)
    {
        list.push_transform(1.06f, r.cx(), r.cy(), 0, 0);
        list.shadow(r, 10, 26, Color::rgb(0x000000, 0.45f));
    }
    list.bordered_rect(r, 8, kFrames, focused ? 4.0f : 1.0f, focused ? kTween : kGrid);
    draw_image(list, logo_rel(game.id), {r.x + 6, r.y + 6, r.w - 12, kThumbH - 6});
    if (game.installed)
    {
        list.circle(r.x + 22, r.y + 22, 10, kFrames);
        list.circle(r.x + 22, r.y + 22, 7, kShape);
    }
    const float tx = r.x + 12, tw = r.w - 24;
    ui::text(list, fonts_.bold, fonts_.bold.font->fit(game.title, 21, tw), tx, r.y + kThumbH + 29, 21,
             kPaper);
    ui::text(list, fonts_.regular, fonts_.regular.font->fit(game.developer, 17, tw), tx,
             r.y + kThumbH + 51, 17, kMuted);
    ui::text(list, fonts_.regular, fonts_.regular.font->fit(kind_label(game.platform, game.library), 16, tw),
             tx, r.y + kThumbH + 71, 16, kMuted);
    if (focused)
        list.pop_transform();
}

void App::draw_timeline(gfx::DrawList &list, const ListState &state, int page_count) const
{
    const int page = pending_page_ ? pending_page_ : state.page;
    const Rect box{kMargin, 178, 1920 - 2 * kMargin, 62};
    list.bordered_rect(box, 6, kFrames, 1, kGrid);
    // Always a 50-frame strip, like Flash's: pages past the last are the
    // empty frames after the final keyframe.
    const int cells = kTimelineCells;
    int start = std::max(1, page - cells / 2);
    start = std::max(1, std::min(start, page_count - cells + 1));
    const float cw = box.w / cells;
    const float ruler = 24;
    list.rounded_rect({box.x, box.y + ruler, box.w, 1}, 0, kGrid);
    list.push_clip(box);
    for (int i = 0; i < cells; ++i)
    {
        const int n = start + i;
        const float x = box.x + i * cw;
        if (n > page_count)
            list.rounded_rect({x, box.y + ruler + 1, cw, box.h - ruler - 1}, 0, kStage);
        else if (n % 5 == 0)
            list.rounded_rect({x, box.y + ruler, cw, box.h - ruler}, 0, Color::rgb(0xffffff, 0.04f));
        if (i > 0)
            list.rounded_rect({x, box.y + ruler, 1, box.h - ruler}, 0, kGrid);
        if ((n == 1 || n % 5 == 0) && n != page)
        {
            list.rounded_rect({x, box.y, 1, ruler}, 0, kGrid);
            ui::text(list, fonts_.pixel, std::to_string(n), x + 4, box.y + 17, 13, kMuted);
        }
        if (n == page_count && page_count > 0)
        {
            // the last page: a keyframe dot closing the span
            list.ring(x + cw / 2, box.y + box.h - 12, 5, 1.5f, kPaper);
        }
        else if (state.seen.count(n))
        {
            list.circle(x + cw / 2, box.y + box.h - 12, 4, kPaper);
        }
    }
    // the playhead
    const float px = box.x + (page - start + 0.5f) * cw;
    list.rounded_rect({px - 1.5f, box.y, 3, box.h}, 0, kPlayhead);
    const std::string label = std::to_string(page);
    const float lw = fonts_.pixel.measure(label, 13) + 12;
    list.rounded_rect({px - lw / 2, box.y, lw, ruler}, 3, kPlayhead);
    ui::text(list, fonts_.pixel, label, px, box.y + 17, 13, kPaper, Align::center);
    list.pop_clip();
}

void App::draw_list(gfx::DrawList &list) const
{
    const ListState &state = lists_[tab_];
    std::string heading = kTabNames[tab_];
    if (tab_ == kLibrary && !state.query.text.empty())
        heading = "\"" + state.query.text + "\"";
    float x = kMargin;
    x += ui::text(list, fonts_.bold, fonts_.bold.font->fit(heading, 44, 620), x, 152, 44, kPaper) + 22;
    if (state.loaded && !state.failed)
        ui::text(list, fonts_.regular, state.total == 1 ? std::string("1 entry") : with_commas(state.total) + " entries",
                 x, 152, 26, kMuted);

    // controller hints, right to left
    float hx = 1920 - kMargin;
    std::vector<std::pair<const char *, std::string>> hints;
    hints.emplace_back("L2", "");
    hints.emplace_back("R2", "Page");
    if (tab_ == kLibrary)
    {
        hints.emplace_back("triangle", "Search");
        hints.emplace_back("square", state.query.ps5_only ? "PS5 only: on" : "PS5 only: off");
        const std::string &lib = state.query.library;
        hints.emplace_back("OPTIONS", lib == "arcade" ? "Games" : lib == "theatre" ? "Animations" : "All");
        if (!state.query.text.empty())
            hints.emplace_back("circle", "Clear search");
    }
    float total_w = 0;
    for (auto &[g, label] : hints)
    {
        const std::string gs = g;
        const bool shape = gs == "cross" || gs == "circle" || gs == "triangle" || gs == "square";
        total_w += (shape ? 30.0f : fonts_.bold.measure(g, 16) + 16) + 10 +
                   (label.empty() ? -2.0f : fonts_.regular.measure(label, 22) + 30);
    }
    hx -= total_w;
    for (auto &[g, label] : hints)
    {
        if (label.empty())
        {
            draw_glyph(list, g, hx, 144);
            hx += fonts_.bold.measure(g, 16) + 16 + 8;
        }
        else
        {
            hx = draw_hint(list, g, label, hx, 152);
        }
    }

    const int page_count = pages(state);
    draw_timeline(list, state, page_count);
    char info[64];
    std::snprintf(info, sizeof(info), "Page %s of %s",
                  with_commas(pending_page_ ? pending_page_ : state.page).c_str(),
                  with_commas(page_count).c_str());
    ui::text(list, fonts_.regular, info, kMargin, 272, 20, kMuted);
    const float legend = ui::text(list, fonts_.regular, "Downloaded to this PS5", 1920 - kMargin, 272, 20,
                                  kMuted, Align::right);
    list.circle(1920 - kMargin - legend - 16, 265, 7, kShape);

    if (!backend_.info().db_open && tab_ == kLibrary)
    {
        draw_message(list, "This PS5 has no Flashpoint library yet",
                     "Copy a Flashpoint folder with Data/flashpoint.sqlite (from Flashpoint Infinity "
                     "on a PC) to /data/flashpoint or a USB drive's flashpoint folder, then open "
                     "Setup and choose Reconnect.",
                     330);
        return;
    }
    if (state.loading && state.games.empty())
    {
        list.arc(kMargin + 26, kGridTop + 60, 22, 5, clock_ * 5.0f, 4.2f, kTween);
        ui::text(list, fonts_.regular, "Loading...", kMargin + 70, kGridTop + 70, 28, kMuted);
        return;
    }
    if (state.failed)
    {
        draw_message(list, "The search didn't finish", "The Flashpoint service didn't answer in time. Flip the page or search again.", 330);
        return;
    }
    if (state.loaded && state.games.empty())
    {
        if (tab_ == kPlayed)
            draw_message(list, "Nothing played yet", "Games you start here show up in this list, newest first.", 330);
        else if (tab_ == kInstalled)
            draw_message(list, "No games downloaded yet",
                         "Playing a game downloads its data to the PS5. After that it starts without "
                         "waiting and appears here.",
                         330);
        else
            draw_message(list, "No matches",
                         state.query.ps5_only
                             ? "Nothing matched among entries that play on the PS5. Press Square to show everything."
                             : "Try fewer words.",
                         330);
        return;
    }

    const float grid_w = kCols * kTileW + (kCols - 1) * kGapX;
    const float x0 = (1920 - grid_w) / 2;
    if (state.loading)
        list.push_opacity(0.45f);
    int focused = -1;
    for (int i = 0; i < static_cast<int>(state.games.size()); ++i)
    {
        if (i == state.focus)
        {
            focused = i;
            continue; // drawn last, on top
        }
        const Rect r{x0 + (i % kCols) * (kTileW + kGapX), kGridTop + (i / kCols) * (kTileH + kGapY), kTileW, kTileH};
        draw_tile(list, state.games[static_cast<std::size_t>(i)], r, false);
    }
    if (focused >= 0)
    {
        const Rect r{x0 + (focused % kCols) * (kTileW + kGapX), kGridTop + (focused / kCols) * (kTileH + kGapY),
                     kTileW, kTileH};
        draw_tile(list, state.games[static_cast<std::size_t>(focused)], r, !state.loading);
    }
    if (state.loading)
        list.pop_opacity();
}

void App::draw_entry(gfx::DrawList &list) const
{
    const GameDetail &d = entry_.detail;
    draw_hint(list, "circle", "Back", kMargin, 150);

    draw_image(list, logo_rel(d.id), {kMargin, 180, 330, 330});

    const float tx = kMargin + 370, tw = 900;
    const ui::ParagraphLines title = ui::paragraph_lines(fonts_.bold, d.title, 54, tw, 2);
    float y = ui::paragraph(list, fonts_.bold, title, tx, 236, 54, 62, kPaper);
    const int description_lines = title.lines.size() > 1 ? 3 : 5;
    std::string by = d.developer;
    if (!d.publisher.empty() && d.publisher != d.developer)
        by += by.empty() ? d.publisher : ", " + d.publisher;
    if (!by.empty())
    {
        ui::text(list, fonts_.regular, fonts_.regular.font->fit("by " + by, 28, tw), tx, y - 8, 28, kMuted);
        y += 34;
    }
    // chips
    float cx = tx;
    const auto chip = [&](const std::string &text, Color bg, Color fg) {
        const float w = fonts_.regular.measure(text, 21) + 28;
        list.rounded_rect({cx, y, w, 36}, 18, bg);
        ui::text(list, fonts_.regular, text, cx + 14, y + 25, 21, fg);
        cx += w + 10;
    };
    if (!d.platform.empty())
        chip(kind_label(d.platform, d.library), kCell, kPaper);
    if (d.kind == "flash")
        chip("Plays in Ruffle Flash", kShape, Color::rgb(0x17311a));
    else if (d.kind == "html5")
        chip("Opens in the PS5 browser", kWarn, Color::rgb(0x3a2a00));
    if (d.installed)
        chip("Downloaded", kShape, Color::rgb(0x17311a));
    if (d.size > 0)
        chip("GameZIP, " + megabytes(d.size), kCell, kPaper);
    y += 66;
    if (!d.description.empty())
        ui::paragraph(list, fonts_.regular, d.description, tx, y + 12, 26, tw, 37, kPaper,
                      description_lines);

    // actions
    const float ax = 1400, aw = 1920 - kMargin - ax;
    float ay = 180;
    const bool can_play = d.playable && !entry_.loading;
    const Rect play{ax, ay, aw, 88};
    if (entry_.focus == 0)
        list.shadow(play, 10, 20, Color::rgb(0x000000, 0.4f));
    list.bordered_rect(play, 10, can_play ? kPlayhead : kCell, entry_.focus == 0 ? 4.0f : 0.0f, kTween);
    draw_glyph(list, "cross", play.x + 30, play.cy());
    ui::text(list, fonts_.bold, entry_.playing ? "Getting ready..." : "Play", play.x + 76, play.cy() + 12, 34,
             can_play ? Color::rgb(0xffffff) : kMuted);
    ay += 104;
    for (std::size_t i = 0; i < d.add_apps.size() && i < 4; ++i)
    {
        const Rect r{ax, ay, aw, 60};
        const bool f = entry_.focus == static_cast<int>(i) + 1;
        list.bordered_rect(r, 8, kCell, f ? 4.0f : 1.0f, f ? kTween : kGrid);
        ui::text(list, fonts_.regular, fonts_.regular.font->fit(d.add_apps[i].name, 24, aw - 40), r.x + 20,
                 r.cy() + 9, 24, kPaper);
        ay += 72;
    }
    if (entry_.progress >= 0.0)
    {
        const Rect bar{ax, ay + 6, aw, 16};
        list.bordered_rect(bar, 4, kFrames, 1, kGrid);
        list.rounded_rect({bar.x + 1, bar.y + 1, (bar.w - 2) * static_cast<float>(std::clamp(entry_.progress, 0.0, 1.0)), bar.h - 2},
                          3, kTween);
        ay += 36;
    }
    if (entry_.loading)
    {
        list.arc(ax + 20, ay + 24, 16, 4, clock_ * 5.0f, 4.2f, kTween);
        ay += 48;
    }
    else if (!entry_.status.empty())
    {
        ay = ui::paragraph(list, fonts_.regular, entry_.status, ax, ay + 30, 24, aw, 34,
                           entry_.status_error ? kWarn : kMuted, 4);
    }

    // screenshot and details
    draw_image(list, shot_rel(d.id), {kMargin, 590, 1240, 450});
    float dy = std::max(700.0f, ay + 40);
    const auto row = [&](const char *label, const std::string &value) {
        if (value.empty() || dy > 1040)
            return;
        ui::text(list, fonts_.regular, label, ax, dy, 21, kMuted);
        ui::text(list, fonts_.regular, fonts_.regular.font->fit(value, 21, aw - 150), ax + 150, dy, 21, kPaper);
        dy += 34;
    };
    row("Tags", d.tags);
    row("Series", d.series);
    row("Released", d.release_date);
    row("Format", d.zipped ? "GameZIP" : (d.kind.empty() ? "" : "Legacy files"));
    row("ID", d.id);
}

void App::draw_setup(gfx::DrawList &list) const
{
    const ServiceInfo &info = backend_.info();
    ui::text(list, fonts_.bold, "Setup", kMargin, 160, 44, kPaper);
    float y = 230;
    const auto row = [&](const char *label, const std::string &value) {
        ui::text(list, fonts_.regular, label, kMargin, y, 26, kMuted);
        ui::text(list, fonts_.regular, value, kMargin + 340, y, 26, kPaper);
        y += 46;
    };
    row("Flashpoint folder", info.flashpoint_path);
    row("Library", info.db_open ? with_commas(info.game_count) + " entries in flashpoint.sqlite"
                                : std::string("No flashpoint.sqlite found"));
    row("Unpacked games", info.run_path);
    row("Flash player", info.ruffle_direct
                            ? "Ruffle Flash (" + info.ruffle_title + "), starts games directly"
                            : "Ruffle Flash (" + info.ruffle_title + "), games are copied into its library");
    row("On a phone or PC", "http://" + info.lan_ip + ":" + std::to_string(info.ui_port));
    row("Service version", info.version);

    y += 20;
    ui::text(list, fonts_.bold, "Getting games onto the PS5", kMargin, y, 32, kPaper);
    y += 54;
    const char *steps[] = {
        "1.  On a PC, get Flashpoint Infinity from flashpointarchive.org and run it once.",
        ("2.  Copy its Data/flashpoint.sqlite to /data/flashpoint/Data/ on the PS5, or to a "
         "flashpoint/Data/ folder on a USB drive."),
        "3.  Install Ruffle Flash for the PS5. Flash games open in it.",
        "4.  Choose Play: game data downloads the first time, then plays straight away.",
    };
    for (const char *step : steps)
        y = ui::paragraph(list, fonts_.regular, step, kMargin, y, 26, 1500, 38, kMuted) + 8;

    y += 20;
    const char *labels[] = {"Reconnect", stop_armed_ ? "Press again to stop the service" : "Stop the service"};
    float bx = kMargin;
    for (int i = 0; i < 2; ++i)
    {
        const float w = fonts_.bold.measure(labels[i], 26) + 60;
        const Rect r{bx, y, w, 64};
        const bool f = setup_focus_ == i;
        list.bordered_rect(r, 8, kCell, f ? 4.0f : 1.0f, f ? kTween : kGrid);
        ui::text(list, fonts_.bold, labels[i], r.cx(), r.cy() + 9, 26, kPaper, Align::center);
        bx += w + 20;
    }
}

void App::draw_toast(gfx::DrawList &list) const
{
    if (toast_timer_ <= 0.0f || toast_.empty())
        return;
    const float alpha = std::min(1.0f, toast_timer_ * 2.0f);
    const float w = std::min(1200.0f, fonts_.regular.measure(toast_, 26) + 60);
    const Rect r{960 - w / 2, 960, w, 70};
    list.push_opacity(alpha);
    list.shadow(r, 10, 30, Color::rgb(0x000000, 0.4f));
    list.rounded_rect(r, 10, kPaper);
    ui::text(list, fonts_.regular, fonts_.regular.font->fit(toast_, 26, w - 40), r.cx(), r.cy() + 9, 26, kStage,
             Align::center);
    list.pop_opacity();
}

} // namespace hui::fp
