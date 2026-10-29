struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET value) : h(value) { if (!h) throw std::runtime_error("WinHTTP initialization failed: " + std::to_string(GetLastError())); }
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
    value.resize(length / sizeof(wchar_t)); return value;
}
// A header WinHTTP has no index for, read by name.
std::wstring header(HINTERNET h, const wchar_t* name) {
    DWORD length = 0;
    WinHttpQueryHeaders(h, WINHTTP_QUERY_CUSTOM, name, nullptr, &length, WINHTTP_NO_HEADER_INDEX);
    if (!length) return {};
    std::wstring value(length / sizeof(wchar_t), 0);
    if (!WinHttpQueryHeaders(h, WINHTTP_QUERY_CUSTOM, name, value.data(), &length, WINHTTP_NO_HEADER_INDEX)) return {};
    value.resize(length / sizeof(wchar_t)); return value;
}
constexpr const wchar_t* HEADER_LINK = L"Link";
struct Request {
    Handle connection, request;
    DWORD status = 0;
    static HINTERNET session() {
        // Keep the worker's session alive across queued ranges and retries.
        thread_local Handle value(WinHttpOpen(L"SkeinDownloader/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        return value;
    }
    struct Url {
        std::wstring host, path; INTERNET_PORT port; bool secure;
        explicit Url(const std::string& address) {
            auto value = wide(address); URL_COMPONENTS c{sizeof c};
            c.dwHostNameLength = c.dwUrlPathLength = c.dwExtraInfoLength = (DWORD)-1;
            if (!WinHttpCrackUrl(value.c_str(), 0, 0, &c) || (c.nScheme != INTERNET_SCHEME_HTTP && c.nScheme != INTERNET_SCHEME_HTTPS))
                throw std::runtime_error("Invalid download URL");
            host.assign(c.lpszHostName, c.dwHostNameLength); path.assign(c.lpszUrlPath, c.dwUrlPathLength);
            if(c.dwExtraInfoLength) path.append(c.lpszExtraInfo, c.dwExtraInfoLength);
            port = c.nPort; secure = c.nScheme == INTERNET_SCHEME_HTTPS;
        }
    };
    // A body makes this a POST (site APIs); downloads are always GET.
    Request(const Url& url, const std::wstring& headers, bool redirects = true, const std::string* body = nullptr) :
        connection(WinHttpConnect(session(), url.host.c_str(), url.port, 0)),
        request(WinHttpOpenRequest(connection, body ? L"POST" : L"GET", url.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0)) {
        WinHttpSetTimeouts(request, 10000, 10000, 10000, 10000);
        DWORD policy = redirects ? WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
        auto allHeaders = L"Accept-Encoding: identity\r\n" + headers;
        auto data = body ? (LPVOID)body->data() : WINHTTP_NO_REQUEST_DATA; DWORD length = body ? (DWORD)body->size() : 0;
        if (!WinHttpSendRequest(request, allHeaders.c_str(), (DWORD)-1, data, length, length, 0) || !WinHttpReceiveResponse(request, nullptr))
            throw NetworkError("Connection failed (Windows error " + std::to_string(GetLastError()) + ")");
        DWORD n = sizeof status;
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &n, nullptr))
            throw NetworkError("Cannot read HTTP status");
        if (status != 200 && status != 206) {
            unsigned seconds = 1;
            auto retryAfter = header(request, WINHTTP_QUERY_RETRY_AFTER);
            if (!retryAfter.empty()) {
                try { seconds = (unsigned)std::stoul(retryAfter); } catch (...) {
                    SYSTEMTIME st{}; FILETIME ft{}, now{};
                    if (WinHttpTimeToSystemTime(retryAfter.c_str(), &st) && SystemTimeToFileTime(&st, &ft)) {
                        GetSystemTimeAsFileTime(&now);
                        ULARGE_INTEGER t{}, current{}; t.LowPart=ft.dwLowDateTime; t.HighPart=ft.dwHighDateTime;
                        current.LowPart=now.dwLowDateTime; current.HighPart=now.dwHighDateTime;
                        if(t.QuadPart>current.QuadPart) seconds=(unsigned)((t.QuadPart-current.QuadPart)/10000000+1);
                    }
                }
            }
            throw statusError(status, seconds);
        }
    }
    DWORD read(char* data, DWORD size) {
        DWORD got = 0;
        if (!WinHttpReadData(request, data, size, &got)) throw NetworkError("Download interrupted (Windows error " + std::to_string(GetLastError()) + ")");
        return got;
    }
    // The URL after redirects.
    std::string url() {
        DWORD length=0; WinHttpQueryOption(request,WINHTTP_OPTION_URL,nullptr,&length);
        if(!length)return {};
        std::wstring value(length/sizeof(wchar_t),0);
        if(!WinHttpQueryOption(request,WINHTTP_OPTION_URL,value.data(),&length))return {};
        value.resize(wcslen(value.c_str()));
        return utf8(value);
    }
    std::string host() { auto value=url(); return value.empty()?"":utf8(Url(value).host); }
};
