#include "ui.h"
#include <wx/menu.h>
#include "util/sys_log.h"
#include <unordered_set>
namespace lite {
namespace {
// The file list in virtual mode, like the main window's JobList: the control asks only for the
// rows it draws, so filtering, sorting and checking cost the same with 40,000 files as with 40.
// The check boxes come from the picker's own data too; the control keeps none.
class FileList : public wxListCtrl {
    std::function<wxString(long, long)> text;
    std::function<bool(long)> isChecked;
    mutable wxItemAttr even, odd;

public:
    FileList(wxWindow* parent, std::function<wxString(long, long)> cellText, std::function<bool(long)> checkedRow)
        : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_VIRTUAL), text(std::move(cellText)), isChecked(std::move(checkedRow)) {}

protected:
    wxString OnGetItemText(long item, long column) const override { return text(item, column); }
    int OnGetItemImage(long) const override { return -1; }
    bool OnGetItemIsChecked(long item) const override { return isChecked(item); }
    // Alternate rows: the window colour, then the button-face colour, as in the main window.
    wxItemAttr* OnGetItemAttr(long item) const override {
        auto& attr = item % 2 ? odd : even;
        attr.SetBackgroundColour(wxSystemSettings::GetColour(item % 2 ? wxSYS_COLOUR_BTNFACE : wxSYS_COLOUR_WINDOW));
        return &attr;
    }
};
}
class FilePicker : public wxDialog {
    downloads::Queue& queue;
    wxTextCtrl *url, *filter, *folder;
    wxListCtrl* list;
    wxChoice *servers, *connections;
    wxStaticText* status;
    // The controls that are locked while "Add to downloads" works (see lockForAdding) and the button that becomes Cancel.
    wxButton *loadButton, *selectAllButton, *clearButton, *checkButton, *uncheckButton, *testButton, *addButton;
    wxString addLabel;
    archive::Item item;
    std::vector<bool> checked;
    std::vector<size_t> rows;
    std::vector<std::string> routes;
    std::vector<archive::ServerSpeed> speeds;
    std::future<archive::Item> loading;
    std::future<std::vector<archive::ServerSpeed>> measuring;
    std::shared_ptr<std::atomic_bool> stop = std::make_shared<std::atomic_bool>(false);
    // The work of "Add to downloads" runs on a thread on copies of what it needs; it writes only these two values.
    std::future<std::vector<downloads::Job>> adding;
    std::shared_ptr<std::atomic_size_t> addDone = std::make_shared<std::atomic_size_t>(0);
    std::shared_ptr<std::atomic_bool> addStop = std::make_shared<std::atomic_bool>(false);
    size_t addTotal = 0;
    bool addLook = false; // the dialog shows the Cancel button, the count and the locked controls
    std::chrono::steady_clock::time_point addStarted;
    wxTimer timer;
    bool closing = false;
    int sortColumn = -1;
    bool ascending = true;
    friend void runBenchmark(MainWindow&, downloads::Queue&, size_t); // benchmark only
    bool busy() const { return loading.valid() || measuring.valid() || adding.valid(); }
    // Rows follow the clicked column header, as in Explorer: natural name order (file2 before file10), unknown sizes after known ones,
    // checked files first, ties by name. Until a header is clicked the files keep the order the source listed them in.
    void order(std::vector<size_t>& shown) const {
        if (sortColumn < 0) return;
        std::vector<wxString> names(item.files.size()), types(item.files.size());
        for (auto i : shown) {
            names[i] = w(item.files[i].name);
            types[i] = w(item.files[i].format);
        }
        auto key = [&](size_t a, size_t b) -> int {
            auto& x = item.files[a];
            auto& y = item.files[b];
            switch (sortColumn) {
            case 1: return x.sizeKnown != y.sizeKnown ? (x.sizeKnown ? -1 : 1) : x.size < y.size ? -1
                                                                         : x.size > y.size       ? 1
                                                                                                 : 0;
            case 2: return wxCmpNatural(types[a], types[b]);
            case 3: return int(checked[b]) - int(checked[a]);
            default: return wxCmpNatural(names[a], names[b]);
            }
        };
        std::stable_sort(shown.begin(), shown.end(), [&](size_t a, size_t b) {
            int c = key(a, b);
            if (!ascending) c = -c;
            if (!c) c = wxCmpNatural(names[a], names[b]);
            return c < 0;
        });
    }
    // A header click sorts by that column; clicking the same header again reverses it.
    void sortBy(int column) {
        if (column < 0 || column > 3) return;
        ascending = column == sortColumn ? !ascending : true;
        sortColumn = column;
        list->ShowSortIndicator(column, ascending);
        refresh();
    }
    void summary() {
        size_t n = 0;
        uint64_t bytes = 0;
        bool unknown = false;
        for (size_t i = 0; i < checked.size(); ++i)
            if (checked[i]) {
                ++n;
                bytes += item.files[i].size;
                unknown |= !item.files[i].sizeKnown;
            }
        status->SetLabel(wxString::Format(_(L"%zu selected / %zu files / "), n, item.files.size()) + w(archive::formatBytes(bytes)) + (unknown ? _(L" + unknown sizes") : wxString()));
    }
    // Runs on every keystroke in the link and folder boxes, so it saves only these small values;
    // the checked files are saved by saveSelection().
    void preferences() {
        save("folder", folder->GetValue());
        save("connections", wxString::Format("%d", connections->GetSelection()));
        if (!item.files.empty() && archive::isArchiveItem(item) && servers->GetSelection() >= 0) save("server", w(routes[size_t(servers->GetSelection())]));
    }
    // The checked files' names, restored when the same item is loaded again (see tick()).
    void saveSelection() {
        if (item.files.empty()) return;
        nlohmann::json names = nlohmann::json::array();
        for (size_t i = 0; i < checked.size(); ++i)
            if (checked[i]) names.push_back(item.files[i].name);
        save("fileItem", w(item.id));
        save("fileSelection", w(names.dump()));
    }
    // The list's cells, read by the control for the rows it draws.
    wxString cell(long row, long column) const {
        if (row < 0 || size_t(row) >= rows.size()) return {};
        auto i = rows[size_t(row)];
        auto& f = item.files[i];
        switch (column) {
        case 0: return w(f.name);
        case 1: return f.sizeKnown ? w(archive::formatBytes(f.size)) : _(L"Unknown");
        case 2: return w(f.format);
        case 3: return checked[i] ? _(L"Selected") : wxString();
        default: return {};
        }
    }
    bool rowChecked(long row) const { return row >= 0 && size_t(row) < rows.size() && checked[rows[size_t(row)]]; }
    void refresh() {
        // Keep highlighted rows across filtering and sorting: the control keeps them by row, so they
        // move to the rows the same files have now.
        std::vector<char> highlighted(item.files.size(), 0);
        bool anyHighlighted = false;
        for (long r = -1; (r = list->GetNextItem(r, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1;)
            if (size_t(r) < rows.size() && rows[size_t(r)] < highlighted.size()) highlighted[rows[size_t(r)]] = anyHighlighted = true;
        rows.clear();
        auto query = filter->GetValue().Lower();
        for (size_t i = 0; i < item.files.size(); ++i)
            if (query.empty() || w(item.files[i].name).Lower().Contains(query)) rows.push_back(i);
        order(rows);
        if (anyHighlighted) list->SetItemState(-1, 0, wxLIST_STATE_SELECTED);
        list->SetItemCount(long(rows.size()));
        if (anyHighlighted)
            for (size_t r = 0; r < rows.size(); ++r)
                if (highlighted[rows[r]]) list->SetItemState(long(r), wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        // Only the rows on screen are drawn again.
        list->Refresh();
        summary();
    }
    void select(bool value, bool highlighted) {
        if (highlighted) {
            for (long r = -1; (r = list->GetNextItem(r, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1;)
                if (size_t(r) < rows.size()) checked[rows[size_t(r)]] = value;
        } else
            for (auto i : rows) checked[i] = value;
        list->Refresh();
        summary();
    }
    // Space checks or unchecks the focused row and the highlighted rows with it. Handled here on both
    // platforms: in virtual mode neither the native list (wxMSW) nor wx's generic list (GTK) does it.
    void toggleFocused() {
        long focused = list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_FOCUSED);
        if (focused < 0) focused = list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (focused < 0 || size_t(focused) >= rows.size()) return;
        const bool value = !checked[rows[size_t(focused)]];
        checked[rows[size_t(focused)]] = value;
        if (list->GetItemState(focused, wxLIST_STATE_SELECTED) & wxLIST_STATE_SELECTED)
            select(value, true);
        else {
            list->RefreshItem(focused);
            summary();
        }
    }
    void refreshServers() {
        // Mirrors are listed by host name; speeds follow an em dash.
        wxString old = pref("server");
        servers->Clear();
        routes = {"", "standard"};
        servers->Append(_(L"Automatic (fastest sample)"));
        servers->Append(_(L"Standard server route"));
        for (auto& base : item.mirrorBases) {
            routes.push_back(base);
            auto start = base.find("://");
            start = start == std::string::npos ? 0 : start + 3;
            servers->Append(w(base.substr(start, base.find('/', start) - start)));
        }
        for (size_t i = 0; i < speeds.size() && i + 1 < routes.size(); ++i) servers->SetString(unsigned(i + 1), servers->GetString(unsigned(i + 1)) + w(" — ") + w(speeds[i].error.empty() ? archive::formatBytes(uint64_t(speeds[i].bytesPerSecond)) + "/s" : speeds[i].error));
        int chosen = 0;
        for (size_t i = 0; i < routes.size(); ++i)
            if (w(routes[i]) == old) chosen = int(i);
        servers->SetSelection(chosen);
    }
    void load() {
        if (busy()) return;
        auto link = u(url->GetValue().Strip(wxString::both));
        if (!archive::isHttpUrl(link) && !w(link).Lower().StartsWith("ftp://")) try {
                archive::parseLink(link);
            } catch (const std::exception& e) {
                LOG_WARN("File link rejected: %s", e.what());
                wxMessageBox(translated(e.what()), _(L"File downloads"), wxOK | wxICON_WARNING, this);
                return;
            }
        preferences();
        saveSelection();
        LOG_INFO("Loading files from %s", link.c_str());
        status->SetLabel(_(L"Loading files…"));
        auto flag = stop;
        loading = std::async(std::launch::async, [link, flag] {
            return archive::loadItem(link, *flag);
        });
    }
    void tick() {
        if (loading.valid() && loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) try {
                item = loading.get();
                checked.assign(item.files.size(), false);
                auto names = nlohmann::json::parse(u(pref("fileSelection", "[]")), nullptr, false);
                bool restore = item.selectedFile.empty() && pref("fileItem") == w(item.id) && names.is_array();
                std::unordered_set<std::string> saved;
                if (restore)
                    for (auto& name : names)
                        if (name.is_string()) saved.insert(name.get<std::string>());
                for (size_t i = 0; i < checked.size(); ++i) checked[i] = item.files[i].name == item.selectedFile || (restore ? saved.contains(item.files[i].name) : item.selectedFile.empty() && item.checkAll);
                speeds.clear();
                refreshServers();
                servers->Enable(archive::isArchiveItem(item));
                refresh();
            } catch (const std::exception& e) {
                LOG_ERROR("Loading files failed: %s", e.what());
                status->SetLabel(translated(e.what()));
            }
        if (measuring.valid() && measuring.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                speeds = measuring.get();
                refreshServers();
            } catch (...) {
                status->SetLabel(_(L"Speed test failed."));
            }
            summary();
        }
        if (adding.valid()) {
            if (adding.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                finishAdding();
            else {
                // Work that ends within about 150 ms never changes the dialog's look.
                if (!addLook && !closing && std::chrono::steady_clock::now() - addStarted >= std::chrono::milliseconds(150)) lockForAdding(true);
                if (addLook) status->SetLabel(wxString::Format(_(L"Checking files… %zu of %zu"), addDone->load(), addTotal));
            }
        }
        if (closing && !busy()) {
            timer.Stop();
            Destroy();
        }
    }
    // The selection and the other inputs must not change while the thread works, and the button becomes Cancel.
    void lockForAdding(bool on) {
        addLook = on;
        for (wxWindow* c : std::initializer_list<wxWindow*>{url, filter, list, loadButton, selectAllButton, clearButton, checkButton, uncheckButton, testButton}) c->Enable(!on);
        addButton->SetLabel(on ? _(L"Cancel") : addLabel);
        Layout();
    }
    // "Add to downloads" starts the thread; pressed again while it works (as Cancel) it stops it.
    void add() {
        if (adding.valid()) {
            *addStop = true;
            return;
        }
        if (busy() || std::find(checked.begin(), checked.end(), true) == checked.end()) return;
        static constexpr unsigned counts[] = {1, 2, 4, 8, 16};
        addTotal = size_t(std::count(checked.begin(), checked.end(), true));
        addDone->store(0);
        addStop = std::make_shared<std::atomic_bool>(false);
        addStarted = std::chrono::steady_clock::now();
        adding = std::async(std::launch::async, [item = item, checked = checked, target = archive::pathFromUtf8(u(folder->GetValue())), connections = counts[connections->GetSelection()], route = routes[size_t(servers->GetSelection())], done = addDone, halt = addStop] {
            return fileJobs(item, checked, target, connections, route, done.get(), halt.get());
        });
        // A small selection is finished before the dialog needs to look any different.
        if (adding.wait_for(std::chrono::milliseconds(25)) == std::future_status::ready) finishAdding();
    }
    // On the GUI thread, when the thread has returned (or the dialog is closing and the result is not wanted).
    void finishAdding() {
        std::vector<downloads::Job> jobs;
        std::string error;
        try {
            jobs = adding.get();
        } catch (const std::exception& e) {
            error = e.what();
        }
        const bool cancelled = *addStop || (error == "Paused");
        if (addLook) lockForAdding(false);
        if (closing) return;
        if (cancelled) {
            status->SetLabel(_(L"Cancelled. Nothing was added."));
            return;
        }
        if (error.empty()) try {
                auto n = queue.addBatch(std::move(jobs));
                preferences();
                saveSelection();
                status->SetLabel(wxString::Format(_(L"Added %zu downloads. Existing files and duplicate queue entries were kept."), n));
                return;
            } catch (const std::exception& e) {
                error = e.what();
            }
        LOG_ERROR("Cannot add files: %s", error.c_str());
        wxMessageBox(translated(error), _(L"Cannot add files"), wxOK | wxICON_WARNING, this);
    }
    void measure() {
        if (busy() || item.files.empty()) return;
        if (!archive::isArchiveItem(item)) {
            wxMessageBox(_(L"Speed tests compare archive.org servers. This source has a single route."), _(L"Server speeds"), wxOK | wxICON_INFORMATION, this);
            return;
        }
        long selected = list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        size_t index = item.files.size();
        if (selected >= 0)
            index = rows[size_t(selected)];
        else
            for (size_t i = 0; i < checked.size(); ++i)
                if (checked[i]) {
                    index = i;
                    break;
                }
        if (index == item.files.size()) {
            wxMessageBox(_(L"Select or check a file to measure its server speeds."), _(L"Server speeds"), wxOK | wxICON_WARNING, this);
            return;
        }
        if (!item.files[index].sizeKnown || !item.files[index].size) {
            wxMessageBox(_(L"Choose a non-empty file with a known size to test speeds."), _(L"Server speeds"), wxOK | wxICON_WARNING, this);
            return;
        }
        auto file = item.files[index];
        std::vector<std::string> urls = {"https://archive.org/download/" + archive::encode(item.id) + "/" + archive::encode(file.name, true)};
        for (auto& base : item.mirrorBases) urls.push_back(base + archive::encode(file.name, true));
        auto flag = stop;
        measuring = std::async(std::launch::async, [file, urls, flag] {
            return archive::measureServers(file, urls, *flag);
        });
        status->SetLabel(_(L"Testing server speeds… up to 256 KiB per source. No file is saved."));
    }

public:
    FilePicker(wxWindow* parent, downloads::Queue& q, const wxString& link, bool loadLink)
        : wxDialog(parent, wxID_ANY, _(L"File downloads"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), queue(q), timer(this) {
        appIcons(this);
        // Dialog layout: 11 px margins and 6 px between items and rows.
        const int gap = FromDIP(6);
        auto outer = new wxBoxSizer(wxVERTICAL);
        auto box = new wxBoxSizer(wxVERTICAL);
        outer->Add(box, 1, wxEXPAND | wxALL, FromDIP(11));
        auto title = new wxStaticText(this, wxID_ANY, _(L"File downloads"));
        auto font = title->GetFont();
        font.SetPointSize(21);
        title->SetFont(font);
        box->Add(title, 0, wxBOTTOM, gap);
        auto row = new wxBoxSizer(wxHORIZONTAL);
        url = field(this, link, _(L"Paste a file, folder, FTP, release, package, MediaFire, Google Drive, OneDrive, WeTransfer, archive.org or Wayback Machine link, or an archive.org item identifier"));
        row->Add(url, 1, wxALIGN_CENTER_VERTICAL);
        loadButton = button(this, row, _(L"Load files"), [this] {
            load();
        });
        box->Add(row, 0, wxEXPAND | wxBOTTOM, gap);
        row = new wxBoxSizer(wxHORIZONTAL);
        filter = field(this, {}, _(L"Filter by filename…"));
        row->Add(filter, 1, wxALIGN_CENTER_VERTICAL);
        selectAllButton = button(this, row, _(L"Select all"), [this] {
            select(true, false);
        });
        clearButton = button(this, row, _(L"Clear selection"), [this] {
            select(false, false);
        });
        box->Add(row, 0, wxEXPAND | wxBOTTOM, gap);
        list = new FileList(
                this,
                [this](long row, long column) {
                    return cell(row, column);
                },
                [this](long row) {
                    return rowChecked(row);
                });
        list->EnableCheckBoxes();
        list->InsertColumn(0, _(L"Name"));
        list->InsertColumn(1, _(L"Size"));
        list->InsertColumn(2, _(L"Type"));
        list->InsertColumn(3, _(L"Selection"));
        list->SetColumnWidth(0, FromDIP(500));
        for (int c = 1; c < 4; ++c) list->SetColumnWidth(c, FromDIP(100));
        box->Add(list, 1, wxEXPAND | wxBOTTOM, gap);
        row = new wxBoxSizer(wxHORIZONTAL);
        checkButton = button(this, row, _(L"Check highlighted"), [this] {
            select(true, true);
        });
        uncheckButton = button(this, row, _(L"Uncheck highlighted"), [this] {
            select(false, true);
        });
        box->Add(row, 0, wxBOTTOM, gap);
        status = new wxStaticText(this, wxID_ANY, "");
        box->Add(status, 0, wxEXPAND | wxBOTTOM, gap);
        row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(this, wxID_ANY, _(L"Save to")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
        folder = field(this, pref("folder", downloadFolder()));
        row->Add(folder, 1, wxALIGN_CENTER_VERTICAL);
        button(this, row, _(L"Browse…"), [this] {
            folderBrowse(this, folder);
        });
        box->Add(row, 0, wxEXPAND | wxBOTTOM, gap);
        row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(this, wxID_ANY, _(L"Server")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
        servers = new wxChoice(this, wxID_ANY);
        row->Add(servers, 1, wxALIGN_CENTER_VERTICAL);
        testButton = button(this, row, _(L"Test speeds"), [this] {
            measure();
        });
        box->Add(row, 0, wxEXPAND | wxBOTTOM, gap);
        refreshServers();
        row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(this, wxID_ANY, _(L"Connections per file")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
        connections = new wxChoice(this, wxID_ANY);
        for (auto n : {"1", "2", "4", "8", "16"}) connections->Append(n);
        long c = 2;
        pref("connections", "2").ToLong(&c);
        connections->SetSelection(std::clamp(int(c), 0, 4));
        row->Add(connections, 0, wxALIGN_CENTER_VERTICAL);
        button(this, row, _(L"Open folder"), [this] {
            auto path = archive::pathFromUtf8(u(folder->GetValue()));
            try {
                if (!item.files.empty() && item.sourceUrl.empty()) {
                    auto sub = archive::safePath(path, item.id);
                    if (std::filesystem::is_directory(sub)) path = sub;
                }
            } catch (...) {}
            openFolder(w(archive::pathUtf8(path)));
        });
        row->AddStretchSpacer();
        addLabel = _(L"Add to downloads");
        addButton = button(this, row, addLabel, [this] {
            add();
        });
        // wxID_CANCEL lets Escape close the dialog, as QDialog's reject does.
        auto closeButton = new wxButton(this, wxID_CANCEL, _(L"Close"));
        row->Add(closeButton, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, gap);
        closeButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            Close();
        });
        box->Add(row, 0, wxEXPAND);
        SetSizer(outer);
        SetClientSize(FromDIP(wxSize(920, 620)));
        SetMinClientSize(FromDIP(wxSize(560, 400)));
        CentreOnParent();
        list->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
            e.Skip();
            CallAfter([this] {
                int rest = 0;
                for (int c = 1; c < 4; ++c) rest += list->GetColumnWidth(c);
                list->SetColumnWidth(0, std::max(FromDIP(120), list->GetClientSize().x - rest - FromDIP(4)));
            });
        });
        filter->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            refresh();
        });
        url->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
            load();
        });
        url->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            preferences();
        });
        folder->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            preferences();
        });
        connections->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            preferences();
        });
        servers->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            preferences();
        });
        // A click on a check box: in virtual mode both wxMSW and the generic list report the new state
        // with these events and leave storing and drawing it to the program.
        auto check = [this](wxListEvent& e) {
            if (e.GetIndex() >= 0 && size_t(e.GetIndex()) < rows.size()) {
                checked[rows[size_t(e.GetIndex())]] = e.GetEventType() == wxEVT_LIST_ITEM_CHECKED;
                list->RefreshItem(e.GetIndex());
                summary();
            }
        };
        list->Bind(wxEVT_LIST_ITEM_CHECKED, check);
        list->Bind(wxEVT_LIST_ITEM_UNCHECKED, check);
        list->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
            if (e.GetKeyCode() == WXK_SPACE && !e.HasAnyModifiers())
                toggleFocused();
            else
                e.Skip();
        });
        list->Bind(wxEVT_LIST_COL_CLICK, [this](wxListEvent& e) {
            sortBy(e.GetColumn());
        });
        list->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, [this](wxListEvent& e) {
            if (e.GetIndex() >= 0 && !(list->GetItemState(e.GetIndex(), wxLIST_STATE_SELECTED) & wxLIST_STATE_SELECTED)) {
                long row = -1;
                while ((row = list->GetNextItem(row, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1) list->SetItemState(row, 0, wxLIST_STATE_SELECTED);
                list->SetItemState(e.GetIndex(), wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
            }
            wxMenu menu;
            menu.Append(1, _(L"Check highlighted"));
            menu.Append(2, _(L"Uncheck highlighted"));
            auto id = GetPopupMenuSelectionFromUser(menu);
            if (id == 1 || id == 2) select(id == 1, true);
        });
        // Closes at once; a lookup still running is stopped and the hidden dialog is destroyed when it returns.
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            tick();
        });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
            preferences();
            saveSelection();
            *stop = true;
            *addStop = true;
            if (busy() && e.CanVeto()) {
                closing = true;
                Hide();
                e.Veto();
            } else
                Destroy();
        });
        timer.Start(100);
        summary();
        url->SetFocus();
        if (loadLink && !link.empty()) CallAfter([this] {
            load();
        });
        if (smoke) {
            item.id = "smoke-files";
            auto sample = [](const char* name, const char* format, uint64_t size) {
                archive::File f;
                f.name = name;
                f.format = format;
                f.size = size;
                f.sizeKnown = true;
                return f;
            };
            item.files = {sample("one.zip", "zip", 42), sample("two.zip", "zip", 99), sample("notes.txt", "text", 7)};
            checked.assign(3, false);
            refresh();
            filter->SetValue(".zip");
            select(true, false);
            if (checked != std::vector<bool>({true, true, false})) throw std::runtime_error("Filtered selection failed");
            filter->Clear();
            if (list->GetItemCount() != 3 || !list->IsItemChecked(0) || list->IsItemChecked(2)) throw std::runtime_error("Checks lost after filtering");
            sortBy(1);
            sortBy(1);
            if (rows != std::vector<size_t>({1, 0, 2})) throw std::runtime_error("Sorting by size failed");
            sortBy(2);
            if (rows != std::vector<size_t>({2, 0, 1}) || list->IsItemChecked(0) || !list->IsItemChecked(1) || !list->IsItemChecked(2)) throw std::runtime_error("Sorting by type failed");
            folder->SetValue(w(archive::pathUtf8(dataPath / "downloads")));
            add();
            if (adding.valid()) { // normally finished within add() already
                adding.wait();
                tick();
            }
            if (queue.snapshot().size() < 3) throw std::runtime_error("File picker failed to queue selected files");
        }
    }
    ~FilePicker() override {
        *stop = true;
        *addStop = true;
        timer.Stop();
    }
};
void showFiles(wxWindow* p, downloads::Queue& q, const wxString& link, bool load) {
    (new FilePicker(p, q, link, load))->Show();
}
// Benchmark only (--bench-files, see app.cpp): times the file picker, the queue and the main window with a
// synthetic item of n files. Pending GUI events are pumped with wxYield() before and after each step, so the
// step's paint cost is part of its time. Each result line is written and flushed as soon as its step is done.
void runBenchmark(MainWindow& main, downloads::Queue& queue, size_t n) {
    using Clock = std::chrono::steady_clock;
    std::ofstream out(dataPath / "bench.txt", std::ios::trunc);
    auto report = [&out](const std::string& name, const std::string& value) {
        out << name << '\t' << value << std::endl;
        LOG_INFO("bench %s\t%s", name.c_str(), value.c_str());
    };
    auto millis = [](Clock::time_point from) {
        char text[32];
        snprintf(text, sizeof text, "%.3f", std::chrono::duration<double, std::milli>(Clock::now() - from).count());
        return std::string(text);
    };
    auto step = [&](const std::string& name, const std::function<void()>& action) {
        wxYield();
        auto start = Clock::now();
        action();
        wxYield();
        report(name, millis(start));
    };
    report("files", std::to_string(n));
    queue.pauseAll();
    main.benchTimer(false); // the window's own tick must not take the first refresh
    archive::Item item;
    item.id = "bench-item";
    item.title = "Benchmark item";
    item.mirrorBases = {"https://ia800001.us.archive.org/1/items/bench-item/", "https://ia800002.us.archive.org/2/items/bench-item/", "https://ia800003.us.archive.org/3/items/bench-item/"};
    for (size_t i = 1; i <= n; ++i) {
        archive::File f;
        char text[64];
        size_t k = i % 300; // 300 prefixes s00a..s99c; a 3-letter filter such as "s42" matches 3 of them, 1% of the files
        snprintf(text, sizeof text, "s%02zu%c_rom_%06zu.zip", k / 3, char('a' + k % 3), i);
        f.name = text;
        f.format = "ZIP";
        f.size = 1000 + (uint64_t(i) * 7919u) % 90000000u;
        f.sizeKnown = true;
        uint64_t a = uint64_t(i) * 0x9E3779B97F4A7C15ull + 1, b = (a ^ (a >> 29)) * 0xBF58476D1CE4E5B9ull;
        snprintf(text, sizeof text, "%016llx%016llx%08x", (unsigned long long)a, (unsigned long long)b, unsigned((a ^ b) >> 32));
        f.sha1 = text;
        item.files.push_back(std::move(f));
    }
    auto picker = new FilePicker(&main, queue, wxString(), false);
    picker->Show();
    picker->folder->SetValue(w(archive::pathUtf8(dataPath / "downloads")));
    picker->item = std::move(item);
    picker->checked.assign(n, false);
    picker->refreshServers();
    wxYield();
    step("picker_fill", [&] {
        picker->refresh();
    });
    step("picker_filter", [&] {
        picker->filter->SetValue("s42"); // the text event runs refresh()
    });
    report("filter_rows", std::to_string(picker->rows.size()));
    step("picker_filter_clear", [&] {
        picker->filter->Clear();
    });
    step("picker_sort", [&] {
        picker->sortBy(1);
    });
    step("picker_sort_name", [&] {
        picker->sortBy(0);
    });
    step("picker_select_all", [&] {
        picker->select(true, false);
    });
    step("picker_preferences", [&] {
        picker->preferences();
    });
    std::vector<downloads::Job> jobs;
    step("picker_add_jobs", [&] {
        jobs = fileJobs(picker->item, picker->checked, archive::pathFromUtf8(u(picker->folder->GetValue())), 2, picker->routes[size_t(picker->servers->GetSelection())]);
    });
    report("jobs_built", std::to_string(jobs.size()));
    step("picker_add_batch", [&] {
        queue.addBatch(std::move(jobs));
    });
    // No yield between the add and the first refresh: that refresh must see the new rows for the first time.
    {
        auto start = Clock::now();
        main.benchRefresh();
        wxYield();
        report("main_refresh_rebuild", millis(start));
    }
    {
        // The saver thread was woken by the add; this is how long it still takes until the file is written.
        auto start = Clock::now();
        queue.flush();
        report("queue_flush_after_add", millis(start));
    }
    {
        wxYield();
        auto start = Clock::now();
        for (int i = 0; i < 5; ++i) {
            main.benchRefresh();
            wxYield();
        }
        char text[32];
        snprintf(text, sizeof text, "%.3f", std::chrono::duration<double, std::milli>(Clock::now() - start).count() / 5);
        report("main_refresh_idle", text);
    }
    {
        auto start = Clock::now();
        size_t count = 0;
        for (int i = 0; i < 5; ++i) count += queue.snapshot().size();
        char text[32];
        snprintf(text, sizeof text, "%.3f", std::chrono::duration<double, std::milli>(Clock::now() - start).count() / 5);
        report("queue_snapshot", text);
        report("snapshot_jobs", std::to_string(count / 5));
    }
    {
        // pause() on a job that is already paused changes nothing but marks the history changed.
        auto id = queue.snapshot().front().id;
        auto start = Clock::now();
        queue.pause(id);
        queue.flush();
        report("queue_history_build_and_save", millis(start));
        std::error_code ec;
        report("history_bytes", std::to_string(std::filesystem::file_size(dataPath / "history.json", ec)));
    }
    {
        // Cancel: press Add, wait until the dialog shows the Cancel button, press it, and see that nothing was added.
        auto before = queue.snapshot().size();
        picker->add();
        auto deadline = Clock::now() + std::chrono::seconds(3);
        while (picker->adding.valid() && !picker->addLook && Clock::now() < deadline) {
            wxYield();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        report("picker_cancel_button_label", u(picker->addButton->GetLabel()));
        report("picker_cancel_list_enabled", picker->list->IsEnabled() ? "yes" : "no");
        picker->add(); // the Cancel button
        while (picker->adding.valid()) {
            wxYield();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        report("picker_cancel_status", u(picker->status->GetLabel()));
        report("picker_cancel_label_restored", u(picker->addButton->GetLabel()));
        report("picker_cancel_list_enabled_after", picker->list->IsEnabled() ? "yes" : "no");
        report("picker_cancel_queue_grew_by", std::to_string(queue.snapshot().size() - before));
    }
    {
        // The user's action end to end: pressing "Add to downloads" until the status shows the result, with the
        // event loop pumped the way the running program does it (a short sleep between rounds). A new folder makes
        // every job new. The longest block is the longest stretch the GUI thread did not return to the event loop:
        // the add() call itself (it copies the item and waits at most 25 ms) or one round of events (a timer tick).
        picker->folder->SetValue(w(archive::pathUtf8(dataPath / "downloads-ui")));
        wxYield();
        auto start = Clock::now();
        auto longest = std::chrono::duration<double, std::milli>::zero();
        auto lap = [&](Clock::time_point from) {
            longest = std::max(longest, std::chrono::duration<double, std::milli>(Clock::now() - from));
        };
        std::set<std::string> labels; // the status lines seen while it ran, numbers cut off
        picker->add();
        lap(start);
        report("picker_add_ui_call", millis(start));
        std::string lastCount;
        while (picker->adding.valid()) {
            auto round = Clock::now();
            wxYield();
            lap(round);
            auto text = u(picker->status->GetLabel());
            labels.insert(text.substr(0, text.find_first_of("0123456789")));
            if (text.find("Checking files") == 0) lastCount = text;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        report("picker_add_ui", millis(start));
        report("picker_add_ui_longest_block", [&] {
            char text[32];
            snprintf(text, sizeof text, "%.3f", longest.count());
            return std::string(text);
        }());
        report("picker_add_ui_last_count", lastCount);
        report("picker_add_ui_look_shown", labels.size() > 1 && lastCount.size() ? "yes" : "no");
        report("picker_add_ui_status", u(picker->status->GetLabel()));
    }
    picker->Close();
    main.benchTimer(true);
    wxYield();
}
}
