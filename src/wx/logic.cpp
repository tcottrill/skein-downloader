#include "logic.h"
#include <cctype>
#include <stdexcept>
namespace lite {
bool visible(const downloads::Job& j,int mode,const std::string& query) {
 auto lower=[](std::string s){for(char& c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return s;};
 return lower(j.name).find(lower(query))!=std::string::npos &&
  ((mode==0&&!j.historyOnly)||(mode==1&&j.state=="Active")||(mode==2&&(j.state=="Queued"||j.state=="Paused"||j.state=="Failed"))||(mode==3&&j.state=="Complete"));
}
std::vector<downloads::Job> fileJobs(const archive::Item& item,const std::vector<bool>& checked,const archive::Path& folder,unsigned connections,const std::string& server) {
 if(!folder.is_absolute())throw std::runtime_error("Choose an absolute download folder.");
 if(checked.size()!=item.files.size())throw std::runtime_error("Invalid file selection.");
 auto base=item.sourceUrl.empty()?archive::safePath(folder,item.id):folder.lexically_normal();
 std::vector<downloads::Job> result;
 for(size_t i=0;i<checked.size();++i)if(checked[i]) {
  const auto& f=item.files[i];auto target=archive::safePath(base,f.name);if(std::filesystem::exists(target))continue;
  nlohmann::json input={{"item",item.id},{"name",f.name},{"format",f.format},{"sha1",f.sha1},{"sha256",f.sha256},{"size",f.size},{"sizeKnown",f.sizeKnown},{"target",archive::pathUtf8(target)},{"mirrors",item.mirrorBases},{"server",server},{"connections",connections}};
  if(!item.sourceUrl.empty()){input["url"]=item.sourceUrl;input["server"]="";}
  if(!item.downloadBase.empty()){input["base"]=item.downloadBase;input["server"]="";}
  if(!f.url.empty()){input["url"]=f.url;input["server"]="";}
  downloads::Job j;j.name=f.name;j.kind="Archive";j.key=downloads::pathKey(target);j.input=std::move(input);result.push_back(std::move(j));
 }
 return result;
}
bool equivalentVideo(const std::vector<downloads::Job>& jobs,const archive::Path& folder,const std::string& url,int quality,bool mp3,bool mp4) {
 for(const auto& j:jobs)if(j.kind=="Video")try {
  const auto& v=j.input;int q=v.at("quality");
  if(v.at("url")==url && downloads::pathKey(archive::pathFromUtf8(v.at("folder")))==downloads::pathKey(folder)&&q==quality&&(q==7&&v.value("mp3",false))==mp3&&(q!=7&&v.value("mp4",false))==mp4)return true;
 }catch(...){}
 return false;
}
}
