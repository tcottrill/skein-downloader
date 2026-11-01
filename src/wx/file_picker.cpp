#include "ui.h"
#include <wx/menu.h>
#include "util/sys_log.h"
namespace lite {
class FilePicker:public wxDialog {
 downloads::Queue& queue;wxTextCtrl *url,*filter,*folder;wxListCtrl* list;wxChoice *servers,*connections;wxStaticText* status;
 archive::Item item;std::vector<bool> checked;std::vector<size_t> rows;std::vector<std::string> routes;std::vector<archive::ServerSpeed> speeds;
 std::future<archive::Item> loading;std::future<std::vector<archive::ServerSpeed>> measuring;
 std::shared_ptr<std::atomic_bool> stop=std::make_shared<std::atomic_bool>(false);wxTimer timer;bool closing=false,rebuilding=false;
 bool busy()const{return loading.valid()||measuring.valid();}
 void summary(){size_t n=0;uint64_t bytes=0;bool unknown=false;for(size_t i=0;i<checked.size();++i)if(checked[i]){++n;bytes+=item.files[i].size;unknown|=!item.files[i].sizeKnown;}status->SetLabel(wxString::Format(_(L"%zu selected / %zu files / "),n,item.files.size())+w(archive::formatBytes(bytes))+(unknown?_(L" + unknown sizes"):wxString()));}
 void preferences(){save("folder",folder->GetValue());save("connections",wxString::Format("%d",connections->GetSelection()));if(!item.files.empty()&&archive::isArchiveItem(item)&&servers->GetSelection()>=0)save("server",w(routes[size_t(servers->GetSelection())]));if(!item.files.empty()){nlohmann::json names=nlohmann::json::array();for(size_t i=0;i<checked.size();++i)if(checked[i])names.push_back(item.files[i].name);save("fileItem",w(item.id));save("fileSelection",w(names.dump()));}}
 void refresh(){
  // Keep highlighted rows across filtering, as Qt's filter proxy does.
  std::set<size_t> highlighted;for(long r=-1;(r=list->GetNextItem(r,wxLIST_NEXT_ALL,wxLIST_STATE_SELECTED))!=-1;)if(size_t(r)<rows.size())highlighted.insert(rows[size_t(r)]);
  rebuilding=true;list->Freeze();list->DeleteAllItems();rows.clear();auto query=filter->GetValue().Lower();
  for(size_t i=0;i<item.files.size();++i){auto& f=item.files[i];if(!w(f.name).Lower().Contains(query))continue;long r=list->InsertItem(list->GetItemCount(),w(f.name));list->SetItem(r,1,f.sizeKnown?w(archive::formatBytes(f.size)):_(L"Unknown"));list->SetItem(r,2,w(f.format));list->SetItem(r,3,checked[i]?_(L"Selected"):wxString());list->SetItemBackgroundColour(r,r%2?wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE):wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));rows.push_back(i);list->CheckItem(r,checked[i]);if(highlighted.contains(i))list->SetItemState(r,wxLIST_STATE_SELECTED,wxLIST_STATE_SELECTED);}
  list->Thaw();rebuilding=false;summary();
 }
 void select(bool value,bool highlighted){rebuilding=true;for(size_t i=0;i<rows.size();++i)if(!highlighted||(list->GetItemState(long(i),wxLIST_STATE_SELECTED)&wxLIST_STATE_SELECTED)) {checked[rows[i]]=value;list->CheckItem(long(i),value);list->SetItem(long(i),3,value?_(L"Selected"):wxString());}rebuilding=false;summary();}
 void refreshServers(){
  // Mirrors are listed by host name; speeds follow an em dash, as in Qt.
  wxString old=pref("server");servers->Clear();routes={"","standard"};servers->Append(_(L"Automatic (fastest sample)"));servers->Append(_(L"Standard server route"));for(auto& base:item.mirrorBases){routes.push_back(base);auto start=base.find("://");start=start==std::string::npos?0:start+3;servers->Append(w(base.substr(start,base.find('/',start)-start)));}
  for(size_t i=0;i<speeds.size()&&i+1<routes.size();++i)servers->SetString(unsigned(i+1),servers->GetString(unsigned(i+1))+w(" — ")+w(speeds[i].error.empty()?archive::formatBytes(uint64_t(speeds[i].bytesPerSecond))+"/s":speeds[i].error));
  int chosen=0;for(size_t i=0;i<routes.size();++i)if(w(routes[i])==old)chosen=int(i);servers->SetSelection(chosen);
 }
 void load(){
  if(busy())return;auto link=u(url->GetValue().Strip(wxString::both));
  if(!archive::isHttpUrl(link))try{archive::parseLink(link);}catch(const std::exception& e){LOG_WARN("File link rejected: %s",e.what());wxMessageBox(translated(e.what()),_(L"File downloads"),wxOK|wxICON_WARNING,this);return;}
  preferences();LOG_INFO("Loading files from %s",link.c_str());status->SetLabel(_(L"Loading files…"));auto flag=stop;loading=std::async(std::launch::async,[link,flag]{return archive::loadItem(link,*flag);});
 }
 void tick(){
  if(loading.valid()&&loading.wait_for(std::chrono::seconds(0))==std::future_status::ready)try{
   item=loading.get();checked.assign(item.files.size(),false);auto names=nlohmann::json::parse(u(pref("fileSelection","[]")),nullptr,false);bool restore=item.selectedFile.empty()&&pref("fileItem")==w(item.id)&&names.is_array();for(size_t i=0;i<checked.size();++i)checked[i]=item.files[i].name==item.selectedFile||(restore?std::find(names.begin(),names.end(),item.files[i].name)!=names.end():item.selectedFile.empty()&&item.checkAll);speeds.clear();refreshServers();servers->Enable(archive::isArchiveItem(item));refresh();
  }catch(const std::exception& e){LOG_ERROR("Loading files failed: %s",e.what());status->SetLabel(translated(e.what()));}
  if(measuring.valid()&&measuring.wait_for(std::chrono::seconds(0))==std::future_status::ready){try{speeds=measuring.get();refreshServers();}catch(...){status->SetLabel(_(L"Speed test failed."));}summary();}
  if(closing&&!busy()){timer.Stop();Destroy();}
 }
 void add(){if(busy()||std::find(checked.begin(),checked.end(),true)==checked.end())return;try{static constexpr unsigned counts[]={1,2,4,8,16};auto jobs=fileJobs(item,checked,archive::pathFromUtf8(u(folder->GetValue())),counts[connections->GetSelection()],routes[size_t(servers->GetSelection())]);auto n=queue.addBatch(std::move(jobs));preferences();status->SetLabel(wxString::Format(_(L"Added %zu downloads. Existing files and duplicate queue entries were kept."),n));}catch(const std::exception& e){LOG_ERROR("Cannot add files: %s",e.what());wxMessageBox(translated(e.what()),_(L"Cannot add files"),wxOK|wxICON_WARNING,this);}}
 void measure(){
  if(busy()||item.files.empty())return;
  if(!archive::isArchiveItem(item)){wxMessageBox(_(L"Speed tests compare archive.org servers. This source has a single route."),_(L"Server speeds"),wxOK|wxICON_INFORMATION,this);return;}
  long selected=list->GetNextItem(-1,wxLIST_NEXT_ALL,wxLIST_STATE_SELECTED);size_t index=item.files.size();if(selected>=0)index=rows[size_t(selected)];else for(size_t i=0;i<checked.size();++i)if(checked[i]){index=i;break;}
  if(index==item.files.size()){wxMessageBox(_(L"Select or check a file to measure its server speeds."),_(L"Server speeds"),wxOK|wxICON_WARNING,this);return;}
  if(!item.files[index].sizeKnown||!item.files[index].size){wxMessageBox(_(L"Choose a non-empty file with a known size to test speeds."),_(L"Server speeds"),wxOK|wxICON_WARNING,this);return;}
  auto file=item.files[index];std::vector<std::string> urls={"https://archive.org/download/"+archive::encode(item.id)+"/"+archive::encode(file.name,true)};for(auto& base:item.mirrorBases)urls.push_back(base+archive::encode(file.name,true));auto flag=stop;
  measuring=std::async(std::launch::async,[file,urls,flag]{return archive::measureServers(file,urls,*flag);});status->SetLabel(_(L"Testing server speeds… up to 256 KiB per source. No file is saved."));
 }
