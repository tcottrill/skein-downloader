#pragma once
#include "download_queue.h"
#include "extras.h"
#include <atomic>
namespace lite {
bool visible(const downloads::Job&, int mode, const std::string& query);
// Jobs for the checked files that do not exist yet. Safe to run on another thread on copies of its arguments:
// progress (optional) is set to the number of checked files examined so far, and when stop (optional) becomes
// true the function throws std::runtime_error("Paused"), the engine's word for a cancelled operation.
// extras (optional, already bound to a link by archive::forLink) go on each job whose download address is on
// the extras' own host and port, bound to that address; other jobs get none.
std::vector<downloads::Job> fileJobs(const archive::Item&, const std::vector<bool>&, const archive::Path&, unsigned, const std::string&, std::atomic_size_t* progress = nullptr, const std::atomic_bool* stop = nullptr, const archive::RequestExtras* extras = nullptr);
bool equivalentVideo(const std::vector<downloads::Job>&, const archive::Path&, const std::string&, int, bool, bool);
}
