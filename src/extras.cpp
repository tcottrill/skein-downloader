#include "extras.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <stdexcept>
namespace archive {
namespace {
std::string lowerAscii(std::string text) {
    for (auto& c : text) c = (char)std::tolower((unsigned char)c);
    return text;
}
std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}
bool hasLineBreakOrNul(const std::string& text) {
    return text.find_first_of(std::string("\r\n") + '\0') != std::string::npos;
}
// An HTTP token (RFC 9110): letters, digits and !#$%&'*+-.^_`|~
bool isToken(const std::string& name) {
    if (name.empty()) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || (c < 0x80 && std::string("!#$%&'*+-.^_`|~").find((char)c) != std::string::npos);
    });
}
// The headers the engine owns (Decisions in the design); Referer and Cookie have their own fields.
bool isForbidden(const std::string& lowerName) {
    static const char* const names[] = {"host", "range", "if-range", "content-length", "transfer-encoding", "connection", "accept-encoding", "authorization", "user-agent"};
    return std::any_of(std::begin(names), std::end(names), [&](const char* name) {
        return lowerName == name;
    });
}
// The registry behind ExtrasScope: stop flag address to extras. Reads look it up once per request;
// scopes_ lets a request with no scope anywhere skip the lock.
std::mutex scopeLock;
std::map<const std::atomic_bool*, std::shared_ptr<const RequestExtras>> scoped;
std::atomic<size_t> scopes_{0};
}
bool RequestExtras::empty() const {
    return referer.empty() && cookie.empty() && headers.empty();
}
std::string extrasError(const RequestExtras& extras) {
    if (hasLineBreakOrNul(extras.referer)) return "The referer contains a line break or a null character.";
    if (hasLineBreakOrNul(extras.cookie)) return "The cookies contain a line break or a null character.";
    for (const auto& [name, value] : extras.headers) {
        if (hasLineBreakOrNul(name)) return "A header name contains a line break or a null character.";
        if (!isToken(name)) return "A header name is empty or has a character that is not allowed in a name (a space, a colon or another separator).";
        const auto key = lowerAscii(name);
        if (key == "referer") return "Referer has its own field; remove it from the extra headers.";
        if (key == "cookie") return "Cookie has its own field; remove it from the extra headers.";
        if (isForbidden(key)) return "The header " + name + " is set by Skein and cannot be changed here.";
        if (hasLineBreakOrNul(value)) return "The value of the header " + name + " contains a line break or a null character.";
    }
    return {};
}
std::vector<std::pair<std::string, std::string>> parseHeaderLines(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> headers;
    size_t number = 0;
    for (size_t at = 0; at <= text.size();) {
        auto end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        ++number;
        auto line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trimmed(line).empty()) continue;
        const auto colon = line.find(':');
        if (colon == std::string::npos) throw std::runtime_error("Line " + std::to_string(number) + " of the extra headers has no colon; write each as Name: value.");
        headers.push_back({trimmed(line.substr(0, colon)), trimmed(line.substr(colon + 1))});
    }
    return headers;
}
RequestExtras forLink(const std::string& link, RequestExtras extras) {
    const auto schemeEnd = link.find("://");
    const auto scheme = schemeEnd == std::string::npos ? std::string() : lowerAscii(link.substr(0, schemeEnd));
    if (scheme != "http" && scheme != "https") throw std::runtime_error("A referer, cookies and headers can only be used with an http or https link.");
    auto authority = link.substr(schemeEnd + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (const auto at = authority.rfind('@'); at != std::string::npos) authority = authority.substr(at + 1);
    std::string host, port;
    if (!authority.empty() && authority[0] == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) throw std::runtime_error("The link's address is not valid.");
        host = authority.substr(1, close - 1);
        const auto rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest[0] != ':') throw std::runtime_error("The link's address is not valid.");
            port = rest.substr(1);
        }
    } else {
        const auto colon = authority.rfind(':');
        host = authority.substr(0, colon);
        if (colon != std::string::npos) port = authority.substr(colon + 1);
    }
    if (host.empty()) throw std::runtime_error("The link's address is not valid.");
    unsigned number = scheme == "https" ? 443 : 80;
    if (!port.empty()) {
        if (port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) throw std::runtime_error("The link's port is not valid.");
        number = (unsigned)std::stoul(port);
        if (!number || number > 65535) throw std::runtime_error("The link's port is not valid.");
    }
    extras.scheme = scheme;
    extras.host = lowerAscii(host);
    extras.port = number;
    return extras;
}
std::string extraHeadersFor(const RequestExtras& extras, const std::string& scheme, const std::string& host, unsigned port) {
    if (extras.empty() || extras.host.empty() || lowerAscii(host) != extras.host || port != extras.port) return {};
    // Never down from https: a link that was https keeps its extras off an unencrypted request.
    if (extras.scheme == "https" && scheme != "https") return {};
    std::string out;
    if (!extras.referer.empty()) out += "Referer: " + extras.referer + "\r\n";
    if (!extras.cookie.empty()) out += "Cookie: " + extras.cookie + "\r\n";
    for (const auto& [name, value] : extras.headers) out += name + ": " + value + "\r\n";
    return out;
}
std::vector<std::string> extraHeaderNames(const RequestExtras& extras) {
    std::vector<std::string> names;
    if (!extras.referer.empty()) names.push_back("Referer");
    if (!extras.cookie.empty()) names.push_back("Cookie");
    for (const auto& header : extras.headers) names.push_back(header.first);
    return names;
}
nlohmann::json extrasToJson(const RequestExtras& extras) {
    auto json = nlohmann::json::object();
    if (extras.empty()) return json;
    json["scheme"] = extras.scheme;
    json["host"] = extras.host;
    json["port"] = extras.port;
    if (!extras.referer.empty()) json["referer"] = extras.referer;
    if (!extras.cookie.empty()) json["cookie"] = extras.cookie;
    if (!extras.headers.empty()) {
        json["headers"] = nlohmann::json::array();
        for (const auto& [name, value] : extras.headers) json["headers"].push_back(nlohmann::json::array({name, value}));
    }
    return json;
}
RequestExtras extrasFromJson(const nlohmann::json& json) {
    RequestExtras extras;
    try {
        if (!json.is_object()) return {};
        const auto text = [&](const char* key, std::string& out) {
            if (!json.contains(key)) return true;
            if (!json[key].is_string()) return false;
            out = json[key].get<std::string>();
            return true;
        };
        if (!text("scheme", extras.scheme) || !text("host", extras.host) || !text("referer", extras.referer) || !text("cookie", extras.cookie)) return {};
        if (json.contains("port")) {
            if (!json["port"].is_number_unsigned() || json["port"].get<uint64_t>() > 65535) return {};
            extras.port = json["port"].get<unsigned>();
        }
        if (json.contains("headers")) {
            if (!json["headers"].is_array()) return {};
            for (const auto& pair : json["headers"]) {
                if (!pair.is_array() || pair.size() != 2 || !pair[0].is_string() || !pair[1].is_string()) return {};
                extras.headers.push_back({pair[0].get<std::string>(), pair[1].get<std::string>()});
            }
        }
    } catch (const std::exception&) {
        return {};
    }
    // A history file edited by hand must not be able to split a header.
    if (!extrasError(extras).empty()) return {};
    return extras;
}
ExtrasScope::ExtrasScope(const std::atomic_bool& stop, RequestExtras extras)
    : stop_(&stop) {
    if (extras.empty()) return;
    std::lock_guard lock(scopeLock);
    scoped[stop_] = std::make_shared<const RequestExtras>(std::move(extras));
    scopes_ = scoped.size();
}
ExtrasScope::~ExtrasScope() {
    std::lock_guard lock(scopeLock);
    scoped.erase(stop_);
    scopes_ = scoped.size();
}
std::shared_ptr<const RequestExtras> extrasFor(const std::atomic_bool& stop) {
    if (!scopes_.load(std::memory_order_relaxed)) return nullptr;
    std::lock_guard lock(scopeLock);
    const auto found = scoped.find(&stop);
    return found == scoped.end() ? nullptr : found->second;
}
}
