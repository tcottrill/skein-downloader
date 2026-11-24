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
struct ParsedSignIns {
    std::vector<archive::SignIn> entries; // secret empty
    size_t skipped = 0;                   // entries that did not parse
};
// Reads that value back. A value that is empty or not a JSON array gives no entries;
// an entry that does not parse is skipped and counted.
ParsedSignIns signInsFromJson(const std::string&);
}
