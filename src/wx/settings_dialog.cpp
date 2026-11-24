#include "ui.h"
#include <wx/spinctrl.h>
#include <wx/hyperlink.h>
#include <wx/statline.h>
#include "core.h"
#include "host_limit.h"
#include "signins_format.h"
#include "util/sys_log.h"
#include <algorithm>
#include <cstring>
namespace lite {
namespace {
constexpr long defaultDownloads = 4, maxDownloadsAtOnce = 4;
constexpr long defaultConnections = 16, maxConnections = 16;
// A stored decimal number in 1..maximum; anything else gives the default.
long stored(const wxString& key, long fallback, long maximum) {
    long value = 0;
    if (!pref(key).ToLong(&value) || value < 1 || value > maximum) return fallback;
    return value;
}

// What one site choice means in the sign-in dialog. The site names are proper names and stay as they are.
struct Site {
    const wchar_t* name;
    const char* host;
    archive::SignInKind kind;
    const char* link;
};
const Site sites[] = {
        {L"Hugging Face", "huggingface.co", archive::SignInKind::Token, "https://huggingface.co/settings/tokens"},
        {L"GitHub", "github.com", archive::SignInKind::Token, "https://github.com/settings/tokens"},
        {L"archive.org", "archive.org", archive::SignInKind::ArchiveKeys, "https://archive.org/account/s3.php"},
};
constexpr int anotherServer = 3;

// "host" or "host:port", with a pasted https://, http:// or ftp:// prefix and anything from the first "/" dropped.
// False when what is left is not a plausible host name.
bool parseServer(const wxString& text, std::string& host, unsigned& port) {
    std::string s = archive::trim(u(text));
    const std::string lowered = archive::lower(s);
    for (const char* prefix : {"https://", "http://", "ftp://"})
        if (lowered.rfind(prefix, 0) == 0) {
            s.erase(0, std::strlen(prefix));
            break;
        }
    if (auto slash = s.find('/'); slash != std::string::npos) s.erase(slash);
    port = 0;
    if (auto colon = s.rfind(':'); colon != std::string::npos) {
        const std::string digits = s.substr(colon + 1);
        if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos) return false;
        const unsigned long n = std::stoul(digits);
        if (n < 1 || n > 65535) return false;
        port = unsigned(n);
        s.erase(colon);
    }
    host = archive::lower(s);
    if (host.empty() || host.size() > 253) return false;
    size_t labelStart = 0;
    for (size_t i = 0; i <= host.size(); ++i) {
        if (i < host.size()) {
            const unsigned char c = host[i];
            if (c != '.' && c != '-' && c != '_' && !(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'z')) return false;
            if (c != '.') continue;
        }
        // No empty label, and none that starts or ends with "-".
        if (i == labelStart || host[labelStart] == '-' || host[i - 1] == '-') return false;
        labelStart = i + 1;
    }
    return true;
}

void scrub(std::string& s) {
    std::fill(s.begin(), s.end(), '\0');
    s.clear();
}

// The dialog that adds or edits one sign-in (editSignIn). What is typed as a secret is held only in the
// text control, cleared as soon as the dialog is done, and in the one SignIn it hands back.
class SignInDialog : public wxDialog {
    const bool hostFixed, editing, storeOk;
    archive::SignIn initial;  // its secret is scrubbed once it is in the text box
    const std::string scheme; // "ftp", "http" or "https" when the sign-in is for a link

    wxChoice *site, *kind;
    wxTextCtrl *server, *user, *secret;
    wxStaticText *serverLabel, *kindLabel, *linkLabel, *userLabel, *secretLabel;
    wxHyperlinkCtrl* link;
    wxCheckBox *remember, *unencrypted;
    wxStaticText* unencryptedNote;
    wxFlexGridSizer* grid;

