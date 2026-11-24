#pragma once
// FTP for folder listings and downloads (anonymous FTP only). Windows uses WinINet
// (ftp_windows.cpp; WinHTTP has no FTP), Linux uses libcurl (ftp_linux.cpp). The rest is
// portable (ftp.cpp). http.cpp makes an FTP file look like an HTTP response to the download
// engine: a byte range becomes a restart offset (REST) and the size and modification time
// (SIZE, MDTM) stand in for an ETag, so multi-connection downloads and resuming still work.
#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace archive::ftp {
// A failed FTP step. reply is the server's three-digit reply, or 0 when the connection failed.
struct Error : std::runtime_error {
    unsigned reply;
    Error(const std::string& message, unsigned code)
        : std::runtime_error(message), reply(code) {}
    // Connection failures and 4xx replies (421 too many users, 425/426 data connection) are temporary.
    bool temporary() const { return reply == 0 || (reply >= 400 && reply < 500); }
};
// An ftp:// address. path is percent-decoded and starts with '/'.
struct Url {
    std::string host, path;
    unsigned port = 21;
};
bool isFtpUrl(const std::string& input);
// Throws for anything but an anonymous ftp:// address (no user name or password).
Url parseUrl(const std::string& input);
struct Entry {
    std::string name;
    bool folder = false;
    uint64_t size = 0;
    bool sizeKnown = false;
};
// One signed-in session that lists folders on the server of url, one after another.
class Lister {
public:
    Lister(const std::string& url, std::atomic_bool& stop);
    ~Lister();
    Lister(const Lister&) = delete;
    // The entries of a folder (a decoded path ending in '/'), without "." and "..".
    std::vector<Entry> list(const std::string& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
// LIST output in Unix "ls -l" or MS-DOS form, as libcurl returns it. A symbolic link is listed
// under its own name as a file.
std::vector<Entry> parseList(const std::string& text);
// One file on a server: the constructor logs in and asks for its size and modification time.
class File {
public:
    File(const std::string& url, std::atomic_bool& stop);
    ~File();
    File(const File&) = delete;
    bool sizeKnown() const;
    uint64_t size() const;
    // The modification time from MDTM (YYYYMMDDhhmmss); empty when the server has none.
    const std::string& modified() const;
    // Starts the transfer at offset (REST, then RETR). read returns 0 at the end of the file.
    void open(uint64_t offset);
    size_t read(char* data, size_t size);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
// How a request with HTTP "Range" and "If-Range" headers is answered from an FTP file:
// status 206 with a Content-Range, or 200 for the whole file (no range, unknown size, or a
// stale If-Range). Throws Error 416 for a range past the end.
struct Answer {
    unsigned status = 200;
    uint64_t offset = 0, length = 0;
    bool lengthKnown = false;
    std::string contentRange, etag;
};
Answer answer(bool sizeKnown, uint64_t size, const std::string& modified, const std::string& requestHeaders);
}
