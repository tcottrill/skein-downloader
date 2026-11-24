// Link parsing, file naming, path safety and site answers (src/core.cpp, hosts.cpp, lockers.cpp),
// without the network.
#include "sites.h"
#include <iostream>
#include <stdexcept>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
template <typename F>
void throws(F&& f, const char* message) {
    try {
        f();
    } catch (const std::exception&) {
        return;
    }
    std::cerr << "FAILED (no exception): " << message << '\n';
    ++failures;
}
const std::string sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const std::string sha1 = "0123456789abcdef0123456789abcdef01234567";
}

static void textHelpers() {
    using namespace archive;
    check(trim(" \tx y\r\n") == "x y", "trim removes spaces, tabs and line breaks at both ends");
    check(trim(" \r\n").empty(), "trim of blank text is empty");
    check(lower("AbC.ZIP") == "abc.zip", "lower");
    check(isHex("0aF9", 4) && !isHex("0aG9", 4) && !isHex("0a", 4), "isHex checks digits and length");
    check(decode("a%20b%2Fc") == "a b/c", "decode");
    throws([] {
        archive::decode("%zz");
    },
           "decode rejects a bad escape");
    throws([] {
        archive::decode("abc%2");
    },
           "decode rejects a cut-off escape");
    check(encode("a b/c") == "a%20b%2Fc" && encode("a b/c", true) == "a%20b/c", "encode, with and without slashes");
    check(isHttpUrl(" https://example.com/x ") && !isHttpUrl("https://user@example.com/") && !isHttpUrl("example.com"), "isHttpUrl");
    check(logUrl("https://x.org/a?token=1") == "https://x.org/a?..." && logUrl("https://x.org/a") == "https://x.org/a", "logUrl hides the query");
    check(formatBytes(0) == "0 B" && formatBytes(1536) == "1.5 KiB", "formatBytes");
    check(hasHttpStatus(NetworkError("x", false, 1, 404), {404, 410}), "hasHttpStatus matches the status");
    check(!hasHttpStatus(NetworkError("x", false, 1, 404), {403}) && !hasHttpStatus(std::runtime_error("HTTP 404"), {404}),
          "hasHttpStatus ignores other statuses and plain errors");
}

static void fileNames() {
    using namespace archive;
    check(safeFilename("a/b\\c:d") == "a_b_c_d", "safeFilename replaces path and reserved characters");
    check(safeFilename("con.txt") == "_con.txt" && safeFilename("Nul") == "_Nul", "safeFilename avoids Windows device names");
    check(safeFilename("  name. ") == "name", "safeFilename trims spaces and trailing dots");
    check(safeFilename("") == "download" && safeFilename("...") == "download", "safeFilename never returns an empty name");
    check(safeFilename("caf\xE9.txt") == "caf_.txt", "safeFilename replaces bytes that are not UTF-8");
    check(safeFilename(std::string(300, 'a')).size() == 200, "safeFilename shortens long names");
    check(safeFilename(std::string(199, 'a') + "\xC3\xA9" + std::string(50, 'b')) == std::string(199, 'a'),
          "safeFilename cuts only between UTF-8 characters");

    check(filenameFromDisposition(R"(attachment; filename="report.pdf")") == "report.pdf", "quoted filename");
    check(filenameFromDisposition(R"(attachment; filename*=UTF-8''na%C3%AFve.txt; filename="naive.txt")") == "na\xC3\xAFve.txt",
          "filename* takes priority and is percent-decoded");
    check(filenameFromDisposition(R"(attachment; filename="C:\temp\evil.exe")") == "evil.exe", "folder parts are dropped");
    check(filenameFromDisposition(R"(attachment; filename="say \"hi\".txt")") == "say \"hi\".txt", "escaped quotes");
    check(filenameFromDisposition("attachment; filename=plain.zip") == "plain.zip", "unquoted filename");
    check(filenameFromDisposition("inline").empty(), "no filename");

    check(filenameFromUrl("https://x.org/a/b%20c.zip?x=1#f") == "b c.zip", "filenameFromUrl decodes the last segment");
    check(filenameFromUrl("https://x.org/dir/").empty(), "filenameFromUrl of a folder is empty");
    check(looksLikeFileUrl("https://x.org/f.ISO") && !looksLikeFileUrl("https://x.org/page.html") && !looksLikeFileUrl("ftp://x.org/f.zip"),
          "looksLikeFileUrl");
}

