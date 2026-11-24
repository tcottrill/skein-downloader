#include "ui.h"
#include <wx/spinctrl.h>
#include "host_limit.h"
#include "util/sys_log.h"
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
    auto box = new wxBoxSizer(wxVERTICAL);
    box->Add(grid, 1, wxEXPAND | wxALL, dialog.FromDIP(12));
    box->Add(dialog.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    dialog.SetSizerAndFit(box);
    dialog.CentreOnParent();
    if (dialog.ShowModal() != wxID_OK) return;
    save("maxDownloads", wxString::Format("%d", downloads->GetSelection() + 1));
    save("hostConnections", wxString::Format("%d", connections->GetValue()));
    applySettings(queue);
}
}
