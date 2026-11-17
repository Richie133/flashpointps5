// Flashpoint PS5 - the app's link to its background service.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fp/backend.hpp"

#include "core/save_file.hpp"
#include "fp/http.hpp"
#include "platform/ps5/system.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace hui::fp
{

namespace
{

const std::string &str(const JsonValue &value, std::string_view key)
{
    return value[key].as_string();
}

long long num(const JsonValue &value, std::string_view key)
{
    return static_cast<long long>(value[key].as_number(0.0));
}

bool flag(const JsonValue &value, std::string_view key)
{
    const JsonValue &v = value[key];
    if (v.is_bool())
        return v.as_bool();
    if (v.is_number())
        return v.as_number() != 0.0;
    return v.is_string() && v.as_string() == "true";
}

std::string joined(const JsonValue &value, std::string_view key)
{
    const JsonValue &v = value[key];
    if (v.is_string())
        return v.as_string();
    std::string out;
    for (const JsonValue &item : v.items())
    {
        if (!out.empty())
            out += ", ";
        out += item.as_string();
    }
    return out;
}

std::vector<Job> parse_jobs(const JsonValue &array)
{
    std::vector<Job> jobs;
    for (const JsonValue &item : array.items())
    {
        Job job;
        job.game_id = str(item, "gameId");
        job.title = str(item, "title");
        job.state = str(item, "state");
        job.done = num(item, "done");
        job.total = num(item, "total");
        job.error = str(item, "error");
        jobs.push_back(std::move(job));
    }
    return jobs;
}

} // namespace

bool push_to_loader(int port, const std::string &data)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    timeval timeout{};
    timeout.tv_sec = 5;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool sent = connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0;
    std::size_t done = 0;
    while (sent && done < data.size())
    {
        const ssize_t n = ::send(fd, data.data() + done, data.size() - done, 0);
        if (n <= 0)
            sent = false;
        else
            done += static_cast<std::size_t>(n);
    }
    close(fd);
    return sent;
}

Backend::Backend(std::string payload_path, int service_port)
    : payload_path_(std::move(payload_path)), port_(service_port)
{
}

Backend::~Backend() = default;

Link Backend::link() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return link_;
}

void Backend::set_link(Link link)
{
    std::lock_guard<std::mutex> lock(mutex_);
    link_ = link;
}

void Backend::post(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lock(mutex_);
    done_.push_back(std::move(fn));
}

void Backend::pump()
{
    std::vector<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready.swap(done_);
    }
    for (auto &fn : ready)
        fn();
}

bool Backend::fetch_json(const std::string &method, const std::string &target, JsonValue *out,
                         long long * /*total*/, int timeout_ms) const
{
    auto transport = make_loopback_transport(port_);
    Request request;
    request.method = method;
    request.target = target;
    request.timeout_ms = timeout_ms;
    const Reply reply = transport->send(request);
    if (reply.status < 200 || reply.status >= 500)
        return false;
    return parse_json(reply.body, *out);
}

bool Backend::get_bytes(const std::string &target, std::string *body, int timeout_ms) const
{
    auto transport = make_loopback_transport(port_);
    Request request;
    request.method = "GET";
    request.target = target;
    request.timeout_ms = timeout_ms;
    Reply reply = transport->send(request);
    if (reply.status != 200)
        return false;
    *body = std::move(reply.body);
    return true;
}

void Backend::connect()
{
    set_link(Link::connecting);
    worker_.queue([this] { connect_job(); });
}

void Backend::connect_job()
{
    JsonValue cfg;
    bool up = fetch_json("GET", "/api/config", &cfg, nullptr, 1500);
    if (!up)
    {
        std::string elf;
        if (!save::read_file(payload_path_, &elf) || elf.size() < 64)
        {
            sys::log("[FP] service ELF missing: %s", payload_path_.c_str());
            set_link(Link::no_payload);
            return;
        }
        if (!push_to_loader(kLoaderPort, elf))
        {
            sys::log("[FP] no ELF loader on port %d", kLoaderPort);
            set_link(Link::no_loader);
            return;
        }
        sys::log("[FP] service sent to the ELF loader (%zu bytes)", elf.size());
        set_link(Link::starting);
        for (int i = 0; i < 40 && !up; ++i)
        {
            sys::sleep_us(500000);
            up = fetch_json("GET", "/api/config", &cfg, nullptr, 1000);
        }
        if (!up)
        {
            set_link(Link::failed);
            return;
        }
    }
    ServiceInfo info;
    info.version = str(cfg, "version");
    info.flashpoint_path = str(cfg, "flashpointPath");
    info.run_path = str(cfg, "runPath");
    info.lan_ip = str(cfg, "lanIp");
    info.ruffle_title = str(cfg, "ruffleTitleId");
    info.db_open = flag(cfg, "dbOpen");
    info.game_count = num(cfg, "gameCount");
    info.ruffle_direct = flag(cfg, "ruffleDirect");
    info.ui_port = static_cast<int>(num(cfg, "uiPort"));
    post([this, info] {
        info_ = info;
        set_link(Link::ready);
    });
}

