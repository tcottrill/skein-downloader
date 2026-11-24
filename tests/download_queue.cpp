#include "download_queue.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;

int main() {
    const auto folder = std::filesystem::temp_directory_path() /
        ("skein-queue-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto history = folder / "history.json";
    int result = 0;
    try {
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<uint64_t> started;
        bool releaseFirst = false;
        auto runner = [&](const downloads::Job& job, std::atomic_bool& stop, const downloads::Report&) {
            std::unique_lock lock(mutex);
            started.push_back(job.id);
            changed.notify_all();
            // Allow only one worker to dispatch the waiting jobs so their start order is observable.
            while (job.id <= 4 && !stop && !(job.id == 1 && releaseFirst)) {
                changed.wait_for(lock, 10ms);
            }
            if (stop) throw std::runtime_error("Paused"); // a stopped runner throws; returning means finished
        };

        {
            downloads::Queue queue(history, runner);
            for (int i = 1; i <= 8; ++i) {
                const auto name = std::to_string(i);
                queue.add(name, "Archive", name, nullptr);
            }
            queue.start();
            {
                std::unique_lock lock(mutex);
                if (!changed.wait_for(lock, 10s, [&] { return started.size() == 4; })) {
                    throw std::runtime_error("Four workers did not start");
                }
            }

            queue.pause(7);
            queue.moveToTop({8, 7, 2, 8, 99});
            const auto jobs = queue.snapshot();
            std::vector<uint64_t> order;
            for (const auto& job : jobs) order.push_back(job.id);
            if (order != std::vector<uint64_t>{1, 2, 3, 4, 7, 8, 5, 6} || jobs[4].state != downloads::State::Paused || jobs[1].state != downloads::State::Active) {
                throw std::runtime_error("Moving the selection did not preserve its order or job states");
            }
            queue.flush(); // history is written by its own thread
            {
                std::ifstream file(history);
                const auto saved = nlohmann::json::parse(file);
                if (saved.at(4).at("id") != 7 || saved.at(5).at("id") != 8) {
                    throw std::runtime_error("The move did not save the new queue order");
                }
            }

            queue.resume(7);
            std::unique_lock lock(mutex);
            releaseFirst = true;
            changed.notify_all();
            if (!changed.wait_for(lock, 10s, [&] { return started.size() == 8; }) ||
                std::vector<uint64_t>(started.begin() + 4, started.end()) != std::vector<uint64_t>{7, 8, 5, 6}) {
                throw std::runtime_error("The worker did not dispatch the promoted jobs first");
            }
        }

        {
            downloads::Queue reopened(history, runner);
            const auto jobs = reopened.snapshot();
            if (jobs.size() != 8 || jobs[4].id != 7 || jobs[5].id != 8) {
                throw std::runtime_error("The promoted order was not restored from history");
            }
        }

        // A pause that arrives after the runner has finished must not turn a saved file into a
        // Paused job (resuming it would fail because the file already exists).
        {
            const auto lateHistory = folder / "late-pause.json";
            std::atomic_bool finishing = false;
            auto lateRunner = [&](const downloads::Job&, std::atomic_bool& stop, const downloads::Report&) {
                finishing = true;
                while (!stop) std::this_thread::sleep_for(1ms);
            };
            downloads::Queue queue(lateHistory, lateRunner);
            queue.add("late", "Archive", "late", nullptr);
            queue.start();
            for (int i = 0; i < 10000 && !finishing; ++i) std::this_thread::sleep_for(1ms);
            const auto id = queue.snapshot().at(0).id;
            queue.pause(id);
            auto state = downloads::State::Active;
            for (int i = 0; i < 10000; ++i) {
                state = queue.snapshot().at(0).state;
                if (state != downloads::State::Active) break;
                std::this_thread::sleep_for(1ms);
            }
            if (state != downloads::State::Complete) throw std::runtime_error(std::string("A finished job was marked ") + downloads::stateName(state) + " after a late pause");
            queue.shutdown();
            std::ifstream file(lateHistory);
            if (nlohmann::json::parse(file).at(0).at("state") != "Complete")
                throw std::runtime_error("The completed state was not saved at shutdown");
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    return result;
}
