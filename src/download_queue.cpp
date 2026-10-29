#include "download_queue.h"
#include "platform/files.h"
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <unordered_set>
namespace downloads {
std::string pathKey(const archive::Path& path){
    auto normal=path.lexically_normal();
#ifdef _WIN32
    auto key=normal.wstring();std::transform(key.begin(),key.end(),key.begin(),[](wchar_t c){return (wchar_t)std::towlower(c);});return archive::utf8(key);
#else
    return archive::pathUtf8(normal);
#endif
}
Queue::Queue(archive::Path path,Runner runner):path_(std::move(path)),runner_(std::move(runner)) {
    try {
        if(!std::filesystem::exists(path_))return;
        std::ifstream in(path_);nlohmann::json data;in>>data;
        if(!data.is_array())throw std::runtime_error("Invalid history format");
        for(auto& j:data){
            auto e=std::make_shared<Entry>();auto& v=e->job;
            v.id=j.at("id").get<uint64_t>();v.name=j.at("name");v.kind=j.at("kind");v.key=j.at("key");v.input=j.at("input");
            v.state=j.value("state","Paused");v.done=j.value("done",uint64_t(0));v.total=j.value("total",uint64_t(0));v.output=j.value("output","");v.detail=j.value("detail","");
            if(v.state!="Complete"&&v.state!="Failed")v.state="Paused";
            v.historyOnly=v.state=="Complete"&&j.value("historyOnly",false);
            next_=std::max(next_,v.id+1);jobs_.push_back(e);
        }
    }catch(const std::exception& e){jobs_.clear();historyReadable_=false;error_=std::string("History could not be read; original preserved. New downloads cannot be saved: ")+e.what();}
}
Queue::~Queue(){shutdown();}
void Queue::saveLocked(){
    if(!historyReadable_)return;
    try{
        nlohmann::json data=nlohmann::json::array();
        for(auto& e:jobs_){if(e->removing)continue;auto& j=e->job;data.push_back({{"id",j.id},{"name",j.name},{"kind",j.kind},{"key",j.key},{"input",j.input},{"state",j.state},{"done",j.done},{"total",j.total},{"detail",j.detail},{"output",j.output},{"historyOnly",j.historyOnly}});}
        std::filesystem::create_directories(path_.parent_path());auto temp=path_;temp+=L".tmp";
        {std::ofstream out(temp,std::ios::trunc);out<<data.dump(2);out.flush();if(!out)throw std::runtime_error("Cannot write history file");}
        platform::replace(temp,path_);
        error_.clear();
    }catch(const std::exception& e){error_=std::string("History not saved: ")+e.what();}
}
bool Queue::add(std::string name,std::string kind,std::string key,nlohmann::json input){
    Job job;job.name=std::move(name);job.kind=std::move(kind);job.key=std::move(key);job.input=std::move(input);
    return addBatch({std::move(job)})==1;
}
size_t Queue::addBatch(std::vector<Job> jobs){
    std::lock_guard lock(mutex_);if(closing_)return 0;
    std::unordered_set<std::string> keys;for(auto& entry:jobs_)keys.insert(entry->job.key);
    size_t count=0;for(auto& job:jobs){
        if(!keys.insert(job.key).second)continue;
        auto entry=std::make_shared<Entry>();entry->job=std::move(job);entry->job.id=next_++;entry->job.state=pausedAll_?"Paused":"Queued";
        entry->job.done=entry->job.total=0;entry->job.historyOnly=false;entry->job.detail.clear();entry->job.output.clear();jobs_.push_back(entry);++count;
    }
    if(count){saveLocked();cv_.notify_all();}return count;
}
void Queue::start(){std::lock_guard lock(mutex_);if(!workers_.empty()||closing_)return;for(int i=0;i<4;++i)workers_.emplace_back([this]{work();});}
void Queue::work(){
    for(;;){
        std::shared_ptr<Entry> entry;Job input;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock,[&]{if(closing_)return true;if(pausedAll_)return false;for(auto& e:jobs_)if(e->job.state=="Queued"&&!e->running&&!e->removing&&e->job.kind!=maintenance_)return true;return false;});
            if(closing_)return;
            for(auto& e:jobs_)if(e->job.state=="Queued"&&!e->running&&!e->removing&&e->job.kind!=maintenance_){entry=e;break;}
            entry->running=true;entry->stop=false;entry->job.state="Active";entry->job.detail="Connecting";input=entry->job;saveLocked();
        }
        std::string failure;
        try{runner_(input,entry->stop,[this,entry](uint64_t done,uint64_t total,const std::string& detail,const std::string& output){
            std::lock_guard lock(mutex_);entry->job.done=done;entry->job.total=total;entry->job.detail=detail;if(!output.empty())entry->job.output=output;
        });}catch(const std::exception& e){failure=e.what();}catch(...){failure="Unexpected transfer failure";}
        {
            std::lock_guard lock(mutex_);entry->running=false;
            entry->job.state=entry->stop?"Paused":failure.empty()?"Complete":"Failed";
            entry->job.detail=entry->stop?"Partial download retained":failure.empty()?"Finished":failure;
            if(entry->job.state=="Complete"){if(!entry->job.total)entry->job.total=entry->job.done;entry->job.done=entry->job.total;}
            if(entry->removing)std::erase(jobs_,entry);
            else if(entry->resumeRequested&&!closing_&&!pausedAll_){entry->job.state="Queued";entry->job.detail.clear();}
            entry->resumeRequested=false;
            saveLocked();cv_.notify_all();
        }
    }
}
void Queue::shutdown(){
    {std::lock_guard lock(mutex_);if(closing_)return;closing_=true;for(auto& e:jobs_){e->stop=true;if(!e->running&&e->job.state=="Queued")e->job.state="Paused";}cv_.notify_all();}
    workers_.clear();std::lock_guard lock(mutex_);saveLocked();
}
void Queue::pause(uint64_t id){std::lock_guard lock(mutex_);for(auto& e:jobs_)if(e->job.id==id){if(e->running){e->stop=true;e->job.detail="Pausing…";}else if(e->job.state=="Queued")e->job.state="Paused";}saveLocked();}
void Queue::resume(uint64_t id){std::lock_guard lock(mutex_);if(closing_)return;for(auto& e:jobs_)if(e->job.id==id&&!e->running&&(e->job.state=="Paused"||e->job.state=="Failed")){e->job.state="Queued";e->job.detail.clear();e->stop=false;}saveLocked();cv_.notify_all();}
void Queue::remove(uint64_t id){std::lock_guard lock(mutex_);for(auto& e:jobs_)if(e->job.id==id){if(e->job.state=="Complete")e->job.historyOnly=true;else{e->removing=true;e->resumeRequested=false;e->stop=true;}}std::erase_if(jobs_,[](auto& e){return e->removing&&!e->running;});saveLocked();cv_.notify_all();}
std::vector<Job> Queue::snapshot()const{std::lock_guard lock(mutex_);std::vector<Job> result;for(auto& e:jobs_)if(!e->removing)result.push_back(e->job);return result;}
std::string Queue::error()const{std::lock_guard lock(mutex_);return error_;}
bool Queue::beginMaintenance(const std::string& kind){std::lock_guard lock(mutex_);if(!maintenance_.empty())return false;for(auto& e:jobs_)if(e->job.kind==kind&&(e->running||e->job.state=="Queued"))return false;maintenance_=kind;return true;}
void Queue::endMaintenance(){std::lock_guard lock(mutex_);maintenance_.clear();cv_.notify_all();}
void Queue::pauseAll(){
    std::lock_guard lock(mutex_);pausedAll_=true;
    for(auto& e:jobs_){e->resumeRequested=false;if(e->running){e->stop=true;e->job.detail="Pausing…";}else if(e->job.state=="Queued")e->job.state="Paused";}
    saveLocked();
}
void Queue::resumeAll(){
    std::lock_guard lock(mutex_);if(closing_)return;pausedAll_=false;
    for(auto& e:jobs_){if(e->removing)continue;if(e->running){if(e->stop)e->resumeRequested=true;}else if(e->job.state=="Paused"||e->job.state=="Failed"){e->job.state="Queued";e->job.detail.clear();}}
    saveLocked();cv_.notify_all();
}
void Queue::removeAll(){
    std::lock_guard lock(mutex_);
    // Completed rows leave All but remain in History; active workers are untouched.
    for(auto& entry:jobs_)if(entry->job.state=="Complete")entry->job.historyOnly=true;
    removeWaitingLocked();
}
void Queue::removeWaiting(){std::lock_guard lock(mutex_);removeWaitingLocked();}
void Queue::removeWaitingLocked(){
    std::erase_if(jobs_,[](const auto& entry){return !entry->running&&(entry->job.state=="Queued"||entry->job.state=="Paused"||entry->job.state=="Failed");});
    saveLocked();
}
bool Queue::paused()const{std::lock_guard lock(mutex_);return pausedAll_;}
bool Queue::downloadAgain(uint64_t id){
    std::lock_guard lock(mutex_);if(closing_)return false;
    auto found=std::find_if(jobs_.begin(),jobs_.end(),[&](auto& e){return e->job.id==id&&!e->removing&&!e->running&&e->job.state=="Complete";});
    if(found==jobs_.end())return false;
    auto entry=std::make_shared<Entry>();entry->job=(*found)->job;auto& j=entry->job;
    j.id=next_++;j.done=0;j.historyOnly=false;j.state=pausedAll_?"Paused":"Queued";j.detail.clear();j.output.clear();
    if(j.kind=="Archive"){
        archive::Path old(archive::pathFromUtf8(j.input.at("target").get<std::string>())),target;
        for(unsigned attempt=0;;++attempt){
            auto name=archive::pathUtf8(old.stem())+" [download-"+std::to_string(j.id)+(attempt?"-"+std::to_string(attempt):"")+"]"+archive::pathUtf8(old.extension());
            target=archive::safePath(old.parent_path(),name);
            j.key=pathKey(target);
            if(!std::filesystem::exists(target)&&std::none_of(jobs_.begin(),jobs_.end(),[&](auto& e){return e->job.key==j.key;}))break;
        }
        j.input["target"]=archive::pathUtf8(target);j.name=archive::pathUtf8(target.filename());
    }else {
        unsigned char bytes[16];
        platform::random(bytes,sizeof bytes);
        std::string token;for(auto b:bytes){token+="0123456789abcdef"[b>>4];token+="0123456789abcdef"[b&15];}
        j.input["outputToken"]=token;j.key+="|repeat|"+token;
    }
    jobs_.push_back(entry);saveLocked();cv_.notify_all();return true;
}
void Queue::clearHistory(){
    std::lock_guard lock(mutex_);
    std::erase_if(jobs_,[](auto& e){return !e->running&&!e->removing&&e->job.state=="Complete";});saveLocked();
}
void Queue::removeHistory(uint64_t id){
    std::lock_guard lock(mutex_);
    std::erase_if(jobs_,[&](auto& e){return e->job.id==id&&!e->running&&!e->removing&&e->job.state=="Complete";});saveLocked();
}
}
