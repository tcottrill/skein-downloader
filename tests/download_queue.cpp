#include "download_queue.h"
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;

int main() {
    const auto folder = std::filesystem::temp_directory_path() /
                        ("skein-queue-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto history = folder / "history.json";
    int result = 0;
    try {
        // States are saved by name; an unknown name (a newer version's state) reads as Paused.
        for (auto state : {downloads::State::Queued, downloads::State::Active, downloads::State::Paused, downloads::State::Failed, downloads::State::Complete})
            if (downloads::stateFromName(downloads::stateName(state)) != state) throw std::runtime_error("A state name does not read back");
        if (downloads::stateFromName("Bogus") != downloads::State::Paused) throw std::runtime_error("An unknown state name is not Paused");
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
                if (!changed.wait_for(lock, 10s, [&] {
                        return started.size() == 4;
                    })) {
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
            if (!changed.wait_for(lock, 10s, [&] {
                    return started.size() == 8;
                }) ||
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

        // Automatic retry: a temporary network failure puts the job back in the queue with a wait.
        {
            // One queue per case, with its own history file. The runner is given the run number
            // (1-based) and may throw; a case waits on its conditions with a timeout.
            std::mutex retryMutex;
            std::condition_variable retryChanged;
            int runs = 0;
            std::function<void(int, const downloads::Job&, const downloads::Report&)> behavior;
            auto retryRunner = [&](const downloads::Job& job, std::atomic_bool&, const downloads::Report& report) {
                int run;
                {
                    std::lock_guard lock(retryMutex);
                    run = ++runs;
                    retryChanged.notify_all();
                }
                behavior(run, job, report);
            };
            auto runCount = [&] {
                std::lock_guard lock(retryMutex);
                return runs;
            };
            auto waitRuns = [&](int count, std::chrono::milliseconds limit) {
                std::unique_lock lock(retryMutex);
                return retryChanged.wait_for(lock, limit, [&] {
                    return runs >= count;
                });
            };
            auto waitState = [](downloads::Queue& queue, downloads::State state, std::chrono::milliseconds limit) {
                const auto end = std::chrono::steady_clock::now() + limit;
                while (std::chrono::steady_clock::now() < end) {
                    if (queue.snapshot().at(0).state == state) return true;
                    std::this_thread::sleep_for(2ms);
                }
                return queue.snapshot().at(0).state == state;
            };
            int caseNumber = 0;
            auto begin = [&](const char*) {
                runs = 0;
                return folder / ("retry-" + std::to_string(++caseNumber) + ".json");
            };
            // A wait of 0 seconds says the server gave no Retry-After, so the schedule decides.
            const auto temporary = [] {
                return archive::NetworkError("x", true, 0);
            };

            { // a. Two temporary failures, then success.
                behavior = [&](int run, const downloads::Job&, const downloads::Report&) {
                    if (run <= 2) throw temporary();
                };
                downloads::Queue queue(begin("a"), retryRunner);
                queue.setRetryWaits({10ms}, 12);
                queue.add("a", "Archive", "a", nullptr);
                queue.start();
                if (!waitState(queue, downloads::State::Complete, 10s) || runCount() != 3)
                    throw std::runtime_error("A job that failed twice for network reasons did not complete on its third run");
            }
            { // b. A failure that is not worth retrying fails at once.
                behavior = [&](int, const downloads::Job&, const downloads::Report&) {
                    throw archive::NetworkError("x", false);
                };
                downloads::Queue queue(begin("b"), retryRunner);
                queue.setRetryWaits({10ms}, 12);
                queue.add("b", "Archive", "b", nullptr);
                queue.start();
                if (!waitState(queue, downloads::State::Failed, 10s) || queue.snapshot().at(0).detail != "x")
                    throw std::runtime_error("A failure marked not retryable did not fail with its text");
                std::this_thread::sleep_for(100ms);
                if (runCount() != 1) throw std::runtime_error("A failure marked not retryable was run again");
            }
            { // c. The retry limit.
                behavior = [&](int, const downloads::Job&, const downloads::Report&) {
                    throw temporary();
                };
                downloads::Queue queue(begin("c"), retryRunner);
                queue.setRetryWaits({10ms}, 3);
                queue.add("c", "Archive", "c", nullptr);
                queue.start();
                if (!waitState(queue, downloads::State::Failed, 10s))
                    throw std::runtime_error("A job that always failed did not end Failed after its retries");
                std::this_thread::sleep_for(100ms);
                if (runCount() != 4) throw std::runtime_error("A job with 3 retries did not run exactly 4 times");
            }
            { // d. A long wait is shown, pause cancels it, resume starts at once.
                behavior = [&](int, const downloads::Job&, const downloads::Report&) {
                    throw temporary();
                };
                downloads::Queue queue(begin("d"), retryRunner);
                queue.setRetryWaits({10s}, 12);
                queue.add("d", "Archive", "d", nullptr);
                queue.start();
                if (!waitRuns(1, 10s)) throw std::runtime_error("The job did not start");
                bool waiting = false;
                downloads::Job job;
                for (int i = 0; i < 2000 && !waiting; ++i) {
                    job = queue.snapshot().at(0);
                    waiting = job.state == downloads::State::Queued && job.retryIn != 0;
                    if (!waiting) std::this_thread::sleep_for(2ms);
                }
                if (!waiting || job.retryIn < 1 || job.retryIn > 10 || job.detail != "x")
                    throw std::runtime_error("A job waiting to retry was not Queued with a countdown and its error");
                queue.pause(job.id);
                job = queue.snapshot().at(0);
                if (job.state != downloads::State::Paused || job.retryIn != 0)
                    throw std::runtime_error("Pause did not cancel the wait");
                std::this_thread::sleep_for(200ms);
                if (runCount() != 1) throw std::runtime_error("A paused job was run again");
                queue.resume(job.id);
                if (!waitRuns(2, 1s)) throw std::runtime_error("Resume did not start the job at once");
            }
            { // e. Retry-After is a minimum.
                behavior = [&](int, const downloads::Job&, const downloads::Report&) {
                    throw archive::NetworkError("x", true, 2);
                };
                downloads::Queue queue(begin("e"), retryRunner);
                queue.setRetryWaits({10ms}, 12);
                queue.add("e", "Archive", "e", nullptr);
                queue.start();
                if (!waitRuns(1, 10s)) throw std::runtime_error("The job did not start");
                bool waiting = false;
                downloads::Job job;
                for (int i = 0; i < 2000 && !waiting; ++i) {
                    job = queue.snapshot().at(0);
                    waiting = job.retryIn != 0;
                    if (!waiting) std::this_thread::sleep_for(2ms);
                }
                if (!waiting || job.retryIn < 1) throw std::runtime_error("A Retry-After of 2 s was not honored over a 10 ms schedule");
            }
            { // f. Progress resets the count.
                behavior = [&](int run, const downloads::Job&, const downloads::Report& report) {
                    report(uint64_t(run), 0, "", "");
                    if (run <= 5) throw temporary();
                };
                downloads::Queue queue(begin("f"), retryRunner);
                queue.setRetryWaits({10ms}, 2);
                queue.add("f", "Archive", "f", nullptr);
                queue.start();
                if (!waitState(queue, downloads::State::Complete, 10s) || runCount() != 6)
                    throw std::runtime_error("Progress did not reset the retry count");
            }
            { // g. Removing a waiting job drops it for good.
                behavior = [&](int, const downloads::Job&, const downloads::Report&) {
                    throw temporary();
                };
                downloads::Queue queue(begin("g"), retryRunner);
                queue.setRetryWaits({10s}, 12);
                queue.add("g", "Archive", "g", nullptr);
                queue.start();
                if (!waitRuns(1, 10s)) throw std::runtime_error("The job did not start");
                uint64_t retryIn = 0;
                for (int i = 0; i < 2000 && !retryIn; ++i) {
                    retryIn = queue.snapshot().at(0).retryIn;
                    if (!retryIn) std::this_thread::sleep_for(2ms);
                }
                if (!retryIn) throw std::runtime_error("The job did not wait to retry");
                queue.remove(queue.snapshot().at(0).id);
                std::this_thread::sleep_for(200ms);
                if (!queue.snapshot().empty() || runCount() != 1)
                    throw std::runtime_error("A removed waiting job stayed or ran again");
            }
            { // h. Only NetworkError retries.
                behavior = [&](int, const downloads::Job&, const downloads::Report&) {
                    throw std::runtime_error("y");
                };
                downloads::Queue queue(begin("h"), retryRunner);
                queue.setRetryWaits({10ms}, 12);
                queue.add("h", "Archive", "h", nullptr);
                queue.start();
                if (!waitState(queue, downloads::State::Failed, 10s))
                    throw std::runtime_error("A plain exception did not fail the job");
                std::this_thread::sleep_for(100ms);
                if (runCount() != 1) throw std::runtime_error("A plain exception was retried");
            }
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    return result;
}
