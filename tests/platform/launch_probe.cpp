// navigator_launch_probe: started by launcher_test through the real IAppLauncher. Writes what it
// inherited to argv[1] (via a temp file + rename, so the marker appears complete):
//   fds=<open fds, ascending, comma separated>
//   sid=<session id>  pgid=<process group id>
//   ppid=<parent pid>
//   cwd=<working directory>
//   stdio_null=<1 if fds 0, 1, 2 are all /dev/null>
//   env=<value of NAVIGATOR_PROBE_ENV>
//   sigmask_empty=<1 if no signal is blocked>
//   sigterm_default=<1 if SIGTERM has the default disposition>
//   args=<argv[2..] joined by '|'>
#include <algorithm>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool is_dev_null(int fd) {
    struct stat st {};
    struct stat null_st {};
    return ::fstat(fd, &st) == 0 && ::stat("/dev/null", &null_st) == 0 && S_ISCHR(st.st_mode) &&
           st.st_rdev == null_st.st_rdev;
}

std::string open_fds() {
    std::vector<int> fds;
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return "error";
    }
    const int own = ::dirfd(dir);
    while (const dirent* ent = ::readdir(dir)) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        const int fd = std::atoi(ent->d_name);
        if (fd != own) {
            fds.push_back(fd);
        }
    }
    ::closedir(dir);
    std::sort(fds.begin(), fds.end());
    std::string out;
    for (const int fd : fds) {
        if (!out.empty()) {
            out += ',';
        }
        out += std::to_string(fd);
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return 2;
    }
    const std::string marker = argv[1];
    std::string report;
    report += "fds=" + open_fds() + "\n";
    report += "sid=" + std::to_string(::getsid(0)) + "\n";
    report += "pgid=" + std::to_string(::getpgrp()) + "\n";
    report += "ppid=" + std::to_string(::getppid()) + "\n";
    std::vector<char> cwd(PATH_MAX + 1, '\0');
    report += std::string("cwd=") + (::getcwd(cwd.data(), cwd.size()) ? cwd.data() : "") + "\n";
    report += std::string("stdio_null=") +
              (is_dev_null(0) && is_dev_null(1) && is_dev_null(2) ? "1" : "0") + "\n";
    const char* env = std::getenv("NAVIGATOR_PROBE_ENV");
    report += std::string("env=") + (env != nullptr ? env : "") + "\n";
    sigset_t mask;
    ::sigprocmask(SIG_SETMASK, nullptr, &mask);
    bool empty = true;
    for (int sig = 1; sig < NSIG; ++sig) {
        if (::sigismember(&mask, sig) == 1) {
            empty = false;
        }
    }
    report += std::string("sigmask_empty=") + (empty ? "1" : "0") + "\n";
    struct sigaction current {};
    ::sigaction(SIGTERM, nullptr, &current);
    report += std::string("sigterm_default=") + (current.sa_handler == SIG_DFL ? "1" : "0") + "\n";
    std::string args;
    for (int i = 2; i < argc; ++i) {
        if (i > 2) {
            args += '|';
        }
        args += argv[i];
    }
    report += "args=" + args + "\n";

    const std::string tmp = marker + ".tmp";
    std::FILE* out = std::fopen(tmp.c_str(), "wb");
    if (out == nullptr) {
        return 3;
    }
    std::fwrite(report.data(), 1, report.size(), out);
    std::fclose(out);
    return std::rename(tmp.c_str(), marker.c_str()) == 0 ? 0 : 4;
}
