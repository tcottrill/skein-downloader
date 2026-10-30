#include "ui.h"
#include <wx/utils.h>
#include <wx/display.h>
#include <iostream>
#include "platform/files.h"
#include "util/sys_log.h"
#ifdef __WXGTK__
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <csignal>
#include "skein_icons.h"
#endif
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif
#include <wx/aboutdlg.h>
#include <wx/generic/aboutdlgg.h>
namespace {
// The "theme" setting: "system" (the default), "light" or "dark", chosen in the About box.
const char* const themeNames[]={"system","light","dark"};
wxApp::Appearance appearance(const wxString& theme){return theme=="light"?wxApp::Appearance::Light:theme=="dark"?wxApp::Appearance::Dark:wxApp::Appearance::System;}
// wx's own About box with a theme choice under the text. GTK changes the theme of open windows
// at once; Windows (wx's dark mode) only when windows are created, so there it applies on the
// next start.
class AboutDialog:public wxGenericAboutDialog {
 void DoAddCustomControls() override {
  auto row=new wxPanel(this);auto box=new wxBoxSizer(wxHORIZONTAL);
  box->Add(new wxStaticText(row,wxID_ANY,"Theme"),0,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(8));
  auto choice=new wxChoice(row,wxID_ANY);for(auto s:{"Same as system","Light","Dark"})choice->Append(s);
  const auto saved=lite::pref("theme","system");choice->SetSelection(0);for(int i=0;i<3;++i)if(saved==themeNames[i])choice->SetSelection(i);
  box->Add(choice,0,wxALIGN_CENTER_VERTICAL);row->SetSizer(box);AddControl(row,wxSizerFlags().Border(wxUP|wxDOWN));
  choice->Bind(wxEVT_CHOICE,[this](wxCommandEvent& e){
   const wxString theme=themeNames[std::clamp(e.GetSelection(),0,2)];lite::save("theme",theme);LOG_INFO("Theme set to %s",lite::u(theme).c_str());
#ifdef __WXMSW__
   wxMessageBox("The new theme applies the next time Skein Downloader starts.","Theme",wxOK|wxICON_INFORMATION,this);
#else
   wxTheApp->SetAppearance(appearance(theme));
#endif
  });
 }
};

#ifdef __WXMSW__
// Every size of the exe's "appicon" group (16-256 px).
const wxIconBundle& iconBundle(){
 static const wxIconBundle icons(wxString("appicon"),static_cast<WXHINSTANCE>(::GetModuleHandleW(nullptr)));
 return icons;
}
#endif
#ifdef __WXGTK__
// One of the compiled-in PNGs as a pixbuf (the caller owns the reference), or null.
GdkPixbuf* pixbuf(const SkeinIcon& icon){
 GdkPixbuf* result=nullptr;auto loader=gdk_pixbuf_loader_new();
 if(gdk_pixbuf_loader_write(loader,icon.data,icon.size,nullptr)&&gdk_pixbuf_loader_close(loader,nullptr)){
  if(auto image=gdk_pixbuf_loader_get_pixbuf(loader))result=GDK_PIXBUF(g_object_ref(image));
 }else gdk_pixbuf_loader_close(loader,nullptr);
 g_object_unref(loader);return result;
}
// Window icons for X11 from the compiled-in PNGs, so they show even when the theme has no
// installed icon (an AppImage that was never integrated). Wayland uses the .desktop icon.
void defaultIcons(){
 GList* list=nullptr;
 for(const auto& icon:skeinIcons)if(auto image=pixbuf(icon))list=g_list_append(list,image);
 gtk_window_set_default_icon_list(list);
 LOG_INFO("Window icons: %u sizes",g_list_length(list));
 g_list_free_full(list,g_object_unref);
}
#endif
}
namespace lite {archive::Path dataPath;std::unique_ptr<wxFileConfig> config;bool smoke=false;
void appIcons(wxTopLevelWindow* window){
#ifdef __WXMSW__
 // wx gives the title bar and the taskbar the size that matches the window's monitor DPI,
 // and picks again after a DPI change.
 if(!iconBundle().IsEmpty())window->SetIcons(iconBundle());
#else
 (void)window; // GTK: the default icon list set at startup covers every window.
#endif
}
void showAbout(wxWindow* parent){
 wxAboutDialogInfo info;
 info.SetName("Skein Downloader");
 info.SetVersion(SKEIN_VERSION);
 info.SetDescription("An open-source download manager for files and video.\nStill a work in progress.");
 info.SetCopyright(wxString::FromUTF8("Copyright \xC2\xA9 2026 Tim Cottrill"));
 info.SetLicence(
  "This program is free software: you can redistribute it and/or modify it under the terms of the "
  "GNU General Public License as published by the Free Software Foundation, either version 3 of the "
  "License, or (at your option) any later version.\n\n"
  "This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without "
  "even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU "
  "General Public License for more details: https://www.gnu.org/licenses/gpl-3.0.html\n\n"
  "Third-party components keep their own licenses: wxWidgets (wxWindows Library Licence), "
  "nlohmann/json (MIT), and the yt-dlp, FFmpeg and Deno video tools.");
#ifdef __WXMSW__
 info.SetIcon(iconBundle().GetIcon(parent->FromDIP(wxSize(64,64)),wxIconBundle::FALLBACK_NEAREST_LARGER));
#else
 if(auto image=pixbuf(skeinIcons[5])){wxIcon icon;icon.CopyFromBitmap(wxBitmap(image));info.SetIcon(icon);} // 128 px
#endif
 LOG_INFO("About box opened");
 AboutDialog dialog;if(dialog.Create(info,parent))dialog.ShowModal();
}
}
namespace {
// wx errors and warnings go to the log file as well as to wx's usual dialogs.
class LogBridge:public wxLog {
protected:
 void DoLogTextAtLevel(wxLogLevel level,const wxString& text) override {auto s=lite::u(text);if(level<=wxLOG_Error)LOG_ERROR("wx: %s",s.c_str());else if(level==wxLOG_Warning)LOG_WARN("wx: %s",s.c_str());else LOG_INFO("wx: %s",s.c_str());}
};
wxString environment(const char* name){wxString value;return wxGetEnv(name,&value)?value:wxString("(unset)");}
// --screenshot: saves what the window draws, like Qt's QWidget::grab().
void capture(wxTopLevelWindow* window,const wxString& path){
#ifdef __WXGTK__
 // Rendering the widget tree works on Wayland too, where the screen cannot be read back.
 auto widget=window->GetHandle();int width=gtk_widget_get_allocated_width(widget),height=gtk_widget_get_allocated_height(widget);
 auto surface=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,width,height);auto cr=cairo_create(surface);gtk_widget_draw(widget,cr);cairo_destroy(cr);
 auto status=cairo_surface_write_to_png(surface,path.utf8_str());cairo_surface_destroy(surface);
 if(status!=CAIRO_STATUS_SUCCESS)LOG_ERROR("Screenshot %s failed: %s",lite::u(path).c_str(),cairo_status_to_string(status));
#else
 // Toolbar plus client area, without the title bar, read from the screen (PNG support is not built into wx here).
 window->Raise();window->Update();wxRect area(window->ClientToScreen(wxPoint(0,0)),window->GetClientSize());
 if(auto frame=wxDynamicCast(window,wxFrame);frame&&frame->GetToolBar())area.Union(frame->GetToolBar()->GetScreenRect());
 wxBitmap bitmap(area.GetSize());{wxMemoryDC memory(bitmap);wxScreenDC screen;memory.Blit(0,0,area.width,area.height,&screen,area.x,area.y);}
 if(!bitmap.SaveFile(path,wxBITMAP_TYPE_BMP))LOG_ERROR("Screenshot %s failed",lite::u(path).c_str());
#endif
 LOG_INFO("Screenshot %s: %dx%d",lite::u(path).c_str(),window->GetClientSize().x,window->GetClientSize().y);
}
}
class Application:public wxApp {
 std::unique_ptr<downloads::Queue> queue;
 std::unique_ptr<platform::FileLock> instance;
 wxTimer smokeTimer;wxString screenshot;
public:
 bool OnInit() override {
  SetAppName("SkeinDownloader");SetVendorName("");
#ifdef __WXGTK__
  // The Wayland app_id / X11 class must match the .desktop file for the KDE taskbar icon.
  g_set_prgname("io.github.tcottrill.SkeinDownloader");
#endif
  bool video=false,logging=false;wxString overridePath;
  // Failures before the profile is usable, or when its log cannot be written, go to the temporary folder.
  auto fallbackLog=[&logging]{if(!logging)logging=Log::open(lite::u(wxFileName::GetTempDir())+"/skein-downloader-startup.log");};
  for(int i=1;i<argc;++i){wxString a=argv[i];if(a=="--smoke-test")lite::smoke=true;else if(a=="/video"||a=="--video")video=true;else if(a=="--data-dir"&&i+1<argc)overridePath=argv[++i];else if(a=="--screenshot"&&i+1<argc)screenshot=argv[++i];}
  try {
   wxString dir=overridePath;
   if(dir.empty()) {
#ifdef _WIN32
    wxString local;wxGetEnv("LOCALAPPDATA",&local);if(local.empty())local=wxGetHomeDir()+"/AppData/Local";dir=local+"/SkeinDownloader";
#else
    wxString base;wxGetEnv("XDG_DATA_HOME",&base);if(base.empty())base=wxGetHomeDir()+"/.local/share";dir=base+"/SkeinDownloader";
#endif
   }
   if(lite::smoke&&overridePath.empty())dir=wxFileName::GetTempDir()+wxString::Format("/skein-downloader-smoke-%ld",wxGetProcessId());
   lite::dataPath=archive::pathFromUtf8(lite::u(dir));std::filesystem::create_directories(lite::dataPath);
   lite::dataPath=std::filesystem::weakly_canonical(lite::dataPath);
   dir=lite::w(archive::pathUtf8(lite::dataPath));
   // wxSingleInstanceChecker compares PIDs, which Flatpak's PID namespace makes meaningless
   // (every sandboxed instance is PID 2). An OS file lock is released when the holder exits.
   try{instance=std::make_unique<platform::FileLock>(lite::dataPath/"instance.lock");}catch(const std::exception&){}
   if(!instance){
    auto pending=lite::dataPath/("activate-"+std::to_string(wxGetProcessId())+".tmp");
    {std::ofstream out(pending);out<<(video?"video":"activate");if(!out)throw std::runtime_error("Cannot request activation of the existing window.");}
    auto request=lite::dataPath/"activate.request";platform::replace(pending,request);
    // The running window takes the request within 250 ms. If nobody does, the lock failed for another
    // reason (unwritable profile, unsupported filesystem): say so rather than exiting without a window.
    for(int i=0;i<20&&std::filesystem::exists(request);++i)wxMilliSleep(100);
    if(std::filesystem::exists(request)){std::error_code ec;std::filesystem::remove(request,ec);throw std::runtime_error("Cannot lock the settings folder "+lite::u(dir)+". Another copy may be stuck, or the folder is not writable.");}
    SetExitOnFrameDelete(false);CallAfter([this]{ExitMainLoop();});return true;
   }
   // Only the instance holding the profile writes the log, so a second launch cannot truncate it.
   logging=Log::open(lite::u(dir)+"/skein-downloader.log");
   if(!logging){std::cerr<<"Cannot open the log file in "<<lite::u(dir)<<std::endl;fallbackLog();LOG_WARN("Cannot open the log file in %s; logging here instead",lite::u(dir).c_str());}
   LOG_INFO("Skein Downloader %s (built %s %s), wxWidgets %d.%d.%d",SKEIN_VERSION,__DATE__,__TIME__,wxMAJOR_VERSION,wxMINOR_VERSION,wxRELEASE_NUMBER);
   LOG_INFO("Profile %s",lite::u(dir).c_str());
   LOG_INFO("Environment: FLATPAK_ID=%s XDG_SESSION_TYPE=%s XDG_CURRENT_DESKTOP=%s WAYLAND_DISPLAY=%s DISPLAY=%s GDK_BACKEND=%s",lite::u(environment("FLATPAK_ID")).c_str(),lite::u(environment("XDG_SESSION_TYPE")).c_str(),lite::u(environment("XDG_CURRENT_DESKTOP")).c_str(),lite::u(environment("WAYLAND_DISPLAY")).c_str(),lite::u(environment("DISPLAY")).c_str(),lite::u(environment("GDK_BACKEND")).c_str());
   new wxLogChain(new LogBridge);
#ifdef __WXGTK__
   // GTK and GLib warnings (theme, display and widget problems) are copied into the log, then printed as usual.
   g_log_set_writer_func([](GLogLevelFlags level,const GLogField* fields,gsize count,gpointer)->GLogWriterOutput{
    if(level&(G_LOG_LEVEL_ERROR|G_LOG_LEVEL_CRITICAL|G_LOG_LEVEL_WARNING|G_LOG_LEVEL_MESSAGE)){auto text=g_log_writer_format_fields(level,fields,count,FALSE);LOG_WARN("%s",text);g_free(text);}
    return g_log_writer_default(level,fields,count,nullptr);},nullptr,nullptr);
   defaultIcons();
   {gchar* theme=nullptr;g_object_get(gtk_settings_get_default(),"gtk-theme-name",&theme,nullptr);LOG_INFO("Display backend %s, GTK %d.%d.%d, theme %s",G_OBJECT_TYPE_NAME(gdk_display_get_default()),gtk_get_major_version(),gtk_get_minor_version(),gtk_get_micro_version(),theme?theme:"(none)");g_free(theme);}
#endif
   {wxDisplay display;auto area=display.GetClientArea();LOG_INFO("Screen %dx%d, work area %dx%d, scale %.2f",display.GetGeometry().width,display.GetGeometry().height,area.width,area.height,display.GetScaleFactor());}
   lite::config=std::make_unique<wxFileConfig>("SkeinDownloader","",dir+"/settings.ini","",wxCONFIG_USE_LOCAL_FILE);
   // Before the first window: Windows applies the theme only to windows created afterwards.
   {auto theme=lite::pref("theme","system");auto result=SetAppearance(appearance(theme));LOG_INFO("Theme %s%s",lite::u(theme).c_str(),result==AppearanceResult::Ok?"":" (not supported here)");}
   queue=std::make_unique<downloads::Queue>(lite::dataPath/"history.json",downloads::runJob);
   queue->start();auto main=new lite::MainWindow(*queue);SetTopWindow(main);main->Show();
   if(video)main->openVideo();
   main->CallAfter([main]{main->logLayout();});
   if(lite::smoke){
    // Give the windows time to lay out and paint before the screenshot, as Qt's smoke run does.
    smokeTimer.Bind(wxEVT_TIMER,[this,main,video](wxTimerEvent&){
     if(!screenshot.empty()){wxTopLevelWindow* target=main;if(video)for(auto child:main->GetChildren())if(auto dialog=wxDynamicCast(child,wxDialog);dialog&&dialog->IsShown())target=dialog;capture(target,screenshot);}
     main->smokeCheck();});
    smokeTimer.StartOnce(screenshot.empty()?10:1500);
   }
#ifdef __WXGTK__
   // Steam's "Exit game" and session logout send SIGTERM; close normally so history and settings are saved.
   for(int signal:{SIGTERM,SIGHUP,SIGINT})g_unix_signal_add(signal,[](gpointer)->gboolean{LOG_INFO("Termination signal received, closing");if(auto top=wxTheApp->GetTopWindow())top->Close();return G_SOURCE_CONTINUE;},nullptr);
   std::cout<<"Display backend: "<<G_OBJECT_TYPE_NAME(gdk_display_get_default())<<std::endl;
#endif
   return true;
  }catch(const std::exception& e){fallbackLog();LOG_ERROR("Startup failed: %s",e.what());wxMessageBox(lite::w(e.what()),"Startup failed",wxOK|wxICON_ERROR);Log::close();return false;}
 }
 bool OnExceptionInMainLoop() override {
  try{throw;}catch(const std::exception& e){LOG_ERROR("Unhandled exception: %s",e.what());}catch(...){LOG_ERROR("Unhandled non-standard exception");}
  Log::close();return wxApp::OnExceptionInMainLoop();
 }
 int OnExit() override {queue.reset();lite::config.reset();instance.reset();LOG_INFO("Exit");Log::close();return 0;}
};
wxIMPLEMENT_APP(Application);
