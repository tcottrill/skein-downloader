#pragma once
#include "core.h"
#include <algorithm>
namespace video {
using Lines=std::function<void(const std::string&)>;
std::wstring quote(const std::wstring& argument);
bool validUrl(const std::wstring& url);
const std::vector<std::wstring>& qualityNames();
std::vector<std::wstring> downloadArgs(const archive::Path& tools,const std::wstring& url,const archive::Path& folder,int quality,bool mp3=false,bool mp4=false);
// Browsers yt-dlp can read cookies from on Windows and Linux.
const std::vector<std::string>& cookieBrowsers();
// Optional access flags, inserted before "--": impersonate a browser's network
// fingerprint (anti-bot checks) and/or read that site's cookies from a browser
// (age gates; effectively the browser's sign-in). Throws for an unknown browser.
std::vector<std::wstring> accessArgs(bool impersonate,const std::string& cookiesFromBrowser);
int run(const archive::Path& executable,const std::vector<std::wstring>& arguments,std::atomic_bool& stop,const Lines& lines);
archive::Path toolExecutable(const archive::Path& folder,const std::string& name);
archive::Path executableFolder();
archive::Path toolsFolder(const archive::Path& root);
bool toolsReady(const archive::Path& folder);
}
