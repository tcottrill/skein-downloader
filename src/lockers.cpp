// File lockers: MediaFire, Google Drive, OneDrive and WeTransfer. Their download links
// expire or change per visit, so each file keeps its page link (File::url or Item::sourceUrl)
// and resolveLockerUrl() asks the site for a fresh download address when the download starts.
#include "sites.h"
#include "json.hpp"
#include <algorithm>
#include <regex>
#include <stdexcept>
namespace archive {
using nlohmann::json;
namespace {
std::string text(const json& j, const char* key) { return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string(); }
json parse(const std::string& body, const char* site) {
    try { return json::parse(body); } catch (const json::exception&) { throw std::runtime_error(std::string("Unexpected data from ") + site + "."); }
}
bool startsWithStatus(const std::exception& e, std::initializer_list<const char*> codes) {
    std::string message = e.what();
    return std::any_of(codes.begin(), codes.end(), [&](const char* code) { return message.starts_with(std::string("HTTP ") + code); });
}
std::string base64Decode(const std::string& in) {
    std::string out; unsigned value = 0; int bits = 0;
    for (unsigned char c : in) {
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
        if (v < 0) { if (c == '=') break; throw std::runtime_error("Invalid base64 text"); }
        value = (value << 6) | (unsigned)v; bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((value >> bits) & 0xFF); }
    }
    return out;
}
// URL-safe base64 without padding.
std::string base64Url(const std::string& in) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out; unsigned value = 0; int bits = 0;
    for (unsigned char c : in) { value = (value << 8) | c; bits += 8; while (bits >= 6) { bits -= 6; out += table[(value >> bits) & 63]; } }
    if (bits) out += table[(value << (6 - bits)) & 63];
    return out;
}
std::string htmlUnescape(std::string s) {
    static const std::regex entity("&(amp|lt|gt|quot|apos|#39|#[0-9]{1,6}|#x[0-9A-Fa-f]{1,5});");
    std::string out; std::smatch m; auto start = s.cbegin();
    while (std::regex_search(start, s.cend(), m, entity)) {
        out.append(start, m[0].first); auto name = m[1].str();
        if (name == "amp") out += '&'; else if (name == "lt") out += '<'; else if (name == "gt") out += '>';
        else if (name == "quot") out += '"'; else if (name == "apos" || name == "#39") out += '\'';
        else {
            unsigned long code = name[1] == 'x' ? std::stoul(name.substr(2), nullptr, 16) : std::stoul(name.substr(1));
            // UTF-8 for a numeric reference.
            if (code < 0x80) out += (char)code;
            else if (code < 0x800) { out += (char)(0xC0 | (code >> 6)); out += (char)(0x80 | (code & 0x3F)); }
            else if (code < 0x10000) { out += (char)(0xE0 | (code >> 12)); out += (char)(0x80 | ((code >> 6) & 0x3F)); out += (char)(0x80 | (code & 0x3F)); }
            else { out += (char)(0xF0 | (code >> 18)); out += (char)(0x80 | ((code >> 12) & 0x3F)); out += (char)(0x80 | ((code >> 6) & 0x3F)); out += (char)(0x80 | (code & 0x3F)); }
        }
        start = m[0].second;
    }
    out.append(start, s.cend());
    return out;
}
File lockerFile(const std::string& name, const std::string& url) {
    File file; file.name = safeFilename(name); file.url = url;
    auto dot = file.name.rfind('.'); if (dot != std::string::npos && dot + 1 < file.name.size()) file.format = file.name.substr(dot + 1);
    return file;
}
// A one-file item: like a direct link, it saves straight into the chosen folder.
Item singleFile(File file) {
    Item item; item.title = file.name; item.selectedFile = file.name; item.sourceUrl = file.url;
    file.url.clear(); item.files.push_back(std::move(file));
    return item;
}
uint64_t digits(const json& value, bool& known) {
    auto s = value.is_string() ? value.get<std::string>() : value.is_number_unsigned() ? value.dump() : std::string();
    known = !s.empty() && s.size() < 20 && s.find_first_not_of("0123456789") == std::string::npos;
    return known ? std::stoull(s) : 0;
}

