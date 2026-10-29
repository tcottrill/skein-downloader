#include "ui.h"
#include "sites.h"
#include <wx/uri.h>
#include <wx/display.h>
#include <wx/renderer.h>
#include <wx/dcbuffer.h>
#include <wx/statline.h>
#ifdef __WXMSW__
#include <wx/msw/wrapcctl.h>
#elif defined(__WXGTK__)
#include <gtk/gtk.h>
#endif
#include <regex>
#include "util/sys_log.h"
namespace lite {
// Qt's ProgressDelegate: a native progress bar with its text centred on the bar.
class ProgressBar:public wxWindow {
 int percent=-1;wxString text;
public:
 explicit ProgressBar(wxWindow* parent):wxWindow(parent,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxBORDER_NONE|wxFULL_REPAINT_ON_RESIZE){
  SetBackgroundStyle(wxBG_STYLE_PAINT);
  Bind(wxEVT_PAINT,[this](wxPaintEvent&){wxAutoBufferedPaintDC dc(this);dc.SetBackground(GetBackgroundColour());dc.Clear();auto r=GetClientRect();wxRendererNative::Get().DrawGauge(this,dc,r,std::max(0,percent),100);dc.SetFont(GetFont());dc.SetTextForeground(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));dc.DrawLabel(text,r,wxALIGN_CENTER);});
 }
 void set(int value,const wxString& label){if(value==percent&&label==text)return;percent=value;text=label;Refresh();}
};
// A text-only QToolButton: plain label, light rounded highlight on hover, darker while pressed, never focused.
// Borderless wxButtons still draw a frame on MSW, and wxToolBar spaces text-only tools unevenly.
class ToolButton:public wxControl {
 bool hover=false,pressed=false;
public:
 ToolButton(wxWindow* parent,const wxString& label):wxControl(parent,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxBORDER_NONE){
  SetLabel(label);SetToolTip(label);SetBackgroundStyle(wxBG_STYLE_PAINT);
  Bind(wxEVT_PAINT,[this](wxPaintEvent&){wxAutoBufferedPaintDC dc(this);auto back=GetParent()->GetBackgroundColour();dc.SetBackground(back);dc.Clear();auto r=GetClientRect();
   if(hover||pressed){dc.SetPen(*wxTRANSPARENT_PEN);dc.SetBrush(back.ChangeLightness(pressed?84:92));dc.DrawRoundedRectangle(r.Deflate(FromDIP(1)),FromDIP(4));}
   dc.SetFont(GetFont());dc.SetTextForeground(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT));dc.DrawLabel(GetLabel(),r,wxALIGN_CENTER);});
  Bind(wxEVT_ENTER_WINDOW,[this](wxMouseEvent&){hover=true;Refresh();});Bind(wxEVT_LEAVE_WINDOW,[this](wxMouseEvent&){hover=pressed=false;Refresh();});
  Bind(wxEVT_LEFT_DOWN,[this](wxMouseEvent&){pressed=true;Refresh();});
  Bind(wxEVT_LEFT_UP,[this](wxMouseEvent&){bool click=pressed&&hover;pressed=false;Refresh();if(click){wxCommandEvent event(wxEVT_BUTTON,GetId());event.SetEventObject(this);ProcessWindowEvent(event);}});
 }
 bool AcceptsFocus()const override{return false;}