static void paths() {
    using namespace archive;
    const auto root = std::filesystem::absolute(std::filesystem::temp_directory_path() / "skein-core-test").lexically_normal();
    check(safePath(root, "a/b.txt") == root / "a" / "b.txt", "safePath joins a relative path");
    for (const char* name : {"", "/x", "x/", "a\\b", "../x", "a/../b", "a/./b", "a//b", "con.txt", "a/LPT1", "name.", "name ", "a:b", "a|b", "a\x01"})
        throws([&] {
            safePath(root, name);
        },
               (std::string("safePath rejects \"") + name + "\"").c_str());
    // With the root already checked, every name is still validated and the result is the same.
    check(safePath(root, "a/b.txt", RootState::Checked) == root / "a" / "b.txt", "safePath with a checked root joins a relative path");
    check(safePath(root / "", "x.zip", RootState::Checked) == root / "x.zip", "safePath with a checked root and a trailing separator");
    for (const char* name : {"", "/x", "x/", "a\\b", "../x", "a/../b", "a/./b", "a//b", "con.txt", "a/LPT1", "name.", "name ", "a:b", "a|b", "a\x01"})
        throws([&] {
            safePath(root, name, RootState::Checked);
        },
               (std::string("safePath with a checked root rejects \"") + name + "\"").c_str());
    // A missing root: the names are validated all the same, the link walk is skipped.
    check(safePath(root, "a/b.txt", RootState::Missing) == root / "a" / "b.txt", "safePath with a missing root joins a relative path");
    for (const char* name : {"", "/x", "../x", "a/../b", "con.txt", "name.", "a:b"})
        throws([&] {
            safePath(root, name, RootState::Missing);
        },
               (std::string("safePath with a missing root rejects \"") + name + "\"").c_str());
    // Links: one inside the root is always found; one at or above the root only when the root is not yet checked.
    std::error_code ec;
    const auto work = std::filesystem::absolute(std::filesystem::temp_directory_path() / "skein-core-test-links").lexically_normal();
    std::filesystem::remove_all(work, ec);
    std::filesystem::create_directories(work / "real" / "sub", ec);
    std::filesystem::create_directory_symlink(work / "real", work / "link", ec);
    if (ec)
        std::cerr << "note: cannot create symbolic links here, link checks skipped\n";
    else {
        std::filesystem::create_directory_symlink(work / "real" / "sub", work / "real" / "inner", ec);
        throws([&] {
            safePath(work / "link", "f.zip");
        },
               "safePath rejects a root that is a link");
        check(safePath(work / "link", "f.zip", RootState::Checked) == work / "link" / "f.zip", "safePath with a checked root does not look at the root again");
        throws([&] {
            safePath(work / "real", "inner/f.zip", RootState::Checked);
        },
               "safePath with a checked root still rejects a link below the root");
        throws([&] {
            safePath(work / "real", "inner", RootState::Checked);
        },
               "safePath with a checked root still rejects a link as the file itself");
        check(safePath(work / "real", "sub/f.zip", RootState::Checked) == work / "real" / "sub" / "f.zip", "safePath with a checked root accepts a plain folder below the root");
    }
    std::filesystem::remove_all(work, ec);
}

static void ranges() {
    auto parts = archive::split(10, 3);
    check(parts.size() == 3 && parts[0].first == 0 && parts[0].last == 3 && parts[1].first == 4 && parts[1].last == 6 && parts[2].first == 7 && parts[2].last == 9, "split spreads the remainder over the first parts");
    check(archive::split(0, 4).empty(), "split of an empty file");
    check(archive::split(2, 8).size() == 2, "split never makes more parts than bytes");
    auto one = archive::split(100, 0);
    check(one.size() == 1 && one[0].first == 0 && one[0].last == 99, "split with no connections uses one");
}

