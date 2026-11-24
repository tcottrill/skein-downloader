#pragma once
#include "core.h"
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
namespace archive {
// Empty when `needed` bytes fit in `available`; otherwise the message to show for a download that cannot fit.
std::string freeSpaceError(uint64_t needed, uint64_t available);
// What identifies one version of a file for resuming: the ETag when it is strong (not empty, not "W/..."),
// otherwise the Last-Modified value; empty when the server gave neither.
std::wstring resumeValidator(const std::wstring& etag, const std::wstring& lastModified);
// True when a 206 answer proves it is another version than the stored validator: a stored ETag
// (it starts with a double quote) is compared only with the answer's strong ETag, a stored
// Last-Modified only with the answer's Last-Modified. A missing header proves nothing.
bool validatorChanged(const std::wstring& stored, const std::wstring& answerEtag, const std::wstring& answerLastModified);
// Converting a staging folder of the older layouts (one file per part): old part i holds valid bytes
// oldRanges[i].first ... first + min(oldLengths[i], its range length) - 1. For each new range, the
// number of contiguous valid bytes starting at its first byte.
std::vector<uint64_t> convertedDone(const std::vector<Range>& oldRanges, const std::vector<uint64_t>& oldLengths, const std::vector<Range>& newRanges);
// How often a download saves its progress while transferring: 5 s unless a test changes it.
void setCheckpointInterval(std::chrono::milliseconds interval);
std::chrono::milliseconds checkpointInterval();
}
