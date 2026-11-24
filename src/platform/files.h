#pragma once
#include "core.h"
#include <cstddef>
#include <cstdint>
#include <memory>
namespace platform {
void regular(const archive::Path&);
// Writes a closed file's data to the disk (fsync / FlushFileBuffers), so that after a crash or
// power loss it is either whole or reported missing, never full of bytes that were never written.
void sync(const archive::Path&);
void replace(const archive::Path& from, const archive::Path& to);
void publish(const archive::Path& from, const archive::Path& to);
void random(unsigned char*, size_t);
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
// One staging file written at any offset from any thread.
class DataFile {
    struct Impl;
    std::unique_ptr<Impl> impl;

public:
    // Opens path, creating it when missing; existing bytes are kept. With sizeKnown the file's
    // length is set to size (on Windows it is marked sparse first; failure to mark is ignored).
    DataFile(const archive::Path& path, bool sizeKnown, uint64_t size);
    ~DataFile();
    DataFile(const DataFile&) = delete;
    DataFile& operator=(const DataFile&) = delete;
    // Writes all bytes at offset; calls with non-overlapping ranges may run on several threads at once.
    // Throws "Disk write failed. Check available space."
    void write(uint64_t offset, const char* data, size_t size);
    // Data to disk (FlushFileBuffers / fsync).
    void sync();
};
}
