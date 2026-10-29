#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace archive {
using Path = std::filesystem::path;
Path pathFromUtf8(const std::string& value);
std::string pathUtf8(const Path& value);
// url is set when each file has its own download link (GitHub release assets).
struct File { std::string name, format, sha1; uint64_t size = 0; bool sizeKnown = false; std::string sha256, url; };
// sourceUrl is set for a direct file link (any website). downloadBase is set for a
// Hugging Face repo: each file downloads from downloadBase + its encoded path.
// Archive.org items leave both empty and use their item id and mirrors.
// checkAll starts with every file checked (a GitHub release is usually wanted whole).
struct Item { std::string id, title, selectedFile; std::vector<File> files; std::vector<std::string> mirrorBases; std::string sourceUrl, downloadBase; bool checkAll = false; };
inline bool isArchiveItem(const Item& item) {
    return item.sourceUrl.empty() && item.downloadBase.empty() && std::all_of(item.files.begin(), item.files.end(), [](const File& f) { return f.url.empty(); });
}
// A Hugging Face Hub repository link: kind is "models", "datasets" or "spaces".
struct HubLink { std::string kind, repo, revision = "main", path; bool file = false; };
bool isHubLink(const std::string& input);
HubLink parseHubLink(const std::string& input);
// The repository's file list from one page of the Hub tree API.
std::vector<File> parseHubTree(const std::string& json);
// True when the Hub repo info marks the repo as gated (terms must be accepted while signed in).
bool hubRepoGated(const std::string& json);
// The next page from a Hub API Link header; empty when this is the last page.
std::string hubNextPage(const std::string& linkHeader);
Item loadHubItem(const std::string& input, std::atomic_bool& stop);
// A GitHub release link; an empty tag means the latest release.
struct GitHubLink { std::string owner, repo, tag, file; };
bool isGitHubLink(const std::string& input);
GitHubLink parseGitHubLink(const std::string& input);
// The release's assets plus its two source-code archives, each with its own URL.
Item parseGitHubRelease(const std::string& json, const GitHubLink& link);
Item loadGitHubRelease(const std::string& input, std::atomic_bool& stop);
struct Link { std::string id, file; };
struct Range { uint64_t first, last; };
struct Progress { uint64_t done, total; std::string status; unsigned connections = 0; std::string source; };
using Report = std::function<void(const Progress&)>;
struct ServerSpeed { std::string url, host; double bytesPerSecond = 0; std::wstring etag; std::string error; };
using SpeedReport = std::function<void(const std::vector<ServerSpeed>&)>;
std::vector<ServerSpeed> measureServers(const File& file, const std::vector<std::string>& urls, std::atomic_bool& stop);
std::wstring wide(const std::string& text);
std::string utf8(const std::wstring& text);
Link parseLink(const std::string& input);
bool isArchiveLink(const std::string& input);
bool isHttpUrl(const std::string& input);
// A link for the log without its query string or fragment, where file lockers and
// signed download links carry access tokens.
std::string logUrl(const std::string& url);
// True when the URL path ends in a common file extension (.zip, .iso, .mp4, ...).
bool looksLikeFileUrl(const std::string& url);
std::string filenameFromDisposition(const std::string& header);
std::string filenameFromUrl(const std::string& url);
// Makes a server-supplied name safe to save; never returns an empty name.
std::string safeFilename(std::string name);
Item loadDirectLink(const std::string& url, std::atomic_bool& stop);
std::string encode(const std::string& input, bool keepSlash = false);
// Percent-decoding; throws on a malformed escape.
std::string decode(const std::string& input);
Path safePath(const Path& root, const std::string& name);
std::vector<Range> split(uint64_t size, unsigned connections);
Item parseMetadata(const std::string& id, const std::string& json);
Item loadItem(const std::string& link, std::atomic_bool& stop);
std::string sha1File(const Path& path, std::atomic_bool& stop);
std::string sha256File(const Path& path, std::atomic_bool& stop);
void download(const std::string& url, const File& file, const Path& destination,
              unsigned connections, std::atomic_bool& stop, const Report& report,
              const std::vector<std::string>& mirrors = {}, const std::string& forcedSource = {},
              const SpeedReport& speeds = {}, const std::string& identity = {});
std::string formatBytes(uint64_t bytes);
}
