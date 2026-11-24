#pragma once
// The parts of the saved sign-ins that need no window and no credential store: the list kept
// in settings.ini (no secrets) and the credential store's naming. Linked by the tests.
#include "signin.h"
#include <string>
#include <vector>
namespace lite {
// "SkeinDownloader/<host>" or "SkeinDownloader/<host>:<port>" (port 0: any port).
std::string signInService(const std::string& host, unsigned port);
// The user name kept next to the secret: SignIn::user, or the literal "token" for a Token.
std::string storedUser(const archive::SignIn&);
// The settings.ini value: a JSON array of {"host","port","kind","user","unencrypted"}, never a secret.
std::string signInsToJson(const std::vector<archive::SignIn>&);
// The sign-in the dialog starts from when a server refused a load: Hugging Face, GitHub and archive.org hosts
// give their site's token or keys under the site's own host (huggingface.co, github.com, archive.org; not
// web.archive.org); any other host gives a password when the server offered Basic, a token otherwise.
// The host is lower-cased; no port, no secret, no permission for unencrypted use.
archive::SignIn suggestedSignIn(const std::string& host, bool basic);
// Whether a sign-in can be sent on a link of this scheme ("https", "http", "ftp") to this host, by the engine's rules:
// https and the loopback hosts (127.0.0.1, ::1, localhost) are encrypted; elsewhere a password needs allowUnencrypted,
// and a token or archive.org keys are never sent.
enum class LinkUse { Usable,
                     NeedsUnencryptedTick,
                     NeverUnencrypted };
LinkUse signInUsableFor(const std::string& scheme, const std::string& host, const archive::SignIn& signIn);
struct ParsedSignIns {
    std::vector<archive::SignIn> entries; // secret empty
    size_t skipped = 0;                   // entries that did not parse
};
// Reads that value back. A value that is empty or not a JSON array gives no entries;
// an entry that does not parse is skipped and counted.
ParsedSignIns signInsFromJson(const std::string&);
}
