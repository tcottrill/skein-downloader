// Staging helpers (src/staging.cpp), without the network.
#include "staging.h"
#include <iostream>
#include <stdexcept>

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    try {
        using namespace archive;
        check(freeSpaceError(10, 10).empty(), "A file that exactly fits has no free-space error.");
        check(freeSpaceError(0, 0).empty(), "Nothing needed has no free-space error.");
        check(freeSpaceError(11, 10) == "Not enough free space: " + formatBytes(11) + " needed, " + formatBytes(10) + " free.", "A file that does not fit names the needed and free space.");
        std::cout << "staging tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
}
