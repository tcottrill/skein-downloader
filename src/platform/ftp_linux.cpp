// FTP through libcurl, behind platform/ftp.h. One easy handle per file or lister keeps the
// control connection open between its requests; transfers are read pull-style, like http_curl.inc.
#include "platform/ftp.h"
#include "core.h"
#include <curl/curl.h>
#include <algorithm>
#include <cstring>
#include <ctime>
namespace archive::ftp {
namespace {
struct CurlInit {
    CurlInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlInit() { curl_global_cleanup(); }
};
void cancelled(const std::atomic_bool& stop) {
    if (stop) throw std::runtime_error("Paused");
}
std::string base(const Url& url) {
    return "ftp://" + url.host + (url.port == 21 ? "" : ":" + std::to_string(url.port));
}
CURL* handle(std::atomic_bool& stop) {
    static CurlInit init;
    auto easy = curl_easy_init();
    if (!easy) throw Error("FTP setup failed.", 0);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "SkeinDownloader/1.0");
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, +[](void* p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
        return static_cast<std::atomic_bool*>(p)->load() ? 1 : 0;
    });
    curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &stop);
    // Without a writer libcurl prints to stdout, including the header-like lines it makes up for SIZE and MDTM.
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, +[](char*, size_t size, size_t count, void*) -> size_t {
        return size * count;
    });
    return easy;
}
Error failure(CURL* easy, CURLcode result, const std::string& step) {
    long reply = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &reply);
    if (result == CURLE_LOGIN_DENIED) return Error("FTP: the server refused anonymous sign-in.", 530);
    if (result == CURLE_REMOTE_FILE_NOT_FOUND || result == CURLE_REMOTE_ACCESS_DENIED || result == CURLE_FTP_COULDNT_RETR_FILE) return Error("FTP " + std::to_string(reply ? reply : 550) + ": " + curl_easy_strerror(result), reply >= 400 ? (unsigned)reply : 550);
    return Error("FTP " + step + ": " + curl_easy_strerror(result), reply >= 400 && reply < 600 ? (unsigned)reply : 0);
}
size_t collect(char* data, size_t size, size_t count, void* p) {
    static_cast<std::string*>(p)->append(data, size * count);
    return size * count;
}
}
struct File::Impl {
    Url url;
    std::atomic_bool& stop;
    CURL* easy = nullptr;
    CURLM* multi = nullptr;
    uint64_t size = 0;
    bool sizeKnown = false;
    std::string modified;
    std::vector<char> buffer;
    bool finished = false, paused = false;
    CURLcode result = CURLE_OK;
    Impl(const std::string& address, std::atomic_bool& s)
        : url(parseUrl(address)), stop(s), easy(handle(s)) {}
    ~Impl() {
        if (multi) {
            curl_multi_remove_handle(multi, easy);
            curl_multi_cleanup(multi);
        }
        curl_easy_cleanup(easy);
    }
    static size_t body(char* data, size_t size, size_t count, void* p) noexcept {
        auto& s = *static_cast<Impl*>(p);
        size *= count;
        if (!s.buffer.empty() && s.buffer.size() + size > 128 * 1024) {
            s.paused = true;
            return CURL_WRITEFUNC_PAUSE;
        }
        try {
            s.buffer.insert(s.buffer.end(), data, data + size);
            return size;
        } catch (...) {
            return 0;
        }
    }
    void pump() {
        cancelled(stop);
        int running = 0;
        if (curl_multi_perform(multi, &running) != CURLM_OK) throw Error("FTP transfer failed.", 0);
        int count = 0;
        while (auto msg = curl_multi_info_read(multi, &count))
            if (msg->msg == CURLMSG_DONE) {
                finished = true;
                result = msg->data.result;
            }
        if (!finished && buffer.empty()) {
            int n = 0;
            curl_multi_poll(multi, nullptr, 0, 100, &n);
        }
    }
};
File::File(const std::string& url, std::atomic_bool& stop)
    : impl(std::make_unique<Impl>(url, stop)) {
    auto& s = *impl;
    cancelled(stop);
    // NOBODY asks for SIZE and, with FILETIME, MDTM, without a transfer.
    curl_easy_setopt(s.easy, CURLOPT_URL, (base(s.url) + encode(s.url.path, true)).c_str());
    curl_easy_setopt(s.easy, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(s.easy, CURLOPT_FILETIME, 1L);
    if (auto result = curl_easy_perform(s.easy); result != CURLE_OK) {
        if (stop) throw std::runtime_error("Paused");
        throw failure(s.easy, result, "file");
    }
    curl_off_t length = -1;
    curl_easy_getinfo(s.easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
    if (length >= 0) {
        s.size = (uint64_t)length;
        s.sizeKnown = true;
    }
    curl_off_t time = -1;
    curl_easy_getinfo(s.easy, CURLINFO_FILETIME_T, &time);
    if (time >= 0) {
        std::time_t t = (std::time_t)time;
        std::tm utc{};
        gmtime_r(&t, &utc);
        char text[16];
        std::strftime(text, sizeof text, "%Y%m%d%H%M%S", &utc);
        s.modified = text;
    }
}
File::~File() = default;
bool File::sizeKnown() const {
    return impl->sizeKnown;
}
uint64_t File::size() const {
    return impl->size;
}
const std::string& File::modified() const {
    return impl->modified;
}
void File::open(uint64_t offset) {
    auto& s = *impl;
    cancelled(s.stop);
    curl_easy_setopt(s.easy, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(s.easy, CURLOPT_FILETIME, 0L);
    curl_easy_setopt(s.easy, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)offset);
    curl_easy_setopt(s.easy, CURLOPT_WRITEFUNCTION, Impl::body);
    curl_easy_setopt(s.easy, CURLOPT_WRITEDATA, impl.get());
    s.multi = curl_multi_init();
    if (!s.multi) throw Error("FTP setup failed.", 0);
    curl_multi_add_handle(s.multi, s.easy);
}
size_t File::read(char* data, size_t size) {
    auto& s = *impl;
    while (s.buffer.empty() && !s.finished) s.pump();
    if (s.buffer.empty()) {
        if (s.result != CURLE_OK) {
            if (s.stop) throw std::runtime_error("Paused");
            throw failure(s.easy, s.result, "download");
        }
        return 0;
    }
    auto n = std::min(size, s.buffer.size());
    std::memcpy(data, s.buffer.data(), n);
    s.buffer.erase(s.buffer.begin(), s.buffer.begin() + n);
    if (s.paused && s.buffer.size() < 64 * 1024) {
        s.paused = false;
        curl_easy_pause(s.easy, CURLPAUSE_CONT);
    }
    return n;
}

struct Lister::Impl {
    Url url;
    std::atomic_bool& stop;
    CURL* easy;
    Impl(const std::string& address, std::atomic_bool& s)
        : url(parseUrl(address)), stop(s), easy(handle(s)) {}
    ~Impl() { curl_easy_cleanup(easy); }
};
Lister::Lister(const std::string& url, std::atomic_bool& stop)
    : impl(std::make_unique<Impl>(url, stop)) {}
Lister::~Lister() = default;
std::vector<Entry> Lister::list(const std::string& path) {
    auto& s = *impl;
    cancelled(s.stop);
    // A URL ending in '/' asks libcurl for the folder's LIST output.
    std::string text;
    curl_easy_setopt(s.easy, CURLOPT_URL, (base(s.url) + encode(path, true)).c_str());
    curl_easy_setopt(s.easy, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(s.easy, CURLOPT_WRITEDATA, &text);
    if (auto result = curl_easy_perform(s.easy); result != CURLE_OK) {
        if (s.stop) throw std::runtime_error("Paused");
        throw failure(s.easy, result, "listing");
    }
    return parseList(text);
}
}