protected:
 wxSize DoGetBestClientSize()const override{return wxSize(GetTextExtent(GetLabel()).x+FromDIP(16),FromDIP(25));}
};
bool fileHost(const wxString& link){auto host=wxURI(link).GetServer().Lower();for(auto name:{"archive.org","www.archive.org","huggingface.co","www.huggingface.co","hf.co","github.com","www.github.com"})if(host==name)return true;return archive::isFileSiteLink(u(link));}
MainWindow::MainWindow(downloads::Queue& q):wxFrame(nullptr,wxID_ANY,"Download Manager"),queue(q),timer(this) {
 // Qt's text-only QToolBar: flat buttons with even padding, a separator and a line underneath.
 // wxToolBar with text-only tools gives uneven widths on both MSW and GTK, so it is built from flat buttons.
 auto toolbar=new wxPanel(this);auto tools=new wxBoxSizer(wxHORIZONTAL);
 auto action=[this,toolbar,tools](const wxString& title,std::function<void()> fn){auto b=new ToolButton(toolbar,title);b->Bind(wxEVT_BUTTON,[fn](wxCommandEvent&){fn();});tools->Add(b,0,wxALIGN_CENTER_VERTICAL|wxTOP|wxBOTTOM,FromDIP(3));}; action("+ File",[this]{showFiles(this,queue);});action("+ Video",[this]{wxString link;if(wxTheClipboard->Open()){wxTextDataObject d;if(wxTheClipboard->GetData(d))link=d.GetText().Strip(wxString::both);wxTheClipboard->Close();}if(fileHost(link)||archive::looksLikeFileUrl(u(link))||!video::validUrl(link.ToStdWstring()))link.clear();showVideo(this,queue,link);});tools->Add(new wxStaticLine(toolbar,wxID_ANY,wxDefaultPosition,FromDIP(wxSize(1,18)),wxLI_VERTICAL),0,wxALIGN_CENTER_VERTICAL|wxLEFT|wxRIGHT,FromDIP(2));
 action("Resume all",[this]{queue.resumeAll();refresh();});action("Pause all",[this]{queue.pauseAll();refresh();});
 action("Remove",[this]{for(auto id:selected())if(mode==3)queue.removeHistory(id);else queue.remove(id);refresh();});action("Remove All",[this]{queue.removeAll();refresh();});
 action("Open folder",[this]{auto ids=selected();for(auto& j:shown)if(!ids.empty()&&j.id==ids.front()){auto s=j.output.empty()?j.input.value("target",j.input.value("folder",std::string())):j.output;if(s.empty())return;auto path=archive::pathFromUtf8(s);if(j.kind=="Archive"||!j.output.empty())path=path.parent_path();openFolder(w(archive::pathUtf8(path)));return;}openFolder(pref("folder",downloadFolder()));});toolbar->SetSizer(tools);
 auto p=panel=new wxPanel(this);
 auto frameBox=new wxBoxSizer(wxVERTICAL);frameBox->Add(toolbar,0,wxEXPAND|wxLEFT,FromDIP(2));frameBox->Add(new wxStaticLine(this),0,wxEXPAND);frameBox->Add(p,1,wxEXPAND);SetSizer(frameBox);auto outer=new wxBoxSizer(wxVERTICAL);auto box=new wxBoxSizer(wxVERTICAL);outer->Add(box,1,wxEXPAND|wxLEFT|wxRIGHT,FromDIP(18));box->AddSpacer(FromDIP(16));
 auto row=new wxBoxSizer(wxHORIZONTAL);url=field(p,{},"Paste a file, release, package, MediaFire, Google Drive, OneDrive, WeTransfer, archive.org or video link");row->Add(url,1,wxRIGHT,FromDIP(6));auto load=new wxButton(p,wxID_ANY,"Load");load->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){route();});row->Add(load,0,wxALIGN_CENTER_VERTICAL);box->Add(row,0,wxEXPAND|wxBOTTOM,FromDIP(12));
 // Qt uses a QTabBar here; a notebook with empty pages is wx's native tab strip.
 row=new wxBoxSizer(wxHORIZONTAL);tabs=new wxNotebook(p,wxID_ANY);for(auto s:{"All","Active","Queue","History"}){auto page=new wxPanel(tabs);page->SetMinSize(wxSize(-1,0));tabs->AddPage(page,s);}fitTabs();row->Add(tabs,0,wxALIGN_BOTTOM);
 tabs->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED,[this](wxBookCtrlEvent& e){if(e.GetEventObject()!=tabs)return;mode=std::max(0,e.GetSelection());refresh();});
 row->AddStretchSpacer();search=field(p,{},"Search downloads");search->SetMinSize(FromDIP(wxSize(180,-1)));search->SetMaxSize(FromDIP(wxSize(240,-1)));row->Add(search,1);box->Add(row,0,wxEXPAND|wxBOTTOM,FromDIP(12));
 historyRow=row=new wxBoxSizer(wxHORIZONTAL);again=button(p,row,"Download Again",[this]{try{for(auto id:selected())queue.downloadAgain(id);refresh();}catch(const std::exception& e){wxMessageBox(w(e.what()),"Download Again",wxOK|wxICON_WARNING,this);}});clear=button(p,row,"Clear History",[this]{queue.clearHistory();refresh();});row->AddStretchSpacer();box->Add(row,0,wxEXPAND|wxBOTTOM,FromDIP(12));
 list=new wxListCtrl(p,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxLC_REPORT|wxCLIP_CHILDREN);int c=0;for(auto s:{"Name","Type","Size","Progress","Status","Speed"})list->InsertColumn(c++,s);
 auto images=new wxImageList(1,FromDIP(54),true);wxImage blank(1,FromDIP(54),true);blank.InitAlpha();std::fill_n(blank.GetAlpha(),FromDIP(54),0);images->Add(wxBitmap(blank));list->AssignImageList(images,wxIMAGE_LIST_SMALL);
 int widths[]={300,75,100,210,105,120};for(int i=0;i<6;++i)list->SetColumnWidth(i,FromDIP(widths[i]));box->Add(list,1,wxEXPAND);
 // A QLabel centres its text vertically in the 40 px minimum height.
 detail=new wxStaticText(p,wxID_ANY,"",wxDefaultPosition,wxDefaultSize,wxST_NO_AUTORESIZE);auto detailBox=new wxBoxSizer(wxVERTICAL);detailBox->SetMinSize(wxSize(-1,FromDIP(40)));detailBox->AddStretchSpacer();detailBox->Add(detail,0,wxEXPAND);detailBox->AddStretchSpacer();box->Add(detailBox,0,wxEXPAND|wxTOP,FromDIP(12));
 footer=new wxStaticText(p,wxID_ANY,"",wxDefaultPosition,wxDefaultSize,wxST_NO_AUTORESIZE);box->Add(footer,0,wxEXPAND|wxTOP,FromDIP(12));box->AddSpacer(FromDIP(16));p->SetSizer(outer);
 // Qt's resize/setMinimumSize set the content size (toolbar included, frame excluded); the window manager places a new window.
 SetMinClientSize(FromDIP(wxSize(640,420)));
 long width=1100,height=700;pref("windowWidth","1100").ToLong(&width);pref("windowHeight","700").ToLong(&height);SetClientSize(FromDIP(wxSize(std::clamp(int(width),640,4000),std::clamp(int(height),420,3000))));
 // Keep the window inside the work area (the Steam Deck's is about 1280x756 in desktop mode).
 {auto area=wxDisplay(this).GetClientArea();SetSize(GetSize().GetWidth()>area.width?area.width:-1,GetSize().GetHeight()>area.height?area.height:-1);}
 if(pref("maximized")=="1")Maximize();
