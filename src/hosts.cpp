// Sites with public file APIs: Codeberg/Gitea and GitLab releases, Zenodo, PyPI, npm,
// crates.io, and Dropbox share links. Each turns a page link into an Item of files.
#include "sites.h"
#include "json.hpp"
#include <algorithm>
#include <regex>
#include <sstream>
#include <stdexcept>
namespace archive {
using nlohmann::json;
namespace {
std::string lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); }); return s; }
std::string text(const json& j, const char* key) { return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string(); }
json parse(const std::string& body, const char* site) {
    try { return json::parse(body); } catch (const json::exception&) { throw std::runtime_error(std::string("Unexpected data from ") + site + "."); }
}
// A file with its own download link; size is taken when the site gives a number.
File remoteFile(const std::string& name, const std::string& url, const json& size) {
    File file; file.name = safeFilename(name); file.url = url;
    if (size.is_number_unsigned()) { file.size = size.get<uint64_t>(); file.sizeKnown = true; }
    auto dot = file.name.rfind('.'); if (dot != std::string::npos && dot + 1 < file.name.size()) file.format = file.name.substr(dot + 1);
    return file;
}
bool hexDigest(const std::string& value, size_t length) { return value.size() == length && std::all_of(value.begin(), value.end(), [](unsigned char c) { return isxdigit(c); }); }
std::string joinDecoded(const std::vector<std::string>& parts, size_t from, size_t to) {
    std::string out; for (size_t i = from; i < to; ++i) out += (out.empty() ? "" : "/") + decode(parts[i]); return out;
}
// A release link names one file, or else every file starts checked.
void selectReleaseFile(Item& item, const std::string& wanted) {
    if (wanted.empty()) { item.checkAll = true; return; }
    auto name = safeFilename(wanted.substr(wanted.rfind('/') + 1));
    if (std::none_of(item.files.begin(), item.files.end(), [&](const File& f) { return f.name == name; }))
        throw std::runtime_error("That file is not in the release: " + wanted);
    item.selectedFile = name;
}
// Release and API responses can be large, but never this large.
Response fetchData(const std::string& url, std::atomic_bool& stop, const std::string& headers = "Accept: application/json\r\n") {
    auto r = fetch(url, stop, headers);
    if (r.file) throw std::runtime_error("The site answered with a file where its file list was expected.");
    return r;
}
bool startsWithStatus(const std::exception& e, std::initializer_list<const char*> codes) {
    std::string message = e.what();
    return std::any_of(codes.begin(), codes.end(), [&](const char* code) { return message.starts_with(std::string("HTTP ") + code); });
}
}
bool splitUrl(const std::string& input, UrlParts& out) {
    auto a = input.find_first_not_of(" \t\r\n"), b = input.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return false;
    static const std::regex url(R"(https?://([^/?#\s@:]+)(?::[0-9]+)?([^?#\s]*)(?:\?([^#\s]*))?(?:#\S*)?)", std::regex::icase);
    std::smatch m; auto s = input.substr(a, b - a + 1);
    if (!std::regex_match(s, m, url)) return false;
    out = {}; out.host = lower(m[1]); if (out.host.starts_with("www.")) out.host.erase(0, 4);
    out.query = m[3];
    std::istringstream in(m[2].str()); std::string part;
    while (std::getline(in, part, '/')) if (!part.empty()) out.path.push_back(part);
    return true;
}
std::string queryValue(const std::string& query, const std::string& key) {
    std::istringstream in(query); std::string pair;
    while (std::getline(in, pair, '&')) {
        auto eq = pair.find('=');
        if (pair.substr(0, eq) != key) continue;
        auto value = eq == std::string::npos ? std::string() : pair.substr(eq + 1);
        std::replace(value.begin(), value.end(), '+', ' ');
        try { return decode(value); } catch (const std::exception&) { return value; }
    }
    return {};
}

