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
        std::cout << "Sign-in list tests passed." << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << std::endl;
        return 1;
    }
}
