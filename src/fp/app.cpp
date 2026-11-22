// Flashpoint PS5 - the app's screens, laid out like the Flashpoint Launcher.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fp/app.hpp"

#include "fp/audio.hpp"
#include "fp/http.hpp"
#include "fp/services.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <ctime>

namespace hui::fp
{

namespace
{

using gfx::Align;
using gfx::Color;
using gfx::Rect;

// Colors of the open theme (apply_theme sets them). The first theme is the
// Flashpoint Launcher's dark one.
Color kBg, kPanel, kBar, kRow, kRowHover, kTileBox, kLine, kText, kMuted, kDim;
Color kAccent;      // selection, as the launcher's
Color kAccentHi;    // focus ring
Color kAccentHover; // a button under the pointer
Color kRed, kGreen, kWarn, kInput;
Color kMark; // the play triangle of the logo
const Color kWhite = Color::rgb(0xffffff); // on the accent, and picture tints
float g_corners = 1.0f;     // corner rounding, times the usual
bool g_link_titles = false; // tile titles as red underlined links (Y8)

Color faded(Color c, float alpha)
{
    c.a *= alpha;
    return c;
}

struct ThemeDef
{
    const char *name;
    std::uint32_t bg, panel, bar, row, row_hover, tile, line, text, muted, dim;
    std::uint32_t accent, accent_hi, accent_hover, red, green, warn, input, mark;
    float corners;
    bool link_titles;
};

// clang-format off
const ThemeDef kThemes[] = {
    {"Flashpoint", 0x1c1c1e, 0x252527, 0x2c2c2f, 0x303034, 0x3a3a3f, 0x2a2a2d, 0x3c3c41, 0xe8e8ea, 0xa2a2a8, 0x6e6e75,
     0x7d2593, 0xb45fd0, 0x9131a9, 0xdd0428, 0x62c46e, 0xffc35a, 0x18181a, 0xf3f4f7, 1.0f, false},
    // gray page, white boxes, red underlined links: the 2000s Flash portal look
    {"Y8", 0xc9c9c9, 0xffffff, 0xe4e4e4, 0xf1f1f1, 0xdedede, 0xffffff, 0x9a9a9a, 0x111111, 0x3a3a3a, 0x7a7a7a,
     0xc0261c, 0xd8382c, 0xa81e15, 0xc0261c, 0x2b8a3e, 0xa66a00, 0xffffff, 0x1a1a1a, 0.25f, true},
    {"Daylight", 0xeef0f4, 0xffffff, 0xffffff, 0xf1f3f7, 0xe2e6ee, 0xe9ecf1, 0xd3d8e0, 0x15181e, 0x525966, 0x8b919c,
     0x0f7a70, 0x16a596, 0x0c6159, 0xd0263a, 0x2b8a3e, 0xb07800, 0xffffff, 0x15181e, 1.0f, false},
    {"Midnight", 0x0c1322, 0x121b2e, 0x162036, 0x1b263e, 0x23314e, 0x172036, 0x293752, 0xe5ebf6, 0x98a6c0, 0x5f6e8a,
     0x2b63c6, 0x6b9dff, 0x3a74da, 0xe0334d, 0x5cc88a, 0xffc35a, 0x0a0f1b, 0xffffff, 1.0f, false},
    {"Ember", 0x191613, 0x221d19, 0x29231e, 0x302924, 0x3a322b, 0x25201b, 0x40372f, 0xf3ece3, 0xb4a795, 0x7a6e60,
     0xc85a17, 0xff9547, 0xdc6a24, 0xe03a2c, 0x7cc46a, 0xffc35a, 0x14110f, 0xffffff, 1.0f, false},
    {"Terminal", 0x050805, 0x0a100a, 0x0d150d, 0x101b10, 0x16261a, 0x0c140c, 0x1d3320, 0x9cffb0, 0x5fbf72, 0x3a7a48,
     0x1f7a34, 0x3dff74, 0x2a9a44, 0xff5a4a, 0x3dff74, 0xe8ff5a, 0x030503, 0x9cffb0, 0.0f, false},
};
// clang-format on
constexpr int kThemeCount = static_cast<int>(sizeof(kThemes) / sizeof(kThemes[0]));

void apply_theme(int index)
{
    const ThemeDef &t = kThemes[std::clamp(index, 0, kThemeCount - 1)];
    kBg = Color::rgb(t.bg);
    kPanel = Color::rgb(t.panel);
    kBar = Color::rgb(t.bar);
    kRow = Color::rgb(t.row);
    kRowHover = Color::rgb(t.row_hover);
    kTileBox = Color::rgb(t.tile);
    kLine = Color::rgb(t.line);
    kText = Color::rgb(t.text);
    kMuted = Color::rgb(t.muted);
    kDim = Color::rgb(t.dim);
    kAccent = Color::rgb(t.accent);
    kAccentHi = Color::rgb(t.accent_hi);
    kAccentHover = Color::rgb(t.accent_hover);
    kRed = Color::rgb(t.red);
    kGreen = Color::rgb(t.green);
    kWarn = Color::rgb(t.warn);
    kInput = Color::rgb(t.input);
    kMark = Color::rgb(t.mark);
    g_corners = t.corners;
    g_link_titles = t.link_titles;
}

constexpr float kW = 1920.0f, kH = 1080.0f;
constexpr float kMenuH = 64.0f, kStatusH = 44.0f;
constexpr float kSideW = 360.0f, kDetailW = 480.0f;
constexpr int kPageSize = 60;
constexpr float kGap = 14.0f;

const char *const kTabNames[] = {"Home", "Arcade", "Theatre", "Web", "Logs", "Config", "About"};

struct SortOption
{
    const char *id, *name;
};
const SortOption kSorts[] = {{"title", "Title"},
                             {"dateAdded", "Date added"},
                             {"developer", "Developer"},
                             {"releaseDate", "Release date"},
                             {"random", "Random"}};
constexpr int kSortCount = 5;
constexpr int kPopupRows = kSortCount + 2; // + ascending, descending

constexpr int kConfigRows = 12;
const char *const kConfigLabels[kConfigRows] = {
    "Theme",
    "Play the intro when the app starts",
    "Music volume",
    "Interface sounds",
    "Open the libraries on \"Plays on PS5\"",
    "Show extreme entries",
    "Show entries websites mark as adult",
    "Tile size",
    "USB keyboard and mouse (from the next start)",
    "Flashpoint library",
    "Background service",
    "Stop the background service",
};

enum Icon
{
    kIconAll,
    kIconCheck,
    kIconClock,
    kIconDownload,
    kIconLink,
    kIconGlobe,
    kIconSearch,
};

std::string logo_rel(const std::string &id)
{
    return id.size() < 4 ? std::string() : "Logos/" + id.substr(0, 2) + "/" + id.substr(2, 2) + "/" + id + ".png";
}

std::string shot_rel(const std::string &id)
{
    return id.size() < 4 ? std::string()
                         : "Screenshots/" + id.substr(0, 2) + "/" + id.substr(2, 2) + "/" + id + ".png";
}

std::string web_thumb(const std::string &url)
{
    return url.empty() ? std::string() : "/api/web/thumb?u=" + url_encode(url);
}

std::string commas(long long n)
{
    const std::string digits = std::to_string(n < 0 ? -n : n);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i)
    {
        if (i && (digits.size() - i) % 3 == 0)
            out += ',';
        out += digits[i];
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

std::string first_platform(const std::string &platforms)
{
    const std::size_t semi = platforms.find(';');
    return semi == std::string::npos ? platforms : platforms.substr(0, semi);
}

std::string date_only(const std::string &iso)
{
    return iso.size() >= 10 ? iso.substr(0, 10) : iso;
}

float ease_out(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

float ease_back(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    const float c = 1.7f;
    return 1.0f + (c + 1.0f) * std::pow(t - 1.0f, 3.0f) + c * std::pow(t - 1.0f, 2.0f);
}

bool inside(const Rect &r, float x, float y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

bool is_shape(const std::string &g)
{
    return g == "cross" || g == "circle" || g == "triangle" || g == "square";
}

// The app's mark: a play triangle with a bolt through it, in a size x size box
// (the same shape as sce_sys/icon0.png, tools/make-icon.py).
void draw_mark(gfx::DrawList &list, float x, float y, float size, Color fill = Color::rgb(0xf3f4f7))
{
    const float tri[3][2] = {{0.25f, 0.14f}, {0.86f, 0.50f}, {0.25f, 0.86f}};
    float pts[3 * 9 * 2];
    int n = 0;
    const float r = 0.07f;
    for (int i = 0; i < 3; ++i)
    {
        const float *p0 = tri[(i + 2) % 3], *p1 = tri[i], *p2 = tri[(i + 1) % 3];
        const auto unit = [](const float *a, const float *b, float out[2]) {
            const float dx = b[0] - a[0], dy = b[1] - a[1], l = std::sqrt(dx * dx + dy * dy);
            out[0] = dx / l;
            out[1] = dy / l;
        };
        float u1[2], u2[2];
        unit(p1, p0, u1);
        unit(p1, p2, u2);
        const float a[2] = {p1[0] + u1[0] * r, p1[1] + u1[1] * r};
        const float b[2] = {p1[0] + u2[0] * r, p1[1] + u2[1] * r};
        for (int k = 0; k <= 8; ++k)
        {
            const float t = k / 8.0f;
            pts[n * 2] = x + size * ((1 - t) * (1 - t) * a[0] + 2 * (1 - t) * t * p1[0] + t * t * b[0]);
            pts[n * 2 + 1] = y + size * ((1 - t) * (1 - t) * a[1] + 2 * (1 - t) * t * p1[1] + t * t * b[1]);
            ++n;
        }
    }
    list.polygon(pts, n, fill);
    const float bolt[] = {0.555f, 0.20f, 0.355f, 0.545f, 0.505f, 0.545f, 0.43f, 0.80f,
                          0.665f, 0.44f, 0.515f, 0.44f, 0.60f, 0.20f};
    float b[14];
    for (int i = 0; i < 7; ++i)
    {
        b[i * 2] = x + bolt[i * 2] * size;
        b[i * 2 + 1] = y + bolt[i * 2 + 1] * size;
    }
    list.polygon(b, 7, Color::rgb(0xdd0428));
}

} // namespace

// ================================================================= setup

App::App(const Fonts &fonts, Backend &backend, Images &images, std::string settings_path)
    : fonts_(fonts), backend_(backend), images_(images), settings_path_(std::move(settings_path))
{
    settings_ = Settings::load(settings_path_);
    apply_theme(settings_.theme);
    browse_[0].library = "arcade";
    browse_[1].library = "theatre";
    browse_[0].filter = browse_[1].filter = settings_.ps5_only ? 1 : 0;
    audio::set_volume(settings_.music, settings_.sounds);
    intro_ = settings_.intro;
    region_ = kPage;
}

void App::save_settings()
{
    settings_.save(settings_path_);
    audio::set_volume(settings_.music, settings_.sounds);
}

bool App::begin_usb()
{
    if (settings_.usb_trying)
    {
        // the last start died while they were starting
        settings_.usb_trying = false;
        settings_.usb_input = false;
        save_settings();
        toast("USB keyboard and mouse are off: starting them failed last time. Config turns them on.", true);
        return false;
    }
    if (!settings_.usb_input)
        return false;
    settings_.usb_trying = true;
    save_settings();
    return true;
}

void App::end_usb(bool any_device)
{
    (void)any_device;
    settings_.usb_trying = false;
    save_settings();
}

bool App::idle() const
{
    if (intro_)
        return false;
    const Link link = backend_.link();
    if (link == Link::connecting || link == Link::starting)
        return false;
    if (const Browse *b = browse())
    {
        if (!b->loading.empty())
            return false;
    }
    if (tab_ == kWeb && (web_.loading || web_.sources.empty()))
        return false;
    return !detail_.loading;
}

void App::sound(int sfx) const
{
    audio::play(static_cast<audio::Sfx>(sfx));
}

void App::toast(const std::string &message, bool error)
{
    toast_ = message;
    toast_error_ = error;
    toast_timer_ = 5.0f;
    if (error)
        sound(static_cast<int>(audio::Sfx::error));
}

// ================================================================ geometry

Rect App::menu_item_rect(int tab) const
{
    float x = 130.0f;
    for (int i = 0; i < tab; ++i)
        x += fonts_.bold.measure(kTabNames[i], 22) + 36.0f;
    return {x, 0.0f, fonts_.bold.measure(kTabNames[tab], 22) + 36.0f, kMenuH};
}

Rect App::search_rect() const { return {990.0f, 12.0f, 430.0f, 40.0f}; }
Rect App::sort_rect() const { return {1432.0f, 12.0f, 190.0f, 40.0f}; }
Rect App::dir_rect() const { return {1630.0f, 12.0f, 172.0f, 40.0f}; }

Rect App::sidebar_area() const { return {0.0f, kMenuH, kSideW, kH - kMenuH - kStatusH}; }

Rect App::sidebar_row_rect(int row) const
{
    return {0.0f, kMenuH + 10.0f + row * 46.0f - side_scroll_, kSideW, 44.0f};
}

float App::side_max_scroll() const
{
    const float content = 20.0f + static_cast<float>(sidebar_rows().size()) * 46.0f;
    return std::max(0.0f, content - sidebar_area().h);
}

void App::side_follow(int row)
{
    const float top = 10.0f + row * 46.0f, h = sidebar_area().h;
    if (top - 10.0f < side_scroll_to_)
        side_scroll_to_ = top - 10.0f;
    else if (top + 44.0f + 10.0f > side_scroll_to_ + h)
        side_scroll_to_ = top + 44.0f + 10.0f - h;
    side_scroll_to_ = std::clamp(side_scroll_to_, 0.0f, side_max_scroll());
}

Rect App::grid_area() const { return {kSideW, kMenuH, kW - kSideW - kDetailW, kH - kMenuH - kStatusH}; }

Rect App::detail_area() const { return {kW - kDetailW, kMenuH, kDetailW, kH - kMenuH - kStatusH}; }

float App::tile_w() const
{
    static const float sizes[] = {128.0f, 158.0f, 204.0f};
    return sizes[std::clamp(settings_.tile_size, 0, 2)];
}

float App::tile_h() const { return tile_w() * 0.78f + 56.0f; }

int App::grid_cols() const
{
    return std::max(1, static_cast<int>((grid_area().w - 2 * 20.0f + kGap) / (tile_w() + kGap)));
}

Rect App::tile_rect(int index) const
{
    const Rect a = grid_area();
    const int cols = grid_cols();
    const float used = cols * tile_w() + (cols - 1) * kGap;
    const float x0 = a.x + (a.w - used) / 2.0f;
    const float scroll = tab_ == kWeb ? web_.scroll : (browse() ? browse()->scroll : 0.0f);
    return {x0 + (index % cols) * (tile_w() + kGap), a.y + 18.0f + (index / cols) * (tile_h() + kGap) - scroll,
            tile_w(), tile_h()};
}

float App::grid_max_scroll() const
{
    const int count = grid_count();
    const int rows = (count + grid_cols() - 1) / grid_cols();
    return std::max(0.0f, 18.0f + rows * (tile_h() + kGap) + 20.0f - grid_area().h);
}

float *App::grid_scroll()
{
    if (tab_ == kWeb)
        return &web_.scroll_to;
    Browse *b = browse();
    return b ? &b->scroll_to : nullptr;
}

void App::keep_focus_visible()
{
    float *target = grid_scroll();
    if (!target)
        return;
    const int row = grid_focus() / grid_cols();
    const float top = 18.0f + row * (tile_h() + kGap);
    const float view = grid_area().h;
    if (top - 12.0f < *target)
        *target = std::max(0.0f, top - 12.0f);
    else if (top + tile_h() + 24.0f > *target + view)
        *target = top + tile_h() + 24.0f - view;
}

Rect App::play_button_rect() const
{
    const Rect a = detail_area();
    return {a.x + 24.0f, a.y + a.h - 400.0f, a.w - 48.0f, 64.0f};
}

Rect App::add_app_rect(int i) const
{
    const Rect p = play_button_rect();
    return {p.x, p.y + 76.0f + i * 50.0f, p.w, 44.0f};
}

Rect App::popup_rect() const
{
    const Rect s = sort_rect();
    return {s.x, s.y + s.h + 6.0f, 260.0f, kPopupRows * 44.0f + 24.0f};
}

Rect App::popup_row_rect(int i) const
{
    const Rect p = popup_rect();
    return {p.x + 6.0f, p.y + 6.0f + i * 44.0f + (i >= kSortCount ? 12.0f : 0.0f), p.w - 12.0f, 42.0f};
}

Rect App::config_row_rect(int row) const { return {200.0f, kMenuH + 110.0f + row * 70.0f, 1520.0f, 60.0f}; }

int App::home_item_count() const
{
    return 5 + static_cast<int>(std::min<std::size_t>(recent_.size(), 8));
}

Rect App::home_item_rect(int item) const
{
    if (item == 0) // the library card's button
        return {150.0f, 686.0f, 360.0f, 60.0f};
    if (item <= 4)
    {
        const int i = item - 1;
        return {120.0f + (i % 2) * 420.0f, 800.0f + (i / 2) * 86.0f, 400.0f, 72.0f};
    }
    const int i = item - 5;
    return {1080.0f + (i % 4) * 182.0f, 236.0f + (i / 4) * 236.0f, 168.0f, 214.0f};
}

// ============================================================= data views

App::Browse *App::browse()
{
    return tab_ == kArcade ? &browse_[0] : tab_ == kTheatre ? &browse_[1] : nullptr;
}

const App::Browse *App::browse() const
{
    return tab_ == kArcade ? &browse_[0] : tab_ == kTheatre ? &browse_[1] : nullptr;
}

const std::vector<std::pair<std::string, long long>> &App::platforms_for(const Browse &b) const
{
    const auto it = stats_.library_platforms.find(b.library);
    return it != stats_.library_platforms.end() ? it->second : stats_.platforms;
}

std::vector<App::SideRow> App::sidebar_rows() const
{
    std::vector<SideRow> rows;
    if (const Browse *b = browse())
    {
        const bool arcade = b->library == "arcade";
        rows.push_back({arcade ? "All games" : "All animations", "", 0, stats_.ok ? (arcade ? stats_.arcade : stats_.theatre) : -1});
        rows.push_back({"Plays on PS5", "", 1});
        rows.push_back({"Recently played", "", 2});
        rows.push_back({"Downloaded", "", 3});
        rows.push_back({"Platforms", "", 5});
        int shown = 0;
        bool chosen = b->platform.empty();
        for (const auto &[name, count] : platforms_for(*b))
        {
            if (shown++ >= 12)
                break;
            rows.push_back({name, name, 4, count});
            chosen |= name == b->platform;
        }
        if (!chosen) // keep the chosen one listed even after it drops out of the top
            rows.push_back({b->platform, b->platform, 4, -1});
    }
    else if (tab_ == kWeb)
    {
        rows.push_back({"Play a .swf link", "", 7});
        for (const WebSource &s : web_.sources)
        {
            rows.push_back({s.name, "", 5});
            for (const WebList &l : s.lists)
            {
                SideRow r{l.name, "", 6};
                r.source = s.id;
                r.list = l.id;
                rows.push_back(r);
            }
        }
    }
    return rows;
}

int App::sidebar_focus() const
{
    if (const Browse *b = browse())
        return b->filter;
    return tab_ == kWeb ? web_.filter : 0;
}

void App::set_sidebar_focus(int row)
{
    if (Browse *b = browse())
    {
        const auto rows = sidebar_rows();
        const bool platform = row >= 0 && row < static_cast<int>(rows.size()) &&
                              rows[static_cast<std::size_t>(row)].kind == 4;
        b->platform = platform ? rows[static_cast<std::size_t>(row)].platform : std::string();
        b->filter = row;
    }
    else if (tab_ == kWeb)
        web_.filter = row;
}

SearchQuery App::query_for(const Browse &b) const
{
    SearchQuery q;
    q.library = b.library;
    q.text = search_;
    q.sort = settings_.sort;
    q.desc = settings_.desc;
    q.show_extreme = settings_.show_extreme;
    q.ps5_only = false;
    q.limit = kPageSize;
    // rows as sidebar_rows() lays them out for a library
    if (b.filter == 1)
        q.ps5_only = true;
    else if (b.filter == 2)
        q.played = true;
    else if (b.filter == 3)
        q.installed = true;
    else if (b.filter >= 5)
        q.platform = b.platform;
    return q;
}

void App::reload_browse(Browse &b)
{
    b.pages.clear();
    b.loading.clear();
    b.total = -1;
    b.failed = false;
    ++b.token;
    b.focus = 0;
    b.scroll = b.scroll_to = 0.0f;
    images_.cancel_pending();
    ensure_pages(b);
}

void App::refresh_browse(Browse &b)
{
    b.pages.clear();
    b.loading.clear();
    b.failed = false;
    ++b.token;
    if (&b == browse())
        ensure_pages(b);
}

void App::reload_all()
{
    for (Browse &b : browse_)
    {
        b.pages.clear();
        b.loading.clear();
        b.total = -1;
        ++b.token;
    }
    if (Browse *b = browse())
        reload_browse(*b);
}

void App::ensure_pages(Browse &b)
{
    if (backend_.link() != Link::ready)
        return;
    const int cols = grid_cols();
    const float row_h = tile_h() + kGap;
    const int first_row = std::max(0, static_cast<int>((b.scroll - 200.0f) / row_h));
    const int last_row = static_cast<int>((b.scroll + grid_area().h + 400.0f) / row_h);
    const int first = first_row * cols;
    long long last = (last_row + 1) * cols;
    if (b.total >= 0)
        last = std::min(last, b.total);
    for (int page = first / kPageSize; page * kPageSize < std::max<long long>(last, 1); ++page)
    {
        if (b.pages.count(page) || b.loading.count(page) || b.loading.size() >= 2)
            continue;
        b.loading.insert(page);
        SearchQuery q = query_for(b);
        q.offset = page * kPageSize;
        const std::uint64_t token = b.token;
        Browse *target = &b;
        backend_.search(q, [this, target, page, token](SearchResult result) {
            if (token != target->token)
                return;
            target->loading.erase(page);
            if (!result.ok)
            {
                target->failed = true;
                return;
            }
            target->failed = false;
            target->total = result.total;
            if (target->focus >= target->total) // entries went away in an update
                target->focus = static_cast<int>(std::max<long long>(0, target->total - 1));
            target->pages[page] = std::move(result.games);
            // the detail panel follows the focus once its page is here (after a
            // jump the focused entry may not have been loaded yet)
            if (browse() == target && page == target->focus / kPageSize)
                select_for_detail();
        });
    }
}

void App::load_web(bool next_page)
{
    const std::vector<SideRow> rows = sidebar_rows();
    if (web_.filter <= 0 || web_.filter >= static_cast<int>(rows.size()))
        return;
    const SideRow &row = rows[static_cast<std::size_t>(web_.filter)];
    if (row.kind != 6)
        return;
    if (!next_page)
    {
        web_.items.clear();
        web_.page = 0;
        web_.focus = 0;
        web_.hidden = 0;
        web_.empty_run = 0;
        web_.scroll = web_.scroll_to = 0.0f;
        web_.error.clear();
        ++web_.token;
    }
    web_.source = row.source;
    web_.list = row.list;
    web_.loading = true;
    const int page = web_.page + 1;
    const std::uint64_t token = web_.token;
    // every source can be searched: the service asks the site, or reads
    // its lists and keeps the titles that match
    backend_.web_list(row.source, row.list, search_, page, settings_.web_adult,
                      [this, token, page](WebPage p) {
                          if (token != web_.token)
                              return;
                          web_.loading = false;
                          web_.error = p.error;
                          web_.page = p.next > page ? p.next - 1 : page;
                          web_.more = p.more;
                          web_.hidden += p.hidden;
                          const std::size_t before = web_.items.size();
                          for (WebItem &i : p.items)
                          {
                              const bool seen = std::any_of(web_.items.begin(), web_.items.end(),
                                                            [&](const WebItem &w) { return w.page == i.page; });
                              if (!seen)
                                  web_.items.push_back(std::move(i));
                          }
                          // pages that added nothing (all hidden, or no match) don't end
                          // the list, but a run of them does
                          web_.empty_run = web_.items.size() == before ? web_.empty_run + 1 : 0;
                          if (web_.empty_run >= 4)
                              web_.more = false;
                          if (tab_ == kWeb && before == 0 && !web_.items.empty())
                          {
                              detail_.web = false;
                              select_for_detail();
                          }
                          // fill at least a few rows without waiting for the cursor
                          if (web_.more && web_.error.empty() && web_.page < 48 &&
                              static_cast<int>(web_.items.size()) < grid_cols() * 3)
                              load_web(true);
                      });
}

int App::grid_count() const
{
    if (tab_ == kWeb)
        return static_cast<int>(web_.items.size());
    const Browse *b = browse();
    return b && b->total > 0 ? static_cast<int>(b->total) : 0;
}

int App::grid_focus() const
{
    if (tab_ == kWeb)
        return web_.focus;
    const Browse *b = browse();
    return b ? b->focus : 0;
}

void App::set_grid_focus(int index, bool with_sound)
{
    const int count = grid_count();
    if (count <= 0)
        return;
    index = std::clamp(index, 0, count - 1);
    if (index == grid_focus())
        return;
    if (std::abs(index - grid_focus()) > grid_cols() * 6)
        images_.cancel_pending(); // a long jump: skip the pictures of the rows passed over
    if (tab_ == kWeb)
        web_.focus = index;
    else if (Browse *b = browse())
        b->focus = index;
    if (with_sound)
        sound(static_cast<int>(audio::Sfx::move));
    keep_focus_visible();
    select_for_detail();
    if (tab_ == kWeb && web_.more && !web_.loading && index >= count - grid_cols() * 2)
        load_web(true);
}

App::Tile App::tile_at(int index) const
{
    Tile t;
    if (tab_ == kWeb)
    {
        if (index >= 0 && index < static_cast<int>(web_.items.size()))
        {
            const WebItem &w = web_.items[static_cast<std::size_t>(index)];
            t.title = w.title;
            t.image = web_thumb(w.thumb);
            t.platform = "Flash";
            t.loaded = true;
        }
        return t;
    }
    const Browse *b = browse();
    if (!b)
        return t;
    const auto it = b->pages.find(index / kPageSize);
    if (it == b->pages.end() || index % kPageSize >= static_cast<int>(it->second.size()))
        return t;
    const Game &g = it->second[static_cast<std::size_t>(index % kPageSize)];
    t.title = g.title;
    t.subtitle = g.developer;
    t.image = logo_rel(g.id);
    t.platform = first_platform(g.platform);
    t.installed = g.installed;
    t.loaded = true;
    return t;
}

void App::select_for_detail()
{
    const int index = grid_focus();
    if (tab_ == kWeb)
    {
        if (index < 0 || index >= static_cast<int>(web_.items.size()))
            return;
        const WebItem &w = web_.items[static_cast<std::size_t>(index)];
        if (detail_.web && detail_.item.page == w.page)
            return;
        const std::uint64_t token = detail_.token + 1;
        detail_ = Detail{};
        detail_.token = token;
        detail_.web = true;
        detail_.item = w;
        detail_.web_source = web_.source;
        for (const WebSource &s : web_.sources)
        {
            if (s.id == web_.source)
                detail_.web_source = s.name;
        }
        return;
    }
    const Browse *b = browse();
    if (!b)
        return;
    const auto it = b->pages.find(index / kPageSize);
    if (it == b->pages.end() || index % kPageSize >= static_cast<int>(it->second.size()))
        return;
    const Game &g = it->second[static_cast<std::size_t>(index % kPageSize)];
    if (!detail_.web && detail_.id == g.id)
        return;
    const std::uint64_t token = detail_.token + 1;
    detail_ = Detail{};
    detail_.token = token;
    detail_.id = g.id;
    detail_.d.id = g.id;
    detail_.d.title = g.title;
    detail_.d.developer = g.developer;
    detail_.d.platform = g.platform;
    detail_.d.library = g.library;
    detail_.d.installed = g.installed;
    detail_.loading = true;
    detail_.wait = 0.12f; // fast scrolling doesn't fetch every entry it passes
}

void App::load_detail()
{
    const std::uint64_t token = ++detail_.token;
    backend_.game(detail_.id, [this, token](GameDetail d) {
        if (token != detail_.token)
            return;
        detail_.loading = false;
        if (!d.ok)
        {
            detail_.status = "This entry didn't load.";
            detail_.status_error = true;
            return;
        }
        detail_.d = std::move(d);
        if (!detail_.d.playable)
            detail_.status = detail_.d.reason;
        else if (detail_.d.kind == "flash" && detail_.d.ruffle_mode == "export")
            detail_.status = "Copies the game into Ruffle Flash's library and opens Ruffle.";
        else
            detail_.status.clear();
    });
}

void App::refresh_stats()
{
    backend_.stats([this](Stats s) {
        if (!s.ok)
            return;
        stats_ = std::move(s);
        // a platform filter follows its platform to its new row
        for (Browse &b : browse_)
        {
            if (b.platform.empty())
                continue;
            int row = 5;
            for (const auto &[name, count] : platforms_for(b))
            {
                if (row - 5 >= 12 || name == b.platform)
                    break;
                ++row;
            }
            b.filter = row;
        }
    });
    SearchQuery q;
    q.played = true;
    q.ps5_only = false;
    q.show_extreme = true;
    q.limit = 8;
    backend_.search(q, [this](SearchResult r) {
        if (r.ok)
            recent_ = std::move(r.games);
    });
}

// ================================================================== update

void App::open_tab(int tab)
{
    intro_ = false;
    switch_tab(tab);
}

void App::update(const InputFrame &raw, const PadSample &pad, const hid::Frame &hid, float dt)
{
    clock_ += dt;
    backend_.pump();
    images_.pump();
    if (toast_timer_ > 0.0f)
        toast_timer_ -= dt;

    InputFrame input = raw;
    const float click_before = pointer_.last_click;
    handle_pointer(pad, hid, dt);
    const bool clicked = pointer_.last_click != click_before;

    if (intro_)
    {
        if (intro_t_ == 0.0f)
            audio::play_music();
        intro_t_ += dt;
        const float music = audio::music_position();
        if (music > 0.0f && std::fabs(music - intro_t_) > 0.05f)
            intro_t_ = music;
        const bool skip = input.pressed != 0 || clicked || !hid.pressed.empty();
        if (intro_fade_ < 0.0f && (skip || intro_t_ > 8.3f))
        {
            intro_fade_ = 0.0f;
            if (skip)
                audio::stop_music(0.6f);
        }
        if (intro_fade_ >= 0.0f)
        {
            intro_fade_ += dt;
            if (intro_fade_ > 0.7f)
                intro_ = false;
        }
        return;
    }

    handle_keyboard(hid, input);
    if (osk_.open)
    {
        update_osk(input);
        return;
    }

    const Link link = backend_.link();
    if (link != last_link_)
    {
        last_link_ = link;
        if (link == Link::ready)
        {
            refresh_stats();
            backend_.sync(false, [this](SyncStatus s) {
                sync_ = s;
                const long long age = static_cast<long long>(std::time(nullptr)) - s.synced_at;
                const bool stale = s.source == "fpfss" && (s.synced_at == 0 || age > 3 * 86400);
                if (s.ok && !s.running && (s.source.empty() || stale))
                {
                    sync_started_ = true;
                    backend_.sync(true, [this](SyncStatus started) { sync_ = started; });
                    sync_poll_ = 1.0f;
                }
            });
            if (Browse *b = browse())
                reload_browse(*b);
        }
    }
    if (link != Link::ready)
    {
        if (input.is_pressed(Action::page_prev) || input.is_pressed(Action::page_next))
            switch_tab(input.is_pressed(Action::page_prev) ? (tab_ + kTabCount - 1) % kTabCount
                                                           : (tab_ + 1) % kTabCount);
        if ((link == Link::no_loader || link == Link::failed || link == Link::no_payload) &&
            input.is_pressed(Action::confirm))
        {
            sound(static_cast<int>(audio::Sfx::select));
            backend_.connect();
        }
        return;
    }

    poll_sync(dt);
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
        {
            const int purpose = keyboard_;
            keyboard_ = 0;
            if (purpose == 1)
                apply_search(text);
            else
                start_swf_link(text);
            break;
        }
        case services::KeyboardState::open:
            break;
        default:
            keyboard_ = 0;
        }
        return;
    }

    if (search_wait_ >= 0.0f)
    {
        search_wait_ -= dt;
        if (search_wait_ < 0.0f)
            apply_search(search_);
    }

    // smooth scrolling
    for (Browse &b : browse_)
        b.scroll += (b.scroll_to - b.scroll) * std::min(1.0f, dt * 14.0f);
    web_.scroll += (web_.scroll_to - web_.scroll) * std::min(1.0f, dt * 14.0f);
    side_scroll_ += (side_scroll_to_ - side_scroll_) * std::min(1.0f, dt * 14.0f);
    if (Browse *b = browse())
        ensure_pages(*b);
    if (tab_ == kWeb && !web_.requested)
    {
        web_.requested = true;
        backend_.web_sources([this](std::vector<WebSource> sources) {
            web_.sources = std::move(sources);
            const auto rows = sidebar_rows();
            if (web_.filter >= static_cast<int>(rows.size()) || rows[static_cast<std::size_t>(web_.filter)].kind != 6)
            {
                for (int i = 0; i < static_cast<int>(rows.size()); ++i)
                {
                    if (rows[static_cast<std::size_t>(i)].kind == 6)
                    {
                        web_.filter = i;
                        break;
                    }
                }
            }
            if (tab_ == kWeb && web_.items.empty())
                load_web(false);
        });
    }

    if (!detail_.web && detail_.loading && !detail_.id.empty() && detail_.wait > 0.0f)
    {
        detail_.wait -= dt;
        if (detail_.wait <= 0.0f)
            load_detail();
    }
    if (detail_.playing)
    {
        detail_.poll -= dt;
        if (detail_.poll <= 0.0f && detail_.poll > -1000.0f)
        {
            detail_.poll = -10000.0f;
            const std::uint64_t token = detail_.token;
            if (detail_.web)
                backend_.web_play(detail_.item.swf.empty() ? detail_.item.page : detail_.item.swf, detail_.item.title,
                                  detail_.web_source, [this, token](PlayReply r) { handle_play(r, token); });
            else
                backend_.play(detail_.id, detail_.play_add_app, [this, token](PlayReply r) { handle_play(r, token); });
        }
    }

    update_main(input, dt);
}

void App::poll_sync(float dt)
{
    sync_poll_ -= dt;
    if (sync_poll_ > 0.0f)
        return;
    sync_poll_ = sync_.running ? 1.0f : 15.0f;
    const bool was_running = sync_.running;
    const long long had = sync_.entries;
    backend_.sync(false, [this, was_running, had](SyncStatus s) {
        if (!s.ok)
            return;
        const bool first = had <= 0 && s.entries > 0;
        const bool finished = was_running && !s.running;
        sync_ = s;
        if (first || finished || (s.running && s.entries - stats_entries_ >= 20000))
        {
            stats_entries_ = s.entries;
            backend_.refresh_info([] {});
            refresh_stats();
            for (Browse &b : browse_)
            {
                if (b.total <= 0)
                {
                    if (&b == browse())
                        reload_browse(b);
                    else
                    {
                        b.total = -1; // loads from the top when opened
                        b.pages.clear();
                        b.loading.clear();
                        ++b.token;
                    }
                }
                else if (finished)
                    refresh_browse(b); // new entries in, without losing the place
            }
        }
        if (finished && s.state == "done")
            toast(s.message);
        else if (finished && s.state == "error")
            toast(s.message, true);
    });
}

void App::handle_pointer(const PadSample &pad, const hid::Frame &hid, float dt)
{
    float dx = 0.0f, dy = 0.0f, wheel = 0.0f;
    bool clicked = false;
    if (pad.touch_count >= 2)
    {
        const float ty = (pad.touch_y[0] + pad.touch_y[1]) * 0.5f;
        if (pointer_.two)
            wheel += (pointer_.two_y - ty) * 2.5f;
        pointer_.two = true;
        pointer_.two_y = ty;
        pointer_.touch_id = -1;
    }
    else if (pad.touch_count == 1)
    {
        pointer_.two = false;
        const float x = pad.touch_x[0], y = pad.touch_y[0];
        if (pad.touch_id[0] == pointer_.touch_id)
        {
            dx += (x - pointer_.touch_x) * 1.3f;
            dy += (y - pointer_.touch_y) * 1.7f;
        }
        pointer_.touch_id = pad.touch_id[0];
        pointer_.touch_x = x;
        pointer_.touch_y = y;
    }
    else
    {
        pointer_.touch_id = -1;
        pointer_.two = false;
    }
    const bool touch_down = (pad.buttons & pad_bits::kTouchpad) != 0;
    bool reveal = false;
    if (touch_down && !touch_was_down_)
    {
        // a press while the pointer is hidden only brings it back: the user
        // can't see where it would click
        if (pointer_.visible)
            clicked = true;
        else
            reveal = true;
    }
    touch_was_down_ = touch_down;

    dx += static_cast<float>(hid.dx);
    dy += static_cast<float>(hid.dy);
    wheel -= static_cast<float>(hid.wheel) * 120.0f;
    if (hid.clicked & hid::kMouseLeft)
        clicked = true;

    if (dx != 0.0f || dy != 0.0f || wheel != 0.0f || clicked || reveal)
    {
        pointer_.visible = true;
        pointer_.idle = 0.0f;
    }
    else
    {
        pointer_.idle += dt;
        if (pointer_.idle > 5.0f)
            pointer_.visible = false;
    }
    pointer_.x = std::clamp(pointer_.x + dx, 0.0f, kW - 1.0f);
    pointer_.y = std::clamp(pointer_.y + dy, 0.0f, kH - 1.0f);
    if (intro_)
    {
        if (clicked)
            pointer_.last_click = clock_;
        return;
    }
    if (dx != 0.0f || dy != 0.0f)
        hover(pointer_.x, pointer_.y);
    if (wheel != 0.0f)
        scroll_at(pointer_.x, pointer_.y, wheel);
    if (clicked)
    {
        const bool twice = clock_ - pointer_.last_click < 0.45f;
        pointer_.last_click = clock_;
        click(pointer_.x, pointer_.y, twice);
    }
}

void App::handle_keyboard(const hid::Frame &hid, InputFrame &input)
{
    if (hid.keyboard)
        usb_keyboard_ = true;
    if (osk_.open) // a USB keyboard types into the app's keyboard too
    {
        for (std::uint16_t key : hid.pressed)
        {
            if (key == hid::key::kEnter)
                return close_osk(true);
            if (key == hid::key::kEscape)
                return close_osk(false);
            if (key == hid::key::kBackspace && !osk_.text.empty())
                osk_.text.pop_back();
            else if (key == hid::key::kUp)
                input.nav = Direction::up;
            else if (key == hid::key::kDown)
                input.nav = Direction::down;
            else if (key == hid::key::kLeft)
                input.nav = Direction::left;
            else if (key == hid::key::kRight)
                input.nav = Direction::right;
        }
        for (char c : hid.text)
        {
            if (c >= 32 && c < 127 && osk_.text.size() < 200)
                osk_.text += c;
        }
        return;
    }
    const auto press = [&](Action a) { input.pressed |= action_bit(a); };
    for (std::uint16_t key : hid.pressed)
    {
        switch (key)
        {
        case hid::key::kUp:
            input.nav = Direction::up;
            break;
        case hid::key::kDown:
            input.nav = Direction::down;
            break;
        case hid::key::kLeft:
            if (region_ != kSearch)
                input.nav = Direction::left;
            break;
        case hid::key::kRight:
            if (region_ != kSearch)
                input.nav = Direction::right;
            break;
        case hid::key::kEnter:
            if (region_ == kSearch)
            {
                apply_search(search_);
                move_focus(kGrid);
            }
            else
            {
                press(Action::confirm);
            }
            break;
        case hid::key::kEscape:
            press(Action::back);
            break;
        case hid::key::kBackspace:
            if (region_ == kSearch)
            {
                if (!search_.empty())
                {
                    search_.pop_back();
                    search_wait_ = 0.5f;
                }
            }
            else
            {
                press(Action::back);
            }
            break;
        case hid::key::kTab:
            if (browse() || tab_ == kWeb)
                move_focus(region_ == kSidebar ? kGrid : region_ == kGrid ? kDetail : kSidebar);
            break;
        case hid::key::kPageUp:
            press(Action::jump_prev);
            break;
        case hid::key::kPageDown:
            press(Action::jump_next);
            break;
        default:
            break;
        }
    }
    if (!hid.text.empty() && keyboard_ == 0 && (browse() || tab_ == kWeb || tab_ == kHome))
    {
        std::string typed;
        for (char c : hid.text)
        {
            if (c >= 32 && c < 127)
                typed += c;
        }
        if (!typed.empty() && (region_ == kSearch || typed != " "))
        {
            if (!browse() && tab_ != kWeb)
                switch_tab(kArcade);
            if (region_ != kSearch)
                move_focus(kSearch);
            search_ += typed;
            search_wait_ = 0.5f;
        }
    }
}

void App::hover(float x, float y)
{
    pointer_.hover_region = -1;
    pointer_.hover_index = -1;
    if (y < kMenuH || y > kH - kStatusH || popup_ || osk_.open)
        return;
    if ((browse() || tab_ == kWeb) && inside(grid_area(), x, y))
    {
        const int count = grid_count();
        const int cols = grid_cols();
        const float scroll = tab_ == kWeb ? web_.scroll : browse()->scroll;
        const int first = std::max(0, static_cast<int>(scroll / (tile_h() + kGap))) * cols;
        for (int i = first; i < std::min(count, first + cols * 12); ++i)
        {
            if (inside(tile_rect(i).inset(-6), x, y))
            {
                pointer_.hover_region = kGrid;
                pointer_.hover_index = i;
                return;
            }
        }
    }
    if ((browse() || tab_ == kWeb) && inside(sidebar_area(), x, y))
    {
        const auto rows = sidebar_rows();
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        {
            if (rows[static_cast<std::size_t>(i)].kind != 5 && inside(sidebar_row_rect(i), x, y))
            {
                pointer_.hover_region = kSidebar;
                pointer_.hover_index = i;
            }
        }
    }
}

void App::scroll_at(float x, float y, float pixels)
{
    if (osk_.open)
        return;
    if (tab_ == kLogs)
    {
        log_scroll_ = std::max(0.0f, log_scroll_ + pixels);
        return;
    }
    if ((browse() || tab_ == kWeb) && inside(sidebar_area(), x, y))
    {
        side_scroll_to_ = std::clamp(side_scroll_to_ + pixels, 0.0f, side_max_scroll());
        return;
    }
    if (!(browse() || tab_ == kWeb) || !inside(grid_area(), x, y))
        return;
    float *target = grid_scroll();
    if (!target)
        return;
    *target = std::clamp(*target + pixels, 0.0f, grid_max_scroll());
    if (tab_ == kWeb && web_.more && !web_.loading && *target > grid_max_scroll() - 300.0f)
        load_web(true);
}

void App::click(float x, float y, bool twice)
{
    if (osk_.open)
    {
        osk_click(x, y);
        return;
    }
    if (popup_)
    {
        for (int i = 0; i < kPopupRows; ++i)
        {
            if (inside(popup_row_rect(i), x, y))
            {
                set_sort(i);
                popup_ = false;
                return;
            }
        }
        popup_ = false;
        sound(static_cast<int>(audio::Sfx::back));
        return;
    }
    if (y < kMenuH)
    {
        for (int t = 0; t < kTabCount; ++t)
        {
            if (inside(menu_item_rect(t), x, y))
            {
                switch_tab(t);
                return;
            }
        }
        if (inside(search_rect(), x, y))
        {
            if (!browse() && tab_ != kWeb)
                switch_tab(kArcade);
            if (usb_keyboard_)
                move_focus(kSearch);
            else
                open_keyboard(1);
            return;
        }
        if (browse() && (inside(sort_rect(), x, y) || inside(dir_rect(), x, y)))
        {
            popup_ = true;
            popup_focus_ = 0;
            sound(static_cast<int>(audio::Sfx::open));
        }
        return;
    }
    if (backend_.link() != Link::ready)
    {
        backend_.connect();
        return;
    }
    if (browse() || tab_ == kWeb)
    {
        if (inside(sidebar_area(), x, y))
        {
            const auto rows = sidebar_rows();
            for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            {
                if (rows[static_cast<std::size_t>(i)].kind != 5 && inside(sidebar_row_rect(i), x, y))
                {
                    region_ = kSidebar;
                    activate_sidebar(i);
                    return;
                }
            }
            return;
        }
        if (inside(grid_area(), x, y))
        {
            hover(x, y);
            if (pointer_.hover_region == kGrid)
            {
                const int index = pointer_.hover_index;
                const bool same = index == grid_focus() && region_ == kGrid;
                region_ = kGrid;
                set_grid_focus(index, false);
                select_for_detail();
                sound(static_cast<int>(audio::Sfx::select));
                if (same && twice)
                    play();
            }
            return;
        }
        if (inside(detail_area(), x, y))
        {
            region_ = kDetail;
            if (inside(play_button_rect(), x, y))
            {
                detail_.focus = 0;
                play();
                return;
            }
            for (int i = 0; i < static_cast<int>(detail_.d.add_apps.size()) && i < 4; ++i)
            {
                if (!detail_.web && inside(add_app_rect(i), x, y))
                {
                    detail_.focus = i + 1;
                    play();
                    return;
                }
            }
        }
        return;
    }
    if (tab_ == kHome)
    {
        for (int i = 0; i < home_item_count(); ++i)
        {
            if (inside(home_item_rect(i), x, y))
            {
                home_focus_ = i;
                home_activate(i);
                return;
            }
        }
    }
    else if (tab_ == kConfig)
    {
        for (int r = 0; r < kConfigRows; ++r)
        {
            const Rect row = config_row_rect(r);
            if (inside(row, x, y))
            {
                config_focus_ = r;
                const bool slider = r == 1 || r == 2;
                config_change(r, slider ? (x < row.x + row.w - 190.0f ? -1 : 1) : 0);
                return;
            }
        }
    }
}

void App::move_focus(int region)
{
    if (region == region_)
        return;
    region_ = region;
    sound(static_cast<int>(audio::Sfx::move));
    if (region == kSidebar)
    {
        pointer_.hover_region = kSidebar;
        pointer_.hover_index = sidebar_focus();
    }
    else if (pointer_.hover_region == kSidebar && !pointer_.visible)
    {
        pointer_.hover_region = -1;
    }
    if (region == kGrid)
        select_for_detail();
}

void App::switch_tab(int tab)
{
    tab = std::clamp(tab, 0, kTabCount - 1);
    if (tab == tab_)
        return;
    if (search_wait_ >= 0.0f) // typed but not applied yet: this tab's list follows it first
        apply_search(search_);
    tab_search_[static_cast<std::size_t>(tab_)] = search_;
    side_scroll_ = side_scroll_to_ = 0.0f;
    tab_ = tab;
    search_ = tab_search_[static_cast<std::size_t>(tab_)];
    images_.cancel_pending(); // the new page's pictures first, not the old page's queue
    popup_ = false;
    stop_armed_ = false;
    pointer_.hover_region = -1;
    sound(static_cast<int>(audio::Sfx::tab));
    const std::uint64_t token = detail_.token + 1;
    detail_ = Detail{};
    detail_.token = token;
    if (Browse *b = browse())
    {
        region_ = kGrid;
        if (b->pages.empty() && b->loading.empty())
        {
            if (b->total < 0)
                reload_browse(*b);
            else
                ensure_pages(*b); // refreshed while away: same place, new data
        }
        else
            select_for_detail();
    }
    else if (tab_ == kWeb)
    {
        region_ = web_.items.empty() ? kSidebar : kGrid;
        if (region_ == kSidebar)
        {
            pointer_.hover_region = kSidebar;
            pointer_.hover_index = std::max(1, web_.filter);
        }
        if (!web_.sources.empty() && web_.items.empty() && !web_.loading)
            load_web(false);
        select_for_detail();
    }
    else
    {
        region_ = kPage;
    }
    if (tab_ == kLogs)
    {
        backend_.log([this](std::string text) {
            log_text_ = text.empty() ? std::string("The log is empty.") : std::move(text);
            log_scroll_ = 1e9f;
        });
    }
    if (tab_ == kHome)
        refresh_stats();
}

void App::activate_sidebar(int row)
{
    const auto rows = sidebar_rows();
    if (row < 0 || row >= static_cast<int>(rows.size()) || rows[static_cast<std::size_t>(row)].kind == 5)
        return;
    if (rows[static_cast<std::size_t>(row)].kind == 7)
    {
        open_keyboard(2);
        return;
    }
    sound(static_cast<int>(audio::Sfx::select));
    set_sidebar_focus(row);
    const std::uint64_t token = detail_.token + 1;
    detail_ = Detail{};
    detail_.token = token;
    if (Browse *b = browse())
        reload_browse(*b);
    else if (tab_ == kWeb)
        load_web(false);
}

void App::open_keyboard(int purpose)
{
    sound(static_cast<int>(audio::Sfx::open));
    const std::string title = purpose == 1 ? "Search" : "Address of a .swf file (or a page with one)";
    const std::string text = purpose == 1 ? search_ : "https://";
    if (services::keyboard_open(title, text))
        keyboard_ = purpose;
    else
        open_osk(purpose, title, text); // no system keyboard: the app's own
}

// ================================================================= the app's keyboard

namespace
{
constexpr int kOskRows = 5, kOskCols = 10;
const char *const kOskLetters[3] = {"qwertyuiop", "asdfghjkl:", "zxcvbnm./_"};
const char *const kOskSymbols[3] = {"!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>/?"};
// the bottom row: Caps, Symbols, Space, Delete, Done, in tenths of the width
const float kOskBottomStart[5] = {0.0f, 1.5f, 3.0f, 6.0f, 8.0f};
const float kOskBottomWidth[5] = {1.5f, 1.5f, 3.0f, 2.0f, 2.0f};
int osk_cols(int row) { return row == kOskRows - 1 ? 5 : kOskCols; }
} // namespace

void App::open_osk(int purpose, const std::string &title, const std::string &text)
{
    osk_ = Osk{};
    osk_.open = true;
    osk_.purpose = purpose;
    osk_.title = title;
    osk_.text = text;
    osk_.row = 1;
}

void App::close_osk(bool accept)
{
    osk_.open = false;
    sound(static_cast<int>(accept ? audio::Sfx::select : audio::Sfx::back));
    if (!accept)
        return;
    if (osk_.purpose == 1)
        apply_search(osk_.text);
    else
        start_swf_link(osk_.text);
}

Rect App::osk_panel() const { return {400.0f, 250.0f, 1120.0f, 620.0f}; }

Rect App::osk_key_rect(int row, int col) const
{
    const Rect p = osk_panel();
    const float unit = (p.w - 80.0f) / kOskCols;
    const float y = p.y + 190.0f + row * 80.0f;
    if (row == kOskRows - 1)
        return {p.x + 40.0f + kOskBottomStart[col] * unit, y, kOskBottomWidth[col] * unit - 10.0f, 70.0f};
    return {p.x + 40.0f + col * unit, y, unit - 10.0f, 70.0f};
}

std::string App::osk_key_label(int row, int col) const
{
    if (row == 0)
        return std::string(1, "1234567890"[col]);
    if (row == kOskRows - 1)
    {
        const char *const names[5] = {"Caps", osk_.symbols ? "abc" : "#+=", "Space", "Delete", "Done"};
        return names[col];
    }
    char c = (osk_.symbols ? kOskSymbols : kOskLetters)[row - 1][col];
    if (osk_.caps && c >= 'a' && c <= 'z')
        c = static_cast<char>(c - 'a' + 'A');
    return std::string(1, c);
}

void App::osk_press(int row, int col)
{
    if (row == kOskRows - 1)
    {
        switch (col)
        {
        case 0: osk_.caps = !osk_.caps; break;
        case 1: osk_.symbols = !osk_.symbols; break;
        case 2:
            if (osk_.text.size() < 200)
                osk_.text += ' ';
            break;
        case 3:
            if (!osk_.text.empty())
                osk_.text.pop_back();
            break;
        default: close_osk(true); return;
        }
        sound(static_cast<int>(audio::Sfx::move));
        return;
    }
    if (osk_.text.size() < 200)
        osk_.text += osk_key_label(row, col);
    sound(static_cast<int>(audio::Sfx::move));
}

void App::osk_click(float x, float y)
{
    for (int row = 0; row < kOskRows; ++row)
    {
        for (int col = 0; col < osk_cols(row); ++col)
        {
            if (inside(osk_key_rect(row, col), x, y))
            {
                osk_.row = row;
                osk_.col = col;
                osk_press(row, col);
                return;
            }
        }
    }
}

void App::update_osk(const InputFrame &input)
{
    if (input.nav == Direction::left || input.nav == Direction::right)
    {
        const int n = osk_cols(osk_.row);
        osk_.col = (osk_.col + (input.nav == Direction::left ? n - 1 : 1)) % n;
        sound(static_cast<int>(audio::Sfx::move));
    }
    else if (input.nav == Direction::up || input.nav == Direction::down)
    {
        // to the key in the next row that sits nearest under (or over) this one
        const int to = (osk_.row + (input.nav == Direction::up ? kOskRows - 1 : 1)) % kOskRows;
        const float cx = osk_key_rect(osk_.row, osk_.col).cx();
        int best = 0;
        for (int col = 1; col < osk_cols(to); ++col)
        {
            if (std::fabs(osk_key_rect(to, col).cx() - cx) < std::fabs(osk_key_rect(to, best).cx() - cx))
                best = col;
        }
        osk_.row = to;
        osk_.col = best;
        sound(static_cast<int>(audio::Sfx::move));
    }
    if (input.is_pressed(Action::confirm))
        osk_press(osk_.row, osk_.col);
    else if (input.is_pressed(Action::west))
        osk_press(kOskRows - 1, 3); // delete
    else if (input.is_pressed(Action::north))
        osk_press(kOskRows - 1, 2); // space
    else if (input.is_pressed(Action::l3))
        osk_press(kOskRows - 1, 0); // caps
    else if (input.is_pressed(Action::menu) || input.is_pressed(Action::jump_next))
        close_osk(true);
    else if (input.is_pressed(Action::back))
        close_osk(false);
}

void App::draw_osk(gfx::DrawList &list) const
{
    list.rounded_rect({0, 0, kW, kH}, 0, Color::rgb(0x000000, 0.55f));
    const Rect p = osk_panel();
    list.shadow(p, 16, 40, Color::rgb(0x000000, 0.5f));
    list.bordered_rect(p, 16, kPanel, 1.0f, kLine);
    ui::text(list, fonts_.bold, osk_.title, p.x + 40, p.y + 58, 28, kText);
    // what's typed so far, its end in view
    const Rect field{p.x + 40, p.y + 86, p.w - 80, 70};
    list.bordered_rect(field, 8, kInput, 2.0f, kAccentHi);
    std::string shown = osk_.text;
    const float room = field.w - 48;
    while (!shown.empty() && fonts_.regular.measure(shown, 28) > room)
        shown.erase(0, 1);
    ui::text(list, fonts_.regular, shown, field.x + 20, field.cy() + 10, 28, kText);
    if (std::fmod(clock_, 1.0f) < 0.6f)
        list.rounded_rect({field.x + 22 + fonts_.regular.measure(shown, 28), field.y + 16, 3, field.h - 32}, 1.5f,
                          kAccentHi);
    for (int row = 0; row < kOskRows; ++row)
    {
        for (int col = 0; col < osk_cols(row); ++col)
        {
            const Rect k = osk_key_rect(row, col);
            const bool f = osk_.row == row && osk_.col == col;
            const bool hot = pointer_.visible && inside(k, pointer_.x, pointer_.y);
            const bool lit = row == kOskRows - 1 && ((col == 0 && osk_.caps) || (col == 1 && osk_.symbols));
            list.bordered_rect(k, 8, f ? kAccent : hot ? kRowHover : kRow, lit ? 2.0f : 1.0f, lit ? kAccentHi : kLine);
            const bool word = row == kOskRows - 1;
            ui::text(list, fonts_.bold, osk_key_label(row, col), k.cx(), k.cy() + (word ? 8 : 10), word ? 22 : 28,
                     f ? kWhite : kText, Align::center);
        }
    }
}

void App::apply_search(const std::string &text)
{
    search_ = text;
    search_wait_ = -1.0f;
    const std::uint64_t token = detail_.token + 1;
    detail_ = Detail{};
    detail_.token = token;
    if (Browse *b = browse())
        reload_browse(*b);
    else if (tab_ == kWeb)
        load_web(false);
}

void App::start_swf_link(const std::string &url)
{
    if (url.size() < 12 || url.find('.') == std::string::npos)
        return;
    if (tab_ != kWeb)
        switch_tab(kWeb);
    region_ = kDetail;
    const std::uint64_t token = detail_.token + 1;
    detail_ = Detail{};
    detail_.token = token;
    detail_.web = true;
    detail_.item.page = url;
    detail_.item.swf = url;
    const std::size_t slash = url.find_last_of('/');
    detail_.item.title = slash == std::string::npos || slash + 1 >= url.size() ? url : url.substr(slash + 1);
    detail_.web_source = "a link";
    play();
}

void App::set_sort(int option)
{
    if (option < kSortCount)
        settings_.sort = kSorts[option].id;
    else
        settings_.desc = option == kSortCount + 1;
    save_settings();
    sound(static_cast<int>(audio::Sfx::select));
    reload_all();
}

void App::play()
{
    if (detail_.playing)
        return;
    if (!detail_.web)
    {
        if (detail_.loading || detail_.id.empty())
            return;
        if (detail_.focus == 0 && !detail_.d.playable)
        {
            toast(detail_.d.reason.empty() ? "This entry doesn't play on the PS5." : detail_.d.reason, true);
            return;
        }
        detail_.play_add_app =
            detail_.focus == 0 || detail_.focus > static_cast<int>(detail_.d.add_apps.size())
                ? std::string()
                : detail_.d.add_apps[static_cast<std::size_t>(detail_.focus - 1)].id;
    }
    sound(static_cast<int>(audio::Sfx::play));
    detail_.playing = true;
    detail_.status = detail_.web ? "Finding the Flash file..." : "Getting the game ready...";
    detail_.status_error = false;
    detail_.progress = -1.0;
    detail_.poll = 0.0f;
    ++detail_.token;
}

void App::handle_play(const PlayReply &reply, std::uint64_t token)
{
    if (token != detail_.token)
        return;
    if (reply.status == "pending")
    {
        const Job *job = nullptr;
        for (const Job &j : reply.jobs)
        {
            if (detail_.web || j.game_id == detail_.id)
                job = &j;
        }
        detail_.progress = job && job->total > 0 ? static_cast<double>(job->done) / job->total : 0.0;
        const std::string state = job ? job->state : "queued";
        if (detail_.web)
            detail_.status = (state.empty() ? std::string("Working") : state) +
                             (job && job->total > 0 ? " (" + megabytes(job->done) + " of " + megabytes(job->total) + ")"
                                                    : std::string("..."));
        else if (state == "downloading" && job && job->total > 0)
            detail_.status = "Downloading " + megabytes(job->done) + " of " + megabytes(job->total);
        else if (state == "extracting")
            detail_.status = "Unpacking";
        else if (state == "verifying")
            detail_.status = "Checking the download";
        else
            detail_.status = "Waiting to download";
        detail_.poll = 0.8f;
        return;
    }
    detail_.playing = false;
    detail_.progress = -1.0;
    detail_.status_error = false;
    if (reply.status == "open")
    {
        detail_.status = "Opening the game in the PS5 browser...";
        if (!services::open_browser(reply.url))
        {
            detail_.status = "The browser didn't open.";
            detail_.status_error = true;
        }
        return;
    }
    detail_.status = reply.message;
    if (reply.status == "launched")
        toast(reply.message);
    else if (reply.status != "message")
    {
        detail_.status_error = true;
        sound(static_cast<int>(audio::Sfx::error));
    }
}

// ============================================================ navigation

void App::update_main(const InputFrame &input, float dt)
{
    (void)dt;
    if (popup_)
    {
        nav_popup(input);
        return;
    }
    if (input.is_pressed(Action::page_prev))
    {
        switch_tab((tab_ + kTabCount - 1) % kTabCount);
        return;
    }
    if (input.is_pressed(Action::page_next))
    {
        switch_tab((tab_ + 1) % kTabCount);
        return;
    }
    const bool lists = browse() || tab_ == kWeb;
    if (lists && input.is_pressed(Action::north))
    {
        if (usb_keyboard_)
            move_focus(kSearch);
        else
            open_keyboard(1);
        return;
    }
    if (browse() && input.is_pressed(Action::west))
    {
        popup_ = true;
        popup_focus_ = 0;
        for (int i = 0; i < kSortCount; ++i)
        {
            if (settings_.sort == kSorts[i].id)
                popup_focus_ = i;
        }
        sound(static_cast<int>(audio::Sfx::open));
        return;
    }
    if (input.is_pressed(Action::menu) && tab_ != kConfig)
    {
        open_keyboard(2);
        return;
    }
    if (lists && std::fabs(input.stick2_y) > 0.2f)
    {
        if (float *target = grid_scroll())
            *target = std::clamp(*target + input.stick2_y * 28.0f, 0.0f, grid_max_scroll());
    }
    switch (region_)
    {
    case kMenu:
        nav_menu(input);
        break;
    case kSearch:
        if (input.is_pressed(Action::confirm))
            open_keyboard(1);
        else if (input.is_pressed(Action::back) || input.nav == Direction::down)
        {
            if (input.is_pressed(Action::back) && !search_.empty())
                apply_search("");
            move_focus(kGrid);
        }
        break;
    case kSidebar:
        nav_sidebar(input);
        break;
    case kGrid:
        nav_grid(input);
        break;
    case kDetail:
        nav_detail(input);
        break;
    default:
        nav_page(input);
        break;
    }
}

void App::nav_menu(const InputFrame &input)
{
    if (input.nav == Direction::down || input.is_pressed(Action::back))
        move_focus(browse() || tab_ == kWeb ? kGrid : kPage);
}

void App::nav_popup(const InputFrame &input)
{
    if (input.nav == Direction::up)
    {
        popup_focus_ = (popup_focus_ + kPopupRows - 1) % kPopupRows;
        sound(static_cast<int>(audio::Sfx::move));
    }
    else if (input.nav == Direction::down)
    {
        popup_focus_ = (popup_focus_ + 1) % kPopupRows;
        sound(static_cast<int>(audio::Sfx::move));
    }
    if (input.is_pressed(Action::confirm))
    {
        set_sort(popup_focus_);
        popup_ = false;
    }
    else if (input.is_pressed(Action::back) || input.is_pressed(Action::west))
    {
        popup_ = false;
        sound(static_cast<int>(audio::Sfx::back));
    }
}

void App::nav_sidebar(const InputFrame &input)
{
    const auto rows = sidebar_rows();
    const int n = static_cast<int>(rows.size());
    if (n == 0)
        return;
    // the sidebar's cursor is the highlighted row (the active filter stays put)
    int cursor = pointer_.hover_region == kSidebar && pointer_.hover_index >= 0 ? pointer_.hover_index
                                                                                 : sidebar_focus();
    cursor = std::clamp(cursor, 0, n - 1);
    if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int step = input.nav == Direction::up ? -1 : 1;
        int next = cursor;
        do
            next += step;
        while (next >= 0 && next < n && rows[static_cast<std::size_t>(next)].kind == 5);
        if (next >= 0 && next < n)
        {
            cursor = next;
            sound(static_cast<int>(audio::Sfx::move));
        }
    }
    pointer_.hover_region = kSidebar;
    pointer_.hover_index = cursor;
    // follow the controller's cursor (not the pointer's row: resting the
    // pointer on a half-shown row would keep scrolling)
    if (input.nav == Direction::up || input.nav == Direction::down || !pointer_.visible)
        side_follow(cursor);
    if (input.is_pressed(Action::confirm))
    {
        const bool link = rows[static_cast<std::size_t>(cursor)].kind == 7;
        activate_sidebar(cursor);
        if (!link)
            move_focus(kGrid);
    }
    else if (input.nav == Direction::right || input.is_pressed(Action::back))
    {
        if (grid_count() > 0)
            move_focus(kGrid);
    }
}

void App::nav_grid(const InputFrame &input)
{
    const int count = grid_count();
    const int cols = grid_cols();
    const int f = grid_focus();
    switch (input.nav)
    {
    case Direction::left:
        if (f % cols == 0)
        {
            move_focus(kSidebar);
            return;
        }
        set_grid_focus(f - 1, true);
        break;
    case Direction::right:
        if (f % cols == cols - 1 || f + 1 >= count)
        {
            move_focus(kDetail);
            return;
        }
        set_grid_focus(f + 1, true);
        break;
    case Direction::up:
        if (f < cols)
        {
            if (!input.nav_repeat)
                move_focus(kSearch);
            return;
        }
        set_grid_focus(f - cols, true);
        break;
    case Direction::down:
        if (f + cols < count)
            set_grid_focus(f + cols, true);
        else if (f / cols < (count - 1) / cols)
            set_grid_focus(count - 1, true);
        break;
    default:
        break;
    }
    const int page_rows = std::max(1, static_cast<int>(grid_area().h / (tile_h() + kGap)) - 1);
    if (input.is_pressed(Action::jump_prev))
        set_grid_focus(f - cols * page_rows, true);
    if (input.is_pressed(Action::jump_next))
        set_grid_focus(std::min(count - 1, f + cols * page_rows), true);
    if (input.is_pressed(Action::confirm))
        play();
    else if (input.is_pressed(Action::back))
    {
        if (!search_.empty())
        {
            sound(static_cast<int>(audio::Sfx::back));
            apply_search("");
        }
        else
        {
            move_focus(kSidebar);
        }
    }
}

void App::nav_detail(const InputFrame &input)
{
    const int count = detail_.web ? 1 : 1 + static_cast<int>(std::min<std::size_t>(detail_.d.add_apps.size(), 4));
    if (input.nav == Direction::up && detail_.focus > 0)
    {
        --detail_.focus;
        sound(static_cast<int>(audio::Sfx::move));
    }
    else if (input.nav == Direction::down && detail_.focus + 1 < count)
    {
        ++detail_.focus;
        sound(static_cast<int>(audio::Sfx::move));
    }
    else if (input.nav == Direction::left || input.is_pressed(Action::back))
    {
        if (grid_count() > 0)
            move_focus(kGrid);
        else
            move_focus(kSidebar);
        return;
    }
    if (input.is_pressed(Action::confirm))
        play();
}

void App::nav_page(const InputFrame &input)
{
    if (tab_ == kHome)
    {
        const int n = home_item_count();
        home_focus_ = std::clamp(home_focus_, 0, n - 1);
        const Rect cur = home_item_rect(home_focus_);
        if (input.nav != Direction::none)
        {
            // the nearest item in that direction
            int best = -1;
            float best_d = 1e9f;
            const bool horizontal = input.nav == Direction::left || input.nav == Direction::right;
            for (int i = 0; i < n; ++i)
            {
                if (i == home_focus_)
                    continue;
                const Rect r = home_item_rect(i);
                const float dx = r.cx() - cur.cx(), dy = r.cy() - cur.cy();
                bool ok = false;
                switch (input.nav)
                {
                case Direction::left: ok = dx < -10.0f; break;
                case Direction::right: ok = dx > 10.0f; break;
                case Direction::up: ok = dy < -10.0f; break;
                case Direction::down: ok = dy > 10.0f; break;
                default: break;
                }
                const float d = horizontal ? std::fabs(dx) + std::fabs(dy) * 2.5f : std::fabs(dy) + std::fabs(dx) * 2.5f;
                if (ok && d < best_d)
                {
                    best_d = d;
                    best = i;
                }
            }
            if (best >= 0)
            {
                home_focus_ = best;
                sound(static_cast<int>(audio::Sfx::move));
            }
        }
        if (input.is_pressed(Action::confirm))
            home_activate(home_focus_);
        return;
    }
    if (tab_ == kConfig)
    {
        if (input.nav == Direction::up && config_focus_ > 0)
        {
            --config_focus_;
            stop_armed_ = false;
            sound(static_cast<int>(audio::Sfx::move));
        }
        else if (input.nav == Direction::down && config_focus_ + 1 < kConfigRows)
        {
            ++config_focus_;
            stop_armed_ = false;
            sound(static_cast<int>(audio::Sfx::move));
        }
        else if (input.nav == Direction::left)
            config_change(config_focus_, -1);
        else if (input.nav == Direction::right)
            config_change(config_focus_, 1);
        if (input.is_pressed(Action::confirm))
            config_change(config_focus_, 0);
        return;
    }
    if (tab_ == kLogs)
    {
        if (input.nav == Direction::up)
            log_scroll_ = std::max(0.0f, log_scroll_ - 120.0f);
        else if (input.nav == Direction::down)
            log_scroll_ += 120.0f;
        if (input.is_pressed(Action::jump_prev))
            log_scroll_ = std::max(0.0f, log_scroll_ - 700.0f);
        if (input.is_pressed(Action::jump_next))
            log_scroll_ += 700.0f;
        if (input.is_pressed(Action::north) || input.is_pressed(Action::confirm))
        {
            sound(static_cast<int>(audio::Sfx::select));
            backend_.log([this](std::string text) {
                log_text_ = text.empty() ? std::string("The log is empty.") : std::move(text);
                log_scroll_ = 1e9f;
            });
        }
    }
}

void App::home_activate(int item)
{
    sound(static_cast<int>(audio::Sfx::select));
    if (item == 0)
    {
        if (!sync_.running)
        {
            if (sync_.source == "pc")
            {
                toast("This library is a copy from a PC Flashpoint install; update it there.");
                return;
            }
            sync_started_ = true;
            backend_.sync(true, [this](SyncStatus s) { sync_ = s; });
            sync_poll_ = 1.0f;
        }
        return;
    }
    if (item == 1)
        switch_tab(kArcade);
    else if (item == 2)
        switch_tab(kTheatre);
    else if (item == 3)
        switch_tab(kWeb);
    else if (item == 4)
        open_keyboard(2);
    else
    {
        const int i = item - 5;
        if (i < static_cast<int>(recent_.size()))
        {
            const Game g = recent_[static_cast<std::size_t>(i)];
            switch_tab(g.library == "theatre" ? kTheatre : kArcade);
            const std::uint64_t token = detail_.token + 1;
            detail_ = Detail{};
            detail_.token = token;
            detail_.id = g.id;
            detail_.d.id = g.id;
            detail_.d.title = g.title;
            detail_.loading = true;
            detail_.wait = 0.01f;
            region_ = kDetail;
        }
    }
}

void App::config_change(int row, int delta)
{
    const auto step = [&](float &v) {
        if (delta == 0)
            v = v >= 0.95f ? 0.0f : v + 0.1f;
        else
            v += delta * 0.1f;
        v = std::clamp(std::round(v * 10.0f) / 10.0f, 0.0f, 1.0f);
    };
    switch (row)
    {
    case 0:
        settings_.theme = (settings_.theme + (delta < 0 ? kThemeCount - 1 : 1)) % kThemeCount;
        apply_theme(settings_.theme);
        break;
    case 1: settings_.intro = !settings_.intro; break;
    case 2: step(settings_.music); break;
    case 3: step(settings_.sounds); break;
    case 4: settings_.ps5_only = !settings_.ps5_only; break;
    case 5:
        settings_.show_extreme = !settings_.show_extreme;
        reload_all();
        break;
    case 6:
        settings_.web_adult = !settings_.web_adult;
        web_.items.clear();
        ++web_.token;
        break;
    case 7: settings_.tile_size = (settings_.tile_size + (delta < 0 ? 2 : 1)) % 3; break;
    case 8: settings_.usb_input = !settings_.usb_input; break;
    case 9:
        if (delta == 0)
            home_activate(0);
        return;
    case 10:
        if (delta != 0)
            return;
        sound(static_cast<int>(audio::Sfx::select));
        backend_.refresh_info([this] { toast("Connected to the Flashpoint service."); });
        return;
    case 11:
        if (delta != 0)
            return;
        if (!stop_armed_)
        {
            stop_armed_ = true;
            sound(static_cast<int>(audio::Sfx::open));
            return;
        }
        stop_armed_ = false;
        backend_.stop_service([this](bool ok) {
            toast(ok ? "The background service stopped. It starts again with the app." : "The service didn't answer.",
                  !ok);
            if (ok)
                backend_.connect();
        });
        return;
    default: return;
    }
    sound(static_cast<int>(row == 2 || row == 3 ? audio::Sfx::move : audio::Sfx::select));
    save_settings();
}

// ================================================================= drawing

void App::draw(gfx::DrawList &list) const
{
    list.set_corner_scale(intro_ && intro_fade_ < 0.0f ? 1.0f : g_corners);
    if (intro_ && intro_fade_ < 0.0f)
    {
        draw_intro(list);
        draw_pointer(list);
        return;
    }
    list.rounded_rect({0, 0, kW, kH}, 0, kBg);
    if (backend_.link() != Link::ready)
    {
        draw_link_state(list);
    }
    else if (browse() || tab_ == kWeb)
    {
        draw_grid(list);
        draw_sidebar(list);
        draw_detail(list);
    }
    else if (tab_ == kHome)
        draw_home(list);
    else if (tab_ == kLogs)
        draw_logs(list);
    else if (tab_ == kConfig)
        draw_config(list);
    else
        draw_about(list);
    draw_menu(list);
    draw_status(list);
    if (popup_)
        draw_popup(list);
    if (osk_.open)
        draw_osk(list);
    draw_toast(list);
    if (intro_)
    {
        // the intro fading out over the app
        list.push_opacity(1.0f - std::clamp(intro_fade_ / 0.7f, 0.0f, 1.0f));
        draw_intro(list);
        list.pop_opacity();
    }
    draw_pointer(list);
}

void App::draw_intro(gfx::DrawList &list) const
{
    const float t = intro_t_;
    list.rounded_rect({0, 0, kW, kH}, 0, Color::rgb(0x101014));
    // the timeline strip: a frame per half beat, the playhead moving with the music
    const float half_beat = 30.0f / 124.0f;
    const float strip_y = 860.0f;
    list.push_opacity(ease_out((t - 0.1f) / 0.8f));
    list.rounded_rect({0, strip_y, kW, 120.0f}, 0, Color::rgb(0x1a1b22));
    list.rounded_rect({0, strip_y, kW, 2.0f}, 0, Color::rgb(0x4b5263));
    const float cell = kW / 32.0f;
    for (int i = 0; i <= 32; ++i)
    {
        const float x = i * cell;
        list.rounded_rect({x, strip_y + 30.0f, 1.0f, 90.0f}, 0, Color::rgb(0x343844));
        if (i % 4 == 0)
            ui::text(list, fonts_.pixel, std::to_string(i + 1), x + 5.0f, strip_y + 22.0f, 14, kDim);
        if (t > i * half_beat && i % 2 == 0 && i < 32)
            list.circle(x + cell / 2.0f, strip_y + 76.0f, 6.0f, Color::rgb(0xf3f4f7, 0.85f));
    }
    const float head = std::min(t / half_beat, 32.0f) * cell;
    list.rounded_rect({head - 1.5f, strip_y, 3.0f, 120.0f}, 0, kRed);
    list.rounded_rect({head - 16.0f, strip_y, 32.0f, 22.0f}, 4, kRed);
    list.pop_opacity();

    // the mark springs in, the name follows
    const float s = ease_back((t - 0.25f) / 0.9f);
    const float size = 300.0f * s;
    if (s > 0.01f)
    {
        const float pulse = 0.3f + 0.12f * std::sin(t * 6.4f);
        list.glow({960.0f - size * 0.3f, 380.0f - size * 0.3f, size * 0.6f, size * 0.6f}, size * 0.3f, 140.0f * s,
                  Color::rgb(0xb8b2f2, pulse * std::min(1.0f, s)));
        draw_mark(list, 960.0f - size / 2.0f, 380.0f - size / 2.0f, size);
    }
    const float name = ease_out((t - 1.7f) / 0.7f);
    if (name > 0.0f)
    {
        list.push_opacity(name);
        const float y = 640.0f + (1.0f - name) * 30.0f;
        const float w1 = fonts_.bold.measure("Flashpoint", 84);
        const float w2 = fonts_.regular.measure(" PS5", 84);
        const float x = 960.0f - (w1 + w2) / 2.0f;
        ui::text(list, fonts_.bold, "Flashpoint", x, y, 84, kWhite);
        ui::text(list, fonts_.regular, " PS5", x + w1, y, 84, Color::rgb(0xb8b2f2));
        list.pop_opacity();
    }
    const float tag = ease_out((t - 3.9f) / 0.8f);
    if (tag > 0.0f)
    {
        list.push_opacity(tag);
        ui::text(list, fonts_.regular, "Web games and animations, played on your PS5", 960.0f, 716.0f, 32, kMuted,
                 Align::center);
        list.pop_opacity();
    }
    // a flash on the last bar line
    const float flash = 1.0f - std::fabs(t - 7.74f) / 0.25f;
    if (flash > 0.0f)
        list.rounded_rect({0, 0, kW, kH}, 0, Color::rgb(0xffffff, 0.35f * flash));
    if (t > 1.0f && intro_fade_ < 0.0f)
        ui::text(list, fonts_.regular, "Press any button to skip", kW - 48.0f, 60.0f, 22,
                 Color::rgb(0x8a8a92, std::min(1.0f, (t - 1.0f) * 2.0f)), Align::right);
}

void App::draw_glyph(gfx::DrawList &list, const char *which, float x, float cy) const
{
    const std::string w = which;
    const float s = 26.0f;
    if (is_shape(w))
    {
        const float c = x + s / 2.0f;
        list.circle(c, cy, s / 2.0f, kRow);
        if (w == "cross")
        {
            list.line(c - 5, cy - 5, c + 5, cy + 5, 2.2f, kText);
            list.line(c - 5, cy + 5, c + 5, cy - 5, 2.2f, kText);
        }
        else if (w == "circle")
            list.ring(c, cy, 6.0f, 2.2f, kText);
        else if (w == "triangle")
        {
            list.line(c, cy - 6, c - 6, cy + 4.5f, 2.2f, kText);
            list.line(c - 6, cy + 4.5f, c + 6, cy + 4.5f, 2.2f, kText);
            list.line(c + 6, cy + 4.5f, c, cy - 6, 2.2f, kText);
        }
        else
            list.bordered_rect({c - 5, cy - 5, 10, 10}, 1, kRow, 2.2f, kText);
        return;
    }
    const float tw = fonts_.bold.measure(which, 14);
    list.bordered_rect({x, cy - 12, tw + 14, 24}, 5, kRow, 1.0f, kLine);
    ui::text(list, fonts_.bold, which, x + 7, cy + 5, 14, kText);
}

float App::draw_hint(gfx::DrawList &list, const char *glyph, const std::string &label, float x, float cy) const
{
    draw_glyph(list, glyph, x, cy);
    x += (is_shape(glyph) ? 26.0f : fonts_.bold.measure(glyph, 14) + 14.0f) + 8.0f;
    ui::text(list, fonts_.regular, label, x, cy + 6, 18, kMuted);
    return x + fonts_.regular.measure(label, 18) + 22.0f;
}

void App::draw_icon(gfx::DrawList &list, int icon, float x, float cy, float size, Color c) const
{
    const float h = size / 2.0f;
    switch (icon)
    {
    case kIconAll:
        for (int i = 0; i < 4; ++i)
            list.rounded_rect({x + (i % 2) * (h + 1.5f), cy - h + (i / 2) * (h + 1.5f), h - 1.5f, h - 1.5f}, 2, c);
        break;
    case kIconCheck:
        list.line(x + 2, cy, x + h - 1, cy + h - 3, 3.0f, c);
        list.line(x + h - 1, cy + h - 3, x + size - 1, cy - h + 3, 3.0f, c);
        break;
    case kIconClock:
        list.ring(x + h, cy, h - 1, 2.4f, c);
        list.line(x + h, cy, x + h, cy - h + 5, 2.4f, c);
        list.line(x + h, cy, x + h + 5, cy + 2, 2.4f, c);
        break;
    case kIconDownload:
        list.line(x + h, cy - h + 2, x + h, cy + 3, 2.6f, c);
        list.line(x + h - 6, cy - 3, x + h, cy + 4, 2.6f, c);
        list.line(x + h + 6, cy - 3, x + h, cy + 4, 2.6f, c);
        list.rounded_rect({x + 2, cy + h - 4, size - 4, 3}, 1, c);
        break;
    case kIconLink:
        list.ring(x + h - 4, cy + 3, 5.5f, 2.4f, c);
        list.ring(x + h + 4, cy - 3, 5.5f, 2.4f, c);
        break;
    case kIconGlobe:
        list.ring(x + h, cy, h - 1, 2.2f, c);
        list.line(x + 2, cy, x + size - 2, cy, 2.0f, c);
        list.line(x + h, cy - h + 1, x + h, cy + h - 1, 2.0f, c);
        break;
    case kIconSearch:
        list.ring(x + h - 2, cy - 2, h - 4, 2.4f, c);
        list.line(x + h + 3, cy + 3, x + size - 1, cy + h - 1, 2.6f, c);
        break;
    default:
        list.ring(x + h, cy, h - 2, 3.0f, c);
        break;
    }
}

void App::draw_badge(gfx::DrawList &list, const std::string &platform, float cx, float cy, float r) const
{
    Color bg = Color::rgb(0x6b6b72), fg = kWhite;
    std::string letter = platform.empty() ? "?" : platform.substr(0, 1);
    if (platform == "Flash")
    {
        bg = Color::rgb(0xd32f2f);
        letter = "f";
    }
    else if (platform == "HTML5")
    {
        bg = Color::rgb(0xe8662a);
        letter = "5";
    }
    else if (platform == "Shockwave")
    {
        bg = Color::rgb(0xe9e9ec);
        fg = Color::rgb(0x202024);
        letter = "S";
    }
    else if (platform == "Unity")
    {
        bg = Color::rgb(0x111113);
        letter = "U";
    }
    else if (platform == "Java")
    {
        bg = Color::rgb(0x8a5a2b);
        letter = "J";
    }
    list.circle(cx, cy, r + 1.5f, Color::rgb(0x000000, 0.4f));
    list.circle(cx, cy, r, bg);
    ui::text(list, fonts_.bold, letter, cx, cy + r * 0.42f, r * 1.25f, fg, Align::center);
}

void App::draw_image(gfx::DrawList &list, const std::string &rel, const Rect &r, float radius) const
{
    list.rounded_rect(r, radius, kTileBox);
    const Texture tex = rel.empty() ? Texture{} : images_.get(rel);
    if (tex.id == 0)
    {
        const float s = std::min(r.w, r.h) * 0.42f;
        list.push_opacity(0.16f);
        draw_mark(list, r.cx() - s / 2.0f, r.cy() - s / 2.0f, s, kMark);
        list.pop_opacity();
        return;
    }
    const float scale = std::min(r.w / static_cast<float>(tex.width), r.h / static_cast<float>(tex.height));
    const float w = tex.width * scale, h = tex.height * scale;
    list.image(tex.id, {r.cx() - w / 2.0f, r.cy() - h / 2.0f, w, h}, gfx::kFullUv, kWhite, radius * 0.5f);
}

void App::draw_spinner(gfx::DrawList &list, float cx, float cy, float r) const
{
    list.arc(cx, cy, r, r * 0.22f, clock_ * 5.0f, 4.2f, kAccentHi);
}

void App::draw_menu(gfx::DrawList &list) const
{
    list.rounded_rect({0, 0, kW, kMenuH}, 0, kBar);
    list.rounded_rect({0, kMenuH - 1.0f, kW, 1.0f}, 0, kLine);
    draw_mark(list, 24.0f, 8.0f, 48.0f, kMark);
    ui::text(list, fonts_.bold, "FP", 76.0f, 41.0f, 21, kText);
    for (int t = 0; t < kTabCount; ++t)
    {
        const Rect r = menu_item_rect(t);
        const bool on = t == tab_;
        const bool hot = pointer_.visible && inside(r, pointer_.x, pointer_.y);
        if (on || hot)
            list.rounded_rect({r.x + 4, 10, r.w - 8, kMenuH - 20}, 6, on ? kRowHover : kRow);
        if (on)
            list.rounded_rect({r.x + 14, kMenuH - 6, r.w - 28, 3}, 1.5f, kAccentHi);
        ui::text(list, fonts_.bold, kTabNames[t], r.cx(), 40.0f, 22, on ? kText : kMuted, Align::center);
    }
    const bool lists = browse() || tab_ == kWeb;
    // search
    const Rect s = search_rect();
    const bool focused = region_ == kSearch;
    list.bordered_rect(s, 6, kInput, focused ? 2.0f : 1.0f, focused ? kAccentHi : kLine);
    draw_icon(list, kIconSearch, s.x + 12, s.cy(), 20, kMuted);
    if (search_.empty() && !focused)
    {
        ui::text(list, fonts_.regular, lists ? "Search..." : "Search the library...", s.x + 44, s.cy() + 7, 19, kDim);
    }
    else
    {
        const std::string shown = fonts_.regular.font->fit(search_, 19, s.w - 60);
        const float w = search_.empty() ? 0.0f : ui::text(list, fonts_.regular, shown, s.x + 44, s.cy() + 7, 19, kText);
        if (focused && std::fmod(clock_, 1.0f) < 0.6f)
            list.rounded_rect({s.x + 46 + w, s.y + 9, 2, s.h - 18}, 1, kText);
    }
    // sorting
    const bool sortable = browse() != nullptr; // web lists come in the site's own order
    const auto dropdown = [&](const Rect &r, const std::string &label) {
        const bool hot = sortable && pointer_.visible && inside(r, pointer_.x, pointer_.y);
        list.bordered_rect(r, 6, hot ? kRowHover : kRow, 1.0f, kLine);
        ui::text(list, fonts_.regular, label, r.x + 14, r.cy() + 7, 19, sortable ? kText : kDim);
        list.triangle({r.x + r.w - 26, r.cy() - 4, 12, 8}, sortable ? kMuted : kDim, 0.0f, 3.14159f);
    };
    std::string sort_name = "Title";
    for (const SortOption &o : kSorts)
    {
        if (settings_.sort == o.id)
            sort_name = o.name;
    }
    dropdown(sort_rect(), sort_name);
    dropdown(dir_rect(), settings_.desc ? "Descending" : "Ascending");
    draw_glyph(list, "L1", 1824.0f, 32.0f);
    draw_glyph(list, "R1", 1866.0f, 32.0f);
}

void App::draw_sidebar(gfx::DrawList &list) const
{
    const Rect a = sidebar_area();
    list.rounded_rect(a, 0, kPanel);
    list.rounded_rect({a.x + a.w - 1, a.y, 1, a.h}, 0, kLine);
    const auto rows = sidebar_rows();
    const int active = sidebar_focus();
    list.push_clip(a);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
    {
        const SideRow &row = rows[static_cast<std::size_t>(i)];
        const Rect r = sidebar_row_rect(i);
        if (r.y > a.y + a.h)
            break;
        if (row.kind == 5)
        {
            const float lw = fonts_.bold.measure(row.label, 17);
            ui::text(list, fonts_.bold, row.label, r.x + 20, r.cy() + 8, 17, kDim);
            list.rounded_rect({r.x + 32 + lw, r.cy() + 2, r.w - 52 - lw, 1}, 0, kLine);
            continue;
        }
        const bool on = i == active && row.kind != 7;
        const bool cursor = pointer_.hover_region == kSidebar && pointer_.hover_index == i;
        const Rect hl{r.x + 8, r.y, r.w - 16, r.h};
        if (on)
            list.rounded_rect(hl, 6, kAccent);
        else if (cursor)
            list.rounded_rect(hl, 6, kRowHover);
        if (cursor && region_ == kSidebar)
            list.bordered_rect(hl, 6, Color::rgb(0, 0.0f), 2.0f, kAccentHi);
        const float ix = r.x + 24;
        const Color ic = on ? kWhite : kMuted;
        switch (row.kind)
        {
        case 0: draw_icon(list, kIconAll, ix, r.cy(), 20, ic); break;
        case 1: draw_icon(list, kIconCheck, ix, r.cy(), 20, ic); break;
        case 2: draw_icon(list, kIconClock, ix, r.cy(), 20, ic); break;
        case 3: draw_icon(list, kIconDownload, ix, r.cy(), 20, ic); break;
        case 4: draw_badge(list, row.platform, ix + 10, r.cy(), 10); break;
        case 6: draw_icon(list, kIconGlobe, ix, r.cy(), 20, ic); break;
        case 7: draw_icon(list, kIconLink, ix, r.cy(), 20, kAccentHi); break;
        default: break;
        }
        const float tx = ix + 34;
        const std::string count = row.count >= 0 ? commas(row.count) : std::string();
        const float cw = count.empty() ? 0.0f : fonts_.regular.measure(count, 17) + 10;
        ui::text(list, fonts_.bold, fonts_.bold.font->fit(row.label, 20, r.w - (tx - r.x) - 24 - cw), tx, r.cy() + 7,
                 20, on ? kWhite : (row.kind == 7 ? kAccentHi : kText));
        if (!count.empty())
            ui::text(list, fonts_.regular, count, r.x + r.w - 22, r.cy() + 6, 17, on ? kText : kDim, Align::right);
    }
    list.pop_clip();
}

void App::draw_grid(gfx::DrawList &list) const
{
    const Rect a = grid_area();
    list.rounded_rect(a, 0, kBg);
    const int count = grid_count();
    if (tab_ == kWeb)
    {
        if (web_.sources.empty())
        {
            draw_spinner(list, a.cx(), a.cy() - 40, 24);
            return;
        }
        if (web_.items.empty())
        {
            if (web_.loading)
                draw_spinner(list, a.cx(), a.cy() - 40, 24);
            else if (!web_.error.empty())
                draw_message(list, a, "That site didn't answer", web_.error);
            else if (web_.filter <= 0)
                draw_message(list, a, "Flash from around the web",
                             "Pick a site and a list on the left, or choose \"Play a .swf link\" to open any Flash "
                             "file in Ruffle.");
            else
                draw_message(list, a, search_.empty() ? "Nothing here" : "No matches",
                             web_.hidden ? "What's here is marked as adult, which is hidden. Config changes that."
                             : search_.empty() ? "Try another list."
                                               : "Try fewer words, or another list.");
            return;
        }
    }
    else if (const Browse *b = browse())
    {
        if (b->total <= 0 && sync_.running && sync_.entries <= 0)
        {
            draw_spinner(list, a.cx(), a.cy() - 110, 26);
            const std::string line =
                sync_.total > 0 ? commas(sync_.done) + " of " + commas(sync_.total) + " entries" : "Connecting";
            draw_message(list, {a.x, a.y + 30, a.w, a.h}, "Downloading the Flashpoint library",
                         line + ". You can browse as soon as the first entries arrive.");
            return;
        }
        if (!backend_.info().db_open && b->total <= 0 && !b->loading.size())
        {
            draw_message(list, a, "No Flashpoint library yet",
                         sync_.state == "error" && !sync_.message.empty()
                             ? sync_.message
                             : "Open Home and choose Download library. It comes from Flashpoint's servers.");
            return;
        }
        if (b->total == 0)
        {
            draw_message(list, a, search_.empty() ? "Nothing here yet" : "No matches",
                         b->filter == 2   ? "Games you play show up here, newest first."
                         : b->filter == 3 ? "Games download the first time you play them, then show up here."
                         : search_.empty() ? "Try another filter on the left."
                                           : "Try fewer words, or another filter on the left.");
            return;
        }
        if (b->total < 0)
        {
            if (b->failed)
                draw_message(list, a, "The search didn't finish", "The background service didn't answer in time.");
            else
                draw_spinner(list, a.cx(), a.cy() - 40, 24);
            return;
        }
    }
    list.push_clip(a);
    const int cols = grid_cols();
    const float scroll = tab_ == kWeb ? web_.scroll : browse()->scroll;
    const int first = std::max(0, static_cast<int>((scroll - 20.0f) / (tile_h() + kGap))) * cols;
    const int last = std::min(count, first + (static_cast<int>(a.h / (tile_h() + kGap)) + 2) * cols);
    const int focus = grid_focus();
    const float thumb_h = tile_w() * 0.78f;
    const float title_size = settings_.tile_size == 0 ? 15.0f : settings_.tile_size == 2 ? 19.0f : 17.0f;
    for (int i = first; i < last; ++i)
    {
        const Rect r = tile_rect(i);
        const Tile t = tile_at(i);
        const bool on = i == focus;
        const bool hot = pointer_.hover_region == kGrid && pointer_.hover_index == i;
        if (on)
            list.rounded_rect(r.inset(-6), 8, region_ == kGrid || region_ == kDetail ? kAccent : faded(kAccent, 0.5f));
        else if (hot)
            list.rounded_rect(r.inset(-6), 8, kRowHover);
        const Rect thumb{r.x, r.y, r.w, thumb_h};
        if (t.loaded)
            draw_image(list, t.image, thumb, 6);
        else
            list.rounded_rect(thumb, 6, kTileBox);
        if (g_link_titles) // pictures in a thin frame, as on a Flash portal
            list.bordered_rect(thumb, 6, Color::rgb(0, 0.0f), 1.0f, kLine);
        if (!t.platform.empty())
            draw_badge(list, t.platform, r.x + r.w - 14, r.y + thumb_h - 14, 10);
        if (t.installed)
        {
            list.circle(r.x + 13, r.y + 13, 8, Color::rgb(0x000000, 0.5f));
            list.circle(r.x + 13, r.y + 13, 6, kGreen);
        }
        if (t.loaded)
        {
            const ui::ParagraphLines lines = ui::paragraph_lines(fonts_.regular, t.title, title_size, r.w, 2);
            float y = r.y + thumb_h + title_size + 8;
            for (const std::string &line : lines.lines)
            {
                const Color c = on ? kWhite : g_link_titles ? kAccent : kText;
                ui::text(list, on ? fonts_.bold : fonts_.regular, line, r.cx(), y, title_size, c, Align::center);
                if (g_link_titles) // titles read as links
                {
                    const float lw = (on ? fonts_.bold : fonts_.regular).measure(line, title_size);
                    list.rounded_rect({r.cx() - lw / 2.0f, y + 3.0f, lw, 1.0f}, 0, c);
                }
                y += title_size + 4;
            }
        }
    }
    list.pop_clip();
    if (tab_ == kWeb && web_.loading)
        draw_spinner(list, a.cx(), a.y + a.h - 40, 16);
    const float max = grid_max_scroll();
    if (max > 0.0f)
    {
        const float track = a.h - 20;
        const float knob = std::max(40.0f, track * a.h / (a.h + max));
        const float y = a.y + 10 + (track - knob) * std::clamp(scroll / max, 0.0f, 1.0f);
        list.rounded_rect({a.x + a.w - 12, a.y + 10, 6, track}, 3, kRow);
        list.rounded_rect({a.x + a.w - 12, y, 6, knob}, 3, kDim);
    }
}

void App::draw_detail(gfx::DrawList &list) const
{
    const Rect a = detail_area();
    list.rounded_rect(a, 0, kPanel);
    list.rounded_rect({a.x, a.y, 1, a.h}, 0, kLine);
    const float x = a.x + 24, w = a.w - 48;
    if (!detail_.web && detail_.id.empty())
    {
        if (grid_count() > 0)
            draw_message(list, {a.x, a.y, a.w, a.h * 0.7f}, "", "Choose an entry to see it here.");
        return;
    }
    float y = a.y + 46;
    const std::string &title = detail_.web ? detail_.item.title : detail_.d.title;
    y = ui::paragraph(list, fonts_.bold, ui::paragraph_lines(fonts_.bold, title, 30, w, 3), x, y, 30, 36, kText);
    const float limit = play_button_rect().y - 30;
    const auto field = [&](const char *label, const std::string &value, Color color) {
        if (value.empty() || y > limit - 10)
            return;
        const std::string l = std::string(label) + ": ";
        ui::text(list, fonts_.regular, l, x, y, 19, kMuted);
        const float lw = fonts_.regular.measure(l, 19);
        ui::text(list, fonts_.regular, fonts_.regular.font->fit(value, 19, w - lw), x + lw, y, 19, color);
        y += 28;
    };
    if (detail_.web)
    {
        ui::text(list, fonts_.regular, "from " + detail_.web_source, x, y + 2, 20, kMuted);
        y += 46;
        field("Platform", "Flash", kText);
        field("Plays in", "Ruffle Flash", kText);
        field("Page", detail_.item.page, kText);
        if (!detail_.item.swf.empty() && detail_.item.swf != detail_.item.page)
            field("File", detail_.item.swf, kText);
    }
    else
    {
        std::string by = detail_.d.developer;
        if (!detail_.d.publisher.empty() && detail_.d.publisher != detail_.d.developer && !detail_.loading)
            by += by.empty() ? detail_.d.publisher : ", " + detail_.d.publisher;
        if (!by.empty())
            y = ui::paragraph(list, fonts_.regular, ui::paragraph_lines(fonts_.regular, "by " + by, 20, w, 2), x, y + 2,
                              20, 26, kMuted);
        y += 18;
        if (detail_.loading)
        {
            draw_spinner(list, x + 16, y + 10, 12);
        }
        else
        {
            field("Tags", detail_.d.tags, kText);
            field("Series", detail_.d.series, kText);
            field("Publisher", detail_.d.publisher, kText);
            field("Source", detail_.d.source, kText);
            field("Platform", detail_.d.platform, kText);
            field("Play mode", detail_.d.play_mode, kText);
            field("Status", detail_.d.status, kText);
            field("Released", detail_.d.release_date, kText);
            field("Date added", date_only(detail_.d.date_added), kText);
            field("Last played", date_only(detail_.d.last_played), kText);
            if (detail_.d.size > 0)
                field("Download", megabytes(detail_.d.size) + (detail_.d.installed ? ", on this PS5" : ""),
                      detail_.d.installed ? kGreen : kText);
            if (!detail_.d.description.empty() && y < limit - 40)
            {
                y += 8;
                const int lines = std::max(1, static_cast<int>((limit - y) / 25));
                ui::paragraph(list, fonts_.regular, detail_.d.description, x, y, 18, w, 25, kMuted, std::min(lines, 6));
            }
        }
    }

    // play
    const Rect p = play_button_rect();
    const bool playable = detail_.web || (detail_.d.playable && !detail_.loading);
    const bool pf = region_ == kDetail && detail_.focus == 0;
    const bool hot = pointer_.visible && inside(p, pointer_.x, pointer_.y);
    if (pf)
        list.shadow(p, 10, 18, Color::rgb(0x000000, 0.5f));
    list.bordered_rect(p, 8, playable ? (hot ? kAccentHover : kAccent) : kRow, pf ? 3.0f : 0.0f, kAccentHi);
    const char *label = detail_.playing ? "Getting ready..." : (detail_.web ? "Play in Ruffle" : "Play");
    list.triangle({p.x + 26, p.cy() - 11, 20, 22}, playable ? kWhite : kDim, 0.0f, 1.5708f);
    ui::text(list, fonts_.bold, label, p.x + 62, p.cy() + 10, 28, playable ? kWhite : kDim);
    float sy = p.y + 76;
    if (!detail_.web)
    {
        for (std::size_t i = 0; i < detail_.d.add_apps.size() && i < 4; ++i)
        {
            const Rect r = add_app_rect(static_cast<int>(i));
            const bool f = region_ == kDetail && detail_.focus == static_cast<int>(i) + 1;
            list.bordered_rect(r, 6, kRow, f ? 3.0f : 1.0f, f ? kAccentHi : kLine);
            ui::text(list, fonts_.regular, fonts_.regular.font->fit(detail_.d.add_apps[i].name, 19, r.w - 32), r.x + 16,
                     r.cy() + 7, 19, kText);
            sy += 50;
        }
    }
    if (detail_.progress >= 0.0)
    {
        list.rounded_rect({p.x, sy, p.w, 8}, 4, kRow);
        list.rounded_rect({p.x, sy, p.w * static_cast<float>(std::clamp(detail_.progress, 0.0, 1.0)), 8}, 4, kAccentHi);
        sy += 20;
    }
    if (!detail_.status.empty())
        ui::paragraph(list, fonts_.regular, detail_.status, p.x, sy + 18, 18, p.w, 24,
                      detail_.status_error ? kWarn : kMuted, 3);

    // screenshot along the bottom, as in the launcher
    const Rect shot{a.x + 16, a.y + a.h - 216, a.w - 32, 200};
    draw_image(list, detail_.web ? web_thumb(detail_.item.thumb) : shot_rel(detail_.id), shot, 6);
}

void App::draw_status(gfx::DrawList &list) const
{
    const float y = kH - kStatusH;
    list.rounded_rect({0, y, kW, kStatusH}, 0, kBar);
    list.rounded_rect({0, y, kW, 1}, 0, kLine);
    float x = 20;
    if (stats_.ok)
    {
        const std::string totals = "Total: " + commas(stats_.arcade + stats_.theatre) + "    Arcade: " +
                                   commas(stats_.arcade) + "    Theatre: " + commas(stats_.theatre);
        x += ui::text(list, fonts_.regular, totals, x, y + 28, 18, kMuted) + 30;
    }
    if (sync_.running)
    {
        const float frac = sync_.total > 0 ? static_cast<float>(sync_.done) / sync_.total : 0.0f;
        list.rounded_rect({x, y + 17, 160, 10}, 5, kRow);
        list.rounded_rect({x, y + 17, 160 * std::clamp(frac, 0.0f, 1.0f), 10}, 5, kAccentHi);
        x += 172;
        x += ui::text(list, fonts_.regular, "Library " + std::to_string(static_cast<int>(frac * 100)) + "%", x, y + 28,
                      18, kText) +
             30;
    }
    for (const Job &job : jobs_)
    {
        if (job.state == "done" || job.state == "error")
            continue;
        char pct[16] = "";
        if (job.total > 0)
            std::snprintf(pct, sizeof(pct), " %d%%", static_cast<int>(job.done * 100 / job.total));
        ui::text(list, fonts_.regular,
                 fonts_.regular.font->fit((job.state == "extracting" ? "Unpacking " : "Downloading ") + job.title + pct,
                                          18, 420),
                 x, y + 28, 18, kText);
        break;
    }
    // controller hints for where the focus is
    std::vector<std::pair<const char *, std::string>> hints;
    if (osk_.open)
        hints = {{"cross", "Type"}, {"square", "Delete"}, {"triangle", "Space"}, {"OPTIONS", "Done"},
                 {"circle", "Cancel"}};
    else if (popup_)
        hints = {{"cross", "Choose"}, {"circle", "Close"}};
    else if (backend_.link() != Link::ready)
        hints = {{"cross", "Try again"}};
    else if (browse() || tab_ == kWeb)
    {
        hints.push_back({"cross", region_ == kGrid || region_ == kDetail ? "Play" : "Open"});
        hints.push_back({"triangle", "Search"});
        if (browse())
            hints.push_back({"square", "Sort"});
        hints.push_back({"OPTIONS", ".swf link"});
    }
    else if (tab_ == kConfig)
        hints = {{"cross", "Change"}};
    else if (tab_ == kLogs)
        hints = {{"triangle", "Refresh"}};
    else if (tab_ == kHome)
        hints = {{"cross", "Open"}, {"OPTIONS", ".swf link"}};
    float total = 0;
    for (auto &[g, label] : hints)
        total += (is_shape(g) ? 26.0f : fonts_.bold.measure(g, 14) + 14.0f) + 8 + fonts_.regular.measure(label, 18) + 22;
    float hx = kW - 20 - total;
    for (auto &[g, label] : hints)
        hx = draw_hint(list, g, label, hx, y + kStatusH / 2.0f);
}

void App::draw_message(gfx::DrawList &list, const Rect &area, const std::string &title, const std::string &body) const
{
    const float w = std::min(area.w - 80.0f, 820.0f);
    float y = area.y + area.h * 0.34f;
    if (!title.empty())
    {
        for (const std::string &line : ui::paragraph_lines(fonts_.bold, title, 34, w, 2).lines)
        {
            ui::text(list, fonts_.bold, line, area.cx(), y, 34, kText, Align::center);
            y += 42;
        }
        y += 12;
    }
    for (const std::string &line : ui::paragraph_lines(fonts_.regular, body, 22, w, 6).lines)
    {
        ui::text(list, fonts_.regular, line, area.cx(), y, 22, kMuted, Align::center);
        y += 32;
    }
}

void App::draw_home(gfx::DrawList &list) const
{
    list.rounded_rect({0, kMenuH, kW, kH - kMenuH - kStatusH}, 0, kBg);
    draw_mark(list, 110.0f, 112.0f, 150.0f, kMark);
    ui::text(list, fonts_.bold, "Flashpoint", 280.0f, 196.0f, 64, kText);
    ui::text(list, fonts_.regular, " PS5", 280.0f + fonts_.bold.measure("Flashpoint", 64), 196.0f, 64, kAccentHi);
    ui::text(list, fonts_.regular, "Web games and animations from the Flashpoint Archive, on your TV.", 282.0f, 240.0f,
             23, kMuted);

    // the library card
    const Rect card{120.0f, 310.0f, 800.0f, 460.0f};
    list.rounded_rect(card, 12, kPanel);
    ui::text(list, fonts_.bold, "Your Flashpoint library", card.x + 30, card.y + 56, 30, kText);
    std::string line1, line2;
    const long long entries = std::max(sync_.entries, backend_.info().game_count);
    if (sync_.running)
    {
        line1 = sync_.total > 0 ? "Downloading: " + commas(sync_.done) + " of " + commas(sync_.total) + " entries"
                                : "Connecting to Flashpoint...";
        line2 = "Arcade and Theatre fill in while it downloads.";
    }
    else if (entries > 0)
    {
        line1 = commas(entries) + " entries";
        if (sync_.source == "pc")
            line2 = "Copied from a PC Flashpoint install (" + backend_.info().flashpoint_path + ").";
        else if (sync_.synced_at > 0)
        {
            const long long days = (static_cast<long long>(std::time(nullptr)) - sync_.synced_at) / 86400;
            line2 = days <= 0 ? "Updated from Flashpoint today."
                              : "Updated from Flashpoint " + std::to_string(days) + (days == 1 ? " day ago." : " days ago.");
        }
    }
    else
    {
        line1 = "Not downloaded yet";
        line2 = sync_.state == "error" ? sync_.message
                                       : "The list of games and animations comes from Flashpoint's servers, so no PC "
                                         "is needed. Game files download when you play them.";
    }
    if (entries > 0 && stats_.ok && !sync_.running)
        line1 += ": " + commas(stats_.arcade) + " games and " + commas(stats_.theatre) + " animations";
    ui::text(list, fonts_.regular, line1, card.x + 30, card.y + 112, 24, kText);
    ui::paragraph(list, fonts_.regular, line2, card.x + 30, card.y + 152, 21, card.w - 60, 30,
                  sync_.state == "error" && !sync_.running ? kWarn : kMuted, 4);
    if (sync_.running)
    {
        const float frac = sync_.total > 0 ? static_cast<float>(sync_.done) / sync_.total : 0.0f;
        list.rounded_rect({card.x + 30, card.y + 300, card.w - 60, 12}, 6, kRow);
        list.rounded_rect({card.x + 30, card.y + 300, (card.w - 60) * std::clamp(frac, 0.0f, 1.0f), 12}, 6, kAccentHi);
    }
    const char *const quick[] = {"Arcade", "Theatre", "Web Flash", "Play a .swf link"};
    const int quick_icons[] = {kIconAll, kIconAll, kIconGlobe, kIconLink};
    for (int i = 0; i < home_item_count(); ++i)
    {
        const Rect r = home_item_rect(i);
        const bool f = home_focus_ == i && region_ == kPage;
        const bool hot = pointer_.visible && inside(r, pointer_.x, pointer_.y);
        if (i == 0)
        {
            const bool can = !sync_.running && sync_.source != "pc";
            list.bordered_rect(r, 8, can ? (hot ? kAccentHover : kAccent) : kRow, f ? 3.0f : 0.0f, kAccentHi);
            const char *label = sync_.running ? "Downloading..." : entries > 0 ? "Update library" : "Download library";
            draw_icon(list, kIconDownload, r.x + 22, r.cy(), 22, can ? kWhite : kDim);
            ui::text(list, fonts_.bold, label, r.x + 58, r.cy() + 8, 23, can ? kWhite : kDim);
        }
        else if (i <= 4)
        {
            list.bordered_rect(r, 10, hot || f ? kRowHover : kRow, f ? 3.0f : 1.0f, f ? kAccentHi : kLine);
            draw_icon(list, quick_icons[i - 1], r.x + 24, r.cy(), 24, i == 4 ? kAccentHi : kText);
            ui::text(list, fonts_.bold, quick[i - 1], r.x + 66, r.cy() + 9, 25, kText);
        }
        else
        {
            const Game &g = recent_[static_cast<std::size_t>(i - 5)];
            if (f || hot)
                list.rounded_rect(r.inset(-6), 8, f ? kAccent : kRowHover);
            draw_image(list, logo_rel(g.id), {r.x, r.y, r.w, 132}, 6);
            draw_badge(list, first_platform(g.platform), r.x + r.w - 14, r.y + 118, 10);
            float ty = r.y + 158;
            for (const std::string &l : ui::paragraph_lines(fonts_.regular, g.title, 17, r.w, 2).lines)
            {
                ui::text(list, fonts_.regular, l, r.cx(), ty, 17, kText, Align::center);
                ty += 21;
            }
        }
    }
    ui::text(list, fonts_.bold, "Recently played", 1080.0f, 200.0f, 28, kText);
    if (recent_.empty())
        ui::paragraph(list, fonts_.regular, "What you play shows up here.", 1080.0f, 250.0f, 21, 700, 30, kMuted);
    const float cy = 760.0f;
    ui::text(list, fonts_.bold, "Controls", 1080.0f, cy, 26, kText);
    const char *const tips[] = {
        "Touchpad: slide to move the pointer, press to click. Two fingers scroll.",
        "A USB mouse and keyboard work too: type to search, arrows and Enter to move and play.",
        "L1 and R1 switch pages. Triangle searches, Square sorts.",
        "OPTIONS plays any .swf link in Ruffle Flash.",
    };
    float ty = cy + 42;
    for (const char *tip : tips)
        ty = ui::paragraph(list, fonts_.regular, tip, 1080.0f, ty, 19, 730, 26, kMuted, 2) + 6;
}

void App::draw_logs(gfx::DrawList &list) const
{
    const Rect a{0, kMenuH, kW, kH - kMenuH - kStatusH};
    list.rounded_rect(a, 0, kBg);
    ui::text(list, fonts_.bold, "Logs", 60, kMenuH + 56, 32, kText);
    ui::text(list, fonts_.regular, "The background service's log, /data/fpps5/fpps5.log", 150, kMenuH + 56, 20, kMuted);
    const Rect box{60, kMenuH + 80, kW - 120, a.h - 110};
    list.rounded_rect(box, 8, kPanel);
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < log_text_.size())
    {
        std::size_t end = log_text_.find('\n', start);
        if (end == std::string::npos)
            end = log_text_.size();
        lines.push_back(log_text_.substr(start, end - start));
        start = end + 1;
    }
    const float lh = 26.0f;
    const float max = std::max(0.0f, lines.size() * lh - (box.h - 30));
    const float scroll = std::min(log_scroll_, max);
    list.push_clip(box);
    float y = box.y + 30 - scroll;
    for (const std::string &l : lines)
    {
        if (y > box.y - lh && y < box.y + box.h + lh)
            ui::text(list, fonts_.regular, fonts_.regular.font->fit(l, 18, box.w - 40), box.x + 20, y, 18,
                     l.find("failed") != std::string::npos || l.find("error") != std::string::npos ? kWarn : kText);
        y += lh;
    }
    list.pop_clip();
    if (lines.empty())
        draw_spinner(list, box.cx(), box.cy(), 20);
}