public:
 FilePicker(wxWindow* parent,downloads::Queue& q,const wxString& link,bool loadLink):wxDialog(parent,wxID_ANY,_(L"File downloads"),wxDefaultPosition,wxDefaultSize,wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER),queue(q),timer(this){
  appIcons(this);
  // Qt's default dialog layout: 11 px margins and 6 px between items and rows.
  const int gap=FromDIP(6);auto outer=new wxBoxSizer(wxVERTICAL);auto box=new wxBoxSizer(wxVERTICAL);outer->Add(box,1,wxEXPAND|wxALL,FromDIP(11));
  auto title=new wxStaticText(this,wxID_ANY,_(L"File downloads"));auto font=title->GetFont();font.SetPointSize(21);title->SetFont(font);box->Add(title,0,wxBOTTOM,gap);auto row=new wxBoxSizer(wxHORIZONTAL);url=field(this,link,_(L"Paste a file, release, package, MediaFire, Google Drive, OneDrive, WeTransfer or archive.org link, or an archive.org item identifier"));row->Add(url,1,wxALIGN_CENTER_VERTICAL);button(this,row,_(L"Load files"),[this]{load();});box->Add(row,0,wxEXPAND|wxBOTTOM,gap);
  row=new wxBoxSizer(wxHORIZONTAL);filter=field(this,{},_(L"Filter by filename…"));row->Add(filter,1,wxALIGN_CENTER_VERTICAL);button(this,row,_(L"Select all"),[this]{select(true,false);});button(this,row,_(L"Clear selection"),[this]{select(false,false);});box->Add(row,0,wxEXPAND|wxBOTTOM,gap);
  list=new wxListCtrl(this,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxLC_REPORT);list->EnableCheckBoxes();list->InsertColumn(0,_(L"Name"));list->InsertColumn(1,_(L"Size"));list->InsertColumn(2,_(L"Type"));list->InsertColumn(3,_(L"Selection"));list->SetColumnWidth(0,FromDIP(500));for(int c=1;c<4;++c)list->SetColumnWidth(c,FromDIP(100));box->Add(list,1,wxEXPAND|wxBOTTOM,gap);
  row=new wxBoxSizer(wxHORIZONTAL);button(this,row,_(L"Check highlighted"),[this]{select(true,true);});button(this,row,_(L"Uncheck highlighted"),[this]{select(false,true);});box->Add(row,0,wxBOTTOM,gap);
  status=new wxStaticText(this,wxID_ANY,"");box->Add(status,0,wxEXPAND|wxBOTTOM,gap);
  row=new wxBoxSizer(wxHORIZONTAL);row->Add(new wxStaticText(this,wxID_ANY,_(L"Save to")),0,wxALIGN_CENTER_VERTICAL|wxRIGHT,gap);folder=field(this,pref("folder",downloadFolder()));row->Add(folder,1,wxALIGN_CENTER_VERTICAL);button(this,row,_(L"Browse…"),[this]{folderBrowse(this,folder);});box->Add(row,0,wxEXPAND|wxBOTTOM,gap);
  row=new wxBoxSizer(wxHORIZONTAL);row->Add(new wxStaticText(this,wxID_ANY,_(L"Server")),0,wxALIGN_CENTER_VERTICAL|wxRIGHT,gap);servers=new wxChoice(this,wxID_ANY);row->Add(servers,1,wxALIGN_CENTER_VERTICAL);button(this,row,_(L"Test speeds"),[this]{measure();});box->Add(row,0,wxEXPAND|wxBOTTOM,gap);refreshServers();
  row=new wxBoxSizer(wxHORIZONTAL);row->Add(new wxStaticText(this,wxID_ANY,_(L"Connections per file")),0,wxALIGN_CENTER_VERTICAL|wxRIGHT,gap);connections=new wxChoice(this,wxID_ANY);for(auto n:{"1","2","4","8","16"})connections->Append(n);long c=2;pref("connections","2").ToLong(&c);connections->SetSelection(std::clamp(int(c),0,4));row->Add(connections,0,wxALIGN_CENTER_VERTICAL);button(this,row,_(L"Open folder"),[this]{auto path=archive::pathFromUtf8(u(folder->GetValue()));try{if(!item.files.empty()&&item.sourceUrl.empty()){auto sub=archive::safePath(path,item.id);if(std::filesystem::is_directory(sub))path=sub;}}catch(...){}openFolder(w(archive::pathUtf8(path)));});row->AddStretchSpacer();button(this,row,_(L"Add to downloads"),[this]{add();});
  // wxID_CANCEL lets Escape close the dialog, as QDialog's reject does.
  auto closeButton=new wxButton(this,wxID_CANCEL,_(L"Close"));row->Add(closeButton,0,wxLEFT|wxALIGN_CENTER_VERTICAL,gap);closeButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){Close();});box->Add(row,0,wxEXPAND);
  SetSizer(outer);SetClientSize(FromDIP(wxSize(920,620)));SetMinClientSize(FromDIP(wxSize(560,400)));CentreOnParent();list->Bind(wxEVT_SIZE,[this](wxSizeEvent& e){e.Skip();CallAfter([this]{int rest=0;for(int c=1;c<4;++c)rest+=list->GetColumnWidth(c);list->SetColumnWidth(0,std::max(FromDIP(120),list->GetClientSize().x-rest-FromDIP(4)));});});
  filter->Bind(wxEVT_TEXT,[this](wxCommandEvent&){refresh();});url->Bind(wxEVT_TEXT_ENTER,[this](wxCommandEvent&){load();});
  url->Bind(wxEVT_TEXT,[this](wxCommandEvent&){preferences();});folder->Bind(wxEVT_TEXT,[this](wxCommandEvent&){preferences();});connections->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){preferences();});servers->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){preferences();});
  auto check=[this](wxListEvent& e){if(!rebuilding&&e.GetIndex()>=0&&size_t(e.GetIndex())<rows.size()){checked[rows[size_t(e.GetIndex())]]=list->IsItemChecked(e.GetIndex());list->SetItem(e.GetIndex(),3,list->IsItemChecked(e.GetIndex())?_(L"Selected"):wxString());summary();}};list->Bind(wxEVT_LIST_ITEM_CHECKED,check);list->Bind(wxEVT_LIST_ITEM_UNCHECKED,check);
  list->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK,[this](wxListEvent& e){if(e.GetIndex()>=0&&!(list->GetItemState(e.GetIndex(),wxLIST_STATE_SELECTED)&wxLIST_STATE_SELECTED)){long row=-1;while((row=list->GetNextItem(row,wxLIST_NEXT_ALL,wxLIST_STATE_SELECTED))!=-1)list->SetItemState(row,0,wxLIST_STATE_SELECTED);list->SetItemState(e.GetIndex(),wxLIST_STATE_SELECTED,wxLIST_STATE_SELECTED);}wxMenu menu;menu.Append(1,_(L"Check highlighted"));menu.Append(2,_(L"Uncheck highlighted"));auto id=GetPopupMenuSelectionFromUser(menu);if(id==1||id==2)select(id==1,true);});
  // Qt closes at once; a lookup still running is stopped and the hidden dialog is destroyed when it returns.
  Bind(wxEVT_TIMER,[this](wxTimerEvent&){tick();});Bind(wxEVT_CLOSE_WINDOW,[this](wxCloseEvent& e){preferences();*stop=true;if(busy()&&e.CanVeto()){closing=true;Hide();e.Veto();}else Destroy();});
  timer.Start(100);summary();url->SetFocus();if(loadLink&&!link.empty())CallAfter([this]{load();});
  if(smoke){
   item.id="smoke-files";item.files={{"one.zip","zip","",42,true},{"two.zip","zip","",99,true},{"notes.txt","text","",7,true}};checked.assign(3,false);refresh();
   filter->SetValue(".zip");select(true,false);if(checked!=std::vector<bool>({true,true,false}))throw std::runtime_error("Filtered selection failed");
   filter->Clear();if(list->GetItemCount()!=3||!list->IsItemChecked(0)||list->IsItemChecked(2))throw std::runtime_error("Checks lost after filtering");
   folder->SetValue(w(archive::pathUtf8(dataPath/"downloads")));add();if(queue.snapshot().size()<3)throw std::runtime_error("File picker failed to queue selected files");
  }
 }
 ~FilePicker()override{*stop=true;timer.Stop();}
};
void showFiles(wxWindow* p,downloads::Queue& q,const wxString& link,bool load){(new FilePicker(p,q,link,load))->Show();}
}
