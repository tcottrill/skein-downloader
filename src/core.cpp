#include "core.h"
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#endif
#include "json.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <stdexcept>
namespace archive {
#ifdef _WIN32
#include "platform/system_windows.inc"
#else
#include "platform/system_linux.inc"
#endif
Path pathFromUtf8(const std::string& value) {
#ifdef _WIN32
    return Path(wide(value));
#else
    (void)wide(value); return Path(value);
#endif
}
std::string pathUtf8(const Path& value) {
#ifdef _WIN32
    return utf8(value.wstring());
#else
    return value.string();
#endif
}
std::string decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%') {
            if (i + 2 >= s.size() || !isxdigit((unsigned char)s[i+1]) || !isxdigit((unsigned char)s[i+2]))
                throw std::runtime_error("Invalid URL escape");
            out += (char)std::stoi(s.substr(i+1, 2), nullptr, 16); i += 2;
        } else out += s[i];
    }
    return out;
}
std::string encode(const std::string& s, bool keepSlash) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~' || (keepSlash && c == '/')) out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
Link parseLink(const std::string& input) {
    auto a = input.find_first_not_of(" \t\r\n"), b = input.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) throw std::runtime_error("Paste an Archive.org link or item identifier.");
    std::string s = input.substr(a, b-a+1);
    Link link;
    if (s.find("://") != std::string::npos) {
        std::smatch match;
        if (!std::regex_match(s, match, std::regex(R"(https?://(?:www\.)?archive\.org/(?:details|download)/([^/?#]+)([^?#]*)(?:[?#].*)?)", std::regex::icase)))
            throw std::runtime_error("This is not a valid download link.");
        link.id = decode(match[1]);
        auto tail = match[2].str();
        if (tail.size() > 1) link.file = decode(tail.substr(1));
    } else link.id = s;
    if (!std::regex_match(link.id, std::regex("[A-Za-z0-9_][A-Za-z0-9_.-]{0,99}")))
        throw std::runtime_error("Invalid Archive.org item identifier.");
    return link;
}
bool isArchiveLink(const std::string& input) {
    try { parseLink(input); return true; } catch (const std::exception&) { return false; }
}
static std::string trimmed(const std::string& input) {
    auto a = input.find_first_not_of(" \t\r\n"), b = input.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : input.substr(a, b-a+1);
}
bool isHttpUrl(const std::string& input) {
    static const std::regex url(R"(https?://[^/?#\s@]+(?:[/?#]\S*)?)", std::regex::icase);
    return std::regex_match(trimmed(input), url);
}
std::string logUrl(const std::string& url) {
    const auto end = url.find_first_of("?#");
    return end == std::string::npos ? url : url.substr(0, end) + "?...";
}
// The last path segment of a URL, percent-decoded; empty when the path ends in '/'.
static std::string lastSegment(const std::string& input) {
    auto s = trimmed(input);
    auto scheme = s.find("://"); if (scheme == std::string::npos) return {};
    auto pathStart = s.find('/', scheme + 3); if (pathStart == std::string::npos) return {};
    auto path = s.substr(pathStart, s.find_first_of("?#", pathStart) - pathStart);
    auto name = path.substr(path.rfind('/') + 1);
    try { return decode(name); } catch (const std::exception&) { return name; }
}
bool looksLikeFileUrl(const std::string& url) {
    if (!isHttpUrl(url)) return false;
    auto name = lastSegment(url); auto dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    auto extension = name.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)tolower(c); });
    static const std::vector<std::string> known = {
        "zip","7z","rar","tar","gz","tgz","bz2","xz","zst","lz","lzma","cab",
        "iso","img","bin","cue","vhd","vhdx","vmdk","qcow2","ova",
        "exe","msi","msix","appx","dmg","pkg","deb","rpm","apk","appimage","flatpak","snap","jar",
        "pdf","epub","mobi","cbz","cbr","djvu","csv","json","xml","txt",
        "mp3","flac","wav","ogg","opus","m4a","aac","mp4","mkv","avi","mov","webm","m4v","wmv"};
    return std::find(known.begin(), known.end(), extension) != known.end();
}
std::string filenameFromDisposition(const std::string& header) {
    auto basename = [](std::string name) { auto at = name.find_last_of("/\\"); return at == std::string::npos ? name : name.substr(at + 1); };
    std::smatch m;
    // RFC 6266: filename* (charset''percent-encoded) takes priority over filename.
    static const std::regex extended(R"(filename\*\s*=\s*([A-Za-z0-9_-]+)'[^']*'([^;\s]+))", std::regex::icase);
    if (std::regex_search(header, m, extended)) {
        auto charset = m[1].str(); std::transform(charset.begin(), charset.end(), charset.begin(), [](unsigned char c) { return (char)tolower(c); });
        if (charset == "utf-8") try { return basename(decode(m[2])); } catch (const std::exception&) {}
    }
    static const std::regex quoted(R"re(filename\s*=\s*"((?:[^"\\]|\\.)*)")re", std::regex::icase);
    if (std::regex_search(header, m, quoted)) {
        std::string out; auto value = m[1].str();
        // Only \" and \\ are escapes; other backslashes stay so basename() drops Windows path parts.
        for (size_t i = 0; i < value.size(); ++i) { if (value[i] == '\\' && i + 1 < value.size() && (value[i+1] == '"' || value[i+1] == '\\')) ++i; out += value[i]; }
        return basename(out);
    }
    static const std::regex token(R"(filename\s*=\s*([^;\s"]+))", std::regex::icase);
    if (std::regex_search(header, m, token)) return basename(m[1]);
    return {};
}
std::string filenameFromUrl(const std::string& url) { return lastSegment(url); }
std::string safeFilename(std::string name) {
    // Server text may not be valid UTF-8 (often Latin-1); keep ASCII and replace the rest.
    try { wide(name); } catch (const std::exception&) { for (auto& c : name) if ((unsigned char)c >= 0x80) c = '_'; }
    for (auto& c : name) if ((unsigned char)c < 32 || std::string("<>:\"/\\|?*").find(c) != std::string::npos) c = '_';
    // Keep names well under filesystem limits, cutting only at a UTF-8 character boundary.
    if (name.size() > 200) { size_t cut = 200; while (cut && ((unsigned char)name[cut] & 0xC0) == 0x80) --cut; name.resize(cut); }
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    if (name.empty()) return "download";
    auto stem = name.substr(0, name.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return (char)toupper(c); });
    if (std::regex_match(stem, std::regex("CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9]"))) name = "_" + name;
    return name;
}
HubLink parseHubLink(const std::string& input) {
    std::smatch match;
    static const std::regex hub(R"(https?://(?:www\.)?(?:huggingface\.co|hf\.co)/([^?#]*)(?:[?#].*)?)", std::regex::icase);
    auto s = trimmed(input);
    if (!std::regex_match(s, match, hub)) throw std::runtime_error("Not a Hugging Face link.");
    std::vector<std::string> parts; std::istringstream in(match[1].str()); std::string part;
    while (std::getline(in, part, '/')) if (!part.empty()) parts.push_back(part);
    HubLink link;
    if (!parts.empty() && (parts[0] == "datasets" || parts[0] == "spaces")) { link.kind = parts[0]; parts.erase(parts.begin()); }
    else link.kind = "models";
    // Site pages that share the URL space with repositories.
    static const std::vector<std::string> site = {"models","datasets","spaces","docs","blog","api","settings","login","join","pricing",
        "organizations","new","collections","papers","posts","tasks","learn","enterprise","chat","search","notifications","inference-endpoints"};
    if (parts.size() < 2 || (link.kind == "models" && std::find(site.begin(), site.end(), parts[0]) != site.end()))
        throw std::runtime_error("Paste a link to a Hugging Face repository, such as huggingface.co/owner/model.");
    static const std::regex name("[A-Za-z0-9][A-Za-z0-9._-]{0,95}");
    if (!std::regex_match(parts[0], name) || !std::regex_match(parts[1], name) || parts[1].ends_with(".") )
        throw std::runtime_error("Invalid Hugging Face repository name.");
    link.repo = parts[0] + "/" + parts[1];
    if (parts.size() >= 4 && (parts[2] == "tree" || parts[2] == "blob" || parts[2] == "resolve")) {
        link.revision = decode(parts[3]);
        static const std::regex revision(R"([A-Za-z0-9._/-]{1,200})");
        if (!std::regex_match(link.revision, revision) || link.revision.find("..") != std::string::npos)
            throw std::runtime_error("Invalid Hugging Face revision.");
        for (size_t i = 4; i < parts.size(); ++i) {
            auto segment = decode(parts[i]);
            if (segment == "." || segment == "..") throw std::runtime_error("Invalid Hugging Face file path.");
            link.path += (link.path.empty() ? "" : "/") + segment;
        }
        link.file = parts[2] != "tree";
        if (link.file && link.path.empty()) throw std::runtime_error("Hugging Face file link has no file path.");
    }
    return link;
}
bool isHubLink(const std::string& input) {
    try { parseHubLink(input); return true; } catch (const std::exception&) { return false; }
}
std::vector<File> parseHubTree(const std::string& text) {
    auto j = nlohmann::json::parse(text);
    if (j.is_object() && j.contains("error")) throw std::runtime_error("Hugging Face: " + (j["error"].is_string() ? j["error"].get<std::string>() : j["error"].dump()));
    if (!j.is_array()) throw std::runtime_error("Unexpected Hugging Face file list.");
    std::vector<File> files;
    for (auto& entry : j) {
        if (!entry.is_object() || entry.value("type", std::string()) != "file" || !entry.contains("path") || !entry["path"].is_string()) continue;
        File file; file.name = entry["path"].get<std::string>();
        if (entry.contains("size") && entry["size"].is_number_unsigned()) { file.size = entry["size"].get<uint64_t>(); file.sizeKnown = true; }
        // Large files are stored through Git LFS/Xet; their oid is the file's SHA-256.
        if (entry.contains("lfs") && entry["lfs"].is_object()) {
            auto& lfs = entry["lfs"];
            if (lfs.contains("oid") && lfs["oid"].is_string()) {
                auto oid = lfs["oid"].get<std::string>(); if (oid.starts_with("sha256:")) oid = oid.substr(7);
                if (std::regex_match(oid, std::regex("[0-9a-fA-F]{64}"))) file.sha256 = oid;
            }
            if (lfs.contains("size") && lfs["size"].is_number_unsigned()) { file.size = lfs["size"].get<uint64_t>(); file.sizeKnown = true; }
        }
        auto base = file.name.substr(file.name.rfind('/') + 1); auto dot = base.rfind('.');
        if (dot != std::string::npos && dot + 1 < base.size()) file.format = base.substr(dot + 1);
        files.push_back(std::move(file));
    }
    return files;
}
GitHubLink parseGitHubLink(const std::string& input) {
    std::smatch match;
    static const std::regex github(R"(https?://(?:www\.)?github\.com/([^?#]*)(?:[?#].*)?)", std::regex::icase);
    auto s = trimmed(input);
    if (!std::regex_match(s, match, github)) throw std::runtime_error("Not a GitHub link.");
    std::vector<std::string> parts; std::istringstream in(match[1].str()); std::string part;
    while (std::getline(in, part, '/')) if (!part.empty()) parts.push_back(part);
    static const std::regex owner("[A-Za-z0-9](?:[A-Za-z0-9-]{0,38})"), repo("[A-Za-z0-9._-]{1,100}");
    static const std::vector<std::string> site = {"settings","orgs","marketplace","topics","explore","features","login","logout","signup","sponsors",
        "about","pricing","enterprise","collections","trending","notifications","new","search","apps","codespaces","issues","pulls","security","customer-stories"};
    if (parts.size() < 2 || !std::regex_match(parts[0], owner) || !std::regex_match(parts[1], repo) || parts[1] == "." || parts[1] == ".."
        || std::find(site.begin(), site.end(), parts[0]) != site.end())
        throw std::runtime_error("Paste a GitHub release link, such as github.com/owner/repo/releases/tag/v1.0.");
    GitHubLink link; link.owner = parts[0]; link.repo = parts[1];
    if (link.repo.ends_with(".git")) link.repo.resize(link.repo.size() - 4);
    // Only the repo itself (latest release) and release pages; code, issues and other pages are not downloads.
    if (parts.size() == 2) return link;
    if (parts[2] != "releases") throw std::runtime_error("Only GitHub release pages can be downloaded. Open the repository's Releases page and copy that link.");
    if (parts.size() == 3 || (parts.size() == 4 && parts[3] == "latest")) return link;
    auto joinDecoded = [&](size_t from, size_t to) { std::string out; for (size_t i = from; i < to; ++i) out += (out.empty() ? "" : "/") + decode(parts[i]); return out; };
    if (parts[3] == "tag" && parts.size() >= 5) link.tag = joinDecoded(4, parts.size());
    else if (parts[3] == "download" && parts.size() >= 6) { link.tag = joinDecoded(4, parts.size() - 1); link.file = decode(parts.back()); }
    else throw std::runtime_error("Paste a GitHub release link, such as github.com/owner/repo/releases/tag/v1.0.");
    if (link.tag.empty() || link.tag.size() > 250 || link.tag.find("..") != std::string::npos) throw std::runtime_error("Invalid GitHub release tag.");
    return link;
}
bool isGitHubLink(const std::string& input) {
    try { parseGitHubLink(input); return true; } catch (const std::exception&) { return false; }
}
Item parseGitHubRelease(const std::string& text, const GitHubLink& link) {
    auto j = nlohmann::json::parse(text);
    if (!j.is_object() || !j.contains("tag_name") || !j["tag_name"].is_string())
        throw std::runtime_error(j.is_object() && j.contains("message") && j["message"].is_string() ? "GitHub: " + j["message"].get<std::string>() : "Unexpected GitHub release data.");
    auto tag = j["tag_name"].get<std::string>();
    Item item; item.title = j.contains("name") && j["name"].is_string() && !j["name"].get<std::string>().empty() ? j["name"].get<std::string>() : tag;
    // Saved beneath owner/repo/tag; the tag becomes one safe folder name.
    item.id = link.owner + "/" + link.repo + "/" + safeFilename(tag);
    if (j.contains("assets") && j["assets"].is_array()) for (auto& asset : j["assets"]) {
        if (!asset.is_object() || !asset.contains("name") || !asset["name"].is_string() || !asset.contains("browser_download_url") || !asset["browser_download_url"].is_string()) continue;
        auto url = asset["browser_download_url"].get<std::string>();
        // Asset downloads must come from GitHub itself (they redirect to its CDN).
        if (!url.starts_with("https://github.com/")) continue;
        File file; file.name = safeFilename(asset["name"].get<std::string>()); file.url = url;
        if (asset.contains("size") && asset["size"].is_number_unsigned()) { file.size = asset["size"].get<uint64_t>(); file.sizeKnown = true; }
        if (asset.contains("digest") && asset["digest"].is_string()) {
            auto digest = asset["digest"].get<std::string>();
            if (digest.starts_with("sha256:") && std::regex_match(digest.substr(7), std::regex("[0-9a-fA-F]{64}"))) file.sha256 = digest.substr(7);
        }
        auto dot = file.name.rfind('.'); if (dot != std::string::npos && dot + 1 < file.name.size()) file.format = file.name.substr(dot + 1);
        item.files.push_back(std::move(file));
    }
    // The "Source code" archives GitHub shows on every release; sizes are not published.
    for (auto [extension, format] : {std::pair{".zip", "Source code"}, std::pair{".tar.gz", "Source code"}}) {
        File file; file.name = safeFilename(link.repo + "-" + tag) + extension; file.format = format;
        file.url = "https://github.com/" + link.owner + "/" + link.repo + "/archive/refs/tags/" + encode(tag, true) + extension;
        if (std::none_of(item.files.begin(), item.files.end(), [&](const File& f) { return f.name == file.name; })) item.files.push_back(std::move(file));
    }
    if (!link.file.empty()) {
        auto name = safeFilename(link.file);
        if (std::none_of(item.files.begin(), item.files.end(), [&](const File& f) { return f.name == name; }))
            throw std::runtime_error("That file is not in the release: " + link.file);
        item.selectedFile = name;
    } else item.checkAll = true;
    return item;
}
bool hubRepoGated(const std::string& text) {
    auto j = nlohmann::json::parse(text);
    if (!j.is_object() || !j.contains("gated")) return false;
    auto& gated = j["gated"];
    return gated.is_string() ? !gated.get<std::string>().empty() : gated.is_boolean() && gated.get<bool>();
}
std::string hubNextPage(const std::string& header) {
    std::smatch m;
    static const std::regex next(R"re(<([^>]+)>\s*;\s*rel="?next"?)re", std::regex::icase);
    if (!std::regex_search(header, m, next)) return {};
    // Pagination must stay on the Hub API; never follow a link elsewhere.
    auto url = m[1].str();
    return url.starts_with("https://huggingface.co/api/") ? url : std::string();
}
Path safePath(const Path& root, const std::string& name) {
    if (name.empty() || name.front() == '/' || name.back() == '/' || name.find('\\') != std::string::npos)
        throw std::runtime_error("Unsafe filename: " + name);
    Path result = std::filesystem::absolute(root).lexically_normal();
    std::istringstream in(name); std::string part;
    while (std::getline(in, part, '/')) {
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ' ||
            part.find_first_of("<>:\"|?*") != std::string::npos ||
            std::any_of(part.begin(), part.end(), [](unsigned char c) { return c < 32; }))
            throw std::runtime_error("Filename cannot be saved safely on Windows: " + name);
        auto stem = part.substr(0, part.find('.'));
        std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return (char)toupper(c); });
        if (std::regex_match(stem, std::regex("CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9]")))
            throw std::runtime_error("Reserved Windows filename: " + name);
        result /= pathFromUtf8(part);
    }
    // Existing junctions/symlinks must not redirect writes outside the chosen folder.
    for (auto p = result; !p.empty() && p != p.root_path(); p = p.parent_path()) {
#ifdef _WIN32
        DWORD attrs = GetFileAttributesW(p.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Destination contains a link or junction. Choose a regular folder.");
#else
        if(std::filesystem::is_symlink(std::filesystem::symlink_status(p))) throw std::runtime_error("Destination contains a symbolic link.");
#endif
    }
    return result;
}
std::vector<Range> split(uint64_t size, unsigned connections) {
    std::vector<Range> out;
    if (!size) return out;
    unsigned count = (unsigned)std::min<uint64_t>(size, std::clamp(connections, 1u, 8u));
    uint64_t first = 0;
    for (unsigned i = 0; i < count; ++i) {
        uint64_t length = size / count + (i < size % count ? 1 : 0);
        out.push_back({first, first + length - 1}); first += length;
    }
    return out;
}
Item parseMetadata(const std::string& id, const std::string& text) {
    auto j = nlohmann::json::parse(text);
    if (!j.is_object()) throw std::runtime_error("Item not found.");
    if (j.contains("error")) throw std::runtime_error("Archive.org: " + j["error"].dump());
    if (!j.contains("files") || !j["files"].is_array()) throw std::runtime_error("This item has no downloadable files.");
    Item item; item.id = id; item.title = id;
    if (j.contains("dir") && j["dir"].is_string()) {
        auto dir = j["dir"].get<std::string>();
        // The path must be this item's storage directory, never a URL or arbitrary path.
        std::smatch match;
        if (std::regex_match(dir, match, std::regex(R"(/[0-9]+/items/([^/]+))")) && match[1] == id) {
            auto add = [&](const nlohmann::json& value) {
                if (!value.is_string()) return;
                auto host = value.get<std::string>();
                if (!std::regex_match(host, std::regex(R"([a-zA-Z0-9-]+\.(?:us|ca|eu)\.archive\.org)"))) return;
                auto base = "https://" + host + dir + "/";
                if (std::find(item.mirrorBases.begin(), item.mirrorBases.end(), base) == item.mirrorBases.end() && item.mirrorBases.size() < 2)
                    item.mirrorBases.push_back(base);
            };
            if (j.contains("workable_servers") && j["workable_servers"].is_array()) for (auto& value:j["workable_servers"]) add(value);
            for (auto key:{"d1","d2"}) if(j.contains(key)) add(j[key]);
        }
    }
    if (j.contains("metadata") && j["metadata"].is_object()) {
        auto& m = j["metadata"];
        if (m.contains("title") && m["title"].is_string()) item.title = m["title"].get<std::string>();
    }
    for (auto& f : j["files"]) {
        if (!f.is_object() || !f.contains("name") || !f["name"].is_string()) continue;
        if (f.contains("private") && f["private"] != false && f["private"] != "false" && f["private"] != "0" && f["private"] != 0) continue;
        File file; file.name = f["name"].get<std::string>();
        if (f.contains("format") && f["format"].is_string()) file.format = f["format"].get<std::string>();
        if (f.contains("sha1") && f["sha1"].is_string()) file.sha1 = f["sha1"].get<std::string>();
        if (f.contains("size")) {
            auto value = f["size"].is_string() ? f["size"].get<std::string>() : f["size"].dump();
            if (!value.empty() && value.find_first_not_of("0123456789") == std::string::npos) {
                try { file.size = std::stoull(value); file.sizeKnown = true; } catch (...) {}
            }
        }
        item.files.push_back(std::move(file));
    }
    return item;
}
std::string formatBytes(uint64_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = (double)bytes; int unit = 0;
    while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
    std::ostringstream s; s << std::fixed << std::setprecision(unit ? 1 : 0) << value << ' ' << units[unit]; return s.str();
}

}