void App::draw_config(gfx::DrawList &list) const
{
    list.rounded_rect({0, kMenuH, kW, kH - kMenuH - kStatusH}, 0, kBg);
    ui::text(list, fonts_.bold, "Config", 200, kMenuH + 70, 34, kText);
    const char *const sizes[] = {"Small", "Medium", "Large"};
    for (int r = 0; r < kConfigRows; ++r)
    {
        const Rect row = config_row_rect(r);
        const bool f = config_focus_ == r && region_ == kPage;
        const bool hot = pointer_.visible && inside(row, pointer_.x, pointer_.y);
        list.bordered_rect(row, 8, f || hot ? kRowHover : kPanel, f ? 2.0f : 0.0f, kAccentHi);
        ui::text(list, fonts_.regular, kConfigLabels[r], row.x + 24, row.cy() + 8, 22, kText);
        std::string value;
        int toggle = -1;
        float slider = -1.0f;
        switch (r)
        {
        case 0: value = kThemes[std::clamp(settings_.theme, 0, kThemeCount - 1)].name; break;
        case 1: toggle = settings_.intro; break;
        case 2: slider = settings_.music; break;
        case 3: slider = settings_.sounds; break;
        case 4: toggle = settings_.ps5_only; break;
        case 5: toggle = settings_.show_extreme; break;
        case 6: toggle = settings_.web_adult; break;
        case 7: value = sizes[std::clamp(settings_.tile_size, 0, 2)]; break;
        case 8: toggle = settings_.usb_input; break;
        case 9: value = sync_.running ? "Downloading..." : sync_.source == "pc" ? "From a PC copy" : "Update now"; break;
        case 10: value = "Reconnect (service " + backend_.info().version + ")"; break;
        case 11: value = stop_armed_ ? "Press again to stop" : "Stop"; break;
        default: break;
        }
        const float rx = row.x + row.w - 24;
        if (toggle >= 0)
        {
            const Rect sw{rx - 64, row.cy() - 15, 64, 30};
            list.rounded_rect(sw, 15, toggle ? kAccent : kRow);
            list.circle(toggle ? sw.x + sw.w - 15 : sw.x + 15, sw.cy(), 11, toggle ? kWhite : kMuted);
        }
        else if (slider >= 0.0f)
        {
            const Rect bar{rx - 300, row.cy() - 4, 220, 8};
            list.rounded_rect(bar, 4, kLine);
            list.rounded_rect({bar.x, bar.y, bar.w * slider, bar.h}, 4, kAccentHi);
            list.circle(bar.x + bar.w * slider, bar.cy(), 11, kAccentHi); // a ring, seen on light themes too
            list.circle(bar.x + bar.w * slider, bar.cy(), 6, kWhite);
            ui::text(list, fonts_.regular, std::to_string(static_cast<int>(std::round(slider * 100))) + "%", rx,
                     row.cy() + 8, 20, kMuted, Align::right);
        }
        else
        {
            const float vw = fonts_.bold.measure(value, 20) + 32;
            list.bordered_rect({rx - vw, row.cy() - 18, vw, 36}, 6, r == 11 && stop_armed_ ? kRed : kRow, 1.0f, kLine);
            ui::text(list, fonts_.bold, value, rx - vw / 2, row.cy() + 7, 20, kText, Align::center);
        }
    }
}

