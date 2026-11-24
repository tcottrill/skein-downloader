// Web folder listings: the "Index of /files/" pages that Apache, nginx, lighttpd and similar
// servers make for a folder, live or as Wayback Machine copies. A folder becomes one Item of
// its files, subfolders included, each named by its path below the folder.
#include "sites.h"
#include "platform/ftp.h"
#include "util/sys_log.h"
#include <algorithm>
#include <cstring>
#include <deque>
#include <future>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
namespace archive {
namespace {
// Bounds for one folder, so a runaway listing (or a site that only looks like one) ends.
constexpr size_t maxFolders = 2000, maxFiles = 50000, maxDepth = 32;
const char* const notListing = "This link opens a web page, not a file or a folder listing. Copy the file's direct download link instead.";
constexpr auto npos = std::string::npos;

std::string withoutFragment(const std::string& url) { return url.substr(0, url.find('#')); }
// An http(s) URL as scheme ("http"), authority ("host:port") and the rest (path and query, starting with '/').
bool parts(const std::string& url, std::string& scheme, std::string& authority, std::string& rest) {
    auto colon = url.find("://"); if (colon == npos) return false;
    scheme = lower(url.substr(0, colon)); if (scheme != "http" && scheme != "https") return false;
    auto start = colon + 3, end = url.find_first_of("/?#", start);
    authority = url.substr(start, end == npos ? npos : end - start);
    if (authority.empty()) return false;
    rest = end == npos ? "/" : url.substr(end);
    if (rest[0] != '/') rest = "/" + rest;
    return true;
}
// Resolves the "." and ".." segments of a path: "/a/b/../c/" is "/a/c/".
std::string removeDots(const std::string& path) {
    std::vector<std::string> segments, out;
    for (size_t start = 1;;) {
        auto slash = path.find('/', start);
        segments.push_back(path.substr(start, slash == npos ? npos : slash - start));
        if (slash == npos) break;
        start = slash + 1;
    }
    for (size_t i = 0; i < segments.size(); ++i) {
        bool last = i + 1 == segments.size();
        if (segments[i] == "." || segments[i] == "..") {
            if (segments[i] == ".." && !out.empty()) out.pop_back();
            if (last) out.push_back("");
        } else out.push_back(segments[i]);
    }
    std::string result; for (auto& segment : out) result += "/" + segment;
    return result.empty() ? "/" : result;
}
// The HTML escapes that appear in link addresses.
std::string unescapeHtml(const std::string& s) {
    static const std::pair<const char*, const char*> escapes[] = {{"&amp;", "&"}, {"&#38;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&#039;", "'"}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}};
    std::string out;
    for (size_t i = 0; i < s.size();) {
        bool replaced = false;
        if (s[i] == '&') for (auto& [from, to] : escapes) if (s.compare(i, strlen(from), from) == 0) { out += to; i += strlen(from); replaced = true; break; }
        if (!replaced) out += s[i++];
    }
    return out;
}
// A link in a page made absolute against the page's address; empty for other schemes (mailto:, ftp:).
std::string resolve(const std::string& base, std::string href) {
    href = trim(unescapeHtml(href));
    std::string scheme, authority, rest;
    if (href.empty() || href[0] == '#' || !parts(base, scheme, authority, rest)) return {};
    auto basePath = rest.substr(0, rest.find('?'));
    auto colon = href.find(':'), delimiter = href.find_first_of("/?#");
    if (colon != npos && (delimiter == npos || colon < delimiter)) { std::string s, a, r; if (!parts(href, s, a, r)) return {}; }
    else if (href.starts_with("//")) href = scheme + ":" + href;
    else if (href[0] == '/') href = scheme + "://" + authority + href;
    else if (href[0] == '?') href = scheme + "://" + authority + basePath + href;
    else href = scheme + "://" + authority + basePath.substr(0, basePath.rfind('/') + 1) + href;
    if (!parts(withoutFragment(href), scheme, authority, rest)) return {};
    auto query = rest.find('?');
    return scheme + "://" + authority + removeDots(rest.substr(0, query)) + (query == npos ? "" : rest.substr(query));
}
// A URL for comparing: the host in lower case without "www." or a default port, then the path.
// The scheme is left out (archives hold the same file under http and https). Empty for other URLs.
std::string urlKey(const std::string& url) {
    std::string scheme, authority, rest;
    if (!parts(withoutFragment(url), scheme, authority, rest)) return {};
    authority = lower(authority);
    if (auto at = authority.rfind('@'); at != npos) authority.erase(0, at + 1);
    if (authority.ends_with(":80") || authority.ends_with(":443")) authority.erase(authority.rfind(':'));
    if (authority.starts_with("www.")) authority.erase(0, 4);
    return authority + rest.substr(0, rest.find('?'));
}
// The href values of a page's <a> tags, in page order.
std::vector<std::string> anchors(const std::string& html) {
    std::vector<std::string> out; auto low = lower(html);
    for (auto at = low.find("<a"); at != npos; at = low.find("<a", at + 2)) {
        if (at + 2 >= low.size() || !isspace((unsigned char)low[at + 2])) continue;
        auto end = low.find('>', at); if (end == npos) break;
        for (auto h = low.find("href", at); h != npos && h < end; h = low.find("href", h + 4)) {
            if (!isspace((unsigned char)low[h - 1])) continue;
            auto i = h + 4; while (i < end && isspace((unsigned char)low[i])) ++i;
            if (i >= end || low[i] != '=') continue;
            ++i; while (i < end && isspace((unsigned char)low[i])) ++i;
            if (i >= end) break;
            if (html[i] == '"' || html[i] == '\'') {
                auto close = html.find(html[i], i + 1); if (close == npos) break;
                out.push_back(html.substr(i + 1, close - i - 1));
            } else out.push_back(html.substr(i, low.find_first_of(" \t\r\n>", i) - i));
            break;
        }
    }
    return out;
}
File linkedFile(const std::string& name, const std::string& url) {
    File file; file.name = name; file.url = url;
    auto base = name.substr(name.rfind('/') + 1); auto dot = base.rfind('.');
    if (dot != npos && dot + 1 < base.size()) file.format = base.substr(dot + 1);
    return file;
}
// The name a folder's downloads are saved under: its last path segment, or the host for a site's root.
std::string folderName(const std::string& url) {
    std::string scheme, authority, rest; if (!parts(url, scheme, authority, rest)) return "folder";
    auto path = rest.substr(0, rest.find('?')); while (!path.empty() && path.back() == '/') path.pop_back();
    auto segment = path.substr(path.rfind('/') + 1);
    if (segment.empty()) segment = lower(authority);
    try { segment = decode(segment); } catch (const std::exception&) {}
    return safeFilename(segment);
}
// The links to files and folders directly inside the folder of pageUrl, from absolute addresses.
std::vector<FolderEntry> entriesIn(const std::vector<std::string>& urls, const std::string& pageUrl) {
    std::vector<FolderEntry> out; std::set<std::string> seen;
    auto folder = urlKey(pageUrl); folder = folder.substr(0, folder.rfind('/') + 1);
    if (folder.empty()) return out;
    for (auto& url : urls) {
        if (url.empty() || url.find('?') != npos) continue; // sorting links such as ?C=N;O=D
        auto key = urlKey(url);
        if (!key.starts_with(folder) || key.size() == folder.size()) continue;
        auto rest = key.substr(folder.size());
        FolderEntry entry; entry.folder = rest.back() == '/';
        if (entry.folder) rest.pop_back();
        if (rest.empty() || rest.find('/') != npos) continue;
        try { entry.name = decode(rest); } catch (const std::exception&) { continue; }
        if (entry.name == "." || entry.name == ".." || entry.name.find_first_of("/\\") != npos) continue;
        if (!seen.insert((entry.folder ? "/" : "") + entry.name).second) continue;
        entry.url = url; out.push_back(std::move(entry));
    }
    return out;
}
struct Listing { std::string url; std::vector<FolderEntry> entries; bool listing = false; };
// Reads one listing page. An archived page is read in its raw "id_" form, and its entries link to
// raw captures from the time of the capture the archive served (it answers with the nearest one).
// The archive's cache can hold a cut-down raw answer (one byte, left by a byte-range request), so
// the archive's normal form of the page, a separate address with its links pointing back into the
// archive, is read when the raw one is not a listing.
Listing readListing(const std::string& url, bool wayback, std::atomic_bool& stop) {
    auto r = fetch(url, stop, "Accept: text/html,*/*;q=0.5\r\n");
    Listing out; out.url = r.url.empty() ? url : r.url;
    if (r.file && !wayback) return out;
    WaybackLink capture;
    if (!wayback) {
        out.listing = looksLikeFolderListing(r.body);
        out.entries = parseFolderListing(r.body, out.url);
        return out;
    }
    if (!parseWaybackLink(out.url, capture) && !parseWaybackLink(url, capture)) return out;
    if (!r.file && looksLikeFolderListing(r.body)) {
        out.listing = true;
        out.entries = parseFolderListing(r.body, capture.original);
    } else {
        auto page = fetch("https://web.archive.org/web/" + capture.time + "/" + capture.original, stop, "Accept: text/html,*/*;q=0.5\r\n");
        WaybackLink served;
        if (page.file || !looksLikeFolderListing(page.body) || !parseWaybackLink(page.url.empty() ? url : page.url, served)) return out;
        std::vector<std::string> originals;
        for (auto& href : anchors(page.body)) if (WaybackLink link; parseWaybackLink(resolve(page.url, href), link)) originals.push_back(link.original);
        capture = served; out.listing = true;
        out.entries = entriesIn(originals, served.original);
    }
    out.url = waybackRawUrl(capture.time, capture.original);
    for (auto& entry : out.entries) entry.url = waybackRawUrl(capture.time, entry.url);
    return out;
}
// Lists a folder's files and those of its subfolders, a level at a time, reading up to parallel
// pages at once. A subfolder that cannot be read is skipped (the archive may lack its page).
Item crawl(const Listing& root, size_t parallel, bool wayback, std::atomic_bool& stop) {
    Item item; size_t folders = 0;
    struct Pending { std::string url, path; };
    std::vector<std::pair<Listing, std::string>> ready{{root, ""}};
    for (size_t depth = 0; !ready.empty(); ++depth) {
        std::vector<Pending> next;
        for (auto& [listing, path] : ready) for (auto& entry : listing.entries) {
            if (entry.folder) {
                if (++folders > maxFolders) throw std::runtime_error("This folder has too many subfolders (over 2000). Open one of its subfolders instead.");
                if (depth + 1 < maxDepth) next.push_back({entry.url, path + safeFilename(entry.name) + "/"});
            } else {
                item.files.push_back(linkedFile(path + safeFilename(entry.name), entry.url));
                if (item.files.size() > maxFiles) throw std::runtime_error("This folder lists too many files (over 50000). Open one of its subfolders instead.");
            }
        }
        ready.clear();
        for (size_t i = 0; i < next.size(); i += parallel) {
            std::vector<std::future<Listing>> reads;
            for (size_t k = i; k < std::min(next.size(), i + parallel); ++k)
                reads.push_back(std::async(std::launch::async, [&stop, wayback, url = next[k].url] { return readListing(url, wayback, stop); }));
            for (size_t k = 0; k < reads.size(); ++k) {
                auto& pending = next[i + k];
                try {
                    auto listing = reads[k].get();
                    if (listing.listing) ready.emplace_back(std::move(listing), pending.path);
                    else LOG_WARN("Skipped folder %s: not a folder listing", logUrl(pending.url).c_str());
                } catch (const std::exception& e) {
                    if (stop) throw;
                    LOG_WARN("Skipped folder %s: %s", logUrl(pending.url).c_str(), e.what());
                }
            }
        }
    }
    return item;
}
void finish(Item& item) {
    if (item.files.empty()) throw std::runtime_error("This folder listing has no files.");
    std::sort(item.files.begin(), item.files.end(), [](const File& a, const File& b) { return a.name < b.name; });
}
// The archive's capture index for an address alone or for everything below it. It can take a
// minute to answer, and it is sometimes offline; then nothing is returned.
std::vector<Capture> captures(const std::string& original, bool prefix, std::atomic_bool& stop) {
    auto address = original.substr(original.find("://") + 3);
    auto query = "https://web.archive.org/cdx/search/cdx?url=" + encode(address) + (prefix ? "&matchType=prefix" : "")
        + "&fl=original,timestamp,length,statuscode&filter=statuscode:200";
    try {
        auto r = fetch(query, stop, {}, 120);
        auto folder = prefix ? original : original.substr(0, original.rfind('/') + 1);
        return parseWaybackCaptures(r.body, folder);
    } catch (const std::exception& e) {
        if (stop) throw;
        LOG_WARN("Wayback Machine capture index failed for %s: %s", logUrl(original).c_str(), e.what());
        return {};
    }
}
Item loadWaybackItem(const WaybackLink& link, std::atomic_bool& stop) {
    std::string scheme, authority, rest; parts(link.original, scheme, authority, rest);
    auto folder = link.original;
    if (!rest.substr(0, rest.find('?')).ends_with('/')) {
        // A file, from its most complete capture; or a folder linked without its final '/'. The page
        // is read whole, never with a byte range: the archive's cache keeps a range answer and gives
        // it to later requests for the whole page.
        auto time = link.time, original = link.original;
        if (auto found = captures(link.original, false, stop); found.size() == 1) { time = found[0].time; original = found[0].original; }
        auto r = fetch(waybackRawUrl(time, original), stop, "Accept: */*\r\n");
        if (r.file || !looksLikeFolderListing(r.body)) {
            File file; file.format = r.type;
            auto name = filenameFromDisposition(r.disposition);
            file.name = safeFilename(name.empty() ? filenameFromUrl(original) : name);
            // No size from this answer: it may be a cut-down copy from the archive's cache (see download()).
            Item item; item.title = item.selectedFile = file.name; item.sourceUrl = r.url.empty() ? waybackRawUrl(time, original) : r.url;
            item.files.push_back(std::move(file));
            return item;
        }
        folder = link.original.substr(0, link.original.find('?')) + "/";
    }
    auto root = readListing(waybackRawUrl(link.time, folder), true, stop);
    WaybackLink served;
    if (!root.listing || !parseWaybackLink(root.url, served)) throw std::runtime_error("This archived page is not a folder listing or a file.");
    Item item;
    for (auto& capture : captures(served.original, true, stop)) item.files.push_back(linkedFile(capture.name, waybackRawUrl(capture.time, capture.original)));
    if (item.files.empty()) {
        LOG_WARN("No capture index for %s; reading its archived listing pages", logUrl(served.original).c_str());
        item = crawl(root, 1, true, stop);
    }
    item.id = folderName(served.original); item.title = served.original;
    finish(item);
    return item;
}
// An ftp:// folder and its subfolders, read one after another over one signed-in session (FTP
// servers limit how many connections an address may open). A link to a file loads that file.
Item loadFtpItem(const std::string& url, std::atomic_bool& stop) {
    auto address = ftp::parseUrl(url);
    const auto server = "ftp://" + address.host + (address.port == 21 ? "" : ":" + std::to_string(address.port));
    ftp::Lister lister(url, stop);
    std::vector<ftp::Entry> entries;
    if (!address.path.ends_with('/')) {
        // A folder linked without its final '/', or else a file (a file cannot be opened as a folder).
        try { entries = lister.list(address.path + "/"); }
        catch (const std::exception&) { if (stop) throw; return loadDirectLink(url, stop); }
        address.path += '/';
    } else entries = lister.list(address.path);
    struct Pending { std::string path, relative; size_t depth; };
    std::deque<Pending> pending{{address.path, "", 0}};
    Item item; size_t folders = 0; bool root = true;
    while (!pending.empty()) {
        auto folder = std::move(pending.front()); pending.pop_front();
        if (!root) try { entries = lister.list(folder.path); }
        catch (const std::exception& e) {
            if (stop) throw;
            LOG_WARN("Skipped FTP folder %s%s: %s", server.c_str(), folder.path.c_str(), e.what());
            continue;
        }
        root = false;
        for (auto& entry : entries) {
            if (entry.name.find_first_of("/\\") != npos) continue;
            if (entry.folder) {
                if (++folders > maxFolders) throw std::runtime_error("This folder has too many subfolders (over 2000). Open one of its subfolders instead.");
                if (folder.depth + 1 < maxDepth) pending.push_back({folder.path + entry.name + "/", folder.relative + safeFilename(entry.name) + "/", folder.depth + 1});
                continue;
            }
            auto file = linkedFile(folder.relative + safeFilename(entry.name), server + encode(folder.path + entry.name, true));
            file.size = entry.size; file.sizeKnown = entry.sizeKnown;
            item.files.push_back(std::move(file));
            if (item.files.size() > maxFiles) throw std::runtime_error("This folder lists too many files (over 50000). Open one of its subfolders instead.");
        }
    }
    auto path = address.path; while (!path.empty() && path.back() == '/') path.pop_back();
    auto name = path.substr(path.rfind('/') + 1);
    item.id = safeFilename(name.empty() ? address.host : name); item.title = server + encode(address.path, true);
    finish(item);
    return item;
}
}