    wxStaticText* row(const wxString& label, wxWindow* control) {
        auto text = new wxStaticText(this, wxID_ANY, label);
        grid->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxBOTTOM, FromDIP(8));
        grid->Add(control, 1, wxEXPAND | wxBOTTOM, FromDIP(8));
        return text;
    }
    // Hidden rows take no room and leave no gap: every cell carries its own bottom margin.
    void showRow(wxWindow* label, wxWindow* control, bool shown) {
        grid->Show(label, shown);
        grid->Show(control, shown);
    }
    archive::SignInKind currentKind() const {
        const int s = site->GetSelection();
        if (s >= 0 && s < anotherServer) return sites[s].kind;
        return kind->GetSelection() == 1 ? archive::SignInKind::Token : archive::SignInKind::Password;
    }
    void update() {
        const int s = site->GetSelection();
        const auto k = currentKind();
        showRow(serverLabel, server, s == anotherServer);
        showRow(kindLabel, kind, s == anotherServer);
        showRow(linkLabel, link, s >= 0 && s < anotherServer);
        if (s >= 0 && s < anotherServer) {
            link->SetURL(w(sites[s].link));
            link->SetLabel(w(sites[s].link));
        }
        showRow(userLabel, user, k != archive::SignInKind::Token);
        userLabel->SetLabel(k == archive::SignInKind::ArchiveKeys ? _(L"Access key") : _(L"User name"));
        secretLabel->SetLabel(k == archive::SignInKind::ArchiveKeys ? _(L"Secret key") : k == archive::SignInKind::Token ? _(L"Access token")
                                                                                                                         : _(L"Password"));
        const bool password = k == archive::SignInKind::Password;
        unencrypted->Show(password);
        unencryptedNote->Show(password);
        Layout();
        GetSizer()->Fit(this);
    }
    void problem(const wxString& message, wxWindow* focus) {
        wxMessageBox(message, _(L"Sign-in"), wxOK | wxICON_WARNING, this);
        focus->SetFocus();
    }
    void onOk() {
        const int s = site->GetSelection();
        archive::SignIn r;
        r.kind = currentKind();
        if (s >= 0 && s < anotherServer) {
            r.host = sites[s].host;
        } else if (!parseServer(server->GetValue(), r.host, r.port)) {
            problem(_(L"Enter a server name such as example.org."), server);
            return;
        }
        if (r.kind != archive::SignInKind::Token) {
            r.user = u(user->GetValue().Strip(wxString::both));
            if (r.user.empty()) {
                problem(r.kind == archive::SignInKind::ArchiveKeys ? _(L"Enter the access key.") : _(L"Enter the user name."), user);
                return;
            }
        }
        r.secret = u(secret->GetValue());
        // An empty secret keeps the saved one, but only for the entry being edited, unchanged in host and kind.
        const bool keeps = editing && r.host == initial.host && r.port == initial.port && r.kind == initial.kind;
        if (r.secret.empty() && !keeps) {
            problem(r.kind == archive::SignInKind::ArchiveKeys ? _(L"Enter the secret key.") : r.kind == archive::SignInKind::Token ? _(L"Enter the access token.")
                                                                                                                                    : _(L"Enter the password."),
                    secret);
            return;
        }
        r.allowUnencrypted = r.kind == archive::SignInKind::Password && unencrypted->GetValue();
        if (!scheme.empty()) {
            const auto use = signInUsableFor(scheme, r.host, r);
            if (use == LinkUse::NeverUnencrypted) {
                problem(_(L"A token cannot be sent over an unencrypted link. Use an https link."), secret);
                return;
            }
            if (use == LinkUse::NeedsUnencryptedTick) {
                problem(scheme == "ftp" ? _(L"FTP always sends the password unencrypted. Tick the box to allow it for this server, or cancel.") : _(L"This link is not encrypted. Tick the box to allow sending the password to this server, or cancel."), unencrypted);
                return;
            }
        }
        scrub(result.secret);
        result = std::move(r);
        EndModal(wxID_OK);
    }
    static bool listed(const archive::SignIn& v) {
        if (v.host.empty()) return false;
        for (const auto& e : signIns())
            if (e.value.host == v.host && e.value.port == v.port) return true;
        return false;
    }

public:
    archive::SignIn result;
    SignInDialog(wxWindow* parent, const archive::SignIn& value, bool rememberIt, bool fixed, const wxString& notice, const std::string& linkScheme)
        : wxDialog(parent, wxID_ANY, _(L"Sign-in"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE), hostFixed(fixed), editing(listed(value)), storeOk(signInStoreUsable()), initial(value), scheme(linkScheme) {
        appIcons(this);
        grid = new wxFlexGridSizer(2, 0, FromDIP(12));
        grid->AddGrowableCol(1);
        const wxSize fieldSize = FromDIP(wxSize(300, -1));
        site = new wxChoice(this, wxID_ANY);
        for (const auto& st : sites) site->Append(wxString(st.name));
        site->Append(_(L"Another server"));
        row(_(L"Site"), site);
        server = new wxTextCtrl(this, wxID_ANY);
        server->SetHint(wxT("example.org"));
        server->SetMinSize(fieldSize);
        serverLabel = row(_(L"Server"), server);
        kind = new wxChoice(this, wxID_ANY);
        kind->Append(_(L"User name and password"));
        kind->Append(_(L"Access token"));
        kind->SetSelection(0);
        kindLabel = row(_(L"Sign in with"), kind);
        link = new wxHyperlinkCtrl(this, wxID_ANY, "https://archive.org", "https://archive.org");
        linkLabel = row(wxEmptyString, link);
        user = new wxTextCtrl(this, wxID_ANY);
        userLabel = row(_(L"User name"), user);
        secret = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
        secretLabel = row(_(L"Password"), secret);

        int choice = anotherServer;
        for (int i = 0; i < anotherServer; ++i)
            if (value.host == sites[i].host && value.kind == sites[i].kind && value.port == 0) choice = i;
        site->SetSelection(choice);
        if (choice == anotherServer) {
            wxString text = w(value.host);
            if (value.port) text += wxString::Format(":%u", value.port);
            server->SetValue(text);
            kind->SetSelection(value.kind == archive::SignInKind::Token ? 1 : 0);
        }
        if (value.kind != archive::SignInKind::Token) user->SetValue(w(value.user));
        // A secret handed in (a password that came with an FTP link) is offered in the box, to be kept or changed.
        if (!value.secret.empty()) secret->SetValue(w(value.secret));
        scrub(initial.secret);
        if (editing) secret->SetHint(_(L"Leave empty to keep the saved one"));
        if (hostFixed) {
            site->Enable(false);
            server->Enable(false);
        }

        remember = new wxCheckBox(this, wxID_ANY, _(L"Remember on this computer"));
        remember->SetValue(rememberIt && storeOk);
        remember->Enable(storeOk);
        unencrypted = new wxCheckBox(this, wxID_ANY, _(L"Allow on an unencrypted connection (http or ftp)"));
        unencrypted->SetValue(value.allowUnencrypted);
        unencryptedNote = new wxStaticText(this, wxID_ANY, _(L"The password can be read by anyone on the network path."));
        unencryptedNote->Wrap(FromDIP(340));

        auto box = new wxBoxSizer(wxVERTICAL);
        if (!notice.empty()) {
            auto why = new wxStaticText(this, wxID_ANY, notice);
            why->Wrap(FromDIP(380));
            box->Add(why, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        }
        box->Add(grid, 0, wxEXPAND | wxALL, FromDIP(12));
        if (!storeOk) {
            auto note = new wxStaticText(this, wxID_ANY, _(L"No credential store is available here: sign-ins are kept only until Skein closes."));
            note->Wrap(FromDIP(380));
            box->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        }
        box->Add(remember, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        box->Add(unencrypted, 0, wxLEFT | wxRIGHT, FromDIP(12));
        box->Add(unencryptedNote, 0, wxLEFT, FromDIP(12) + FromDIP(24));
        box->AddSpacer(FromDIP(12));
        box->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        SetSizer(box);
        site->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            update();
        });
        kind->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            update();
        });
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            onOk();
        },
             wxID_OK);
        update();
        CentreOnParent();
        if (choice == anotherServer && !hostFixed)
            server->SetFocus();
        else if (currentKind() != archive::SignInKind::Token && user->GetValue().empty())
            user->SetFocus();
        else
            secret->SetFocus();
    }
    ~SignInDialog() override {
        wipe();
        scrub(initial.secret);
        scrub(result.secret);
    }
    // Clears the typed secret from the control (called before the dialog goes away).
    void wipe() { secret->ChangeValue(wxEmptyString); }
    bool remembered() const { return remember->GetValue(); }
};