// MediaFire: file/KEY/name/file and folder/KEY/name pages. Its public API lists files with
// their SHA-256; the download link is read from the file's page.
struct MediaFireLink { std::string key; bool folder = false; };
bool parseMediaFire(const UrlParts& u, MediaFireLink& out) {
    if (u.host != "mediafire.com" && u.host != "app.mediafire.com") return false;
    auto& p = u.path; out = {};
    if (p.size() >= 2 && (p[0] == "file" || p[0] == "file_premium" || p[0] == "download" || p[0] == "view")) out.key = p[1];
    else if (p.size() >= 2 && p[0] == "folder") { out.key = p[1]; out.folder = true; }
    else if (p.empty() && !u.query.empty()) out.key = u.query.substr(0, u.query.find('&'));
    return std::regex_match(out.key, std::regex("[A-Za-z0-9]{6,20}"));
}
std::string mediafirePage(const std::string& key) { return "https://www.mediafire.com/file/" + key + "/file"; }
json mediafireApi(const std::string& call, std::atomic_bool& stop) {
    auto j = parse(fetch("https://www.mediafire.com/api/1.5/" + call + "&response_format=json", stop).body, "MediaFire");
    auto& response = j.contains("response") ? j["response"] : j;
    if (text(response, "result") != "Success") {
        auto message = text(response, "message");
        throw std::runtime_error(message.empty() ? "MediaFire did not return the file list." : "MediaFire: " + message + ". The file may be private or removed.");
    }
    return response;
}
File mediafireFile(const json& info, const std::string& folder) {
    auto file = lockerFile(text(info, "filename"), mediafirePage(text(info, "quickkey")));
    if (!folder.empty()) file.name = folder + "/" + file.name;
    file.size = digits(info.value("size", json()), file.sizeKnown);
    auto hash = text(info, "hash");
    if (hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](unsigned char c) { return isxdigit(c); })) file.sha256 = hash;
    return file;
}
Item loadMediaFire(const MediaFireLink& link, std::atomic_bool& stop) {
    if (!link.folder) {
        auto response = mediafireApi("file/get_info.php?quick_key=" + link.key, stop);
        if (!response.contains("file_info") || text(response["file_info"], "quickkey").empty()) throw std::runtime_error("MediaFire did not describe this file.");
        return singleFile(mediafireFile(response["file_info"], ""));
    }
    auto info = mediafireApi("folder/get_info.php?folder_key=" + link.key, stop);
    Item item; item.title = info.contains("folder_info") ? text(info["folder_info"], "name") : std::string();
    if (item.title.empty()) item.title = "MediaFire folder " + link.key;
    item.id = "mediafire/" + safeFilename(item.title);
    // Subfolders are walked breadth-first; their names become folders on disk.
    std::vector<std::pair<std::string, std::string>> pending{{link.key, ""}};
    for (size_t next = 0; next < pending.size(); ++next) {
        if (pending.size() > 500 || item.files.size() > 10000) throw std::runtime_error("This MediaFire folder is too large to list (over 10,000 files or 500 folders).");
        auto [key, path] = pending[next];
        for (auto type : {"files", "folders"}) for (int chunk = 1; chunk <= 1000; ++chunk) {
            auto response = mediafireApi("folder/get_content.php?folder_key=" + key + "&content_type=" + type + "&chunk=" + std::to_string(chunk), stop);
            auto& content = response.contains("folder_content") ? response["folder_content"] : response;
            if (content.contains(type) && content[type].is_array()) for (auto& entry : content[type]) {
                if (std::string(type) == "files") { if (!text(entry, "quickkey").empty() && !text(entry, "filename").empty()) item.files.push_back(mediafireFile(entry, path)); }
                else if (std::regex_match(text(entry, "folderkey"), std::regex("[A-Za-z0-9]{6,20}")))
                    pending.push_back({text(entry, "folderkey"), (path.empty() ? "" : path + "/") + safeFilename(text(entry, "name"))});
            }
            if (text(content, "more_chunks") != "yes") break;
        }
    }
    if (item.files.empty()) throw std::runtime_error("This MediaFire folder has no files.");
    return item;
}
std::string resolveMediaFire(const std::string& page, std::atomic_bool& stop) {
    Response r;
    try { r = fetch(page, stop); }
    catch (const std::exception& e) { if (startsWithStatus(e, {"403", "404", "410"})) throw std::runtime_error("This MediaFire file was removed or is private."); throw; }
    // A file that needs no page answers with the file itself.
    if (r.file) return r.url;
    auto link = mediafireDownloadLink(r.body);
    if (!link.empty()) return link;
    if (r.url.find("error.php") != std::string::npos) throw std::runtime_error("This MediaFire file was removed or is private.");
    if (r.body.find("Malware Detected") != std::string::npos) throw std::runtime_error("MediaFire blocked this file: its scan reports malware.");
    throw std::runtime_error("MediaFire did not show a download link for this file. Its page may have changed; open it in a browser to check.");
}