ForgeLink parseForgeLink(const std::string& input) {
    UrlParts u; if (!splitUrl(input, u)) throw std::runtime_error("Not a Codeberg, Gitea or GitLab link.");
    auto& p = u.path;
    auto dash = std::find(p.begin(), p.end(), "-");
    ForgeLink link; link.host = u.host;
    link.gitlab = u.host == "gitlab.com" || (dash != p.end() && dash + 1 != p.end() && *(dash + 1) == "releases");
    if (!link.gitlab && u.host != "codeberg.org" && u.host != "gitea.com") throw std::runtime_error("Not a Codeberg, Gitea or GitLab link.");
    static const std::regex name("[A-Za-z0-9_][A-Za-z0-9_.-]{0,99}");
    if (link.gitlab) {
        static const std::string usage = "Paste a GitLab release link, such as gitlab.com/group/project/-/releases/v1.0.";
        std::vector<std::string> project(p.begin(), dash), rest(dash == p.end() ? dash : dash + 1, p.end());
        static const std::vector<std::string> site = {"explore","users","help","dashboard","admin","api","search","projects","groups","uploads"};
        if (project.size() < 2 || std::find(site.begin(), site.end(), project[0]) != site.end()
            || std::any_of(project.begin(), project.end(), [](const std::string& s) { return !std::regex_match(s, name) || s.ends_with(".git"); }))
            throw std::runtime_error(usage);
        for (auto& segment : project) link.project += (link.project.empty() ? "" : "/") + segment;
        // The project itself (latest release) and release pages; other project pages are not downloads.
        if (rest.empty()) return link;
        if (rest[0] != "releases") throw std::runtime_error("Only GitLab release pages can be downloaded. Open the project's Releases page and copy that link.");
        if (rest.size() == 1 || (rest.size() == 3 && rest[1] == "permalink" && rest[2] == "latest")) return link;
        link.tag = decode(rest[1]);
        if (rest.size() >= 4 && rest[2] == "downloads") link.file = joinDecoded(rest, 3, rest.size());
        else if (rest.size() != 2) throw std::runtime_error(usage);
    } else {
        static const std::string usage = "Paste a release link, such as codeberg.org/owner/repo/releases/tag/v1.0.";
        if (p.size() < 2 || !std::regex_match(p[0], name) || !std::regex_match(p[1], name)) throw std::runtime_error(usage);
        static const std::vector<std::string> site = {"explore","user","admin","api","assets","repo","org","notifications","issues","pulls","milestones"};
        if (std::find(site.begin(), site.end(), p[0]) != site.end()) throw std::runtime_error(usage);
        link.project = p[0] + "/" + (p[1].ends_with(".git") ? p[1].substr(0, p[1].size() - 4) : p[1]);
        if (p.size() == 2) return link;
        if (p[2] != "releases") throw std::runtime_error("Only release pages can be downloaded. Open the repository's Releases page and copy that link.");
        if (p.size() == 3 || (p.size() == 4 && p[3] == "latest")) return link;
        if (p[3] == "tag" && p.size() >= 5) link.tag = joinDecoded(p, 4, p.size());
        else if (p[3] == "download" && p.size() >= 6) { link.tag = joinDecoded(p, 4, p.size() - 1); link.file = decode(p.back()); }
        else throw std::runtime_error(usage);
    }
    if (link.tag.empty() || link.tag.size() > 250 || link.tag.find("..") != std::string::npos) throw std::runtime_error("Invalid release tag.");
    return link;
}
bool isForgeLink(const std::string& input) {
    try { parseForgeLink(input); return true; } catch (const std::exception&) { return false; }
}
Item parseGiteaRelease(const std::string& body, const ForgeLink& link) {
    auto j = parse(body, link.host.c_str());
    auto tag = text(j, "tag_name");
    if (tag.empty()) throw std::runtime_error(!text(j, "message").empty() ? link.host + ": " + text(j, "message") : "Unexpected release data.");
    Item item; item.title = text(j, "name").empty() ? tag : text(j, "name");
    item.id = link.project + "/" + safeFilename(tag);
    // Downloads must come from the same site.
    auto site = "https://" + link.host + "/";
    if (j.contains("assets") && j["assets"].is_array()) for (auto& asset : j["assets"]) {
        auto url = text(asset, "browser_download_url");
        if (text(asset, "name").empty() || !url.starts_with(site)) continue;
        item.files.push_back(remoteFile(text(asset, "name"), url, asset.value("size", json())));
    }
    auto repo = link.project.substr(link.project.rfind('/') + 1);
    for (auto [key, extension] : {std::pair{"zipball_url", ".zip"}, std::pair{"tarball_url", ".tar.gz"}}) {
        auto url = text(j, key); if (!url.starts_with(site)) continue;
        auto file = remoteFile(safeFilename(repo + "-" + tag) + extension, url, json()); file.format = "Source code";
        if (std::none_of(item.files.begin(), item.files.end(), [&](const File& f) { return f.name == file.name; })) item.files.push_back(std::move(file));
    }
    selectReleaseFile(item, link.file);
    return item;
}
Item parseGitLabRelease(const std::string& body, const ForgeLink& link) {
    auto j = parse(body, "GitLab");
    auto tag = text(j, "tag_name");
    if (tag.empty()) throw std::runtime_error(!text(j, "message").empty() ? "GitLab: " + text(j, "message") : "Unexpected GitLab release data.");
    Item item; item.title = text(j, "name").empty() ? tag : text(j, "name");
    item.id = link.project + "/" + safeFilename(tag);
    auto site = "https://" + link.host + "/";
    json assets = j.contains("assets") && j["assets"].is_object() ? j["assets"] : json::object();
    if (assets.contains("links") && assets["links"].is_array()) for (auto& asset : assets["links"]) {
        // Release links can point anywhere; only files on the same GitLab are listed.
        auto url = text(asset, "direct_asset_url"); if (!url.starts_with(site)) url = text(asset, "url");
        if (text(asset, "name").empty() || !url.starts_with(site)) continue;
        item.files.push_back(remoteFile(text(asset, "name"), url, json()));
    }
    auto repo = link.project.substr(link.project.rfind('/') + 1);
    if (assets.contains("sources") && assets["sources"].is_array()) for (auto& source : assets["sources"]) {
        auto format = text(source, "format"), url = text(source, "url");
        if ((format != "zip" && format != "tar.gz") || !url.starts_with(site)) continue;
        auto file = remoteFile(safeFilename(repo + "-" + tag) + "." + format, url, json()); file.format = "Source code";
        if (std::none_of(item.files.begin(), item.files.end(), [&](const File& f) { return f.name == file.name; })) item.files.push_back(std::move(file));
    }
    selectReleaseFile(item, link.file);
    return item;
}

