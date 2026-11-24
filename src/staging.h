#pragma once
#include "core.h"
#include <cstdint>
#include <string>
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
}
