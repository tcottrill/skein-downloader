#include "staging.h"
namespace archive {
std::string freeSpaceError(uint64_t needed, uint64_t available) {
    if (needed <= available) return {};
    return "Not enough free space: " + formatBytes(needed) + " needed, " + formatBytes(available) + " free.";
}
std::wstring resumeValidator(const std::wstring& etag, const std::wstring& lastModified) {
    return !etag.empty() && !etag.starts_with(L"W/") ? etag : lastModified;
}
bool validatorChanged(const std::wstring& stored, const std::wstring& answerEtag, const std::wstring& answerLastModified) {
    if (stored.empty()) return false;
    if (stored.starts_with(L"\"")) return !answerEtag.empty() && !answerEtag.starts_with(L"W/") && answerEtag != stored;
    return !answerLastModified.empty() && answerLastModified != stored;
}
}
