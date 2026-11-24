#pragma once
// Saved sign-ins: the list in settings.ini (no secrets), the secrets in the system's credential
// store (wxSecretStore: Windows Credential Manager, or the Secret Service on Linux), and the
// table handed to the engine (archive::setSignIns).
//
// Every function here is a GUI-thread function: the table is a single in-memory vector with no
// lock. The engine's own table is thread-safe; this one is not.
#include "signin.h"
#include <string>
#include <vector>
namespace lite {
struct SavedSignIn {         // one row of the list; the secret is not kept here once handed over
    archive::SignIn value;   // secret always empty in what signIns() returns
    bool remembered = false; // written to the credential store (and listed in settings.ini)
    bool available = true;   // false: listed in settings.ini but its secret could not be read
};
// True when the credential store can be used here (wxSecretStore::IsOk). Always false in a
// build without wxUSE_SECRETSTORE, and on a system with no running Secret Service: sign-ins are
// then kept only until the program closes. Checked once, then remembered.
bool signInStoreUsable();
// The current list, session-only entries included. Secrets are not in it.
std::vector<SavedSignIn> signIns();
// Adds or replaces the sign-in for value.host and value.port. It is written to the store when
// remember is set and the store is usable; otherwise (or when the store refuses it) it stays in
// memory only and is not remembered. Returns true when it was written to the store.
// An empty value.secret keeps the secret the host's existing entry has (Edit changing the other
// fields); with none to keep it throws std::runtime_error. Changing an entry from remembered to
// not remembered deletes it from the store. The engine's table is updated.
bool saveSignIn(const archive::SignIn& value, bool remember);
// Removes the entry for host and port from the list, the store and the engine's table.
void removeSignIn(const std::string& host, unsigned port);
// At startup: reads the list from settings.ini and, only when that list is not empty (reading
// can make the system ask to unlock the keyring), the secrets from the store, then gives the
// engine its table. Entries that do not parse are skipped with a log warning.
void loadSignIns();
}