bool parseZenodoLink(const std::string& input, ZenodoLink& out) {
    UrlParts u; if (!splitUrl(input, u)) return false;
    auto digits = [](const std::string& s) { return !s.empty() && s.size() < 20 && s.find_first_not_of("0123456789") == std::string::npos; };
    out = {};
    if (u.host == "zenodo.org" && u.path.size() >= 2 && (u.path[0] == "records" || u.path[0] == "record") && digits(u.path[1])) {
        out.id = u.path[1];
        if (u.path.size() >= 4 && u.path[2] == "files") try { out.file = joinDecoded(u.path, 3, u.path.size()); } catch (const std::exception&) { return false; }
        return true;
    }
    // DOIs such as doi.org/10.5281/zenodo.1234567.
    if (u.host == "doi.org" && u.path.size() == 2 && u.path[0] == "10.5281" && lower(u.path[1]).starts_with("zenodo.") && digits(u.path[1].substr(7))) {
        out.id = u.path[1].substr(7); return true;
    }
    return false;
}
Item parseZenodoRecord(const std::string& body, const ZenodoLink& link) {
    auto j = parse(body, "Zenodo");
    if (!j.is_object()) throw std::runtime_error("Unexpected Zenodo record data.");
    Item item; item.id = "zenodo/" + link.id;
    item.title = j.contains("metadata") ? text(j["metadata"], "title") : std::string();
    if (item.title.empty()) item.title = "Zenodo record " + link.id;
    auto add = [&](const json& entry) {
        auto url = entry.contains("links") ? text(entry["links"], "self") : std::string();
        if (text(entry, "key").empty() || !url.starts_with("https://zenodo.org/")) return;
        // Zenodo publishes MD5 checksums only, which downloads do not verify.
        item.files.push_back(remoteFile(text(entry, "key"), url, entry.value("size", json())));
    };
    if (j.contains("files") && j["files"].is_array()) for (auto& entry : j["files"]) add(entry);
    if (item.files.empty()) throw std::runtime_error("This Zenodo record has no public files (they may be restricted).");
    if (!link.file.empty()) {
        auto name = safeFilename(link.file);
        if (std::none_of(item.files.begin(), item.files.end(), [&](const File& f) { return f.name == name; }))
            throw std::runtime_error("That file is not in the record: " + link.file);
        item.selectedFile = name;
    } else if (item.files.size() == 1) item.selectedFile = item.files[0].name;
    return item;
}

