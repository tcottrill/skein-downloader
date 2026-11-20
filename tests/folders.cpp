// Folder listings and Wayback Machine links (src/folders.cpp), without the network.
#include "sites.h"
#include "platform/ftp.h"
#include <iostream>
#include <stdexcept>

static void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static const archive::FolderEntry* find(const std::vector<archive::FolderEntry>& entries, const std::string& name) {
    for (auto& e : entries) if (e.name == name) return &e;
    return nullptr;
}
int main() {
    try {
        using namespace archive;
        // Apache, as the Wayback Machine serves it raw (id_): sorting links, a parent link and relative entries.
        const std::string apache = R"(<html><head><title>Index of /pokeymax_files</title></head><body><h1>Index of /pokeymax_files</h1>
<table><tr><th><a href="?C=N;O=D">Name</a></th><th><a href="?C=M;O=A">Last modified</a></th></tr>
<tr><td><img src="/icons/back.gif" alt="[PARENTDIR]"></td><td><a href="/">Parent Directory</a></td></tr>
<tr><td><a href="1.22_20210113/">1.22_20210113/</a></td></tr>
<tr><td><a href="README.html">README.html</a></td></tr>
<tr><td><a HREF='pokeymax%20user%20guide.pdf'>pokeymax user guide.pdf</a></td></tr>
<tr><td><a href="a&amp;b.txt">a&amp;b.txt</a></td></tr>
<tr><td><a href="https://64kib.com/pokeymax_files/software/">software/</a></td></tr>
<tr><td><a href="https://elsewhere.example/file.zip">elsewhere</a></td></tr>
<tr><td><a href="../other/">other</a></td><td><a href="mailto:x@example.com">mail</a></td></tr>
<tr><td><a href="README.html">again</a></td></tr></table></body></html>)";
        check(looksLikeFolderListing(apache), "Apache listing detected");
        auto entries = parseFolderListing(apache, "http://www.64kib.com/pokeymax_files/");
        check(entries.size() == 5, "Apache entries: two folders, three files, nothing else");
        auto folder = find(entries, "1.22_20210113");
        check(folder && folder->folder && folder->url == "http://www.64kib.com/pokeymax_files/1.22_20210113/", "Relative folder");
        auto pdf = find(entries, "pokeymax user guide.pdf");
        check(pdf && !pdf->folder && pdf->url == "http://www.64kib.com/pokeymax_files/pokeymax%20user%20guide.pdf", "Encoded name decoded, link kept");
        check(find(entries, "a&b.txt"), "HTML escapes in links");
        auto software = find(entries, "software");
        check(software && software->folder, "Absolute link into the folder (other host spelling)");
        check(!find(entries, "file.zip") && !find(entries, "other"), "Links outside the folder are left out");

        // nginx.
        const std::string nginx = R"(<html><head><title>Index of /pub/</title></head><body><h1>Index of /pub/</h1><hr><pre><a href="../">../</a>
<a href="tools/">tools/</a>                                              01-Jan-2024 10:00       -
<a href="image.iso">image.iso</a>                                         01-Jan-2024 10:00    1048576
</pre><hr></body></html>)";
        entries = parseFolderListing(nginx, "https://mirror.example.org/pub/");
        check(entries.size() == 2 && find(entries, "tools") && find(entries, "image.iso"), "nginx entries");
        check(find(entries, "image.iso")->url == "https://mirror.example.org/pub/image.iso", "nginx file link");

        // Python's http.server.
        check(looksLikeFolderListing("<title>Directory listing for /</title>"), "Python listing detected");
        check(!looksLikeFolderListing("<html><title>Welcome</title><a href=\"a.zip\">a</a></html>"), "Ordinary page is not a listing");

        // Wayback Machine links.
        WaybackLink link;
        check(parseWaybackLink("https://web.archive.org/web/20260216055916/http://www.64kib.com/pokeymax_files/", link), "Wayback link");
        check(link.time == "20260216055916" && link.original == "http://www.64kib.com/pokeymax_files/", "Wayback parts");
        check(parseWaybackLink("  web.archive.org/web/2023id_/https://example.com/a.zip", link) == false, "A link needs its scheme");
        check(parseWaybackLink("http://web.archive.org/web/2023id_/https:/example.com/a.zip#top", link) && link.time == "2023" && link.original == "https://example.com/a.zip", "Raw form, collapsed slashes, fragment");
        check(parseWaybackLink("https://web.archive.org/web/20200101000000/example.com/dir/", link) && link.original == "http://example.com/dir/", "Bare address");
        check(!isWaybackLink("https://web.archive.org/web/*/example.com/*"), "Search pages are not captures");
        check(!isWaybackLink("https://archive.org/details/item"), "archive.org items are not Wayback links");
        check(isFileSiteLink("https://web.archive.org/web/20260216055916/http://www.64kib.com/pokeymax_files/"), "Wayback links open the file picker");
        check(waybackRawUrl("2023", "http://example.com/a.zip") == "https://web.archive.org/web/2023id_/http://example.com/a.zip", "Raw capture address");
        check(waybackAlternateUrl("https://web.archive.org/web/2023id_/http://example.com/a.zip") == "https://web.archive.org/web/2023if_/http://example.com/a.zip", "Alternate raw form");
        check(waybackAlternateUrl("https://web.archive.org/web/2023/http://example.com/a.zip").empty(), "Only raw captures have the alternate form");

        // The capture index: the most complete capture of each file, any host spelling, nothing outside the folder.
        const std::string cdx =
            "http://www.64kib.com/pokeymax_files/ 20201126002139 1047 200\n"
            "http://www.64kib.com/pokeymax_files/guide.pdf 20201126015326 1040831 200\n"
            "https://www.64kib.com/pokeymax_files/guide.pdf 20210618233030 1040828 200\n"
            "http://www.64kib.com/pokeymax_files/guide.pdf 20231119085319 42622923 200\n"
            "http://64kib.com/pokeymax_files/guide.pdf 20251213162052 5236339 200\n"
            "http://64kib.com:80/pokeymax_files/1.29/build%20one/out.rbf 20250101000000 100 200\n"
            "http://64kib.com/pokeymax_files/1.29/ 20250101000000 100 200\n"
            "http://64kib.com/pokeymax_files/?C=N;O=D 20250101000000 100 200\n"
            "http://64kib.com/pokeymax_files/gone.zip 20250101000000 100 404\n"
            "http://64kib.com/other/file.zip 20250101000000 100 200\n";
        auto found = parseWaybackCaptures(cdx, "http://64kib.com/pokeymax_files/");
        check(found.size() == 2, "Two files captured below the folder");
        check(found[0].name == "1.29/build one/out.rbf" && found[0].original == "http://64kib.com:80/pokeymax_files/1.29/build%20one/out.rbf", "Nested file keeps its path");
        check(found[1].name == "guide.pdf" && found[1].time == "20231119085319" && found[1].length == 42622923, "Largest capture wins over truncated ones");
        check(looksLikeFolderUrl("https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/") && looksLikeFolderUrl("http://example.org/"), "Folder addresses");
        check(!looksLikeFolderUrl("https://www.youtube.com/watch?v=abc") && !looksLikeFolderUrl("https://example.org/file.zip") && !looksLikeFolderUrl("https://example.org/list/?sort=1"), "Pages and files are not folders");

        // FTP addresses, LIST output and byte ranges answered with restart offsets.
        auto ftpUrl = ftp::parseUrl("ftp://ftp.example.org:2121/pub/My%20Files/");
        check(ftpUrl.host == "ftp.example.org" && ftpUrl.port == 2121 && ftpUrl.path == "/pub/My Files/", "FTP address");
        check(ftp::parseUrl("FTP://ftp.example.org").path == "/" && ftp::parseUrl("ftp://h/x").port == 21, "FTP defaults");
        check(!ftp::isFtpUrl("ftp://user:secret@ftp.example.org/") && !ftp::isFtpUrl("http://example.org/"), "Only anonymous FTP links");
        check(isFileSiteLink("ftp://ftp.gnu.org/gnu/hello/"), "FTP links open the file picker");
        auto listed = ftp::parseList(
            "total 12\r\n"
            "drwxr-xr-x    2 ftp      ftp          4096 Jan 01  2020 sub folder\r\n"
            "-rw-r--r--    1 ftp      ftp       1048576 Mar 15 10:42 image.iso\r\n"
            "lrwxrwxrwx    1 ftp      ftp            12 Mar 15 10:42 latest.tar.gz -> hello-2.12.tar.gz\r\n"
            "-rw-r--r--    1 owner           77 Dec 31  1999 no-group.txt\r\n"
            "drwxr-xr-x    2 ftp      ftp          4096 Jan 01  2020 .\r\n"
            "01-02-20  03:04PM       <DIR>          Dos Folder\r\n"
            "01-02-20  03:04PM                  123 dos file.txt\r\n");
        check(listed.size() == 6, "FTP LIST entries");
        check(listed[0].name == "sub folder" && listed[0].folder, "Unix folder with a space");
        check(listed[1].name == "image.iso" && listed[1].sizeKnown && listed[1].size == 1048576, "Unix file size");
        check(listed[2].name == "latest.tar.gz" && !listed[2].folder && !listed[2].sizeKnown, "Symbolic link listed by its own name");
        check(listed[3].name == "no-group.txt" && listed[3].size == 77, "Listing without a group column");
        check(listed[4].name == "Dos Folder" && listed[4].folder && listed[5].name == "dos file.txt" && listed[5].size == 123, "MS-DOS listing");
        auto whole = ftp::answer(true, 1000, "20240102030405", "");
        check(whole.status == 200 && whole.lengthKnown && whole.length == 1000 && whole.etag == "\"ftp-1000-20240102030405\"", "Whole file");
        auto part = ftp::answer(true, 1000, "20240102030405", "Range: bytes=100-199\r\nIf-Range: \"ftp-1000-20240102030405\"\r\n");
        check(part.status == 206 && part.offset == 100 && part.length == 100 && part.contentRange == "bytes 100-199/1000", "Range becomes a restart offset");
        check(ftp::answer(true, 1000, "20240102030405", "Range: bytes=900-\r\n").contentRange == "bytes 900-999/1000", "Open-ended range");
        check(ftp::answer(true, 1000, "20250101000000", "Range: bytes=100-199\r\nIf-Range: \"ftp-1000-20240102030405\"\r\n").status == 200, "Changed file sends the whole file");
        check(ftp::answer(true, 1000, "", "Range: bytes=0-0\r\n").etag.empty(), "No modification time, no validator");
        bool pastEnd = false; try { ftp::answer(true, 1000, "", "Range: bytes=1000-1000\r\n"); } catch (const ftp::Error& e) { pastEnd = e.reply == 416; }
        check(pastEnd, "Range past the end");
        std::cout << "folder tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
}
