// The sign-in table (src/signin.cpp), without the network.
#include "core.h"
#include "signin.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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
static bool covers(const std::string& scheme, const std::string& host, unsigned port = 443) {
    return archive::signInFor(scheme, host, port).has_value();
}

static void coverageTests() {
    using namespace archive;
    setSignIns({make(SignInKind::Token, "huggingface.co", 0, "", "hf"), make(SignInKind::Token, "github.com", 0, "", "gh"), make(SignInKind::ArchiveKeys, "archive.org", 0, "K", "S")});
    check(covers("https", "huggingface.co"), "A Hugging Face token covers huggingface.co.");
    check(covers("https", "hf.co"), "A Hugging Face token also covers hf.co.");
    check(!covers("https", "cdn-lfs.hf.co"), "A Hugging Face token does not cover hf.co subdomains.");
    check(!covers("https", "xyz.huggingface.co"), "A Hugging Face token does not cover huggingface.co subdomains.");
    check(!covers("https", "example.org"), "A Hugging Face token does not cover another host.");
    check(covers("https", "github.com"), "A GitHub token covers github.com.");
    check(covers("https", "api.github.com"), "A GitHub token also covers api.github.com.");
    check(!covers("https", "objects.githubusercontent.com"), "A GitHub token does not cover its storage hosts.");
    check(covers("https", "archive.org"), "The archive.org keys cover archive.org.");
    check(covers("https", "ia800100.us.archive.org"), "The archive.org keys cover its download servers.");
    check(covers("https", "www.archive.org"), "The archive.org keys cover www.archive.org.");
    check(!covers("https", "web.archive.org"), "The archive.org keys do not cover web.archive.org.");
    check(!covers("https", "notarchive.org"), "The archive.org keys do not cover a look-alike host.");
    check(!covers("https", "archive.org.evil.test"), "The archive.org keys do not cover archive.org as a prefix.");
    check(covers("https", "HuggingFace.CO"), "Host comparison ignores case.");
    check(covers("HTTPS", "github.com"), "Scheme comparison ignores case.");
    check(!covers("https", "github.com."), "A trailing dot is not a match.");
    check(!covers("https", " github.com"), "A surrounding space is not a match.");
    check(archive::signInFor("https", "hf.co", 443)->secret == "hf", "The covered sign-in is the saved one.");
    setSignIns({make(SignInKind::Token, "hf.co", 0, "", "other")});
    check(!covers("https", "huggingface.co"), "A token saved under hf.co does not extend to huggingface.co.");
    setSignIns({make(SignInKind::Password, "archive.org", 0, "u", "p")});
    check(!covers("https", "ia800100.us.archive.org"), "A password for archive.org covers only archive.org.");
    setSignIns({make(SignInKind::Token, "archive.org", 0, "", "t")});
    check(!covers("https", "ia800100.us.archive.org"), "A token for archive.org covers only archive.org.");
}

static void portTests() {
    using namespace archive;
    setSignIns({make(SignInKind::Password, "example.org", 8443, "u", "p")});
    check(covers("https", "example.org", 8443), "A port-specific sign-in matches its port.");
    check(!covers("https", "example.org", 443), "A port-specific sign-in does not match another port.");
    setSignIns({make(SignInKind::Password, "example.org", 0, "u", "p")});
    check(covers("https", "example.org", 443) && covers("https", "example.org", 8443), "Port 0 matches any port.");
    setSignIns({make(SignInKind::Password, "example.org", 0, "any", "p"), make(SignInKind::Password, "example.org", 8443, "specific", "p")});
    check(archive::signInFor("https", "example.org", 8443)->user == "specific", "The port-specific sign-in wins.");
    check(archive::signInFor("https", "example.org", 443)->user == "any", "The any-port sign-in covers other ports.");
    setSignIns({make(SignInKind::Password, "example.org", 8443, "specific", "p"), make(SignInKind::Password, "example.org", 0, "any", "p")});
    check(archive::signInFor("https", "example.org", 8443)->user == "specific", "The port-specific sign-in wins whatever the order.");
}

