module;

#include <curl/curl.h>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

export module sage.vendor.curl;

import std;

export namespace sage::vendor::curl {

using std::size_t;

class CurlGlobal {
public:
    static void init() {
        static CurlGlobal instance;
        (void)instance;
    }
private:
    CurlGlobal() { curl_global_init(CURL_GLOBAL_ALL); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

class CurlEasy {
public:
    CurlEasy() : handle_(curl_easy_init()) {
        CurlGlobal::init();
        if (handle_) {
            curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(handle_, CURLOPT_FAILONERROR, 1L);
            curl_easy_setopt(handle_, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT, 30L);
            curl_easy_setopt(handle_, CURLOPT_USERAGENT, "curl/8.12.0");
        }
    }

    ~CurlEasy() {
        if (handle_) {
            curl_easy_cleanup(handle_);
            handle_ = nullptr;
        }
    }

    CurlEasy(const CurlEasy&) = delete;
    CurlEasy& operator=(const CurlEasy&) = delete;

    CurlEasy(CurlEasy&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    CurlEasy& operator=(CurlEasy&& other) noexcept {
        if (this != &other) {
            if (handle_) curl_easy_cleanup(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] CURL* handle() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    template <typename T>
    CURLcode setopt(CURLoption opt, T val) {
        return curl_easy_setopt(handle_, opt, val);
    }

    std::expected<void, std::string> perform() {
        if (!handle_) return std::unexpected("Uninitialized CURL handle");
        CURLcode res = curl_easy_perform(handle_);
        if (res != CURLE_OK) {
            return std::unexpected(curl_easy_strerror(res));
        }
        return {};
    }

private:
    CURL* handle_{nullptr};
};

using ProgressCallback = std::function<void(size_t downloaded_bytes, size_t total_bytes)>;

inline std::filesystem::path parse_local_url(std::string_view url) {
    if (url.starts_with("file://")) {
        return std::filesystem::path(url.substr(7));
    }
    if (url.starts_with("/")) {
        return std::filesystem::path(url);
    }
    return {};
}

inline std::expected<std::string, std::string> fetch_string(std::string_view url, long timeout_secs = 30) {
    if (auto local_p = parse_local_url(url); !local_p.empty()) {
        if (std::filesystem::exists(local_p)) {
            std::ifstream f(local_p);
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }
        return std::unexpected("Local file does not exist: " + local_p.string());
    }

    CurlEasy curl;
    if (!curl) return std::unexpected("Failed to initialize curl");

    std::string response;
    std::string url_str(url);

    curl.setopt(CURLOPT_URL, url_str.c_str());
    curl.setopt(CURLOPT_TIMEOUT, timeout_secs);
    curl.setopt(CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        auto* str = static_cast<std::string*>(userdata);
        str->append(ptr, size * nmemb);
        return size * nmemb;
    });
    curl.setopt(CURLOPT_WRITEDATA, &response);

    auto res = curl.perform();
    if (!res) return std::unexpected(res.error());
    return response;
}

// Download file with stream writing and automatic retry on transient errors
inline std::expected<void, std::string> download_file(
    std::string_view url,
    const std::filesystem::path& dest_path,
    ProgressCallback progress_cb = nullptr,
    size_t num_threads = 4) 
{
    (void)num_threads;
    if (auto parent = dest_path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    if (auto local_p = parse_local_url(url); !local_p.empty()) {
        if (std::filesystem::exists(local_p)) {
            std::error_code ec;
            std::filesystem::copy_file(local_p, dest_path, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) return std::unexpected("Failed to copy local package: " + ec.message());
            if (progress_cb) {
                size_t sz = std::filesystem::file_size(dest_path, ec);
                progress_cb(sz, sz);
            }
            return {};
        }
        return std::unexpected("Local source file does not exist: " + local_p.string());
    }

    std::string url_str(url);

    std::filesystem::path tmp_path = dest_path.string() + ".part";
    std::string last_error = "Unknown download error";
    for (int retry = 0; retry < 5; ++retry) {
        int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return std::unexpected("Failed to open destination file for write");

        struct SingleCtx {
            int fd;
            ProgressCallback cb;
            size_t total{0};
            size_t downloaded{0};
        } ctx{fd, progress_cb, 0, 0};

        CurlEasy curl;
        curl.setopt(CURLOPT_URL, url_str.c_str());
        curl.setopt(CURLOPT_FOLLOWLOCATION, 1L);
        curl.setopt(CURLOPT_FAILONERROR, 1L);
        curl.setopt(CURLOPT_CONNECTTIMEOUT, 30L);
        curl.setopt(CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl.setopt(CURLOPT_LOW_SPEED_TIME, 30L);

        curl.setopt(CURLOPT_XFERINFOFUNCTION, +[](void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) -> int {
            auto* c = static_cast<SingleCtx*>(clientp);
            if (dltotal > 0) c->total = static_cast<size_t>(dltotal);
            if (c->cb && c->total > 0) c->cb(static_cast<size_t>(dlnow), c->total);
            return 0;
        });
        curl.setopt(CURLOPT_XFERINFODATA, &ctx);
        curl.setopt(CURLOPT_NOPROGRESS, 0L);

        curl.setopt(CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            auto* c = static_cast<SingleCtx*>(userdata);
            const size_t bytes = size * nmemb;
            size_t done = 0;
            while (done < bytes) {
                const ssize_t written = ::write(c->fd, ptr + done, bytes - done);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) return static_cast<size_t>(0);
                done += static_cast<size_t>(written);
            }
            c->downloaded += done;
            return bytes;
        });
        curl.setopt(CURLOPT_WRITEDATA, &ctx);

        auto res = curl.perform();
        ::close(fd);

        if (res) {
            std::error_code ec;
            std::filesystem::rename(tmp_path, dest_path, ec);
            if (ec) {
                std::filesystem::remove(tmp_path, ec);
                return std::unexpected("Failed to rename downloaded archive: " + ec.message());
            }
            return {};
        }

        last_error = res.error();
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        if (retry < 4) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 * (retry + 1)));
        }
    }
    return std::unexpected(last_error);
}

} // namespace sage::vendor::curl
