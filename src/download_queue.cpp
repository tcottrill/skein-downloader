#include "download_queue.h"
#include "platform/files.h"
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <unordered_set>
namespace downloads {
const char* stateName(State state) {
    switch (state) {
    case State::Queued: return "Queued";
    case State::Active: return "Active";
    case State::Paused: return "Paused";
    case State::Failed: return "Failed";
    case State::Complete: return "Complete";
    }
    return "Paused";
}
State stateFromName(const std::string& name) {
    for (auto state : {State::Queued, State::Active, State::Paused, State::Failed, State::Complete})
        if (name == stateName(state)) return state;
    return State::Paused;
}
std::string pathKey(const archive::Path& path) {
    auto normal = path.lexically_normal();
#ifdef _WIN32
    auto key = normal.wstring();
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) {
        return (wchar_t)std::towlower(c);
    });
    return archive::utf8(key);
#else
    return archive::pathUtf8(normal);
#endif
}
Queue::Queue(archive::Path path, Runner runner)
    : path_(std::move(path)), runner_(std::move(runner)) {
    // Started first, so the early return below cannot skip it; it waits until something changes.
    saver_ = std::jthread([this] {
        saveLoop();
    });
    try {
        if (!std::filesystem::exists(path_)) return;
        std::ifstream in(path_);
        nlohmann::json data;
        in >> data;
        if (!data.is_array()) throw std::runtime_error("Invalid history format");
        for (auto& j : data) {
            auto e = std::make_shared<Entry>();
            auto& v = e->job;
            v.id = j.at("id").get<uint64_t>();
            v.name = j.at("name");
            v.kind = j.at("kind");
            v.key = j.at("key");
            v.input = j.at("input");
            v.state = stateFromName(j.value("state", "Paused"));
            v.done = j.value("done", uint64_t(0));
            v.total = j.value("total", uint64_t(0));
            v.output = j.value("output", "");
            v.detail = j.value("detail", "");
            if (v.state != State::Complete && v.state != State::Failed) v.state = State::Paused;
            v.historyOnly = v.state == State::Complete && j.value("historyOnly", false);
            next_ = std::max(next_, v.id + 1);
            jobs_.push_back(e);
        }
    } catch (const std::exception& e) {
        jobs_.clear();
        historyReadable_ = false;
        error_ = std::string("History could not be read; original preserved. New downloads cannot be saved: ") + e.what();
    }
}
Queue::~Queue() {
    shutdown();
}
nlohmann::json Queue::historyLocked() const {
    nlohmann::json data = nlohmann::json::array();
    for (auto& e : jobs_) {
        if (e->removing) continue;
        auto& j = e->job;
        data.push_back({{"id", j.id}, {"name", j.name}, {"kind", j.kind}, {"key", j.key}, {"input", j.input}, {"state", stateName(j.state)}, {"done", j.done}, {"total", j.total}, {"detail", j.detail}, {"output", j.output}, {"historyOnly", j.historyOnly}});
    }
    return data;
}
// Touches only the file, so it runs without mutex_; one writer at a time (the saver thread, or
// saveLocked() once that thread has ended).
void Queue::storeHistory(const nlohmann::json& data) {
    std::filesystem::create_directories(path_.parent_path());
    auto temp = path_;
    temp += L".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << data.dump(2);
        out.flush();
        if (!out) throw std::runtime_error("Cannot write history file");
    }
    platform::replace(temp, path_);
}
void Queue::saveLocked() {
    if (!historyReadable_) return;
    if (saverDone_) {
        try {
            storeHistory(historyLocked());
            error_.clear();
        } catch (const std::exception& e) {
            error_ = std::string("History not saved: ") + e.what();
        }
        return;
    }
    dirty_ = true;
    saveCv_.notify_all();
}
void Queue::saveLoop() {
    std::unique_lock lock(mutex_);
    for (;;) {
        saveCv_.wait(lock, [this] {
            return dirty_ || saverStop_;
        });
        if (!dirty_) {
            saverDone_ = true;
            saveCv_.notify_all();
            return;
        }
        // The copy is made under the lock; the slow part (writing and flushing it to disk) is not.
        auto data = historyLocked();
        dirty_ = false;
        saving_ = true;
        lock.unlock();
        std::string failure;
        try {
            storeHistory(data);
        } catch (const std::exception& e) {
            failure = std::string("History not saved: ") + e.what();
        }
        lock.lock();
        saving_ = false;
        error_ = failure;
        saveCv_.notify_all();
    }
}
void Queue::flush() {
    std::unique_lock lock(mutex_);
    saveCv_.wait(lock, [this] {
        return saverDone_ || (!dirty_ && !saving_);
    });
}
bool Queue::add(std::string name, std::string kind, std::string key, nlohmann::json input) {
    Job job;
    job.name = std::move(name);
    job.kind = std::move(kind);
    job.key = std::move(key);
    job.input = std::move(input);
    return addBatch({std::move(job)}) == 1;
}
size_t Queue::addBatch(std::vector<Job> jobs) {
    std::lock_guard lock(mutex_);
    if (closing_) return 0;
    std::unordered_set<std::string> keys;
    for (auto& entry : jobs_) keys.insert(entry->job.key);
    size_t count = 0;
    for (auto& job : jobs) {
        if (!keys.insert(job.key).second) continue;
        auto entry = std::make_shared<Entry>();
        entry->job = std::move(job);
        entry->job.id = next_++;
        entry->job.state = pausedAll_ ? State::Paused : State::Queued;
        entry->job.done = entry->job.total = 0;
        entry->job.historyOnly = false;
        entry->job.detail.clear();
        entry->job.output.clear();
        jobs_.push_back(entry);
        ++count;
    }
    if (count) {
        saveLocked();
        cv_.notify_all();
    }
    return count;
}
void Queue::start() {
    std::lock_guard lock(mutex_);
    if (!workers_.empty() || closing_) return;
    for (int i = 0; i < 4; ++i) workers_.emplace_back([this] {
        work();
    });
}
void Queue::work() {
    for (;;) {
        std::shared_ptr<Entry> entry;
        Job input;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] {
                if (closing_) return true;
                if (pausedAll_) return false;
                for (auto& e : jobs_)
                    if (e->job.state == State::Queued && !e->running && !e->removing && e->job.kind != maintenance_) return true;
                return false;
            });
            if (closing_) return;
            for (auto& e : jobs_)
                if (e->job.state == State::Queued && !e->running && !e->removing && e->job.kind != maintenance_) {
                    entry = e;
                    break;
                }
            entry->running = true;
            entry->stop = false;
            entry->job.state = State::Active;
            entry->job.detail = "Connecting";
            input = entry->job;
            saveLocked();
        }
        std::string failure;
        bool completed = false;
        try {
            runner_(input, entry->stop, [this, entry](uint64_t done, uint64_t total, const std::string& detail, const std::string& output) {
                std::lock_guard lock(mutex_);
                entry->job.done = done;
                entry->job.total = total;
                entry->job.detail = detail;
                if (!output.empty()) entry->job.output = output;
            });
            completed = true;
        } catch (const std::exception& e) {
            failure = e.what();
        } catch (...) {
            failure = "Unexpected transfer failure";
        }
        {
            std::lock_guard lock(mutex_);
            entry->running = false;
            // A runner that returns has finished the download, even when a pause arrived at the last
            // moment; a stopped runner throws. Marking a saved file Paused would make Resume fail.
            entry->job.state = completed ? State::Complete : entry->stop ? State::Paused
                                                                         : State::Failed;
            entry->job.detail = completed ? "Finished" : entry->stop ? "Partial download retained"
                                                                     : failure;
            if (completed) {
                if (!entry->job.total) entry->job.total = entry->job.done;
                entry->job.done = entry->job.total;
            }
            if (entry->removing) {
                if (completed) {
                    entry->removing = false;
                    entry->job.historyOnly = true;
                } else
                    std::erase(jobs_, entry);
            } else if (!completed && entry->resumeRequested && !closing_ && !pausedAll_) {
                entry->job.state = State::Queued;
                entry->job.detail.clear();
            }
            entry->resumeRequested = false;
            saveLocked();
            cv_.notify_all();
        }
    }
}
void Queue::shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (closing_) return;
        closing_ = true;
        for (auto& e : jobs_) {
            e->stop = true;
            if (!e->running && e->job.state == State::Queued) e->job.state = State::Paused;
        }
        cv_.notify_all();
    }
    workers_.clear();
    // The saver writes the final state, then ends; later changes are written directly.
    {
        std::lock_guard lock(mutex_);
        saveLocked();
        saverStop_ = true;
        saveCv_.notify_all();
    }
    if (saver_.joinable()) saver_.join();
}
void Queue::pause(uint64_t id) {
    std::lock_guard lock(mutex_);
    for (auto& e : jobs_)
        if (e->job.id == id) {
            if (e->running) {
                e->stop = true;
                e->job.detail = "Pausing…";
            } else if (e->job.state == State::Queued)
                e->job.state = State::Paused;
        }
    saveLocked();
}
void Queue::resume(uint64_t id) {
    std::lock_guard lock(mutex_);
    if (closing_) return;
    for (auto& e : jobs_)
        if (e->job.id == id && !e->running && (e->job.state == State::Paused || e->job.state == State::Failed)) {
            e->job.state = State::Queued;
            e->job.detail.clear();
            e->stop = false;
        }
    saveLocked();
    cv_.notify_all();
}
void Queue::moveToTop(const std::vector<uint64_t>& ids) {
    if (ids.empty()) return;
    const std::unordered_set<uint64_t> selected(ids.begin(), ids.end());
    std::lock_guard lock(mutex_);
    if (closing_) return;

    // Keep the rows above the waiting queue in place. Workers use this same order.
    const auto firstWaiting = std::find_if(jobs_.begin(), jobs_.end(), [](const auto& entry) {
        return !entry->removing && (entry->job.canMoveToTop() || entry->job.state == State::Failed);
    });
    const auto promote = [&](const auto& entry) {
        return !entry->running && !entry->removing && entry->job.canMoveToTop() && selected.contains(entry->job.id);
    };
    if (std::is_partitioned(firstWaiting, jobs_.end(), promote)) return;
    std::stable_partition(firstWaiting, jobs_.end(), promote);
    saveLocked();
}
void Queue::remove(uint64_t id) {
    std::lock_guard lock(mutex_);
    for (auto& e : jobs_)
        if (e->job.id == id) {
            if (e->job.state == State::Complete)
                e->job.historyOnly = true;
            else {
                e->removing = true;
                e->resumeRequested = false;
                e->stop = true;
            }
        }
    std::erase_if(jobs_, [](auto& e) {
        return e->removing && !e->running;
    });
    saveLocked();
    cv_.notify_all();
}
std::vector<Job> Queue::snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<Job> result;
    for (auto& e : jobs_)
        if (!e->removing) result.push_back(e->job);
    return result;
}
std::string Queue::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}
bool Queue::beginMaintenance(const std::string& kind) {
    std::lock_guard lock(mutex_);
    if (!maintenance_.empty()) return false;
    for (auto& e : jobs_)
        if (e->job.kind == kind && (e->running || e->job.state == State::Queued)) return false;
    maintenance_ = kind;
    return true;
}
void Queue::endMaintenance() {
    std::lock_guard lock(mutex_);
    maintenance_.clear();
    cv_.notify_all();
}
void Queue::pauseAll() {
    std::lock_guard lock(mutex_);
    pausedAll_ = true;
    for (auto& e : jobs_) {
        e->resumeRequested = false;
        if (e->running) {
            e->stop = true;
            e->job.detail = "Pausing…";
        } else if (e->job.state == State::Queued)
            e->job.state = State::Paused;
    }
    saveLocked();
}
void Queue::resumeAll() {
    std::lock_guard lock(mutex_);
    if (closing_) return;
    pausedAll_ = false;
    for (auto& e : jobs_) {
        if (e->removing) continue;
        if (e->running) {
            if (e->stop) e->resumeRequested = true;
        } else if (e->job.state == State::Paused || e->job.state == State::Failed) {
            e->job.state = State::Queued;
            e->job.detail.clear();
        }
    }
    saveLocked();
    cv_.notify_all();
}
void Queue::removeAll() {
    std::lock_guard lock(mutex_);
    // Completed rows leave All but remain in History; active workers are untouched.
    for (auto& entry : jobs_)
        if (entry->job.state == State::Complete) entry->job.historyOnly = true;
    removeWaitingLocked();
}
void Queue::removeWaiting() {
    std::lock_guard lock(mutex_);
    removeWaitingLocked();
}
void Queue::removeWaitingLocked() {
    std::erase_if(jobs_, [](const auto& entry) {
        return !entry->running && (entry->job.state == State::Queued || entry->job.state == State::Paused || entry->job.state == State::Failed);
    });
    saveLocked();
}
bool Queue::paused() const {
    std::lock_guard lock(mutex_);
    return pausedAll_;
}
bool Queue::downloadAgain(uint64_t id) {
    std::lock_guard lock(mutex_);
    if (closing_) return false;
    auto found = std::find_if(jobs_.begin(), jobs_.end(), [&](auto& e) {
        return e->job.id == id && !e->removing && !e->running && e->job.state == State::Complete;
    });
    if (found == jobs_.end()) return false;
    auto entry = std::make_shared<Entry>();
    entry->job = (*found)->job;
    auto& j = entry->job;
    j.id = next_++;
    j.done = 0;
    j.historyOnly = false;
    j.state = pausedAll_ ? State::Paused : State::Queued;
    j.detail.clear();
    j.output.clear();
    if (j.kind == "Archive") {
        archive::Path old(archive::pathFromUtf8(j.input.at("target").get<std::string>())), target;
        for (unsigned attempt = 0;; ++attempt) {
            auto name = archive::pathUtf8(old.stem()) + " [download-" + std::to_string(j.id) + (attempt ? "-" + std::to_string(attempt) : "") + "]" + archive::pathUtf8(old.extension());
            target = archive::safePath(old.parent_path(), name);
            j.key = pathKey(target);
            if (!std::filesystem::exists(target) && std::none_of(jobs_.begin(), jobs_.end(), [&](auto& e) {
                    return e->job.key == j.key;
                })) break;
        }
        j.input["target"] = archive::pathUtf8(target);
        j.name = archive::pathUtf8(target.filename());
    } else {
        unsigned char bytes[16];
        platform::random(bytes, sizeof bytes);
        std::string token;
        for (auto b : bytes) {
            token += "0123456789abcdef"[b >> 4];
            token += "0123456789abcdef"[b & 15];
        }
        j.input["outputToken"] = token;
        j.key += "|repeat|" + token;
    }
    jobs_.push_back(entry);
    saveLocked();
    cv_.notify_all();
    return true;
}
void Queue::clearHistory() {
    std::lock_guard lock(mutex_);
    std::erase_if(jobs_, [](auto& e) {
        return !e->running && !e->removing && e->job.state == State::Complete;
    });
    saveLocked();
}
void Queue::removeHistory(uint64_t id) {
    std::lock_guard lock(mutex_);
    std::erase_if(jobs_, [&](auto& e) {
        return e->job.id == id && !e->running && !e->removing && e->job.state == State::Complete;
    });
    saveLocked();
}
}