static void archiveOrg() {
    using namespace archive;
    auto link = parseLink("https://archive.org/download/item/dir/file%20name.zip");
    check(link.id == "item" && link.file == "dir/file name.zip", "parseLink: download link with a file");
    link = parseLink("https://archive.org/details/foo_bar");
    check(link.id == "foo_bar" && link.file.empty(), "parseLink: details page");
    check(parseLink(" item-1 ").id == "item-1", "parseLink: bare identifier");
    throws([] {
        parseLink("https://example.com/details/x");
    },
           "parseLink rejects other sites");
    throws([] {
        parseLink("../x");
    },
           "parseLink rejects a path as identifier");
    throws([] {
        parseLink(" ");
    },
           "parseLink rejects an empty link");

    const std::string metadata = R"({"dir":"/12/items/item","d1":"ia800.us.archive.org",
        "workable_servers":["ia801.us.archive.org","evil.example.com"],"metadata":{"title":"T"},
        "files":[{"name":"a.zip","format":"ZIP","sha1":")" +
                                 sha1 + R"(","size":"123"},{"name":"p.zip","private":"true"},
                 {"name":"b.txt","size":"x"}]})";
    auto item = parseMetadata("item", metadata);
    check(item.title == "T" && item.files.size() == 2, "parseMetadata keeps public files");
    check(item.files[0].sizeKnown && item.files[0].size == 123 && item.files[0].sha1 == sha1 && !item.files[1].sizeKnown,
          "parseMetadata reads sizes and hashes");
    check(item.mirrorBases == std::vector<std::string>{"https://ia801.us.archive.org/12/items/item/", "https://ia800.us.archive.org/12/items/item/"},
          "parseMetadata takes two archive.org mirrors and skips other hosts");
    auto other = parseMetadata("item", R"({"dir":"/12/items/other","d1":"ia800.us.archive.org","files":[]})");
    check(other.mirrorBases.empty(), "parseMetadata ignores a storage folder of another item");
    throws([] {
        parseMetadata("item", R"({"error":"x"})");
    },
           "parseMetadata reports an error answer");
}

static void huggingFace() {
    using namespace archive;
    auto link = parseHubLink("https://huggingface.co/org/model");
    check(link.kind == "models" && link.repo == "org/model" && link.revision == "main" && !link.file, "parseHubLink: model");
    link = parseHubLink("https://huggingface.co/datasets/org/data/tree/v1.0/sub/dir");
    check(link.kind == "datasets" && link.revision == "v1.0" && link.path == "sub/dir" && !link.file, "parseHubLink: dataset folder");
    link = parseHubLink("https://hf.co/org/m/resolve/main/weights.bin");
    check(link.file && link.path == "weights.bin", "parseHubLink: file");
    throws([] {
        parseHubLink("https://huggingface.co/docs/x");
    },
           "parseHubLink rejects site pages");
    throws([] {
        parseHubLink("https://huggingface.co/org/m/blob/main");
    },
           "parseHubLink rejects a file link without a file");
    throws([] {
        parseHubLink("https://huggingface.co/org/m/tree/a..b");
    },
           "parseHubLink rejects '..' in a revision");

    auto files = parseHubTree(R"([{"type":"file","path":"a/b.bin","size":5,"lfs":{"oid":"sha256:)" + sha256 + R"(","size":100}},
        {"type":"directory","path":"a"}])");
    check(files.size() == 1 && files[0].name == "a/b.bin" && files[0].size == 100 && files[0].sha256 == sha256 && files[0].format == "bin",
          "parseHubTree: LFS size and SHA-256");
    throws([] {
        parseHubTree(R"({"error":"x"})");
    },
           "parseHubTree reports an error answer");
    check(hubRepoGated(R"({"gated":"auto"})") && !hubRepoGated(R"({"gated":false})") && !hubRepoGated("{}"), "hubRepoGated");
    check(hubNextPage(R"(<https://huggingface.co/api/models/x/tree/main?cursor=abc>; rel="next")") == "https://huggingface.co/api/models/x/tree/main?cursor=abc",
          "hubNextPage follows the Hub API");
    check(hubNextPage(R"(<https://evil.example/>; rel="next")").empty(), "hubNextPage never leaves the Hub API");
}

