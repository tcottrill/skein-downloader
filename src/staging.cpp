#include "staging.h"
namespace archive {
std::string freeSpaceError(uint64_t needed, uint64_t available) {
    if (needed <= available) return {};
    return "Not enough free space: " + formatBytes(needed) + " needed, " + formatBytes(available) + " free.";
}
}