// Google Drive: single files shared as "Anyone with the link". Large files show a virus-scan
// warning page whose form carries the confirmation for the real download.
std::string driveDownloadUrl(const std::string& id) { return "https://drive.usercontent.google.com/download?id=" + id + "&export=download"; }
std::string driveError(const std::string& body) {
    if (body.find("Too many users") != std::string::npos || body.find("Quota exceeded") != std::string::npos || body.find("quota") != std::string::npos)
        return "Google Drive: too many people downloaded this file recently. Try again later (usually within 24 hours).";
    if (body.find("accounts.google.com") != std::string::npos || body.find("ServiceLogin") != std::string::npos)
        return "This Google Drive file is not shared publicly. Its owner must set it to \"Anyone with the link\" (sign-in is not supported in this version).";
    return "Google Drive did not offer this file for download. It may be private or removed.";
}
Response driveFetch(const std::string& id, std::atomic_bool& stop) {
    try { return fetch(driveDownloadUrl(id), stop); }
    catch (const std::exception& e) {
        if (startsWithStatus(e, {"401", "403", "404"})) throw std::runtime_error("This Google Drive file is private or was removed (sign-in is not supported in this version).");
        if (startsWithStatus(e, {"429"})) throw std::runtime_error(driveError("Too many users"));
        throw;
    }
}
Item loadDrive(const std::string& id, std::atomic_bool& stop) {
    auto r = driveFetch(id, stop);
    File file;
    if (r.file) { file = lockerFile(filenameFromDisposition(r.disposition), ""); file.size = r.size; file.sizeKnown = r.sizeKnown; }
    else {
        if (driveConfirmUrl(r.body).empty()) throw std::runtime_error(driveError(r.body));
        file = lockerFile(driveWarningName(r.body), "");
    }
    if (file.name == "download") file.name = safeFilename(id);
    file.url = "https://drive.google.com/file/d/" + id + "/view";
    return singleFile(std::move(file));
}
std::string resolveDrive(const std::string& id, std::atomic_bool& stop) {
    auto r = driveFetch(id, stop);
    if (r.file) return driveDownloadUrl(id);
    auto confirm = driveConfirmUrl(r.body);
    if (confirm.empty()) throw std::runtime_error(driveError(r.body));
    return confirm;
}

