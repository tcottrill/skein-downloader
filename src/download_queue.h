#pragma once
#include "core.h"
#include "json.hpp"
#include <chrono>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
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
    // A referer, cookies and headers for this job's link (archive::extrasToJson form), an empty object when
    // none. Unlike input it changes: the queue clears it when the job completes, and it is saved with the
    // job's small fields under the lock. Left out of rows(), which the window shows.
    nlohmann::json extras = nlohmann::json::object();
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
// The least time between the starts of two history file writes. A crash (or a power cut) can lose
// the queue changes of the last interval, up to this long: job states and the order of the list.
// It never loses downloaded data, which the engine checkpoints on its own.
inline constexpr std::chrono::milliseconds defaultSaveInterval{2000};
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

public:
    // The view tabs' counts: jobs listed (not History-only), running, waiting (queued, paused or
    // failed) and finished.
    struct Counts {
        size_t listed = 0, active = 0, waiting = 0, done = 0;
    };

private:
    // generation(): moved under mutex_ by every change it covers; editGeneration_ only by changes
    // made through the queue's calls (edit), not by the workers. active_ holds the entries that are
    // running or waiting to retry, so changing() never walks the whole list; counts_ is kept by the
    // same walk, which runs only when something changed.
    uint64_t generation_ = 0, editGeneration_ = 0;
    std::vector<std::shared_ptr<Entry>> active_;
    Counts counts_;
    void changedLocked(bool edit = true);
    std::vector<std::jthread> workers_;
    uint64_t next_ = 1;
    bool closing_ = false, historyReadable_ = true, pausedAll_ = false;
    std::string error_, maintenance_;
    std::vector<std::chrono::milliseconds> retryWaits_{std::chrono::seconds(5), std::chrono::seconds(15), std::chrono::seconds(60), std::chrono::seconds(300)};
    unsigned maxRetries_ = 12;
    unsigned limit_ = 4; // downloads running at once, 1 to 4
    // History is written by its own thread, outside mutex_: saveLocked() only marks it changed.
    // A write that takes long (a large history, a slow disk) then blocks neither the window nor
    // the downloads, and changes made meanwhile are saved together in the next write.
    // After shutdown the thread has ended (saverDone_) and saveLocked() writes directly.
    // File writes start at least saveInterval_ apart: changes made meanwhile are gathered and written
    // together. flush() (flushNow_) and shutdown() (saverStop_) end the wait.
    std::condition_variable saveCv_;
    bool dirty_ = false, saving_ = false, saverStop_ = false, saverDone_ = false, flushNow_ = false;
    std::optional<std::chrono::steady_clock::time_point> lastWriteStart_;
    std::chrono::milliseconds saveInterval_ = defaultSaveInterval;
    std::atomic<uint64_t> writes_{0};
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
    // The least time between the starts of two history file writes (default defaultSaveInterval).
    void setSaveInterval(std::chrono::milliseconds interval);
    // The number of history file writes completed so far.
    uint64_t historyWrites() const;
    // How many downloads may run at once, clamped to 1 to 4 (default 4). Callable at any time from
    // any thread; lowering it never stops a running job, new ones just wait until fewer are running.
    void setLimit(unsigned jobs);
    void shutdown();
    void pause(uint64_t id);
    void resume(uint64_t id);
    void remove(uint64_t id);
    void moveToTop(const std::vector<uint64_t>& ids);
    std::vector<Job> snapshot() const;
    // A counter that moves on every change of the set of jobs, their order, or a job's state,
    // name, historyOnly flag or retry wait; byte progress (done, total, detail, output of a running
    // job) does not move it. A window takes a new snapshot() only when it moved.
    uint64_t generation() const;
    // The jobs whose shown values change between generations: those running and those waiting to
    // retry (retryIn filled as by snapshot()), in queue order. Costs nothing per job that is idle.
    std::vector<Job> changing() const;
    // Moves on changes made through the queue's calls (add, remove, pause, resume, move to top and
    // the others), not when a worker starts or ends a job or schedules its retry. A window shows the
    // first kind at once and may gather the second.
    uint64_t editGeneration() const;
    // snapshot() without each job's input and key (left empty), which are most of its cost.
    std::vector<Job> rows() const;
    // One full job, as snapshot() has it; none for an unknown or removed id.
    std::optional<Job> job(uint64_t id) const;
    // The counts as of the last change; no walk over the jobs.
    Counts counts() const;
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
    // Benchmark only (--bench-files): sets a job's state as a worker starting it (Active) or ending
    // it would, without running anything, so the window's handling of job turnover can be timed
    // without a network. Use it only on a paused queue.
    void benchSetState(uint64_t id, State state);
};
}
