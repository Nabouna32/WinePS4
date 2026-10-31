// WinePS4 process probe: can the app start processes of its own? Steam runs several (steam.exe, a
// few steamwebhelper.exe), every game is another, and Wine gives each Windows process a Unix one.
//
// eboot.bin (this file, PROBE_ROLE "parent") tries fork, rfork and fork + execve of child.bin (the
// same source, PROBE_ROLE "child", another fself in the package), then the network Steam needs (DNS,
// a TCP connection to port 443). Children use raw system calls only (no C library: its state was
// copied mid-flight). Everything goes to /data/WinePS4/processprobe.log.
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PROBE_ROLE
#define PROBE_ROLE "parent"
#endif

namespace {

const char *const LOG = "/data/WinePS4/processprobe.log";

// a FreeBSD system call: the result, or -errno
long sys(long n, long a = 0, long b = 0, long c = 0, long d = 0) {
    long ret;
    unsigned char failed;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall\n\tsetc %1"
                     : "=a"(ret), "=r"(failed)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r8", "r9", "r11", "memory", "cc");
    return failed ? -ret : ret;
}

// fork and rfork: FreeBSD returns the other side's pid in both, and rdx = 1 in the child (the C
// library turns that into 0): 0 in the child here, the child's pid in the parent, or -errno
long sys_fork(long n, long a = 0) {
    long ret, child = 0;
    unsigned char failed;
    __asm__ volatile("syscall\n\tsetc %2"
                     : "=a"(ret), "+d"(child), "=r"(failed)
                     : "a"(n), "D"(a)
                     : "rcx", "rsi", "r8", "r9", "r10", "r11", "memory", "cc");
    if (failed)
        return -ret;
    return child ? 0 : ret;
}

constexpr long SYS_EXIT = 1, SYS_FORK = 2, SYS_WRITE = 4, SYS_OPEN = 5, SYS_CLOSE = 6, SYS_WAIT4 = 7, SYS_GETPID = 20,
               SYS_EXECVE = 59, SYS_RFORK = 251, SYS_NANOSLEEP = 240;

// a line to the log with system calls only (children)
void raw_line(const char *text) {
    const long fd = sys(SYS_OPEN, reinterpret_cast<long>(LOG), O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;
    sys(SYS_WRITE, fd, reinterpret_cast<long>(text), static_cast<long>(std::strlen(text)));
    sys(SYS_CLOSE, fd);
}

void raw_number_line(const char *what, long n) {
    char line[160];
    std::snprintf(line, sizeof line, "%s %ld\n", what, n);  // (snprintf touches no shared state)
    raw_line(line);
}

FILE *log_file;
void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(log_file, fmt, args);
    va_end(args);
    std::fputc('\n', log_file);
    std::fflush(log_file);
    fsync(fileno(log_file));
}

// waits up to 3 s for the child; its status, or -1
int wait_child(long pid) {
    for (int i = 0; i < 60; i++) {
        int status = 0;
        const long r = sys(SYS_WAIT4, pid, reinterpret_cast<long>(&status), 1 /* WNOHANG */, 0);
        if (r == pid)
            return status;
        if (r < 0) {
            say("  wait4: errno %ld", -r);
            return -1;
        }
        const timespec pause = {0, 50000000};
        sys(SYS_NANOSLEEP, reinterpret_cast<long>(&pause), 0);
    }
    say("  the child did not end within 3 s");
    return -1;
}

void try_fork(const char *name, long number, long arg) {
    std::fflush(log_file);
    const long pid = sys_fork(number, arg);
    if (pid == 0) {  // the child
        raw_number_line("  child: running, pid", sys(SYS_GETPID));
        sys(SYS_EXIT, 7);
        for (;;) {
        }
    }
    if (pid < 0) {
        say("%s: NO (errno %ld)", name, -pid);
        return;
    }
    say("%s: child pid %ld", name, pid);
    const int status = wait_child(pid);
    say("%s: %s (status %#x)", name, status >= 0 && ((status >> 8) & 0xff) == 7 ? "YES, the child ran and ended" : "unclear", status);
}

void try_exec() {
    std::fflush(log_file);
    static const char *const argv[] = {"child.bin", "hello-from-parent", nullptr};
    static const char *const envp[] = {"WINEPS4_PROBE=1", nullptr};
    const long pid = sys_fork(SYS_FORK);
    if (pid == 0) {
        const long r = sys(SYS_EXECVE, reinterpret_cast<long>("/app0/child.bin"), reinterpret_cast<long>(argv),
                           reinterpret_cast<long>(envp));
        raw_number_line("  child: execve failed, errno", -r);
        sys(SYS_EXIT, 9);
        for (;;) {
        }
    }
    if (pid < 0) {
        say("fork + execve: no fork (errno %ld)", -pid);
        return;
    }
    say("fork + execve /app0/child.bin: child pid %ld", pid);
    const int status = wait_child(pid);
    const int code = (status >> 8) & 0xff;
    say("fork + execve: %s (status %#x)",
        status < 0 ? "unclear" : code == 42 ? "YES, child.bin ran with its arguments" : code == 9 ? "NO, execve failed" : "child.bin ended oddly",
        status);
}

void try_network() {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    const int g = getaddrinfo("store.steampowered.com", "443", &hints, &res);
    if (g != 0 || !res) {
        say("network: DNS for store.steampowered.com failed (%d)", g);
        return;
    }
    char ip[64] = "?";
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(res->ai_addr)->sin_addr, ip, sizeof ip);
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    const int c = s < 0 ? -1 : connect(s, res->ai_addr, res->ai_addrlen);
    say("network: store.steampowered.com is %s; TCP connection to port 443: %s (errno %d)", ip, c == 0 ? "YES" : "NO", c == 0 ? 0 : errno);
    if (s >= 0)
        close(s);
    freeaddrinfo(res);
}

} // namespace

int main(int argc, char **argv) {
#ifdef PROBE_CHILD
    // child.bin, started by execve: says what it got, ends with 42
    char line[200];
    std::snprintf(line, sizeof line, "  child.bin: running, pid %ld, %d argument(s), argv[1] %s\n", sys(SYS_GETPID), argc,
                  argc > 1 ? argv[1] : "(none)");
    raw_line(line);
    sys(SYS_EXIT, 42);
    return 42;
#else
    mkdir("/data/WinePS4", 0777);
    log_file = std::fopen(LOG, "a");
    if (!log_file)
        return 1;
    std::time_t t = std::time(nullptr);
    say("==== process probe, pid %d, uid %d, %s", getpid(), getuid(), std::ctime(&t));
    try_fork("fork", SYS_FORK, 0);
    try_fork("rfork(RFPROC|RFFDG)", SYS_RFORK, 0x10 | 0x4);
    try_exec();
    try_network();
    say("done");
    return 0;
#endif
}
