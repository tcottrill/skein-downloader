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
        check(resumeValidator(L"\"a\"", L"date") == L"\"a\"", "A strong ETag is the validator.");
        check(resumeValidator(L"W/\"a\"", L"date") == L"date", "A weak ETag is ignored in favor of Last-Modified.");
        check(resumeValidator(L"", L"date") == L"date", "Without an ETag, Last-Modified is the validator.");
        check(resumeValidator(L"", L"").empty(), "Without either header there is no validator.");
        check(resumeValidator(L"W/\"a\"", L"").empty(), "A weak ETag alone is no validator.");
        check(!validatorChanged(L"\"a\"", L"\"a\"", L"any"), "The same ETag is not a change, whatever the date.");
        check(validatorChanged(L"\"a\"", L"\"b\"", L""), "A different strong ETag is a change.");
        check(!validatorChanged(L"\"a\"", L"", L"d"), "A stored ETag is not compared with a date.");
        check(!validatorChanged(L"\"a\"", L"W/\"b\"", L""), "A weak ETag proves nothing.");
        check(!validatorChanged(L"d1", L"", L"d1"), "The same Last-Modified is not a change.");
        check(validatorChanged(L"d1", L"", L"d2"), "A different Last-Modified is a change.");
        check(!validatorChanged(L"d1", L"\"x\"", L""), "A stored date is not compared with an ETag.");
        check(!validatorChanged(L"", L"\"x\"", L"d"), "Nothing stored proves nothing.");
        std::cout << "staging tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
}
