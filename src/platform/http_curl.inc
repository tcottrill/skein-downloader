// Pull-style transport adapter. libcurl's multi interface bounds response buffering,
// so probing a server that ignores Range never downloads the entire body into RAM.
using DWORD = unsigned long;
constexpr const char* WINHTTP_QUERY_CONTENT_RANGE = "content-range";
constexpr const char* WINHTTP_QUERY_CONTENT_LENGTH = "content-length";
constexpr const char* WINHTTP_QUERY_ETAG = "etag";
constexpr const char* WINHTTP_QUERY_LAST_MODIFIED = "last-modified";
constexpr const char* WINHTTP_QUERY_CONTENT_TYPE = "content-type";
constexpr const char* WINHTTP_QUERY_CONTENT_DISPOSITION = "content-disposition";
constexpr const char* HEADER_LINK = "link";
// Header values are usually ASCII; read anything else as Latin-1, like WinHTTP, instead of failing the request.
std::wstring headerText(const std::string& value) {
    try {
        return wide(value);
    } catch (const std::exception&) {
        std::wstring out;
        for (unsigned char c : value) out += wchar_t(c);
        return out;
    }
}
// True when WWW-Authenticate challenges (several joined by commas) offer HTTP Basic. Each comma-separated
// piece is a challenge ("Basic realm=x") or one of its parameters ("charset=UTF-8"), which has '=' in its first word.
bool offersBasic(const std::string& challenges) {
    std::istringstream in(challenges);
    std::string piece;
    while (std::getline(in, piece, ',')) {
        const auto text = trim(piece);
        if (lower(text.substr(0, text.find_first_of(" \t"))) == "basic") return true;
    }
    return false;
}
// A bundled libcurl (AppImage) only knows the CA path of the distribution it was built on;
// find this system's bundle instead. SSL_CERT_FILE overrides, as for OpenSSL.
const char* caBundle() {
    static const std::string found = [] {
        if (auto file = std::getenv("SSL_CERT_FILE"); file && *file) return std::string(file);
        for (auto path : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem", "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", "/etc/ssl/cert.pem"})
            if (std::filesystem::is_regular_file(path)) return std::string(path);
        return std::string();
    }();
    return found.empty() ? nullptr : found.c_str();
}
struct CurlInit {
    CurlInit() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("Cannot initialize HTTP");
    }
    ~CurlInit() { curl_global_cleanup(); }
};
struct ConnectionCache {
    CURLSH* share = curl_share_init();
    ConnectionCache() {
        curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
        curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    }
    ~ConnectionCache() { curl_share_cleanup(share); }
};
struct Request {
    struct Url {
        std::string address;
        std::wstring host;
        unsigned port = 0;
        bool secure = false, isFtp = false;
        explicit Url(const std::string& value)
            : address(value) {
            static CurlInit init;
            if (ftp::isFtpUrl(value)) {
                auto u = ftp::parseUrl(value);
                host = wide(u.host);
                port = u.port;
                isFtp = true;
                return;
            }
            auto u = curl_url();
            if (!u) throw std::runtime_error("Cannot parse URL");
            char *h = nullptr, *scheme = nullptr, *p = nullptr;
            bool ok = curl_url_set(u, CURLUPART_URL, value.c_str(), 0) == CURLUE_OK && curl_url_get(u, CURLUPART_HOST, &h, 0) == CURLUE_OK && curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
                      curl_url_get(u, CURLUPART_PORT, &p, CURLU_DEFAULT_PORT) == CURLUE_OK;
            ok = ok && scheme && (std::string(scheme) == "https" || std::string(scheme) == "http");
            if (ok) {
                host = wide(h);
                secure = std::string(scheme) == "https";
                port = (unsigned)std::strtoul(p, nullptr, 10);
            }
            curl_free(h);
            curl_free(scheme);
            curl_free(p);
            curl_url_cleanup(u);
            if (!ok) throw std::runtime_error("Invalid download URL");
        }
    };
    CURL* easy = nullptr;
    CURLM* multi = nullptr;
    curl_slist* headers = nullptr;
    Request* request = this;
    DWORD status = 0;
    std::map<std::string, std::wstring> fields;
    std::vector<char> buffer;
    bool ready = false, finished = false, paused = false;
    bool manual = false;   // never follows a redirect; a redirect or a 401 answer is returned, not thrown
    bool basic = false;    // a 401 answer offered HTTP Basic
    std::wstring location; // a manual request's 3xx answer: where it points
    CURLcode result = CURLE_OK;
    std::atomic_bool* stop = nullptr;
    std::unique_ptr<FtpTransfer> ftpTransfer; // an FTP file, answered like an HTTP response
    std::string address;
    void dispose() {
        if (multi && easy) curl_multi_remove_handle(multi, easy);
        if (easy) curl_easy_cleanup(easy);
        if (multi) curl_multi_cleanup(multi);
        curl_slist_free_all(headers);
    }
    static size_t body(char* data, size_t size, size_t count, void* p) noexcept {
        auto& r = *static_cast<Request*>(p);
        size *= count;
        if (r.status != 200 && r.status != 206) return size;
        // Never pause with an empty buffer: read() only resumes after draining data.
        if (!r.buffer.empty() && r.buffer.size() + size > 128 * 1024) {
            r.paused = true;
            return CURL_WRITEFUNC_PAUSE;
        }
        try {
            r.buffer.insert(r.buffer.end(), data, data + size);
            return size;
        } catch (...) {
            return 0;
        }
    }
    static size_t onHeader(char* data, size_t size, size_t count, void* p) noexcept {
        auto& r = *static_cast<Request*>(p);
        size *= count;
        try {
            std::string s(data, size);
            if (s.starts_with("HTTP/")) {
                r.fields.clear();
                r.ready = false;
                auto at = s.find(' ');
                r.status = std::stoul(s.substr(at + 1));
            } else if (s == "\r\n") {
                // A manual request follows nothing, so its 3xx answer is the final one.
                if (r.status >= 200 && (r.status < 300 || r.status >= 400 || r.manual)) r.ready = true;
            } else if (auto at = s.find(':'); at != std::string::npos) {
                auto key = s.substr(0, at);
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
                    return std::tolower(c);
                });
                auto value = s.substr(at + 1);
                auto first = value.find_first_not_of(" \t");
                auto last = value.find_last_not_of("\r\n \t");
                auto text = first == std::string::npos ? L"" : headerText(value.substr(first, last - first + 1));
                // A server may send several challenges in separate headers; all of them are kept.
                if (key == "www-authenticate" && !r.fields[key].empty())
                    r.fields[key] += L", " + text;
                else
                    r.fields[key] = text;
            }
            return size;
        } catch (...) {
            return 0;
        }
    }
    static int progress(void* p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        auto r = static_cast<Request*>(p);
        return r->stop && r->stop->load() ? 1 : 0;
    }
    void pump() {
        if (stop && *stop) throw std::runtime_error("Paused");
        int running = 0;
        if (curl_multi_perform(multi, &running) != CURLM_OK) throw NetworkError("HTTP transfer failed");
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
    // A body makes this a POST (site APIs); downloads are always GET. wait is how long the
    // server may go without sending data (slow search services need longer).
    // manual (a request that carries a sign-in): never follows a redirect, and returns a redirect
    // or a 401 answer instead of throwing, for the caller to handle.
    Request(const Url& url, const std::wstring& extra, bool redirects = true, std::atomic_bool* cancel = nullptr, const std::string* post = nullptr, unsigned wait = 10, bool manualRedirects = false)
        : manual(manualRedirects), stop(cancel), address(url.address) {
        if (url.isFtp) {
            ftpTransfer = std::make_unique<FtpTransfer>(url.address, extra, cancel);
            auto& a = ftpTransfer->answer;
            status = a.status;
            if (!a.contentRange.empty()) fields["content-range"] = headerText(a.contentRange);
            if (a.lengthKnown) fields["content-length"] = std::to_wstring(a.length);
            if (!a.etag.empty()) fields["etag"] = headerText(a.etag);
            fields["content-type"] = L"application/octet-stream";
            return;
        }
        try {
            thread_local ConnectionCache cache;
            easy = curl_easy_init();
            multi = curl_multi_init();
            if (!easy || !multi) throw std::runtime_error("Cannot initialize HTTP request");
            curl_easy_setopt(easy, CURLOPT_SHARE, cache.share);
            if (auto ca = caBundle()) curl_easy_setopt(easy, CURLOPT_CAINFO, ca);
            curl_easy_setopt(easy, CURLOPT_URL, url.address.c_str());
            curl_easy_setopt(easy, CURLOPT_USERAGENT, "SkeinDownloader/1.0");
            // HTTPS never redirects down to HTTP. The _STR options need libcurl 7.85; older ones (Ubuntu 22.04) take bitmasks.
#if LIBCURL_VERSION_NUM >= 0x075500
            curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
            curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, url.address.starts_with("https:") ? "https" : "http,https");
#else
            curl_easy_setopt(easy, CURLOPT_PROTOCOLS, long(CURLPROTO_HTTP | CURLPROTO_HTTPS));
            curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS, url.address.starts_with("https:") ? long(CURLPROTO_HTTPS) : long(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
            curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, redirects && !manual ? 1L : 0L);
            curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 10L);
            curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
            curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
            curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, long(wait));
            curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
            headers = curl_slist_append(headers, "Accept-Encoding: identity");
            std::istringstream in(utf8(extra));
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) headers = curl_slist_append(headers, line.c_str());
            }
            if (post) {
                curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, long(post->size()));
                curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, post->c_str());
            }
            // Session cookies, shared by this thread's requests like WinHTTP's (WeTransfer needs them).
            curl_easy_setopt(easy, CURLOPT_COOKIEFILE, "");
            curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, body);
            curl_easy_setopt(easy, CURLOPT_WRITEDATA, this);
            curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, onHeader);
            curl_easy_setopt(easy, CURLOPT_HEADERDATA, this);
            curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, progress);
            curl_easy_setopt(easy, CURLOPT_XFERINFODATA, this);
            curl_multi_add_handle(multi, easy);
            while (!ready && !finished) pump();
            if (!ready && result != CURLE_OK) throw NetworkError(curl_easy_strerror(result));
            if (status == 401) basic = offersBasic(utf8(fields["www-authenticate"]));
            if (manual && (status == 401 || isRedirect(status))) {
                location = fields["location"];
                return;
            }
            // The host that refused: the last one, after any redirects.
            if (status == 401) {
                const auto after = this->url();
                throw SignInNeeded(signInHost(after.empty() ? url.host : Url(after).host), false, basic);
            }
            if (status != 200 && status != 206) {
                unsigned seconds = 1;
                auto value = fields["retry-after"];
                if (!value.empty()) {
                    try {
                        seconds = std::stoul(value);
                    } catch (...) {
                        auto when = curl_getdate(utf8(value).c_str(), nullptr);
                        if (when > std::time(nullptr)) seconds = unsigned(when - std::time(nullptr));
                    }
                }
                throw statusError(status, seconds);
            }
        } catch (...) {
            dispose();
            throw;
        }
    }
    ~Request() { dispose(); }
    DWORD read(char* data, DWORD size) {
        if (ftpTransfer) return ftpTransfer->read(data, size);
        while (buffer.empty() && !finished) pump();
        if (buffer.empty()) {
            if (result != CURLE_OK) throw NetworkError(curl_easy_strerror(result));
            return 0;
        }
        auto n = std::min<size_t>(size, buffer.size());
        std::memcpy(data, buffer.data(), n);
        buffer.erase(buffer.begin(), buffer.begin() + n);
        if (paused && buffer.size() < 64 * 1024) {
            paused = false;
            curl_easy_pause(easy, CURLPAUSE_CONT);
        }
        return n;
    }
    // The URL after redirects.
    std::string url() {
        if (ftpTransfer) return address;
        char* value = nullptr;
        curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &value);
        return value ? value : "";
    }
    std::string host() {
        auto value = url();
        return value.empty() ? "" : utf8(Url(value).host);
    }
};
std::wstring header(Request* r, const char* name) {
    return r->fields[name];
}
