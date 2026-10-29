#pragma once
#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/fileconf.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/dir.h>
#include <wx/clipbrd.h>
#include <wx/notebook.h>
#include <wx/gauge.h>
#include <wx/imaglist.h>
#include <wx/toolbar.h>
#include <wx/weakref.h>
#include "logic.h"
#include "job_runner.h"
#include "video.h"
#include <future>
#include <set>
#include <fstream>
#include <map>

namespace lite {
inline wxString w(const std::string& s){return wxString::FromUTF8(s);}
inline std::string u(const wxString& s){return s.ToUTF8().data();}
extern archive::Path dataPath;
extern std::unique_ptr<wxFileConfig> config;
extern bool smoke;
inline wxString pref(const wxString& key,const wxString& fallback={}) {wxString v;config->Read(key,&v,fallback);return v;}
inline void save(const wxString& key,const wxString& value){config->Write(key,value);config->Flush();}
inline wxString downloadFolder(){
 auto dir=wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Downloads);
#ifndef _WIN32
 // Without user-dirs.dirs (as in a Flatpak sandbox) wx falls back to the home folder,
 // which the sandbox cannot write; ~/Downloads is the folder the package is granted.
 if(wxFileName::DirName(dir).SameAs(wxFileName::DirName(wxGetHomeDir())))dir=wxGetHomeDir()+"/Downloads";
#else
 dir.Replace("\\","/"); // Qt shows Windows paths with forward slashes
#endif
 return dir;
}
inline wxTextCtrl* field(wxWindow* parent,const wxString& value={},const wxString& hint={}) {
 auto c=new wxTextCtrl(parent,wxID_ANY,value,wxDefaultPosition,wxDefaultSize,wxTE_PROCESS_ENTER);c->SetHint(hint);return c;
}
inline wxButton* button(wxWindow* parent,wxSizer* row,const wxString& label,std::function<void()> action) {
 // Qt box layouts put 6 px between neighbouring items, none after the last one.
 auto b=new wxButton(parent,wxID_ANY,label);row->Add(b,0,(row->GetItemCount()?wxLEFT:0)|wxALIGN_CENTER_VERTICAL,parent->FromDIP(6));b->Bind(wxEVT_BUTTON,[action](wxCommandEvent&){action();});return b;
}
inline void folderBrowse(wxWindow* parent,wxTextCtrl* target){wxDirDialog d(parent,"Download folder",target->GetValue());if(d.ShowModal()==wxID_OK)target->SetValue(d.GetPath());}
inline void openFolder(const wxString& path){if(wxDirExists(path))wxLaunchDefaultApplication(path);}
inline bool ready(std::future<void>& f){return f.valid()&&f.wait_for(std::chrono::seconds(0))==std::future_status::ready;}
void showFiles(wxWindow*,downloads::Queue&,const wxString& link={});
void showVideo(wxWindow*,downloads::Queue&,const wxString& link={});
bool fileHost(const wxString&);
class ProgressBar;
class MainWindow:public wxFrame {
 downloads::Queue& queue;wxTextCtrl *url,*search;wxListCtrl* list;wxNotebook* tabs;int mode=0;wxStaticText *detail,*footer;wxPanel* panel;wxSizer* historyRow;
 wxButton *again,*clear;wxTimer timer;std::vector<downloads::Job> shown;std::future<void> closing;bool stopping=false,closeRequested=false;
 std::vector<ProgressBar*> bars;
 struct Sample{uint64_t bytes;std::chrono::steady_clock::time_point time;wxString rate=w("—");};std::map<uint64_t,Sample> samples;
 std::map<uint64_t,wxString> rates;wxString selectedDetail;long selectionAnchor=-1;std::map<uint64_t,std::string> loggedStates;std::string loggedError;
 void refresh();void route();void fitTabs();std::vector<uint64_t> selected()const;void close(wxCloseEvent&);void progressCells();void columns();void updateRates(const std::vector<downloads::Job>&);
public:MainWindow(downloads::Queue&);void openVideo(){showVideo(this,queue);}void activate(bool video=false);void smokeCheck();void logLayout();
};
}
