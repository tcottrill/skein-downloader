int run(const archive::Path& executable,const std::vector<std::wstring>& arguments,std::atomic_bool& stop,const Lines& lines){
    if(stop)return 1;
    int descriptors[2];if(pipe2(descriptors,O_CLOEXEC)<0)throw std::runtime_error("Cannot create helper pipe");
    struct Fds{int r,w;~Fds(){if(r>=0)close(r);if(w>=0)close(w);}} fds{descriptors[0],descriptors[1]};
    std::vector<std::string> values{archive::pathUtf8(executable)};for(auto& a:arguments)values.push_back(archive::utf8(a));
    std::vector<char*> argv;for(auto& a:values)argv.push_back(a.data());argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
    posix_spawnattr_t attributes;posix_spawnattr_init(&attributes);
    struct Cleanup{posix_spawn_file_actions_t* a;posix_spawnattr_t* b;~Cleanup(){posix_spawn_file_actions_destroy(a);posix_spawnattr_destroy(b);}} cleanup{&actions,&attributes};
    posix_spawn_file_actions_adddup2(&actions,fds.w,STDOUT_FILENO);posix_spawn_file_actions_adddup2(&actions,fds.w,STDERR_FILENO);
    posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0);
    posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETPGROUP);posix_spawnattr_setpgroup(&attributes,0);
    pid_t pid=0;int error=posix_spawn(&pid,executable.c_str(),&actions,&attributes,argv.data(),environ);
    if(error)throw std::runtime_error("Cannot start video helper: "+std::to_string(error));
    // The group is only signalled while the leader is unreaped, so its id cannot have been reused.
    struct Child{pid_t id;bool reaped=false;~Child(){if(!reaped){kill(-id,SIGKILL);int status;while(waitpid(id,&status,0)<0&&errno==EINTR){}}}} child{pid};
    close(fds.w);fds.w=-1;fcntl(fds.r,F_SETFL,fcntl(fds.r,F_GETFL)|O_NONBLOCK);
    std::string pending;int status=0;bool stopping=false;auto deadline=std::chrono::steady_clock::now();
    auto emit=[&](std::string line){if(!line.empty()&&line.back()=='\r')line.pop_back();if(lines&&!line.empty())lines(line);};
    for(;;){
        if(stop&&!stopping){stopping=true;kill(-pid,SIGTERM);deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);}
        if(stopping&&std::chrono::steady_clock::now()>=deadline)kill(-pid,SIGKILL);
        char buffer[8192];auto n=read(fds.r,buffer,sizeof buffer);
        if(n>0){pending.append(buffer,n);size_t at;while((at=pending.find('\n'))!=std::string::npos){emit(pending.substr(0,at));pending.erase(0,at+1);}if(pending.size()>65536){emit("[Long output line omitted]");pending.clear();}}
        if(!child.reaped){
            // Detect exit without reaping, kill leftover descendants, then reap.
            siginfo_t info{};auto result=waitid(P_PID,pid,&info,WEXITED|WNOHANG|WNOWAIT);
            if(result==0&&info.si_pid==pid){kill(-pid,SIGKILL);while(waitpid(pid,&status,0)<0&&errno==EINTR){}child.reaped=true;}
            else if(result<0&&errno!=EINTR)throw std::runtime_error("Cannot read helper exit status");
        }
        if(child.reaped&&n<=0)break;
        if(n<=0)std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if(!pending.empty())emit(pending);
    return stop?1:WIFEXITED(status)?WEXITSTATUS(status):1;
}
archive::Path executableFolder(){return std::filesystem::read_symlink("/proc/self/exe").parent_path();}
// Tools sit beside the program (Flatpak: /app/bin) or, for the AppImage and native
// installs, in ../libexec/skein-downloader. SKEIN_VIDEO_TOOLS overrides both.
archive::Path toolsFolder(const archive::Path& root){
    if(auto p=std::getenv("SKEIN_VIDEO_TOOLS"))return p;
    auto libexec=(root/".."/"libexec"/"skein-downloader").lexically_normal();
    return toolsReady(root)||!toolsReady(libexec)?root:libexec;
}
bool toolsReady(const archive::Path& folder){for(auto name:{"yt-dlp","ffmpeg","ffprobe","deno"})if(access((folder/name).c_str(),X_OK)!=0)return false;return true;}
