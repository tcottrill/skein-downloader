#include "core.h"
#include "sites.h"
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
#include "util/sys_log.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <thread>
namespace archive {
namespace {
struct NetworkError : std::runtime_error {
    bool retry; unsigned wait;
    NetworkError(std::string message, bool again = true, unsigned seconds = 1) : std::runtime_error(message), retry(again), wait(seconds) {}
};
// Shared by both transports for any response other than 200/206.
NetworkError statusError(unsigned long status, unsigned retryAfter) {
    std::string message = "HTTP " + std::to_string(status);
    if (status == 401 || status == 403) message += ": file requires access permission or sign-in (not supported in this version).";
    if (status == 404) message += ": file not found.";
    return NetworkError(message, status == 429 || status == 408 || status >= 500, retryAfter);
}
void cancelled(const std::atomic_bool& stop) { if (stop) throw std::runtime_error("Paused"); }
void delay(unsigned seconds, const std::atomic_bool& stop) {
    for (uint64_t i = 0; i < (uint64_t)seconds * 10; ++i) { cancelled(stop); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
}
#ifdef _WIN32
#include "platform/http_winhttp.inc"
#else
#include "platform/http_curl.inc"
#endif
std::unique_ptr<Request> get(const std::string& url, const std::wstring& headers, std::atomic_bool& stop, bool redirects = true) {
    for (unsigned attempt = 0;; ++attempt) {
        cancelled(stop);
        try {
#ifdef _WIN32
            return std::make_unique<Request>(Request::Url(url), headers, redirects);
#else
            return std::make_unique<Request>(Request::Url(url), headers, redirects, &stop);
#endif
        }
        catch (const NetworkError& e) {
            if (!e.retry || attempt == 3) { LOG_ERROR("Request failed: %s: %s", logUrl(url).c_str(), e.what()); throw; }
            const auto wait = std::max(e.wait, 1u << attempt);
            LOG_WARN("Request failed (attempt %u of 4), retrying in %us: %s: %s", attempt + 1, wait, logUrl(url).c_str(), e.what());
            delay(wait, stop);
        }
    }
}
void validateRange(Request& r, uint64_t first, uint64_t last, uint64_t total) {
    auto expected = L"bytes " + std::to_wstring(first) + L"-" + std::to_wstring(last) + L"/" + std::to_wstring(total);
    if (r.status != 206 || header(r.request, WINHTTP_QUERY_CONTENT_RANGE) != expected)
        throw NetworkError("Server returned an unexpected byte range. Reload the item and retry with one connection.",false);
}
void regular(const Path& path) {
    platform::regular(path);
}
std::vector<Range> queuedRanges(uint64_t size) {
    std::vector<Range> result;
    // Range identities depend on size, never on the chosen worker count.
    // Usually 1-4 MiB; grow for huge files to bound the number of staging files.
    auto ceilDiv=[](uint64_t n,uint64_t d){return n/d+(n%d!=0);};
    auto chunk=std::max({uint64_t(1024*1024),std::min<uint64_t>(4*1024*1024,ceilDiv(size,32)),ceilDiv(size,16384)});
    for(uint64_t first=0;first<size;) {
        auto length=std::min(chunk,size-first);
        result.push_back({first,first+length-1});first+=length;
    }
    return result;
}
}
// Header text arrives as Latin-1 characters; servers often send UTF-8 bytes in it.
std::string headerUtf8(const std::wstring& value) {
    if (std::all_of(value.begin(), value.end(), [](wchar_t c) { return c <= 0xFF; })) {
        std::string bytes; for (auto c : value) bytes += (char)(unsigned char)c;
        try { wide(bytes); return bytes; } catch (const std::exception&) {}
    }
    return utf8(value);
}
Item loadDirectLink(const std::string& url, std::atomic_bool& stop) {
    if (!isHttpUrl(url)) throw std::runtime_error("Paste an HTTP or HTTPS link to a file.");
    auto r = get(url, L"Range: bytes=0-0\r\n", stop);
    File file;
    if (r->status == 206) {
        std::wsmatch m; auto range = header(r->request, WINHTTP_QUERY_CONTENT_RANGE);
        if (std::regex_match(range, m, std::wregex(L"bytes 0-0/([0-9]+)"))) { file.size = std::stoull(m[1]); file.sizeKnown = true; }
    } else {
        auto length = header(r->request, WINHTTP_QUERY_CONTENT_LENGTH);
        if (!length.empty() && length.find_first_not_of(L"0123456789") == std::wstring::npos) { file.size = std::stoull(length); file.sizeKnown = true; }
    }
    auto type = headerUtf8(header(r->request, WINHTTP_QUERY_CONTENT_TYPE));
    type = type.substr(0, type.find(';')); std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return (char)tolower(c); });
    while (!type.empty() && type.back() == ' ') type.pop_back();
    auto name = filenameFromDisposition(headerUtf8(header(r->request, WINHTTP_QUERY_CONTENT_DISPOSITION)));
    auto finalUrl = r->url();
    if (name.empty()) name = filenameFromUrl(finalUrl.empty() ? url : finalUrl);
    if (name.empty()) name = filenameFromUrl(url);
    auto lower = name; std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)tolower(c); });
    if (type == "text/html" && !lower.ends_with(".html") && !lower.ends_with(".htm"))
        throw std::runtime_error("This link opens a web page, not a file. Copy the file's direct download link instead.");
    file.name = safeFilename(name); file.format = type;
    Item item; item.title = file.name; item.selectedFile = file.name; item.sourceUrl = url; item.files.push_back(std::move(file));
    return item;
}
namespace {
Response respond(Request& r, const std::string& requested, std::atomic_bool& stop) {
    Response out; out.url = r.url(); if (out.url.empty()) out.url = requested;
    out.type = headerUtf8(header(r.request, WINHTTP_QUERY_CONTENT_TYPE));
    out.type = out.type.substr(0, out.type.find(';')); std::transform(out.type.begin(), out.type.end(), out.type.begin(), [](unsigned char c) { return (char)tolower(c); });
    while (!out.type.empty() && out.type.back() == ' ') out.type.pop_back();
    out.disposition = headerUtf8(header(r.request, WINHTTP_QUERY_CONTENT_DISPOSITION));
    auto length = header(r.request, WINHTTP_QUERY_CONTENT_LENGTH);
    if (!length.empty() && length.find_first_not_of(L"0123456789") == std::wstring::npos) { out.size = std::stoull(length); out.sizeKnown = true; }
    out.file = !(out.type.starts_with("text/") || out.type.find("json") != std::string::npos || out.type.find("xml") != std::string::npos);
    if (out.file) return out;
    char buffer[65536];
    for (;;) {
        cancelled(stop); auto n = r.read(buffer, sizeof buffer); if (!n) break;
        if (out.body.size() + n > 16 * 1024 * 1024) throw std::runtime_error("The site's page is too large (16 MiB limit).");
        out.body.append(buffer, n);
    }
    return out;
}
}
Response fetch(const std::string& url, std::atomic_bool& stop, const std::string& headers) {
    auto r = get(url, wide(headers), stop);
    return respond(*r, url, stop);
}
Response post(const std::string& url, const std::string& body, std::atomic_bool& stop, const std::string& headers) {
    cancelled(stop);
#ifdef _WIN32
    Request r(Request::Url(url), wide(headers), true, &body);
#else
    Request r(Request::Url(url), wide(headers), true, &stop, &body);
#endif
    return respond(r, url, stop);
}
Item loadHubItem(const std::string& input, std::atomic_bool& stop) {
    auto link = parseHubLink(input);
    // A file link lists only its own folder; a tree link lists that folder; otherwise the whole repo.
    auto folder = link.file ? link.path.substr(0, link.path.rfind('/') == std::string::npos ? 0 : link.path.rfind('/')) : link.path;
    std::string page = "https://huggingface.co/api/" + link.kind + "/" + link.repo + "/tree/" + encode(link.revision)
        + (folder.empty() ? "" : "/" + encode(folder, true)) + "?recursive=true";
    auto readAll = [&](Request& r) {
        std::string body; char buffer[65536];
        for (;;) {
            cancelled(stop); auto n = r.read(buffer, sizeof buffer); if (!n) break;
            if (body.size() + n > 64 * 1024 * 1024) throw std::runtime_error("Repository file list is too large (64 MiB limit).");
            body.append(buffer, n);
        }
        return body;
    };
    // Check access first: gated repos list their files publicly but refuse every download.
    try {
        auto info = get("https://huggingface.co/api/" + link.kind + "/" + link.repo, L"", stop);
        if (hubRepoGated(readAll(*info)))
            throw std::runtime_error("This repository is gated: its files need you to accept its terms while signed in to Hugging Face. Signed-in downloads are not supported in this version.");
    } catch (const NetworkError& e) {
        // The Hub answers 401 for both missing and private repos.
        if (std::string(e.what()).starts_with("HTTP 401") || std::string(e.what()).starts_with("HTTP 404"))
            throw std::runtime_error("This Hugging Face repository was not found, or it is private (sign-in is not supported in this version).");
        throw;
    }
    Item item; item.id = item.title = link.repo;
    for (unsigned pages = 0; !page.empty(); ++pages) {
        if (pages == 1000) throw std::runtime_error("This repository lists too many files (over 1000 pages).");
        auto r = get(page, L"", stop);
        auto files = parseHubTree(readAll(*r));
        item.files.insert(item.files.end(), std::make_move_iterator(files.begin()), std::make_move_iterator(files.end()));
        page = hubNextPage(headerUtf8(header(r->request, HEADER_LINK)));
    }
    if (item.files.empty()) throw std::runtime_error("This repository has no files at that location.");
    if (link.file) {
        if (std::none_of(item.files.begin(), item.files.end(), [&](const File& f) { return f.name == link.path; }))
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
        std::string body; char buffer[65536];
        for (;;) {
            cancelled(stop); auto n = r->read(buffer, sizeof buffer); if (!n) break;
            if (body.size() + n > 16 * 1024 * 1024) throw std::runtime_error("GitHub release data is too large (16 MiB limit).");
            body.append(buffer, n);
        }
        auto item = parseGitHubRelease(body, link);
        if (item.files.empty()) throw std::runtime_error("This release has no files.");
        return item;
    } catch (const NetworkError& e) {
        std::string message = e.what();
        if (message.starts_with("HTTP 404"))
            throw std::runtime_error(link.tag.empty() ? "This repository has no published releases, or it is private." : "Release not found. Check the tag, or the repository may be private.");
        // Anonymous API use is limited to 60 requests an hour per network address.
        if (message.starts_with("HTTP 403") || message.starts_with("HTTP 429"))
            throw std::runtime_error("GitHub's limit for release lookups without signing in (60 an hour) was reached. Try again later.");
        throw;
    }
}
Item loadItem(const std::string& input, std::atomic_bool& stop) {
    if (isHubLink(input)) return loadHubItem(input, stop);
    if (isGitHubLink(input)) return loadGitHubRelease(input, stop);
    if (isHostLink(input)) return loadHostItem(input, stop);
    if (isLockerLink(input)) return loadLockerItem(input, stop);
    if (!isArchiveLink(input) && isHttpUrl(input)) {
        auto a = input.find_first_not_of(" \t\r\n"), b = input.find_last_not_of(" \t\r\n");
        return loadDirectLink(input.substr(a, b - a + 1), stop);
    }
    auto link = parseLink(input);
    auto r = get("https://archive.org/metadata/" + encode(link.id), L"", stop);
    std::string body; char buffer[65536];
    for (;;) {
        cancelled(stop); auto n = r->read(buffer, sizeof buffer); if (!n) break;
        if (body.size() + n > 64 * 1024 * 1024) throw std::runtime_error("Item metadata is too large (64 MiB limit).");
        body.append(buffer, n);
    }
    auto item = parseMetadata(link.id, body); item.selectedFile = link.file; return item;
}
std::vector<ServerSpeed> measureServers(const File& file, const std::vector<std::string>& urls, std::atomic_bool& stop) {
    if(!file.sizeKnown || !file.size)throw std::runtime_error("Choose a non-empty file with a known size to test speeds.");
    std::vector<ServerSpeed> results(std::min<size_t>(urls.size(),3));
    std::vector<std::jthread> probes;
    for(size_t i=0;i<results.size();++i)probes.emplace_back([&,i]{
        auto& result=results[i];result.url=urls[i];
        try {
            cancelled(stop);result.host=utf8(Request::Url(urls[i]).host);
            uint64_t sample=std::min<uint64_t>(file.size,256*1024);
            auto start=std::chrono::steady_clock::now();
#ifdef _WIN32
            Request r(Request::Url(urls[i]),L"Range: bytes=0-"+std::to_wstring(sample-1)+L"\r\n",i==0);
#else
            Request r(Request::Url(urls[i]),L"Range: bytes=0-"+std::to_wstring(sample-1)+L"\r\n",i==0,&stop);
#endif
            validateRange(r,0,sample-1,file.size);
            uint64_t got=0;char buffer[32768];
            while(got<sample) {
                cancelled(stop);auto n=r.read(buffer,(DWORD)std::min<uint64_t>(sizeof buffer,sample-got));
                if(!n)throw std::runtime_error("Incomplete speed sample");got+=n;
                if(std::chrono::steady_clock::now()-start>std::chrono::seconds(10))throw std::runtime_error("Speed sample timed out");
            }
            result.host=r.host();result.etag=header(r.request,WINHTTP_QUERY_ETAG);
            if(result.etag.starts_with(L"W/"))result.etag.clear();
            double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            result.bytesPerSecond=sample/std::max(seconds,.001);
        }catch(const std::exception& e){result.error=e.what();}
    });
    for(auto& thread:probes)thread.join();cancelled(stop);
    return results;
}
void download(const std::string& url, const File& file, const Path& destination, unsigned connections, std::atomic_bool& stop, const Report& report, const std::vector<std::string>& mirrors, const std::string& forcedSource, const SpeedReport& speeds, const std::string& identity) {
    const std::string selectedSource=forcedSource.empty()?url:forcedSource;
    // File lockers hand out a new download address on each visit; their page link names the file in the resume state.
    const std::string& key=identity.empty()?url:identity;
    const bool redirects=forcedSource.empty() || forcedSource==url;
    cancelled(stop);
    if (std::filesystem::exists(destination)) throw std::runtime_error("Destination already exists; it was left unchanged.");
    for (auto p = destination; !p.empty() && p != p.root_path(); p=p.parent_path()) regular(p);
    std::filesystem::create_directories(destination.parent_path());
    uint64_t size = file.size; bool known = file.sizeKnown, ranged = false;
    std::wstring etag;
    if (!known || size) {
        auto probe = get(selectedSource, L"Range: bytes=0-0\r\n", stop, redirects);
        ranged = probe->status == 206;
        if (ranged) {
            std::wsmatch m; auto value = header(probe->request, WINHTTP_QUERY_CONTENT_RANGE);
            if (!std::regex_match(value, m, std::wregex(L"bytes 0-0/([0-9]+)"))) throw std::runtime_error("Invalid server range response");
            auto actual = std::stoull(m[1]);
            if (known && actual != size) throw std::runtime_error("File size changed. Reload the item.");
            size = actual; known = true;
        } else {
            auto length = header(probe->request, WINHTTP_QUERY_CONTENT_LENGTH);
            if (!length.empty()) {
                auto actual = std::stoull(length);
                if (known && actual != size) throw std::runtime_error("File size changed. Reload the item.");
                size = actual; known = true;
            }
        }
        etag = header(probe->request, WINHTTP_QUERY_ETAG);
        if (etag.starts_with(L"W/")) etag.clear();
        LOG_INFO("Probe %s: HTTP %lu, size %s, byte ranges %s, ETag %s", logUrl(selectedSource).c_str(), (unsigned long)probe->status,
            known ? std::to_string(size).c_str() : "unknown", ranged ? "yes" : "no", etag.empty() ? "none" : "strong");
    }
    // Only reuse partial data when a content hash or strong server validator identifies it.
    const bool sha1Hash = std::regex_match(file.sha1, std::regex("[0-9a-fA-F]{40}"));
    const bool sha256Hash = std::regex_match(file.sha256, std::regex("[0-9a-fA-F]{64}"));
    bool resumable = ranged && (!file.sha1.empty() || sha256Hash || !etag.empty());
    // Without a stable identity, combining separate responses could mix file versions.
    if (!resumable) ranged = false;
    connections=std::clamp(connections,1u,16u);
    auto ranges = ranged ? queuedRanges(size) : split(size,1);
    struct Source { std::string url, host; std::wstring tag; double rate=0; };
    std::vector<Source> sources{{selectedSource,utf8(Request::Url(selectedSource).host),etag,0}};
    // Measure a small sample before choosing a storage server. Cross-server use
    // requires the Archive's whole-file hash; ETags are not portable between hosts.
    if (forcedSource.empty() && ranged && size >= 1024*1024 && !mirrors.empty() && sha1Hash) {
        if(report)report({0,size,"Checking mirror speeds (HTTPS)",0,""});
        std::vector<std::string> candidates{url};
        for(auto& mirror:mirrors) if(candidates.size()<3 && std::find(candidates.begin(),candidates.end(),mirror)==candidates.end())candidates.push_back(mirror);
        File sampleFile=file;sampleFile.size=size;sampleFile.sizeKnown=true;
        auto measured=measureServers(sampleFile,candidates,stop);
        if(speeds)speeds(measured);
        for(auto& result:measured){
            if(result.error.empty())LOG_INFO("Mirror %s: %.0f KiB/s",result.host.c_str(),result.bytesPerSecond/1024.0);
            else LOG_WARN("Mirror %s failed the speed test: %s",result.host.c_str(),result.error.c_str());
        }
        std::vector<Source> tested;
        for(auto& result:measured)if(result.error.empty())tested.push_back({result.url,result.host,result.etag,result.bytesPerSecond});
        std::stable_sort(tested.begin(),tested.end(),[](const Source& a,const Source& b){return a.rate>b.rate;});
        if(!tested.empty()) {
            if(std::none_of(tested.begin(),tested.end(),[&](const Source& s){return s.url==url;}))tested.push_back(sources[0]);
            sources=std::move(tested);
        }
    }
    Path parts = destination; parts += L".archive-parts";
    regular(parts);
    bool created = std::filesystem::create_directory(parts);
    auto ownerPath = parts / L"owner.txt";
    regular(ownerPath);
    const std::string owner = "SkeinDownloader staging v1";
    if (created) {
        std::ofstream out(ownerPath); out << owner; out.flush();
        if (!out) throw std::runtime_error("Cannot initialize download staging folder");
    } else {
        std::ifstream in(ownerPath); std::string value; std::getline(in,value);
        if (value != owner) {
            LOG_ERROR("Staging folder %s has owner \"%s\", not this app", pathUtf8(parts).c_str(), value.c_str());
            throw std::runtime_error("The partial-download folder already exists and is not owned by this app. Choose another destination.");
        }
    }
    auto lockPath = parts / L"lock"; regular(lockPath);
    platform::FileLock fileLock(lockPath);
    auto partPath = [&](size_t i) { return parts / (std::to_wstring(i) + L".part"); };
    regular(parts / L"state.json"); regular(parts / L"assembled.tmp");
    nlohmann::json previous;
    try { std::ifstream in(parts / L"state.json"); in >> previous; } catch (...) {}
    size_t oldCount=0;
    if(previous.is_object() && previous.contains("parts") && previous["parts"].is_number_unsigned()) {
        auto count=previous["parts"].get<uint64_t>();
        if(count<=16384)oldCount=(size_t)count;
    }
    // Honor the old partition map when upgrading an in-progress download.
    bool legacy=previous.is_object() && !previous.contains("layout") && oldCount>0 && oldCount<=8
        && previous.value("url",std::string())==key && previous.value("size",uint64_t(0))==size
        && previous.value("sha1",std::string())==file.sha1 && previous.value("ranged",false)==ranged;
    if(legacy)ranges=split(size,(unsigned)oldCount);
    const size_t cleanupCount=std::max({size_t(8),oldCount,ranges.size()});
    for(size_t i=0;i<cleanupCount;++i)regular(partPath(i));
    nlohmann::json state = {{"url",key},{"size",size},{"known",known},{"sha1",file.sha1},{"etag",utf8(etag)},{"parts",ranges.size()},{"ranged",ranged}};
    if(!legacy)state["layout"]="queued-v1";
    // Recorded only when present so manifests written before SHA-256 support still match.
    if(!file.sha256.empty())state["sha256"]=file.sha256;
    if(!sha1Hash&&!sha256Hash) {
        state["source"]=identity.empty()?selectedSource:identity;
        if(previous.is_object()&&!previous.contains("source"))previous["source"]=url;
    }
    // Server-specific ETags can change across redirects even when the content hash
    // is identical. Preserve verified-identity partials, including older manifests.
    if (previous.is_object() && (sha1Hash || sha256Hash)) previous["etag"]=state["etag"];
    if (!resumable || previous != state) {
        if (previous.is_object())
            LOG_INFO("Discarding partial data for %s: %s", pathUtf8(destination).c_str(),
                resumable ? "the file or its source changed" : "the server cannot resume this file");
        for (size_t i=0;i<cleanupCount;++i) std::filesystem::remove(partPath(i));
    }
    { std::ofstream out(parts / L"state.json", std::ios::trunc); out << state.dump(); if (!out) throw std::runtime_error("Cannot save resume state"); }
    std::mutex reportLock;
    std::atomic_uint64_t done = 0;
    std::vector<uint64_t> offsets(ranges.size());
    for(size_t i=0;i<ranges.size();++i) {
        auto path=partPath(i);
        if(resumable && std::filesystem::exists(path)) {
            auto length=ranges[i].last-ranges[i].first+1;
            offsets[i]=std::filesystem::file_size(path);
            if(offsets[i]>length){std::filesystem::remove(path);offsets[i]=0;}
            done+=offsets[i];
        }
    }
    std::atomic_uint active = 0;
    auto notify = [&](const std::string& status, const std::string& host = "") { std::lock_guard guard(reportLock); if (report) report({done.load(), known ? size : 0, status, active.load(), host.empty()?sources[0].host:host}); };
    auto receive = [&](size_t i, bool useRange, size_t& sourceIndex) {
        uint64_t length = known ? (useRange ? ranges[i].last-ranges[i].first+1 : size) : UINT64_MAX;
        auto path = partPath(i); uint64_t offset = i<offsets.size()?offsets[i]:0;
        for (unsigned attempt=0;;++attempt) {
            cancelled(stop);
            if (known && offset == length) break;
            try {
                const auto& source=sources[sourceIndex];
                std::wstring headers;
                if (useRange) {
                    headers = L"Range: bytes=" + std::to_wstring(ranges[i].first+offset) + L"-" + std::to_wstring(ranges[i].last) + L"\r\n";
                    if (!source.tag.empty()) headers += L"If-Range: " + source.tag + L"\r\n";
                } else { done -= offset; offset=0; }
                auto r = get(source.url, headers, stop, redirects);
                if (useRange) validateRange(*r, ranges[i].first+offset, ranges[i].last, size);
                else if (r->status != 200) throw std::runtime_error("Unexpected partial response");
                auto effectiveHost=r->host();
                struct Active { std::atomic_uint& count; Active(std::atomic_uint& c):count(c){++count;} ~Active(){--count;} } activeRequest(active);
                std::ofstream out(path, std::ios::binary | (offset ? std::ios::app : std::ios::trunc));
                if (!out) throw std::runtime_error("Cannot create partial file");
                char buffer[65536];
                for (;;) {
                    cancelled(stop);
                    DWORD n = r->read(buffer, sizeof buffer); if (!n) break;
                    if (known && n > length-offset) throw std::runtime_error("Server sent more bytes than requested");
                    out.write(buffer,n); if (!out) throw std::runtime_error("Disk write failed. Check available space.");
                    offset += n; done += n; notify("Downloading",effectiveHost);
                }
                out.flush(); if (!out) throw std::runtime_error("Cannot flush downloaded data");
                if (known && offset != length) throw NetworkError("Server closed the connection before the file was complete");
                break;
            } catch (const NetworkError& e) {
                const auto& failedHost = sources[sourceIndex].host;
                if (attempt == 3 || (!e.retry && sources.size()==1)) {
                    LOG_ERROR("Part %zu from %s failed after %u attempts: %s", i, failedHost.c_str(), attempt + 1, e.what());
                    throw;
                }
                LOG_WARN("Part %zu from %s failed (attempt %u): %s; %s", i, failedHost.c_str(), attempt + 1, e.what(),
                    sources.size() > 1 ? "trying another source" : "retrying");
                sourceIndex=(sourceIndex+1)%sources.size();
                notify(sources.size()>1?"Trying another HTTPS source":"Retrying after connection error");
                if(e.retry)delay(std::max(e.wait, 1u << attempt), stop);
            }
        }
    };
    {
        std::string hosts; for (auto& s : sources) hosts += (hosts.empty() ? "" : ", ") + s.host;
        LOG_INFO("Downloading %s: %s, %zu part(s), %u connection(s), %llu bytes already on disk, sources %s", pathUtf8(destination).c_str(),
            ranged ? "parallel" : "single connection", ranges.size(), ranged ? connections : 1u, (unsigned long long)done.load(), hosts.c_str());
    }
    notify(ranged ? "Starting parallel download" : "Starting single connection");
    if (ranged && size) {
        std::mutex errorLock; std::exception_ptr error;
        std::atomic_size_t next=0;std::atomic_bool failed=false;
        std::vector<std::jthread> threads;
        for (size_t worker=0;worker<std::min<size_t>(connections,ranges.size());++worker) threads.emplace_back([&] {
            size_t sourceIndex=0;
            try {
                while(!failed && !stop) {
                    auto i=next.fetch_add(1);if(i>=ranges.size())break;
                    receive(i,true,sourceIndex);
                }
            } catch (...) { failed=true;std::lock_guard lock(errorLock); if(!error) error=std::current_exception(); }
        });
        for (auto& t:threads) t.join();
        if (error) std::rethrow_exception(error);
    } else if (!known || size) {size_t sourceIndex=0;receive(0,false,sourceIndex);}
    cancelled(stop); notify("Assembling");
    auto assembled = parts / L"assembled.tmp";
    {
        std::ofstream out(assembled, std::ios::binary|std::ios::trunc);
        if (!out) throw std::runtime_error("Cannot assemble download");
        size_t count = ranged ? ranges.size() : ((!known || size) ? 1 : 0);
        for (size_t i=0;i<count;++i) {
            std::ifstream in(partPath(i), std::ios::binary);
            if (!in) throw std::runtime_error("Partial file is missing");
            char buffer[65536];
            while (in) { cancelled(stop); in.read(buffer,sizeof buffer); out.write(buffer,in.gcount()); if(!out) throw std::runtime_error("Disk full while assembling download"); }
            if (!in.eof()) throw std::runtime_error("Cannot read partial file");
        }
        out.flush(); if (!out) throw std::runtime_error("Cannot flush completed file");
    }
    if (known && std::filesystem::file_size(assembled) != size) throw std::runtime_error("Completed file has the wrong size");
    // SHA-256 is preferred when a source provides it (Hugging Face); SHA-1 comes from archive.org.
    if (!file.sha256.empty() || !file.sha1.empty()) {
        const bool useSha256 = !file.sha256.empty();
        notify(useSha256 ? "Verifying SHA-256" : "Verifying SHA-1"); auto expected = useSha256 ? file.sha256 : file.sha1;
        std::transform(expected.begin(),expected.end(),expected.begin(),[](unsigned char c){return (char)tolower(c);});
        const auto actual = useSha256 ? sha256File(assembled,stop) : sha1File(assembled,stop);
        if (actual != expected) {
            LOG_ERROR("%s mismatch for %s: expected %s, got %s", useSha256 ? "SHA-256" : "SHA-1", pathUtf8(destination).c_str(), expected.c_str(), actual.c_str());
            for (size_t i=0;i<cleanupCount;++i) std::filesystem::remove(partPath(i));
            std::filesystem::remove(assembled);
            throw std::runtime_error("Checksum mismatch. Partial data was discarded; retry the download.");
        }
    }
    cancelled(stop);
    platform::publish(assembled,destination);
    for (size_t i=0;i<cleanupCount;++i) std::filesystem::remove(partPath(i));
    std::filesystem::remove(parts/L"state.json");
    fileLock.removeAndUnlock();
    std::filesystem::remove(ownerPath);
    std::error_code ignored; std::filesystem::remove(parts,ignored);
    LOG_INFO("Saved %s (%llu bytes)", pathUtf8(destination).c_str(), (unsigned long long)(known ? size : done.load()));
    notify("Complete");
}
}
