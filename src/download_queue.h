#pragma once
#include "core.h"
#include "json.hpp"
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
namespace downloads {
enum class State { Queued,
                   Active,
                   Paused,
                   Failed,
                   Complete };
// The English name saved in the history file, logged, and translated for display.
const char* stateName(State);
// The state for a saved name; an unknown name reads as Paused.
State stateFromName(const std::string&);
struct Job {
    uint64_t id = 0, done = 0, total = 0;
    std::string name, kind, key, detail, output;
    State state = State::Queued;
    nlohmann::json input;
    bool historyOnly = false;
    // Seconds until an automatic retry, 0 when none; filled by Queue::snapshot(), never saved.
    uint64_t retryIn = 0;
    bool canMoveToTop() const { return state == State::Queued || state == State::Paused; }
};
// Duplicate-detection key for a destination path; case-insensitive on Windows,
// matching keys saved by earlier versions.
std::string pathKey(const archive::Path& path);
using Report = std::function<void(uint64_t, uint64_t, const std::string&, const std::string&)>;
// Runs one job. Returning means the job is finished (state Complete); a runner that stops
// early, paused or failed, throws. The stop flag only says which of the two a throw was.
using Runner = std::function<void(const Job&, std::atomic_bool&, const Report&)>;
class Queue {
    struct Entry {
        Job job;
        std::atomic_bool stop = false;
        bool running = false, removing = false, resumeRequested = false;
        // Automatic retry: runs in a row without progress, the earliest start (a default time_point
        // means none), and the job's done value when the current run began. Never saved.
        unsigned retries = 0;
        std::chrono::steady_clock::time_point notBefore{};
        uint64_t doneAtStart = 0;
    };
    archive::Path path_;
    Runner runner_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::shared_ptr<Entry>> jobs_;
    std::vector<std::jthread> workers_;
    uint64_t next_ = 1;
    bool closing_ = false, historyReadable_ = true, pausedAll_ = false;
    std::string error_, maintenance_;
    std::vector<std::chrono::milliseconds> retryWaits_{std::chrono::seconds(5), std::chrono::seconds(15), std::chrono::seconds(60), std::chrono::seconds(300)};
    unsigned maxRetries_ = 12;
    // History is written by its own thread, outside mutex_: saveLocked() only marks it changed.
    // A write that takes long (a large history, a slow disk) then blocks neither the window nor
    // the downloads, and changes made meanwhile are saved together in the next write.
    // After shutdown the thread has ended (saverDone_) and saveLocked() writes directly.
    std::condition_variable saveCv_;
    bool dirty_ = false, saving_ = false, saverStop_ = false, saverDone_ = false;
    std::jthread saver_;
    void saveLocked();
    nlohmann::json historyLocked() const;
    void storeHistory(const nlohmann::json&);
    void saveLoop();
    void removeWaitingLocked();
    void work();

public:
    Queue(archive::Path path, Runner runner);
    ~Queue();
    bool add(std::string name, std::string kind, std::string key, nlohmann::json input);
    size_t addBatch(std::vector<Job> jobs);
    void start();
    // Waits before each automatic retry of a job that failed for a temporary network reason (the
    // last one repeats) and how many retries in a row without progress are allowed.
    void setRetryWaits(std::vector<std::chrono::milliseconds> waits, unsigned maxRetries);
    void shutdown();
    void pause(uint64_t id);
    void resume(uint64_t id);
    void remove(uint64_t id);
    void moveToTop(const std::vector<uint64_t>& ids);
    std::vector<Job> snapshot() const;
    std::string error() const;
    // Waits until every change made so far is in the history file (or failed to save; see error()).
    void flush();
    bool beginMaintenance(const std::string& kind);
    void endMaintenance();
    void pauseAll();
    void resumeAll();
    void removeAll();
    void removeWaiting();
    bool paused() const;
    bool downloadAgain(uint64_t id);
    void clearHistory();
    void removeHistory(uint64_t id);
};
}
