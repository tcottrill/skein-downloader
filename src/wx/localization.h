#pragma once
#include <wx/translation.h>
#include <wx/arrstr.h>

namespace lite {
struct Language {const char* code;const char* name;};
// Native names keep the selector usable even after choosing an unfamiliar language.
inline constexpr Language languages[]={
 {"system",""},{"en","English"},{"zh_CN","简体中文"},{"zh_TW","繁體中文"},{"de","Deutsch"},{"vi","Tiếng Việt"},
 {"ru","Русский"},{"ja","日本語"},{"ko","한국어"},{"fr","Français"},{"it","Italiano"},{"es","Español"},
 {"pt","Português"},{"hi","हिन्दी"}
};
wxString resolveLanguage(const wxString& saved,const wxArrayString& preferred,unsigned codePage=0);
wxString currentLanguage();
void initTranslations(const wxString& saved);
// Translation at the UI boundary, without changing persisted backend state.
wxString translated(const std::string& message);
}
