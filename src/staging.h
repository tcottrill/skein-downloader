#pragma once
#include "core.h"
#include <cstdint>
#include <string>
namespace archive {
// Empty when `needed` bytes fit in `available`; otherwise the message to show for a download that cannot fit.
std::string freeSpaceError(uint64_t needed, uint64_t available);
}
