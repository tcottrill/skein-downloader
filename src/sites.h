#pragma once
#include "core.h"

// File sites beyond archive.org, Hugging Face and GitHub. hosts.cpp has the sites with
// public file APIs; lockers.cpp has the file lockers, whose download links change per visit.
namespace archive {
// A page or API answer. A response that is a file (not a web page, JSON or text) is not
// read: body stays empty and file is true, with the size from its headers when known.
struct Response {
    std::string url, type, disposition, body;
    uint64_t size = 0;
    bool sizeKnown = false, file = false;
};
// HTTP for the site loaders, from http.cpp. headers are "Name: value\r\n" lines; wait is
// how many seconds the server may take to answer or to send more data.
Response fetch(const std::string& url, std::atomic_bool& stop, const std::string& headers = {}, unsigned wait = 10);
Response post(const std::string& url, const std::string& body, std::atomic_bool& stop, const std::string& headers = {});

// A URL split for matching: host is lowercase without "www.", path segments stay percent-encoded.
struct UrlParts {
    std::string host, query;
    std::vector<std::string> path;
};
bool splitUrl(const std::string& url, UrlParts& out);
// One query parameter, decoded; empty when absent.
std::string queryValue(const std::string& query, const std::string& key);

// Codeberg and Gitea releases, and GitLab releases (gitlab.com or any "/-/releases" page).
struct ForgeLink {
    std::string host, project, tag, file;
    bool gitlab = false;
};
bool isForgeLink(const std::string& input);
ForgeLink parseForgeLink(const std::string& input);
Item parseGiteaRelease(const std::string& json, const ForgeLink& link);
Item parseGitLabRelease(const std::string& json, const ForgeLink& link);
// Zenodo records: id is the record number, file an optional file within it.
struct ZenodoLink {
    std::string id, file;
};
bool parseZenodoLink(const std::string& input, ZenodoLink& out);
Item parseZenodoRecord(const std::string& json, const ZenodoLink& link);
// Package pages on pypi.org, npmjs.com and crates.io; an empty version means the latest.
struct PackageLink {
    std::string registry, name, version;
};
bool parsePackageLink(const std::string& input, PackageLink& out);
Item parsePyPIRelease(const std::string& json);
Item parseNpmVersion(const std::string& json);
Item parseCrateVersion(const std::string& json, const PackageLink& link);
// A Dropbox share link turned into its direct download (dl=1); empty for other links.
std::string dropboxDirectUrl(const std::string& input);
bool isHostLink(const std::string& input);
Item loadHostItem(const std::string& input, std::atomic_bool& stop);

// MediaFire, Google Drive, OneDrive and WeTransfer.
bool isLockerLink(const std::string& input);
Item loadLockerItem(const std::string& input, std::atomic_bool& stop);
// The current download address for a locker file; other links come back unchanged.
std::string resolveLockerUrl(const std::string& url, std::atomic_bool& stop);
// Page parsing, exposed for tests.
std::string mediafireDownloadLink(const std::string& html);
std::string driveFileId(const std::string& input);
std::string driveConfirmUrl(const std::string& html);
std::string driveWarningName(const std::string& html);
std::string oneDriveShareToken(const std::string& link);

// Web folder listings ("Index of /files/" pages from Apache, nginx, lighttpd and similar
// servers), live or as Wayback Machine copies; folders.cpp. A folder becomes one Item whose
// files keep their paths below it ("sub/folder/file.zip"), each with its own download link.
// A Wayback Machine link: web.archive.org/web/<time>[flags_]/<original address>.
struct WaybackLink {
    std::string time, original;
};
bool parseWaybackLink(const std::string& input, WaybackLink& out);
bool isWaybackLink(const std::string& input);
// The capture's original bytes: the archive's "id_" address, without its toolbar or rewritten links.
std::string waybackRawUrl(const std::string& time, const std::string& original);
// The same capture through the archive's "if_" form, a separate address in its cache; empty when
// url is not a raw ("id_") capture address. Files other than web pages come back unchanged in it.
std::string waybackAlternateUrl(const std::string& url);
// One listing page's entries: the links to files and folders directly inside the page's folder,
// with decoded names. Sorting links, the parent folder and links elsewhere are left out.
struct FolderEntry {
    std::string name, url;
    bool folder = false;
};
std::vector<FolderEntry> parseFolderListing(const std::string& html, const std::string& pageUrl);
// True for a server-made folder listing page ("Index of /...", "Directory listing for /...").
bool looksLikeFolderListing(const std::string& html);
// The Wayback Machine's capture index (CDX, fields: original timestamp length statuscode) for a
// folder: every file below it, each once, with its most complete capture. Crawlers often keep only
// the first 1 or 5 MiB of a large file, so the largest capture wins.
struct Capture {
    std::string name, time, original;
    uint64_t length = 0;
};
std::vector<Capture> parseWaybackCaptures(const std::string& cdx, const std::string& folderUrl);
// An http(s) address whose path ends in '/' and that has no query: a folder, likely a listing.
bool looksLikeFolderUrl(const std::string& input);
// A folder listing link (live or Wayback), or a single archived file from a Wayback link.
Item loadFolderItem(const std::string& input, std::atomic_bool& stop);

// Every link the file picker handles: archive.org, Hugging Face, GitHub, the hosts, the lockers and Wayback links.
bool isFileSiteLink(const std::string& input);
}