void Backend::refresh_info(std::function<void()> done)
{
    worker_.queue([this, done = std::move(done)] {
        connect_job();
        post(done);
    });
}

void Backend::search(const SearchQuery &query, std::function<void(SearchResult)> done)
{
    std::string target = "/api/search?fields=id,title,developer,publisher,platform,library,installed";
    if (!query.text.empty())
        target += "&smartSearch=" + url_encode(query.text);
    if (!query.library.empty())
        target += "&library=" + url_encode(query.library);
    if (query.ps5_only)
        target += "&ps5=true";
    if (query.played)
        target += "&played=true&sort=lastPlayed&dir=desc";
    if (query.installed)
        target += "&installed=true";
    target += "&filter=true&envelope=1&limit=" + std::to_string(query.limit) +
              "&offset=" + std::to_string(query.offset);
    worker_.queue([this, target, done = std::move(done)] {
        SearchResult result;
        JsonValue root;
        if (fetch_json("GET", target, &root, nullptr, 15000) && root["results"].is_array())
        {
            result.ok = true;
            result.total = num(root, "total");
            for (const JsonValue &item : root["results"].items())
            {
                Game g;
                g.id = str(item, "id");
                g.title = str(item, "title");
                g.developer = str(item, "developer");
                if (g.developer.empty())
                    g.developer = str(item, "publisher");
                g.platform = str(item, "platform");
                g.library = str(item, "library");
                g.installed = flag(item, "installed");
                result.games.push_back(std::move(g));
            }
        }
        post([done, result] { done(result); });
    });
}

void Backend::game(const std::string &id, std::function<void(GameDetail)> done)
{
    worker_.queue([this, id, done = std::move(done)] {
        GameDetail d;
        JsonValue root;
        if (fetch_json("GET", "/api/game?id=" + url_encode(id), &root) && root["game"].is_object())
        {
            const JsonValue &g = root["game"];
            d.ok = true;
            d.id = str(g, "id");
            d.title = str(g, "title");
            d.developer = str(g, "developer");
            d.publisher = str(g, "publisher");
            d.platform = str(g, "platform");
            d.library = str(g, "library");
            d.description = str(g, "originalDescription");
            d.notes = str(g, "notes");
            d.tags = joined(g, "tags");
            d.release_date = str(g, "releaseDate");
            d.series = str(g, "series");
            d.installed = flag(g, "installed");
            d.zipped = flag(g, "zipped");
            for (const JsonValue &data : g["gameData"].items())
            {
                if (flag(data, "active") || d.size == 0)
                    d.size = num(data, "size");
            }
            for (const JsonValue &a : g["addApps"].items())
            {
                AddApp app;
                app.id = str(a, "id");
                app.name = str(a, "name");
                app.application_path = str(a, "applicationPath");
                if (app.application_path != ":extras:")
                    d.add_apps.push_back(std::move(app));
            }
            const JsonValue &p = root["play"];
            d.kind = str(p, "kind");
            d.playable = flag(p, "playable");
            d.reason = str(p, "reason");
            d.ruffle_mode = str(p, "ruffleMode");
        }
        post([done, d] { done(d); });
    });
}

void Backend::play(const std::string &id, const std::string &add_app,
                   std::function<void(PlayReply)> done)
{
    std::string target = "/api/play?id=" + url_encode(id);
    if (!add_app.empty())
        target += "&addApp=" + url_encode(add_app);
    poll_worker_.queue([this, target, done = std::move(done)] {
        PlayReply r;
        JsonValue root;
        if (!fetch_json("POST", target, &root, nullptr, 20000))
        {
            r.status = "offline";
            r.message = "The Flashpoint service didn't answer.";
        }
        else
        {
            r.status = str(root, "status");
            r.message = str(root, "message");
            r.url = str(root, "url");
            r.jobs = parse_jobs(root["jobs"]);
        }
        post([done, r] { done(r); });
    });
}

void Backend::jobs(std::function<void(std::vector<Job>)> done)
{
    poll_worker_.queue([this, done = std::move(done)] {
        JsonValue root;
        std::vector<Job> jobs;
        if (fetch_json("GET", "/api/jobs", &root, nullptr, 2000))
            jobs = parse_jobs(root);
        post([done, jobs] { done(jobs); });
    });
}

void Backend::stop_service(std::function<void(bool)> done)
{
    worker_.queue([this, done = std::move(done)] {
        JsonValue root;
        const bool ok = fetch_json("GET", "/api/quit", &root, nullptr, 2000);
        if (ok)
            set_link(Link::connecting);
        post([done, ok] { done(ok); });
    });
}

} // namespace hui::fp