wxString kindText(const SavedSignIn& s) {
    wxString text = s.value.kind == archive::SignInKind::Token ? _(L"Access token") : s.value.kind == archive::SignInKind::ArchiveKeys ? _(L"Access keys")
                                                                                                                                       : _(L"Password");
    if (!s.available)
        text += L" · " + _(L"not available");
    else if (!s.remembered)
        text += L" · " + _(L"until Skein closes");
    return text;
}
wxString hostText(const archive::SignIn& s) {
    return s.port ? w(s.host) + wxString::Format(":%u", s.port) : w(s.host);
}
}
bool editSignIn(wxWindow* parent, archive::SignIn& value, bool& remember, bool hostFixed, const wxString& notice, const std::string& linkScheme) {
    SignInDialog dialog(parent, value, remember, hostFixed, notice, linkScheme);
    const bool ok = dialog.ShowModal() == wxID_OK;
    dialog.wipe();
    if (ok) {
        value = std::move(dialog.result);
        remember = dialog.remembered();
    }
    return ok;
}
void applySettings(downloads::Queue& queue) {
    const auto jobs = stored("maxDownloads", defaultDownloads, maxDownloadsAtOnce);
    const auto connections = stored("hostConnections", defaultConnections, maxConnections);
    queue.setLimit(unsigned(jobs));
    archive::setHostLimit(unsigned(connections));
    LOG_INFO("Settings applied: %ld downloads at once, %ld connections per server", jobs, connections);
}
void showSettings(wxWindow* parent, downloads::Queue& queue) {
    wxDialog dialog(parent, wxID_ANY, _(L"Settings"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE);
    appIcons(&dialog);
    auto downloadsLabel = new wxStaticText(&dialog, wxID_ANY, _(L"Downloads at once"));
    auto downloads = new wxChoice(&dialog, wxID_ANY);
    for (int i = 1; i <= maxDownloadsAtOnce; ++i) downloads->Append(wxString::Format("%d", i));
    downloads->SetSelection(int(stored("maxDownloads", defaultDownloads, maxDownloadsAtOnce)) - 1);
    auto downloadsNote = new wxStaticText(&dialog, wxID_ANY, _(L"How many downloads run at the same time."));
    auto connectionsLabel = new wxStaticText(&dialog, wxID_ANY, _(L"Connections per server"));
    auto connections = new wxSpinCtrl(&dialog, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, int(maxConnections), int(stored("hostConnections", defaultConnections, maxConnections)));
    auto connectionsNote = new wxStaticText(&dialog, wxID_ANY, _(L"The maximum number of connections to one server, shared by all downloads."));
    auto grid = new wxFlexGridSizer(2, dialog.FromDIP(wxSize(12, 4)));
    grid->AddGrowableCol(1);
    grid->Add(downloadsLabel, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(downloads, 0, wxALIGN_CENTER_VERTICAL);
    grid->AddSpacer(0);
    grid->Add(downloadsNote, 0, wxBOTTOM, dialog.FromDIP(12));
    grid->Add(connectionsLabel, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(connections, 0, wxALIGN_CENTER_VERTICAL);
    grid->AddSpacer(0);
    grid->Add(connectionsNote);

    // The sign-ins: saved as soon as the add/edit dialog is confirmed or Remove is pressed, not by OK.
    auto heading = new wxStaticText(&dialog, wxID_ANY, _(L"Sign-ins"));
    auto font = heading->GetFont();
    font.MakeBold();
    heading->SetFont(font);
    auto list = new wxListCtrl(&dialog, wxID_ANY, wxDefaultPosition, dialog.FromDIP(wxSize(460, 130)), wxLC_REPORT | wxLC_SINGLE_SEL);
    list->AppendColumn(_(L"Server"), wxLIST_FORMAT_LEFT, dialog.FromDIP(200));
    list->AppendColumn(_(L"Kind"), wxLIST_FORMAT_LEFT, dialog.FromDIP(240));
    auto buttons = new wxBoxSizer(wxVERTICAL);
    auto addButton = new wxButton(&dialog, wxID_ANY, _(L"Add…"));
    auto editButton = new wxButton(&dialog, wxID_ANY, _(L"Edit…"));
    auto removeButton = new wxButton(&dialog, wxID_ANY, _(L"Remove"));
    buttons->Add(addButton, 0, wxBOTTOM, dialog.FromDIP(6));
    buttons->Add(editButton, 0, wxBOTTOM, dialog.FromDIP(6));
    buttons->Add(removeButton);
    auto listRow = new wxBoxSizer(wxHORIZONTAL);
    listRow->Add(list, 1, wxEXPAND | wxRIGHT, dialog.FromDIP(8));
    listRow->Add(buttons);

    auto rows = std::make_shared<std::vector<SavedSignIn>>();
    auto selectedRow = [&]() -> long {
        return list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    };
    auto enable = [&] {
        const bool any = selectedRow() >= 0;
        editButton->Enable(any);
        removeButton->Enable(any);
    };
    auto refresh = [&] {
        *rows = signIns();
        list->DeleteAllItems();
        for (size_t i = 0; i < rows->size(); ++i) {
            const long at = list->InsertItem(long(i), hostText((*rows)[i].value));
            list->SetItem(at, 1, kindText((*rows)[i]));
        }
        enable();
    };
    auto store = [&](const archive::SignIn& value, bool remember) {
        try {
            const bool wrote = saveSignIn(value, remember);
            if (remember && !wrote) wxMessageBox(_(L"The sign-in could not be saved to the credential store. It will be kept until Skein closes."), _(L"Sign-ins"), wxOK | wxICON_WARNING, &dialog);
        } catch (const std::exception& e) {
            LOG_ERROR("A sign-in was not saved: %s", e.what());
            wxMessageBox(w(e.what()), _(L"Sign-ins"), wxOK | wxICON_WARNING, &dialog);
        }
        refresh();
    };
    auto edit = [&] {
        const long at = selectedRow();
        if (at < 0 || size_t(at) >= rows->size()) return;
        archive::SignIn value = (*rows)[at].value;
        bool remember = (*rows)[at].remembered;
        if (editSignIn(&dialog, value, remember, true)) store(value, remember);
        scrub(value.secret);
    };
    addButton->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        archive::SignIn value;
        bool remember = true;
        if (editSignIn(&dialog, value, remember, false)) store(value, remember);
        scrub(value.secret);
    });
    editButton->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        edit();
    });
    removeButton->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        const long at = selectedRow();
        if (at < 0 || size_t(at) >= rows->size()) return;
        const auto host = (*rows)[at].value.host;
        const auto port = (*rows)[at].value.port;
        if (wxMessageBox(wxString::Format(_(L"Remove the sign-in for %s?"), hostText((*rows)[at].value)), _(L"Remove"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, &dialog) != wxYES) return;
        removeSignIn(host, port);
        refresh();
    });
    list->Bind(wxEVT_LIST_ITEM_SELECTED, [&](wxListEvent&) {
        enable();
    });
    list->Bind(wxEVT_LIST_ITEM_DESELECTED, [&](wxListEvent&) {
        enable();
    });
    list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [&](wxListEvent&) {
        edit();
    });
    refresh();

    auto box = new wxBoxSizer(wxVERTICAL);
    box->Add(grid, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
    box->Add(new wxStaticLine(&dialog), 0, wxEXPAND | wxLEFT | wxRIGHT, dialog.FromDIP(12));
    box->Add(heading, 0, wxLEFT | wxRIGHT | wxTOP, dialog.FromDIP(12));
    if (!signInStoreUsable()) {
        auto note = new wxStaticText(&dialog, wxID_ANY, _(L"No credential store is available here: sign-ins are kept only until Skein closes."));
        note->Wrap(dialog.FromDIP(460));
        box->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, dialog.FromDIP(6));
    }
    box->Add(listRow, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, dialog.FromDIP(8));
    auto saved = new wxStaticText(&dialog, wxID_ANY, _(L"Changes to sign-ins are saved as soon as you make them. OK and Cancel apply to the two settings above."));
    saved->Wrap(dialog.FromDIP(520));
    box->Add(saved, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
    box->Add(dialog.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    dialog.SetSizerAndFit(box);
    dialog.CentreOnParent();
    if (dialog.ShowModal() != wxID_OK) return;
    save("maxDownloads", wxString::Format("%d", downloads->GetSelection() + 1));
    save("hostConnections", wxString::Format("%d", connections->GetValue()));
    applySettings(queue);
}
}
