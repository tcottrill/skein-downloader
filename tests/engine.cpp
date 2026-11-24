// End-to-end download engine tests against the local server in tests/engine_server.py,
// started by tests/run_engine_tests.py, which passes the server's base URL.
#include "host_limit.h"
#include "signin.h"
#include "sites.h"
#include "staging.h"
#include "util/sys_log.h"
#include "json.hpp"
#include <cctype>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "platform/files.h"
#include <windows.h>
#endif

namespace {
constexpr uint64_t MiB = 1024 * 1024;

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// The server's content: byte i of version v is (i * 31 + 7 + v) & 0xFF.
std::string expectedBytes(uint64_t size, int version = 1) {
    std::string data(size, '\0');
    for (uint64_t i = 0; i < size; ++i)
        data[i] = static_cast<char>((i * 31 + 7 + static_cast<uint64_t>(version)) & 0xFF);
    return data;
}

std::string readFile(const archive::Path& path) {
    std::ifstream in(path, std::ios::binary);
    expect(static_cast<bool>(in), "The file " + archive::pathUtf8(path) + " can be opened.");
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

nlohmann::json stats(const std::string& base) {
    std::atomic_bool stop = false;
    return nlohmann::json::parse(archive::fetch(base + "/stats", stop).body);
}

void reset(const std::string& base) {
    std::atomic_bool stop = false;
    archive::fetch(base + "/reset", stop);
}

// Runs archive::download; when pauseAt is not 0, sets stop once done >= pauseAt and expects "Paused".
void run(const std::string& url, const archive::Path& target, unsigned connections, uint64_t pauseAt = 0) {
    std::atomic_bool stop = false;
    archive::File file;
    file.name = "file.bin";
    const auto report = [&](const archive::Progress& progress) {
        if (pauseAt && progress.done >= pauseAt) stop = true;
    };
    bool paused = false;
    try {
        archive::download(url, file, target, connections, stop, report);
    } catch (const std::runtime_error& e) {
        if (!pauseAt || std::string(e.what()) != "Paused") throw;
        paused = true;
    }
    expect(!pauseAt || paused, "The download paused once " + std::to_string(pauseAt) + " bytes were done.");
}

// A fresh folder for one case, removed afterwards.
struct TempFolder {
    archive::Path path;
    explicit TempFolder(const std::string& name)
        : path(std::filesystem::temp_directory_path() / ("skein-engine-test-" + name)) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

#ifdef _WIN32
bool isSparse(const archive::Path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_SPARSE_FILE);
}

// True when the temp drive can mark a file sparse (decided once with a probe DataFile).
bool driveSupportsSparse() {
    static const bool supported = [] {
        TempFolder folder("sparse-probe");
        const auto probe = folder.path / "probe.bin";
        {
            platform::DataFile data(probe, true, 4 * MiB);
        }
        return isSparse(probe);
    }();
    return supported;
}
#endif

void ranges(const std::string& base) {
    TempFolder folder("ranges");
    reset(base);
    const auto target = folder.path / "file.bin";
    run(base + "/etag/" + std::to_string(10 * MiB) + "/file.bin", target, 4);
    expect(readFile(target) == expectedBytes(10 * MiB), "A 10 MiB file with a strong ETag downloads with the right content.");
#ifdef _WIN32
    expect(!isSparse(target), "A finished download is not marked sparse.");
#endif
    expect(stats(base)["ranged"].get<int>() >= 10, "A 10 MiB file with a strong ETag is fetched in at least 10 ranged requests.");
}

void withoutRanges(const std::string& base) {
    for (const char* mode : {"norange", "nolength"}) {
        TempFolder folder(mode);
        const uint64_t size = std::string(mode) == "norange" ? 3 * MiB : 1 * MiB;
        const auto target = folder.path / "file.bin";
        run(base + "/" + mode + "/" + std::to_string(size) + "/file.bin", target, 4);
        expect(readFile(target) == expectedBytes(size), std::string("A file from a server in mode ") + mode + " downloads with the right content.");
    }
}

void emptyFile(const std::string& base) {
    TempFolder folder("empty");
    const auto target = folder.path / "file.bin";
    run(base + "/etag/0/file.bin", target, 4);
    expect(std::filesystem::exists(target) && std::filesystem::file_size(target) == 0, "An empty file is created.");
}

void pauseAndResume(const std::string& base) {
    TempFolder folder("resume");
    reset(base);
    const auto target = folder.path / "file.bin";
    const auto url = base + "/etag/" + std::to_string(10 * MiB) + "/file.bin";
    run(url, target, 4, 3 * MiB);
    expect(!std::filesystem::exists(target), "A paused download leaves no destination file.");
    auto staging = target;
    staging += L".archive-parts";
#ifdef _WIN32
    if (driveSupportsSparse()) expect(isSparse(staging / L"data.tmp"), "An unfinished download's data.tmp is still marked sparse.");
#endif
    run(url, target, 4);
    expect(readFile(target) == expectedBytes(10 * MiB), "A resumed download has the right content.");
    expect(!std::filesystem::exists(staging), "The staging folder is gone after the download finishes.");
    expect(stats(base)["bytes"].get<uint64_t>() < 15 * MiB, "The resumed download reused the data already received.");
}

std::string urlFor(const std::string& base, const std::string& mode, uint64_t size) {
    return base + "/" + mode + "/" + std::to_string(size) + "/file.bin";
}

void lastModifiedRanges(const std::string& base) {
    TempFolder folder("modified");
    reset(base);
    const auto target = folder.path / "file.bin";
    run(urlFor(base, "modified", 10 * MiB), target, 4);
    expect(readFile(target) == expectedBytes(10 * MiB), "A file with only a Last-Modified header downloads with the right content.");
    expect(stats(base)["maxConcurrent"].get<int>() >= 2, "A file with only a Last-Modified header is fetched on several connections.");
}

void lastModifiedResume(const std::string& base) {
    TempFolder folder("modified-resume");
    reset(base);
    const auto target = folder.path / "file.bin";
    const auto url = urlFor(base, "modified", 10 * MiB);
    run(url, target, 4, 3 * MiB);
    expect(!std::filesystem::exists(target), "A paused download from a Last-Modified server leaves no destination file.");
    run(url, target, 4);
    expect(readFile(target) == expectedBytes(10 * MiB), "A download resumed with Last-Modified has the right content.");
    expect(stats(base)["bytes"].get<uint64_t>() < 15 * MiB, "The download resumed with Last-Modified reused the data already received.");
}

void weakEtagOnly(const std::string& base) {
    TempFolder folder("weak");
    reset(base);
    const auto target = folder.path / "file.bin";
    run(urlFor(base, "weak", 10 * MiB), target, 4);
    expect(readFile(target) == expectedBytes(10 * MiB), "A file with only a weak ETag downloads with the right content.");
    expect(stats(base)["ranged"].get<int>() == 0, "A file with only a weak ETag is fetched without ranged requests.");
}

void rangeProbeOnly(const std::string& base) {
    TempFolder folder("rangeprobe");
    reset(base);
    const auto target = folder.path / "file.bin";
    run(urlFor(base, "rangeprobe", 10 * MiB), target, 4);
    expect(readFile(target) == expectedBytes(10 * MiB), "A server that answers only the one-byte probe with 206 still gives the right content.");
}

void changedFile(const std::string& base) {
    for (const char* mode : {"changed", "ignore-ifrange"}) {
        TempFolder folder(mode);
        reset(base);
        const auto target = folder.path / "file.bin";
        run(urlFor(base, mode, 10 * MiB), target, 4);
        expect(readFile(target) == expectedBytes(10 * MiB, 2), std::string("A file that changes during the download (mode ") + mode + ") ends as the new version, never a mix.");
    }
}

archive::Path stagingFor(const archive::Path& target) {
    auto staging = target;
    staging += L".archive-parts";
    return staging;
}

// Runs archive::download; the watch callback sees each progress report and returns true to stop the
// download. Returns true when the download ended with "Paused" after watch asked it to stop.
bool runWatched(const std::string& url, const archive::Path& target, unsigned connections, const std::function<bool(const archive::Progress&)>& watch) {
    std::atomic_bool stop = false;
    archive::File file;
    file.name = "file.bin";
    const auto report = [&](const archive::Progress& progress) {
        if (watch(progress)) stop = true;
    };
    try {
        archive::download(url, file, target, connections, stop, report);
    } catch (const std::runtime_error& e) {
        if (!stop || std::string(e.what()) != "Paused") throw;
        return true;
    }
    return false;
}

// True when the staging folder holds a file of the old layouts (numbered .part files, assembled.tmp).
bool hasOldLayoutFiles(const archive::Path& staging) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(staging, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().wstring();
        if (name.ends_with(L".part") || name == L"assembled.tmp") return true;
    }
    return false;
}

bool hasFile(const archive::Path& staging, const wchar_t* name) {
    std::error_code ec;
    return std::filesystem::exists(staging / name, ec);
}

void writeFile(const archive::Path& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    expect(static_cast<bool>(out), "The file " + archive::pathUtf8(path) + " can be written.");
}

// The bytes a staging folder's state.json records as done (0 when it cannot be read).
uint64_t savedDone(const archive::Path& state) {
    try {
        std::ifstream in(state, std::ios::binary);
        auto json = nlohmann::json::parse(in);
        uint64_t total = 0;
        for (const auto& count : json.at("done")) total += count.get<uint64_t>();
        return total;
    } catch (const std::exception&) {
        return 0;
    }
}

void singleDataFile(const std::string& base) {
    for (const char* mode : {"etag", "modified"}) {
        TempFolder folder(std::string("single-") + mode);
        reset(base);
        const auto target = folder.path / "file.bin";
        const auto staging = stagingFor(target);
        bool oldFiles = false, dataFile = false;
        runWatched(urlFor(base, mode, 10 * MiB), target, 4, [&](const archive::Progress&) {
            if (hasOldLayoutFiles(staging)) oldFiles = true;
            if (hasFile(staging, L"data.tmp")) dataFile = true;
            return false;
        });
        expect(!oldFiles, std::string("While downloading (mode ") + mode + ") the staging folder never holds a .part file or assembled.tmp.");
        expect(dataFile, std::string("While downloading (mode ") + mode + ") the staging folder holds data.tmp.");
        expect(readFile(target) == expectedBytes(10 * MiB), std::string("A file staged in one data file (mode ") + mode + ") has the right content.");
    }
}

// A copy of the staging folder taken mid-download, as a crash would leave it, resumes from its last checkpoint.
void snapshotResume(const std::string& base) {
    archive::setCheckpointInterval(std::chrono::milliseconds(100));
    for (const char* mode : {"etag", "modified"}) {
        TempFolder folder(std::string("snapshot-") + mode);
        reset(base);
        const auto target = folder.path / "file.bin";
        const auto staging = stagingFor(target);
        const auto side = folder.path / "side";
        const auto url = urlFor(base, mode, 10 * MiB);
        std::filesystem::create_directories(side);
        bool copied = false;
        const bool paused = runWatched(url, target, 4, [&](const archive::Progress& progress) {
            if (copied || progress.done < 4 * MiB) return copied;
            // The connections wait for this callback; give the checkpoint timer up to 2 s to record some data.
            for (int i = 0; i < 200 && savedDone(staging / L"state.json") == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            // The state first: the data copied after it holds at least what it records.
            writeFile(side / L"state.json", readFile(staging / L"state.json"));
            for (const auto& entry : std::filesystem::directory_iterator(staging)) {
                const auto name = entry.path().filename();
                // state.json.tmp exists only while a checkpoint is written and renamed; a crash that leaves one loses nothing, as the engine deletes it at the start.
                if (name == L"lock" || name == L"state.json" || name == L"state.json.tmp") continue;
                writeFile(side / name, readFile(entry.path()));
            }
            copied = true;
            return true;
        });
        expect(paused && copied, std::string("The download (mode ") + mode + ") paused after its staging folder was copied.");
        const auto saved = savedDone(side / L"state.json");
        expect(saved > 0, std::string("The copied state (mode ") + mode + ") records some data as done.");
        for (const auto& entry : std::filesystem::directory_iterator(staging))
            if (entry.path().filename() != L"lock") std::filesystem::remove(entry.path());
        for (const auto& entry : std::filesystem::directory_iterator(side)) std::filesystem::copy_file(entry.path(), staging / entry.path().filename());
        const auto before = stats(base)["bytes"].get<uint64_t>();
        run(url, target, 4);
        expect(readFile(target) == expectedBytes(10 * MiB), std::string("A download resumed from a snapshot (mode ") + mode + ") has the right content.");
        const auto bytes = stats(base)["bytes"].get<uint64_t>();
        expect(bytes < 17 * MiB, std::string("A download resumed from a snapshot (mode ") + mode + ") reused the checkpointed data.");
        expect(bytes - before <= 10 * MiB - saved + 64 * 1024, std::string("A download resumed from a snapshot (mode ") + mode + ") fetched only what its checkpoint did not record.");
    }
    archive::setCheckpointInterval(std::chrono::seconds(5));
}

// A staging folder left by the version that wrote one file per part ("queued-v1"): parts 0 and 1
// complete, part 2 half written. With "clean" (a normal stop) it is converted; without it, discarded.
void oldLayout(const std::string& base, bool clean) {
    TempFolder folder(clean ? "old-clean" : "old-unclean");
    reset(base);
    const auto target = folder.path / "file.bin";
    const auto staging = stagingFor(target);
    const auto url = urlFor(base, "etag", 10 * MiB);
    std::filesystem::create_directories(staging);
    writeFile(staging / L"owner.txt", "SkeinDownloader staging v1");
    nlohmann::json state = {{"url", url}, {"size", 10 * MiB}, {"known", true}, {"sha1", ""}, {"etag", "\"skein-test-1\""}, {"parts", 10}, {"ranged", true}, {"layout", "queued-v1"}, {"source", url}};
    if (clean) state["clean"] = true;
    writeFile(staging / L"state.json", state.dump());
    const auto data = expectedBytes(10 * MiB);
    writeFile(staging / L"0.part", data.substr(0, MiB));
    writeFile(staging / L"1.part", data.substr(MiB, MiB));
    writeFile(staging / L"2.part", data.substr(2 * MiB, MiB / 2));
    bool oldFiles = false;
    runWatched(url, target, 4, [&](const archive::Progress&) {
        if (hasOldLayoutFiles(staging)) oldFiles = true;
        return false;
    });
    const std::string kind = clean ? "A converted old staging folder" : "An old staging folder without a clean stop";
    expect(readFile(target) == expectedBytes(10 * MiB), kind + " gives the right content.");
    expect(!oldFiles, kind + " leaves no .part file while downloading.");
    const auto bytes = stats(base)["bytes"].get<uint64_t>();
    if (clean)
        expect(bytes < 8 * MiB + MiB / 2, kind + " reuses its parts.");
    else
        expect(bytes >= 10 * MiB, kind + " is downloaded again.");
}

// The same old folder with "clean", but part 2 is longer than its range: it is not what the state says, so it is
// not reused, while parts 0 and 1 are.
void oldLayoutLongPart(const std::string& base) {
    TempFolder folder("old-long-part");
    reset(base);
    const auto target = folder.path / "file.bin";
    const auto staging = stagingFor(target);
    const auto url = urlFor(base, "etag", 10 * MiB);
    std::filesystem::create_directories(staging);
    writeFile(staging / L"owner.txt", "SkeinDownloader staging v1");
    const nlohmann::json state = {{"url", url}, {"size", 10 * MiB}, {"known", true}, {"sha1", ""}, {"etag", "\"skein-test-1\""}, {"parts", 10}, {"ranged", true}, {"layout", "queued-v1"}, {"source", url}, {"clean", true}};
    writeFile(staging / L"state.json", state.dump());
    const auto data = expectedBytes(10 * MiB);
    writeFile(staging / L"0.part", data.substr(0, MiB));
    writeFile(staging / L"1.part", data.substr(MiB, MiB));
    writeFile(staging / L"2.part", data.substr(2 * MiB, MiB + 10));
    bool oldFiles = false;
    runWatched(url, target, 4, [&](const archive::Progress&) {
        if (hasOldLayoutFiles(staging)) oldFiles = true;
        return false;
    });
    expect(readFile(target) == expectedBytes(10 * MiB), "An old staging folder with a part longer than its range gives the right content.");
    expect(!oldFiles, "An old staging folder with a part longer than its range leaves no .part file while downloading.");
    const auto bytes = stats(base)["bytes"].get<uint64_t>();
    expect(bytes >= 8 * MiB, "A part longer than its range is not reused.");
    expect(bytes < 10 * MiB, "The parts of an old staging folder that fit their ranges are reused.");
}

// Two downloads from one server, each on 4 connections, share the host's limit of 2 connections.
void hostLimit(const std::string& base) {
    // The limit is process-wide: restore it even when this case fails.
    struct Restore {
        ~Restore() { archive::setHostLimit(16); }
    } restore;
    archive::setHostLimit(2);
    TempFolder folder("host-limit");
    reset(base);
    const auto first = folder.path / "first.bin";
    const auto second = folder.path / "second.bin";
    const auto url = urlFor(base, "etag", 6 * MiB);
    std::exception_ptr failure;
    std::thread other([&] {
        try {
            run(url, second, 4);
        } catch (...) {
            failure = std::current_exception();
        }
    });
    try {
        run(url, first, 4);
    } catch (...) {
        other.join();
        throw;
    }
    other.join();
    if (failure) std::rethrow_exception(failure);
    expect(readFile(first) == expectedBytes(6 * MiB), "The first of two downloads sharing a host limit has the right content.");
    expect(readFile(second) == expectedBytes(6 * MiB), "The second of two downloads sharing a host limit has the right content.");
    expect(stats(base)["maxConcurrent"].get<int>() <= 2, "Two downloads from one server with a limit of 2 never have more than 2 ranged responses at once.");
}

void existingDestination(const std::string& base) {
    TempFolder folder("existing");
    const auto target = folder.path / "file.bin";
    {
        std::ofstream out(target, std::ios::binary);
        out << "keep me";
    }
    bool threw = false;
    try {
        run(base + "/etag/1024/file.bin", target, 4);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "Downloading onto an existing file throws.");
    expect(readFile(target) == "keep me", "An existing destination file is left unchanged.");
}

// A LoadWatch counts the response bytes and answered requests of the loaders' reads made with its stop flag, and only those.
void loadWatch(const std::string& base) {
    std::atomic_bool stop = false, other = false;
    archive::LoadProgress progress, otherProgress;
    size_t bytes = 0;
    {
        archive::LoadWatch watch(stop, progress);
        archive::LoadWatch otherWatch(other, otherProgress);
        const auto body = archive::fetch(base + "/stats", stop).body;
        bytes = body.size();
        expect(bytes > 0, "The stats answer has a body.");
        expect(progress.requests == 1 && progress.bytes == bytes, "One fetch under a LoadWatch counts one request and the body's bytes.");
        archive::fetch(base + "/stats", stop);
        expect(progress.requests == 2 && progress.bytes >= 2 * bytes - 8, "A second fetch is added to the same progress.");
        expect(otherProgress.requests == 0 && otherProgress.bytes == 0, "A watch on another stop flag counts nothing for this one.");
    }
    const auto requests = progress.requests.load(), counted = progress.bytes.load();
    archive::fetch(base + "/stats", stop);
    expect(progress.requests == requests && progress.bytes == counted, "Reads after the LoadWatch is gone count nothing.");
}

// Sets the sign-in table for one case; the table is process-wide, so it is cleared even when the case fails.
struct SignIns {
    explicit SignIns(std::vector<archive::SignIn> table) { archive::setSignIns(std::move(table)); }
    ~SignIns() { archive::setSignIns({}); }
};

archive::SignIn passwordFor(const std::string& host, const std::string& user, const std::string& secret) {
    archive::SignIn s;
    s.kind = archive::SignInKind::Password;
    s.host = host;
    s.user = user;
    s.secret = secret;
    return s;
}

archive::SignIn tokenFor(const std::string& host, const std::string& secret) {
    archive::SignIn s;
    s.kind = archive::SignInKind::Token;
    s.host = host;
    s.secret = secret;
    return s;
}

// The Authorization values the server received on a host name since the last reset.
std::vector<std::string> authOn(const std::string& base, const std::string& host) {
    const auto all = stats(base)["auth"];
    if (!all.contains(host)) return {};
    return all[host].get<std::vector<std::string>>();
}

// The number of requests in a server mode that arrived without an Authorization header since the last reset.
int anonymous(const std::string& base, const std::string& mode) {
    const auto all = stats(base)["anonymous"];
    return all.contains(mode) ? all[mode].get<int>() : 0;
}

// The SignInNeeded a download of url throws.
archive::SignInNeeded signInNeeded(const std::string& url, const archive::Path& target, const std::string& what) {
    try {
        run(url, target, 4);
    } catch (const archive::SignInNeeded& e) {
        return e;
    }
    throw std::runtime_error(what + " throws SignInNeeded.");
}

void signInMissing(const std::string& base) {
    SignIns table({});
    TempFolder folder("signin-missing");
    const auto e = signInNeeded(urlFor(base, "basic", 10 * MiB), folder.path / "file.bin", "A download from a server that asks for Basic, with no sign-in saved,");
    expect(!e.rejected, "A refusal when no sign-in was sent is not reported as a rejected sign-in.");
    expect(e.basic, "A refusal with a Basic challenge reports that the server offered Basic.");
    expect(e.host == "127.0.0.1", "A refusal names the host that refused (127.0.0.1), not \"" + e.host + "\".");
}

void basicRanges(const std::string& base) {
    SignIns table({passwordFor("127.0.0.1", "user", "pass")});
    const auto url = urlFor(base, "basic", 10 * MiB);
    {
        TempFolder folder("basic-ranges");
        reset(base);
        const auto target = folder.path / "file.bin";
        run(url, target, 4);
        expect(readFile(target) == expectedBytes(10 * MiB), "A file behind Basic sign-in downloads with the right content.");
        expect(stats(base)["maxConcurrent"].get<int>() >= 2, "A file behind Basic sign-in is fetched on several connections.");
        expect(anonymous(base, "basic") <= 2, "After a server's Basic challenge the password is sent at once: at most 2 requests went without it, not " +
                                                      std::to_string(anonymous(base, "basic")) + ".");
    }
    {
        // A new table forgets which servers challenged: the next request waits for a challenge again.
        archive::setSignIns({passwordFor("127.0.0.1", "user", "other")});
        reset(base);
        std::atomic_bool stop = false;
        bool rejected = false;
        try {
            archive::fetch(url, stop);
        } catch (const archive::SignInNeeded& e) {
            rejected = e.rejected;
        }
        expect(rejected, "A changed password the server refuses is reported as a rejected sign-in.");
        expect(anonymous(base, "basic") == 1 && authOn(base, "127.0.0.1").size() == 1,
               "After the sign-ins change, the first request goes without the password and it is sent once, after the challenge.");
        archive::setSignIns({passwordFor("127.0.0.1", "user", "pass")});
    }
    TempFolder folder("basic-resume");
    reset(base);
    const auto target = folder.path / "file.bin";
    run(url, target, 4, 3 * MiB);
    run(url, target, 4);
    expect(readFile(target) == expectedBytes(10 * MiB), "A resumed download behind Basic sign-in has the right content.");
    expect(stats(base)["bytes"].get<uint64_t>() < 15 * MiB, "A resumed download behind Basic sign-in reused the data already received.");
}

void basicWrong(const std::string& base) {
    SignIns table({passwordFor("127.0.0.1", "user", "nope")});
    TempFolder folder("basic-wrong");
    reset(base);
    const auto e = signInNeeded(urlFor(base, "basic", 10 * MiB), folder.path / "file.bin", "A download with a wrong password");
    expect(e.rejected, "A password the server refuses is reported as a rejected sign-in.");
    expect(authOn(base, "127.0.0.1").size() <= 2, "A refused password is sent at most twice.");
}

void bearer(const std::string& base) {
    SignIns table({tokenFor("127.0.0.1", "tok")});
    TempFolder folder("bearer");
    reset(base);
    const auto target = folder.path / "file.bin";
    run(urlFor(base, "bearer", 10 * MiB), target, 4);
    expect(readFile(target) == expectedBytes(10 * MiB), "A file behind a token downloads with the right content.");
    const auto auth = authOn(base, "127.0.0.1");
    expect(auth.size() == stats(base)["requests"].get<size_t>(), "Every request to a host with a saved token carries it.");
    expect(std::all_of(auth.begin(), auth.end(), [](const std::string& value) {
               return value == "Bearer tok";
           }),
           "The token is sent as \"Bearer tok\".");
}

void redirectOut(const std::string& base) {
    SignIns table({tokenFor("127.0.0.1", "tok")});
    TempFolder folder("redirect-out");
    reset(base);
    const auto target = folder.path / "file.bin";
    run(urlFor(base, "redirect-out", 10 * MiB), target, 4);
    expect(readFile(target) == expectedBytes(10 * MiB), "A file redirected from a signed-in host to another host downloads with the right content.");
    expect(authOn(base, "localhost").empty(), "A token for 127.0.0.1 is never sent to localhost after a redirect.");
}

void redirectBack(const std::string& base) {
    SignIns table({tokenFor("127.0.0.1", "tok")});
    TempFolder folder("redirect-back");
    reset(base);
    const auto target = folder.path / "file.bin";
    run(urlFor(base, "redirect-back", 10 * MiB), target, 4);
    expect(readFile(target) == expectedBytes(10 * MiB), "A file redirected to a token-protected address on the same host downloads with the right content.");
}

void redirectLoop(const std::string& base) {
    SignIns table({tokenFor("127.0.0.1", "tok")});
    TempFolder folder("redirect-loop");
    reset(base);
    bool threw = false;
    try {
        run(urlFor(base, "redirect-loop", 10 * MiB), folder.path / "file.bin", 4);
    } catch (const archive::NetworkError& e) {
        threw = true;
        expect(!e.retry, "Too many redirects is not a retryable error.");
    }
    expect(threw, "A redirect loop from a signed-in host throws a NetworkError.");
    expect(stats(base)["requests"].get<int>() <= 11, "A redirect loop is given up after at most 11 requests.");
}

void forbidden(const std::string& base) {
    SignIns table({});
    TempFolder folder("forbidden");
    std::string message;
    try {
        run(urlFor(base, "forbidden", 10 * MiB), folder.path / "file.bin", 4);
    } catch (const archive::SignInNeeded&) {
        throw std::runtime_error("A 403 answer is not reported as SignInNeeded.");
    } catch (const std::exception& e) {
        message = e.what();
    }
    expect(message.starts_with("HTTP 403"), "A 403 answer fails with a message starting \"HTTP 403\", not \"" + message + "\".");
    expect(message == "HTTP 403: the server refused access to this file.", "A 403 answer says the server refused access, not \"" + message + "\".");
}

void nothingUnasked(const std::string& base) {
    SignIns table({tokenFor("localhost", "tok"), passwordFor("127.0.0.1", "user", "pass")});
    TempFolder folder("unasked");
    reset(base);
    const auto target = folder.path / "file.bin";
    run(urlFor(base, "etag", 10 * MiB), target, 4);
    expect(readFile(target) == expectedBytes(10 * MiB), "A file that needs no sign-in downloads with the right content while sign-ins are saved.");
    expect(stats(base)["auth"].empty(), "No sign-in is sent to a server that never asked for one.");
}

std::string base64Of(const std::string& text) {
    static const char* digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < text.size(); i += 3) {
        const size_t left = text.size() - i;
        const uint32_t n = (uint8_t(text[i]) << 16) | (left > 1 ? uint8_t(text[i + 1]) << 8 : 0) | (left > 2 ? uint8_t(text[i + 2]) : 0);
        out += digits[(n >> 18) & 63];
        out += digits[(n >> 12) & 63];
        out += left > 1 ? digits[(n >> 6) & 63] : '=';
        out += left > 2 ? digits[n & 63] : '=';
    }
    return out;
}

std::string percentEncoded(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~')
            out += char(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// Every file under folder (except skip) that contains one of the needles, as "file: needle".
std::vector<std::string> filesHolding(const archive::Path& folder, const archive::Path& skip, const std::vector<std::string>& needles) {
    std::vector<std::string> hits;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder)) {
        if (!entry.is_regular_file() || entry.path() == skip) continue;
        const auto text = readFile(entry.path());
        for (const auto& needle : needles)
            if (text.find(needle) != std::string::npos) hits.push_back(archive::pathUtf8(entry.path().filename()) + ": " + needle);
    }
    return hits;
}

// The forms a secret could take in a file: as it is, URL-encoded, and Base64 of it alone and with a user name in front.
std::vector<std::string> secretForms(const std::string& user, const std::string& secret) {
    std::vector<std::string> forms = {secret, percentEncoded(secret), base64Of(secret), base64Of(user + ":" + secret), base64Of(user + ":" + percentEncoded(secret))};
    if (user.empty()) forms.resize(3);
    forms.erase(std::unique(forms.begin(), forms.end()), forms.end());
    return forms;
}

// A signed-in download that is paused and then finished leaves its secret nowhere on disk: not in the staging folder,
// not in the log (read after the pause and again after the finish), not beside the file.
void noSecretOnDisk(const std::string& base) {
    struct Case {
        const char* mode;
        archive::SignIn signIn;
        std::string user, secret;
    };
    const Case cases[] = {{"bearer-secret", tokenFor("127.0.0.1", "tok-SECRET-123"), "", "tok-SECRET-123"},
                          {"basic-secret", passwordFor("127.0.0.1", "user", "pass-SECRET-456"), "user", "pass-SECRET-456"}};
    Log::setConsoleOutputEnabled(false);
    for (const auto& c : cases) {
        SignIns table({c.signIn});
        TempFolder folder(std::string("no-secret-") + c.mode);
        reset(base);
        const auto target = folder.path / "file.bin";
        const auto url = urlFor(base, c.mode, 10 * MiB);
        const auto forms = secretForms(c.user, c.secret);
        expect(Log::open(archive::pathUtf8(folder.path / "paused.log")), "The log opens in the test folder.");
        run(url, target, 4, 3 * MiB);
        Log::close();
        auto staging = target;
        staging += L".archive-parts";
        expect(std::filesystem::exists(staging), std::string("A paused download in mode ") + c.mode + " leaves its staging folder.");
        auto hits = filesHolding(staging, {}, forms);
        expect(hits.empty(), "The staging folder of a paused " + std::string(c.mode) + " download holds no secret, but " + (hits.empty() ? "" : hits[0]) + ".");
        hits = filesHolding(folder.path, {}, forms);
        expect(hits.empty(), "Nothing in the folder of a paused " + std::string(c.mode) + " download holds a secret, but " + (hits.empty() ? "" : hits[0]) + ".");
        expect(readFile(folder.path / "paused.log").find("Probe") != std::string::npos, "The log of the paused download has its probe line, so the search of it means something.");
        expect(Log::open(archive::pathUtf8(folder.path / "finished.log")), "The log opens again for the resumed download.");
        run(url, target, 4);
        Log::close();
        expect(readFile(target) == expectedBytes(10 * MiB), std::string("The resumed ") + c.mode + " download has the right content.");
        hits = filesHolding(folder.path, target, forms);
        expect(hits.empty(), "After a " + std::string(c.mode) + " download finishes, no file beside it holds a secret, but " + (hits.empty() ? "" : hits[0]) + ".");
    }
}

void loaderSignsIn(const std::string& base) {
    SignIns table({passwordFor("127.0.0.1", "user", "pass")});
    std::atomic_bool stop = false;
    expect(archive::fetch(urlFor(base, "basic", 1024), stop).file, "A loader's fetch from a Basic-protected address signs in and gets the file.");
}
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: engine_tests <server base URL>\n";
        return 2;
    }
    const std::string base = argv[1];
    try {
        ranges(base);
        withoutRanges(base);
        emptyFile(base);
        pauseAndResume(base);
        existingDestination(base);
        lastModifiedRanges(base);
        lastModifiedResume(base);
        weakEtagOnly(base);
        rangeProbeOnly(base);
        changedFile(base);
        singleDataFile(base);
        snapshotResume(base);
        oldLayout(base, true);
        oldLayout(base, false);
        oldLayoutLongPart(base);
        hostLimit(base);
        loadWatch(base);
        signInMissing(base);
        basicRanges(base);
        basicWrong(base);
        bearer(base);
        redirectOut(base);
        redirectBack(base);
        redirectLoop(base);
        forbidden(base);
        nothingUnasked(base);
        loaderSignsIn(base);
        noSecretOnDisk(base);
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