void App::draw_about(gfx::DrawList &list) const
{
    list.rounded_rect({0, kMenuH, kW, kH - kMenuH - kStatusH}, 0, kBg);
    draw_mark(list, 190, 136, 120, kMark);
    ui::text(list, fonts_.bold, "Flashpoint PS5", 340, 200, 48, kText);
    ui::text(list, fonts_.regular, "App 0.0.6 alpha, service " + backend_.info().version, 342, 240, 22, kMuted);
    const char *const paragraphs[] = {
        "An unofficial PS5 front end for the Flashpoint Archive, the community project that preserves web games and "
        "animations. It is not made or endorsed by Flashpoint Archive.",
        "Flash plays in Ruffle Flash PS5 (Ruffle). HTML5 opens in the PS5's browser. Other platforms have no PS5 "
        "player yet.",
        "Built on the Flashpoint Launcher and Flashpoint Database (MIT), ps5-native-app-boilerplate and "
        "ps5-homebrew-ui by BlackBearReloaded, PayLoader by Marice, offact and Ruffle Flash PS5 (GPL-3.0), stb, "
        "SQLite, miniz and cJSON. Fonts: Atkinson Hyperlegible and Silkscreen (SIL OFL). Music and sounds are original.",
        "Dagobah, PiczoGame, swfchan and Flash Museum lists are read from those sites' own pages when you open "
        "them. Their files belong to their authors. WebP pictures are decoded with libwebp (BSD).",
        "Licensed under the GNU GPL v3 or later. Source: github.com/Richie133/flashpointps5",
    };
    const ServiceInfo &info = backend_.info();
    if (!info.lan_ip.empty())
        ui::text(list, fonts_.regular,
                 "On a phone or PC on the same network: http://" + info.lan_ip + ":" + std::to_string(info.ui_port), 342,
                 276, 21, kAccentHi);
    float y = 330;
    for (const char *p : paragraphs)
        y = ui::paragraph(list, fonts_.regular, p, 190, y, 23, 1400, 34, kText, 5) + 22;
}

