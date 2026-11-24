// The sign-in table: which saved sign-in a request may carry, and how it is written as a header.
#include "signin.h"
#include "core.h"
#include <mutex>
#include <utility>
namespace archive {
namespace {
std::mutex tableMutex;
std::vector<SignIn> table;
uint64_t generation = 0; // guarded by tableMutex

bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}
bool isLoopback(const std::string& host) {
    return host == "127.0.0.1" || host == "::1" || host == "localhost";
}
// True when this saved sign-in covers the (lower case) host: the saved host itself, plus the three
// named extensions of the design.
bool coversHost(const SignIn& s, const std::string& host) {
    if (host == s.host) return true;
    if (s.kind == SignInKind::Token && s.host == "huggingface.co") return host == "hf.co";
    if (s.kind == SignInKind::Token && s.host == "github.com") return host == "api.github.com";
    if (s.kind == SignInKind::ArchiveKeys && s.host == "archive.org") return host != "web.archive.org" && endsWith(host, ".archive.org");
    return false;
}
// The covering sign-in for this host and port, the port-specific one first.
std::optional<SignIn> find(const std::string& host, unsigned port) {
    const std::string h = lower(host);
    std::lock_guard<std::mutex> lock(tableMutex);
    const SignIn* any = nullptr;
    for (const SignIn& s : table) {
        if (!coversHost(s, h)) continue;
        if (s.port != 0) {
            if (s.port == port) return s;
        } else if (!any) {
            any = &s;
        }
    }
    if (any) return *any;
    return std::nullopt;
}
std::string base64(const std::string& data) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8) | static_cast<unsigned char>(data[i + 2]);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
    }
    if (data.size() - i == 1) {
        const unsigned v = static_cast<unsigned char>(data[i]) << 16;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += "==";
    } else if (data.size() - i == 2) {
        const unsigned v = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}
}

void setSignIns(std::vector<SignIn> signIns) {
    for (SignIn& s : signIns) s.host = lower(s.host);
    std::lock_guard<std::mutex> lock(tableMutex);
    table = std::move(signIns);
    ++generation;
}

uint64_t signInGeneration() {
    std::lock_guard<std::mutex> lock(tableMutex);
    return generation;
}

std::optional<SignIn> signInFor(const std::string& scheme, const std::string& host, unsigned port) {
    std::optional<SignIn> s = find(host, port);
    if (!s) return std::nullopt;
    const bool encrypted = lower(scheme) == "https" || isLoopback(lower(host));
    if (encrypted) return s;
    if (s->kind == SignInKind::Password && s->allowUnencrypted) return s;
    return std::nullopt;
}

bool hasSignIn(const std::string& host, unsigned port) {
    return find(host, port).has_value();
}

std::string authorizationHeader(const SignIn& s) {
    switch (s.kind) {
    case SignInKind::Token: return "Authorization: Bearer " + s.secret + "\r\n";
    case SignInKind::ArchiveKeys: return "Authorization: LOW " + s.user + ":" + s.secret + "\r\n";
    case SignInKind::Password: break;
    }
    return "Authorization: Basic " + base64(s.user + ":" + s.secret) + "\r\n";
}

bool sentAtOnce(const SignIn& s) {
    return s.kind != SignInKind::Password;
}

SignInNeeded::SignInNeeded(const std::string& host, bool rejected, bool basic)
    : std::runtime_error(rejected ? "The sign-in for " + host + " was not accepted." : "Sign-in needed for " + host + ". Add it in Settings, then resume."), host(host), rejected(rejected), basic(basic) {}
}
