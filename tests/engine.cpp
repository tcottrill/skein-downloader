// End-to-end download engine tests against the local server in tests/engine_server.py,
// started by tests/run_engine_tests.py, which passes the server's base URL.
#include "sites.h"
#include "json.hpp"
#include <atomic>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

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

void ranges(const std::string& base) {
    TempFolder folder("ranges");
    reset(base);
    const auto target = folder.path / "file.bin";
    run(base + "/etag/" + std::to_string(10 * MiB) + "/file.bin", target, 4);
    expect(readFile(target) == expectedBytes(10 * MiB), "A 10 MiB file with a strong ETag downloads with the right content.");
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
    run(url, target, 4);
    expect(readFile(target) == expectedBytes(10 * MiB), "A resumed download has the right content.");
    auto staging = target;
    staging += L".archive-parts";
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
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
