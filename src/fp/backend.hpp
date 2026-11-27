// Flashpoint PS5 - the app's link to its background service.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The service (payload/, flashpoint-ps5.elf) runs outside the app's sandbox:
// it reads the Flashpoint folder on /data or USB, downloads game data and
// hands games to Ruffle Flash. The app starts it through the ELF loader on
// port 9021 when it isn't running yet, then asks it everything over HTTP on
// 127.0.0.1:22600. Requests run on worker threads; their answers come back on
// the main thread through pump().

#pragma once

#include "fp/json.hpp"
#include "fp/worker.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <map>
#include <string>
#include <vector>

namespace hui::fp
{

constexpr int kServicePort = 22600;
constexpr int kLoaderPort = 9021;

struct Game
{
    std::string id;
    std::string title;
    std::string developer;
    std::string platform;
    std::string library;
    bool installed = false;
};

struct SearchQuery
{
    std::string text;      // smartSearch
    std::string library;   // "", "arcade", "theatre"
    bool ps5_only = true;  // Flash and HTML5 only
    bool played = false;   // recently played, newest first
    bool installed = false;
    std::string platform;  // "", "Flash", "HTML5", ...
    std::string sort = "title"; // title, dateAdded, developer, releaseDate, lastPlayed, random
    bool desc = false;
    bool show_extreme = false;
    int offset = 0;
    int limit = 21;
    std::uint64_t seed = 0; // sort "random": the same seed gives the same order on every page
    // Dropped before it reaches the service when *live has moved on from
    // token (the list it was for was reloaded while it waited its turn).
    std::shared_ptr<const std::atomic<std::uint64_t>> live;
    std::uint64_t token = 0;

    bool operator==(const SearchQuery &) const = default;
};

struct SearchResult
{
    bool ok = false;
    long long total = 0;
    std::vector<Game> games;
};

struct AddApp
{
    std::string id;
    std::string name;
    std::string application_path;
};

struct GameDetail
{
    bool ok = false;
    std::string id;
    std::string title;
    std::string developer;
    std::string publisher;
    std::string platform;
    std::string library;
    std::string description;
    std::string notes;
    std::string tags;
    std::string release_date;
    std::string series;
    std::string play_mode;
    std::string status;
    std::string source;
    std::string date_added;
    std::string language;
    std::string alternate_titles;
    std::string last_played;
    bool installed = false;
    bool zipped = false;
    long long size = 0;
    std::vector<AddApp> add_apps;
    // how it plays on the PS5
    std::string kind; // flash, html5, message, extras, unsupported
    bool playable = false;
    std::string reason;
    std::string ruffle_mode; // direct, export
};

struct Job
{
    std::string game_id;
    std::string title;
    std::string state; // queued, downloading, verifying, extracting, done, error
    long long done = 0;
    long long total = 0;
    std::string error;
};

struct PlayReply
{
    std::string status; // pending, launched, open, message, unsupported, error, offline
    std::string message;
    std::string url;
    std::vector<Job> jobs;
};

struct ServiceInfo
{
    std::string version;
    std::string flashpoint_path;
    std::string run_path;
    std::string lan_ip;
    std::string ruffle_title;
    bool db_open = false;
    long long game_count = 0;
    bool ruffle_direct = false;
    int ui_port = kServicePort;
};

struct Stats
{
    bool ok = false;
    long long arcade = 0, theatre = 0;
    std::vector<std::pair<std::string, long long>> platforms; // most entries first
    std::map<std::string, std::vector<std::pair<std::string, long long>>> library_platforms; // the same, per library
};

struct SyncStatus
{
    bool ok = false;
    std::string state; // idle, running, done, error
    bool running = false;
    long long done = 0, total = 0;
    std::string message;
    std::string source; // fpfss (downloaded here), pc (copied from a PC), "" (none)
    long long synced_at = 0;
    long long entries = 0;
};

struct WebList
{
    std::string id, name;
};

struct WebSource
{
    std::string id, name, home;
    bool search = false;
    std::vector<WebList> lists;
};

struct WebItem
{
    std::string title, page, swf, thumb;
    bool adult = false;
};

struct WebPage
{
    bool ok = false;
    std::vector<WebItem> items;
    bool more = false;
    int next = 0; // the page to ask for next (searches may read several at once)
    int hidden = 0;
    std::string error;
};

enum class Link : std::uint8_t
{
    connecting, // asking the service
    starting,   // sent it to the ELF loader, waiting for it to answer
    ready,
    no_loader,  // nothing listens on port 9021
    no_payload, // the app folder lacks assets/payload/flashpoint-ps5.elf
    failed,     // started, but it never answered
};

class Backend
{
  public:
    // payload_path: the service ELF inside the app folder.
    Backend(std::string payload_path, int service_port = kServicePort);
    ~Backend();
    Backend(const Backend &) = delete;
    Backend &operator=(const Backend &) = delete;

    // Connects, starting the service when needed. Call again to retry.
    void connect();
    Link link() const;
    const ServiceInfo &info() const
    {
        return info_;
    }

    void search(const SearchQuery &query, std::function<void(SearchResult)> done);
    void game(const std::string &id, std::function<void(GameDetail)> done);
    void play(const std::string &id, const std::string &add_app, std::function<void(PlayReply)> done);
    void jobs(std::function<void(std::vector<Job>)> done);
    void refresh_info(std::function<void()> done);
    void stop_service(std::function<void(bool)> done);
    void stats(std::function<void(Stats)> done);
    // start: begin (or continue) downloading the library first
    void sync(bool start, std::function<void(SyncStatus)> done);
    void web_sources(std::function<void(std::vector<WebSource>)> done);
    void web_list(const std::string &source, const std::string &list, const std::string &query, int page,
                  bool adult, std::function<void(WebPage)> done);
    // Finds, downloads and starts a .swf (from a site or any link); poll like play().
    void web_play(const std::string &url, const std::string &title, const std::string &source,
                  std::function<void(PlayReply)> done);
    void log(std::function<void(std::string)> done);
    // Raw GET for images (worker of the caller's choice runs it).
    bool get_bytes(const std::string &target, std::string *body, int timeout_ms = 8000) const;

    // Runs the answers that arrived since the last call (main thread).
    void pump();

  private:
    void post(std::function<void()> fn);
    bool fetch_json(const std::string &method, const std::string &target, JsonValue *out,
                    long long *total = nullptr, int timeout_ms = 6000) const;
    void connect_job();

    std::string payload_path_;
    int port_;
    JobWorker worker_;      // API requests, in order
    JobWorker poll_worker_; // play polling and job status, so a slow search can't stall them
    mutable std::mutex mutex_;
    std::vector<std::function<void()>> done_;
    void set_link(Link link);

    Link link_ = Link::connecting; // guarded by mutex_
    ServiceInfo info_;             // main thread only
};

// Sends an ELF to the loader listening on 127.0.0.1:port. False when nothing listens.
bool push_to_loader(int port, const std::string &data);

} // namespace hui::fp
