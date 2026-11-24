#include "download_queue.h"
#include "signin.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <unordered_set>

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
            { // b2. A missing sign-in fails the job at once, naming the host; it is not run again and no window is involved.
                behavior = [&](int, const downloads::Job&, const downloads::Report&) {
                    throw archive::SignInNeeded("example.org", false, true);
                };
                downloads::Queue queue(begin("b2"), retryRunner);
                queue.setRetryWaits({10ms}, 12);
                queue.add("b2", "Archive", "b2", nullptr);
                queue.start();
                if (!waitState(queue, downloads::State::Failed, 10s) || queue.snapshot().at(0).detail != "Sign-in needed for example.org. Add it in Settings, then resume.")
                    throw std::runtime_error("A missing sign-in did not fail the job with the sentence naming the host");
                std::this_thread::sleep_for(100ms);
                if (runCount() != 1) throw std::runtime_error("A job that needed a sign-in was run again");
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
            { // e2. A huge Retry-After is capped at one hour.
                behavior = [&](int, const downloads::Job&, const downloads::Report&) {
                    throw archive::NetworkError("x", true, 7200);
                };
                downloads::Queue queue(begin("e2"), retryRunner);
                queue.setRetryWaits({10ms}, 12);
                queue.add("e2", "Archive", "e2", nullptr);
                queue.start();
                if (!waitRuns(1, 10s)) throw std::runtime_error("The job did not start");
                bool waiting = false;
                downloads::Job job;
                for (int i = 0; i < 2000 && !waiting; ++i) {
                    job = queue.snapshot().at(0);
                    waiting = job.retryIn != 0;
                    if (!waiting) std::this_thread::sleep_for(2ms);
                }
                if (!waiting || job.state != downloads::State::Queued || job.retryIn <= 3590 || job.retryIn > 3600)
                    throw std::runtime_error("A Retry-After of 7200 s was not capped at one hour");
                queue.pause(job.id);
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

        // Downloads-at-once limit: the runner blocks each job until the test releases it by id.
        {
            std::mutex limitMutex;
            std::condition_variable limitChanged;
            std::vector<uint64_t> limitStarted;
            std::unordered_set<uint64_t> released;
            auto limitRunner = [&](const downloads::Job& job, std::atomic_bool& stop, const downloads::Report&) {
                std::unique_lock lock(limitMutex);
                limitStarted.push_back(job.id);
                limitChanged.notify_all();
                while (!stop && !released.contains(job.id)) limitChanged.wait_for(lock, 10ms);
                if (stop) throw std::runtime_error("Paused");
            };
            auto startedCount = [&] {
                std::lock_guard lock(limitMutex);
                return limitStarted.size();
            };
            auto waitStarted = [&](size_t count, std::chrono::milliseconds limit) {
                std::unique_lock lock(limitMutex);
                return limitChanged.wait_for(lock, limit, [&] {
                    return limitStarted.size() >= count;
                });
            };
            auto release = [&](uint64_t id) {
                std::lock_guard lock(limitMutex);
                released.insert(id);
                limitChanged.notify_all();
            };
            auto running = [](downloads::Queue& queue) {
                size_t count = 0;
                for (const auto& job : queue.snapshot()) count += job.state == downloads::State::Active;
                return count;
            };
            auto waitRunning = [&](downloads::Queue& queue, size_t count, std::chrono::milliseconds limit) {
                const auto end = std::chrono::steady_clock::now() + limit;
                while (running(queue) != count && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(2ms);
                return running(queue) == count;
            };
            { // a to c. A limit set before start, raised, then lowered while full.
                downloads::Queue queue(folder / "limit-1.json", limitRunner);
                for (int i = 1; i <= 4; ++i) queue.add(std::to_string(i), "Archive", std::to_string(i), nullptr);
                queue.setLimit(2);
                queue.start();
                std::this_thread::sleep_for(300ms);
                if (startedCount() != 2) throw std::runtime_error("A limit of 2 did not hold the number of started jobs at 2");
                queue.setLimit(3);
                if (!waitStarted(3, 1s)) throw std::runtime_error("Raising the limit to 3 did not start a third job");
                std::this_thread::sleep_for(100ms);
                if (startedCount() != 3) throw std::runtime_error("A limit of 3 started more than 3 jobs");
                queue.setLimit(1);
                release(1);
                if (!waitRunning(queue, 2, 1s)) throw std::runtime_error("Releasing a job did not leave 2 running");
                std::this_thread::sleep_for(200ms);
                if (startedCount() != 3 || running(queue) != 2) throw std::runtime_error("Lowering the limit to 1 still started a job while 2 were running");
                release(2);
                if (!waitRunning(queue, 1, 1s)) throw std::runtime_error("Releasing a second job did not leave 1 running");
                std::this_thread::sleep_for(200ms);
                if (startedCount() != 3 || running(queue) != 1) throw std::runtime_error("Lowering the limit to 1 still started a job while 1 was running");
                release(3);
                if (!waitStarted(4, 1s)) throw std::runtime_error("Releasing the last running job did not start the fourth");
            }
            { // d. Out-of-range limits are clamped to 1 and 4.
                limitStarted.clear();
                released.clear();
                downloads::Queue queue(folder / "limit-2.json", limitRunner);
                for (int i = 1; i <= 6; ++i) queue.add(std::to_string(i), "Archive", std::to_string(i), nullptr);
                queue.setLimit(0);
                queue.start();
                std::this_thread::sleep_for(300ms);
                if (startedCount() != 1) throw std::runtime_error("A limit of 0 did not behave as 1");
                queue.setLimit(9);
                if (!waitStarted(4, 1s)) throw std::runtime_error("A limit of 9 did not allow 4 jobs");
                std::this_thread::sleep_for(300ms);
                if (startedCount() != 4) throw std::runtime_error("A limit of 9 did not behave as 4");
            }
            { // setLimit is safe after shutdown.
                downloads::Queue queue(folder / "limit-3.json", limitRunner);
                queue.start();
                queue.shutdown();
                queue.setLimit(2);
            }
        }

        // Change tracking for the main window: generation() moves on every change of the job set,
        // order, states, names, the History flag or a retry wait, never on byte progress; changing()
        // lists exactly the running jobs and those waiting to retry.
        {
            // The runner reports growing progress until the test tells its job to finish or to fail
            // for a temporary network reason; a stopped runner throws.
            enum class Next { Run,
                              Finish,
                              Fail };
            std::mutex genMutex;
            std::condition_variable genChanged;
            std::map<uint64_t, Next> next;
            std::vector<uint64_t> genStarted;
            auto genRunner = [&](const downloads::Job& job, std::atomic_bool& stop, const downloads::Report& report) {
                {
                    std::lock_guard lock(genMutex);
                    genStarted.push_back(job.id);
                    genChanged.notify_all();
                }
                for (uint64_t done = 1;; ++done) {
                    report(done, 1000000, "Downloading", "");
                    std::unique_lock lock(genMutex);
                    if (stop) throw std::runtime_error("Paused");
                    if (next[job.id] == Next::Finish) return;
                    if (next[job.id] == Next::Fail) {
                        next[job.id] = Next::Run;
                        throw archive::NetworkError("x", true, 0);
                    }
                    genChanged.wait_for(lock, 1ms);
                }
            };
            auto tell = [&](uint64_t id, Next what) {
                std::lock_guard lock(genMutex);
                next[id] = what;
                genChanged.notify_all();
            };
            auto waitStarted = [&](size_t count) {
                std::unique_lock lock(genMutex);
                return genChanged.wait_for(lock, 10s, [&] {
                    return genStarted.size() >= count;
                });
            };
            auto job = [](downloads::Queue& queue, uint64_t id) {
                for (auto& j : queue.snapshot())
                    if (j.id == id) return j;
                throw std::runtime_error("A job is missing from the snapshot");
            };
            auto waitFor = [](const std::function<bool()>& condition) {
                const auto end = std::chrono::steady_clock::now() + 10s;
                while (!condition() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(2ms);
                return condition();
            };
            auto changingIds = [](downloads::Queue& queue) {
                std::vector<uint64_t> ids;
                for (auto& j : queue.changing()) ids.push_back(j.id);
                std::sort(ids.begin(), ids.end());
                return ids;
            };
            downloads::Queue queue(folder / "generation.json", genRunner);
            queue.setRetryWaits({10s}, 12);
            queue.setLimit(1);
            auto generation = queue.generation();
            auto advanced = [&] {
                const auto now = queue.generation();
                const bool moved = now != generation;
                generation = now;
                return moved;
            };
            queue.add("a", "Archive", "a", nullptr); // id 1
            if (!advanced()) throw std::runtime_error("Adding a job did not change the generation");
            queue.add("b", "Archive", "b", nullptr); // id 2
            queue.add("c", "Archive", "c", nullptr); // id 3
            advanced();
            if (!queue.changing().empty()) throw std::runtime_error("Jobs that are only queued were listed as changing");
            queue.start();
            if (!waitStarted(1) || !waitFor([&] {
                    return job(queue, 1).state == downloads::State::Active;
                }))
                throw std::runtime_error("The first job did not start");
            if (!advanced()) throw std::runtime_error("A job starting did not change the generation");
            const auto before = job(queue, 1).done;
            if (!waitFor([&] {
                    return job(queue, 1).done > before + 5;
                }))
                throw std::runtime_error("The runner did not report progress");
            if (advanced()) throw std::runtime_error("Progress reports changed the generation");
            const auto moving = queue.changing();
            if (moving.size() != 1 || moving[0].id != 1 || moving[0].state != downloads::State::Active || moving[0].done == 0 || moving[0].detail != "Downloading")
                throw std::runtime_error("changing() did not list exactly the running job with its progress");
            queue.pause(3);
            if (!advanced()) throw std::runtime_error("Pausing a job did not change the generation");
            queue.resume(3);
            if (!advanced()) throw std::runtime_error("Resuming a job did not change the generation");
            queue.moveToTop({3});
            if (!advanced()) throw std::runtime_error("Moving a job to the top did not change the generation");
            queue.remove(2);
            if (!advanced()) throw std::runtime_error("Removing a job did not change the generation");
            tell(1, Next::Finish);
            if (!waitFor([&] {
                    return job(queue, 1).state == downloads::State::Complete;
                }))
                throw std::runtime_error("The first job did not finish");
            if (!advanced()) throw std::runtime_error("A job finishing did not change the generation");
            // The limit of 1 now starts job 3; it fails for a temporary reason and waits 10 s.
            if (!waitStarted(2) || !waitFor([&] {
                    return job(queue, 3).state == downloads::State::Active;
                }))
                throw std::runtime_error("The third job did not start");
            advanced();
            if (changingIds(queue) != std::vector<uint64_t>{3}) throw std::runtime_error("changing() did not list exactly the running job after the first finished");
            tell(3, Next::Fail);
            if (!waitFor([&] {
                    return job(queue, 3).retryIn != 0;
                }))
                throw std::runtime_error("The third job did not wait to retry");
            if (!advanced()) throw std::runtime_error("A scheduled retry did not change the generation");
            const auto waiting = queue.changing();
            if (waiting.size() != 1 || waiting[0].id != 3 || waiting[0].state != downloads::State::Queued || waiting[0].retryIn < 1 || waiting[0].retryIn > 10)
                throw std::runtime_error("changing() did not list the job waiting to retry with its countdown");
            queue.pause(3);
            if (!advanced()) throw std::runtime_error("Pausing a job waiting to retry did not change the generation");
            if (!queue.changing().empty()) throw std::runtime_error("changing() listed a paused or finished job");
        }
        // For a window with many jobs: rows() is snapshot() without each job's input and key, job(id) is one
        // full job, counts() are the view tabs' counts kept as the queue changes, and editGeneration() moves on
        // changes made through the queue's calls but not when a worker starts or ends a job or schedules a retry.
        {
            enum class Next { Run,
                              Finish,
                              Fail };
            std::mutex rowMutex;
            std::map<uint64_t, Next> next;
            auto rowRunner = [&](const downloads::Job& job, std::atomic_bool& stop, const downloads::Report& report) {
                for (uint64_t done = 1;; ++done) {
                    report(done, 1000000, "Downloading", "");
                    std::unique_lock lock(rowMutex);
                    if (stop) throw std::runtime_error("Paused");
                    if (next[job.id] == Next::Finish) return;
                    if (next[job.id] == Next::Fail) {
                        next[job.id] = Next::Run;
                        throw archive::NetworkError("x", true, 0);
                    }
                    lock.unlock();
                    std::this_thread::sleep_for(1ms);
                }
            };
            auto tell = [&](uint64_t id, Next what) {
                std::lock_guard lock(rowMutex);
                next[id] = what;
            };
            auto waitFor = [](const std::function<bool()>& condition) {
                const auto end = std::chrono::steady_clock::now() + 10s;
                while (!condition() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(2ms);
                return condition();
            };
            downloads::Queue queue(folder / "rows.json", rowRunner);
            queue.setRetryWaits({10s}, 12);
            queue.setLimit(1);
            auto state = [&](uint64_t id) {
                auto j = queue.job(id);
                return j ? j->state : downloads::State::Paused;
            };
            auto countsMatch = [&] {
                downloads::Queue::Counts expected;
                for (auto& j : queue.snapshot()) {
                    expected.listed += !j.historyOnly;
                    expected.active += j.state == downloads::State::Active;
                    expected.waiting += j.state == downloads::State::Queued || j.state == downloads::State::Paused || j.state == downloads::State::Failed;
                    expected.done += j.state == downloads::State::Complete;
                }
                const auto counts = queue.counts();
                return counts.listed == expected.listed && counts.active == expected.active && counts.waiting == expected.waiting && counts.done == expected.done;
            };
            auto edits = queue.editGeneration();
            auto edited = [&] {
                const auto now = queue.editGeneration();
                const bool moved = now != edits;
                edits = now;
                return moved;
            };
            queue.add("a", "Archive", "key-a", {{"target", "/a"}}); // id 1
            if (!edited()) throw std::runtime_error("Adding a job did not change the edit generation");
            queue.add("b", "Archive", "key-b", {{"target", "/b"}}); // id 2
            queue.add("c", "Archive", "key-c", {{"target", "/c"}}); // id 3
            edited();
            if (!countsMatch() || queue.counts().waiting != 3) throw std::runtime_error("The counts did not match three queued jobs");
            {
                const auto rows = queue.rows();
                const auto full = queue.snapshot();
                if (rows.size() != 3 || rows[1].id != 2 || rows[1].name != "b" || rows[1].kind != "Archive" || rows[1].state != downloads::State::Queued || !rows[1].input.is_null() || !rows[1].key.empty())
                    throw std::runtime_error("rows() did not list the jobs without their input and key");
                if (full[1].input.at("target") != "/b") throw std::runtime_error("snapshot() lost a job's input");
            }
            {
                const auto one = queue.job(2);
                if (!one || one->name != "b" || one->key != "key-b" || one->input.at("target") != "/b") throw std::runtime_error("job(id) did not return the full job");
                if (queue.job(99)) throw std::runtime_error("job(id) returned a job for an unknown id");
            }
            queue.start();
            if (!waitFor([&] {
                    return state(1) == downloads::State::Active;
                }))
                throw std::runtime_error("The first job did not start");
            if (edited()) throw std::runtime_error("A job starting changed the edit generation");
            if (!countsMatch() || queue.counts().active != 1) throw std::runtime_error("The counts did not follow a job starting");
            queue.pause(3);
            if (!edited()) throw std::runtime_error("Pausing a job did not change the edit generation");
            queue.resume(3);
            if (!edited()) throw std::runtime_error("Resuming a job did not change the edit generation");
            queue.moveToTop({3});
            if (!edited()) throw std::runtime_error("Moving a job to the top did not change the edit generation");
            queue.remove(2);
            if (!edited()) throw std::runtime_error("Removing a job did not change the edit generation");
            if (!countsMatch() || queue.counts().waiting != 1) throw std::runtime_error("The counts did not follow a removal");
            tell(1, Next::Finish);
            if (!waitFor([&] {
                    return state(1) == downloads::State::Complete && state(3) == downloads::State::Active;
                }))
                throw std::runtime_error("The first job did not finish and hand over to the third");
            if (edited()) throw std::runtime_error("A job finishing changed the edit generation");
            if (!countsMatch() || queue.counts().done != 1) throw std::runtime_error("The counts did not follow a job finishing");
            tell(3, Next::Fail);
            if (!waitFor([&] {
                    auto j = queue.job(3);
                    return j && j->state == downloads::State::Queued && j->retryIn != 0;
                }))
                throw std::runtime_error("The third job did not wait to retry");
            if (edited()) throw std::runtime_error("A scheduled retry changed the edit generation");
            {
                const auto rows = queue.rows(); // job 1 (finished), then job 3, moved above job 2 before it was removed
                if (rows.size() != 2 || rows[1].id != 3 || rows[1].retryIn == 0) throw std::runtime_error("rows() did not fill the retry countdown");
            }
            queue.remove(1); // a finished job leaves All but stays in History
            if (!edited() || !countsMatch() || queue.counts().listed != 1 || queue.counts().done != 1) throw std::runtime_error("The counts did not follow a finished job leaving the list");
            queue.pause(3);
        }
        // History writes start at least one save interval apart; flush() and shutdown() write at once. The
        // file is written without indentation, and files written with it (older versions) still load.
        {
            const auto waitFor = [](const std::function<bool()>& condition) {
                const auto end = std::chrono::steady_clock::now() + 10s;
                while (!condition() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(2ms);
                return condition();
            };
            const auto saved = [](const std::filesystem::path& file) {
                std::ifstream in(file);
                nlohmann::json data = nlohmann::json::parse(in, nullptr, false);
                return data.is_array() ? data.size() : size_t(0);
            };
            const auto idle = [](const downloads::Job&, std::atomic_bool&, const downloads::Report&) {
            };
            const auto add = [](downloads::Queue& queue, int from, int to) {
                for (int i = from; i <= to; ++i) queue.add(std::to_string(i), "Archive", std::to_string(i), nullptr);
            };
            {
                const auto file = folder / "throttle.json";
                downloads::Queue queue(file, idle);
                queue.setSaveInterval(200ms);
                add(queue, 1, 20); // within a few milliseconds
                if (!waitFor([&] {
                        return saved(file) == 20;
                    }))
                    throw std::runtime_error("The last of 20 quick changes was not saved within the wait");
                if (queue.historyWrites() > 2) throw std::runtime_error("20 quick changes were written in more than 2 file writes");
                const auto writes = queue.historyWrites();
                std::this_thread::sleep_for(450ms);
                if (queue.historyWrites() != writes) throw std::runtime_error("The history was written again with nothing changed");
            }
            {
                const auto file = folder / "flush.json";
                downloads::Queue queue(file, idle);
                queue.setSaveInterval(10s);
                add(queue, 1, 1);
                if (!waitFor([&] {
                        return queue.historyWrites() == 1;
                    }))
                    throw std::runtime_error("The first change was not written at once");
                add(queue, 2, 3);
                const auto start = std::chrono::steady_clock::now();
                queue.flush();
                if (std::chrono::steady_clock::now() - start > 5s || saved(file) != 3 || queue.historyWrites() != 2)
                    throw std::runtime_error("flush() did not write inside the save interval at once");
                queue.flush(); // nothing changed: no wait, no write
                if (queue.historyWrites() != 2) throw std::runtime_error("flush() wrote a history that was already saved");
            }
            {
                const auto file = folder / "shutdown.json";
                downloads::Queue queue(file, idle);
                queue.setSaveInterval(10s);
                add(queue, 1, 1);
                if (!waitFor([&] {
                        return queue.historyWrites() == 1;
                    }))
                    throw std::runtime_error("The first change was not written at once before the shutdown");
                add(queue, 2, 4);
                const auto start = std::chrono::steady_clock::now();
                queue.shutdown();
                if (std::chrono::steady_clock::now() - start > 5s || saved(file) != 4)
                    throw std::runtime_error("shutdown() did not leave the final state on disk at once");
            }
            {
                // The format stays compatible both ways.
                const auto file = folder / "format.json";
                {
                    downloads::Queue queue(file, idle);
                    add(queue, 1, 3);
                    queue.flush();
                }
                std::ifstream in(file);
                const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (text.empty() || text.front() != '[' || text.find('\n') != std::string::npos || text.find("  ") != std::string::npos)
                    throw std::runtime_error("The history file was not written without indentation");
                downloads::Queue reread(file, idle);
                if (reread.snapshot().size() != 3 || !reread.error().empty()) throw std::runtime_error("A history written without indentation did not load");
                const auto old = folder / "indented.json";
                nlohmann::json data = nlohmann::json::array();
                for (int i = 1; i <= 2; ++i)
                    data.push_back({{"id", i}, {"name", "old" + std::to_string(i)}, {"kind", "Archive"}, {"key", "old" + std::to_string(i)}, {"input", nullptr}, {"state", "Complete"}, {"done", 5}, {"total", 5}, {"detail", "Finished"}, {"output", ""}, {"historyOnly", false}});
                {
                    std::ofstream out(old);
                    out << data.dump(2);
                }
                downloads::Queue legacy(old, idle);
                const auto jobs = legacy.snapshot();
                if (jobs.size() != 2 || jobs[1].name != "old2" || jobs[0].state != downloads::State::Complete || !legacy.error().empty())
                    throw std::runtime_error("A history written with indentation did not load");
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
