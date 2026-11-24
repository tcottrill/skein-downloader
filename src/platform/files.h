#pragma once
#include "core.h"
#include <cstddef>
#include <memory>
namespace platform {
void regular(const archive::Path&);
// Writes a closed file's data to the disk (fsync / FlushFileBuffers), so that after a crash or
// power loss it is either whole or reported missing, never full of bytes that were never written.
void sync(const archive::Path&);
void replace(const archive::Path& from,const archive::Path& to);
void publish(const archive::Path& from,const archive::Path& to);
void random(unsigned char*,size_t);
class FileLock {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    explicit FileLock(const archive::Path&);
    ~FileLock();
    // Deletes the lock file as the download finishes. On Linux the file is unlinked
    // while still locked so no other process can lock a file that is about to vanish.
    void removeAndUnlock();
};
}