static void encryptionTests() {
    using namespace archive;
    setSignIns({make(SignInKind::Token, "example.org", 0, "", "t"), make(SignInKind::Password, "pw.example", 0, "u", "p"), make(SignInKind::Password, "ok.example", 0, "u", "p", true), make(SignInKind::Token, "127.0.0.1", 0, "", "t"), make(SignInKind::Password, "localhost", 0, "u", "p"),
                make(SignInKind::ArchiveKeys, "::1", 0, "K", "S")});
    check(covers("https", "example.org"), "A token is sent over https.");
    check(!covers("http", "example.org", 80), "A token is not sent over http.");
    check(!covers("ftp", "example.org", 21), "A token is not sent over ftp.");
    check(covers("https", "pw.example"), "A password is sent over https.");
    check(!covers("http", "pw.example", 80), "A password is not sent over http without allowUnencrypted.");
    check(!covers("ftp", "pw.example", 21), "A password is not sent over ftp without allowUnencrypted.");
    check(covers("http", "ok.example", 80), "A password is sent over http with allowUnencrypted.");
    check(covers("ftp", "ok.example", 21), "A password is sent over ftp with allowUnencrypted.");
    check(covers("http", "127.0.0.1", 80), "A token is sent over http to 127.0.0.1.");
    check(covers("http", "localhost", 80), "A password is sent over http to localhost.");
    check(covers("http", "::1", 80), "Keys are sent over http to ::1.");
    check(covers("ftp", "localhost", 21), "A password is sent over ftp to localhost.");
    check(hasSignIn("example.org", 80), "hasSignIn is true although the scheme refuses the sign-in.");
    check(hasSignIn("EXAMPLE.org", 443), "hasSignIn ignores case.");
    check(!hasSignIn("other.example", 443), "hasSignIn is false for an unknown host.");
    check(!hasSignIn("cdn.example.org", 443), "hasSignIn is false for a subdomain.");
}

static void headerTests() {
    using namespace archive;
    check(authorizationHeader(make(SignInKind::Password, "h", 0, "user", "pass")) == "Authorization: Basic dXNlcjpwYXNz\r\n", "A password gives a Basic header.");
    check(authorizationHeader(make(SignInKind::Token, "h", 0, "", "abc")) == "Authorization: Bearer abc\r\n", "A token gives a Bearer header.");
    check(authorizationHeader(make(SignInKind::ArchiveKeys, "h", 0, "K", "S")) == "Authorization: LOW K:S\r\n", "Keys give a LOW header.");
    check(authorizationHeader(make(SignInKind::Password, "h", 0, "a", "")) == "Authorization: Basic YTo=\r\n", "Base64 pads one byte short.");
    check(authorizationHeader(make(SignInKind::Password, "h", 0, "ab", "")) == "Authorization: Basic YWI6\r\n", "Base64 needs no padding for three bytes.");
    check(authorizationHeader(make(SignInKind::Password, "h", 0, "abc", "")) == "Authorization: Basic YWJjOg==\r\n", "Base64 pads two bytes short.");
    check(!sentAtOnce(make(SignInKind::Password, "h", 0, "u", "p")), "A password waits for a challenge.");
    check(sentAtOnce(make(SignInKind::Token, "h", 0, "", "t")), "A token is sent at once.");
    check(sentAtOnce(make(SignInKind::ArchiveKeys, "h", 0, "K", "S")), "Keys are sent at once.");
}

static void replaceTests() {
    using namespace archive;
    setSignIns({make(SignInKind::Password, "example.org", 0, "u", "p")});
    check(covers("https", "example.org"), "The table holds the saved sign-in.");
    setSignIns({});
    check(!covers("https", "example.org"), "A second call replaces the whole table.");
    check(!hasSignIn("example.org", 443), "An empty table has no sign-ins.");
    const auto before = signInGeneration();
    check(signInGeneration() == before, "Reading the table's generation does not change it.");
    setSignIns({make(SignInKind::Password, "example.org", 0, "u", "p")});
    const auto after = signInGeneration();
    check(after != before, "Replacing the table gives it a new generation.");
    setSignIns({make(SignInKind::Password, "example.org", 0, "u", "p")});
    check(signInGeneration() != after, "Every replacement gives a new generation, even with the same entries.");
    setSignIns({});
}

static void exceptionTests() {
    using namespace archive;
    SignInNeeded needed("example.org", false, true);
    check(std::string(needed.what()) == "Sign-in needed for example.org. Add it in Settings, then resume.", "The needed message names the host.");
    check(needed.host == "example.org" && !needed.rejected && needed.basic, "The needed fields are kept.");
    SignInNeeded rejected("example.org", true, false);
    check(std::string(rejected.what()) == "The sign-in for example.org was not accepted.", "The rejected message names the host.");
    check(rejected.rejected && !rejected.basic, "The rejected fields are kept.");
    const std::runtime_error& base = rejected;
    check(dynamic_cast<const NetworkError*>(&base) == nullptr, "SignInNeeded is not a NetworkError.");
}

int main() {
    try {
        coverageTests();
        portTests();
        encryptionTests();
        headerTests();
        replaceTests();
        exceptionTests();
        std::cout << "signin tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
}
