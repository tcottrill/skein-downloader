#include "job_runner.h"
#include "video.h"
#include "sites.h"
#include "util/sys_log.h"
#include <cmath>
namespace downloads {
std::vector<std::wstring> videoArgs(const downloads::Job& job,const archive::Path& tools){
    auto& j=job.input;
    auto args=video::downloadArgs(tools,archive::wide(j.at("url").get<std::string>()),archive::pathFromUtf8(j.at("folder").get<std::string>()),j.at("quality"),j.value("mp3",false),j.value("mp4",false));
    auto output=std::find(args.begin(),args.end(),L"--output")+1;
    // Persisted queue identity isolates .part files even when two URLs resolve to the same media.
    output->insert(output->find(L".%(ext)s"),L" [download-"+archive::wide(j.value("outputToken",std::to_string(job.id)))+L"]");
    auto access=video::accessArgs(j.value("impersonate",false),j.value("cookiesFromBrowser",std::string()));
    args.insert(std::find(args.begin(),args.end(),L"--"),access.begin(),access.end());
    return args;
}
std::string fileSourceUrl(const downloads::Job& job){
    auto& j=job.input;auto name=j.at("name").get<std::string>();
    if(j.contains("url"))return j.at("url").get<std::string>();
    if(j.contains("base"))return j.at("base").get<std::string>()+archive::encode(name,true);
    return "https://archive.org/download/"+archive::encode(j.at("item").get<std::string>())+"/"+archive::encode(name,true);
}
void runJob(const downloads::Job& job,std::atomic_bool& stop,const downloads::Report& report){
    auto& j=job.input;
    if(job.kind=="Archive"){
        archive::File file{j.at("name"),j.at("format"),j.at("sha1"),j.at("size"),j.at("sizeKnown"),j.value("sha256","")};
        auto target=archive::pathFromUtf8(j.at("target").get<std::string>());
        // Direct links (any website) and Hugging Face repos have one source; archive.org items add mirrors.
        std::string canonical=fileSourceUrl(job),forced,identity;std::vector<std::string> mirrors;
        // A file locker's page link stays the file's identity; each run asks the site for a current download link.
        if(archive::isLockerLink(canonical)){
            try{auto resolved=archive::resolveLockerUrl(canonical,stop);if(resolved!=canonical){identity=canonical;canonical=resolved;}}
            catch(const std::exception& e){LOG_ERROR("Job %llu: could not get a download link from %s: %s",(unsigned long long)job.id,archive::logUrl(canonical).c_str(),e.what());throw;}
            LOG_INFO("Job %llu: file locker link resolved to %s",(unsigned long long)job.id,archive::logUrl(canonical).c_str());
        }
        if(!j.contains("url")&&!j.contains("base")){
            for(auto& base:j.at("mirrors"))mirrors.push_back(base.get<std::string>()+archive::encode(file.name,true));
            auto server=j.value("server","");forced=server.empty()?"":server=="standard"?canonical:server+archive::encode(file.name,true);
        }
        LOG_INFO("Job %llu: %s from %s to %s, %u connection(s), %zu mirror(s)%s%s",(unsigned long long)job.id,file.name.c_str(),archive::logUrl(canonical).c_str(),
            archive::pathUtf8(target).c_str(),j.value("connections",4u),mirrors.size(),forced.empty()?"":", forced source ",forced.empty()?"":archive::logUrl(forced).c_str());
        archive::download(canonical,file,target,j.value("connections",4u),stop,[&](const archive::Progress& p){report(p.done,p.total,p.status+(p.source.empty()?"":" · "+p.source),archive::pathUtf8(target));},mirrors,forced,{},identity);
    }else{
        auto tools=video::toolsFolder(video::executableFolder());
        if(!video::toolsReady(tools)){LOG_ERROR("Job %llu: video tools are missing from %s",(unsigned long long)job.id,archive::pathUtf8(tools).c_str());throw std::runtime_error("Install video tools from Add > Video first.");}
        archive::Path folder(archive::pathFromUtf8(j.at("folder").get<std::string>()));std::filesystem::create_directories(folder);
        auto args=videoArgs(job,tools);
        auto at=std::find(args.begin(),args.end(),L"--");args.insert(at,{L"--print",L"after_move:OUTPUT %(filepath)s"});
        {std::string command;for(auto& arg:args)command+=" "+archive::utf8(arg);LOG_INFO("Job %llu: yt-dlp%s",(unsigned long long)job.id,command.c_str());}
        uint64_t percent=0;std::string output,lastError;
        int code=video::run(video::toolExecutable(tools,"yt-dlp"),args,stop,[&](const std::string& line){
            if(line.starts_with("PROGRESS ")){try{double value=std::stod(line.substr(9));if(std::isfinite(value))percent=(uint64_t)std::clamp(value,0.0,100.0);}catch(...){}report(percent,100,line.substr(9),output);}
            else if(line.starts_with("OUTPUT ")){output=line.substr(7);report(percent,100,"Finalizing",output);}
            else {
                if(line.find("ERROR")!=std::string::npos){lastError=line;LOG_ERROR("Job %llu: %s",(unsigned long long)job.id,line.c_str());}
                else if(line.find("WARNING")!=std::string::npos)LOG_WARN("Job %llu: %s",(unsigned long long)job.id,line.c_str());
                report(percent,100,line,output);
            }
        });
        LOG_INFO("Job %llu: yt-dlp exited with code %d%s",(unsigned long long)job.id,code,stop?" (stopped)":"");
        if(!output.empty())LOG_INFO("Job %llu: saved %s",(unsigned long long)job.id,output.c_str());
        if(code&&!stop)throw std::runtime_error(lastError.empty()?"Video helper exited with code "+std::to_string(code):lastError);
    }
}
}
