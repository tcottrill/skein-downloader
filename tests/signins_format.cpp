// The sign-in list kept in settings.ini and the credential store's service names (src/wx/signins_format.cpp).
#include "signins_format.h"
#include <iostream>
#include <stdexcept>

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static archive::SignIn make(archive::SignInKind kind, const std::string& host, unsigned port, const std::string& user, const std::string& secret, bool unencrypted = false) {
    archive::SignIn s;
    s.kind = kind;
    s.host = host;
    s.port = port;
    s.user = user;
    s.secret = secret;
    s.allowUnencrypted = unencrypted;
    return s;
}

int main() {
    try {
        using archive::SignInKind;
        check(lite::signInService("example.org", 0) == "SkeinDownloader/example.org", "The service name without a port is SkeinDownloader/<host>.");
        check(lite::signInService("example.org", 8080) == "SkeinDownloader/example.org:8080", "The service name with a port is SkeinDownloader/<host>:<port>.");
        check(lite::storedUser(make(SignInKind::Token, "github.com", 0, "ignored", "s")) == "token", "A token is stored under the user name token.");
        check(lite::storedUser(make(SignInKind::Password, "example.org", 0, "bob", "s")) == "bob", "A password is stored under its user name.");

        std::vector<archive::SignIn> in = {make(SignInKind::Password, "example.org", 8080, "bob", "SECRET-ONE", true), make(SignInKind::Token, "huggingface.co", 0, "", "SECRET-TWO"), make(SignInKind::ArchiveKeys, "archive.org", 0, "ACCESS", "SECRET-THREE")};
        auto json = lite::signInsToJson(in);
        check(json.find("SECRET") == std::string::npos, "The saved list holds no secret.");
        auto out = lite::signInsFromJson(json);
        check(out.entries.size() == 3 && out.skipped == 0, "The list reads back with its three entries.");
        check(out.entries[0].host == "example.org" && out.entries[0].port == 8080 && out.entries[0].user == "bob" && out.entries[0].kind == SignInKind::Password && out.entries[0].allowUnencrypted, "A password entry keeps host, port, user, kind and the unencrypted flag.");
        check(out.entries[1].kind == SignInKind::Token && out.entries[1].host == "huggingface.co" && !out.entries[1].allowUnencrypted, "A token entry reads back.");
        check(out.entries[2].kind == SignInKind::ArchiveKeys && out.entries[2].user == "ACCESS", "An archive.org entry keeps its access key.");
        check(out.entries[0].secret.empty() && out.entries[2].secret.empty(), "Entries read from the list have no secret.");

        check(lite::signInsFromJson("").entries.empty() && lite::signInsFromJson("").skipped == 0, "An empty setting is an empty list.");
        check(lite::signInsFromJson("not json").entries.empty(), "Text that is not JSON is an empty list.");
        check(lite::signInsFromJson("{\"a\":1}").entries.empty(), "JSON that is not an array is an empty list.");
        auto bad = lite::signInsFromJson(R"([{"host":"a.org","port":0,"kind":"password","user":"u","unencrypted":false},
            {"host":"","port":0,"kind":"password","user":"u"},
            {"host":"b.org","port":70000,"kind":"password","user":"u"},
            {"host":"c.org","port":0,"kind":"nonsense","user":"u"},
            {"host":"d.org","kind":"token"},
            7,
            {"host":"e.org","port":"x","kind":"token"}])");
        check(bad.entries.size() == 2 && bad.entries[0].host == "a.org" && bad.entries[1].host == "d.org", "Entries that do not parse are skipped, the others kept.");
        check(bad.skipped == 5, "Skipped entries are counted.");
        check(lite::signInsFromJson(R"([{"host":"A.Org","port":0,"kind":"token"}])").entries[0].host == "a.org", "Hosts are kept in lower case.");
        // What the sign-in dialog starts from for a refused host.
        auto preset = [](const std::string& host, bool basic) {
            return lite::suggestedSignIn(host, basic);
        };
        for (const char* host : {"huggingface.co", "hf.co", "HuggingFace.co"})
            check(preset(host, false).host == "huggingface.co" && preset(host, true).kind == SignInKind::Token, "Hugging Face hosts give the Hugging Face token, kept under huggingface.co.");
        for (const char* host : {"github.com", "api.github.com"})
            check(preset(host, true).host == "github.com" && preset(host, true).kind == SignInKind::Token, "GitHub hosts give the GitHub token, kept under github.com.");
        for (const char* host : {"archive.org", "ia800001.us.archive.org", "ia.archive.org"})
            check(preset(host, false).host == "archive.org" && preset(host, false).kind == SignInKind::ArchiveKeys, "archive.org hosts give the archive.org keys, kept under archive.org.");
        check(preset("web.archive.org", true).host == "web.archive.org" && preset("web.archive.org", true).kind == SignInKind::Password, "The Wayback Machine is an ordinary server.");
        check(preset("notarchive.org", false).host == "notarchive.org" && preset("notarchive.org", false).kind == SignInKind::Token, "A host that only ends in the letters of archive.org is an ordinary server.");
        check(preset("example.org", true).kind == SignInKind::Password && preset("example.org", true).host == "example.org", "A server that offered Basic gets a password.");
        check(preset("Example.ORG", false).kind == SignInKind::Token && preset("Example.ORG", false).host == "example.org", "Any other server gets a token, with the host in lower case.");
        check(preset("example.org", true).port == 0 && preset("example.org", true).secret.empty() && !preset("example.org", true).allowUnencrypted, "The preset has no port, no secret and no unencrypted permission.");
        // Whether a sign-in can be sent over a link's scheme.
        {
            using lite::LinkUse;
            auto pw = [](bool unencrypted) {
                return make(SignInKind::Password, "example.org", 0, "u", "p", unencrypted);
            };
            check(lite::signInUsableFor("https", "example.org", pw(false)) == LinkUse::Usable, "A password over https needs no permission.");
            check(lite::signInUsableFor("ftp", "example.org", pw(false)) == LinkUse::NeedsUnencryptedTick, "An FTP password needs the unencrypted permission.");
            check(lite::signInUsableFor("ftp", "example.org", pw(true)) == LinkUse::Usable, "An FTP password with the permission is usable.");
            check(lite::signInUsableFor("http", "example.org", pw(false)) == LinkUse::NeedsUnencryptedTick, "An http password needs the unencrypted permission.");
            check(lite::signInUsableFor("http", "example.org", pw(true)) == LinkUse::Usable, "An http password with the permission is usable.");
            for (const char* host : {"127.0.0.1", "::1", "localhost"}) {
                check(lite::signInUsableFor("http", host, pw(false)) == LinkUse::Usable, "A loopback host counts as encrypted for http.");
                check(lite::signInUsableFor("ftp", host, pw(false)) == LinkUse::Usable, "A loopback host counts as encrypted for ftp.");
            }
            check(lite::signInUsableFor("http", "localhost.example.org", pw(false)) == LinkUse::NeedsUnencryptedTick, "A name that only starts with localhost is not loopback.");
            for (auto kind : {SignInKind::Token, SignInKind::ArchiveKeys}) {
                auto s = make(kind, "example.org", 0, "u", "p", true);
                check(lite::signInUsableFor("http", "example.org", s) == LinkUse::NeverUnencrypted && lite::signInUsableFor("ftp", "example.org", s) == LinkUse::NeverUnencrypted, "A token or key is never sent unencrypted, whatever the permission.");
                check(lite::signInUsableFor("https", "example.org", s) == LinkUse::Usable && lite::signInUsableFor("http", "127.0.0.1", s) == LinkUse::Usable, "A token over https or loopback is usable.");
            }
            check(lite::signInUsableFor("HTTP", "Example.org", pw(false)) == LinkUse::NeedsUnencryptedTick, "The scheme and host are compared in lower case.");
        }
        std::cout << "Sign-in list tests passed." << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << std::endl;
        return 1;
    }
}
