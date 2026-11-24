#include "files.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#include <cerrno>
#include <stdexcept>
#include <system_error>
namespace platform {
static void failure(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}
void regular(const archive::Path& path) {
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(path))) throw std::runtime_error("A download path is a symbolic link.");
}
void sync(const archive::Path& path) {
    int f = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (f < 0) failure("Open completed file");
    int result = fsync(f);
    int saved = errno;
    ::close(f);
    if (result < 0) {
        errno = saved;
        failure("Flush completed file");
    }
}
static void syncDirectory(const archive::Path& path) {
    int f = open(path.parent_path().c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC);
    if (f >= 0) {
        fsync(f);
        ::close(f);
    }
}
void replace(const archive::Path& a, const archive::Path& b) {
    regular(a);
    regular(b);
    sync(a);
    if (rename(a.c_str(), b.c_str()) < 0) failure("Replace history");
    syncDirectory(b);
}
static void publishByPlaceholder(const archive::Path& a, const archive::Path& b) {
    // Reserve the name exclusively, then rename over that empty placeholder only.
    int f = open(b.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (f < 0) failure("Finalize download without overwriting");
    ::close(f);
    if (rename(a.c_str(), b.c_str()) < 0) {
        int saved = errno;
        unlink(b.c_str());
        errno = saved;
        failure("Finalize download");
    }
}
void publish(const archive::Path& a, const archive::Path& b) {
    regular(a);
    regular(b);
    sync(a);
    if (syscall(SYS_renameat2, AT_FDCWD, a.c_str(), AT_FDCWD, b.c_str(), RENAME_NOREPLACE) < 0) {
        // NFS, CIFS and FUSE folders such as the Flatpak document portal may not support RENAME_NOREPLACE.
        if (errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP) failure("Finalize download without overwriting");
        publishByPlaceholder(a, b);
    }
    syncDirectory(b);
}
void random(unsigned char* p, size_t n) {
    while (n) {
        auto got = getrandom(p, n, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            failure("Generate identity");
        }
        p += got;
        n -= got;
    }
}
struct FileLock::Impl {
    int fd = -1;
    archive::Path path;
    ~Impl() {
        if (fd >= 0) ::close(fd);
    }
};
FileLock::FileLock(const archive::Path& path)
    : impl(std::make_unique<Impl>()) {
    impl->path = path;
    for (int attempt = 0; attempt < 100; ++attempt) {
        impl->fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (impl->fd < 0 || flock(impl->fd, LOCK_EX | LOCK_NB) < 0) throw std::runtime_error("Another download is using this destination, or the folder is not writable.");
        // The previous holder may have unlinked the file between our open and flock; only the file still at path counts.
        struct stat held{}, current{};
        if (fstat(impl->fd, &held) == 0 && lstat(path.c_str(), &current) == 0 && held.st_dev == current.st_dev && held.st_ino == current.st_ino) return;
        ::close(impl->fd);
        impl->fd = -1;
    }
    throw std::runtime_error("Cannot lock the download destination.");
}
FileLock::~FileLock() = default;
void FileLock::removeAndUnlock() {
    if (impl && impl->fd >= 0 && unlink(impl->path.c_str()) < 0 && errno != ENOENT) failure("Remove download lock");
    impl.reset();
}
struct DataFile::Impl {
    int fd = -1;
    ~Impl() {
        if (fd >= 0) ::close(fd);
    }
};
DataFile::DataFile(const archive::Path& path, bool sizeKnown, uint64_t size)
    : impl(std::make_unique<Impl>()) {
    regular(path);
    impl->fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (impl->fd < 0) throw std::runtime_error("Cannot open the download file.");
    if (!sizeKnown) return;
    struct stat info{};
    if (fstat(impl->fd, &info) < 0) throw std::runtime_error("Cannot read the download file size.");
    if (static_cast<uint64_t>(info.st_size) == size) return;
    int result;
    do {
        result = ftruncate(impl->fd, static_cast<off_t>(size));
    } while (result < 0 && errno == EINTR);
    if (result < 0) throw std::runtime_error("Cannot set the download file size. Check available space.");
}
DataFile::~DataFile() = default;
void DataFile::write(uint64_t offset, const char* data, size_t size) {
    while (size) {
        const ssize_t written = pwrite(impl->fd, data, size, static_cast<off_t>(offset));
        if (written < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("Disk write failed. Check available space.");
        }
        if (written == 0) throw std::runtime_error("Disk write failed. Check available space.");
        data += written;
        offset += static_cast<uint64_t>(written);
        size -= static_cast<size_t>(written);
    }
}
void DataFile::sync() {
    int result;
    do {
        result = fsync(impl->fd);
    } while (result < 0 && errno == EINTR);
    if (result < 0) throw std::runtime_error("Cannot save the download file to disk.");
}
}
