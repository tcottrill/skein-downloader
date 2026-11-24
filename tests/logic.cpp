// The file picker's job builder (src/wx/logic.cpp) without a window: results, the progress count and stopping.
#include "logic.h"
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
