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
#endif
namespace lite {archive::Path dataPath;std::unique_ptr<wxFileConfig> config;bool smoke=false;}
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
   {gchar* theme=nullptr;g_object_get(gtk_settings_get_default(),"gtk-theme-name",&theme,nullptr);LOG_INFO("Display backend %s, GTK %d.%d.%d, theme %s",G_OBJECT_TYPE_NAME(gdk_display_get_default()),gtk_get_major_version(),gtk_get_minor_version(),gtk_get_micro_version(),theme?theme:"(none)");g_free(theme);}
#endif
   {wxDisplay display;auto area=display.GetClientArea();LOG_INFO("Screen %dx%d, work area %dx%d, scale %.2f",display.GetGeometry().width,display.GetGeometry().height,area.width,area.height,display.GetScaleFactor());}
   lite::config=std::make_unique<wxFileConfig>("SkeinDownloader","",dir+"/settings.ini","",wxCONFIG_USE_LOCAL_FILE);
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
