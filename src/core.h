#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>
#include "signin.h"

namespace archive {
using Path = std::filesystem::path;
// A request that failed. status is the server's HTTP status (404, 503, ...), or 0 when no answer
// arrived; retry and wait say whether trying again may help and after how many seconds.
struct NetworkError : std::runtime_error {
    bool retry;
    unsigned wait, status;
    NetworkError(const std::string& message, bool again = true, unsigned seconds = 1, unsigned httpStatus = 0)
        : std::runtime_error(message), retry(again), wait(seconds), status(httpStatus) {}
};
// True when e is a NetworkError for one of these HTTP statuses. A SignInNeeded is the 401 the engine
// raises for that status, so callers that explain a refusal by status see it as 401.
inline bool hasHttpStatus(const std::exception& e, std::initializer_list<unsigned> statuses) {
    unsigned status = 0;
    if (auto network = dynamic_cast<const NetworkError*>(&e))
        status = network->status;
    else if (dynamic_cast<const SignInNeeded*>(&e))
        status = 401;
    return status && std::find(statuses.begin(), statuses.end(), status) != statuses.end();
}
// A link that opens a web page rather than a file (from loadDirectLink).
struct WebPageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
Path pathFromUtf8(const std::string& value);
std::string pathUtf8(const Path& value);
// url is set when each file has its own download link (GitHub release assets).
struct File {
    std::string name, format, sha1;
    uint64_t size = 0;
    bool sizeKnown = false;
    std::string sha256, url;
};
// sourceUrl is set for a direct file link (any website). downloadBase is set for a
// Hugging Face repo: each file downloads from downloadBase + its encoded path.
// Archive.org items leave both empty and use their item id and mirrors.
// checkAll starts with every file checked (a GitHub release is usually wanted whole).
struct Item {
    std::string id, title, selectedFile;
    std::vector<File> files;
    std::vector<std::string> mirrorBases;
    std::string sourceUrl, downloadBase;
    bool checkAll = false;
};
inline bool isArchiveItem(const Item& item) {
    return item.sourceUrl.empty() && item.downloadBase.empty() && std::all_of(item.files.begin(), item.files.end(), [](const File& f) {
               return f.url.empty();
           });
}
// A Hugging Face Hub repository link: kind is "models", "datasets" or "spaces".
struct HubLink {
    std::string kind, repo, revision = "main", path;
    bool file = false;
};
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
struct GitHubLink {
    std::string owner, repo, tag, file;
};
bool isGitHubLink(const std::string& input);
GitHubLink parseGitHubLink(const std::string& input);
// The release's assets plus its two source-code archives, each with its own URL.
Item parseGitHubRelease(const std::string& json, const GitHubLink& link);
// True for exactly https://api.github.com/repos/<owner>/<repo>/releases/assets/<number>: the address that
// serves a release asset's bytes when asked for application/octet-stream.
bool isGitHubAssetApiUrl(const std::string& url);
Item loadGitHubRelease(const std::string& input, std::atomic_bool& stop);
struct Link {
    std::string id, file;
};
struct Range {
    uint64_t first, last;
};
struct Progress {
    uint64_t done, total;
    std::string status;
    unsigned connections = 0;
    std::string source;
};
using Report = std::function<void(const Progress&)>;
struct ServerSpeed {
    std::string url, host;
    double bytesPerSecond = 0;
    std::wstring etag;
    std::string error;
};
using SpeedReport = std::function<void(const std::vector<ServerSpeed>&)>;
std::vector<ServerSpeed> measureServers(const File& file, const std::vector<std::string>& urls, std::atomic_bool& stop);
std::wstring wide(const std::string& text);
std::string utf8(const std::wstring& text);
// Text without leading or trailing spaces, tabs and line breaks.
std::string trim(const std::string& text);
// ASCII lower case (hosts, header values, file extensions).
std::string lower(std::string text);
// True when text is exactly length hexadecimal digits (a SHA-1 or SHA-256 in hex).
bool isHex(const std::string& text, size_t length);
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
// How much of the root the caller already knows, for thousands of names in one folder: Checked means an earlier call
// with this same root found no link in it or above it, so only the parts below the root are looked at; Missing means
// that and that the root does not exist, so nothing below it can be a link either. The name is always validated.
enum class RootState { Unchecked,
                       Checked,
                       Missing };
// Links and junctions in the result path (the root and its parents included, unless known) are rejected.
Path safePath(const Path& root, const std::string& name, RootState rootState = RootState::Unchecked);
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
// Progress of one load operation, identified by its stop flag. Only atomics: any thread may read it.
struct LoadProgress {
    std::atomic<uint64_t> bytes{0};    // response bytes read so far
    std::atomic<uint64_t> requests{0}; // requests answered so far
};
// While it lives, the loaders' reads made with this stop flag add to progress (bytes of each response
// body read, one request for each response obtained). One per stop flag at a time; the progress
// object must outlive it. Reads with no watch on their stop flag cost one atomic load.
class LoadWatch {
public:
    LoadWatch(const std::atomic_bool& stop, LoadProgress& progress);
    ~LoadWatch();
    LoadWatch(const LoadWatch&) = delete;
    LoadWatch& operator=(const LoadWatch&) = delete;

private:
    const std::atomic_bool* stop_;
};
}
