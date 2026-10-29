#pragma once
#include "download_queue.h"
namespace lite {
bool visible(const downloads::Job&,int mode,const std::string& query);
std::vector<downloads::Job> fileJobs(const archive::Item&,const std::vector<bool>&,const archive::Path&,unsigned,const std::string&);
bool equivalentVideo(const std::vector<downloads::Job>&,const archive::Path&,const std::string&,int,bool,bool);
}