#ifdef _WIN32
 long x=0,y=0;if(pref("windowX").ToLong(&x)&&pref("windowY").ToLong(&y)&&wxDisplay::GetFromPoint(wxPoint(int(x)+40,int(y)+40))!=wxNOT_FOUND)Move(int(x),int(y));
#endif
 url->Bind(wxEVT_TEXT_ENTER,[this](wxCommandEvent&){route();});search->Bind(wxEVT_TEXT,[this](wxCommandEvent&){refresh();});
 auto selection=[this](wxListEvent& e){if(e.GetEventType()==wxEVT_LIST_ITEM_SELECTED&&!wxGetKeyState(WXK_SHIFT))selectionAnchor=e.GetIndex();e.Skip();CallAfter([this]{if(!stopping)refresh();});};list->Bind(wxEVT_LIST_ITEM_SELECTED,selection);list->Bind(wxEVT_LIST_ITEM_DESELECTED,selection);
 // Per-cell tooltips as in Qt. The generic (GTK) list receives mouse events in a child window.
 auto tooltip=[this](wxMouseEvent& e){e.Skip();auto target=static_cast<wxWindow*>(e.GetEventObject());int flags=0;long column=-1;long row=list->HitTest(e.GetPosition(),flags,&column);wxString tip;if(row>=0&&size_t(row)<shown.size())tip=column==5?w("Transfer speed in bytes per second"):w(shown[size_t(row)].detail+"\n"+shown[size_t(row)].output);if(target->GetToolTipText()!=tip){if(tip.empty())target->UnsetToolTip();else target->SetToolTip(tip);}};
 list->Bind(wxEVT_MOTION,tooltip);for(auto child:list->GetChildren())child->Bind(wxEVT_MOTION,tooltip);
 list->Bind(wxEVT_SIZE,[this](wxSizeEvent& e){e.Skip();CallAfter([this]{columns();progressCells();});});
 list->Bind(wxEVT_SCROLLWIN_THUMBTRACK,[this](wxScrollWinEvent& e){e.Skip();CallAfter([this]{progressCells();});});list->Bind(wxEVT_SCROLLWIN_THUMBRELEASE,[this](wxScrollWinEvent& e){e.Skip();CallAfter([this]{progressCells();});});list->Bind(wxEVT_MOUSEWHEEL,[this](wxMouseEvent& e){e.Skip();CallAfter([this]{progressCells();});});list->Bind(wxEVT_LIST_COL_END_DRAG,[this](wxListEvent& e){e.Skip();CallAfter([this]{progressCells();});});
 Bind(wxEVT_TIMER,[this](wxTimerEvent&){if(stopping){if(ready(closing)){try{closing.get();}catch(...){}Destroy();}}else if(closeRequested)Close();else {auto request=dataPath/"activate.request";std::ifstream in(request);if(in){std::string message;std::getline(in,message);in.close();std::error_code ec;std::filesystem::remove(request,ec);activate(message=="video");}refresh();}});Bind(wxEVT_CLOSE_WINDOW,&MainWindow::close,this);
 timer.Start(250);refresh();CallAfter([this]{columns();});
}
void MainWindow::columns(){int rest=0;for(int c=1;c<6;++c)rest+=list->GetColumnWidth(c);list->SetColumnWidth(0,std::max(FromDIP(140),list->GetClientSize().x-rest-FromDIP(4)));}
std::vector<uint64_t> MainWindow::selected()const{std::vector<uint64_t> ids;long i=-1;while((i=list->GetNextItem(i,wxLIST_NEXT_ALL,wxLIST_STATE_SELECTED))!=-1)if(size_t(i)<shown.size())ids.push_back(shown[size_t(i)].id);return ids;}
void MainWindow::updateRates(const std::vector<downloads::Job>& jobs){
 rates.clear();auto now=std::chrono::steady_clock::now();for(auto& j:jobs){wxString rate=w("—");
  if(j.state=="Active"&&j.kind=="Archive"&&j.detail.starts_with("Downloading")){auto [it,inserted]=samples.try_emplace(j.id,Sample{j.done,now});auto& s=it->second;if(j.done<s.bytes)s=Sample{j.done,now};else if(!inserted){double sec=std::chrono::duration<double>(now-s.time).count();if(sec>=0.5){s.rate=w(archive::formatBytes(uint64_t(double(j.done-s.bytes)/sec)))+"/s";s.bytes=j.done;s.time=now;}}rate=s.rate;}else samples.erase(j.id);
  if(j.state=="Active"&&j.kind=="Video"){static const std::regex speed(R"(^\s*[\d.]+%\s*\|\s*([\d.]+\s*[KMGT]?i?B/s)\s*\|)");std::smatch match;if(std::regex_search(j.detail,match,speed))rate=w(match[1].str());}
  rates[j.id]=rate;
 }std::erase_if(samples,[this](const auto& pair){return !rates.contains(pair.first);});
}
void MainWindow::progressCells(){
 size_t used=0;int top=list->GetTopItem();int last=std::min(int(shown.size()),top+list->GetCountPerPage()+2);int clientHeight=list->GetClientSize().y;
 for(int r=std::max(0,top);r<last;++r){wxRect rect;if(!list->GetSubItemRect(r,3,rect)||rect.y<FromDIP(20)||rect.GetBottom()>clientHeight)continue;
  if(used==bars.size()){auto control=new ProgressBar(list);bars.push_back(control);control->Bind(wxEVT_LEFT_DOWN,[this,control](wxMouseEvent& event){long row=long(reinterpret_cast<intptr_t>(control->GetClientData()))-1;if(row<0)return;bool wasSelected=(list->GetItemState(row,wxLIST_STATE_SELECTED)&wxLIST_STATE_SELECTED)!=0;long anchor=selectionAnchor;if(!event.ControlDown()){long selected=-1;while((selected=list->GetNextItem(selected,wxLIST_NEXT_ALL,wxLIST_STATE_SELECTED))!=-1)list->SetItemState(selected,0,wxLIST_STATE_SELECTED);}if(event.ShiftDown()&&anchor>=0&&anchor<long(shown.size())){for(long i=std::min(row,anchor);i<=std::max(row,anchor);++i)list->SetItemState(i,wxLIST_STATE_SELECTED,wxLIST_STATE_SELECTED);}else{list->SetItemState(row,event.ControlDown()&&wasSelected?0:wxLIST_STATE_SELECTED,wxLIST_STATE_SELECTED);selectionAnchor=row;}list->SetItemState(row,wxLIST_STATE_FOCUSED,wxLIST_STATE_FOCUSED);list->SetFocus();});}
  auto cell=bars[used++];auto& j=shown[size_t(r)];int percent=j.state=="Complete"?100:j.total?int(std::min(100.0,100.0*double(j.done)/double(j.total))):-1;
  cell->SetClientData(reinterpret_cast<void*>(intptr_t(r+1)));
  // Qt insets the bar by 8 px horizontally and 10 px vertically inside the cell.
  cell->SetSize(rect.x+FromDIP(8),rect.y+FromDIP(10),std::max(1,rect.width-FromDIP(16)),std::max(1,rect.height-FromDIP(20)));cell->SetBackgroundColour(list->GetItemBackgroundColour(r));
  cell->set(percent,percent<0?(j.state=="Active"?w("Working…"):w("—")):percent==100&&j.state=="Active"?w("Processing…"):wxString::Format("%d%%",percent));cell->SetToolTip(w(j.detail+"\n"+j.output));cell->Show();
 }
 for(size_t i=used;i<bars.size();++i)bars[i]->Hide();
}
// wxNotebook sizes itself from its (empty) pages, not its tabs, which left the strip 3-13 px wide
// and the view tabs invisible. Size it from the real tab extents instead.
void MainWindow::fitTabs(){
 int width=0,height=0;
#ifdef __WXMSW__
 RECT rect;if(TabCtrl_GetItemRect(static_cast<HWND>(tabs->GetHWND()),int(tabs->GetPageCount())-1,&rect)){width=rect.right+FromDIP(2);height=rect.bottom+FromDIP(2);}
#elif defined(__WXGTK__)
 // wxGTK makes the tabs scrollable, which shrinks the preferred width to one tab plus arrows;
 // without the page frame only the tab row (and its baseline) is drawn, like a QTabBar.
 gtk_notebook_set_scrollable(GTK_NOTEBOOK(tabs->GetHandle()),FALSE);gtk_notebook_set_show_border(GTK_NOTEBOOK(tabs->GetHandle()),FALSE);
 gint minimum=0,natural=0;gtk_widget_get_preferred_width(tabs->GetHandle(),&minimum,&natural);width=natural;gtk_widget_get_preferred_height(tabs->GetHandle(),&minimum,&natural);height=natural;
 gint pageMinimum=0,pageNatural=0;gtk_widget_get_preferred_height(tabs->GetPage(0)->GetHandle(),&pageMinimum,&pageNatural);height-=pageNatural;
#endif
 if(width<=0){for(size_t i=0;i<tabs->GetPageCount();++i)width+=tabs->GetTextExtent(tabs->GetPageText(i)).x+FromDIP(24);height=tabs->GetBestSize().y;}
 tabs->SetMinSize(wxSize(width,height));
}
void MainWindow::logLayout(){
 auto r=[](wxWindow* c){return wxString::Format("%d,%d %dx%d best %dx%d%s",c->GetPosition().x,c->GetPosition().y,c->GetSize().x,c->GetSize().y,c->GetBestSize().x,c->GetBestSize().y,c->IsShownOnScreen()?"":" hidden");};
 LOG_INFO("Window %dx%d client %dx%d%s",GetSize().x,GetSize().y,GetClientSize().x,GetClientSize().y,IsMaximized()?" maximized":"");
 LOG_INFO("Layout: url %s; tabs %s; search %s; list %s",u(r(url)).c_str(),u(r(tabs)).c_str(),u(r(search)).c_str(),u(r(list)).c_str());
}
void MainWindow::refresh(){
 if(stopping)return;auto ids=selected();auto jobs=queue.snapshot();updateRates(jobs);
 for(auto& j:jobs){auto& seen=loggedStates[j.id];if(seen!=j.state){if(j.state=="Failed")LOG_ERROR("%s %s: %s -> Failed: %s",j.kind.c_str(),j.name.c_str(),seen.empty()?"new":seen.c_str(),j.detail.c_str());else LOG_INFO("%s %s: %s -> %s",j.kind.c_str(),j.name.c_str(),seen.empty()?"new":seen.c_str(),j.state.c_str());seen=j.state;}}
 if(queue.error()!=loggedError){loggedError=queue.error();if(!loggedError.empty())LOG_ERROR("Queue: %s",loggedError.c_str());}std::vector<downloads::Job> next;auto query=search->GetValue().Lower();for(auto& j:jobs)if(visible(j,mode,"")&&w(j.name).Lower().Contains(query))next.push_back(j);
 bool rebuild=next.size()!=shown.size();if(!rebuild)for(size_t i=0;i<next.size();++i)if(next[i].id!=shown[i].id){rebuild=true;break;}
 list->Freeze();if(rebuild)list->DeleteAllItems();shown=next;
 for(size_t i=0;i<next.size();++i){auto& j=next[i];if(rebuild)list->InsertItem(long(i),w(j.name));wxString values[]={w(j.name),j.kind=="Archive"?"File":"Video",j.kind=="Video"?w("—"):w(j.total?archive::formatBytes(j.total):"Unknown"),"",w(j.state),rates[j.id]};
  for(int c=0;c<6;++c)if(rebuild||list->GetItemText(long(i),c)!=values[c])list->SetItem(long(i),c,values[c]);
  if(rebuild){list->SetItemBackgroundColour(long(i),i%2?wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE):wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));if(std::find(ids.begin(),ids.end(),j.id)!=ids.end())list->SetItemState(long(i),wxLIST_STATE_SELECTED,wxLIST_STATE_SELECTED);}
 }
 list->Thaw();auto current=selected();selectedDetail="Paste a URL above. Select one, all, or specific files before adding downloads.";for(const auto& j:shown)if(!current.empty()&&j.id==current.front()){selectedDetail=w(j.name+"\n"+j.detail+(j.output.empty()?"":" · "+j.output));break;}detail->SetLabel(selectedDetail);detail->Wrap(std::max(1,detail->GetClientSize().x));
 // Hide the spacer too so the empty row collapses, as Qt's layout does.
 bool history=mode==3;historyRow->ShowItems(history);again->Enable(!current.empty());int active=0,waiting=0,done=0,listed=0;for(auto& j:jobs){active+=j.state=="Active";waiting+=j.state=="Queued"||j.state=="Paused"||j.state=="Failed";done+=j.state=="Complete";listed+=!j.historyOnly;}
 clear->Enable(done>0);int counts[]={listed,active,waiting,done};const char* names[]={"All","Active","Queue","History"};for(int i=0;i<4;++i){auto label=wxString::Format("%s (%d)",names[i],counts[i]);if(tabs->GetPageText(size_t(i))!=label){tabs->SetPageText(size_t(i),label);fitTabs();}}
 footer->SetLabel(queue.error().empty()?(queue.paused()?w("All paused  ·  "):wxString())+wxString::Format(wxS("%d / 4 downloading  ·  %d waiting  ·  %d completed"),active,waiting,done):w(queue.error()));footer->Wrap(std::max(1,footer->GetClientSize().x));detail->GetParent()->Layout();progressCells();
}
void MainWindow::route(){auto text=url->GetValue().Strip(wxString::both);auto link=u(text);if(fileHost(text)||archive::looksLikeFileUrl(link))showFiles(this,queue,text);else if(video::validUrl(text.ToStdWstring()))showVideo(this,queue,text);else wxMessageBox("Paste a valid HTTP or HTTPS file, archive.org or video URL.","URL",wxOK|wxICON_WARNING,this);}
void MainWindow::activate(bool video){if(stopping)return;if(IsIconized())Iconize(false);Show();Raise();RequestUserAttention();if(video)openVideo();}
void MainWindow::close(wxCloseEvent& e){
 if(!IsMaximized()&&!IsIconized()){auto size=ToDIP(GetClientSize());save("windowWidth",wxString::Format("%d",size.x));save("windowHeight",wxString::Format("%d",size.y));auto pos=GetPosition();save("windowX",wxString::Format("%d",pos.x));save("windowY",wxString::Format("%d",pos.y));}save("maximized",IsMaximized()?"1":"0");
 if(!e.CanVeto()){timer.Stop();queue.shutdown();Destroy();return;}e.Veto();if(stopping)return;bool pending=false;auto children=GetChildren();for(auto child:children)if(child->IsTopLevel()){child->Close();if(child->IsShown())pending=true;}if(pending){closeRequested=true;footer->SetLabel(w("Stopping dialog tasks before closing…"));return;}stopping=true;panel->Disable();footer->SetLabel(w("Stopping downloads and saving history…"));closing=std::async(std::launch::async,[this]{queue.shutdown();});
}
void MainWindow::smokeCheck(){
 queue.pauseAll();queue.add("Smoke file","Archive","smoke",{{"target",archive::pathUtf8(dataPath/"smoke.bin")}});refresh();if(list->GetItemCount()!=1)throw std::runtime_error("Queue row not displayed");list->SetItemState(0,wxLIST_STATE_SELECTED,wxLIST_STATE_SELECTED);refresh();if(selected().size()!=1)throw std::runtime_error("Selection lost");if(list->GetColumnCount()!=6||list->GetItemText(0,1)!="File")throw std::runtime_error("Queue columns differ from Qt");search->SetValue("missing");if(list->GetItemCount()!=0)throw std::runtime_error("Filter failed");search->Clear();
 for(int index:{1,2,3,0}){tabs->SetSelection(size_t(index));if(mode!=index||tabs->GetSelection()!=index)throw std::runtime_error("View tab did not switch mode");if(list->GetItemCount()!=((index==0||index==2)?1:0))throw std::runtime_error("View tab filter failed");}
 if(tabs->GetPageText(0)!="All (1)")throw std::runtime_error("View tab counts differ from Qt");
 // The four labels need well over 150 px; a strip sized from the empty pages was 3-13 px wide.
 if(tabs->GetSize().x<FromDIP(150)||tabs->GetSize().y<FromDIP(16))throw std::runtime_error("View tabs are not fully visible");
 showFiles(this,queue);showVideo(this,queue);queue.removeAll();std::ofstream(dataPath/"smoke-passed.txt")<<"Native controls, six columns, view buttons, queue selection/filter, file checkboxes and shutdown passed.\n";CallAfter([this]{Close();});
}
}
