// Staging helpers (src/staging.cpp), without the network.
#include "host_limit.h"
#include "staging.h"
#include "platform/files.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
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
// Polls for up to `limit`; true when the condition became true.
template <class F>
static bool within(std::chrono::milliseconds limit, F condition) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}
// A thread that takes a HostSlot, records that it has it, and keeps it until released.
struct SlotHolder {
    std::atomic_bool got{false}, release{false}, stop{false}, failed{false};
    std::atomic_int waited{0};
    std::thread thread;
    SlotHolder(const std::string& host) {
        thread = std::thread([this, host] {
            try {
                archive::HostSlot slot(host, stop, [this] {
                    ++waited;
                });
                got = true;
                while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } catch (const std::exception&) {
                failed = true;
            }
        });
    }
    ~SlotHolder() {
        release = true;
        stop = true;
        if (thread.joinable()) thread.join();
    }
};
static void hostLimitTests() {
    using namespace std::chrono_literals;
    // The limit is process-wide: put it back whatever happens.
    struct Restore {
        ~Restore() { archive::setHostLimit(16); }
    } restore;
    {
        archive::setHostLimit(2);
        std::atomic_int now{0}, most{0}, finished{0};
        std::atomic_bool never{false};
        std::vector<std::thread> threads;
        for (int i = 0; i < 5; ++i) threads.emplace_back([&] {
            archive::HostSlot slot("a.test", never);
            const int n = ++now;
            int seen = most;
            while (n > seen && !most.compare_exchange_weak(seen, n)) {}
            std::this_thread::sleep_for(50ms);
            --now;
            ++finished;
        });
        for (auto& t : threads) t.join();
        check(most <= 2, "With a limit of 2, no more than 2 slots of a host are held at once.");
        check(most == 2, "With a limit of 2 and 5 waiting threads, 2 slots are in use at once.");
        check(finished == 5, "All 5 threads get their slot in the end.");
    }
    {
        archive::setHostLimit(1);
        std::atomic_bool stop{false};
        archive::HostSlot held("A.test", stop);
        SlotHolder sameHost("a.test");
        SlotHolder otherHost("b.test");
        check(within(1000ms, [&] {
                  return otherHost.got.load();
              }),
              "A slot for another host is not blocked by a full host.");
        check(!within(150ms, [&] {
            return sameHost.got.load();
        }),
              "Host names are compared without regard to case: A.test blocks a.test.");
        check(sameHost.waited == 1, "A waiting constructor reports that it waits once.");
    }
    {
        archive::setHostLimit(1);
        std::atomic_bool stop{false};
        std::atomic_int calls{0};
        archive::HostSlot held("c.test", stop);
        std::atomic_bool threw{false};
        std::thread waiter([&] {
            try {
                archive::HostSlot slot("c.test", stop, [&] {
                    ++calls;
                });
            } catch (const std::runtime_error& e) {
                threw = std::string(e.what()) == "Paused";
            }
        });
        std::this_thread::sleep_for(250ms);
        const auto asked = std::chrono::steady_clock::now();
        stop = true;
        waiter.join();
        check(threw, "A constructor waiting for a slot throws Paused when stop becomes true.");
        check(std::chrono::steady_clock::now() - asked < 300ms, "A waiting constructor leaves within 300 ms of the stop.");
        check(calls == 1, "The waiting callback is called exactly once.");
    }
    {
        archive::setHostLimit(2);
        SlotHolder first("d.test"), second("d.test");
        check(within(1000ms, [&] {
                  return first.got.load() && second.got.load();
              }),
              "Two slots of a limit of 2 are taken at once.");
        archive::setHostLimit(1);
        SlotHolder third("d.test");
        check(!within(150ms, [&] {
            return third.got.load();
        }),
              "A third constructor waits while 2 slots are held.");
        first.release = true;
        first.thread.join();
        check(!within(150ms, [&] {
            return third.got.load();
        }),
              "After lowering the limit to 1, a third constructor still waits while 1 slot is held.");
        second.release = true;
        second.thread.join();
        check(within(1000ms, [&] {
                  return third.got.load();
              }),
              "After both slots are released, the third constructor gets its slot.");
    }
    {
        archive::setHostLimit(1);
        SlotHolder first("e.test");
        check(within(1000ms, [&] {
                  return first.got.load();
              }),
              "The first constructor gets the only slot.");
        // Started only now, so it is the one that waits.
        SlotHolder second("e.test");
        check(!within(150ms, [&] {
            return second.got.load();
        }),
              "The second constructor waits while the only slot is held.");
        archive::setHostLimit(2);
        check(within(300ms, [&] {
                  return second.got.load();
              }),
              "Raising the limit gives the waiter its slot within 300 ms.");
    }
    archive::setHostLimit(0);
    archive::setHostLimit(100);
    {
        // 0 and 100 are clamped to 1 and 16: after 100, sixteen slots are free at once.
        std::atomic_bool stop{false};
        std::vector<std::unique_ptr<archive::HostSlot>> slots;
        for (int i = 0; i < 16; ++i) slots.push_back(std::make_unique<archive::HostSlot>("f.test", stop));
        SlotHolder extra("f.test");
        check(!within(150ms, [&] {
            return extra.got.load();
        }),
              "A limit above 16 is clamped to 16.");
    }
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
        hostLimitTests();
        std::cout << "staging tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
}
