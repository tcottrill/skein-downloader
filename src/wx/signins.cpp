#include "ui.h"
#include "signins.h"
#include "signins_format.h"
#include "util/sys_log.h"
#include <wx/secretstore.h>
#include <algorithm>
#include <stdexcept>

namespace lite {
namespace {
// The table: the list the window shows with the secrets the engine needs, GUI thread only.
struct Entry {
    SavedSignIn row;
    std::string secret; // empty when unavailable
};
std::vector<Entry> table;

// Overwrites a string's contents (the store's wipe where it is there, a plain overwrite otherwise).
void wipe(std::string& s) {
    if (!s.empty()) wxSecretValue::Wipe(s.size(), &s[0]);
    s.clear();
}

#if wxUSE_SECRETSTORE
// A store that is not usable never becomes usable while the program runs, and asking again
// could make the system prompt again.
bool storeUsable() {
    static const bool usable = [] {
        wxString why;
        const bool ok = wxSecretStore::GetDefault().IsOk(&why);
        if (!ok) LOG_WARN("Credential store not usable, sign-ins are kept until Skein closes: %s", u(why).c_str());
        return ok;
    }();
    return usable;
}
// The store logs its own failures through wxLog, which would open a message box; ours go to the log.
bool storeSave(const archive::SignIn& s) {
    wxLogNull quiet;
    wxSecretValue value(s.secret.size(), s.secret.data());
    return wxSecretStore::GetDefault().Save(w(signInService(s.host, s.port)), w(storedUser(s)), value);
}
bool storeLoad(const archive::SignIn& s, std::string& secret) {
    wxLogNull quiet;
    wxString user;
    wxSecretValue value;
    if (!wxSecretStore::GetDefault().Load(w(signInService(s.host, s.port)), user, value) || !value.IsOk()) return false;
    secret.assign(static_cast<const char*>(value.GetData()), value.GetSize());
    return true;
}
void storeDelete(const archive::SignIn& s) {
    wxLogNull quiet;
    wxSecretStore::GetDefault().Delete(w(signInService(s.host, s.port)));
}
#else
bool storeUsable() {
    return false;
}
bool storeSave(const archive::SignIn&) {
    return false;
}
bool storeLoad(const archive::SignIn&, std::string&) {
    return false;
}
void storeDelete(const archive::SignIn&) {
}
#endif

std::vector<Entry>::iterator find(const std::string& host, unsigned port) {
    return std::find_if(table.begin(), table.end(), [&](const Entry& e) {
        return e.row.value.host == host && e.row.value.port == port;
    });
}

// Writes the list of remembered entries to settings.ini (unless it was only just read from there)
// and gives the engine the usable ones.
void publish(bool persist = true) {
    std::vector<archive::SignIn> listed, forEngine;
    for (const auto& e : table) {
        if (e.row.remembered) listed.push_back(e.row.value);
        if (e.row.available) {
            auto s = e.row.value;
            s.secret = e.secret;
            forEngine.push_back(std::move(s));
        }
    }
    if (persist && config) save("signIns", w(signInsToJson(listed)));
    archive::setSignIns(std::move(forEngine));
}
}

bool signInStoreUsable() {
    return storeUsable();
}

std::vector<SavedSignIn> signIns() {
    std::vector<SavedSignIn> rows;
    for (const auto& e : table) {
        rows.push_back(e.row);
        rows.back().value.secret.clear();
    }
    return rows;
}

bool saveSignIn(const archive::SignIn& value, bool remember) {
    auto existing = find(value.host, value.port);
    Entry entry;
    entry.row.value = value;
    entry.row.value.secret.clear();
    entry.secret = value.secret;
    if (entry.secret.empty()) {
        if (existing == table.end() || existing->secret.empty()) throw std::runtime_error("A sign-in needs a secret");
        entry.secret = existing->secret;
    }
    const bool wasRemembered = existing != table.end() && existing->row.remembered;
    const auto oldUser = wasRemembered ? storedUser(existing->row.value) : std::string();
    bool written = false;
    if (remember && storeUsable()) {
        archive::SignIn toStore = entry.row.value;
        toStore.secret = entry.secret;
        // The store keeps one user per service; a changed user name would leave the old item behind.
        if (wasRemembered && oldUser != storedUser(toStore)) storeDelete(toStore);
        written = storeSave(toStore);
        wipe(toStore.secret);
        if (!written) LOG_WARN("The credential store refused the sign-in for %s, keeping it until Skein closes", signInService(value.host, value.port).c_str());
    } else if (wasRemembered) {
        storeDelete(entry.row.value);
    }
    entry.row.remembered = written;
    entry.row.available = true;
    if (existing == table.end()) {
        table.push_back(std::move(entry));
    } else {
        wipe(existing->secret);
        *existing = std::move(entry);
    }
    publish();
    return written;
}

void removeSignIn(const std::string& host, unsigned port) {
    auto it = find(host, port);
    if (it == table.end()) return;
    if (it->row.remembered && storeUsable()) storeDelete(it->row.value);
    wipe(it->secret);
    table.erase(it);
    publish();
}

void loadSignIns() {
    for (auto& e : table) wipe(e.secret);
    table.clear();
    auto parsed = signInsFromJson(u(pref("signIns")));
    if (parsed.skipped) LOG_WARN("%zu saved sign-in(s) in settings.ini could not be read and were skipped", parsed.skipped);
    // Only now may the store be touched: an empty list never asks the system to unlock anything.
    const bool usable = !parsed.entries.empty() && storeUsable();
    for (auto& s : parsed.entries) {
        if (find(s.host, s.port) != table.end()) continue;
        Entry entry;
        entry.row.value = s;
        entry.row.remembered = true;
        entry.row.available = usable && storeLoad(s, entry.secret) && !entry.secret.empty();
        if (!entry.row.available) {
            wipe(entry.secret);
            LOG_WARN("The saved sign-in for %s is not available (its secret could not be read)", signInService(s.host, s.port).c_str());
        }
        table.push_back(std::move(entry));
    }
    publish(false);
}
}