// OneDrive. Personal share links (1drv.ms, onedrive.live.com) go through the public shares
// API, whose answers carry a short-lived download URL; each file is named by its API address.
// Work and school links (sharepoint.com) download directly with download=1.
bool isOneDrivePersonal(const UrlParts& u) { return u.host == "1drv.ms" || u.host == "onedrive.live.com"; }
bool isOneDriveApi(const UrlParts& u) { return u.host == "api.onedrive.com" && u.path.size() >= 4 && u.path[0] == "v1.0" && u.path[1] == "shares"; }
bool isSharePoint(const UrlParts& u) {
    return u.host.ends_with(".sharepoint.com") && !u.path.empty() && (u.path[0].starts_with(":") || u.path[0] == "personal" || u.path[0] == "sites");
}
json oneDriveItem(const std::string& url, std::atomic_bool& stop) {
    try { return parse(fetch(url, stop, "Accept: application/json\r\n").body, "OneDrive"); }
    catch (const std::exception& e) {
        if (startsWithStatus(e, {"400", "401", "403", "404", "500"}))
            throw std::runtime_error("OneDrive did not allow this link to be read without signing in. Check that it is shared with \"Anyone with the link\", or open it in a browser and copy the Download link.");
        throw;
    }
}
File oneDriveFile(const json& entry, const std::string& api, const std::string& folder) {
    auto file = lockerFile(text(entry, "name"), api);
    if (!folder.empty()) file.name = folder + "/" + file.name;
    file.size = digits(entry.value("size", json()), file.sizeKnown);
    if (entry.contains("file") && entry["file"].contains("hashes")) {
        auto& hashes = entry["file"]["hashes"];
        auto sha1 = text(hashes, "sha1Hash"), sha256 = text(hashes, "sha256Hash");
        if (sha256.size() == 64) file.sha256 = sha256; else if (sha1.size() == 40) file.sha1 = sha1;
    }
    return file;
}
Item loadOneDrive(const std::string& link, std::atomic_bool& stop) {
    auto base = "https://api.onedrive.com/v1.0/shares/" + oneDriveShareToken(link);
    auto root = oneDriveItem(base + "/driveItem?$expand=children", stop);
    if (text(root, "name").empty()) throw std::runtime_error("OneDrive did not describe this link.");
    if (!root.contains("folder")) return singleFile(oneDriveFile(root, base + "/driveItem", ""));
    Item item; item.title = text(root, "name"); item.id = "onedrive/" + safeFilename(item.title);
    std::vector<std::pair<json, std::string>> pending{{root, ""}};
    for (size_t next = 0; next < pending.size(); ++next) {
        if (pending.size() > 500 || item.files.size() > 10000) throw std::runtime_error("This OneDrive folder is too large to list (over 10,000 files or 500 folders).");
        auto [folder, path] = pending[next];
        if (!folder.contains("children")) folder = oneDriveItem(base + "/items/" + encode(text(folder, "id")) + "?$expand=children", stop);
        if (folder.contains("children") && folder["children"].is_array()) for (auto& child : folder["children"]) {
            if (text(child, "id").empty() || text(child, "name").empty()) continue;
            if (child.contains("folder")) pending.push_back({child, (path.empty() ? "" : path + "/") + safeFilename(text(child, "name"))});
            else if (child.contains("file")) item.files.push_back(oneDriveFile(child, base + "/items/" + encode(text(child, "id")), path));
        }
    }
    if (item.files.empty()) throw std::runtime_error("This OneDrive folder has no files.");
    return item;
}
std::string resolveOneDrive(const std::string& api, std::atomic_bool& stop) {
    auto entry = oneDriveItem(api, stop);
    auto url = text(entry, "@content.downloadUrl");
    if (!url.starts_with("https://")) throw std::runtime_error("OneDrive did not offer a download for this file.");
    return url;
}

