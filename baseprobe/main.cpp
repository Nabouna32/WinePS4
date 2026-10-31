// WinePS4 base probe: can the app's own code live somewhere other than 0x400000? 32-bit Windows
// programs without relocations (SteamSetup.exe, NSIS installers) must load at exactly 0x400000,
// where the PS4 puts every eboot of type ET_SCE_DYNEXEC (0xFE10, what create-fself writes). Two
// reimplementations of the PS4 kernel (fpPS4, Obliteration) map an ET_SCE_EXEC (0xFE00) image at
// the addresses it was linked for instead.
//
// One package, three eboots: eboot.bin (the launcher, built as usual) starts high-<n>.bin with
// sceSystemServiceLoadExec, one per run of the app: linked at 0x60000000, then at 0x20000000 (code
// above 2 GiB could not reach ps4-kit's weak hooks at 0), each patched to 0xFE00 (build.sh). The launcher notes which
// one it starts before it does, so one that never runs is seen as such, and the next run tries the
// next. Everything goes to /data/WinePS4/baseprobe.log.
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef PROBE_ROLE
#define PROBE_ROLE "launcher"
#endif

extern "C" {
int sceSystemServiceLoadExec(const char *path, const char *args[]);
int32_t sceKernelQueryMemoryProtection(void *addr, void **start, void **end, uint32_t *prot);
int32_t sceKernelMapNamedSystemFlexibleMemory(void **addr, size_t len, int32_t prot, int32_t flags, const char *name);
void *sceKernelGetProcParam(void);
int32_t sceKernelGetCompiledSdkVersion(uint32_t *version);
uint64_t enter32(uint64_t code32, uint64_t stack32_top, uint64_t cs32);
void probe32_back(void);
extern char __text_start[];
}

namespace {

const char *const LOG = "/data/WinePS4/baseprobe.log";
const char *const NEXT = "/data/WinePS4/baseprobe-next.txt";
const char *const VARIANTS[] = {"/app0/high-1g.bin", "/app0/high-512m.bin"};

FILE *log_file;

void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(log_file, fmt, args);
    va_end(args);
    std::fputc('\n', log_file);
    std::fflush(log_file);  // (a crash right after still leaves the line)
    fsync(fileno(log_file));
}

thread_local int tls_value = 1234;

int launcher() {
    int next = 0;
    if (FILE *f = std::fopen(NEXT, "r")) {
        if (std::fscanf(f, "%d", &next) != 1)
            next = 0;
        std::fclose(f);
    }
    if (next >= static_cast<int>(sizeof(VARIANTS) / sizeof(VARIANTS[0]))) {
        say("launcher: every variant has been tried (delete %s to try them again)", NEXT);
        return 0;
    }
    if (FILE *f = std::fopen(NEXT, "w")) {
        std::fprintf(f, "%d\n", next + 1);
        std::fclose(f);
    }
    say("launcher: eboot at %p; starting %s (if no line from it follows, it did not start)", static_cast<void *>(__text_start),
        VARIANTS[next]);
    const int r = sceSystemServiceLoadExec(VARIANTS[next], nullptr);
    say("launcher: sceSystemServiceLoadExec returned %08x", static_cast<unsigned>(r));
    return 0;
}

#ifdef PROBE_BASE
int high() {
    say("%s: running; code at %p (linked for %#llx), main at %p", PROBE_ROLE, static_cast<void *>(__text_start),
        static_cast<unsigned long long>(PROBE_BASE), reinterpret_cast<void *>(&high));
    void *param = sceKernelGetProcParam();
    uint32_t sdk = 0;
    const int32_t got = sceKernelGetCompiledSdkVersion(&sdk);
    say("%s: process param at %p, compiled SDK version %08x (%08x)", PROBE_ROLE, param, sdk, static_cast<unsigned>(got));
    try {
        throw std::runtime_error("thrown");
    } catch (const std::exception &e) {
        say("%s: C++ exception caught (%s)", PROBE_ROLE, e.what());
    }
    int seen = 0;
    std::thread([&] { seen = tls_value + 1; }).join();
    say("%s: thread-local storage %d, in a new thread %d", PROBE_ROLE, tls_value, seen);

    void *start = nullptr, *end = nullptr;
    uint32_t prot = 0;
    const int32_t q = sceKernelQueryMemoryProtection(reinterpret_cast<void *>(0x400000), &start, &end, &prot);
    say("%s: 0x400000 is %s (%08x: %p-%p prot %u)", PROBE_ROLE, q == 0 ? "MAPPED" : "free", static_cast<unsigned>(q), start, end, prot);

    // 32-bit code at 0x400000, as a 32-bit program without relocations would be
    const size_t len = 4 << 20;
    void *want = reinterpret_cast<void *>(0x400000), *low = want;
    int32_t m = sceKernelMapNamedSystemFlexibleMemory(&low, len, 7, 0x10 /* fixed */, "base probe 32");
    if (m != 0 || low != want) {
        say("%s: system flexible RWX at 0x400000: %08x at %p; trying mmap", PROBE_ROLE, static_cast<unsigned>(m), low);
        low = mmap(want, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (low != want || mprotect(low, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            say("%s: NO: 0x400000 could not be mapped (%p, errno %d)", PROBE_ROLE, low, errno);
            return 0;
        }
    }
    auto *code = static_cast<uint8_t *>(low);
    std::memset(code, 0xcc, 256);
    const uint8_t prologue[] = {
        0xb8, 0x00, 0x00, 0x00, 0x00,  // mov eax, 0
        0x48,                          // 32-bit: dec eax (-> 0xffffffff); 64-bit: a REX prefix
        0x90,                          // nop
        0x68, 0x78, 0x56, 0x34, 0x12,  // push 0x12345678 (on the 32-bit stack)
        0x5b,                          // pop ebx
        0xea,                          // jmp far cs64:0x400040
    };
    size_t k = sizeof prologue;
    std::memcpy(code, prologue, k);
    const uint32_t back = 0x400040;
    std::memcpy(code + k, &back, 4);
    uint16_t cs64 = 0;
    __asm__("movw %%cs, %0" : "=r"(cs64));
    std::memcpy(code + k + 4, &cs64, 2);
    uint8_t *stub = code + 0x40;  // 64-bit: movabs rcx, probe32_back; jmp rcx
    stub[0] = 0x48;
    stub[1] = 0xb9;
    const uint64_t target = reinterpret_cast<uint64_t>(&probe32_back);
    std::memcpy(stub + 2, &target, 8);
    stub[10] = 0xff;
    stub[11] = 0xe1;
    say("%s: 32-bit code at 0x400000, entering it (cs 0x33)", PROBE_ROLE);
    const uint64_t r = enter32(0x400000, 0x400000 + len - 64, 0x33);
    const bool ok = (r & 0xffffffff) == 0xffffffff && (r >> 32) == 0x12345678;
    say("%s: %s: result %#llx", PROBE_ROLE, ok ? "YES, 32-bit code ran at 0x400000" : "NO, wrong result",
        static_cast<unsigned long long>(r));
    return 0;
}
#endif

} // namespace

int main() {
    mkdir("/data/WinePS4", 0777);
    log_file = std::fopen(LOG, "a");
    if (!log_file)
        return 1;
    std::time_t t = std::time(nullptr);
    say("==== %s, %s", PROBE_ROLE, std::ctime(&t));
#ifdef PROBE_BASE
    return high();
#else
    return launcher();
#endif
}
