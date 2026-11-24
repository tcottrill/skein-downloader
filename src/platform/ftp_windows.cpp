// FTP through WinINet: WinHTTP, used for everything else, has no FTP. wininet.h cannot be
// included with winhttp.h, so this file stands alone behind platform/ftp.h. WinINet calls
// block, so stop is checked between them; timeouts end a stalled server.
#include "platform/ftp.h"
#include "core.h"
#include <windows.h>
#include <wininet.h>
#include <algorithm>
#include <cstdio>
namespace archive::ftp {
namespace {
struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET value)
        : h(value) {}
    Handle(const Handle&) = delete;
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            h = other.h;
            other.h = nullptr;
        }
        return *this;
    }
    ~Handle() { reset(); }
    void reset() {
        if (h) InternetCloseHandle(h);
        h = nullptr;
    }
};
// The text of the server's last reply ("550 No such file"), whole and as its code.
std::string lastReply(unsigned& code) {
    DWORD error = 0, length = 0;
    code = 0;
    InternetGetLastResponseInfoW(&error, nullptr, &length);
    if (!length) return {};
    std::wstring text(length + 1, L'\0');
    length = (DWORD)text.size();
    if (!InternetGetLastResponseInfoW(&error, text.data(), &length)) return {};
    text.resize(length);
    std::string reply;
    try {
        reply = utf8(text);
    } catch (const std::exception&) {}
    // A multi-line reply ends with the line that carries the final code.
    while (!reply.empty() && (reply.back() == '\r' || reply.back() == '\n' || reply.back() == ' ')) reply.pop_back();
    auto start = reply.find_last_of('\n');
    auto last = reply.substr(start == std::string::npos ? 0 : start + 1);
    if (last.size() >= 3 && std::all_of(last.begin(), last.begin() + 3, [](unsigned char c) {
            return isdigit(c);
        })) {
        code = std::stoul(last.substr(0, 3));
        reply = last;
    }
    return reply;
}
Error failure(const std::string& step) {
    const DWORD error = GetLastError();
    unsigned code = 0;
    auto reply = lastReply(code);
    if (error == ERROR_INTERNET_LOGIN_FAILURE) return Error("FTP: the server refused anonymous sign-in" + (reply.empty() ? std::string(".") : " (" + reply + ")."), 530);
    if (error == ERROR_INTERNET_EXTENDED_ERROR && code) return Error("FTP " + reply, code);
    if (error == ERROR_INTERNET_TIMEOUT) return Error("FTP " + step + ": the server stopped answering.", 0);
    if (error == ERROR_INTERNET_NAME_NOT_RESOLVED) return Error("FTP: the server name was not found.", 0);
    return Error("FTP " + step + " failed (Windows error " + std::to_string(error) + ")", 0);
}
void cancelled(const std::atomic_bool& stop) {
    if (stop) throw std::runtime_error("Paused");
}
struct Session {
    Handle internet, connection;
    Session(const Url& url, std::atomic_bool& stop) {
        cancelled(stop);
        // A direct connection: WinINet cannot send FTP through the HTTP proxies that Windows settings name.
        internet = Handle(InternetOpenW(L"SkeinDownloader/1.0", INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0));
        if (!internet.h) throw failure("setup");
        DWORD timeout = 30000;
        for (DWORD option : {INTERNET_OPTION_CONNECT_TIMEOUT, INTERNET_OPTION_SEND_TIMEOUT, INTERNET_OPTION_RECEIVE_TIMEOUT, INTERNET_OPTION_DATA_SEND_TIMEOUT, INTERNET_OPTION_DATA_RECEIVE_TIMEOUT})
            InternetSetOptionW(internet.h, option, &timeout, sizeof timeout);
        connection = Handle(InternetConnectW(internet.h, wide(url.host).c_str(), (INTERNET_PORT)url.port, L"anonymous", L"anonymous@", INTERNET_SERVICE_FTP, INTERNET_FLAG_PASSIVE, 0));
        if (!connection.h) throw failure("sign-in");
    }
    // A command without a data connection. False when the server refuses it; code is its reply.
    bool command(const std::string& text, std::string* reply = nullptr, unsigned* code = nullptr) {
        unsigned value = 0;
        if (!FtpCommandW(connection.h, FALSE, FTP_TRANSFER_TYPE_BINARY, wide(text).c_str(), 0, nullptr)) {
            if (GetLastError() != ERROR_INTERNET_EXTENDED_ERROR) throw failure("command");
            auto answer = lastReply(value);
            if (reply) *reply = answer;
            if (code) *code = value;
            return false;
        }
        auto answer = lastReply(value);
        if (reply) *reply = answer;
        if (code) *code = value;
        return value < 400;
    }
};
}
struct File::Impl {
    Url url;
    std::atomic_bool& stop;
    Session session;
    Handle file;
    uint64_t size = 0;
    bool sizeKnown = false;
    std::string modified;
    Impl(const std::string& address, std::atomic_bool& s)
        : url(parseUrl(address)), stop(s), session(url, s) {}
};
File::File(const std::string& url, std::atomic_bool& stop)
    : impl(std::make_unique<Impl>(url, stop)) {
    auto& s = *impl;
    std::string reply;
    // SIZE answers in binary mode only on some servers; MDTM gives the time that identifies this version.
    s.session.command("TYPE I");
    unsigned code = 0;
    if (s.session.command("SIZE " + s.url.path, &reply, &code) && reply.starts_with("213 ")) {
        try {
            s.size = std::stoull(reply.substr(4));
            s.sizeKnown = true;
        } catch (const std::exception&) {}
    } else if (code == 550)
        throw Error("FTP " + reply, 550); // no such file (or a folder)
    if (!s.sizeKnown) {
        // No SIZE command: the file's own listing entry. Listing a folder lists what is in it, so the
        // entry must carry the file's own name.
        WIN32_FIND_DATAW data{};
        Handle find(FtpFindFirstFileW(s.session.connection.h, wide(s.url.path).c_str(), &data, INTERNET_FLAG_RELOAD, 0));
        auto name = wide(s.url.path.substr(s.url.path.rfind('/') + 1));
        if (find.h && !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name == data.cFileName) {
            s.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            s.sizeKnown = true;
        }
    }
    if (s.session.command("MDTM " + s.url.path, &reply) && reply.size() >= 18 && reply.starts_with("213 ")) {
        auto time = reply.substr(4, 14);
        if (std::all_of(time.begin(), time.end(), [](unsigned char c) {
                return isdigit(c);
            })) s.modified = time;
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
    // REST must come right before RETR, which FtpOpenFile sends (after PASV, which servers allow between them).
    if (offset && !s.session.command("REST " + std::to_string(offset))) throw failure("resume");
    s.file = Handle(FtpOpenFileW(s.session.connection.h, wide(s.url.path).c_str(), GENERIC_READ, FTP_TRANSFER_TYPE_BINARY | INTERNET_FLAG_RELOAD, 0));
    if (!s.file.h) throw failure("download");
}
size_t File::read(char* data, size_t size) {
    auto& s = *impl;
    cancelled(s.stop);
    DWORD got = 0;
    if (!InternetReadFile(s.file.h, data, (DWORD)std::min<size_t>(size, 1u << 20), &got)) throw failure("download");
    return got;
}

struct Lister::Impl {
    Url url;
    std::atomic_bool& stop;
    Session session;
    Impl(const std::string& address, std::atomic_bool& s)
        : url(parseUrl(address)), stop(s), session(url, s) {}
};
Lister::Lister(const std::string& url, std::atomic_bool& stop)
    : impl(std::make_unique<Impl>(url, stop)) {}
Lister::~Lister() = default;
std::vector<Entry> Lister::list(const std::string& path) {
    auto& s = *impl;
    cancelled(s.stop);
    if (!FtpSetCurrentDirectoryW(s.session.connection.h, wide(path).c_str())) throw failure("folder");
    std::vector<Entry> out;
    WIN32_FIND_DATAW data{};
    Handle find(FtpFindFirstFileW(s.session.connection.h, nullptr, &data, INTERNET_FLAG_RELOAD, 0));
    if (!find.h) {
        if (GetLastError() == ERROR_NO_MORE_FILES) return out;
        throw failure("listing");
    }
    do {
        cancelled(s.stop);
        Entry e;
        try {
            e.name = utf8(data.cFileName);
        } catch (const std::exception&) {
            continue;
        }
        if (e.name.empty() || e.name == "." || e.name == "..") continue;
        e.folder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!e.folder) {
            e.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            e.sizeKnown = true;
        }
        out.push_back(std::move(e));
    } while (InternetFindNextFileW(find.h, &data));
    if (GetLastError() != ERROR_NO_MORE_FILES) throw failure("listing");
    return out;
}
}
