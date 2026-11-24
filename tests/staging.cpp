// Staging helpers (src/staging.cpp), without the network.
#include "staging.h"
#include "platform/files.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static std::string readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
static bool allBytes(const std::string& data, size_t offset, size_t count, char value) {
    for (size_t i = 0; i < count; ++i)
        if (data[offset + i] != value) return false;
    return true;
}
static void dataFileTests() {
    namespace fs = std::filesystem;
    static std::atomic_int counter{0};
    const fs::path dir = fs::temp_directory_path() / ("skein-datafile-test-" + std::to_string(counter++) + "-" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())));
    fs::remove_all(dir);
    fs::create_directories(dir);
    try {
        constexpr size_t MiB = 1024 * 1024;
        const fs::path file = dir / "a.part";
        {
            platform::DataFile data(file, true, 8 * MiB);
            const std::string high(MiB, '\xAB'), low(MiB, '\xCD');
            std::thread a([&] {
                data.write(6 * MiB, high.data(), high.size());
            });
            std::thread b([&] {
                data.write(0, low.data(), low.size());
            });
            a.join();
            b.join();
            data.sync();
        }
        check(fs::file_size(file) == 8 * MiB, "A DataFile created with a known size has that length.");
        auto bytes = readAll(file);
        check(allBytes(bytes, 6 * MiB, MiB, '\xAB'), "The region written at 6 MiB reads back.");
        check(allBytes(bytes, 0, MiB, '\xCD'), "The region written at 0 reads back.");
        {
            platform::DataFile data(file, true, 8 * MiB);
            data.write(3 * MiB, "x", 1);
            data.write(4 * MiB, "x", 0);
        }
        check(fs::file_size(file) == 8 * MiB, "Reopening with the same size keeps the length.");
        bytes = readAll(file);
        check(allBytes(bytes, 6 * MiB, MiB, '\xAB') && allBytes(bytes, 0, MiB, '\xCD'), "Reopening keeps the bytes already written.");
        check(bytes[3 * MiB] == 'x', "A byte written after reopening reads back.");
        {
            platform::DataFile data(file, true, 4 * MiB);
        }
        check(fs::file_size(file) == 4 * MiB, "Reopening with a smaller size truncates.");
        const fs::path open = dir / "b.part";
        {
            platform::DataFile data(open, false, 0);
            data.write(0, "0123456789", 10);
            data.write(10, "abcdefghij", 10);
        }
        check(readAll(open) == "0123456789abcdefghij", "Without a known size the file grows with the writes.");
#ifndef _WIN32
        const fs::path link = dir / "link.part";
        fs::create_symlink(file, link);
        bool refused = false;
        try {
            platform::DataFile data(link, true, 8 * MiB);
        } catch (const std::runtime_error&) {
            refused = true;
        }
        check(refused, "A DataFile refuses a symbolic link.");
#else
        // A file symbolic link needs administrator rights or Developer Mode; try, and skip the check when it cannot be made.
        const fs::path link = dir / "link.part";
        std::error_code ec;
        fs::create_symlink(file, link, ec);
        if (!ec) {
            bool refused = false;
            try {
                platform::DataFile data(link, true, 8 * MiB);
            } catch (const std::runtime_error&) {
                refused = true;
            }
            check(refused, "A DataFile refuses a symbolic link.");
        }
#endif
    } catch (...) {
        fs::remove_all(dir);
        throw;
    }
    std::error_code ignored;
    fs::remove_all(dir, ignored);
}
static void convertedDoneTests() {
    using archive::convertedDone;
    using archive::Range;
    using Counts = std::vector<uint64_t>;
    const std::vector<Range> same{{0, 9}, {10, 19}, {20, 29}};
    check(convertedDone(same, {10, 4, 0}, same) == Counts{10, 4, 0}, "With the same partition old and new, the counts are the old lengths.");
    const std::vector<Range> quarters{{0, 24}, {25, 49}, {50, 74}, {75, 99}};
    check(convertedDone({{0, 99}}, {50}, quarters) == Counts{25, 25, 0, 0}, "Half of one old part fills the first two of four new ranges.");
    check(convertedDone({{0, 49}, {50, 99}}, {50, 10}, {{0, 39}, {40, 79}, {80, 99}}) == Counts{40, 20, 0}, "Adjacent old parts count as one run of valid bytes.");
    check(convertedDone({{0, 49}, {50, 99}}, {80, 0}, {{0, 39}, {40, 79}, {80, 99}}) == Counts{40, 10, 0}, "An old length larger than its range counts as the range length.");
    check(convertedDone({}, {}, quarters) == Counts{0, 0, 0, 0}, "Without old parts every count is zero.");
    check(convertedDone({{0, 49}, {50, 99}}, {0, 50}, {{0, 49}, {50, 99}}) == Counts{0, 50}, "A valid run that starts inside a new range is not counted for it.");
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
        convertedDoneTests();
        dataFileTests();
        std::cout << "staging tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
}
