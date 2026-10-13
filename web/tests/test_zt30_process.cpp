#include "../zt30-process.h"
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <fstream>

#ifdef __linux__
int main(int argc, char **) {
    if (argc > 1) { while (true) pause(); }
    char root[]="/tmp/zt30-process-XXXXXX";
    assert(mkdtemp(root));
    std::string prefix=std::string(root)+"/stage.";
    std::string stage=prefix+"aB0123";
    assert(mkdir(stage.c_str(),0700)==0);
    std::string original=stage+"/camera-app", installed=std::string(root)+"/camera-app";
    std::ifstream src("/proc/self/exe",std::ios::binary);
    std::ofstream dst(original,std::ios::binary); dst<<src.rdbuf();dst.close();
    assert(chmod(original.c_str(),0700)==0);
    assert(rename(original.c_str(),installed.c_str())==0);
    int pipefd[2];assert(pipe(pipefd)==0);
    pid_t pid=fork();assert(pid>=0);
    if (!pid) {
        close(pipefd[0]);assert(fcntl(pipefd[1],F_SETFD,FD_CLOEXEC)==0);
        execl(installed.c_str(),installed.c_str(),"wait",(char*)nullptr);_exit(127);
    }
    close(pipefd[1]);char c;assert(read(pipefd[0],&c,1)==0);close(pipefd[0]);
    // Replace the running inode, just as the filesystem updater does.
    assert(unlink(installed.c_str())==0);
    char proc[64],exe[4096];snprintf(proc,sizeof(proc),"/proc/%ld/exe",(long)pid);
    ssize_t n=readlink(proc,exe,sizeof(exe)-1);assert(n>0);exe[n]=0;
    std::string observed=exe;
    assert(observed.size()>10 && observed.substr(observed.size()-10)==" (deleted)");
    observed.resize(observed.size()-10);
    // Kernels may report either the canonical or the original staging path.
    assert(observed==installed || observed==original);
    assert(zt30_staged_app_pid(pid,original.c_str(),installed.c_str(),prefix.c_str()));
    assert(!zt30_staged_app_pid(pid,original.c_str(),"/unrelated/camera-app",prefix.c_str()));
    assert(!zt30_staged_app_pid(pid,(prefix+"bad/camera-app").c_str(),installed.c_str(),prefix.c_str()));
    assert(!zt30_staged_app_pid(pid,(original+".other").c_str(),installed.c_str(),prefix.c_str()));
    for (const char *tail : {"abc12/camera-app", "abc1234/camera-app", "ab.123/camera-app", "ab/123/camera-app", "abc123/camera-apx", "abc123camera-app/"})
        assert(!zt30_staged_app_pid(pid,(prefix+tail).c_str(),installed.c_str(),prefix.c_str()));
    assert(!zt30_staged_app_pid(pid,"camera-app",installed.c_str(),prefix.c_str()));
    assert(!zt30_staged_app_pid(pid,original.c_str(),(installed+"-other").c_str(),prefix.c_str()));
    kill(pid,SIGTERM);int status;waitpid(pid,&status,0);rmdir(stage.c_str());rmdir(root);
    puts("ZT30 staged executable identity PASS");
}

#else
int main() { puts("SKIP ZT30 staged executable identity: Linux procfs required"); }
#endif
