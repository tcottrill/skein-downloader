#include "wx/localization.h"
#include <wx/init.h>
#include <iostream>
#include <stdexcept>

void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
int main(){
 try {
  wxInitializer init;check(init.IsOk(),"wx initialization");
  check(lite::resolveLanguage("system",{"fr-CA"})=="fr","French regional fallback");
  check(lite::resolveLanguage("system",{"ko-KR"})=="ko","Korean system language");
  check(lite::resolveLanguage("",{"ko-KR"},1252)=="ko","No saved choice defaults to system language");
  check(lite::resolveLanguage("system",{},949)=="ko","Korean Windows code page");
  check(lite::resolveLanguage("en",{"ko-KR"},949)=="en","Saved English overrides Korean system settings");
  check(lite::resolveLanguage("system",{"es-MX"},1252)=="es","Mexican Spanish detection");
  check(lite::resolveLanguage("system",{"es-ES"})=="es","Spain Spanish detection");
  check(lite::resolveLanguage("system",{"es-419"})=="es","Latin American Spanish detection");
  check(lite::resolveLanguage("es",{"en-US"})=="es","Explicit Spanish preference");
  check(lite::resolveLanguage("system",{"zh-Hans-SG"})=="zh_CN","Simplified Chinese script");
  for(auto locale:{"zh-TW","zh-HK","zh-MO","zh-Hant","zh-Hant-HK"})
   check(lite::resolveLanguage("system",{locale})=="zh_TW","Traditional Chinese regions and script");
  check(lite::resolveLanguage("system",{"de-AT","ja-JP"})=="de","German preferred language");
  check(lite::resolveLanguage("system",{"vi-VN"})=="vi","Vietnamese system language");
  check(lite::resolveLanguage("system",{"pt-PT"})=="pt","Portugal Portuguese system language");
  check(lite::resolveLanguage("system",{"pt-BR"})=="pt","Brazilian Portuguese shared catalog");
  check(lite::resolveLanguage("system",{"hi-IN"})=="hi","Hindi system language");
  check(lite::resolveLanguage("system",{},1252)=="en","Western code page remains ambiguous");
  check(lite::resolveLanguage("system",{},65001)=="en","UTF-8 code page remains ambiguous");
  check(lite::resolveLanguage("pt",{"hi-IN"},1252)=="pt","Saved Portuguese overrides system language");
  check(lite::resolveLanguage("hi",{"pt-BR"},65001)=="hi","Saved Hindi overrides system language");
  check(lite::resolveLanguage("en",{"fr-FR"},1252)=="en","Saved English overrides French system language and code page");
  check(lite::resolveLanguage("en",{"es-ES"},1252)=="en","Saved English overrides Spanish system language and code page");
  check(lite::resolveLanguage("invalid",{"fr-FR"})=="en","Unknown saved setting falls back safely");
  check(lite::resolveLanguage("system",{},932)=="ja","Japanese Windows code page");
  check(lite::resolveLanguage("system",{},936)=="zh_CN","Simplified Chinese Windows code page");
  check(lite::resolveLanguage("system",{},1251)=="ru","Russian Windows code page");
  check(lite::resolveLanguage("system",{"fr-FR"},1252)=="fr","Shared Western code page uses UI language");
  check(lite::resolveLanguage("system",{},1252)=="en","Ambiguous Western code page falls back to English");
  check(lite::resolveLanguage("system",{},950)=="zh_TW","Traditional Chinese code page");
  check(lite::resolveLanguage("system",{},1258)=="vi","Vietnamese code page");
  check(lite::resolveLanguage("system",{},437)=="en","Unsupported code page falls back to English");
  check(lite::resolveLanguage("en",{},932)=="en","Explicit language overrides code page");
  for(const auto& language:lite::languages){
   if(wxString(language.code)=="system")continue;
   for(unsigned codePage:{932u,936u,949u,950u,1251u,1252u,1258u,65001u}){
    check(lite::resolveLanguage(language.code,{"ja-JP","fr-FR","ko-KR"},codePage)==language.code,
          "Every saved language overrides system preferences and code page");
   }
  }
  for(auto code:{"zh_CN","ru","ja","fr","it","es","ko","de","zh_TW","vi","pt","hi"}){
   lite::initTranslations(code);
   check(wxGetTranslation("Language")!="Language","Embedded catalog loaded");
   check(wxGetTranslation("untranslated sentinel")=="untranslated sentinel","English fallback");
   check(wxGetTranslation("Cancel")!="Cancel","Standard dialog translations");
  }
  lite::initTranslations("fr");
  check(lite::currentLanguage()=="fr","Active language for toolbar label");
  check(wxGetTranslation("Language")==wxString::FromUTF8("Langue"),"French lookup");
  check(wxString::Format(wxGetTranslation("%zu selected / %zu files / "),size_t(2),size_t(5))==wxString::FromUTF8("2 sélectionnés / 5 fichiers / "),"Format placeholders");
  check(_(L"Browse…")==wxString::FromUTF8("Parcourir…"),"Non-ASCII source message");
  check(lite::translated("Downloading · example.org")==wxString::FromUTF8("Téléchargement · example.org"),"Progress source preserved");
  check(lite::translated("12.5% | 1.2MiB/s | ETA 00:09")=="12.5% | 1.2MiB/s | ETA 00:09","Untranslated backend detail passes through");
  lite::initTranslations("es");
  check(lite::currentLanguage()=="es","Spanish active language");
  check(_(L"Language")==L"Idioma","Spanish application catalog");
  check(_(L"Cancel")==L"Cancelar","Spanish standard dialog catalog");
  check(_(L"Browse…")==L"Examinar…","Spanish punctuation lookup");
  check(wxString::Format(_(L"%zu selected / %zu files / "),size_t(2),size_t(5))==L"2 seleccionados / 5 archivos / ","Spanish format arguments");
  lite::initTranslations("ko");
  check(lite::currentLanguage()=="ko","Korean active language");
  check(_(L"Language")==L"언어","Korean application catalog");
  check(_(L"Browse…")==L"찾아보기…","Korean punctuation lookup");
  check(wxString::Format(_(L"%zu selected / %zu files / "),size_t(2),size_t(5))==L"선택한 파일 2개 / 전체 파일 5개 / ","Korean format arguments");
  lite::initTranslations("pt");
  check(lite::currentLanguage()=="pt","Portuguese active language");
  check(_(L"Language")==L"Idioma","Portuguese application catalog");
  check(_(L"Cancel")==L"Cancelar","Portuguese standard dialog catalog");
  check(_(L"Browse…")==L"Procurar…","Portuguese punctuation lookup");
  check(wxString::Format(_(L"%zu selected / %zu files / "),size_t(2),size_t(5))==L"2 selecionados / 5 ficheiros / ","Portuguese format and whitespace");
  lite::initTranslations("hi");
  check(lite::currentLanguage()=="hi","Hindi active language");
  check(_(L"Language")==L"भाषा","Hindi application catalog");
  check(_(L"Cancel")==L"निरस्त","Hindi standard dialog catalog");
  check(_(L"Browse…")==L"ब्राउज़ करें…","Hindi punctuation lookup");
  lite::initTranslations("en");
  check(lite::currentLanguage()=="en","Active language reset to English");
  check(wxGetTranslation("Language")=="Language","Switch to English");
  std::cout<<"Localization checks passed\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