bool parsePackageLink(const std::string& input, PackageLink& out) {
    UrlParts u; if (!splitUrl(input, u)) return false;
    static const std::regex name("[A-Za-z0-9][A-Za-z0-9._-]{0,213}"), version("[A-Za-z0-9][A-Za-z0-9.+_-]{0,99}");
    auto& p = u.path; out = {};
    try {
        if (u.host == "pypi.org" && p.size() >= 2 && p[0] == "project") {
            out.registry = "pypi"; out.name = decode(p[1]); if (p.size() >= 3) out.version = decode(p[2]);
        } else if (u.host == "npmjs.com" && p.size() >= 2 && p[0] == "package") {
            out.registry = "npm"; size_t next = 2;
            auto first = decode(p[1]);
            // Scoped packages: npmjs.com/package/@scope/name.
            if (first.starts_with("@")) {
                if (first.find('/') != std::string::npos) out.name = first;
                else if (p.size() >= 3) { out.name = first + "/" + decode(p[2]); next = 3; }
                else return false;
                auto slash = out.name.find('/');
                if (!std::regex_match(out.name.substr(1, slash - 1), name) || !std::regex_match(out.name.substr(slash + 1), name)) return false;
            } else out.name = first;
            if (p.size() >= next + 2 && p[next] == "v") out.version = decode(p[next + 1]);
        } else if (u.host == "crates.io" && p.size() >= 2 && p[0] == "crates") {
            out.registry = "crates"; out.name = decode(p[1]); if (p.size() >= 3) out.version = decode(p[2]);
        } else return false;
    } catch (const std::exception&) { return false; }
    if (!out.name.starts_with("@") && !std::regex_match(out.name, name)) return false;
    return out.version.empty() || std::regex_match(out.version, version);
}
Item parsePyPIRelease(const std::string& body) {
    auto j = parse(body, "PyPI");
    if (!j.is_object() || !j.contains("info") || !j.contains("urls") || !j["urls"].is_array()) throw std::runtime_error("Unexpected PyPI release data.");
    auto name = text(j["info"], "name"), version = text(j["info"], "version");
    Item item; item.title = name + " " + version; item.id = "pypi/" + safeFilename(name) + "/" + safeFilename(version);
    for (auto& entry : j["urls"]) {
        auto url = text(entry, "url");
        if (text(entry, "filename").empty() || !url.starts_with("https://files.pythonhosted.org/")) continue;
        auto file = remoteFile(text(entry, "filename"), url, entry.value("size", json()));
        if (entry.contains("digests") && hexDigest(text(entry["digests"], "sha256"), 64)) file.sha256 = text(entry["digests"], "sha256");
        item.files.push_back(std::move(file));
    }
    if (item.files.empty()) throw std::runtime_error("This PyPI release has no files.");
    if (item.files.size() == 1) item.selectedFile = item.files[0].name;
    return item;
}
Item parseNpmVersion(const std::string& body) {
    auto j = parse(body, "npm");
    auto tarball = j.is_object() && j.contains("dist") ? text(j["dist"], "tarball") : std::string();
    if (!tarball.starts_with("https://registry.npmjs.org/")) throw std::runtime_error(!text(j, "error").empty() ? "npm: " + text(j, "error") : "Unexpected npm package data.");
    auto name = text(j, "name"), version = text(j, "version");
    Item item; item.title = name + " " + version; item.id = "npm/" + name + "/" + safeFilename(version);
    auto file = remoteFile(tarball.substr(tarball.rfind('/') + 1), tarball, json());
    // npm's shasum is the tarball's SHA-1.
    if (hexDigest(text(j["dist"], "shasum"), 40)) file.sha1 = text(j["dist"], "shasum");
    item.selectedFile = file.name; item.files.push_back(std::move(file));
    return item;
}
Item parseCrateVersion(const std::string& body, const PackageLink& link) {
    auto j = parse(body, "crates.io");
    if (!j.is_object()) throw std::runtime_error("Unexpected crates.io data.");
    json version;
    if (j.contains("version") && j["version"].is_object()) version = j["version"];
    else if (j.contains("crate") && j.contains("versions") && j["versions"].is_array()) {
        auto wanted = text(j["crate"], "max_stable_version"); if (wanted.empty()) wanted = text(j["crate"], "newest_version");
        for (auto& v : j["versions"]) if (text(v, "num") == wanted) { version = v; break; }
    }
    auto num = text(version, "num");
    if (num.empty()) throw std::runtime_error(j.contains("errors") ? "crates.io: crate or version not found." : "Unexpected crates.io data.");
    Item item; item.title = link.name + " " + num; item.id = "crates/" + link.name + "/" + safeFilename(num);
    auto file = remoteFile(link.name + "-" + num + ".crate", "https://static.crates.io/crates/" + encode(link.name) + "/" + encode(link.name + "-" + num) + ".crate", version.value("crate_size", json()));
    if (hexDigest(text(version, "checksum"), 64)) file.sha256 = text(version, "checksum");
    item.selectedFile = file.name; item.files.push_back(std::move(file));
    return item;
}

