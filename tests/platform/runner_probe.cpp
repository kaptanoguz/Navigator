// navigator_runner_probe: started by process_runner_test through the real IProcessRunner (and
// inside bubblewrap). One sub-command per behaviour under test:
//   exit N                 exit with status N
//   signal N               raise signal N
//   out N M                write N bytes 'o' to stdout and M bytes 'e' to stderr, exit 0
//   argv ARGS...           print each ARG on its own line
//   fds                    print the open fds, ascending, comma separated
//   stdin                  print how many bytes stdin delivered before EOF
//   pgid                   print "<pid> <pgid>"
//   env NAME               print $NAME or "<unset>"
//   pwd                    print the working directory
//   alloc BYTES            malloc and touch BYTES; exit 0 on success, 3 when malloc fails
//   spin                   burn CPU forever
//   writefile PATH N       write N bytes to PATH; exit 0, or 4 printing the errno text
//   connect PORT           TCP connect to 127.0.0.1:PORT; exit 0 on success, 5 on failure
//   group PIDFILE          fork a grandchild, write "<pid> <grandchild pid>" to PIDFILE
//                          (temp file + rename), then both sleep forever
// Plain C library only; no sanitizer runtime (see tests/platform/CMakeLists.txt).
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

void write_all(int fd, const char* data, std::size_t size) {
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return;
        }
        data += n;
        size -= static_cast<std::size_t>(n);
    }
}

void write_repeated(int fd, char ch, std::size_t count) {
    const std::string chunk(65536, ch);
    while (count > 0) {
        const std::size_t now = count < chunk.size() ? count : chunk.size();
        write_all(fd, chunk.data(), now);
        count -= now;
    }
}

[[noreturn]] void sleep_forever() {
    for (;;) {
        ::pause();
    }
}

std::size_t parse_size(const char* text) {
    return static_cast<std::size_t>(std::strtoull(text, nullptr, 10));
}

int list_fds() {
    std::vector<int> fds;
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return 2;
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
    std::string text;
    for (std::size_t i = 0; i < fds.size(); ++i) {
        text += (i == 0 ? "" : ",") + std::to_string(fds[i]);
    }
    std::printf("%s\n", text.c_str());
    return 0;
}

int do_group(const char* pidfile) {
    const pid_t child = ::fork();
    if (child < 0) {
        return 2;
    }
    if (child == 0) {
        sleep_forever();
    }
    const std::string tmp = std::string(pidfile) + ".tmp";
    FILE* file = std::fopen(tmp.c_str(), "w");
    if (file == nullptr) {
        return 2;
    }
    std::fprintf(file, "%d %d\n", static_cast<int>(::getpid()), static_cast<int>(child));
    std::fclose(file);
    if (std::rename(tmp.c_str(), pidfile) != 0) {
        return 2;
    }
    sleep_forever();
}

int do_connect(const char* port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::printf("socket: %s\n", std::strerror(errno));
        return 5;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(std::atoi(port)));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        std::printf("connect: %s\n", std::strerror(errno));
        return 5;
    }
    ::close(fd);
    return 0;
}

int do_writefile(const char* path, std::size_t count) {
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        std::printf("open: %s\n", std::strerror(errno));
        return 4;
    }
    const std::string chunk(65536, 'w');
    while (count > 0) {
        const std::size_t now = count < chunk.size() ? count : chunk.size();
        const ssize_t n = ::write(fd, chunk.data(), now);
        if (n < 0) {
            std::printf("write: %s\n", std::strerror(errno));
            return 4;
        }
        count -= static_cast<std::size_t>(n);
    }
    ::close(fd);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "exit" && argc == 3) {
        return std::atoi(argv[2]);
    }
    if (cmd == "signal" && argc == 3) {
        ::raise(std::atoi(argv[2]));
        return 2;
    }
    if (cmd == "out" && argc == 4) {
        write_repeated(1, 'o', parse_size(argv[2]));
        write_repeated(2, 'e', parse_size(argv[3]));
        return 0;
    }
    if (cmd == "argv") {
        for (int i = 2; i < argc; ++i) {
            std::printf("%s\n", argv[i]);
        }
        return 0;
    }
    if (cmd == "fds") {
        return list_fds();
    }
    if (cmd == "stdin") {
        std::size_t total = 0;
        char buffer[4096];
        ssize_t n = 0;
        while ((n = ::read(0, buffer, sizeof buffer)) > 0) {
            total += static_cast<std::size_t>(n);
        }
        std::printf("%zu\n", total);
        return 0;
    }
    if (cmd == "pgid") {
        std::printf("%d %d\n", static_cast<int>(::getpid()), static_cast<int>(::getpgrp()));
        return 0;
    }
    if (cmd == "env" && argc == 3) {
        const char* value = std::getenv(argv[2]);
        std::printf("%s\n", value != nullptr ? value : "<unset>");
        return 0;
    }
    if (cmd == "pwd") {
        char buffer[4096];
        std::printf("%s\n", ::getcwd(buffer, sizeof buffer) != nullptr ? buffer : "<error>");
        return 0;
    }
    if (cmd == "alloc" && argc == 3) {
        const std::size_t bytes = parse_size(argv[2]);
        void* block = std::malloc(bytes);
        if (block == nullptr) {
            return 3;
        }
        std::memset(block, 1, bytes);
        std::printf("%d\n", static_cast<int>(static_cast<unsigned char*>(block)[bytes - 1]));
        std::free(block);
        return 0;
    }
    if (cmd == "spin") {
        volatile unsigned long counter = 0;
        for (;;) {
            counter = counter + 1;
        }
    }
    if (cmd == "writefile" && argc == 4) {
        return do_writefile(argv[2], parse_size(argv[3]));
    }
    if (cmd == "connect" && argc == 3) {
        return do_connect(argv[2]);
    }
    if (cmd == "group" && argc == 3) {
        return do_group(argv[2]);
    }
    return 2;
}
