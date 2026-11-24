#pragma once
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
namespace archive {
enum class SignInKind { Password,
                        Token,
                        ArchiveKeys };
struct SignIn {
    SignInKind kind = SignInKind::Password;
    std::string host;         // lower case, as saved: "example.org", "huggingface.co", "github.com", "archive.org"
    unsigned port = 0;        // 0: any port
    std::string user, secret; // Password: user name and password. Token: secret only. ArchiveKeys: access key and secret.
    bool allowUnencrypted = false;
};
// Replaces the whole table. Thread-safe; takes effect for requests built afterwards.
void setSignIns(std::vector<SignIn> signIns);
// The sign-in a request to scheme://host:port may carry, after the coverage and encryption rules; none otherwise.
std::optional<SignIn> signInFor(const std::string& scheme, const std::string& host, unsigned port);
// True when a sign-in is saved for this host at all, whatever the scheme (to tell "refused, none saved" from "may not be sent").
bool hasSignIn(const std::string& host, unsigned port);
// "Authorization: <value>\r\n" for HTTP.
std::string authorizationHeader(const SignIn&);
// Sent with the first request (tokens, archive.org keys) or only after a Basic challenge (passwords).
bool sentAtOnce(const SignIn&);
// A server refused for lack of a sign-in (rejected == false) or refused the one that was sent (rejected == true).
struct SignInNeeded : std::runtime_error {
    std::string host;
    bool rejected, basic; // basic: the server offered HTTP Basic
    SignInNeeded(const std::string& host, bool rejected, bool basic);
};
}
