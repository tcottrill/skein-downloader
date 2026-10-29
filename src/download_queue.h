#pragma once
#include "core.h"
#include "json.hpp"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
namespace downloads {
struct Job {
    uint64_t id=0,done=0,total=0;
    std::string name,kind,key,state="Queued",detail,output;
    nlohmann::json input;
    bool historyOnly=false;
};
// Duplicate-detection key for a destination path; case-insensitive on Windows,
// matching keys saved by earlier versions.
std::string pathKey(const archive::Path& path);
using Report=std::function<void(uint64_t,uint64_t,const std::string&,const std::string&)>;
using Runner=std::function<void(const Job&,std::atomic_bool&,const Report&)>;
class Queue {
    struct Entry {Job job;std::atomic_bool stop=false;bool running=false,removing=false,resumeRequested=false;};
    archive::Path path_;Runner runner_;mutable std::mutex mutex_;std::condition_variable cv_;
    std::vector<std::shared_ptr<Entry>> jobs_;std::vector<std::jthread> workers_;
    uint64_t next_=1;bool closing_=false,historyReadable_=true,pausedAll_=false;std::string error_,maintenance_;
    void saveLocked();void removeWaitingLocked();void work();
public:
    Queue(archive::Path path,Runner runner);~Queue();
    bool add(std::string name,std::string kind,std::string key,nlohmann::json input);
    size_t addBatch(std::vector<Job> jobs);
    void start();void shutdown();void pause(uint64_t id);void resume(uint64_t id);void remove(uint64_t id);
    std::vector<Job> snapshot()const;std::string error()const;
    bool beginMaintenance(const std::string& kind);void endMaintenance();
    void pauseAll();void resumeAll();void removeAll();void removeWaiting();bool paused()const;
    bool downloadAgain(uint64_t id);void clearHistory();void removeHistory(uint64_t id);
};
}
