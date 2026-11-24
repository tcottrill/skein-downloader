#include "staging.h"
#include <algorithm>
#include <atomic>
#include <iterator>
#include <utility>
namespace archive {
namespace {
std::atomic<std::chrono::milliseconds::rep> checkpointMilliseconds{5000};
}
void setCheckpointInterval(std::chrono::milliseconds interval) {
    checkpointMilliseconds = interval.count();
}
std::chrono::milliseconds checkpointInterval() {
    return std::chrono::milliseconds(checkpointMilliseconds.load());
}
std::string freeSpaceError(uint64_t needed, uint64_t available) {
    if (needed <= available) return {};
    return "Not enough free space: " + formatBytes(needed) + " needed, " + formatBytes(available) + " free.";
}
std::wstring resumeValidator(const std::wstring& etag, const std::wstring& lastModified) {
    return !etag.empty() && !etag.starts_with(L"W/") ? etag : lastModified;
}
std::vector<uint64_t> convertedDone(const std::vector<Range>& oldRanges, const std::vector<uint64_t>& oldLengths, const std::vector<Range>& newRanges) {
    // The valid bytes as half-open intervals [first, end), sorted and merged where they touch.
    std::vector<std::pair<uint64_t, uint64_t>> valid;
    for (size_t i = 0; i < std::min(oldRanges.size(), oldLengths.size()); ++i) {
        const auto& range = oldRanges[i];
        const auto length = std::min(oldLengths[i], range.last - range.first + 1);
        if (length) valid.push_back({range.first, range.first + length});
    }
    std::sort(valid.begin(), valid.end());
    std::vector<std::pair<uint64_t, uint64_t>> merged;
    for (const auto& interval : valid) {
        if (!merged.empty() && interval.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, interval.second);
        else
            merged.push_back(interval);
    }
    std::vector<uint64_t> done;
    done.reserve(newRanges.size());
    for (const auto& range : newRanges) {
        uint64_t count = 0;
        // The last merged interval that starts at or before the range's first byte.
        auto after = std::upper_bound(merged.begin(), merged.end(), range.first, [](uint64_t first, const std::pair<uint64_t, uint64_t>& interval) {
            return first < interval.first;
        });
        if (after != merged.begin()) {
            const auto& interval = *std::prev(after);
            if (interval.second > range.first) count = std::min(interval.second, range.last + 1) - range.first;
        }
        done.push_back(count);
    }
    return done;
}
bool validatorChanged(const std::wstring& stored, const std::wstring& answerEtag, const std::wstring& answerLastModified) {
    if (stored.empty()) return false;
    if (stored.starts_with(L"\"")) return !answerEtag.empty() && !answerEtag.starts_with(L"W/") && answerEtag != stored;
    return !answerLastModified.empty() && answerLastModified != stored;
}
}
