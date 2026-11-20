// Flashpoint PS5 - logos and screenshots as GL textures.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fp/images.hpp"

extern "C"
{
#include "third_party/stb/image.h"
}

#include <GL/glcorearb.h>

#include <algorithm>
#include <utility>

namespace hui::fp
{

namespace
{
constexpr std::size_t kMaxTextures = 160;
constexpr int kUploadsPerFrame = 4;
constexpr int kMaxSide = 1024; // larger pictures are halved until they fit

void shrink(std::vector<unsigned char> &rgba, int &w, int &h)
{
    while (w > kMaxSide || h > kMaxSide)
    {
        const int nw = std::max(1, w / 2);
        const int nh = std::max(1, h / 2);
        std::vector<unsigned char> out(static_cast<std::size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y)
        {
            for (int x = 0; x < nw; ++x)
            {
                for (int c = 0; c < 4; ++c)
                {
                    const auto at = [&](int xx, int yy) {
                        xx = std::min(xx, w - 1);
                        yy = std::min(yy, h - 1);
                        return static_cast<int>(rgba[(static_cast<std::size_t>(yy) * w + xx) * 4 + c]);
                    };
                    const int sum = at(x * 2, y * 2) + at(x * 2 + 1, y * 2) + at(x * 2, y * 2 + 1) +
                                    at(x * 2 + 1, y * 2 + 1);
                    out[(static_cast<std::size_t>(y) * nw + x) * 4 + c] = static_cast<unsigned char>(sum / 4);
                }
            }
        }
        rgba.swap(out);
        w = nw;
        h = nh;
    }
}
} // namespace

Images::Images(const Backend &backend) : backend_(backend)
{
}

Images::~Images()
{
    for (auto &[rel, entry] : entries_)
    {
        if (entry.texture.id != 0)
        {
            GLuint id = entry.texture.id;
            glDeleteTextures(1, &id);
        }
    }
}

Texture Images::get(const std::string &rel)
{
    Entry &entry = entries_[rel];
    entry.used = ++clock_;
    if (entry.failed && std::chrono::steady_clock::now() >= entry.retry_at)
        entry.failed = false; // a slow site or a busy moment shouldn't lose the picture for good
    if (entry.texture.id != 0 || entry.loading || entry.failed)
        return entry.texture;
    entry.loading = true;
    const std::uint64_t generation = generation_.load();
    workers_[next_worker_].queue([this, rel, generation] {
        if (generation != generation_.load())
            return; // cancelled while waiting
        Decoded decoded;
        decoded.rel = rel;
        std::string bytes;
        // the service may have to fetch it from the internet first
        if (backend_.get_bytes(rel.starts_with("/") ? rel : "/images/" + rel, &bytes, 20000))
        {
            int channels = 0;
            unsigned char *pixels = stbi_load_from_memory(
                reinterpret_cast<const unsigned char *>(bytes.data()), static_cast<int>(bytes.size()),
                &decoded.width, &decoded.height, &channels, 4);
            if (pixels != nullptr)
            {
                decoded.rgba.assign(pixels, pixels + static_cast<std::size_t>(decoded.width) *
                                                         decoded.height * 4);
                stbi_image_free(pixels);
                shrink(decoded.rgba, decoded.width, decoded.height);
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        decoded_.push_back(std::move(decoded));
    });
    next_worker_ = (next_worker_ + 1) % 2;
    return entry.texture;
}

void Images::cancel_pending()
{
    // Jobs already queued see their generation gone and skip the download;
    // their entries are marked so the next get() asks again.
    generation_.fetch_add(1);
    for (auto &[rel, entry] : entries_)
    {
        if (entry.loading)
            entry.loading = false;
    }
}

void Images::pump()
{
    std::vector<Decoded> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t n = std::min<std::size_t>(decoded_.size(), kUploadsPerFrame);
        ready.assign(std::make_move_iterator(decoded_.begin()),
                     std::make_move_iterator(decoded_.begin() + static_cast<std::ptrdiff_t>(n)));
        decoded_.erase(decoded_.begin(), decoded_.begin() + static_cast<std::ptrdiff_t>(n));
    }
    for (Decoded &d : ready)
    {
        Entry &entry = entries_[d.rel];
        entry.loading = false;
        if (d.rgba.empty())
        {
            // try again later, waiting longer each time (4 s, 8 s ... 5 min)
            entry.failed = true;
            entry.attempts = std::min(entry.attempts + 1, 7);
            entry.retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(4 << (entry.attempts - 1));
            if (entry.attempts == 7)
                entry.retry_at = std::chrono::steady_clock::now() + std::chrono::minutes(5);
            continue;
        }
        if (entry.texture.id != 0)
            continue; // a second download of the same picture
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, d.width, d.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     d.rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        entry.texture = {texture, d.width, d.height};
    }

    if (entries_.size() > kMaxTextures)
    {
        std::vector<std::pair<std::uint64_t, std::string>> order;
        order.reserve(entries_.size());
        for (const auto &[rel, entry] : entries_)
        {
            if (!entry.loading)
                order.emplace_back(entry.used, rel);
        }
        std::sort(order.begin(), order.end());
        std::size_t drop = entries_.size() - kMaxTextures;
        for (std::size_t i = 0; i < order.size() && drop > 0; ++i, --drop)
        {
            auto it = entries_.find(order[i].second);
            if (it->second.texture.id != 0)
            {
                GLuint id = it->second.texture.id;
                glDeleteTextures(1, &id);
            }
            entries_.erase(it);
        }
    }
}

} // namespace hui::fp