static void gitHub() {
    using namespace archive;
    auto link = parseGitHubLink("https://github.com/owner/repo");
    check(link.owner == "owner" && link.repo == "repo" && link.tag.empty(), "parseGitHubLink: latest release");
    check(parseGitHubLink("https://github.com/o/repo.git/releases/latest").repo == "repo", "parseGitHubLink drops .git");
    check(parseGitHubLink("https://github.com/o/r/releases/tag/v1.0").tag == "v1.0", "parseGitHubLink: tag");
    link = parseGitHubLink("https://github.com/o/r/releases/download/v1.0/app.zip");
    check(link.tag == "v1.0" && link.file == "app.zip", "parseGitHubLink: asset");
    throws([] {
        parseGitHubLink("https://github.com/o/r/issues");
    },
           "parseGitHubLink rejects other repository pages");
    throws([] {
        parseGitHubLink("https://github.com/settings/x");
    },
           "parseGitHubLink rejects site pages");

    const std::string release = R"({"tag_name":"v1.0","name":"Release 1","assets":[
        {"name":"app.zip","browser_download_url":"https://github.com/o/r/releases/download/v1.0/app.zip","size":10,"digest":"sha256:)" +
                                sha256 + R"("},
        {"name":"evil.zip","browser_download_url":"https://evil.example/x"}]})";
    GitHubLink o{"o", "r", "v1.0", ""};
    auto item = parseGitHubRelease(release, o);
    check(item.id == "o/r/v1.0" && item.title == "Release 1" && item.checkAll, "parseGitHubRelease: item");
    check(item.files.size() == 3 && item.files[0].name == "app.zip" && item.files[0].sha256 == sha256 && item.files[0].size == 10 && item.files[1].name == "r-v1.0.zip" && item.files[2].name == "r-v1.0.tar.gz", "parseGitHubRelease: assets from GitHub plus source archives");
    o.file = "missing.zip";
    throws([&] {
        parseGitHubRelease(release, o);
    },
           "parseGitHubRelease: a named file must be in the release");
}

