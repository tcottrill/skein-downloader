// The file picker's job builder (src/wx/logic.cpp) without a window: results, the progress count and stopping.
#include "logic.h"
#include "platform/ftp.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
archive::Item sample(size_t count) {
    archive::Item item;
    item.id = "logic-test";
    item.mirrorBases = {"https://ia800001.us.archive.org/1/items/logic-test/"};
    for (size_t i = 0; i < count; ++i) {
        archive::File f;
        f.name = "f" + std::to_string(i) + ".zip";
        f.format = "ZIP";
        f.size = 10 + i;
        f.sizeKnown = true;
        item.files.push_back(std::move(f));
    }
    return item;
}
}

int main() {
    try {
        const auto root = std::filesystem::absolute(std::filesystem::temp_directory_path() / "skein-logic-test").lexically_normal();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        auto item = sample(5);
        std::vector<bool> checked = {true, false, true, true, false};

        // The folder does not exist yet: every checked file becomes a job, in order, with a lower-case key.
        auto jobs = lite::fileJobs(item, checked, root, 2, "standard");
        check(jobs.size() == 3 && jobs[0].name == "f0.zip" && jobs[1].name == "f2.zip" && jobs[2].name == "f3.zip", "jobs for the checked files only");
        check(jobs[0].input.at("target") == archive::pathUtf8(root / "logic-test" / "f0.zip") && jobs[0].input.at("connections") == 2, "job input");
        check(jobs[0].key == downloads::pathKey(root / "logic-test" / "f0.zip"), "job key");

        // Existing files are left out.
        std::filesystem::create_directories(root / "logic-test");
        std::ofstream(root / "logic-test" / "f2.zip") << "x";
        jobs = lite::fileJobs(item, checked, root, 2, "standard");
        check(jobs.size() == 2 && jobs[0].name == "f0.zip" && jobs[1].name == "f3.zip", "an existing file is skipped");

        // Progress counts the checked files examined, existing ones included.
        std::atomic_size_t progress{99};
        jobs = lite::fileJobs(item, checked, root, 2, "standard", &progress);
        check(progress == 3 && jobs.size() == 2, "progress ends at the number of checked files");

        // A stop flag that is already set cancels with the engine's word and no result.
        std::atomic_bool stop{true};
        bool paused = false;
        try {
            lite::fileJobs(item, checked, root, 2, "standard", &progress, &stop);
        } catch (const std::runtime_error& e) {
            paused = std::string(e.what()) == "Paused";
        }
        check(paused, "stop throws Paused");
        stop = false;
        check(lite::fileJobs(item, checked, root, 2, "standard", &progress, &stop).size() == 2, "a stop flag that is not set changes nothing");

        // A file loaded from an FTP link that carried a password: the item the loaders make has the address without it,
        // and nothing of it reaches the job input that history.json stores.
        {
            archive::Item ftpItem = sample(1);
            ftpItem.sourceUrl = archive::ftp::withoutUserInfo("ftp://bob:pw-SECRET-789@example.org/a.zip");
            ftpItem.files[0].url = archive::ftp::withoutUserInfo("ftp://bob:pw-SECRET-789@example.org/a.zip");
            check(ftpItem.sourceUrl == "ftp://example.org/a.zip", "the loaders' cleaning leaves the FTP address without user information");
            auto ftpJobs = lite::fileJobs(ftpItem, {true}, root, 2, "standard");
            check(ftpJobs.size() == 1, "an FTP item makes a job");
            const auto text = ftpJobs[0].input.dump();
            check(text.find("pw-SECRET-789") == std::string::npos && text.find("bob") == std::string::npos, "a job's stored input holds no FTP password or user");
        }

        // Extras go to the jobs whose download address is on the extras' own host and port, and to no other.
        {
            archive::Item mixed = sample(2);
            mixed.sourceUrl = "https://example.org/dir/";
            mixed.files[0].url = "https://example.org/dir/f0.zip";
            mixed.files[1].url = "https://other.org/dir/f1.zip";
            archive::RequestExtras extras;
            extras.cookie = "c=1";
            extras.headers.push_back({"X-A", "1"});
            const auto bound = archive::forLink("https://example.org/dir/f0.zip", extras);
            auto withExtras = lite::fileJobs(mixed, {true, true}, root, 2, "standard", nullptr, nullptr, &bound);
            check(withExtras.size() == 2, "extras do not change which jobs are made");
            check(archive::extrasFromJson(withExtras[0].extras).cookie == "c=1" && archive::extrasFromJson(withExtras[0].extras).host == "example.org", "a job on the extras' host carries them, bound to its address");
            check(withExtras[1].extras.is_object() && withExtras[1].extras.empty(), "a job on another host carries none");
            auto plain = lite::fileJobs(mixed, {true, true}, root, 2, "standard");
            check(plain[0].extras.is_object() && plain[0].extras.empty() && plain[1].extras.is_object() && plain[1].extras.empty(), "without the parameter no job carries extras");
            const auto otherPort = archive::forLink("https://example.org:8443/x", extras);
            check(lite::fileJobs(mixed, {true, true}, root, 2, "standard", nullptr, nullptr, &otherPort)[0].extras.empty(), "extras for another port of the same host are not given to the job");
        }

        // Names are validated for every file, also after the first.
        item.files[3].name = "../escape.zip";
        bool rejected = false;
        try {
            lite::fileJobs(item, checked, root, 2, "standard");
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        check(rejected, "an unsafe name later in the list is still rejected");

        std::filesystem::remove_all(root, ec);
    } catch (const std::exception& e) {
        std::cerr << "FAILED: unexpected exception: " << e.what() << '\n';
        ++failures;
    }
    if (failures) return 1;
    std::cout << "logic tests passed\n";
    return 0;
}
