#pragma once
#include <atomic>
#include <functional>
#include <string>
namespace archive {
// The most connections that transfer file data at once to one server, across all downloads.
// Clamped to 1..16 (default 16); takes effect at once, also for connections that are waiting.
// Process-wide; it is not saved here.
void setHostLimit(unsigned connections);
// Holds one of a host's connection slots for its lifetime.
class HostSlot {
public:
    // Waits for a free slot of host (compared in lower case). Calls waiting() once if it has to wait.
    // Throws std::runtime_error("Paused") when stop becomes true while waiting.
    HostSlot(const std::string& host, std::atomic_bool& stop, const std::function<void()>& waiting = {});
    ~HostSlot();
    HostSlot(const HostSlot&) = delete;
    HostSlot& operator=(const HostSlot&) = delete;

private:
    std::string host_;
};
}
