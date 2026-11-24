#include "signins_format.h"
#include "json.hpp"
#include <algorithm>
#include <cctype>
namespace lite {
std::string signInService(const std::string& host, unsigned port) {
    return "SkeinDownloader/" + host + (port ? ":" + std::to_string(port) : std::string());
}
std::string storedUser(const archive::SignIn& s) {
    return s.kind == archive::SignInKind::Token ? "token" : s.user;
}
archive::SignIn suggestedSignIn(const std::string& host, bool basic) {
    archive::SignIn s;
    s.host = host;
    std::transform(s.host.begin(), s.host.end(), s.host.begin(), [](unsigned char c) {
        return char(std::tolower(c));
    });
    const auto& h = s.host;
    const bool archiveHost = h == "archive.org" || (h.size() > 12 && h.compare(h.size() - 12, 12, ".archive.org") == 0 && h != "web.archive.org");
    if (h == "huggingface.co" || h == "hf.co") {
        s.host = "huggingface.co";
        s.kind = archive::SignInKind::Token;
    } else if (h == "github.com" || h == "api.github.com") {
        s.host = "github.com";
        s.kind = archive::SignInKind::Token;
    } else if (archiveHost) {
        s.host = "archive.org";
        s.kind = archive::SignInKind::ArchiveKeys;
    } else
        s.kind = basic ? archive::SignInKind::Password : archive::SignInKind::Token;
    return s;
}
LinkUse signInUsableFor(const std::string& scheme, const std::string& host, const archive::SignIn& signIn) {
    auto low = [](std::string t) {
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) {
            return char(std::tolower(c));
        });
        return t;
    };
    const auto sch = low(scheme), h = low(host);
    if (sch == "https" || h == "127.0.0.1" || h == "::1" || h == "localhost") return LinkUse::Usable;
    if (signIn.kind != archive::SignInKind::Password) return LinkUse::NeverUnencrypted;
    return signIn.allowUnencrypted ? LinkUse::Usable : LinkUse::NeedsUnencryptedTick;
}
static const char* kindName(archive::SignInKind kind) {
    switch (kind) {
    case archive::SignInKind::Token: return "token";
    case archive::SignInKind::ArchiveKeys: return "archive";
    default: return "password";
    }
}
std::string signInsToJson(const std::vector<archive::SignIn>& signIns) {
    auto list = nlohmann::json::array();
    for (const auto& s : signIns) list.push_back({{"host", s.host}, {"port", s.port}, {"kind", kindName(s.kind)}, {"user", s.user}, {"unencrypted", s.allowUnencrypted}});
    return list.dump();
}
ParsedSignIns signInsFromJson(const std::string& text) {
    ParsedSignIns result;
    if (text.empty()) return result;
    auto json = nlohmann::json::parse(text, nullptr, false);
    if (!json.is_array()) return result;
    for (const auto& row : json) {
        try {
            archive::SignIn s;
            s.host = row.at("host").get<std::string>();
            std::transform(s.host.begin(), s.host.end(), s.host.begin(), [](unsigned char c) {
                return char(std::tolower(c));
            });
            const auto kind = row.at("kind").get<std::string>();
            if (kind == "password")
                s.kind = archive::SignInKind::Password;
            else if (kind == "token")
                s.kind = archive::SignInKind::Token;
            else if (kind == "archive")
                s.kind = archive::SignInKind::ArchiveKeys;
            else
                throw std::runtime_error("kind");
            const auto port = row.value("port", 0);
            if (s.host.empty() || port < 0 || port > 65535) throw std::runtime_error("host or port");
            s.port = unsigned(port);
            s.user = row.value("user", std::string());
            s.allowUnencrypted = row.value("unencrypted", false);
            result.entries.push_back(std::move(s));
        } catch (const std::exception&) {
            ++result.skipped;
        }
    }
    return result;
}
}
