#include "host_limit.h"
#include "core.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>
namespace archive {
namespace {
// One lock, one condition variable and one count per host, shared by every download.
struct HostSlots {
    std::mutex lock;
    std::condition_variable changed;
    unsigned limit = 16;
    std::map<std::string, unsigned> used;
    bool hasFree(const std::string& host) const {
        const auto it = used.find(host);
        return (it == used.end() ? 0u : it->second) < limit;
    }
};
HostSlots& slots() {
    static HostSlots instance;
    return instance;
}
}
void setHostLimit(unsigned connections) {
    auto& s = slots();
    {
        std::lock_guard guard(s.lock);
        s.limit = std::clamp(connections, 1u, 16u);
    }
    s.changed.notify_all();
}
// The constructor either takes a slot or throws; the destructor runs only for a slot that was taken.
HostSlot::HostSlot(const std::string& host, std::atomic_bool& stop, const std::function<void()>& waiting)
    : host_(lower(host)) {
    auto& s = slots();
    std::unique_lock guard(s.lock);
    if (!s.hasFree(host_)) {
        // Called without the lock, so the callback may take locks of its own.
        guard.unlock();
        if (waiting) waiting();
        guard.lock();
        // Wakes on every release and limit change; the timeout is how soon a stop is noticed.
        while (!s.hasFree(host_)) {
            if (stop) throw std::runtime_error("Paused");
            s.changed.wait_for(guard, std::chrono::milliseconds(100));
        }
    }
    ++s.used[host_];
}
HostSlot::~HostSlot() {
    auto& s = slots();
    {
        std::lock_guard guard(s.lock);
        auto it = s.used.find(host_);
        if (it != s.used.end() && --it->second == 0) s.used.erase(it);
    }
    // Waiters of other hosts wake too and go back to sleep; there are few of them.
    s.changed.notify_all();
}
}
