#include "files.h"
#include <windows.h>
#include <bcrypt.h>
#include <winioctl.h>
#include <stdexcept>
namespace platform {
void regular(const archive::Path& path) {
    auto a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("A download path is a link or junction.");
}
void sync(const archive::Path& path) {
    // FlushFileBuffers needs write access. MOVEFILE_WRITE_THROUGH below flushes only the rename,
    // not the data of the file being renamed.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open file to save it to disk (Windows error " + std::to_string(GetLastError()) + ")");
    const BOOL ok = FlushFileBuffers(h);
    const DWORD error = GetLastError();
    CloseHandle(h);
    if (!ok) throw std::runtime_error("Cannot save file to disk (Windows error " + std::to_string(error) + ")");
}
void replace(const archive::Path& a, const archive::Path& b) {
    regular(a);
    regular(b);
    sync(a);
    if (!MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot replace history file");
}
void publish(const archive::Path& a, const archive::Path& b) {
    regular(a);
    regular(b);
    sync(a);
    if (!MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot finalize file; destination may already exist");
}
void random(unsigned char* p, size_t n) {
    if (BCryptGenRandom(nullptr, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) throw std::runtime_error("Cannot generate download identity");
}
struct FileLock::Impl {
    HANDLE h;
    archive::Path path;
    ~Impl() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
FileLock::FileLock(const archive::Path& path)
    : impl(std::make_unique<Impl>()) {
    impl->path = path;
    impl->h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl->h == INVALID_HANDLE_VALUE) throw std::runtime_error("Another download is using this destination, or the folder is not writable.");
}
FileLock::~FileLock() = default;
// The handle is opened without sharing, so no other process can hold the file while it is deleted.
void FileLock::removeAndUnlock() {
    if (!impl) return;
    auto path = impl->path;
    impl.reset();
    std::filesystem::remove(path);
}
struct DataFile::Impl {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Impl() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
DataFile::DataFile(const archive::Path& path, bool sizeKnown, uint64_t size)
    : impl(std::make_unique<Impl>()) {
    regular(path);
    impl->h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (impl->h == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open the download file (Windows error " + std::to_string(GetLastError()) + ")");
    if (!sizeKnown) return;
    LARGE_INTEGER current{};
    if (!GetFileSizeEx(impl->h, &current)) throw std::runtime_error("Cannot read the download file size (Windows error " + std::to_string(GetLastError()) + ")");
    if (static_cast<uint64_t>(current.QuadPart) == size) return;
    DWORD returned = 0;
    DeviceIoControl(impl->h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr); // best effort
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFileInformationByHandle(impl->h, FileEndOfFileInfo, &info, sizeof(info))) throw std::runtime_error("Cannot set the download file size. Check available space (Windows error " + std::to_string(GetLastError()) + ")");
}
DataFile::~DataFile() = default;
void DataFile::write(uint64_t offset, const char* data, size_t size) {
    while (size) {
        const DWORD chunk = size > 0x40000000 ? 0x40000000 : static_cast<DWORD>(size);
        OVERLAPPED where{};
        where.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
        where.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD written = 0;
        if (!WriteFile(impl->h, data, chunk, &written, &where) || written == 0) throw std::runtime_error("Disk write failed. Check available space.");
        data += written;
        offset += written;
        size -= written;
    }
}
void DataFile::sync() {
    if (!FlushFileBuffers(impl->h)) throw std::runtime_error("Cannot save the download file to disk (Windows error " + std::to_string(GetLastError()) + ")");
}
}
