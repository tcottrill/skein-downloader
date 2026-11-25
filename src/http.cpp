#include "core.h"
#include "extras.h"
#include "host_limit.h"
#include "signin.h"
#include "sites.h"
#include "staging.h"
#include "json.hpp"
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#include <map>
#include <cstring>
#include <ctime>
#endif
#include "platform/files.h"
#include "platform/ftp.h"
#include "util/sys_log.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <thread>
namespace archive {
std::string headerUtf8(const std::wstring& value);
namespace {
// The registry behind LoadWatch: stop flag address to progress. Reads look it up once per call;
// watches_ lets a read with no watch anywhere skip the lock.
std::mutex watchLock;
std::map<const std::atomic_bool*, LoadProgress*> watched;
std::atomic<size_t> watches_{0};
LoadProgress* progressFor(const std::atomic_bool& stop) {
    if (!watches_.load(std::memory_order_relaxed)) return nullptr;
    std::lock_guard lock(watchLock);
    auto found = watched.find(&stop);
    return found == watched.end() ? nullptr : found->second;
}
// One more request answered for the load operation that owns this stop flag, if any.
void countRequest(const std::atomic_bool& stop) {
    if (auto progress = progressFor(stop)) ++progress->requests;
}
// The User-Agent file requests send (see setUserAgent); the lock is only held to copy it.
std::mutex agentLock;
std::string agentValue = defaultUserAgent;
// Shared by both transports for any response other than 200/206 (a 401 is SignInNeeded instead).
NetworkError statusError(unsigned long status, unsigned retryAfter) {
    std::string message = "HTTP " + std::to_string(status);
    if (status == 403) {
        message += ": the server refused access to this file.";
        if (userAgent() == defaultUserAgent) message += " Some servers refuse download programs; Settings can make Skein identify as a web browser.";
    }
    if (status == 404) message += ": file not found.";
    return NetworkError(message, status == 429 || status == 408 || status >= 500, retryAfter, (unsigned)status);
}
// The statuses whose Location the engine follows itself on a request that carries a sign-in.
bool isRedirect(unsigned long status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}
// A host as the sign-in table names it: lower case, without the brackets of an IPv6 address.
std::string signInHost(const std::wstring& host) {
    auto value = lower(utf8(host));
    if (value.size() >= 2 && value.front() == '[' && value.back() == ']') value = value.substr(1, value.size() - 2);
    return value;
}
// A ranged request showed that the parts cannot be combined: the server sent the whole file (it will not
// honor ranges, or the file changed since the probe) or another version of it. Not a NetworkError, so
// the per-part retry does not catch it; download() discards the staged data and starts again on one connection.
struct RestartWhole : std::runtime_error {
    using std::runtime_error::runtime_error;
};
void cancelled(const std::atomic_bool& stop) {
    if (stop) throw std::runtime_error("Paused");
}
void delay(unsigned seconds, const std::atomic_bool& stop) {
    for (uint64_t i = 0; i < (uint64_t)seconds * 10; ++i) {
        cancelled(stop);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
// An FTP file read through the HTTP-shaped Request: the "Range" and "If-Range" headers pick
// the restart offset, and FTP errors become the network errors the engine retries or reports.
NetworkError ftpFailure(const ftp::Error& e) {
    if (e.reply == 550) return NetworkError("FTP 550: file not found, or it is a folder.", false);
    return NetworkError(e.what(), e.temporary(), e.reply == 421 ? 10 : 1);
}
struct FtpTransfer {
    std::unique_ptr<ftp::File> file;
    ftp::Answer answer;
    uint64_t left = 0;
    bool opened = false;
    FtpTransfer(const std::string& url, const std::wstring& headers, std::atomic_bool* stop) {
        static std::atomic_bool never = false;
        try {
            file = std::make_unique<ftp::File>(url, stop ? *stop : never);
            answer = ftp::answer(file->sizeKnown(), file->size(), file->modified(), utf8(headers));
            left = answer.length;
        } catch (const ftp::Error& e) {
            throw ftpFailure(e);
        }
    }
    // The transfer starts on the first read, so a probe for the size opens no data connection.
    unsigned long read(char* data, unsigned long size) {
        try {
            if (!opened) {
                file->open(answer.offset);
                opened = true;
            }
            if (answer.lengthKnown) {
                if (!left) return 0;
                size = (unsigned long)std::min<uint64_t>(size, left);
            }
            auto n = file->read(data, size);
            left -= std::min<uint64_t>(n, left);
            return (unsigned long)n;
        } catch (const ftp::Error& e) {
            throw ftpFailure(e);
        }
    }
};
#ifdef _WIN32
#include "platform/http_winhttp.inc"
#else
#include "platform/http_curl.inc"
#endif
// "/a/./b/../c" as "/a/c" (RFC 3986, section 5.2.4), for a redirect's relative address.
std::string removeDotSegments(const std::string& path) {
    std::vector<std::string> kept;
    bool directory = false; // the last segment was "." or "..": the result ends with a slash
    for (size_t at = 1; at <= path.size();) {
        auto end = path.find('/', at);
        if (end == std::string::npos) end = path.size();
        const auto segment = path.substr(at, end - at);
        directory = segment == "." || segment == "..";
        if (segment == "..") {
            if (!kept.empty()) kept.pop_back();
        } else if (segment != ".")
            kept.push_back(segment);
        at = end + 1;
    }
    std::string out;
    for (auto& segment : kept) out += "/" + segment;
    if (out.empty() || directory) out += "/";
    return out;
}
// A redirect's Location made absolute against the address that answered with it; empty when the
// result is not an http or https address.
std::string resolveLocation(const std::string& base, std::string location) {
    location = trim(location);
    location = location.substr(0, location.find('#'));
    const auto schemeEnd = base.find("://");
    if (schemeEnd == std::string::npos) return {};
    const auto authorityEnd = base.find_first_of("/?#", schemeEnd + 3);
    const auto origin = base.substr(0, authorityEnd);
    auto rest = authorityEnd == std::string::npos ? std::string("/") : base.substr(authorityEnd);
    rest = rest.substr(0, rest.find('#'));
    if (rest.empty() || rest[0] != '/') rest = "/" + rest;
    const auto basePath = rest.substr(0, rest.find('?'));
    std::string target;
    const auto colon = location.find(':'), delimiter = location.find_first_of("/?");
    if (colon != std::string::npos && colon > 0 && (delimiter == std::string::npos || colon < delimiter))
        target = location;
    else if (location.starts_with("//"))
        target = base.substr(0, schemeEnd) + ":" + location;
    else if (location.starts_with("/"))
        target = origin + location;
    else if (location.empty())
        target = origin + rest;
    else if (location[0] == '?')
        target = origin + basePath + location;
    else
        target = origin + basePath.substr(0, basePath.rfind('/') + 1) + location;
    const auto lowered = lower(target.substr(0, 8));
    if (!lowered.starts_with("http://") && !lowered.starts_with("https://")) return {};
    // The path's dot segments are resolved; the address's own spelling is otherwise kept.
    const auto hostStart = target.find("://") + 3;
    const auto pathStart = target.find_first_of("/?", hostStart);
    if (pathStart == std::string::npos) return target + "/";
    const auto queryStart = target.find('?', pathStart);
    const auto path = target.substr(pathStart, queryStart == std::string::npos ? std::string::npos : queryStart - pathStart);
    return target.substr(0, pathStart) + (path.empty() ? std::string("/") : removeDotSegments(path)) + (queryStart == std::string::npos ? "" : target.substr(queryStart));
}
// The servers ("host:port", lower-case host) that answered a request on the manual path with a Basic
// challenge, so that a password goes with later requests to them at once, as browsers do. Each entry
// belongs to the sign-in table generation it was learned under: a newer table empties the set, and a
// request built under an older one neither reads nor changes it.
std::mutex challengedLock;
std::set<std::string> challenged;
uint64_t challengedGeneration = 0;
void adoptGeneration(uint64_t generation) {
    if (generation > challengedGeneration) {
        challenged.clear();
        challengedGeneration = generation;
    }
}
bool wasChallenged(const std::string& server, uint64_t generation) {
    std::lock_guard lock(challengedLock);
    adoptGeneration(generation);
    return generation == challengedGeneration && challenged.contains(server);
}
void setChallenged(const std::string& server, uint64_t generation, bool value) {
    std::lock_guard lock(challengedLock);
    adoptGeneration(generation);
    if (generation != challengedGeneration) return;
    if (value)
        challenged.insert(server);
    else
        challenged.erase(server);
}
// One transport request, as the platform's Request takes its arguments.
std::unique_ptr<Request> build(const Request::Url& url, const std::wstring& headers, bool redirects, std::atomic_bool& stop, const std::string* body, unsigned wait, bool manual) {
#ifdef _WIN32
    return std::make_unique<Request>(url, headers, redirects, body, wait, &stop, manual);
#else
    return std::make_unique<Request>(url, headers, redirects, &stop, body, wait, manual);
#endif
}
// Every HTTP request is built here, once, without retries. A request to a host with no saved sign-in
// is exactly the transport's request, which follows redirects itself (when redirects is true). A host
// with a sign-in takes the manual path: the sign-in is attached when it may be (at once for a token,
// after a Basic challenge for a password), and each redirect is followed here, so that every hop looks
// its own host up again and a sign-in never reaches a host it does not cover. Request::url() is the
// final address on both paths. A 401 is SignInNeeded on both.
std::unique_ptr<Request> open(const std::string& url, const std::wstring& requestHeaders, std::atomic_bool& stop, bool redirects = true, unsigned wait = 10, const std::string* body = nullptr) {
    std::string current = url;
    // Extras of this operation (a referer, cookies, headers of one link): while it has any, every request of
    // it takes the manual path, so that the engine follows its redirects and each hop gets the extras only if
    // it is to the link's own host, never a transport-followed redirect that would carry them along.
    const auto extras = extrasFor(stop);
    for (unsigned hops = 0;;) {
        cancelled(stop);
        // A release asset's API address answers with the file only when asked for application/octet-stream:
        // every request for that address (probe, ranges, speed samples) gets it, and no other address does.
        auto headers = isGitHubAssetApiUrl(current) ? requestHeaders + L"Accept: application/octet-stream\r\n" : requestHeaders;
        const Request::Url target(current);
        const auto host = signInHost(target.host);
        if (target.isFtp || (!extras && !hasSignIn(host, target.port))) return build(target, headers, redirects, stop, body, wait, false);
        // The extras for this hop's own address, when it is the link's host (never logged).
        if (extras) headers += wide(extraHeadersFor(*extras, target.secure ? "https" : "http", host, target.port));
        // Read before the lookup: if the table changes in between, what this request learns is dropped.
        const auto generation = signInGeneration();
        const auto signIn = signInFor(target.secure ? "https" : "http", host, target.port);
        const auto server = host + ":" + std::to_string(target.port);
        // A password goes at once to a server that has challenged before under this table.
        const bool early = signIn && !sentAtOnce(*signIn) && wasChallenged(server, generation);
        bool sent = signIn && (sentAtOnce(*signIn) || early);
        auto r = build(target, sent ? headers + wide(authorizationHeader(*signIn)) : headers, redirects, stop, body, wait, true);
        if (r->status == 401 && signIn && !sent && r->basic) {
            r.reset();
            setChallenged(server, generation, true);
            cancelled(stop);
            sent = true;
            r = build(target, headers + wide(authorizationHeader(*signIn)), redirects, stop, body, wait, true);
        }
        if (r->status == 401) {
            if (early) setChallenged(server, generation, false);
            throw SignInNeeded(host, sent, r->basic);
        }
        if (!isRedirect(r->status)) return r;
        // The transport would not have followed it either.
        const auto status = r->status;
        if (!redirects) throw statusError(status, 1);
        const auto next = resolveLocation(current, headerUtf8(r->location));
        r.reset();
        if (next.empty()) throw NetworkError("HTTP " + std::to_string(status) + ": the server's redirect has no usable address.", false, 1, (unsigned)status);
        if (target.secure && !lower(next.substr(0, 8)).starts_with("https://"))
            throw NetworkError("The server redirected from HTTPS to an unencrypted address, which is not followed.", false, 1, (unsigned)status);
        if (++hops > 10) throw NetworkError("Too many redirects (more than 10).", false, 1, (unsigned)status);
        // As browsers and both transports do: a POST redirected by 301, 302 or 303 becomes a GET.
        if (body && status != 307 && status != 308) body = nullptr;
        current = next;
    }
}
std::unique_ptr<Request> get(const std::string& url, const std::wstring& headers, std::atomic_bool& stop, bool redirects = true, unsigned wait = 10) {
    for (unsigned attempt = 0;; ++attempt) {
        cancelled(stop);
        try {
            return open(url, headers, stop, redirects, wait);
        } catch (const SignInNeeded& e) {
            // Not retried: the same request would be refused again.
            LOG_ERROR("Request failed: %s: %s", logUrl(url).c_str(), e.what());
            throw;
        } catch (const NetworkError& e) {
            if (!e.retry || attempt == 3) {
                LOG_ERROR("Request failed: %s: %s", logUrl(url).c_str(), e.what());
                throw;
            }
            const auto pause = std::max(e.wait, 1u << attempt);
            LOG_WARN("Request failed (attempt %u of 4), retrying in %us: %s: %s", attempt + 1, pause, logUrl(url).c_str(), e.what());
            delay(pause, stop);
        }
    }
}
void validateRange(Request& r, uint64_t first, uint64_t last, uint64_t total) {
    auto expected = L"bytes " + std::to_wstring(first) + L"-" + std::to_wstring(last) + L"/" + std::to_wstring(total);
    if (r.status != 206 || header(r.request, WINHTTP_QUERY_CONTENT_RANGE) != expected)
        throw NetworkError("Server returned an unexpected byte range. Reload the item and retry with one connection.", false);
}
void regular(const Path& path) {
    platform::regular(path);
}
std::vector<Range> queuedRanges(uint64_t size) {
    std::vector<Range> result;
    // Range identities depend on size, never on the chosen worker count.
    // Usually 1-4 MiB; grow for huge files to bound the number of staging files.
    auto ceilDiv = [](uint64_t n, uint64_t d) {
        return n / d + (n % d != 0);
    };
    auto chunk = std::max({uint64_t(1024 * 1024), std::min<uint64_t>(4 * 1024 * 1024, ceilDiv(size, 32)), ceilDiv(size, 16384)});
    for (uint64_t first = 0; first < size;) {
        auto length = std::min(chunk, size - first);
        result.push_back({first, first + length - 1});
        first += length;
    }
    return result;
}
}
void setUserAgent(const std::string& value) {
    if (value.size() > 512) throw std::runtime_error("The user agent is longer than 512 characters.");
    for (unsigned char c : value)
        if (c < 0x20 || c == 0x7F) throw std::runtime_error("The user agent contains a control character.");
    std::lock_guard lock(agentLock);
    agentValue = value.empty() ? std::string(defaultUserAgent) : value;
}
std::string userAgent() {
    std::lock_guard lock(agentLock);
    return agentValue;
}
LoadWatch::LoadWatch(const std::atomic_bool& stop, LoadProgress& progress)
    : stop_(&stop) {
    std::lock_guard lock(watchLock);
    watched[stop_] = &progress;
    watches_ = watched.size();
}
LoadWatch::~LoadWatch() {
    std::lock_guard lock(watchLock);
    watched.erase(stop_);
    watches_ = watched.size();
}
// Header text arrives as Latin-1 characters; servers often send UTF-8 bytes in it.
std::string headerUtf8(const std::wstring& value) {
    if (std::all_of(value.begin(), value.end(), [](wchar_t c) {
            return c <= 0xFF;
        })) {
        std::string bytes;
        for (auto c : value) bytes += (char)(unsigned char)c;
        try {
            wide(bytes);
            return bytes;
        } catch (const std::exception&) {}
    }
    return utf8(value);
}
namespace {
// The whole response body, read until the server ends it; past limit bytes it fails with tooLarge.
std::string readBody(Request& r, std::atomic_bool& stop, size_t limit, const char* tooLarge) {
    std::string body;
    char buffer[65536];
    auto* progress = progressFor(stop);
    for (;;) {
        cancelled(stop);
        auto n = r.read(buffer, sizeof buffer);
        if (!n) break;
        if (body.size() + n > limit) throw std::runtime_error(tooLarge);
        body.append(buffer, n);
        if (progress) progress->bytes += n;
    }
    return body;
}
// The Content-Type's media type ("text/html"), in lower case and without parameters.
std::string mediaType(Request& r) {
    auto type = headerUtf8(header(r.request, WINHTTP_QUERY_CONTENT_TYPE));
    return lower(trim(type.substr(0, type.find(';'))));
}
// Content-Length, when the server sent it as a plain number.
bool contentLength(Request& r, uint64_t& out) {
    auto length = header(r.request, WINHTTP_QUERY_CONTENT_LENGTH);
    if (length.empty() || length.size() > 19 || length.find_first_not_of(L"0123456789") != std::wstring::npos) return false;
    out = std::stoull(length);
    return true;
}
// The total size from a "bytes 0-0/<size>" Content-Range (the answer to a one-byte probe).
bool probedSize(Request& r, uint64_t& out) {
    static const std::wregex range(L"bytes 0-0/([0-9]{1,19})");
    std::wsmatch m;
    auto value = header(r.request, WINHTTP_QUERY_CONTENT_RANGE);
    if (!std::regex_match(value, m, range)) return false;
    out = std::stoull(m[1]);
    return true;
}
}
Item loadDirectLink(const std::string& url, std::atomic_bool& stop) {
    if (!isHttpUrl(url) && !ftp::isFtpUrl(url)) throw std::runtime_error("Paste an HTTP, HTTPS or FTP link to a file.");
    auto r = get(url, L"Range: bytes=0-0\r\n", stop);
    countRequest(stop);
    File file;
    file.sizeKnown = r->status == 206 ? probedSize(*r, file.size) : contentLength(*r, file.size);
    auto type = mediaType(*r);
    auto name = filenameFromDisposition(headerUtf8(header(r->request, WINHTTP_QUERY_CONTENT_DISPOSITION)));
    auto finalUrl = r->url();
    if (name.empty()) name = filenameFromUrl(finalUrl.empty() ? url : finalUrl);
    if (name.empty()) name = filenameFromUrl(url);
    const auto lowerName = lower(name);
    if (type == "text/html" && !lowerName.ends_with(".html") && !lowerName.ends_with(".htm"))
        throw WebPageError("This link opens a web page, not a file. Copy the file's direct download link instead.");
    file.name = safeFilename(name);
    file.format = type;
    Item item;
    item.title = file.name;
    item.selectedFile = file.name;
    item.sourceUrl = ftp::withoutUserInfo(url); // an FTP address may carry a password; it is never stored
    item.files.push_back(std::move(file));
    return item;
}
namespace {
Response respond(Request& r, const std::string& requested, std::atomic_bool& stop) {
    Response out;
    out.url = r.url();
    if (out.url.empty()) out.url = requested;
    out.type = mediaType(r);
    out.disposition = headerUtf8(header(r.request, WINHTTP_QUERY_CONTENT_DISPOSITION));
    out.sizeKnown = contentLength(r, out.size);
    out.file = !(out.type.starts_with("text/") || out.type.find("json") != std::string::npos || out.type.find("xml") != std::string::npos);
    if (!out.file) out.body = readBody(r, stop, 16 * 1024 * 1024, "The site's page is too large (16 MiB limit).");
    return out;
}
}
Response fetch(const std::string& url, std::atomic_bool& stop, const std::string& headers, unsigned wait) {
    auto r = get(url, wide(headers), stop, true, wait);
    auto response = respond(*r, url, stop);
    countRequest(stop);
    return response;
}
Response post(const std::string& url, const std::string& body, std::atomic_bool& stop, const std::string& headers) {
    auto r = open(url, wide(headers), stop, true, 10, &body);
    auto response = respond(*r, url, stop);
    countRequest(stop);
    return response;
}
Item loadHubItem(const std::string& input, std::atomic_bool& stop) {
    auto link = parseHubLink(input);
    // A file link lists only its own folder; a tree link lists that folder; otherwise the whole repo.
    auto folder = link.file ? link.path.substr(0, link.path.rfind('/') == std::string::npos ? 0 : link.path.rfind('/')) : link.path;
    std::string page = "https://huggingface.co/api/" + link.kind + "/" + link.repo + "/tree/" + encode(link.revision) + (folder.empty() ? "" : "/" + encode(folder, true)) + "?recursive=true";
    auto readAll = [&](Request& r) {
        return readBody(r, stop, 64 * 1024 * 1024, "Repository file list is too large (64 MiB limit).");
    };
    // The Hub answers 401 for both missing and private repos, so a 401 is a sign-in question: with no token
    // saved the window offers to add one, with one saved it was not accepted. A 403 with a token saved means
    // the account has not accepted a gated repository's terms.
    const bool hasToken = hasSignIn("huggingface.co", 0);
    auto hubGet = [&](const std::string& address) {
        try {
            return get(address, L"", stop);
        } catch (const SignInNeeded&) {
            throw SignInNeeded("huggingface.co", hasToken, false);
        } catch (const NetworkError& e) {
            if (e.status == 403) {
                if (!hasToken) throw SignInNeeded("huggingface.co", false, false);
                throw std::runtime_error("This repository is gated: accept its terms on huggingface.co while signed in to the account this token belongs to.");
            }
            throw;
        }
    };
    // Check access first: gated repos list their files publicly but refuse every download.
    try {
        auto info = hubGet("https://huggingface.co/api/" + link.kind + "/" + link.repo);
        const auto infoBody = readAll(*info);
        countRequest(stop);
        // With a token saved the account may well have access: the listing goes on, and a 403 there says what to do.
        if (hubRepoGated(infoBody) && !hasToken) throw SignInNeeded("huggingface.co", false, false);
    } catch (const NetworkError& e) {
        if (e.status == 404)
            throw std::runtime_error("This Hugging Face repository was not found, or it is private.");
        throw;
    }
    Item item;
    item.id = item.title = link.repo;
    for (unsigned pages = 0; !page.empty(); ++pages) {
        if (pages == 1000) throw std::runtime_error("This repository lists too many files (over 1000 pages).");
        auto r = hubGet(page);
        auto pageBody = readAll(*r);
        countRequest(stop);
        auto files = parseHubTree(pageBody);
        item.files.insert(item.files.end(), std::make_move_iterator(files.begin()), std::make_move_iterator(files.end()));
        page = hubNextPage(headerUtf8(header(r->request, HEADER_LINK)));
    }
    if (item.files.empty()) throw std::runtime_error("This repository has no files at that location.");
    if (link.file) {
        if (std::none_of(item.files.begin(), item.files.end(), [&](const File& f) {
                return f.name == link.path;
            }))
            throw std::runtime_error("That file is not in the repository: " + link.path);
        item.selectedFile = link.path;
    }
    // Revisions such as refs/pr/1 are one URL segment, so their slashes are encoded.
    item.downloadBase = "https://huggingface.co/" + (link.kind == "models" ? std::string() : link.kind + "/") + link.repo + "/resolve/" + encode(link.revision) + "/";
    return item;
}
Item loadGitHubRelease(const std::string& input, std::atomic_bool& stop) {
    auto link = parseGitHubLink(input);
    auto api = "https://api.github.com/repos/" + link.owner + "/" + link.repo + "/releases/" + (link.tag.empty() ? std::string("latest") : "tags/" + encode(link.tag, true));
    try {
        auto r = get(api, L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n", stop);
        auto releaseBody = readBody(*r, stop, 16 * 1024 * 1024, "GitHub release data is too large (16 MiB limit).");
        countRequest(stop);
        auto item = parseGitHubRelease(releaseBody, link);
        if (item.files.empty()) throw std::runtime_error("This release has no files.");
        return item;
    } catch (const SignInNeeded&) {
        throw SignInNeeded("github.com", hasSignIn("github.com", 0), false);
    } catch (const NetworkError& e) {
        if (e.status == 404)
            throw std::runtime_error(link.tag.empty() ? "This repository has no published releases, or it is private." : "Release not found. Check the tag, or the repository may be private.");
        if (e.status == 403 || e.status == 429) {
            if (hasSignIn("github.com", 0))
                throw std::runtime_error("GitHub refused this request for the saved token (it may lack access to this repository, or its rate limit was reached).");
            // Anonymous API use is limited to 60 requests an hour per network address.
            throw std::runtime_error("GitHub's limit for release lookups without signing in (60 an hour) was reached. Try again later.");
        }
        throw;
    }
}
Item loadItem(const std::string& input, std::atomic_bool& stop) {
    if (isHubLink(input)) return loadHubItem(input, stop);
    if (isGitHubLink(input)) return loadGitHubRelease(input, stop);
    if (isHostLink(input)) return loadHostItem(input, stop);
    if (isLockerLink(input)) return loadLockerItem(input, stop);
    // FTP links, including ftp://user@host ones, which loadFolderItem explains it cannot sign in to.
    const auto url = trim(input);
    if (isWaybackLink(input) || lower(url.substr(0, 6)) == "ftp://") return loadFolderItem(input, stop);
    if (!isArchiveLink(input) && isHttpUrl(input)) {
        try {
            return loadDirectLink(url, stop);
        }
        // A web page can be a folder listing ("Index of /files/"), whose files are listed instead.
        catch (const WebPageError&) {
            return loadFolderItem(url, stop);
        }
    }
    auto link = parseLink(input);
    auto r = get("https://archive.org/metadata/" + encode(link.id), L"", stop);
    auto metadata = readBody(*r, stop, 64 * 1024 * 1024, "Item metadata is too large (64 MiB limit).");
    countRequest(stop);
    auto item = parseMetadata(link.id, metadata);
    item.selectedFile = link.file;
    return item;
}
std::vector<ServerSpeed> measureServers(const File& file, const std::vector<std::string>& urls, std::atomic_bool& stop) {
    if (!file.sizeKnown || !file.size) throw std::runtime_error("Choose a non-empty file with a known size to test speeds.");
    std::vector<ServerSpeed> results(std::min<size_t>(urls.size(), 3));
    std::vector<std::jthread> probes;
    for (size_t i = 0; i < results.size(); ++i) probes.emplace_back([&, i] {
        auto& result = results[i];
        result.url = urls[i];
        try {
            cancelled(stop);
            result.host = utf8(Request::Url(urls[i]).host);
            uint64_t sample = std::min<uint64_t>(file.size, 256 * 1024);
            auto start = std::chrono::steady_clock::now();
            auto request = open(urls[i], L"Range: bytes=0-" + std::to_wstring(sample - 1) + L"\r\n", stop, i == 0);
            auto& r = *request;
            validateRange(r, 0, sample - 1, file.size);
            uint64_t got = 0;
            char buffer[32768];
            while (got < sample) {
                cancelled(stop);
                auto n = r.read(buffer, (DWORD)std::min<uint64_t>(sizeof buffer, sample - got));
                if (!n) throw std::runtime_error("Incomplete speed sample");
                got += n;
                if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) throw std::runtime_error("Speed sample timed out");
            }
            result.host = r.host();
            result.etag = header(r.request, WINHTTP_QUERY_ETAG);
            if (result.etag.starts_with(L"W/")) result.etag.clear();
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            result.bytesPerSecond = sample / std::max(seconds, .001);
        } catch (const std::exception& e) {
            result.error = e.what();
        }
    });
    for (auto& thread : probes) thread.join();
    cancelled(stop);
    return results;
}
void download(const std::string& url, const File& file, const Path& destination, unsigned connections, std::atomic_bool& stop, const Report& report, const std::vector<std::string>& mirrors, const std::string& forcedSource, const SpeedReport& speeds, const std::string& identity) {
    std::string selectedSource = forcedSource.empty() ? url : forcedSource;
    // The Wayback Machine's cache keeps one answer per address for half an hour, whatever byte range
    // was asked for, and gives it to every later request: one range request there cuts later downloads
    // of the file short. Its files are read whole on one connection, and a cut-down answer already
    // cached (from anyone's range request) is avoided through the archive's other raw form (if_).
    const bool wayback = isWaybackLink(selectedSource);
    // File lockers hand out a new download address on each visit; their page link names the file in the resume state.
    const std::string& key = identity.empty() ? url : identity;
    const bool redirects = forcedSource.empty() || forcedSource == url;
    cancelled(stop);
    if (std::filesystem::exists(destination)) throw std::runtime_error("Destination already exists; it was left unchanged.");
    for (auto p = destination; !p.empty() && p != p.root_path(); p = p.parent_path()) regular(p);
    std::filesystem::create_directories(destination.parent_path());
    uint64_t size = file.size;
    bool known = file.sizeKnown, ranged = false;
    std::wstring etag, lastModified;
    if (!known || size) {
        auto probe = get(selectedSource, wayback ? L"" : L"Range: bytes=0-0\r\n", stop, redirects);
        if (wayback && probe->status == 206) {
            auto alternate = waybackAlternateUrl(selectedSource);
            LOG_WARN("The Wayback Machine sent part of %s to a whole-file request (a cached range answer); trying %s", logUrl(selectedSource).c_str(), alternate.empty() ? "nothing else" : logUrl(alternate).c_str());
            if (!alternate.empty()) {
                selectedSource = alternate;
                probe = get(selectedSource, L"", stop, redirects);
            }
            if (probe->status == 206) throw NetworkError("The Wayback Machine is sending only part of this file for now (its cache holds a partial copy for up to 30 minutes). Retry later.", false);
        }
        ranged = !wayback && probe->status == 206;
        if (ranged) {
            uint64_t actual = 0;
            if (!probedSize(*probe, actual)) throw std::runtime_error("Invalid server range response");
            if (known && actual != size) throw std::runtime_error("File size changed. Reload the item.");
            size = actual;
            known = true;
        } else {
            auto length = header(probe->request, WINHTTP_QUERY_CONTENT_LENGTH);
            if (!length.empty()) {
                if (length.size() > 19 || length.find_first_not_of(L"0123456789") != std::wstring::npos)
                    throw NetworkError("The server sent an invalid file size (Content-Length).", false);
                auto actual = std::stoull(length);
                if (known && actual != size) throw std::runtime_error("File size changed. Reload the item.");
                size = actual;
                known = true;
            }
        }
        etag = header(probe->request, WINHTTP_QUERY_ETAG);
        if (etag.starts_with(L"W/")) etag.clear();
        lastModified = header(probe->request, WINHTTP_QUERY_LAST_MODIFIED);
        LOG_INFO("Probe %s: HTTP %lu, size %s, byte ranges %s, ETag %s, Last-Modified %s", logUrl(selectedSource).c_str(), (unsigned long)probe->status,
                 known ? std::to_string(size).c_str() : "unknown", ranged ? "yes" : "no", etag.empty() ? "none" : "strong", lastModified.empty() ? "none" : "yes");
    }
    // Only reuse partial data when a content hash or a server validator (strong ETag, else Last-Modified) identifies it.
    const std::wstring validator = resumeValidator(etag, lastModified);
    const bool sha1Hash = isHex(file.sha1, 40);
    const bool sha256Hash = isHex(file.sha256, 64);
    bool resumable = ranged && (sha1Hash || sha256Hash || !validator.empty());
    // Without a stable identity, combining separate responses could mix file versions.
    if (!resumable) ranged = false;
    connections = std::clamp(connections, 1u, 16u);
    auto ranges = ranged ? queuedRanges(size) : split(size, 1);
    struct Source {
        std::string url, host;
        std::wstring tag;
        double rate = 0;
    };
    // With a hash the hash is the identity: the tag stays the strong ETag only, as before Last-Modified support.
    std::vector<Source> sources{{selectedSource, utf8(Request::Url(selectedSource).host), sha1Hash || sha256Hash ? etag : validator, 0}};
    // Measure a small sample before choosing a storage server. Cross-server use
    // requires the Archive's whole-file hash; ETags are not portable between hosts.
    if (forcedSource.empty() && ranged && size >= 1024 * 1024 && !mirrors.empty() && sha1Hash) {
        if (report) report({0, size, "Checking mirror speeds (HTTPS)", 0, ""});
        std::vector<std::string> candidates{url};
        for (auto& mirror : mirrors)
            if (candidates.size() < 3 && std::find(candidates.begin(), candidates.end(), mirror) == candidates.end()) candidates.push_back(mirror);
        File sampleFile = file;
        sampleFile.size = size;
        sampleFile.sizeKnown = true;
        auto measured = measureServers(sampleFile, candidates, stop);
        if (speeds) speeds(measured);
        for (auto& result : measured) {
            if (result.error.empty())
                LOG_INFO("Mirror %s: %.0f KiB/s", result.host.c_str(), result.bytesPerSecond / 1024.0);
            else
                LOG_WARN("Mirror %s failed the speed test: %s", result.host.c_str(), result.error.c_str());
        }
        std::vector<Source> tested;
        for (auto& result : measured)
            if (result.error.empty()) tested.push_back({result.url, result.host, result.etag, result.bytesPerSecond});
        std::stable_sort(tested.begin(), tested.end(), [](const Source& a, const Source& b) {
            return a.rate > b.rate;
        });
        if (!tested.empty()) {
            if (std::none_of(tested.begin(), tested.end(), [&](const Source& s) {
                    return s.url == url;
                })) tested.push_back(sources[0]);
            sources = std::move(tested);
        }
    }
    Path parts = destination;
    parts += L".archive-parts";
    regular(parts);
    bool created = std::filesystem::create_directory(parts);
    auto ownerPath = parts / L"owner.txt";
    regular(ownerPath);
    const std::string owner = "SkeinDownloader staging v1";
    if (created) {
        std::ofstream out(ownerPath);
        out << owner;
        out.flush();
        if (!out) throw std::runtime_error("Cannot initialize download staging folder");
    } else {
        std::ifstream in(ownerPath);
        std::string value;
        std::getline(in, value);
        if (value != owner) {
            LOG_ERROR("Staging folder %s has owner \"%s\", not this app", pathUtf8(parts).c_str(), value.c_str());
            throw std::runtime_error("The partial-download folder already exists and is not owned by this app. Choose another destination.");
        }
    }
    auto lockPath = parts / L"lock";
    regular(lockPath);
    platform::FileLock fileLock(lockPath);
    // The numbered part files of the older layouts, converted or deleted below.
    auto partPath = [&](size_t i) {
        return parts / (std::to_wstring(i) + L".part");
    };
    const auto statePath = parts / L"state.json";
    const auto stateTemp = parts / L"state.json.tmp";
    const auto dataPath = parts / L"data.tmp";
    regular(statePath);
    regular(stateTemp);
    regular(dataPath);
    regular(parts / L"assembled.tmp");
    nlohmann::json previous;
    try {
        std::ifstream in(statePath);
        in >> previous;
    } catch (...) {}
    const bool hashVerified = sha1Hash || sha256Hash;
    // "single-v1": data.tmp plus the bytes done in each range ("done"), saved at checkpoints. The older layouts
    // kept one file per part: "queued-v1" for the parts of queuedRanges, and no layout key for split(size, parts).
    std::string previousLayout;
    if (previous.is_object() && previous.contains("layout") && previous["layout"].is_string()) previousLayout = previous["layout"].get<std::string>();
    // The older layouts reused part files identified only by a validator when the last run stopped normally
    // and saved its parts to disk first ("clean"): after a crash a part file can be longer than the data that reached the disk.
    const bool previousClean = previous.is_object() && previous.value("clean", false);
    nlohmann::json previousDone;
    if (previous.is_object()) {
        previous.erase("clean");
        if (previous.contains("done")) previousDone = previous["done"];
        previous.erase("done");
    }
    size_t oldCount = 0;
    if (previous.is_object() && previous.contains("parts") && previous["parts"].is_number_unsigned()) {
        auto count = previous["parts"].get<uint64_t>();
        if (count <= 16384) oldCount = (size_t)count;
    }
    const size_t cleanupCount = std::max({size_t(8), oldCount, ranges.size()});
    for (size_t i = 0; i < cleanupCount; ++i) regular(partPath(i));
    nlohmann::json state = {{"url", key}, {"size", size}, {"known", known}, {"sha1", file.sha1}, {"etag", utf8(etag)}, {"parts", ranges.size()}, {"ranged", ranged}, {"layout", "single-v1"}};
    // Recorded only when present so manifests written before SHA-256 support still match.
    if (!file.sha256.empty()) state["sha256"] = file.sha256;
    // Last-Modified, when it is the validator (no strong ETag); recorded only when present for the same reason.
    if (etag.empty() && !lastModified.empty()) state["modified"] = utf8(lastModified);
    if (!sha1Hash && !sha256Hash) {
        state["source"] = identity.empty() ? selectedSource : identity;
        if (previous.is_object() && !previous.contains("source")) previous["source"] = url;
    }
    // Server-specific ETags can change across redirects even when the content hash
    // is identical. Preserve verified-identity partials, including older manifests.
    if (previous.is_object() && (sha1Hash || sha256Hash)) {
        previous["etag"] = state["etag"];
        if (state.contains("modified"))
            previous["modified"] = state["modified"];
        else
            previous.erase("modified");
    }
    // What the last run left is reused only when it is the same file version: data.tmp with its saved counts
    // (single-v1), or the part files of an older layout under the rules of the version that wrote them.
    // Nothing; data.tmp and its counts (Data); the part files of an older layout, converted (Parts).
    enum class Reuse { Nothing,
                       Data,
                       Parts };
    Reuse reuse = Reuse::Nothing;
    const char* discardReason = "the server cannot resume this file";
    // The bytes done in each range at the start (at least one entry: a transfer of unknown size counts in entry 0).
    std::vector<uint64_t> startDone(std::max<size_t>(ranges.size(), 1), 0);
    std::vector<Range> oldRanges;
    if (resumable && previous.is_object()) {
        if (previousLayout == "single-v1") {
            std::error_code ec;
            const auto dataSize = std::filesystem::file_size(dataPath, ec);
            auto validCounts = previousDone.is_array() && previousDone.size() == ranges.size();
            for (size_t i = 0; validCounts && i < ranges.size(); ++i) {
                validCounts = previousDone[i].is_number_unsigned() && previousDone[i].get<uint64_t>() <= ranges[i].last - ranges[i].first + 1;
                if (validCounts) startDone[i] = previousDone[i].get<uint64_t>();
            }
            if (previous != state)
                discardReason = "the file or its source changed";
            else if (!validCounts)
                discardReason = "its saved progress is not valid";
            else if (ec || dataSize < size)
                discardReason = "its data file is missing or shorter than the file";
            else
                reuse = Reuse::Data;
        } else {
            auto expected = state;
            if (previousLayout == "queued-v1") {
                expected["layout"] = "queued-v1";
                oldRanges = queuedRanges(size);
            } else if (previousLayout.empty() && oldCount > 0 && oldCount <= 8) {
                expected.erase("layout");
                expected["parts"] = oldCount;
                oldRanges = split(size, (unsigned)oldCount);
            }
            if (oldRanges.empty() || previous != expected)
                discardReason = "the file or its source changed";
            else if (!hashVerified && !previousClean)
                discardReason = "the last run did not stop normally, so its data may not have reached the disk";
            else
                reuse = Reuse::Parts;
        }
    }
    if (reuse != Reuse::Data) std::fill(startDone.begin(), startDone.end(), 0);
    if (reuse == Reuse::Nothing && previous.is_object()) LOG_INFO("Discarding partial data for %s: %s", pathUtf8(destination).c_str(), discardReason);
    // One file for the whole download, written at each range's offset by all connections. It is kept only with its counts.
    std::unique_ptr<platform::DataFile> data;
    if (reuse != Reuse::Data) std::filesystem::remove(dataPath);
    if (reuse == Reuse::Parts) {
        // Each old part is copied to its offset, flushed and deleted before the next, so the conversion needs no extra disk.
        data = std::make_unique<platform::DataFile>(dataPath, true, size);
        std::vector<uint64_t> lengths(oldRanges.size(), 0);
        for (size_t i = 0; i < oldRanges.size(); ++i) {
            const auto path = partPath(i);
            if (!std::filesystem::exists(path)) continue;
            const auto length = std::filesystem::file_size(path);
            // A part saved normally never runs past its range; a longer one is not what the state says, so it is not valid.
            if (length > oldRanges[i].last - oldRanges[i].first + 1) {
                std::filesystem::remove(path);
                continue;
            }
            {
                std::ifstream in(path, std::ios::binary);
                char buffer[65536];
                for (uint64_t copied = 0; copied < length;) {
                    in.read(buffer, (std::streamsize)std::min<uint64_t>(sizeof buffer, length - copied));
                    const auto n = (size_t)in.gcount();
                    if (!n) throw std::runtime_error("Cannot read partial file");
                    data->write(oldRanges[i].first + copied, buffer, n);
                    copied += n;
                }
            }
            data->sync();
            std::filesystem::remove(path);
            lengths[i] = length;
        }
        startDone = convertedDone(oldRanges, lengths, ranges);
        startDone.resize(std::max<size_t>(ranges.size(), 1), 0);
        LOG_INFO("Converted the partial data of %s from %zu part file(s) to one data file", pathUtf8(destination).c_str(), oldRanges.size());
    }
    for (size_t i = 0; i < cleanupCount; ++i) std::filesystem::remove(partPath(i));
    std::filesystem::remove(parts / L"assembled.tmp");
    std::filesystem::remove(stateTemp);
    std::mutex reportLock;
    std::atomic_uint64_t done = 0;
    // The bytes done in each range; a connection adds to its range's count after each write.
    std::vector<std::atomic_uint64_t> counts(startDone.size());
    for (size_t i = 0; i < startDone.size(); ++i) {
        counts[i] = startDone[i];
        done += startDone[i];
    }
    // Serializes checkpoints, and guards data, counts, ranges and state while the checkpoint timer runs.
    std::mutex checkpointLock;
    // Saves the progress for a later run: the counts are copied first, then the data is flushed, then the state is
    // replaced atomically, so the saved counts are never ahead of the data on disk. Nothing to do once data is closed.
    auto checkpoint = [&] {
        std::lock_guard guard(checkpointLock);
        if (!data) return;
        auto saved = state;
        saved["done"] = nlohmann::json::array();
        for (size_t i = 0; i < ranges.size(); ++i) saved["done"].push_back(counts[i].load());
        data->sync();
        {
            std::ofstream out(stateTemp, std::ios::binary | std::ios::trunc);
            out << saved.dump();
            out.flush();
            if (!out) throw std::runtime_error("Cannot save resume state");
        }
        platform::replace(stateTemp, statePath);
    };
    // Discards the staged data (restart, or a retry of a transfer without ranges): a new, empty data.tmp.
    auto discardData = [&] {
        std::lock_guard guard(checkpointLock);
        data.reset();
        std::filesystem::remove(dataPath);
        data = std::make_unique<platform::DataFile>(dataPath, known, known ? size : 0);
    };
    auto checkFreeSpace = [&] {
        if (!known) return;
        // The data file is written in place, so only the bytes not yet done need room.
        const uint64_t needed = size - std::min<uint64_t>(done.load(), size);
        std::error_code spaceError;
        const auto space = std::filesystem::space(destination.parent_path(), spaceError);
        if (!spaceError) {
            auto message = freeSpaceError(needed, space.available);
            if (!message.empty()) {
                LOG_ERROR("%s (%s)", message.c_str(), pathUtf8(destination).c_str());
                throw std::runtime_error(message);
            }
        }
    };
    // A converted folder is saved in this layout at once: its part files are gone.
    if (reuse == Reuse::Parts) checkpoint();
    checkFreeSpace();
    if (!data) data = std::make_unique<platform::DataFile>(dataPath, known, known ? size : 0);
    // The state in this layout, before any new data is written.
    checkpoint();
    // Set when a connection finds that the parts cannot be combined, so that the others stop mid-part.
    std::atomic_bool restartAsked = false;
    std::atomic_uint active = 0;
    // Byte progress (throttle) is sent at most every 100 ms across all connections; the window
    // shows it four times a second, and each report takes the queue's lock.
    std::chrono::steady_clock::time_point lastReport{};
    auto notify = [&](const std::string& status, const std::string& host = "", bool throttle = false) {
        std::lock_guard guard(reportLock);
        const auto now = std::chrono::steady_clock::now();
        if (throttle && now - lastReport < std::chrono::milliseconds(100)) return;
        lastReport = now;
        if (report) report({done.load(), known ? size : 0, status, active.load(), host.empty() ? sources[0].host : host});
    };
    auto receive = [&](size_t i, bool useRange, size_t& sourceIndex) {
        uint64_t length = known ? (useRange ? ranges[i].last - ranges[i].first + 1 : size) : UINT64_MAX;
        // Where this range starts in the data file; a transfer without ranges writes the whole file from 0.
        const uint64_t first = useRange ? ranges[i].first : 0;
        uint64_t offset = counts[i].load();
        for (unsigned attempt = 0;; ++attempt) {
            cancelled(stop);
            if (known && offset == length) break;
            try {
                const auto& source = sources[sourceIndex];
                std::wstring headers;
                if (useRange) {
                    headers = L"Range: bytes=" + std::to_wstring(ranges[i].first + offset) + L"-" + std::to_wstring(ranges[i].last) + L"\r\n";
                    if (!source.tag.empty()) headers += L"If-Range: " + source.tag + L"\r\n";
                } else if (offset) {
                    // The whole file again: a new, empty data file, so a shorter answer of unknown size leaves no old tail.
                    done -= offset;
                    offset = 0;
                    counts[i] = 0;
                    discardData();
                }
                // One of the server's connection slots, from before the request until its response has been read
                // or has failed; it is given back before a retry waits, so a waiting retry holds none.
                HostSlot slot(lower(utf8(Request::Url(source.url).host)), stop, [&] {
                    notify("Waiting for a free connection", source.host);
                });
                auto r = get(source.url, headers, stop, redirects);
                if (useRange) {
                    if (r->status == 200) {
                        restartAsked = true;
                        throw RestartWhole("the server answered a range request with the whole file");
                    }
                    validateRange(*r, ranges[i].first + offset, ranges[i].last, size);
                    // Without a hash only the validator proves that this answer is the file the other parts came from.
                    // A server may leave the headers out of a 206, which proves nothing either way.
                    if (!hashVerified && !source.tag.empty()) {
                        if (validatorChanged(source.tag, header(r->request, WINHTTP_QUERY_ETAG), header(r->request, WINHTTP_QUERY_LAST_MODIFIED))) {
                            restartAsked = true;
                            throw RestartWhole("the file changed on the server during the download");
                        }
                    }
                } else if (r->status != 200)
                    throw std::runtime_error("Unexpected partial response");
                auto effectiveHost = r->host();
                struct Active {
                    std::atomic_uint& count;
                    Active(std::atomic_uint& c)
                        : count(c) { ++count; }
                    ~Active() { --count; }
                } activeRequest(active);
                char buffer[65536];
                for (;;) {
                    cancelled(stop);
                    if (restartAsked) throw RestartWhole("another connection found that the parts cannot be combined");
                    DWORD n = r->read(buffer, sizeof buffer);
                    if (!n) break;
                    if (known && n > length - offset) throw std::runtime_error("Server sent more bytes than requested");
                    data->write(first + offset, buffer, n);
                    // Counted only once written, so a checkpoint never records bytes that are not in the file.
                    offset += n;
                    counts[i] = offset;
                    done += n;
                    notify("Downloading", effectiveHost, true);
                }
                if (known && offset != length) throw NetworkError("Server closed the connection before the file was complete");
                break;
            } catch (const NetworkError& e) {
                const auto& failedHost = sources[sourceIndex].host;
                if (attempt == 3 || (!e.retry && sources.size() == 1)) {
                    LOG_ERROR("Part %zu from %s failed after %u attempts: %s", i, failedHost.c_str(), attempt + 1, e.what());
                    throw;
                }
                LOG_WARN("Part %zu from %s failed (attempt %u): %s; %s", i, failedHost.c_str(), attempt + 1, e.what(),
                         sources.size() > 1 ? "trying another source" : "retrying");
                sourceIndex = (sourceIndex + 1) % sources.size();
                notify(sources.size() > 1 ? "Trying another HTTPS source" : "Retrying after connection error");
                if (e.retry) delay(std::max(e.wait, 1u << attempt), stop);
            }
        }
    };
    {
        std::string hosts;
        for (auto& s : sources) hosts += (hosts.empty() ? "" : ", ") + s.host;
        LOG_INFO("Downloading %s: %s, %zu part(s), %u connection(s), %llu bytes already on disk, sources %s", pathUtf8(destination).c_str(),
                 ranged ? "parallel" : "single connection", ranges.size(), ranged ? connections : 1u, (unsigned long long)done.load(), hosts.c_str());
    }
    notify(ranged ? "Starting parallel download" : "Starting single connection");
    // Checkpoints every interval while the transfer runs. Joined (stopTimer) before the function returns or throws
    // and before data.tmp is closed, published or deleted.
    std::jthread checkpointTimer;
    auto stopTimer = [&] {
        checkpointTimer.request_stop();
        if (checkpointTimer.joinable()) checkpointTimer.join();
    };
    try { // until the file is published; a stop for any reason ends with a checkpoint
        checkpointTimer = std::jthread([&](std::stop_token token) {
            std::mutex waitLock;
            std::condition_variable_any wake;
            std::unique_lock lock(waitLock);
            for (;;) {
                wake.wait_for(lock, token, checkpointInterval(), [] {
                    return false;
                });
                if (token.stop_requested()) return;
                try {
                    checkpoint();
                } catch (const std::exception& e) {
                    LOG_WARN("Could not save the progress of %s (%s); retrying at the next checkpoint", pathUtf8(destination).c_str(), e.what());
                }
            }
        });
        auto transfer = [&] {
            if (ranged && size) {
                std::mutex errorLock;
                std::exception_ptr error;
                std::atomic_size_t next = 0;
                std::atomic_bool failed = false;
                std::vector<std::jthread> threads;
                for (size_t worker = 0; worker < std::min<size_t>(connections, ranges.size()); ++worker) threads.emplace_back([&] {
                    size_t sourceIndex = 0;
                    try {
                        while (!failed && !stop) {
                            auto i = next.fetch_add(1);
                            if (i >= ranges.size()) break;
                            receive(i, true, sourceIndex);
                        }
                    } catch (...) {
                        failed = true;
                        std::lock_guard lock(errorLock);
                        if (!error) error = std::current_exception();
                    }
                });
                for (auto& t : threads) t.join();
                if (error) std::rethrow_exception(error);
            } else if (!known || size) {
                size_t sourceIndex = 0;
                receive(0, false, sourceIndex);
            }
        };
        // The one place that gives up on the parts: once per call, the staged data is deleted and the file is
        // fetched whole on one connection, which cannot mix two versions of it.
        for (bool restarted = false;; restarted = true) {
            try {
                transfer();
                break;
            } catch (const RestartWhole& e) {
                if (restarted) throw;
                LOG_WARN("Restarting %s on one connection without resume: %s", pathUtf8(destination).c_str(), e.what());
                {
                    // The connections have ended; the timer may be checkpointing, so the layout changes under its lock.
                    std::lock_guard guard(checkpointLock);
                    ranged = false;
                    resumable = false;
                    ranges = split(size, 1);
                    counts = std::vector<std::atomic_uint64_t>(std::max<size_t>(ranges.size(), 1));
                    done = 0;
                    state["ranged"] = false;
                    state["parts"] = ranges.size();
                }
                discardData();
                restartAsked = false;
                checkpoint();
                checkFreeSpace();
                notify("Starting single connection");
            }
        }
        stopTimer();
        // The last checkpoint: the data reaches the disk before it is checked and published.
        checkpoint();
        cancelled(stop);
        {
            std::lock_guard guard(checkpointLock);
            // Every byte is written: the finished file must not stay sparse after it is published.
            if (data && known && done.load() == size) data->complete();
            data.reset();
        }
        if (known && (done.load() != size || std::filesystem::file_size(dataPath) != size)) throw std::runtime_error("Completed file has the wrong size");
        // SHA-256 is preferred when a source provides it (Hugging Face); SHA-1 comes from archive.org.
        if (!file.sha256.empty() || !file.sha1.empty()) {
            const bool useSha256 = !file.sha256.empty();
            notify(useSha256 ? "Verifying SHA-256" : "Verifying SHA-1");
            auto expected = useSha256 ? file.sha256 : file.sha1;
            expected = lower(expected);
            const auto actual = useSha256 ? sha256File(dataPath, stop) : sha1File(dataPath, stop);
            if (actual != expected) {
                LOG_ERROR("%s mismatch for %s: expected %s, got %s", useSha256 ? "SHA-256" : "SHA-1", pathUtf8(destination).c_str(), expected.c_str(), actual.c_str());
                // With data.tmp gone the saved counts are not reused (a missing data file discards them).
                std::filesystem::remove(dataPath);
                throw std::runtime_error("Checksum mismatch. Partial data was discarded; retry the download.");
            }
        }
        cancelled(stop);
    } catch (...) {
        stopTimer();
        // The final checkpoint when the transfer stops early (pause, error, restart failure): what reached the
        // disk is recorded for the next run. Nothing to do once data.tmp is closed (it was checkpointed then).
        try {
            checkpoint();
        } catch (const std::exception& e) {
            LOG_WARN("Could not save the partial data of %s to disk (%s); the last checkpoint is kept", pathUtf8(destination).c_str(), e.what());
        }
        throw;
    } // from the transfer or verification
    platform::publish(dataPath, destination);
    std::filesystem::remove(statePath);
    fileLock.removeAndUnlock();
    std::filesystem::remove(ownerPath);
    std::error_code ignored;
    std::filesystem::remove(parts, ignored);
    LOG_INFO("Saved %s (%llu bytes)", pathUtf8(destination).c_str(), (unsigned long long)(known ? size : done.load()));
    notify("Complete");
}
}