// WeTransfer: we.tl short links and wetransfer.com/downloads/ID[/RECIPIENT]/HASH pages.
// Its web API needs the page's session cookie and CSRF token; files are named by
// the transfer page plus ?file_id=.
struct WeTransferLink { std::string id, recipient, hash, file; };
bool parseWeTransfer(const UrlParts& u, WeTransferLink& out) {
    if (u.host != "wetransfer.com" && !u.host.ends_with(".wetransfer.com")) return false;
    auto& p = u.path; out = {};
    if (p.size() == 3 && p[0] == "downloads") { out.id = p[1]; out.hash = p[2]; }
    else if (p.size() == 4 && p[0] == "downloads") { out.id = p[1]; out.recipient = p[2]; out.hash = p[3]; }
    else return false;
    out.file = queryValue(u.query, "file_id");
    static const std::regex token("[A-Za-z0-9]{6,80}");
    return std::regex_match(out.id, token) && std::regex_match(out.hash, token) && (out.recipient.empty() || std::regex_match(out.recipient, token))
        && (out.file.empty() || std::regex_match(out.file, token));
}
std::string weTransferPage(const WeTransferLink& link) {
    return "https://wetransfer.com/downloads/" + link.id + "/" + (link.recipient.empty() ? "" : link.recipient + "/") + link.hash;
}
json weTransferCall(const WeTransferLink& link, const std::string& action, json body, std::atomic_bool& stop) {
    // Visiting the page sets the session cookie; its CSRF token goes with the API call.
    auto page = fetch(weTransferPage(link), stop);
    std::smatch m; std::string headers = "Content-Type: application/json\r\nAccept: application/json\r\nX-Requested-With: XMLHttpRequest\r\n";
    if (std::regex_search(page.body, m, std::regex(R"re(name="csrf-token" content="([^"]+)")re"))) headers += "X-CSRF-Token: " + m[1].str() + "\r\n";
    body["security_hash"] = link.hash;
    if (!link.recipient.empty()) body["recipient_id"] = link.recipient;
    try { return parse(post("https://wetransfer.com/api/v4/transfers/" + link.id + "/" + action, body.dump(), stop, headers).body, "WeTransfer"); }
    catch (const std::exception& e) {
        if (startsWithStatus(e, {"401", "403", "404", "410", "422"})) throw std::runtime_error("This WeTransfer transfer has expired, was deleted, or needs a password (not supported in this version).");
        throw;
    }
}
Item loadWeTransfer(WeTransferLink link, std::atomic_bool& stop) {
    auto j = weTransferCall(link, "prepare-download", json::object(), stop);
    if (!text(j, "state").empty() && text(j, "state") != "downloadable") throw std::runtime_error("This WeTransfer transfer has expired or was deleted.");
    Item item; item.title = text(j, "display_name"); if (item.title.empty()) item.title = "WeTransfer " + link.id;
    item.id = "wetransfer/" + link.id; item.checkAll = true;
    const json& list = j.contains("files") && j["files"].is_array() ? j["files"] : j.contains("items") && j["items"].is_array() ? j["items"] : json::array();
    for (auto& entry : list) {
        if (text(entry, "id").empty() || text(entry, "name").empty()) continue;
        link.file = text(entry, "id");
        auto file = lockerFile(text(entry, "name"), weTransferPage(link) + "?file_id=" + encode(link.file));
        file.size = digits(entry.value("size", json()), file.sizeKnown);
        item.files.push_back(std::move(file));
    }
    if (item.files.empty()) throw std::runtime_error("This WeTransfer transfer has no files.");
    return item;
}
std::string resolveWeTransfer(const WeTransferLink& link, std::atomic_bool& stop) {
    auto j = weTransferCall(link, "download", {{"intent", "single_file"}, {"file_ids", json::array({link.file})}}, stop);
    auto url = text(j, "direct_link");
    if (!url.starts_with("https://")) throw std::runtime_error("WeTransfer did not offer a download for this file.");
    return url;
}
}

std::string mediafireDownloadLink(const std::string& html) {
    std::smatch m;
    static const std::regex button(R"re(href="(https://download[0-9]*\.mediafire\.com/[^"\s]+)")re", std::regex::icase);
    if (std::regex_search(html, m, button)) return htmlUnescape(m[1]);
    // Newer pages hide the link in base64.
    static const std::regex scrambled(R"re(data-scrambled-url="([A-Za-z0-9+/=]+)")re");
    if (std::regex_search(html, m, scrambled)) try {
        auto url = base64Decode(m[1]);
        if (std::regex_match(url, std::regex(R"(https://download[0-9]*\.mediafire\.com/\S+)", std::regex::icase))) return url;
    } catch (const std::exception&) {}
    return {};
}
std::string driveFileId(const std::string& input) {
    UrlParts u; if (!splitUrl(input, u)) return {};
    if (u.host != "drive.google.com" && u.host != "docs.google.com" && u.host != "drive.usercontent.google.com") return {};
    std::string id;
    auto& p = u.path;
    if (p.size() >= 3 && p[0] == "file" && p[1] == "d") id = p[2];
    else if (p.size() >= 1 && (p[0] == "open" || p[0] == "uc" || p[0] == "download")) id = queryValue(u.query, "id");
    return std::regex_match(id, std::regex("[A-Za-z0-9_-]{10,100}")) ? id : std::string();
}
std::string driveConfirmUrl(const std::string& html) {
    std::smatch m;
    static const std::regex form(R"re(<form[^>]*id="download-form"[^>]*>([\s\S]*?)</form>)re");
    if (!std::regex_search(html, m, form)) return {};
    auto tag = m[0].str().substr(0, m[0].str().find('>')), inner = m[1].str();
    std::smatch a;
    if (!std::regex_search(tag, a, std::regex(R"re(action="([^"]+)")re"))) return {};
    auto action = htmlUnescape(a[1]);
    if (!action.starts_with("https://drive.usercontent.google.com/")) return {};
    std::string query;
    static const std::regex input(R"re(<input[^>]*type="hidden"[^>]*>)re"), name(R"re(name="([^"]*)")re"), value(R"re(value="([^"]*)")re");
    for (auto it = std::sregex_iterator(inner.begin(), inner.end(), input); it != std::sregex_iterator(); ++it) {
        auto field = it->str(); std::smatch n, v;
        if (!std::regex_search(field, n, name)) continue;
        std::regex_search(field, v, value);
        query += (query.empty() ? "" : "&") + encode(htmlUnescape(n[1])) + "=" + encode(v.empty() ? std::string() : htmlUnescape(v[1]));
    }
    return query.empty() ? std::string() : action + (action.find('?') == std::string::npos ? "?" : "&") + query;
}
std::string driveWarningName(const std::string& html) {
    std::smatch m;
    if (std::regex_search(html, m, std::regex(R"re(class="uc-name-size"><a [^>]*>([^<]+)</a>)re"))) return htmlUnescape(m[1]);
    return {};
}
std::string oneDriveShareToken(const std::string& link) {
    auto a = link.find_first_not_of(" \t\r\n"), b = link.find_last_not_of(" \t\r\n");
    return "u!" + base64Url(a == std::string::npos ? std::string() : link.substr(a, b - a + 1));
}

bool isLockerLink(const std::string& input) {
    UrlParts u; if (!splitUrl(input, u)) return false;
    MediaFireLink mediafire; WeTransferLink transfer;
    return parseMediaFire(u, mediafire) || !driveFileId(input).empty() || isOneDrivePersonal(u) || isOneDriveApi(u) || isSharePoint(u)
        || (u.host == "we.tl" && u.path.size() == 1) || parseWeTransfer(u, transfer)
        || (u.host == "drive.google.com" && u.path.size() >= 3 && u.path[0] == "drive" && u.path[1] == "folders")
        || (u.host == "docs.google.com" && u.path.size() >= 3 && u.path[1] == "d");
}
Item loadLockerItem(const std::string& input, std::atomic_bool& stop) {
    UrlParts u; if (!splitUrl(input, u)) throw std::runtime_error("Paste an HTTP or HTTPS link.");
    MediaFireLink mediafire;
    if (parseMediaFire(u, mediafire)) return loadMediaFire(mediafire, stop);
    if (auto id = driveFileId(input); !id.empty()) return loadDrive(id, stop);
    if (u.host == "drive.google.com") throw std::runtime_error("Google Drive folders are not supported in this version. Open the folder and copy the link of each file (Share > Copy link).");
    if (u.host == "docs.google.com") throw std::runtime_error("Google Docs, Sheets and Slides are not files to download. Use File > Download in the document, or share it as a file.");
    if (isSharePoint(u)) {
        auto trimmed = input.substr(input.find_first_not_of(" \t\r\n")); trimmed = trimmed.substr(0, trimmed.find_last_not_of(" \t\r\n") + 1);
        auto hash = trimmed.find('#'); if (hash != std::string::npos) trimmed.resize(hash);
        return loadDirectLink(trimmed + (trimmed.find('?') == std::string::npos ? "?" : "&") + "download=1", stop);
    }
    if (isOneDrivePersonal(u)) return loadOneDrive(input.substr(input.find_first_not_of(" \t\r\n")), stop);
    WeTransferLink transfer;
    if (u.host == "we.tl") {
        // Short links redirect to the transfer page.
        Response page;
        try { page = fetch("https://we.tl/" + u.path[0], stop); }
        catch (const std::exception& e) { if (startsWithStatus(e, {"404", "410"})) throw std::runtime_error("This WeTransfer link has expired or was deleted."); throw; }
        UrlParts target;
        if (!splitUrl(page.url, target) || !parseWeTransfer(target, transfer)) throw std::runtime_error("This WeTransfer link has expired or was deleted.");
        return loadWeTransfer(transfer, stop);
    }
    if (parseWeTransfer(u, transfer)) return loadWeTransfer(transfer, stop);
    throw std::runtime_error("Paste the link to the file itself.");
}
std::string resolveLockerUrl(const std::string& url, std::atomic_bool& stop) {
    UrlParts u; if (!splitUrl(url, u)) return url;
    MediaFireLink mediafire; WeTransferLink transfer;
    if (parseMediaFire(u, mediafire) && !mediafire.folder) return resolveMediaFire(mediafirePage(mediafire.key), stop);
    if (auto id = driveFileId(url); !id.empty()) return resolveDrive(id, stop);
    if (isOneDriveApi(u)) return resolveOneDrive(url, stop);
    if (parseWeTransfer(u, transfer) && !transfer.file.empty()) return resolveWeTransfer(transfer, stop);
    return url;
}
}
