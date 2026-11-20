// The portable part of FTP support: addresses, LIST parsing and the HTTP-style answer that
// lets the download engine treat an FTP file like a server with byte ranges. See platform/ftp.h.
#include "platform/ftp.h"
#include "core.h"
#include <algorithm>
#include <regex>
#include <sstream>
namespace archive::ftp {
bool isFtpUrl(const std::string& input) {
    try { parseUrl(input); return true; } catch (const std::exception&) { return false; }
}
Url parseUrl(const std::string& input) {
    auto a = input.find_first_not_of(" \t\r\n"), b = input.find_last_not_of(" \t\r\n");
    static const std::regex address(R"(ftp://([^/?#\s@:]+)(?::([0-9]{1,5}))?(/[^?#\s]*)?)", std::regex::icase);
    std::smatch m; auto text = a == std::string::npos ? std::string() : input.substr(a, b - a + 1);
    if (!std::regex_match(text, m, address))
        throw std::runtime_error(text.find('@') != std::string::npos
            ? "FTP sign-in is not supported. Only anonymous FTP links (ftp://host/path) can be loaded."
            : "Not an FTP link.");
    Url url; url.host = m[1];
    if (m[2].matched) { url.port = (unsigned)std::stoul(m[2]); if (!url.port || url.port > 65535) throw std::runtime_error("Invalid FTP port."); }
    url.path = m[3].matched ? decode(m[3]) : "/";
    if (url.path.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("Invalid FTP path.");
    return url;
}
std::vector<Entry> parseList(const std::string& text) {
    static const std::vector<std::string> months = {"jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec"};
    static const std::regex dos(R"((\d{2}-\d{2}-\d{2,4})\s+(\d{1,2}:\d{2}\s*[AaPp][Mm])\s+(<DIR>|\d+)\s+(.+))");
    std::vector<Entry> out; std::istringstream in(text); std::string line;
    auto add = [&](Entry e) { if (!e.name.empty() && e.name != "." && e.name != "..") out.push_back(std::move(e)); };
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        std::smatch m;
        if (std::regex_match(line, m, dos)) {
            Entry e; e.name = m[4]; e.folder = m[3] == "<DIR>";
            if (!e.folder) { e.size = std::stoull(m[3]); e.sizeKnown = true; }
            add(e); continue;
        }
        const char kind = line[0];
        if (kind != '-' && kind != 'd' && kind != 'l') continue; // "total 12", other file types
        // Unix "ls -l": permissions links owner [group] size month day time-or-year name. The month
        // marks the columns, since some servers leave out the group.
        std::vector<std::pair<size_t, size_t>> tokens; // start, end
        for (size_t i = 0; i < line.size() && tokens.size() < 9;) {
            while (i < line.size() && line[i] == ' ') ++i;
            if (i >= line.size()) break;
            auto end = line.find(' ', i); if (end == std::string::npos) end = line.size();
            tokens.push_back({i, end}); i = end;
        }
        auto token = [&](size_t k) { return line.substr(tokens[k].first, tokens[k].second - tokens[k].first); };
        for (size_t k = 2; k + 3 < tokens.size(); ++k) {
            auto month = token(k); for (auto& c : month) c = (char)tolower((unsigned char)c);
            if (std::find(months.begin(), months.end(), month) == months.end()) continue;
            auto size = token(k - 1);
            if (size.empty() || size.find_first_not_of("0123456789") != std::string::npos) continue;
            auto start = tokens[k + 2].second; while (start < line.size() && line[start] == ' ') ++start;
            Entry e; e.name = line.substr(start); e.folder = kind == 'd';
            if (kind == 'l') { auto arrow = e.name.find(" -> "); if (arrow != std::string::npos) e.name.resize(arrow); }
            if (kind == '-' && size.size() < 20) { e.size = std::stoull(size); e.sizeKnown = true; }
            add(e); break;
        }
    }
    return out;
}
Answer answer(bool sizeKnown, uint64_t size, const std::string& modified, const std::string& headers) {
    Answer a;
    if (sizeKnown && !modified.empty()) a.etag = "\"ftp-" + std::to_string(size) + "-" + modified + "\"";
    static const std::regex rangeLine(R"(range:\s*bytes=([0-9]+)-([0-9]*)\s*)", std::regex::icase);
    static const std::regex ifRangeLine(R"(if-range:\s*(.*?)\s*)", std::regex::icase);
    bool ranged = false; uint64_t first = 0, last = UINT64_MAX; std::string ifRange;
    std::istringstream in(headers); std::string line; std::smatch m;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (std::regex_match(line, m, rangeLine)) { ranged = true; first = std::stoull(m[1]); if (m[2].length()) last = std::stoull(m[2]); }
        else if (std::regex_match(line, m, ifRangeLine)) ifRange = m[1];
    }
    if (ranged && sizeKnown && size && (ifRange.empty() || ifRange == a.etag)) {
        if (first >= size || last < first) throw Error("FTP: the requested part is past the end of the file.", 416);
        last = std::min(last, size - 1);
        a.status = 206; a.offset = first; a.length = last - first + 1; a.lengthKnown = true;
        a.contentRange = "bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" + std::to_string(size);
    } else { a.length = size; a.lengthKnown = sizeKnown; }
    return a;
}
}
