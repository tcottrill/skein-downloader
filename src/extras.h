#pragma once
#include "json.hpp"
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>
// What one link may send besides the engine's own headers: a referer, cookies and extra headers.
// They belong to that link: they go to its host (exact name and port) and to no other, whatever
// redirects happen. No value is ever logged or put in an error message; names may be.
namespace archive {
struct RequestExtras {
    std::string scheme, host; // of the link they belong to; host lower case, set by forLink()
    unsigned port = 0;        // the link's port (the scheme's default when the link names none)
    std::string referer, cookie;
    std::vector<std::pair<std::string, std::string>> headers; // name, value
    bool empty() const;                                       // no referer, cookie or header
};
// Empty when the extras are acceptable; otherwise an English sentence saying what is wrong
// (a forbidden header name, a name that is not a token, a control character, a duplicate of Referer or Cookie).
// The sentence names headers but never quotes a value.
std::string extrasError(const RequestExtras&);
// Parses "Name: value" lines (blank lines ignored) into headers; throws std::runtime_error with the offending line number.
std::vector<std::pair<std::string, std::string>> parseHeaderLines(const std::string& text);
// The same extras bound to a link's scheme, host and port. Throws for anything but an http or https link.
RequestExtras forLink(const std::string& link, RequestExtras extras);
// The header lines ("Name: value\r\n" each) for a request to scheme://host:port, or empty when the
// request is not to the extras' own host and port, or would downgrade from https to http.
// Referer first, then Cookie, then the headers in order.
std::string extraHeadersFor(const RequestExtras&, const std::string& scheme, const std::string& host, unsigned port);
// JSON for storing with a job, and back; an empty object for empty extras. Unknown or malformed input gives empty extras.
nlohmann::json extrasToJson(const RequestExtras&);
RequestExtras extrasFromJson(const nlohmann::json&);
// While it lives, every request made with this stop flag carries these extras where extraHeadersFor allows.
// One per stop flag at a time. Requests with no scope anywhere cost one atomic load.
class ExtrasScope {
public:
    ExtrasScope(const std::atomic_bool& stop, RequestExtras extras);
    ~ExtrasScope();
    ExtrasScope(const ExtrasScope&) = delete;
    ExtrasScope& operator=(const ExtrasScope&) = delete;

private:
    const std::atomic_bool* stop_;
};
// The non-empty extras in scope for this stop flag, or null (used by the engine's request code).
std::shared_ptr<const RequestExtras> extrasFor(const std::atomic_bool& stop);
// The names of the headers these extras carry ("Referer", "Cookie", then the custom names), for logs; never a value.
std::vector<std::string> extraHeaderNames(const RequestExtras&);
}
