struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET value)
        : h(value) {
        if (!h) throw std::runtime_error("WinHTTP initialization failed: " + std::to_string(GetLastError()));
    }
    ~Handle() { WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete;
    operator HINTERNET() const { return h; }
};
std::wstring header(HINTERNET h, DWORD type) {
    DWORD length = 0;
    WinHttpQueryHeaders(h, type, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &length, WINHTTP_NO_HEADER_INDEX);
    if (!length) return {};
    std::wstring value(length / sizeof(wchar_t), 0);
    if (!WinHttpQueryHeaders(h, type, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &length, WINHTTP_NO_HEADER_INDEX)) return {};
    value.resize(length / sizeof(wchar_t));
    return value;
}
// A header WinHTTP has no index for, read by name.
std::wstring header(HINTERNET h, const wchar_t* name) {
    DWORD length = 0;
    WinHttpQueryHeaders(h, WINHTTP_QUERY_CUSTOM, name, nullptr, &length, WINHTTP_NO_HEADER_INDEX);
    if (!length) return {};
    std::wstring value(length / sizeof(wchar_t), 0);
    if (!WinHttpQueryHeaders(h, WINHTTP_QUERY_CUSTOM, name, value.data(), &length, WINHTTP_NO_HEADER_INDEX)) return {};
    value.resize(length / sizeof(wchar_t));
    return value;
}
constexpr const wchar_t* HEADER_LINK = L"Link";
struct Request {
    std::unique_ptr<Handle> connection, http; // WinHTTP; empty for an FTP file
    std::unique_ptr<FtpTransfer> ftpTransfer; // an FTP file, answered like an HTTP response
    Request* request = this;                  // header(r->request, ...) reads either kind, as with libcurl
    DWORD status = 0;
    std::string address;
    static HINTERNET session() {
        // One session for the whole program, shared by every thread (WinHTTP sessions are thread-safe)
        // and kept across queued ranges and retries. It is never closed: a per-thread session was closed
        // by its thread_local destructor, which runs during thread exit under the Windows loader lock,
        // where WinHttpCloseHandle can deadlock. That left a finished worker unjoinable, so the window
        // could not close. Windows releases the session when the process ends.
        static Handle* const value = new Handle(WinHttpOpen(L"SkeinDownloader/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        return *value;
    }
    struct Url {
        std::wstring host, path;
        INTERNET_PORT port;
        bool secure = false, isFtp = false;
        std::string address;
        explicit Url(const std::string& text)
            : address(text) {
            if (ftp::isFtpUrl(text)) {
                auto u = ftp::parseUrl(text);
                host = wide(u.host);
                port = (INTERNET_PORT)u.port;
                isFtp = true;
                return;
            }
            auto value = wide(text);
            URL_COMPONENTS c{sizeof c};
            c.dwHostNameLength = c.dwUrlPathLength = c.dwExtraInfoLength = (DWORD)-1;
            if (!WinHttpCrackUrl(value.c_str(), 0, 0, &c) || (c.nScheme != INTERNET_SCHEME_HTTP && c.nScheme != INTERNET_SCHEME_HTTPS))
                throw std::runtime_error("Invalid download URL");
            host.assign(c.lpszHostName, c.dwHostNameLength);
            path.assign(c.lpszUrlPath, c.dwUrlPathLength);
            if (c.dwExtraInfoLength) path.append(c.lpszExtraInfo, c.dwExtraInfoLength);
            port = c.nPort;
            secure = c.nScheme == INTERNET_SCHEME_HTTPS;
        }
    };
    // A body makes this a POST (site APIs); downloads are always GET. wait is how long the
    // server may take to answer or to send more data (slow search services need longer).
    Request(const Url& url, const std::wstring& headers, bool redirects = true, const std::string* body = nullptr, unsigned wait = 10, std::atomic_bool* stop = nullptr)
        : address(url.address) {
        if (url.isFtp) {
            ftpTransfer = std::make_unique<FtpTransfer>(url.address, headers, stop);
            status = ftpTransfer->answer.status;
            return;
        }
        connection = std::make_unique<Handle>(WinHttpConnect(session(), url.host.c_str(), url.port, 0));
        http = std::make_unique<Handle>(WinHttpOpenRequest(*connection, body ? L"POST" : L"GET", url.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0));
        auto& handle = *http;
        WinHttpSetTimeouts(handle, 10000, 10000, 10000, int(wait * 1000));
        DWORD policy = redirects ? WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(handle, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
        auto allHeaders = L"Accept-Encoding: identity\r\n" + headers;
        auto data = body ? (LPVOID)body->data() : WINHTTP_NO_REQUEST_DATA;
        DWORD length = body ? (DWORD)body->size() : 0;
        if (!WinHttpSendRequest(handle, allHeaders.c_str(), (DWORD)-1, data, length, length, 0) || !WinHttpReceiveResponse(handle, nullptr))
            throw NetworkError("Connection failed (Windows error " + std::to_string(GetLastError()) + ")");
        DWORD n = sizeof status;
        if (!WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &n, nullptr))
            throw NetworkError("Cannot read HTTP status");
        if (status != 200 && status != 206) {
            unsigned seconds = 1;
            auto retryAfter = header(handle, WINHTTP_QUERY_RETRY_AFTER);
            if (!retryAfter.empty()) {
                try {
                    seconds = (unsigned)std::stoul(retryAfter);
                } catch (...) {
                    SYSTEMTIME st{};
                    FILETIME ft{}, now{};
                    if (WinHttpTimeToSystemTime(retryAfter.c_str(), &st) && SystemTimeToFileTime(&st, &ft)) {
                        GetSystemTimeAsFileTime(&now);
                        ULARGE_INTEGER t{}, current{};
                        t.LowPart = ft.dwLowDateTime;
                        t.HighPart = ft.dwHighDateTime;
                        current.LowPart = now.dwLowDateTime;
                        current.HighPart = now.dwHighDateTime;
                        if (t.QuadPart > current.QuadPart) seconds = (unsigned)((t.QuadPart - current.QuadPart) / 10000000 + 1);
                    }
                }
            }
            throw statusError(status, seconds);
        }
    }
    DWORD read(char* data, DWORD size) {
        if (ftpTransfer) return ftpTransfer->read(data, size);
        DWORD got = 0;
        if (!WinHttpReadData(*http, data, size, &got)) throw NetworkError("Download interrupted (Windows error " + std::to_string(GetLastError()) + ")");
        return got;
    }
    // The URL after redirects.
    std::string url() {
        if (ftpTransfer) return address;
        DWORD length = 0;
        WinHttpQueryOption(*http, WINHTTP_OPTION_URL, nullptr, &length);
        if (!length) return {};
        std::wstring value(length / sizeof(wchar_t), 0);
        if (!WinHttpQueryOption(*http, WINHTTP_OPTION_URL, value.data(), &length)) return {};
        value.resize(wcslen(value.c_str()));
        return utf8(value);
    }
    std::string host() {
        auto value = url();
        return value.empty() ? "" : utf8(Url(value).host);
    }
};
// Response headers of either kind of request; an FTP file has the few the download engine reads.
std::wstring header(Request* r, DWORD type) {
    if (!r->ftpTransfer) return header((HINTERNET)*r->http, type);
    const auto& a = r->ftpTransfer->answer;
    switch (type) {
    case WINHTTP_QUERY_CONTENT_RANGE: return wide(a.contentRange);
    case WINHTTP_QUERY_CONTENT_LENGTH: return a.lengthKnown ? std::to_wstring(a.length) : std::wstring();
    case WINHTTP_QUERY_ETAG: return wide(a.etag);
    case WINHTTP_QUERY_CONTENT_TYPE: return L"application/octet-stream";
    default: return {};
    }
}
std::wstring header(Request* r, const wchar_t* name) {
    return r->ftpTransfer ? std::wstring() : header((HINTERNET)*r->http, name);
}
