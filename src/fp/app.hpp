// Flashpoint PS5 - the app's screens, laid out like the Flashpoint Launcher:
// a menu bar (Home, Arcade, Theatre, Web, Logs, Config, About) with search
// and sorting, a sidebar of filters, a grid of tiles, the selected entry's
// details beside it, and a status bar. Controller, touchpad pointer, USB
// mouse and keyboard all drive the same focus.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/input.hpp"
#include "fp/backend.hpp"
#include "fp/hid.hpp"
#include "fp/images.hpp"
#include "fp/settings.hpp"
#include "gfx/draw_list.hpp"
#include "ui/fonts.hpp"

#include <atomic>
#include <cstdint>
#include <array>
#include <ctime>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hui::fp
{

struct Fonts
{
    ui::FontRef regular;
    ui::FontRef bold;
    ui::FontRef pixel;
};

class App
{
  public:
    App(const Fonts &fonts, Backend &backend, Images &images, std::string settings_path);

    void update(const InputFrame &input, const PadSample &pad, const hid::Frame &hid, float dt);
    void draw(gfx::DrawList &list) const;
    bool idle() const;

    // USB keyboard and mouse start once, guarded: begin_usb() says whether
    // to start them (and notes the attempt), end_usb() that it returned.
    bool begin_usb();
    void end_usb(bool any_device);

    enum Tab
    {
        kHome,
        kArcade,
        kTheatre,
        kWeb,
        kLogs,
        kConfig,
        kAbout,
        kTabCount
    };
    void open_tab(int tab); // for the PC snapshot tour

  private:
    enum Region
    {
        kMenu,
        kSearch,
        kSort,
        kSidebar,
        kGrid,
        kDetail,
        kPage,
    };

    struct SideRow
    {
        std::string label;
        std::string platform; // platform filter rows
        int kind = 0;         // 0 all, 1 PS5, 2 played, 3 downloaded, 4 platform, 5 heading, 6 web list, 7 SWF link
        long long count = -1;
        std::string source, list; // web rows
    };

    struct Browse
    {
        std::string library;
        int filter = 1;
        std::string platform; // the platform row's name (rows move when counts change)
        long long total = -1;
        std::map<int, std::vector<Game>> pages;
        std::set<int> loading;
        std::uint64_t token = 0;
        // token, for the request thread: searches queued for an older one are dropped
        std::shared_ptr<std::atomic<std::uint64_t>> live = std::make_shared<std::atomic<std::uint64_t>>(0);
        int focus = 0;
        float scroll = 0.0f, scroll_to = 0.0f;
        bool failed = false;
    };

    struct WebState
    {
        std::vector<WebSource> sources;
        bool requested = false;
        int filter = 2; // the first site's first list
        std::string source, list;
        std::vector<WebItem> items;
        int page = 0;
        bool more = false, loading = false;
        std::string error;
        int hidden = 0;
        int empty_run = 0; // pages in a row that added nothing
        std::uint64_t token = 0;
        int focus = 0;
        float scroll = 0.0f, scroll_to = 0.0f;
    };

    struct Detail
    {
        bool web = false;
        std::string id; // library entry
        WebItem item;   // web entry
        std::string web_source;
        GameDetail d;
        bool loading = false;
        float wait = 0.0f;
        std::uint64_t token = 0;
        int focus = 0; // 0 play, then additional apps
        bool playing = false;
        std::string play_add_app;
        std::string status;
        bool status_error = false;
        double progress = -1.0;
        float poll = 0.0f;
    };

    struct Tile
    {
        std::string title, subtitle, image, platform;
        bool installed = false, loaded = false;
    };

    // input
    void handle_pointer(const PadSample &pad, const hid::Frame &hid, float dt);
    void handle_keyboard(const hid::Frame &hid, InputFrame &input);
    void click(float x, float y, bool twice);
    void scroll_at(float x, float y, float pixels);
    void hover(float x, float y);

    // navigation
    void update_main(const InputFrame &input, float dt);
    void nav_menu(const InputFrame &input);
    void nav_sidebar(const InputFrame &input);
    void nav_grid(const InputFrame &input);
    void nav_detail(const InputFrame &input);
    void nav_page(const InputFrame &input);
    void nav_popup(const InputFrame &input);
    void switch_tab(int tab);
    void activate_sidebar(int row);
    void move_focus(int region);

    // data
    std::vector<SideRow> sidebar_rows() const;
    const std::vector<std::pair<std::string, long long>> &platforms_for(const Browse &b) const;
    int sidebar_focus() const;
    void set_sidebar_focus(int row);
    Browse *browse();
    const Browse *browse() const;
    SearchQuery query_for(const Browse &b) const;
    static void bump(Browse &b); // a new token: what's loading or queued for b is out of date
    std::uint64_t random_seed_ = static_cast<std::uint64_t>(std::time(nullptr)); // the Random sort's order
    void reload_browse(Browse &b);
    void refresh_browse(Browse &b); // new data, same focus and scroll
    void reload_all();
    void ensure_pages(Browse &b);
    void load_web(bool next_page);
    int grid_count() const;
    int grid_focus() const;
    void set_grid_focus(int index, bool sound);
    Tile tile_at(int index) const;
    void select_for_detail();
    void load_detail();
    void play();
    void handle_play(const PlayReply &reply, std::uint64_t token);
    void open_keyboard(int purpose);
    void apply_search(const std::string &text);
    void start_swf_link(const std::string &url);
    void set_sort(int option);
    void config_change(int row, int delta);
    void home_activate(int item);
    void poll_sync(float dt);
    void refresh_stats();
    void toast(const std::string &message, bool error = false);
    void sound(int sfx) const;
    void save_settings();

    // geometry (shared by drawing and pointer hit tests)
    gfx::Rect menu_item_rect(int tab) const;
    gfx::Rect search_rect() const;
    gfx::Rect sort_rect() const;
    gfx::Rect dir_rect() const;
    gfx::Rect sidebar_area() const;
    gfx::Rect sidebar_row_rect(int row) const;
    gfx::Rect grid_area() const;
    gfx::Rect detail_area() const;
    float tile_w() const;
    float tile_h() const;
    int grid_cols() const;
    gfx::Rect tile_rect(int index) const;
    gfx::Rect play_button_rect() const;
    gfx::Rect add_app_rect(int i) const;
    gfx::Rect popup_rect() const;
    gfx::Rect popup_row_rect(int i) const;
    gfx::Rect config_row_rect(int row) const;
    gfx::Rect home_item_rect(int item) const;
    int home_item_count() const;
    float grid_max_scroll() const;
    void keep_focus_visible();
    float *grid_scroll();

    // drawing
    void draw_intro(gfx::DrawList &list) const;
    void draw_menu(gfx::DrawList &list) const;
    void draw_sidebar(gfx::DrawList &list) const;
    void draw_grid(gfx::DrawList &list) const;
    void draw_detail(gfx::DrawList &list) const;
    void draw_status(gfx::DrawList &list) const;
    void draw_home(gfx::DrawList &list) const;
    void draw_logs(gfx::DrawList &list) const;
    void draw_config(gfx::DrawList &list) const;
    void draw_about(gfx::DrawList &list) const;
    void draw_link_state(gfx::DrawList &list) const;
    void draw_popup(gfx::DrawList &list) const;
    void draw_toast(gfx::DrawList &list) const;
    void draw_pointer(gfx::DrawList &list) const;
    void draw_image(gfx::DrawList &list, const std::string &rel, const gfx::Rect &r, float radius) const;
    void draw_badge(gfx::DrawList &list, const std::string &platform, float cx, float cy, float r) const;
    void draw_icon(gfx::DrawList &list, int icon, float x, float cy, float size, gfx::Color color) const;
    float draw_hint(gfx::DrawList &list, const char *glyph, const std::string &label, float x, float cy) const;
    void draw_glyph(gfx::DrawList &list, const char *glyph, float x, float cy) const;
    void draw_message(gfx::DrawList &list, const gfx::Rect &area, const std::string &title,
                      const std::string &body) const;
    void draw_spinner(gfx::DrawList &list, float cx, float cy, float r) const;

    const Fonts &fonts_;
    Backend &backend_;
    Images &images_;
    std::string settings_path_;
    Settings settings_;

    float clock_ = 0.0f;
    bool intro_ = true;
    float intro_t_ = 0.0f;
    float intro_fade_ = -1.0f; // seconds into the closing fade, or < 0

    int tab_ = kHome;
    int region_ = kPage;
    Browse browse_[2];
    WebState web_;
    Detail detail_;
    // The app's own keyboard, for when the system keyboard can't open.
    struct Osk
    {
        bool open = false;
        int purpose = 0; // as keyboard_: 1 search, 2 .swf address
        std::string title, text;
        int row = 1, col = 0;
        bool caps = false, symbols = false;
    };
    Osk osk_;
    void open_osk(int purpose, const std::string &title, const std::string &text);
    void close_osk(bool accept);
    void update_osk(const InputFrame &input);
    void osk_press(int row, int col);
    void osk_click(float x, float y);
    gfx::Rect osk_panel() const;
    gfx::Rect osk_key_rect(int row, int col) const;
    std::string osk_key_label(int row, int col) const;
    void draw_osk(gfx::DrawList &list) const;

    float side_scroll_ = 0.0f, side_scroll_to_ = 0.0f; // the sidebar, when its rows don't fit
    void side_follow(int row);                         // scroll the sidebar to show a row
    float side_max_scroll() const;
    std::string search_;                            // the open tab's search box
    std::array<std::string, kTabCount> tab_search_; // each tab keeps its own
    float search_wait_ = -1.0f;
    int home_focus_ = 0;
    int config_focus_ = 0;
    bool stop_armed_ = false;
    float log_scroll_ = 0.0f;
    std::string log_text_;
    bool popup_ = false;
    int popup_focus_ = 0;
    int keyboard_ = 0; // 0 closed, 1 search, 2 SWF link
    struct
    {
        float x = 960.0f, y = 540.0f;
        bool visible = false;
        float idle = 0.0f;
        int touch_id = -1;
        float touch_x = 0, touch_y = 0;
        float two_y = 0;
        bool two = false;
        float last_click = -10.0f;
        int last_click_index = -1;
        int hover_region = -1, hover_index = -1;
    } pointer_;
    bool usb_keyboard_ = false;
    bool touch_was_down_ = false;

    Stats stats_;
    SyncStatus sync_;
    float sync_poll_ = 0.0f;
    long long stats_entries_ = 0; // library size at the last stats refresh
    bool sync_started_ = false;
    std::vector<Game> recent_;
    std::vector<Job> jobs_;
    float jobs_timer_ = 0.0f;
    Link last_link_ = Link::connecting;
    std::string toast_;
    bool toast_error_ = false;
    float toast_timer_ = 0.0f;
};

} // namespace hui::fp
