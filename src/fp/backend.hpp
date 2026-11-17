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

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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
    int offset = 0;
    int limit = 21;

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