std::string dropboxDirectUrl(const std::string& input) {
    UrlParts u; if (!splitUrl(input, u)) return {};
    if (u.host != "dropbox.com" || u.path.empty() || (u.path[0] != "s" && u.path[0] != "scl" && u.path[0] != "sh")) return {};
    // dl=1 asks for the file (a folder comes as one .zip) instead of the preview page.
    std::string query; std::istringstream in(u.query); std::string pair;
    while (std::getline(in, pair, '&')) if (!pair.empty() && !pair.starts_with("dl=") && !pair.starts_with("raw=")) query += pair + "&";
    std::string path; for (auto& part : u.path) path += "/" + part;
    return "https://www.dropbox.com" + path + "?" + query + "dl=1";
}

bool isHostLink(const std::string& input) {
    ZenodoLink zenodo; PackageLink package;
    return isForgeLink(input) || parseZenodoLink(input, zenodo) || parsePackageLink(input, package) || !dropboxDirectUrl(input).empty();
}
Item loadHostItem(const std::string& input, std::atomic_bool& stop) {
    if (auto direct = dropboxDirectUrl(input); !direct.empty()) {
        try { return loadDirectLink(direct, stop); }
        catch (const std::exception& e) {
            // A missing or private share answers with Dropbox's web page instead of a file.
            if (startsWithStatus(e, {"404", "409", "410"}) || std::string(e.what()).starts_with("This link opens a web page"))
                throw std::runtime_error("This Dropbox link was removed, has expired, or is not shared publicly.");
            throw;
        }
    }
    ZenodoLink zenodo;
    if (parseZenodoLink(input, zenodo)) {
        try { return parseZenodoRecord(fetchData("https://zenodo.org/api/records/" + zenodo.id, stop).body, zenodo); }
        catch (const std::exception& e) { if (startsWithStatus(e, {"404", "410"})) throw std::runtime_error("Zenodo record not found, or it was removed."); throw; }
    }
    PackageLink package;
    if (parsePackageLink(input, package)) {
        try {
            if (package.registry == "pypi")
                return parsePyPIRelease(fetchData("https://pypi.org/pypi/" + encode(package.name) + (package.version.empty() ? "" : "/" + encode(package.version)) + "/json", stop).body);
            if (package.registry == "npm")
                return parseNpmVersion(fetchData("https://registry.npmjs.org/" + encode(package.name) + "/" + (package.version.empty() ? "latest" : encode(package.version)), stop).body);
            return parseCrateVersion(fetchData("https://crates.io/api/v1/crates/" + encode(package.name) + (package.version.empty() ? "" : "/" + encode(package.version)), stop).body, package);
        } catch (const std::exception& e) {
            if (startsWithStatus(e, {"404"})) throw std::runtime_error(package.version.empty() ? "Package not found." : "Package or version not found.");
            throw;
        }
    }
    auto link = parseForgeLink(input);
    auto api = link.gitlab
        ? "https://" + link.host + "/api/v4/projects/" + encode(link.project) + "/releases/" + (link.tag.empty() ? std::string("permalink/latest") : encode(link.tag))
        : "https://" + link.host + "/api/v1/repos/" + link.project + "/releases/" + (link.tag.empty() ? std::string("latest") : "tags/" + encode(link.tag));
    try {
        auto r = fetchData(api, stop);
        auto item = link.gitlab ? parseGitLabRelease(r.body, link) : parseGiteaRelease(r.body, link);
        if (item.files.empty()) throw std::runtime_error("This release has no files.");
        return item;
    } catch (const std::exception& e) {
        if (startsWithStatus(e, {"401", "403", "404"}))
            throw std::runtime_error(link.tag.empty() ? "This project has no published releases, or it is private." : "Release not found. Check the tag, or the project may be private.");
        throw;
    }
}

bool isFileSiteLink(const std::string& input) {
    return isHubLink(input) || isGitHubLink(input) || isHostLink(input) || isLockerLink(input) || (isHttpUrl(input) && isArchiveLink(input));
}
}