static void forgesAndPackages() {
    using namespace archive;
    auto forge = parseForgeLink("https://codeberg.org/owner/repo/releases/tag/v2");
    check(forge.host == "codeberg.org" && forge.project == "owner/repo" && forge.tag == "v2" && !forge.gitlab, "parseForgeLink: Codeberg");
    forge = parseForgeLink("https://gitlab.com/group/sub/project/-/releases/v1.0/downloads/bin/app.zip");
    check(forge.gitlab && forge.project == "group/sub/project" && forge.tag == "v1.0" && forge.file == "bin/app.zip", "parseForgeLink: GitLab asset");
    check(!isForgeLink("https://codeberg.org/owner/repo/issues") && !isForgeLink("https://example.com/a/b"), "isForgeLink rejects other pages");

    auto gitea = parseGiteaRelease(R"({"tag_name":"v2","name":"","assets":[
        {"name":"a.tar.xz","browser_download_url":"https://codeberg.org/owner/repo/releases/download/v2/a.tar.xz","size":7},
        {"name":"x","browser_download_url":"https://evil.example/x"}],
        "zipball_url":"https://codeberg.org/owner/repo/archive/v2.zip","tarball_url":"https://codeberg.org/owner/repo/archive/v2.tar.gz"})",
                                   ForgeLink{"codeberg.org", "owner/repo", "v2", "", false});
    check(gitea.title == "v2" && gitea.files.size() == 3 && gitea.files[0].size == 7 && gitea.files[1].name == "repo-v2.zip" && gitea.checkAll,
          "parseGiteaRelease keeps files from the same site");
    auto gitlab = parseGitLabRelease(R"({"tag_name":"v1.0","assets":{
        "links":[{"name":"app.zip","direct_asset_url":"https://gitlab.com/g/p/-/releases/v1.0/downloads/app.zip"},{"name":"ext","url":"https://other.example/x"}],
        "sources":[{"format":"zip","url":"https://gitlab.com/g/p/-/archive/v1.0/p-v1.0.zip"},{"format":"tar.bz2","url":"https://gitlab.com/g/p/-/archive/v1.0/p-v1.0.tar.bz2"}]}})",
                                     ForgeLink{"gitlab.com", "g/p", "v1.0", "", true});
    check(gitlab.files.size() == 2 && gitlab.files[0].name == "app.zip" && gitlab.files[1].name == "p-v1.0.zip", "parseGitLabRelease");

    ZenodoLink zenodo;
    check(parseZenodoLink("https://zenodo.org/records/123/files/data%20set.csv", zenodo) && zenodo.id == "123" && zenodo.file == "data set.csv",
          "parseZenodoLink: record file");
    check(parseZenodoLink("https://doi.org/10.5281/zenodo.456", zenodo) && zenodo.id == "456", "parseZenodoLink: DOI");
    check(!parseZenodoLink("https://zenodo.org/communities/x", zenodo), "parseZenodoLink rejects other pages");
    auto record = parseZenodoRecord(R"({"metadata":{"title":"Rec"},"files":[{"key":"a.csv","size":3,
        "links":{"self":"https://zenodo.org/api/records/123/files/a.csv/content"}}]})",
                                    ZenodoLink{"123", ""});
    check(record.title == "Rec" && record.files.size() == 1 && record.selectedFile == "a.csv", "parseZenodoRecord");
    throws([] {
        parseZenodoRecord(R"({"files":[]})", ZenodoLink{"1", ""});
    },
           "parseZenodoRecord: no public files");

    PackageLink package;
    check(parsePackageLink("https://pypi.org/project/requests/2.31.0/", package) && package.registry == "pypi" && package.name == "requests" && package.version == "2.31.0", "parsePackageLink: PyPI");
    check(parsePackageLink("https://www.npmjs.com/package/@scope/pkg/v/1.2.3", package) && package.registry == "npm" && package.name == "@scope/pkg" && package.version == "1.2.3", "parsePackageLink: scoped npm package");
    check(parsePackageLink("https://crates.io/crates/serde", package) && package.registry == "crates" && package.version.empty(), "parsePackageLink: crate");
    check(!parsePackageLink("https://pypi.org/search", package), "parsePackageLink rejects other pages");

    auto pypi = parsePyPIRelease(R"({"info":{"name":"requests","version":"2.31.0"},"urls":[
        {"filename":"requests-2.31.0.tar.gz","url":"https://files.pythonhosted.org/p/r.tar.gz","size":100,"digests":{"sha256":")" +
                                 sha256 + R"("}},
        {"filename":"bad","url":"https://evil.example/x"}]})");
    check(pypi.files.size() == 1 && pypi.files[0].sha256 == sha256 && pypi.selectedFile == "requests-2.31.0.tar.gz", "parsePyPIRelease");
    auto npm = parseNpmVersion(R"({"name":"pkg","version":"1.0.0","dist":{"tarball":"https://registry.npmjs.org/pkg/-/pkg-1.0.0.tgz","shasum":")" + sha1 + R"("}})");
    check(npm.files.size() == 1 && npm.files[0].name == "pkg-1.0.0.tgz" && npm.files[0].sha1 == sha1, "parseNpmVersion");
    throws([] {
        parseNpmVersion(R"({"dist":{"tarball":"https://evil.example/pkg.tgz"}})");
    },
           "parseNpmVersion: tarball from the registry only");
    PackageLink crate{"crates", "serde", ""};
    auto exact = parseCrateVersion(R"({"version":{"num":"1.0.1","crate_size":50,"checksum":")" + sha256 + R"("}})", crate);
    check(exact.files.size() == 1 && exact.files[0].name == "serde-1.0.1.crate" && exact.files[0].url == "https://static.crates.io/crates/serde/serde-1.0.1.crate" && exact.files[0].size == 50 && exact.files[0].sha256 == sha256, "parseCrateVersion: one version");
    auto latest = parseCrateVersion(R"({"crate":{"max_stable_version":"1.0.2"},"versions":[{"num":"1.0.3"},{"num":"1.0.2"}]})", crate);
    check(latest.selectedFile == "serde-1.0.2.crate", "parseCrateVersion: latest stable version");

    check(dropboxDirectUrl("https://www.dropbox.com/s/abc/file.zip?dl=0&rlkey=x") == "https://www.dropbox.com/s/abc/file.zip?rlkey=x&dl=1",
          "dropboxDirectUrl asks for the file");
    check(dropboxDirectUrl("https://example.com/s/x").empty(), "dropboxDirectUrl: other sites");
    check(queryValue("a=1&b=x+y%21", "b") == "x y!" && queryValue("a=1", "b").empty(), "queryValue");
}

static void lockers() {
    using namespace archive;
    check(mediafireDownloadLink(R"(<a href="https://download1234.mediafire.com/abc/file.zip" id="downloadButton">)") == "https://download1234.mediafire.com/abc/file.zip", "mediafireDownloadLink: button");
    check(mediafireDownloadLink(R"(<a data-scrambled-url="aHR0cHM6Ly9kb3dubG9hZDkubWVkaWFmaXJlLmNvbS94L3kuemlw">)") == "https://download9.mediafire.com/x/y.zip", "mediafireDownloadLink: base64 link");
    check(mediafireDownloadLink(R"(<a href="https://evil.example/file.zip">)").empty(), "mediafireDownloadLink: other hosts");

    check(driveFileId("https://drive.google.com/file/d/1AbCdEfGhIjKlMnOp/view?usp=sharing") == "1AbCdEfGhIjKlMnOp", "driveFileId: file page");
    check(driveFileId("https://drive.google.com/uc?id=1AbCdEfGhIjK&export=download") == "1AbCdEfGhIjK", "driveFileId: download link");
    check(driveFileId("https://example.com/file/d/1AbCdEfGhIjKlMnOp").empty(), "driveFileId: other sites");
    const std::string form = R"(<form id="download-form" action="https://drive.usercontent.google.com/download" method="get">
        <input type="hidden" name="id" value="ABC"><input type="hidden" name="confirm" value="t"><input type="hidden" name="uuid" value="u&amp;1"></form>)";
    check(driveConfirmUrl(form) == "https://drive.usercontent.google.com/download?id=ABC&confirm=t&uuid=u%261", "driveConfirmUrl");
    auto elsewhere = form;
    elsewhere.replace(elsewhere.find("https://drive.usercontent.google.com"), 36, "https://evil.example");
    check(driveConfirmUrl(elsewhere).empty(), "driveConfirmUrl: only Google's download address");
    check(driveWarningName(R"(<span class="uc-name-size"><a href="/x">big &amp; file.iso</a> (5G)</span>)") == "big & file.iso", "driveWarningName");
    check(oneDriveShareToken(" https://1drv.ms/u/s!abc ") == "u!aHR0cHM6Ly8xZHJ2Lm1zL3UvcyFhYmM", "oneDriveShareToken");
}

int main() {
    try {
        textHelpers();
        fileNames();
        paths();
        ranges();
        archiveOrg();
        huggingFace();
        gitHub();
        forgesAndPackages();
        lockers();
    } catch (const std::exception& e) {
        std::cerr << "Unexpected exception: " << e.what() << '\n';
        return 1;
    }
    if (failures) std::cerr << failures << " check(s) failed\n";
    return failures ? 1 : 0;
}
