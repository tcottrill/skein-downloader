#pragma once
#include "download_queue.h"
namespace downloads {
std::vector<std::wstring> videoArgs(const Job&,const archive::Path&);
// The canonical URL a file job downloads from: a direct link, a Hugging Face resolve URL, or archive.org.
std::string fileSourceUrl(const Job&);
void runJob(const Job&,std::atomic_bool&,const Report&);
}
