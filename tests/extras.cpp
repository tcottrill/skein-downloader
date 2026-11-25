// Request extras (src/extras.cpp): validation, parsing, binding to a link, and where they are sent, without the network.
#include "extras.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>

static void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
static archive::RequestExtras withHeader(const std::string& name, const std::string& value) {
    archive::RequestExtras e;
    e.headers.push_back({name, value});
    return e;
}
template <typename F>
static std::string thrown(F&& f) {
    try {
        f();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

static void validationTests() {
    using namespace archive;
    check(extrasError(RequestExtras{}).empty(), "Empty extras are acceptable.");
    check(extrasError(withHeader("X-Token", "abc")).empty(), "X-Token: abc is acceptable.");
    for (const char* name : {"Host", "Range", "If-Range", "Content-Length", "Transfer-Encoding", "Connection", "Accept-Encoding", "Authorization", "User-Agent"}) {
        check(!extrasError(withHeader(name, "x")).empty(), std::string("The header ") + name + " is forbidden.");
        std::string upper = name, low = name;
        for (auto& c : upper) c = (char)toupper((unsigned char)c);
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        check(!extrasError(withHeader(upper, "x")).empty(), "The header " + upper + " is forbidden in capitals.");
        check(!extrasError(withHeader(low, "x")).empty(), "The header " + low + " is forbidden in lower case.");
    }
    check(!extrasError(withHeader("Referer", "x")).empty(), "Referer among the headers is rejected (it has its own field).");
    check(!extrasError(withHeader("cookie", "x")).empty(), "Cookie among the headers is rejected (it has its own field).");
    check(!extrasError(withHeader("Bad Name", "x")).empty(), "A header name with a space is rejected.");
    check(!extrasError(withHeader("Bad:Name", "x")).empty(), "A header name with a colon is rejected.");
    check(!extrasError(withHeader("", "x")).empty(), "An empty header name is rejected.");
    check(extrasError(withHeader("X-Ok!#$%&'*+-.^_`|~9", "x")).empty(), "Every token character is allowed in a name.");
    const std::string nul = std::string("a") + '\0' + "b";
    for (const std::string& bad : {std::string("a\rb"), std::string("a\nb"), nul}) {
        check(!extrasError(withHeader("X-A", bad)).empty(), "A header value with a CR, LF or NUL is rejected.");
        RequestExtras cookie;
        cookie.cookie = bad;
        check(!extrasError(cookie).empty(), "A cookie with a CR, LF or NUL is rejected.");
        RequestExtras referer;
        referer.referer = bad;
        check(!extrasError(referer).empty(), "A referer with a CR, LF or NUL is rejected.");
        check(!extrasError(withHeader(bad, "x")).empty(), "A header name with a CR, LF or NUL is rejected.");
    }
    // The sentence may name a header but never quotes a value.
    const auto message = extrasError(withHeader("X-A", "secret-value\r\nInjected: 1"));
    check(!message.empty() && message.find("secret-value") == std::string::npos, "The error for a bad value does not quote it.");
}

static void parseTests() {
    using namespace archive;
    const auto parsed = parseHeaderLines("A: 1\n\nB:2\r\n");
    check(parsed.size() == 2 && parsed[0].first == "A" && parsed[0].second == "1" && parsed[1].first == "B" && parsed[1].second == "2", "Header lines parse to {A,1},{B,2}.");
    const auto spaced = parseHeaderLines("  X-Name :   some value  \n");
    check(spaced.size() == 1 && spaced[0].first == "X-Name" && spaced[0].second == "some value", "Names and values are trimmed.");
    check(parseHeaderLines("").empty() && parseHeaderLines("\n\r\n").empty(), "Blank text has no headers.");
    const auto first = thrown([] {
        parseHeaderLines("no colon here");
    });
    check(first.find("1") != std::string::npos, "A line without a colon throws naming line 1, not \"" + first + "\".");
    const auto third = thrown([] {
        parseHeaderLines("A: 1\n\nbad line");
    });
    check(third.find("3") != std::string::npos, "The offending line number counts blank lines, not \"" + third + "\".");
    check(third.find("bad line") == std::string::npos, "The error does not quote the line.");
}

static void forLinkTests() {
    using namespace archive;
    RequestExtras base;
    base.cookie = "a=b";
    auto e = forLink("https://Example.org:8443/x", base);
    check(e.scheme == "https" && e.host == "example.org" && e.port == 8443 && e.cookie == "a=b", "forLink binds scheme, lower-case host and port.");
    e = forLink("https://example.org/x", base);
    check(e.port == 443, "An https link without a port is bound to 443.");
    e = forLink("http://example.org/x", base);
    check(e.port == 80 && e.scheme == "http", "An http link without a port is bound to 80.");
    e = forLink("http://user:pw@Example.org:81?q=1", base);
    check(e.host == "example.org" && e.port == 81, "User information and a missing path do not confuse the host and port.");
    e = forLink("http://[::1]:5000/x", base);
    check(e.host == "::1" && e.port == 5000, "An IPv6 host is bound without its brackets.");
    check(!thrown([&] {
               forLink("ftp://example.org/x", base);
           }).empty(),
          "An ftp link cannot carry extras.");
    check(!thrown([&] {
               forLink("example.org/x", base);
           }).empty(),
          "A link without a scheme cannot carry extras.");
}

static void sendTests() {
    using namespace archive;
    RequestExtras e;
    e.referer = "https://example.org/page";
    e.cookie = "pass=1";
    e.headers = {{"X-Skein-Test", "yes"}, {"X-Two", "2"}};
    const auto https = forLink("https://example.org/x", e);
    const std::string expected = "Referer: https://example.org/page\r\nCookie: pass=1\r\nX-Skein-Test: yes\r\nX-Two: 2\r\n";
    check(extraHeadersFor(https, "https", "example.org", 443) == expected, "The same scheme, host and port gets referer, cookie, then the headers in order.");
    check(extraHeadersFor(https, "https", "EXAMPLE.org", 443) == expected, "The host is compared without regard to case.");
    check(extraHeadersFor(https, "https", "other.org", 443).empty(), "Another host gets nothing.");
    check(extraHeadersFor(https, "https", "example.org", 8443).empty(), "Another port gets nothing.");
    check(extraHeadersFor(https, "https", "www.example.org", 443).empty(), "A subdomain gets nothing.");
    check(extraHeadersFor(https, "http", "example.org", 443).empty(), "Extras bound to an https link are never sent over http.");
    const auto http = forLink("http://example.org:8080/x", e);
    check(extraHeadersFor(http, "http", "example.org", 8080) == expected, "An http link's extras go to the same host and port.");
    check(extraHeadersFor(http, "https", "example.org", 8080) == expected, "An http link's extras may go over https to the same host and port.");
    check(extraHeadersFor(http, "https", "example.org", 443).empty(), "An http link's extras do not go over https to another port.");
    check(extraHeadersFor(RequestExtras{}, "https", "example.org", 443).empty(), "Empty extras send nothing.");
}

static void jsonTests() {
    using namespace archive;
    check(extrasToJson(RequestExtras{}).is_object() && extrasToJson(RequestExtras{}).empty(), "Empty extras are an empty JSON object.");
    check(extrasFromJson(nlohmann::json::object()).empty(), "An empty object is empty extras.");
    RequestExtras e;
    e.referer = "https://example.org/page";
    e.cookie = "pass=1";
    e.headers = {{"X-A", "1"}, {"X-B", "2"}};
    e = forLink("https://example.org:8443/x", e);
    const auto back = extrasFromJson(nlohmann::json::parse(extrasToJson(e).dump()));
    check(back.scheme == e.scheme && back.host == e.host && back.port == e.port && back.referer == e.referer && back.cookie == e.cookie && back.headers == e.headers, "Extras survive a JSON round trip.");
    check(extrasFromJson(nlohmann::json::parse("{\"cookie\":\"c=1\"}")).cookie == "c=1", "Extras with only a cookie load.");
    for (const char* garbage : {"[]", "5", "\"x\"", "null", "{\"cookie\":5}", "{\"headers\":5}", "{\"headers\":[[\"A\"]]}", "{\"headers\":[[\"A\",1]]}", "{\"port\":\"x\"}", "{\"headers\":[[\"Host\",\"x\"]]}", "{\"cookie\":\"a\\r\\nB: c\"}"})
        check(extrasFromJson(nlohmann::json::parse(garbage)).empty(), std::string("Malformed input gives empty extras: ") + garbage);
}

static void scopeTests() {
    using namespace archive;
    std::atomic_bool stop = false, other = false;
    check(!extrasFor(stop), "With no scope a stop flag has no extras.");
    RequestExtras e = forLink("https://example.org/x", withHeader("X-A", "1"));
    {
        ExtrasScope scope(stop, e);
        const auto found = extrasFor(stop);
        check(found && found->headers.size() == 1 && found->host == "example.org", "A scope makes its extras visible for its stop flag.");
        check(!extrasFor(other), "Another stop flag sees nothing.");
    }
    check(!extrasFor(stop), "After the scope is gone the stop flag has no extras.");
    {
        ExtrasScope scope(stop, RequestExtras{});
        check(!extrasFor(stop), "A scope of empty extras counts as none.");
    }
}

int main() {
    try {
        validationTests();
        parseTests();
        forLinkTests();
        sendTests();
        jsonTests();
        scopeTests();
        std::cout << "extras tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
}
