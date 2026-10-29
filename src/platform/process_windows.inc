namespace {
struct Handle {
    HANDLE h=nullptr;
    explicit Handle(HANDLE value=nullptr):h(value){}
    ~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
    Handle(const Handle&)=delete;
    operator HANDLE()const{return h;}
    void close(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);h=nullptr;}
};
void require(bool ok,const char* operation){if(!ok)throw std::runtime_error(std::string(operation)+" (Windows error "+std::to_string(GetLastError())+")");}
}
int run(const archive::Path& executable,const std::vector<std::wstring>& arguments,std::atomic_bool& stop,const Lines& lines) {
    if(stop)return 1;
    std::wstring command=quote(executable.wstring());for(auto& arg:arguments)command+=L" "+quote(arg);
    if(command.size()>=32767)throw std::runtime_error("Video command is too long.");
    SECURITY_ATTRIBUTES sa{sizeof sa,nullptr,TRUE};Handle read,write;
    require(CreatePipe(&read.h,&write.h,&sa,0)!=FALSE,"Create output pipe");
    require(SetHandleInformation(read,HANDLE_FLAG_INHERIT,0)!=FALSE,"Set output pipe inheritance");
    Handle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr));
    require(input.h!=INVALID_HANDLE_VALUE,"Open process input");
    Handle job(CreateJobObjectW(nullptr,nullptr));require(job.h!=nullptr,"Create process job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof limits)!=FALSE,"Set process job limits");
    SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
    std::vector<unsigned char> storage(bytes);auto attributes=(LPPROC_THREAD_ATTRIBUTE_LIST)storage.data();
    require(InitializeProcThreadAttributeList(attributes,1,0,&bytes)!=FALSE,"Initialize process attributes");
    struct Cleanup {LPPROC_THREAD_ATTRIBUTE_LIST p;~Cleanup(){DeleteProcThreadAttributeList(p);}} cleanup{attributes};
    HANDLE inherited[]={write.h,input.h};
    require(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof inherited,nullptr,nullptr)!=FALSE,"Restrict inherited handles");
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof startup;startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput=startup.StartupInfo.hStdError=write;startup.StartupInfo.hStdInput=input;startup.lpAttributeList=attributes;
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,executable.parent_path().c_str(),&startup.StartupInfo,&process)!=FALSE,"Start video helper");
    Handle child(process.hProcess),thread(process.hThread);
    if(!AssignProcessToJobObject(job,child)){TerminateProcess(child,1);require(false,"Assign video helper to job");}
    if(ResumeThread(thread)==(DWORD)-1){TerminateJobObject(job,1);require(false,"Resume video helper");}
    write.close();input.close();thread.close();
    std::string pending;
    auto emit=[&](std::string line){if(!line.empty()&&line.back()=='\r')line.pop_back();if(lines&&!line.empty())lines(line);};
    bool ended=false;auto endTime=std::chrono::steady_clock::now();
    for(;;) {
        if(stop){TerminateJobObject(job,1);WaitForSingleObject(child,2000);return 1;}
        DWORD available=0;
        if(!PeekNamedPipe(read,nullptr,0,nullptr,&available,nullptr))break;
        if(available) {
            char buffer[8192];DWORD got=0;require(ReadFile(read,buffer,std::min<DWORD>(available,sizeof buffer),&got,nullptr)!=FALSE,"Read video output");
            pending.append(buffer,got);
            size_t at;while((at=pending.find('\n'))!=std::string::npos){emit(pending.substr(0,at));pending.erase(0,at+1);}
            if(pending.size()>65536){emit("[Long output line omitted]");pending.clear();}
        }
        if(WaitForSingleObject(child,available?0:25)==WAIT_OBJECT_0) {
            if(!ended){ended=true;endTime=std::chrono::steady_clock::now();continue;}
            if(!available||std::chrono::steady_clock::now()-endTime>std::chrono::seconds(1))break;
        }
    }
    if(!pending.empty())emit(pending);
    if(WaitForSingleObject(child,0)!=WAIT_OBJECT_0) {
        // A helper may close its output before exiting; still allow cancellation.
        while(WaitForSingleObject(child,50)==WAIT_TIMEOUT)if(stop){TerminateJobObject(job,1);return 1;}
    }
    DWORD code=1;require(GetExitCodeProcess(child,&code)!=FALSE,"Read video helper exit status");return (int)code;
}
archive::Path executableFolder() {
    std::wstring value(32768,0);DWORD n=GetModuleFileNameW(nullptr,value.data(),(DWORD)value.size());
    if(!n||n>=value.size())throw std::runtime_error("Cannot locate video application.");value.resize(n);return archive::Path(value).parent_path();
}
archive::Path toolsFolder(const archive::Path& root) {
    std::ifstream in(root/L"video-tools"/L"current.txt");std::string name;std::getline(in,name);
    if(name.empty()||name.size()>80||name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-")!=std::string::npos)return {};
    return root/L"video-tools"/archive::wide(name);
}
bool toolsReady(const archive::Path& folder) {
    if(folder.empty())return false;
    for(auto name:{L"yt-dlp.exe",L"ffmpeg.exe",L"ffprobe.exe",L"deno.exe"})if(!std::filesystem::is_regular_file(folder/name))return false;
    return true;
}