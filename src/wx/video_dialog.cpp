#include "ui.h"
#include <mutex>
#include "util/sys_log.h"
namespace lite {
class VideoDialog:public wxDialog {
 downloads::Queue& queue;wxTextCtrl *url,*folder,*log;wxChoice *quality,*browser;wxRadioButton *mp4,*mkv;wxCheckBox *mp3,*impersonate,*cookies;wxStaticText *status,*tip,*toolsLabel,*accessTip;wxButton *addButton,*install,*update,*cancel,*browse,*open;wxGauge* progress;wxTimer timer;
 struct Work {std::atomic_bool stop=false;std::mutex mutex;std::vector<std::string> lines;std::string completion;};
 std::shared_ptr<Work> work=std::make_shared<Work>();std::future<void> task;bool closing=false;
 // The browser list shows "Firefox" but settings and yt-dlp use "firefox", as in Qt.
 std::string browserName(){return u(browser->GetStringSelection().Lower());}
 void prefs(){save("videoUrl",url->GetValue());save("videoFolder",folder->GetValue());save("videoQuality",wxString::Format("%d",quality->GetSelection()));save("videoFormat",mp4->GetValue()?"0":"1");save("videoMp3",mp3->GetValue()?"1":"0");save("videoImpersonate",impersonate->GetValue()?"1":"0");save("videoBrowser",w(browserName()));}
 // Qt's word-wrapped labels span the dialog between its 24 px side margins.
 void wrap(wxStaticText* label,const wxString& text){label->SetLabel(text);label->Wrap(std::max(1,GetClientSize().x-FromDIP(48)));}
 void controls(){bool busy=task.valid(),audio=quality->GetSelection()==7;auto tools=video::toolsFolder(video::executableFolder());bool toolsReady=video::toolsReady(tools);url->Enable(!busy);folder->Enable(!busy);quality->Enable(!busy);mp4->Enable(!busy&&!audio);mkv->Enable(!busy&&!audio);mp3->Enable(!busy&&audio);impersonate->Enable(!busy);cookies->Enable(!busy);browser->Enable(!busy&&cookies->GetValue());addButton->Enable(!busy&&toolsReady);cancel->Enable(busy&&!work->stop);browse->Enable(!busy);open->Enable(!folder->GetValue().empty());
#ifdef _WIN32
  install->Enable(!busy);update->Enable(!busy&&toolsReady);
#else
  install->Disable();update->Disable();
#endif
  wrap(tip,audio?(mp3->GetValue()?"Best available audio, saved as MP3 using the highest VBR quality setting.\nConversion cannot improve the source audio quality.":"Best available audio, kept in its original audio format.\nChoose MP3 for a widely compatible audio file."):mp4->GetValue()?"MP4: H.264 video + AAC audio for iPhone playback. No incompatible codec fallback.\nYour resolution limit still applies; unavailable compatible streams will report an error.":"MKV: best available source codecs. Your resolution limit still applies.\nSeparate video and audio are merged; a combined source may keep its original container.");
  wrap(toolsLabel,toolsReady?"Video tools are ready. Update yt-dlp if a site stops working.":
#ifdef _WIN32
  "Install video tools once to enable downloads (about 250 MiB). No admin access needed."
#else
  "Video tools are provided by this app package. Update or reinstall the app to repair them."
#endif
  );wrap(accessTip,
#ifdef _WIN32
  "Cookies give this site your browser's sign-in for it. Use only with sites you trust. Firefox works best: Chrome and Edge on Windows often block other apps from reading their cookies. Close the browser before downloading."
#else
  "Cookies give this site your browser's sign-in for it. Use only with sites you trust. The Flatpak cannot see browser profiles until you allow it read access to them, for example ~/.mozilla with Flatseal."
#endif
  );accessTip->Show(cookies->GetValue());Layout();
 }
 void add(){if(task.valid())return;try{auto link=url->GetValue().Strip(wxString::both);auto dest=archive::pathFromUtf8(u(folder->GetValue()));int q=quality->GetSelection();bool m3=q==7&&mp3->GetValue(),m4=q!=7&&mp4->GetValue();auto tools=video::toolsFolder(video::executableFolder());video::downloadArgs(tools,link.ToStdWstring(),dest,q,m3,m4);
  if(equivalentVideo(queue.snapshot(),dest,u(link),q,m3,m4)){prefs();status->SetLabel("Already in downloads. Resume it from the main window.");return;}
  if(!video::toolsReady(tools))throw std::runtime_error("Install the video tools first.");
  nlohmann::json input={{"url",u(link)},{"folder",archive::pathUtf8(dest)},{"quality",q},{"mp3",m3},{"mp4",m4}};if(impersonate->GetValue())input["impersonate"]=true;if(cookies->GetValue()){auto name=browserName();video::accessArgs(false,name);input["cookiesFromBrowser"]=name;}
  auto key=downloads::pathKey(dest)+"|"+u(link)+"|"+std::to_string(q)+(m3?"|mp3":"")+(m4?"|mp4":"");auto suffix=m3?", MP3":q==7?"":m4?", MP4":", MKV";bool added=queue.add(u(link)+" ["+archive::utf8(video::qualityNames()[q])+suffix+"]","Video",key,input);prefs();status->SetLabel(added?"Added to downloads.":"Already in downloads. Resume it from the main window.");if(added)Hide();
 }catch(const std::exception& e){LOG_WARN("Video not added: %s",e.what());wxMessageBox(w(e.what()),"Video downloads",wxOK|wxICON_INFORMATION,this);}}
 void tools(bool repair){
  if(task.valid())return;
#ifdef _WIN32
  auto root=video::executableFolder();archive::Path exe;std::vector<std::wstring> args;
  if(repair){wchar_t sys[32768];auto n=GetSystemDirectoryW(sys,32768);if(!n||n>=32768){status->SetLabel("Cannot locate Windows PowerShell.");return;}exe=archive::Path(sys)/L"WindowsPowerShell/v1.0/powershell.exe";args={L"-NoProfile",L"-NonInteractive",L"-ExecutionPolicy",L"Bypass",L"-File",(root/L"install-video-tools.ps1").wstring()};}
  else {if(!video::toolsReady(video::toolsFolder(root))){status->SetLabel("Install the video tools first.");return;}exe=video::toolExecutable(video::toolsFolder(root),"yt-dlp");args={L"--ignore-config",L"--no-plugin-dirs",L"-U"};}
  if(!queue.beginMaintenance("Video")){status->SetLabel("Pause video downloads in the main window before updating tools.");return;}
  prefs();work->stop=false;auto state=work;auto q=&queue;
  try {task=std::async(std::launch::async,[state,q,exe,args,root,repair]{struct End{downloads::Queue* q;~End(){q->endMaintenance();}}end{q};auto append=[state](const std::string& s){
     // Installer and updater output also goes to the log file; progress lines would only add noise.
     if(!s.starts_with("PROGRESS ")){if(s.find("ERROR")!=std::string::npos)LOG_ERROR("Video tools: %s",s.c_str());else if(s.find("WARNING")!=std::string::npos)LOG_WARN("Video tools: %s",s.c_str());else LOG_INFO("Video tools: %s",s.c_str());}
     std::lock_guard lock(state->mutex);if(state->lines.size()<1500)state->lines.push_back(s);};
    LOG_INFO("Video tools: %s",repair?"installing":"updating yt-dlp");std::string completion;try{int code=video::run(exe,args,state->stop,append);completion=state->stop?(repair?"Operation cancelled. You can retry.":"Stopped. Use the same URL, quality and folder to resume."):code?"Could not finish (exit "+std::to_string(code)+"). See the log above.":repair?"Video tools ready.":"yt-dlp update finished.";if(!code&&!state->stop){auto installed=video::toolsFolder(root);video::run(video::toolExecutable(installed,"yt-dlp"),{L"--ignore-config",L"--no-plugin-dirs",L"--version"},state->stop,[append](const std::string& line){append("yt-dlp version: "+line);});}}catch(const std::exception& e){completion=e.what();append(completion);}catch(...){completion="Unexpected error while managing video tools.";}LOG_INFO("Video tools: %s",completion.c_str());{std::lock_guard lock(state->mutex);state->completion=completion;}});}catch(const std::exception& e){queue.endMaintenance();LOG_ERROR("Could not start video tools: %s",e.what());status->SetLabel("Could not start video tools: "+w(e.what()));return;}
  log->Clear();progress->SetValue(0);status->SetLabel(repair?wxS("Installing verified video tools…"):wxS("Checking for a yt-dlp update…"));controls();
#else
  (void)repair;status->SetLabel("Video tools are bundled with this app. Update or reinstall the package to repair them.");
#endif
 }
 void tick(){std::vector<std::string> lines;{std::lock_guard lock(work->mutex);lines.swap(work->lines);}for(auto& line:lines){log->AppendText(w(line)+"\n");if(line.starts_with("PROGRESS ")){try{progress->SetValue(std::clamp(int(std::stod(line.substr(9))),0,100));}catch(...){}}}if(log->GetNumberOfLines()>1500)log->Remove(0,log->XYToPosition(0,500));if(ready(task)){task.get();{std::lock_guard lock(work->mutex);status->SetLabel(w(work->completion));}work->stop=false;controls();if(closing){closing=false;Hide();}}}
public:
 VideoDialog(wxWindow* parent,downloads::Queue& q,const wxString& link):wxDialog(parent,wxID_ANY,"Video downloads",wxDefaultPosition,wxDefaultSize,wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER),queue(q),timer(this){
  appIcons(this);
  // Qt: margins 24/20, 12 px between items, form spacing 14 x 12, 6 px inside rows.
  const int gap=FromDIP(12),inner=FromDIP(6);auto outer=new wxBoxSizer(wxVERTICAL);auto box=new wxBoxSizer(wxVERTICAL);outer->Add(box,1,wxEXPAND|wxLEFT|wxRIGHT,FromDIP(24));outer->Insert(0,0,FromDIP(20));outer->AddSpacer(FromDIP(20));
  auto heading=new wxStaticText(this,wxID_ANY,"Video downloads");auto font=heading->GetFont();font.SetPointSize(font.GetPointSize()+8);font.SetWeight(wxFONTWEIGHT_BOLD);heading->SetFont(font);box->Add(heading,0,wxBOTTOM,gap);toolsLabel=new wxStaticText(this,wxID_ANY,"");box->Add(toolsLabel,0,wxEXPAND|wxBOTTOM,gap);auto form=new wxFlexGridSizer(2,FromDIP(12),FromDIP(14));form->AddGrowableCol(1);auto label=[&](const char* s){form->Add(new wxStaticText(this,wxID_ANY,s),0,wxALIGN_CENTER_VERTICAL);};
  label("Video URL");url=field(this,link.empty()?pref("videoUrl"):link,"Paste a YouTube or other supported video URL");form->Add(url,1,wxEXPAND);
  label("Quality");quality=new wxChoice(this,wxID_ANY);for(auto& s:video::qualityNames())quality->Append(wxString(s));long val=0;pref("videoQuality","0").ToLong(&val);quality->SetSelection(std::clamp(int(val),0,7));auto qualityRow=new wxBoxSizer(wxHORIZONTAL);qualityRow->Add(quality,1,wxRIGHT,inner);mp3=new wxCheckBox(this,wxID_ANY,"Save as MP3 (best quality)");mp3->SetValue(pref("videoMp3")=="1");qualityRow->Add(mp3,0,wxALIGN_CENTER_VERTICAL);form->Add(qualityRow,1,wxEXPAND);
  label("Video format");auto formats=new wxBoxSizer(wxHORIZONTAL);mp4=new wxRadioButton(this,wxID_ANY,"MP4 (iPhone compatible)",wxDefaultPosition,wxDefaultSize,wxRB_GROUP);mkv=new wxRadioButton(this,wxID_ANY,"MKV (original formats)");mp4->SetValue(pref("videoFormat","0")!="1");mkv->SetValue(!mp4->GetValue());formats->Add(mp4,0,wxRIGHT,inner);formats->Add(mkv);form->Add(formats,1,wxEXPAND);
  label("Site access");auto row=new wxBoxSizer(wxHORIZONTAL);impersonate=new wxCheckBox(this,wxID_ANY,"Try as browser");impersonate->SetToolTip("Sends requests with a real Chrome network fingerprint. Helps with Cloudflare and other \"checking your browser\" pages.");impersonate->SetValue(pref("videoImpersonate")=="1");row->Add(impersonate,0,wxRIGHT|wxALIGN_CENTER_VERTICAL,FromDIP(16)+inner);cookies=new wxCheckBox(this,wxID_ANY,"Use cookies from");cookies->SetToolTip("Lets yt-dlp read this site's cookies from your browser, for age checks or other gates. It acts with your browser's sign-in.");row->Add(cookies,0,wxRIGHT|wxALIGN_CENTER_VERTICAL,inner);browser=new wxChoice(this,wxID_ANY);for(auto& s:video::cookieBrowsers()){auto name=w(s);browser->Append(name.Left(1).Upper()+name.Mid(1));}auto saved=pref("videoBrowser","firefox").Lower();browser->SetSelection(0);for(unsigned i=0;i<browser->GetCount();++i)if(browser->GetString(i).Lower()==saved)browser->SetSelection(int(i));row->Add(browser);form->Add(row,1,wxEXPAND);
  label("Save to");row=new wxBoxSizer(wxHORIZONTAL);folder=field(this,pref("videoFolder",downloadFolder()+"/Videos"));row->Add(folder,1,wxALIGN_CENTER_VERTICAL);browse=button(this,row,wxS("Browse…"),[this]{folderBrowse(this,folder);});form->Add(row,1,wxEXPAND);box->Add(form,0,wxEXPAND|wxBOTTOM,gap);
  tip=new wxStaticText(this,wxID_ANY,"");box->Add(tip,0,wxEXPAND|wxBOTTOM,gap);
  accessTip=new wxStaticText(this,wxID_ANY,"");box->Add(accessTip,0,wxEXPAND|wxBOTTOM,gap);
  row=new wxBoxSizer(wxHORIZONTAL);install=button(this,row,"Install / repair tools",[this]{tools(true);});update=button(this,row,"Update yt-dlp",[this]{tools(false);});open=button(this,row,"Open folder",[this]{openFolder(folder->GetValue());});box->Add(row,0,wxBOTTOM,gap);
  log=new wxTextCtrl(this,wxID_ANY,"",wxDefaultPosition,wxDefaultSize,wxTE_MULTILINE|wxTE_READONLY);box->Add(log,1,wxEXPAND|wxBOTTOM,gap);
  progress=new wxGauge(this,wxID_ANY,100);box->Add(progress,0,wxEXPAND|wxBOTTOM,gap);status=new wxStaticText(this,wxID_ANY,"Paste a link, choose a quality, then add it to downloads.",wxDefaultPosition,wxDefaultSize,wxST_NO_AUTORESIZE);status->SetMinSize(wxSize(-1,2*GetCharHeight()));
  row=new wxBoxSizer(wxHORIZONTAL);row->Add(status,1,wxALIGN_CENTER_VERTICAL);cancel=button(this,row,"Stop",[this]{work->stop=true;cancel->Disable();status->SetLabel(wxS("Stopping video helpers… partial downloads will be kept."));});addButton=button(this,row,"Add to downloads",[this]{add();});addButton->SetDefault();box->Add(row,0,wxEXPAND);
  SetSizer(outer);SetClientSize(FromDIP(wxSize(880,680)));SetMinClientSize(FromDIP(wxSize(760,560)));CentreOnParent();
  quality->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){controls();prefs();});mp4->Bind(wxEVT_RADIOBUTTON,[this](wxCommandEvent&){controls();prefs();});mkv->Bind(wxEVT_RADIOBUTTON,[this](wxCommandEvent&){controls();prefs();});mp3->Bind(wxEVT_CHECKBOX,[this](wxCommandEvent&){controls();prefs();});cookies->Bind(wxEVT_CHECKBOX,[this](wxCommandEvent&){controls();});
  url->Bind(wxEVT_TEXT,[this](wxCommandEvent&){prefs();});folder->Bind(wxEVT_TEXT,[this](wxCommandEvent&){prefs();controls();});browser->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){prefs();});impersonate->Bind(wxEVT_CHECKBOX,[this](wxCommandEvent&){prefs();});
  // Enter in the text fields presses the default button; Escape closes, as in a QDialog.
  url->Bind(wxEVT_TEXT_ENTER,[this](wxCommandEvent&){add();});folder->Bind(wxEVT_TEXT_ENTER,[this](wxCommandEvent&){add();});
  Bind(wxEVT_CHAR_HOOK,[this](wxKeyEvent& e){if(e.GetKeyCode()==WXK_ESCAPE)Close();else e.Skip();});
  Bind(wxEVT_SIZE,[this](wxSizeEvent& e){e.Skip();CallAfter([this]{controls();});});
  Bind(wxEVT_TIMER,[this](wxTimerEvent&){tick();});Bind(wxEVT_CLOSE_WINDOW,[this](wxCloseEvent& e){prefs();if(task.valid()&&e.CanVeto()){closing=true;work->stop=true;cancel->Disable();status->SetLabel(wxS("Stopping video helpers before closing…"));e.Veto();}else if(e.CanVeto()){e.Veto();Hide();}else{work->stop=true;Destroy();}});timer.Start(100);controls();
  // Like a Qt layout, never let the dialog shrink below its contents.
  auto least=GetSizer()->GetMinSize();SetMinClientSize(wxSize(std::max(FromDIP(760),least.x),std::max(FromDIP(560),least.y)));
  if(smoke){quality->SetSelection(7);controls();if(!mp3->IsEnabled()||mp4->IsEnabled())throw std::runtime_error("Audio options mismatch");quality->SetSelection(0);controls();if(mp3->IsEnabled()||!mp4->IsEnabled()||cookies->GetValue())throw std::runtime_error("Video options mismatch");if(browser->GetStringSelection()!="Firefox"||browserName()!="firefox")throw std::runtime_error("Browser names differ from Qt");}
 }
 ~VideoDialog()override{work->stop=true;timer.Stop();if(task.valid())task.wait();}
 void setLink(const wxString& link){if(task.valid()){status->SetLabel("Video tools are busy. Wait for them to finish, then load the new link.");return;}if(!link.empty())url->SetValue(link);}
};
void showVideo(wxWindow* p,downloads::Queue& q,const wxString& link){static wxWeakRef<VideoDialog> dialog;if(!dialog)dialog=new VideoDialog(p,q,link);else dialog->setLink(link);dialog->Show();dialog->Raise();}
}
