#ifndef SIGAW_AVATAR_CACHE_H
#define SIGAW_AVATAR_CACHE_H

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <errno.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <curl/curl.h>

#include "../common/config.h"

namespace sigaw {

class AvatarCache {
public:
    AvatarCache() {
        std::error_code ec;
        root_ = Config::avatar_cache_dir();
        std::filesystem::create_directories(root_, ec);
        worker_ = std::thread([this]() { run(); });
    }

    ~AvatarCache() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    AvatarCache(const AvatarCache&) = delete;
    AvatarCache& operator=(const AvatarCache&) = delete;

    void request(uint64_t user_id, const std::string& avatar_hash) {
        if (avatar_hash.empty()) {
            return;
        }

        const auto path = Config::avatar_cache_path(user_id, avatar_hash);
        if (path.empty()) {
            return;
        }
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            return;
        }

        const auto key = cache_key(user_id, avatar_hash);
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!queued_.insert(key).second) {
                return;
            }
            jobs_.push_back(Job{user_id, avatar_hash, path});
        }
        cv_.notify_one();
    }

private:
    struct Job {
        uint64_t              user_id;
        std::string           avatar_hash;
        std::filesystem::path target_path;
    };

    static std::string cache_key(uint64_t user_id, const std::string& avatar_hash) {
        return std::to_string(static_cast<unsigned long long>(user_id)) + ":" + avatar_hash;
    }

    static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* data = static_cast<std::vector<uint8_t>*>(userdata);
        const size_t bytes = size * nmemb;
        data->insert(data->end(), reinterpret_cast<uint8_t*>(ptr),
                     reinterpret_cast<uint8_t*>(ptr) + bytes);
        return bytes;
    }

    void run() {
        while (true) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [&]() { return stop_ || !jobs_.empty(); });
                if (stop_ && jobs_.empty()) {
                    return;
                }
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }

            download(job);

            std::lock_guard<std::mutex> lock(mu_);
            queued_.erase(cache_key(job.user_id, job.avatar_hash));
        }
    }

    void download(const Job& job) {
        std::error_code ec;
        std::filesystem::create_directories(job.target_path.parent_path(), ec);
        if (ec) {
            fprintf(stderr, "[sigaw] avatar: mkdir failed: %s\n", ec.message().c_str());
            return;
        }

        std::vector<uint8_t> bytes;
        const std::string url =
            "https://cdn.discordapp.com/avatars/" +
            std::to_string(static_cast<unsigned long long>(job.user_id)) + "/" +
            job.avatar_hash + ".png?size=64";
        fprintf(stderr, "[sigaw] avatar: downloading %s -> %s\n",
                url.c_str(), job.target_path.string().c_str());

        CURL* curl = curl_easy_init();
        if (!curl) {
            return;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bytes);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "sigaw/0.1");

        const CURLcode res = curl_easy_perform(curl);
        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK || response_code != 200 || bytes.empty()) {
            fprintf(stderr, "[sigaw] avatar: download failed res=%d http=%ld size=%zu\n",
                    (int)res, response_code, bytes.size());
            return;
        }
        fprintf(stderr, "[sigaw] avatar: downloaded %zu bytes\n", bytes.size());

        const auto tmp = job.target_path.string() + ".part";
        {
            std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
            if (!file.is_open()) {
                fprintf(stderr, "[sigaw] avatar: open failed %s: %s\n",
                        tmp.c_str(), std::strerror(errno));
                return;
            }
            file.write(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            if (!file.good()) {
                fprintf(stderr, "[sigaw] avatar: write failed %s\n", tmp.c_str());
                return;
            }
        }

        std::filesystem::rename(tmp, job.target_path, ec);
        if (ec) {
            fprintf(stderr, "[sigaw] avatar: rename failed %s -> %s: %s\n",
                    tmp.c_str(), job.target_path.string().c_str(), ec.message().c_str());
            std::filesystem::remove(tmp, ec);
            return;
        }

        std::filesystem::permissions(
            job.target_path,
            std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace,
            ec
        );
    }

    std::filesystem::path        root_;
    std::mutex                   mu_;
    std::condition_variable      cv_;
    std::deque<Job>              jobs_;
    std::unordered_set<std::string> queued_;
    std::thread                  worker_;
    bool                         stop_ = false;
};

} /* namespace sigaw */

#endif /* SIGAW_AVATAR_CACHE_H */
