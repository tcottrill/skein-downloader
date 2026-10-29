#include "video.h"
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#include <spawn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <thread>
#include <cerrno>
extern char **environ;
#endif
#include <fstream>
#include <stdexcept>
#include <chrono>
namespace video {
std::wstring quote(const std::wstring& value) {
    std::wstring result=L"\"";size_t slashes=0;
    for(wchar_t c:value) {
        if(c==L'\\'){++slashes;continue;}
        result.append(slashes*(c==L'"'?2:1),L'\\');slashes=0;
        if(c==L'"')result+=L'\\';result+=c;
    }
    result.append(slashes*2,L'\\');return result+L'"';
}
bool validUrl(const std::wstring& url) {
    if(url.size()>8192||std::any_of(url.begin(),url.end(),[](wchar_t c){return c<=32||c==L'"';}))return false;
#ifdef _WIN32
    URL_COMPONENTS c{sizeof c};c.dwHostNameLength=c.dwUserNameLength=c.dwPasswordLength=(DWORD)-1;
    return WinHttpCrackUrl(url.c_str(),0,0,&c) && (c.nScheme==INTERNET_SCHEME_HTTPS||c.nScheme==INTERNET_SCHEME_HTTP)
        && c.dwHostNameLength && !c.dwUserNameLength && !c.dwPasswordLength;
#else
    auto u=curl_url(); if(!u)return false;
    auto text=archive::utf8(url); bool ok=curl_url_set(u,CURLUPART_URL,text.c_str(),0)==CURLUE_OK;
    char *scheme=nullptr,*host=nullptr,*user=nullptr,*password=nullptr;
    ok=ok&&curl_url_get(u,CURLUPART_SCHEME,&scheme,0)==CURLUE_OK&&curl_url_get(u,CURLUPART_HOST,&host,0)==CURLUE_OK;
    curl_url_get(u,CURLUPART_USER,&user,0);curl_url_get(u,CURLUPART_PASSWORD,&password,0);
    ok=ok&&scheme&&(std::string(scheme)=="https"||std::string(scheme)=="http")&&host&&*host&&!user&&!password;
    curl_free(scheme);curl_free(host);curl_free(user);curl_free(password);curl_url_cleanup(u);return ok;
#endif
}
const std::vector<std::wstring>& qualityNames() {
    static const std::vector<std::wstring> names={L"Best available",L"Up to 2160p (4K)",L"Up to 1440p",L"Up to 1080p",L"Up to 720p",L"Up to 480p",L"Up to 360p",L"Audio only"};return names;
}
std::vector<std::wstring> downloadArgs(const archive::Path& tools,const std::wstring& url,const archive::Path& folder,int quality,bool mp3,bool mp4) {
    if(!validUrl(url))throw std::runtime_error("Paste a valid HTTP or HTTPS video URL.");
    if(!folder.is_absolute())throw std::runtime_error("Choose an absolute download folder.");
    archive::safePath(folder,"video-output-check");
    if(quality<0||quality>7)throw std::runtime_error("Choose a quality.");
    std::wstring format=L"bv*+ba/b";
    if(quality>=1&&quality<=6) {
        const int heights[]={2160,1440,1080,720,480,360};auto height=std::to_wstring(heights[quality-1]);
        format=L"bv*[height<="+height+L"]+ba/b[height<="+height+L"]";
    }
    if(quality==7)format=L"ba/b";
    if(mp4&&quality!=7){
        std::wstring cap;
        if(quality>=1&&quality<=6){const int heights[]={2160,1440,1080,720,480,360};cap=L"[height<="+std::to_wstring(heights[quality-1])+L"]";}
        format=L"bv[ext=mp4][vcodec^=avc1]"+cap+L"+ba[ext=m4a][acodec^=mp4a]/b[ext=mp4][vcodec^=avc1][acodec^=mp4a]"+cap;
    }
    const wchar_t* suffixes[]={L"best",L"up-to-2160p",L"up-to-1440p",L"up-to-1080p",L"up-to-720p",L"up-to-480p",L"up-to-360p",L"audio"};
    auto output=L"%(title).150B [%(id)s] ["+std::wstring(quality==7&&mp3?L"audio-mp3":suffixes[quality])+L"].%(ext)s";
    if(mp4&&quality!=7)output.insert(output.find(L"].%(ext)s"),L"-mp4-h264");
    std::vector<std::wstring> args={L"--ignore-config",L"--no-plugin-dirs",L"--no-remote-components",L"--no-cache-dir",L"--no-playlist",L"--playlist-items",L"1",L"--newline",L"--progress",L"--progress-delta",L"0.5",L"--color",L"never",L"--encoding",L"utf-8",L"--windows-filenames",L"--no-overwrites",L"--continue",L"--ffmpeg-location",archive::wide(archive::pathUtf8(tools)),L"--no-js-runtimes",L"--js-runtimes",L"deno:"+archive::wide(archive::pathUtf8(toolExecutable(tools,"deno"))),L"--paths",archive::wide(archive::pathUtf8(folder)),L"--output",L"%(title).150B [%(id)s].%(ext)s",L"--format",format,L"--merge-output-format",L"mkv",L"--progress-template",L"download:PROGRESS %(progress._percent_str)s | %(progress._speed_str)s | ETA %(progress._eta_str)s"};
    *(std::find(args.begin(),args.end(),L"--output")+1)=output;
    if(mp4&&quality!=7)*(std::find(args.begin(),args.end(),L"--merge-output-format")+1)=L"mp4";
    if(quality==7)args.push_back(L"--extract-audio");
    if(quality==7&&mp3)args.insert(args.end(),{L"--audio-format",L"mp3",L"--audio-quality",L"0"});
    args.push_back(L"--");args.push_back(url);return args;
}
const std::vector<std::string>& cookieBrowsers(){
    static const std::vector<std::string> names={"firefox","chrome","edge","brave","chromium","opera","vivaldi"};return names;
}
std::vector<std::wstring> accessArgs(bool impersonate,const std::string& cookiesFromBrowser){
    std::vector<std::wstring> args;
    // "chrome" lets yt-dlp pick its newest Chrome target (provided by curl_cffi in the bundled build).
    if(impersonate)args.insert(args.end(),{L"--impersonate",L"chrome"});
    if(!cookiesFromBrowser.empty()){
        auto& names=cookieBrowsers();
        if(std::find(names.begin(),names.end(),cookiesFromBrowser)==names.end())throw std::runtime_error("Unsupported browser for cookies: "+cookiesFromBrowser);
        args.insert(args.end(),{L"--cookies-from-browser",archive::wide(cookiesFromBrowser)});
    }
    return args;
}
#ifdef _WIN32
#include "platform/process_windows.inc"
#else
#include "platform/process_linux.inc"
#endif
archive::Path toolExecutable(const archive::Path& folder,const std::string& name) {
#ifdef _WIN32
    return folder / archive::pathFromUtf8(name+".exe");
#else
    return folder / name;
#endif
}
}