void App::draw_link_state(gfx::DrawList &list) const
{
    const Rect a{0, kMenuH, kW, kH - kMenuH - kStatusH};
    list.rounded_rect(a, 0, kBg);
    switch (backend_.link())
    {
    case Link::connecting:
    case Link::starting:
        draw_spinner(list, a.cx(), a.y + a.h * 0.3f - 60, 30);
        draw_message(list, a, "Starting the Flashpoint service",
                     "It runs in the background, keeps the library and downloads games when you play them.");
        break;
    case Link::no_loader:
        draw_message(list, a, "Start your ELF loader first",
                     "The app's background service is a payload, and nothing is listening on port 9021. Start your "
                     "jailbreak's ELF loader (etaHEN, or kstuff with elfldr), then press Cross to try again.");
        break;
    case Link::no_payload:
        draw_message(list, a, "The app is incomplete",
                     "assets/payload/flashpoint-ps5.elf couldn't be read from the app folder. Copy the whole "
                     "PPSA72386 folder from the release zip again.");
        break;
    case Link::failed:
        draw_message(list, a, "The service didn't answer",
                     "It was sent to the ELF loader but never came up. Its log is /data/fpps5/fpps5.log. Press Cross "
                     "to try again.");
        break;
    case Link::ready:
        break;
    }
}