bool parseWaybackLink(const std::string& input, WaybackLink& out) {
    static const std::regex link(R"(https?://web\.archive\.org/web/([0-9]{4,14})(?:[a-z]{2}_)?/+(\S+))", std::regex::icase);
    static const std::regex scheme(R"((https?):/+(.*))", std::regex::icase);
    std::smatch m, s; auto text = trim(input);
    if (!std::regex_match(text, m, link)) return false;
    auto original = m[2].str();
    // The archive also takes "http:/host" (some clients collapse the slashes) and a bare "host/path".
    if (std::regex_match(original, s, scheme)) original = lower(s[1]) + "://" + s[2].str();
    else if (original.find("://") == npos) original = "http://" + original;
    else return false;
    original = withoutFragment(original);
    if (!isHttpUrl(original)) return false;
    out = {m[1].str(), original};
    return true;
}
bool looksLikeFolderUrl(const std::string& input) {
    std::string scheme, authority, rest; auto url = trim(input);
    if (!isHttpUrl(url) || !parts(withoutFragment(url), scheme, authority, rest)) return false;
    return rest.find('?') == npos && rest.back() == '/';
}
bool isWaybackLink(const std::string& input) { WaybackLink link; return parseWaybackLink(input, link); }
std::string waybackRawUrl(const std::string& time, const std::string& original) {
    return "https://web.archive.org/web/" + time + "id_/" + original;
}
std::string waybackAlternateUrl(const std::string& url) {
    static const std::regex raw(R"((https?://web\.archive\.org/web/[0-9]{4,14})id_(/.*))", std::regex::icase);
    std::smatch m;
    return std::regex_match(url, m, raw) ? m[1].str() + "if_" + m[2].str() : std::string();
}
std::vector<FolderEntry> parseFolderListing(const std::string& html, const std::string& pageUrl) {
    std::vector<std::string> urls;
    for (auto& href : anchors(html)) urls.push_back(resolve(pageUrl, href));
    return entriesIn(urls, pageUrl);
}
bool looksLikeFolderListing(const std::string& html) {
    auto start = lower(html.substr(0, 65536));
    return start.find("index of /") != npos || start.find("directory listing for /") != npos;
}
std::vector<Capture> parseWaybackCaptures(const std::string& cdx, const std::string& folderUrl) {
    auto folder = urlKey(folderUrl); if (folder.empty()) return {};
    if (folder.back() != '/') folder += '/';
    std::map<std::string, Capture> best;
    std::istringstream in(cdx); std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line); std::string original, time, length, status;
        if (!(fields >> original >> time >> length >> status) || status != "200") continue;
        if (original.find('?') != npos || time.empty() || time.find_first_not_of("0123456789") != npos) continue;
        auto key = urlKey(original);
        if (!key.starts_with(folder) || key.size() == folder.size() || key.back() == '/') continue;
        Capture capture; capture.time = time; capture.original = withoutFragment(original);
        bool usable = true; std::istringstream segments(key.substr(folder.size())); std::string segment;
        while (usable && std::getline(segments, segment, '/')) {
            try { segment = decode(segment); } catch (const std::exception&) { usable = false; break; }
            usable = !segment.empty() && segment != "." && segment != "..";
            capture.name += (capture.name.empty() ? "" : "/") + safeFilename(segment);
        }
        if (!usable) continue;
        if (!length.empty() && length.size() < 20 && length.find_first_not_of("0123456789") == npos) capture.length = std::stoull(length);
        auto [it, added] = best.try_emplace(capture.name, capture);
        if (!added && (capture.length > it->second.length || (capture.length == it->second.length && capture.time > it->second.time))) it->second = capture;
    }
    std::vector<Capture> out;
    for (auto& [name, capture] : best) out.push_back(std::move(capture));
    return out;
}
Item loadFolderItem(const std::string& input, std::atomic_bool& stop) {
    auto url = trim(input);
    if (WaybackLink link; parseWaybackLink(url, link)) return loadWaybackItem(link, stop);
    if (lower(url.substr(0, 6)) == "ftp://") return loadFtpItem(url, stop);
    auto root = readListing(url, false, stop);
    if (!root.listing) throw std::runtime_error(notListing);
    auto item = crawl(root, 4, false, stop);
    item.id = folderName(root.url); item.title = root.url;
    finish(item);
    return item;
}
}
