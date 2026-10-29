#include "files.h"
#include <windows.h>
#include <bcrypt.h>
#include <stdexcept>
namespace platform {
void regular(const archive::Path& path){auto a=GetFileAttributesW(path.c_str());if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("A download path is a link or junction.");}
void replace(const archive::Path& a,const archive::Path& b){if(!MoveFileExW(a.c_str(),b.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot replace history file");}
void publish(const archive::Path& a,const archive::Path& b){if(!MoveFileExW(a.c_str(),b.c_str(),MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot finalize file; destination may already exist");}
void random(unsigned char* p,size_t n){if(BCryptGenRandom(nullptr,p,(ULONG)n,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("Cannot generate download identity");}
struct FileLock::Impl{HANDLE h;archive::Path path;~Impl(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}};
FileLock::FileLock(const archive::Path& path):impl(std::make_unique<Impl>()){impl->path=path;impl->h=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(impl->h==INVALID_HANDLE_VALUE)throw std::runtime_error("Another download is using this destination, or the folder is not writable.");}
FileLock::~FileLock()=default;
// The handle is opened without sharing, so no other process can hold the file while it is deleted.
void FileLock::removeAndUnlock(){if(!impl)return;auto path=impl->path;impl.reset();std::filesystem::remove(path);}
}