void App::draw_popup(gfx::DrawList &list) const
{
    const Rect p = popup_rect();
    list.shadow(p, 10, 30, Color::rgb(0x000000, 0.6f));
    list.bordered_rect(p, 10, kBar, 1.0f, kLine);
    for (int i = 0; i < kPopupRows; ++i)
    {
        const Rect r = popup_row_rect(i);
        const bool f = popup_focus_ == i;
        const bool hot = pointer_.visible && inside(r, pointer_.x, pointer_.y);
        const bool chosen = i < kSortCount ? settings_.sort == kSorts[i].id : settings_.desc == (i == kSortCount + 1);
        if (f || hot)
            list.rounded_rect(r, 6, f ? kAccent : kRowHover);
        if (chosen)
            draw_icon(list, kIconCheck, r.x + 12, r.cy(), 16, kText);
        ui::text(list, fonts_.regular, i < kSortCount ? kSorts[i].name : (i == kSortCount ? "Ascending" : "Descending"),
                 r.x + 42, r.cy() + 7, 20, kText);
    }
    list.rounded_rect({p.x + 14, popup_row_rect(kSortCount).y - 8, p.w - 28, 1}, 0, kLine);
}

void App::draw_toast(gfx::DrawList &list) const
{
    if (toast_timer_ <= 0.0f || toast_.empty())
        return;
    const float alpha = std::min(1.0f, toast_timer_ * 2.0f);
    const float w = std::min(1300.0f, fonts_.regular.measure(toast_, 22) + 60);
    const Rect r{960 - w / 2, kH - kStatusH - 90, w, 60};
    list.push_opacity(alpha);
    list.shadow(r, 10, 26, Color::rgb(0x000000, 0.5f));
    list.bordered_rect(r, 10, kBar, 1.0f, toast_error_ ? kWarn : kAccentHi);
    ui::text(list, fonts_.regular, fonts_.regular.font->fit(toast_, 22, w - 40), r.cx(), r.cy() + 8, 22, kText,
             Align::center);
    list.pop_opacity();
}

void App::draw_pointer(gfx::DrawList &list) const
{
    if (!pointer_.visible)
        return;
    const float x = pointer_.x, y = pointer_.y;
    const float outline[] = {x - 1.5f, y - 2.5f, x - 1.5f, y + 27, x + 6.5f, y + 20.5f, x + 12.5f, y + 33,
                             x + 19.5f, y + 30, x + 13.5f, y + 18.5f, x + 23.5f, y + 18};
    list.polygon(outline, 7, Color::rgb(0x000000, 0.85f));
    const float fill[] = {x + 1, y + 2.5f, x + 1, y + 22, x + 7.2f, y + 16.4f, x + 13.5f, y + 29,
                          x + 16.2f, y + 27.8f, x + 10, y + 15.3f, x + 17.6f, y + 15.3f};
    list.polygon(fill, 7, kWhite);
}

} // namespace hui::fp
