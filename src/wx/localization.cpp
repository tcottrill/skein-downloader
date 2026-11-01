#include "localization.h"
#include <wx/uilocale.h>
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif
#include "translations.h"

namespace lite {
namespace {
wxString activeLanguage="en";
wxString supported(wxString name){
 name.Replace("-","_");name=name.BeforeFirst('.').BeforeFirst('@').Lower();
 if(name=="zh"||name=="zh_cn"||name=="zh_sg"||name=="zh_hans"||name.StartsWith("zh_hans_"))return "zh_CN";
 if(name=="zh_tw"||name=="zh_hk"||name=="zh_mo"||name=="zh_hant"||name.StartsWith("zh_hant_"))return "zh_TW";
 for(auto code:{"en","ru","ja","ko","fr","it","es","de","vi","pt","hi"})if(name==code||name.StartsWith(wxString(code)+"_"))return code;
 return {};
}
class EmbeddedLoader final:public wxTranslationsLoader {
public:
 wxMsgCatalog* LoadCatalog(const wxString& domain,const wxString& lang)override{
  for(const auto& c:embeddedCatalogs)if(domain==c.domain&&lang==c.language)
   return wxMsgCatalog::CreateFromData(wxScopedCharBuffer::CreateNonOwned(reinterpret_cast<const char*>(c.data),c.size),domain);
  return nullptr;
 }
 wxArrayString GetAvailableTranslations(const wxString& domain)const override{
  wxArrayString result;for(const auto& c:embeddedCatalogs)if(domain==c.domain)result.Add(c.language);return result;
 }
};
}
wxString resolveLanguage(const wxString& saved,const wxArrayString& preferred,unsigned codePage){
 if(saved!="system"&&!saved.empty()){auto code=supported(saved);return code.empty()?wxString("en"):code;}
 for(const auto& name:preferred){auto code=supported(name);if(!code.empty())return code;}
 // Code pages are a fallback when no supported UI language is available.
 // 1252 and UTF-8 cannot distinguish Western languages alone.
 switch(codePage){case 932:return "ja";case 936:return "zh_CN";case 949:return "ko";case 950:return "zh_TW";case 1251:return "ru";case 1258:return "vi";default:break;}
 return "en";
}
wxString currentLanguage(){return activeLanguage;}
void initTranslations(const wxString& saved){
 wxArrayString preferred;for(const auto& name:wxUILocale::GetPreferredUILanguages())preferred.Add(name);
 unsigned codePage=0;
#ifdef __WXMSW__
 codePage=::GetACP();
#endif
 activeLanguage=resolveLanguage(saved,preferred,codePage);
 auto translations=new wxTranslations;
 translations->SetLoader(new EmbeddedLoader);
 translations->SetLanguage(activeLanguage);
 wxTranslations::Set(translations);
 translations->AddStdCatalog();
 translations->AddCatalog("skein");
 // Do not change the C locale: HTTP/JSON numbers and external tools require
 // their existing formatting regardless of the UI's language.
}
wxString translated(const std::string& message){
 const auto text=wxString::FromUTF8(message);
 // Not wxGetTranslation: it keeps every untranslated string for the life of the thread,
 // and details such as yt-dlp progress lines change on every refresh.
 const auto lookup=[](const wxString& s){auto catalogs=wxTranslations::Get();auto found=catalogs?catalogs->GetTranslatedString(s):nullptr;return found?*found:s;};
 // File progress may append a server URL. Translate only the status part.
 const auto split=text.find(wxString::FromUTF8(" · "));
 if(split!=wxString::npos)return lookup(text.Left(split))+text.Mid(split);
 return lookup(text);
}
// Backend values and video quality names are stored in English; mark them for
// catalog extraction and translate only when presenting them to the user.
[[maybe_unused]] static const char* const backendMessages[]={
 wxTRANSLATE("Active"),wxTRANSLATE("Queued"),wxTRANSLATE("Paused"),wxTRANSLATE("Failed"),wxTRANSLATE("Complete"),
 wxTRANSLATE("Video"),wxTRANSLATE("Unknown"),wxTRANSLATE("Connecting"),wxTRANSLATE("Partial download retained"),
 wxTRANSLATE("Finished"),wxTRANSLATE("Pausing…"),wxTRANSLATE("Finalizing"),wxTRANSLATE("Downloading"),
 wxTRANSLATE("Checking mirror speeds (HTTPS)"),wxTRANSLATE("Trying another HTTPS source"),
 wxTRANSLATE("Retrying after connection error"),wxTRANSLATE("Starting parallel download"),
 wxTRANSLATE("Starting single connection"),wxTRANSLATE("Assembling"),wxTRANSLATE("Verifying SHA-256"),wxTRANSLATE("Verifying SHA-1"),
 wxTRANSLATE("Best available"),wxTRANSLATE("Up to 2160p (4K)"),wxTRANSLATE("Up to 1440p"),wxTRANSLATE("Up to 1080p"),
 wxTRANSLATE("Up to 720p"),wxTRANSLATE("Up to 480p"),wxTRANSLATE("Up to 360p"),wxTRANSLATE("Audio only"),
 wxTRANSLATE("Choose an absolute download folder."),wxTRANSLATE("Invalid file selection."),
 wxTRANSLATE("Paste a valid HTTP or HTTPS video URL."),wxTRANSLATE("Install video tools from Add > Video first."),
 wxTRANSLATE("Operation cancelled. You can retry."),wxTRANSLATE("Stopped. Use the same URL, quality and folder to resume."),
 wxTRANSLATE("Video tools ready."),wxTRANSLATE("yt-dlp update finished."),wxTRANSLATE("Unexpected error while managing video tools.")
};
}
