#include "ui.h"
#include <wx/menu.h>
#include <wx/imagpng.h>
#include <wx/mstream.h>
#include <array>
#include <cstring>
#include "language_flags.h"

namespace lite {
namespace {
// The PNGs are embedded in the executable, then decoded into bundles once for this selector.
wxBitmapBundle flagIcon(const char* code){
 if(!wxImage::FindHandler(wxBITMAP_TYPE_PNG))wxImage::AddHandler(new wxPNGHandler);
 const char* language=*code?code:"system";
 wxVector<wxBitmap> bitmaps;
 for(int size:{20,30,40,60})for(const auto& asset:embeddedLanguageFlags){
  if(asset.size!=size||std::strcmp(asset.language,language)!=0)continue;
  wxMemoryInputStream stream(asset.data,asset.length);
  wxImage image;
  if(image.LoadFile(stream,wxBITMAP_TYPE_PNG))bitmaps.push_back(wxBitmap(image));
  else wxLogError("Could not decode embedded flag %s at %d px",language,size);
  break;
 }
 return wxBitmapBundle::FromBitmaps(bitmaps);
}
}
wxButton* languageButton(wxWindow* parent){
 const auto code=currentLanguage();wxString name="English";
 for(const auto& language:languages)if(code==language.code)name=wxString::FromUTF8(language.name);
 std::array<wxBitmapBundle,std::size(languages)> flagBundles;
 size_t currentFlag=1;
 for(size_t i=0;i<std::size(languages);++i){
  flagBundles[i]=flagIcon(languages[i].code);
  if(code==languages[i].code)currentFlag=i;
 }
 auto button=new wxButton(parent,wxID_ANY,name+L" \u25be");
 button->SetBitmap(flagBundles[currentFlag]);
#ifdef __WXMSW__
 // Use wx's themed owner drawing for image+text buttons: the native image-list
 // text layout clips the first glyph at some Windows DPI scales.
 button->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT));
#endif
 button->SetToolTip(_(L"Language"));
 button->SetName("languageSelector");
 button->Bind(wxEVT_BUTTON,[button,flagBundles=std::move(flagBundles)](wxCommandEvent&){
  wxMenu menu;const auto saved=pref("language","system");
  for(size_t i=0;i<std::size(languages);++i){
   const auto& language=languages[i];
   wxString label=i?wxString::FromUTF8(language.name):_(L"Same as system");
   if(saved==language.code)label+=L"  \u2713";
   auto item=menu.Append(int(i)+1,label);item->SetBitmap(flagBundles[i]);
  }
  const int selected=button->GetPopupMenuSelectionFromUser(menu,wxPoint(0,button->GetClientSize().y))-1;
  if(selected<0||size_t(selected)>=std::size(languages)||saved==languages[selected].code)return;
  save("language",languages[selected].code);
  wxMessageBox(_(L"The new language applies the next time Skein Downloader starts."),_(L"Language"),wxOK|wxICON_INFORMATION,wxGetTopLevelParent(button));
 });
 return button;
}
}
