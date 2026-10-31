// Wine probe: what a native Wine for the PS4 would need from the console, tested one by one.
//
// Results: /data/WinePS4/probe.log (and the kernel log). Nothing is drawn: the app runs for a few
// seconds on a black screen and closes itself. Every test that could fault runs under a signal handler
// that resumes the probe, so one refusal does not end the run.
//
// What Wine needs, and the section that checks it:
//   memory at fixed low addresses (PE images at 0x400000 / 0x140000000, KUSER_SHARED_DATA at
//   0x7ffe0000, a 32-bit address space for WoW64)            - address map, fixed mappings
//   running code it wrote or mapped from a file                - fixed mappings, file mappings
//   4 KiB protection granularity (PE sections, guard pages)    - page granularity
//   faults it can see and resume with a changed context        - signals
//   the GS register for the TEB                                - GS base
//   32-bit code in a 64-bit process (new WoW64)                - 32-bit mode
//   stopping another thread to read its context (SuspendThread)- thread signals
//   Unix sockets with descriptor passing (wineserver)          - IPC
#include <cpuid.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>

#include "ps4_kit.h"

#ifndef PROBE_VERSION
#define PROBE_VERSION "00.00"
#endif

extern "C" {
int sceKernelDebugOutText(int channel, const char* text);
uint64_t sceKernelReadTsc(void);
uint64_t sceKernelGetTscFrequency(void);
struct SceKernelSwVersion {
    size_t size;
    char text[0x1C];
    uint32_t version;
};
int32_t sceKernelGetSystemSwVersion(SceKernelSwVersion* version);
int32_t sceKernelAvailableFlexibleMemorySize(size_t* size);
size_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAvailableDirectMemorySize(off_t start, off_t end, size_t align, off_t* startOut, size_t* sizeOut);
struct SceKernelVirtualQueryInfo {
    void* start;
    void* end;
    off_t offset;
    int32_t protection;
    int32_t memoryType;
    uint8_t isFlexibleMemory : 1, isDirectMemory : 1, isStack : 1, isPooledMemory : 1, isCommitted : 1;
    char name[32];
};
int32_t sceKernelVirtualQuery(const void* addr, int flags, SceKernelVirtualQueryInfo* info, size_t size);
int32_t sceKernelReserveVirtualRange(void** addr, size_t len, int flags, size_t align);
int32_t sceKernelMapNamedSystemFlexibleMemory(void** addr, size_t len, int prot, int flags, const char* name);
int32_t sceKernelMunmap(void* addr, size_t len);
int32_t sceKernelMprotect(const void* addr, size_t len, int prot);
int sysarch(int number, void* args);

// The kernel's (FreeBSD) sigaction: its own layout and signal numbers, whatever signal.h says.
struct BsdSigaction {
    void (*handler)(int, void*, void*);
    int flags;
    uint32_t mask[4];
};
int bsd_sigaction(int sig, const BsdSigaction* action, BsdSigaction* old) __asm__("sigaction");
int bsd_pthread_kill(pthread_t thread, int sig) __asm__("pthread_kill");
ssize_t bsd_sendmsg(int fd, const void* msg, int flags) __asm__("sendmsg");
ssize_t bsd_recvmsg(int fd, void* msg, int flags) __asm__("recvmsg");

long probe_call(long (*fn)(void*), void* arg, uint64_t* saved_rsp);
void probe_recover(void);
uint64_t enter32(uint64_t code32, uint64_t stack32_top, uint64_t cs32);
void probe32_back(void);
}


namespace {

// --- Results -----------------------------------------------------------------------------------

FILE* g_log = nullptr;

void emit(const char* line) {
    if (!g_log) {
        mkdir("/data/WinePS4", 0777);
        g_log = fopen("/data/WinePS4/probe.log", "w");
    }
    if (g_log) {
        fputs(line, g_log);
        fputc('\n', g_log);
        fflush(g_log);  // a crash must not take the lines before it along
    }
    char klog[600];
    snprintf(klog, sizeof klog, "[wine-probe] %s\n", line);
    sceKernelDebugOutText(0, klog);
}

__attribute__((format(printf, 1, 2))) void report(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    emit(line);
}

// The answers that decide the port, repeated at the end.
char g_verdicts[24][200];
int g_verdictCount = 0;

__attribute__((format(printf, 2, 3))) void verdict(bool ok, const char* fmt, ...) {
    char text[180];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    report("%s %s", ok ? "YES" : "NO ", text);
    if (g_verdictCount < 24)
        snprintf(g_verdicts[g_verdictCount++], sizeof g_verdicts[0], "%s %s", ok ? "YES" : "NO ", text);
}

uint64_t nowUs() {
    static const uint64_t freq = sceKernelGetTscFrequency();
    return static_cast<uint64_t>(static_cast<unsigned __int128>(sceKernelReadTsc()) * 1000000u / freq);
}

// --- Faults ------------------------------------------------------------------------------------

constexpr int kSigIll = 4, kSigTrap = 5, kSigFpe = 8, kSigBus = 10, kSigSegv = 11, kSigUsr1 = 30;
constexpr int kSaOnstack = 0x1, kSaSiginfo = 0x40;
constexpr int kMcontextOffset = 0x40;  // as ps4_crashlog.cpp
// FreeBSD amd64 mcontext, in 8-byte slots
constexpr int kRax = 7, kTrapno = 16, kErr = 19, kRip = 20, kCs = 21, kRsp = 23, kFsBase = 92, kGsBase = 93;
constexpr size_t PAGE = 16 * 1024;

enum FaultMode { kRecover, kFixProtection, kSkip };

struct Fault {
    int sig, code;
    uint64_t addr, rip, trapno, err, cs, ctxFsBase, ctxGsBase, gsAtHandler;
};

volatile int g_guardActive = 0;
volatile int g_mode = kRecover;
uint64_t g_savedRsp = 0;
uint64_t g_cs64 = 0;
Fault g_fault;
volatile int g_faults = 0;
// kFixProtection: the range made read/write again; kSkip: bytes skipped, rax to set (if g_setRax)
void* g_fixBase = nullptr;
size_t g_fixLen = 0;
int g_skipBytes = 0;
bool g_setRax = false;
uint64_t g_raxValue = 0;
bool g_readGsInHandler = false;

const int kFaultSignals[] = {kSigIll, kSigTrap, kSigFpe, kSigBus, kSigSegv};
BsdSigaction g_oldActions[5];

void restoreHandlers() {
    for (int i = 0; i < 5; i++)
        bsd_sigaction(kFaultSignals[i], &g_oldActions[i], nullptr);
}

void faultHandler(int sig, void* info, void* context) {
    auto* regs = reinterpret_cast<uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
    if (!g_guardActive) {
        // not ours: the kit's crash handler gets it when the instruction runs again
        restoreHandlers();
        return;
    }
    Fault& f = g_fault;
    f.sig = sig;
    f.code = *reinterpret_cast<int*>(static_cast<char*>(info) + 8);
    f.addr = *reinterpret_cast<uint64_t*>(static_cast<char*>(info) + 24);
    f.rip = regs[kRip];
    f.trapno = regs[kTrapno] & 0xffffffffu;
    f.err = regs[kErr];
    f.cs = regs[kCs];
    f.ctxFsBase = regs[kFsBase];
    f.ctxGsBase = regs[kGsBase];
    if (g_readGsInHandler) {
        uint64_t v = 0;
        asm volatile("mov %%gs:0x30, %0" : "=r"(v));
        f.gsAtHandler = v;
    }
    g_faults = g_faults + 1;
    switch (g_mode) {
    case kFixProtection:
        mprotect(g_fixBase, g_fixLen, PROT_READ | PROT_WRITE);
        return;
    case kSkip:
        regs[kRip] += g_skipBytes;
        if (g_setRax)
            regs[kRax] = g_raxValue;
        return;
    default:
        regs[kRsp] = g_savedRsp;
        regs[kRip] = reinterpret_cast<uint64_t>(&probe_recover);
        regs[kCs] = g_cs64;  // a fault in 32-bit mode must come back in 64-bit mode
        regs[kRax] = 0xdeadfa17;
        return;
    }
}

void installHandlers() {
    BsdSigaction action;
    memset(&action, 0, sizeof action);
    action.handler = faultHandler;
    action.flags = kSaSiginfo | kSaOnstack;
    for (int i = 0; i < 5; i++)
        bsd_sigaction(kFaultSignals[i], &action, &g_oldActions[i]);
}

// Runs fn(arg) under the handler; true when it returned without a fault (*result its value).
bool guarded(long (*fn)(void*), void* arg, long* result = nullptr, int mode = kRecover) {
    memset(&g_fault, 0, sizeof g_fault);
    g_faults = 0;
    g_mode = mode;
    g_guardActive = 1;
    const long r = probe_call(fn, arg, &g_savedRsp);
    g_guardActive = 0;
    g_mode = kRecover;
    if (result)
        *result = r;
    return mode != kRecover || g_faults == 0;
}

const char* faultText() {
    static char text[200];
    snprintf(text, sizeof text, "signal %d code %d addr %#llx rip %#llx trapno %llu err %#llx", g_fault.sig,
             g_fault.code, (unsigned long long)g_fault.addr, (unsigned long long)g_fault.rip,
             (unsigned long long)g_fault.trapno, (unsigned long long)g_fault.err);
    return text;
}

long readByte(void* p) { return *static_cast<volatile uint8_t*>(p); }
long writeByte(void* p) {
    *static_cast<volatile uint8_t*>(p) = 0x5a;
    return 0;
}
long callCode(void* p) { return reinterpret_cast<long (*)()>(p)(); }

// mov eax, 0x5a5a; ret
const uint8_t kStub64[] = {0xB8, 0x5A, 0x5A, 0x00, 0x00, 0xC3};

bool runsStub(void* code, long* result) {
    long r = 0;
    const bool ok = guarded(callCode, code, &r);
    if (result)
        *result = r;
    return ok && r == 0x5a5a;
}

// --- System ------------------------------------------------------------------------------------

void testSystem() {
    report("== system");
    SceKernelSwVersion version;
    memset(&version, 0, sizeof version);
    version.size = sizeof version;
    if (sceKernelGetSystemSwVersion(&version) == 0) {
        version.text[sizeof version.text - 1] = 0;
        report("firmware %s (%#010x)", version.text, version.version);
    }
    unsigned a, b, c, d;
    __get_cpuid(1, &a, &b, &c, &d);
    const bool sse42 = c & (1u << 20), avx = c & (1u << 28), osxsave = c & (1u << 27), fma = c & (1u << 12);
    const bool popcnt = c & (1u << 23);
    unsigned a7 = 0, b7 = 0, c7 = 0, d7 = 0;
    __get_cpuid_count(7, 0, &a7, &b7, &c7, &d7);
    const bool avx2 = b7 & (1u << 5), bmi2 = b7 & (1u << 8), fsgsbase = b7 & 1u;
    report("cpu: sse4.2 %d popcnt %d avx %d osxsave %d fma %d avx2 %d bmi2 %d fsgsbase %d", sse42, popcnt, avx,
           osxsave, fma, avx2, bmi2, fsgsbase);
    verdict(!avx2, "CPU has no AVX2: games that require AVX2 cannot run (SSE4.2 + AVX1 only)");
    report("page size %ld, cpus online %ld", sysconf(_SC_PAGESIZE), sysconf(_SC_NPROCESSORS_ONLN));
    size_t flex = 0;
    sceKernelAvailableFlexibleMemorySize(&flex);
    off_t start = 0;
    size_t largest = 0;
    sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 16384, &start, &largest);
    report("memory: direct %zu MiB (largest free %zu MiB), flexible free %zu MiB", sceKernelGetDirectMemorySize() >> 20,
           largest >> 20, flex >> 20);
    uint16_t cs, ss, ds, es, fs, gs;
    asm volatile("mov %%cs, %0" : "=r"(cs));
    asm volatile("mov %%ss, %0" : "=r"(ss));
    asm volatile("mov %%ds, %0" : "=r"(ds));
    asm volatile("mov %%es, %0" : "=r"(es));
    asm volatile("mov %%fs, %0" : "=r"(fs));
    asm volatile("mov %%gs, %0" : "=r"(gs));
    g_cs64 = cs;
    report("segments: cs %#x ss %#x ds %#x es %#x fs %#x gs %#x", cs, ss, ds, es, fs, gs);
    report("exec: %d (%s)", ps4_exec_probe(), ps4_kit_exec_source());
    report("eboot code at %p, main stack near %p, heap block at %p", reinterpret_cast<void*>(&testSystem),
           static_cast<void*>(&a), malloc(16));
}

// --- Address map -------------------------------------------------------------------------------

struct Gap {
    uint64_t start, end;
};
Gap g_lowGaps[64];
int g_lowGapCount = 0;

void testAddressMap() {
    report("== address map (every mapping; gaps below 4 GiB listed after)");
    uint64_t addr = 0, prevEnd = 0x10000;
    int regions = 0;
    while (regions < 400) {
        SceKernelVirtualQueryInfo info;
        memset(&info, 0, sizeof info);
        if (sceKernelVirtualQuery(reinterpret_cast<void*>(addr), 1, &info, sizeof info) != 0)
            break;
        const uint64_t s = reinterpret_cast<uint64_t>(info.start), e = reinterpret_cast<uint64_t>(info.end);
        if (e <= addr)
            break;
        info.name[31] = 0;
        report("map %012llx-%012llx %8llu KiB prot %#x type %d %s%s%s%s '%s'", (unsigned long long)s,
               (unsigned long long)e, (unsigned long long)((e - s) >> 10), info.protection, info.memoryType,
               info.isFlexibleMemory ? "flex " : "", info.isDirectMemory ? "direct " : "",
               info.isStack ? "stack " : "", info.isCommitted ? "" : "uncommitted ", info.name);
        if (s > prevEnd && prevEnd < 0x100000000ull && g_lowGapCount < 64)
            g_lowGaps[g_lowGapCount++] = {prevEnd, s < 0x100000000ull ? s : 0x100000000ull};
        prevEnd = e > prevEnd ? e : prevEnd;
        addr = e;
        regions++;
    }
    if (prevEnd < 0x100000000ull && g_lowGapCount < 64)
        g_lowGaps[g_lowGapCount++] = {prevEnd, 0x100000000ull};
    uint64_t freeLow = 0;
    for (int i = 0; i < g_lowGapCount; i++) {
        report("free below 4 GiB: %#llx-%#llx (%llu MiB)", (unsigned long long)g_lowGaps[i].start,
               (unsigned long long)g_lowGaps[i].end,
               (unsigned long long)((g_lowGaps[i].end - g_lowGaps[i].start) >> 20));
        freeLow += g_lowGaps[i].end - g_lowGaps[i].start;
    }
    report("%d mappings, %llu MiB unmapped below 4 GiB", regions, (unsigned long long)(freeLow >> 20));
}

bool rangeFree(uint64_t start, uint64_t len) {
    SceKernelVirtualQueryInfo info;
    memset(&info, 0, sizeof info);
    if (sceKernelVirtualQuery(reinterpret_cast<void*>(start), 1, &info, sizeof info) != 0)
        return true;  // nothing mapped at or after it
    return reinterpret_cast<uint64_t>(info.start) >= start + len;
}

// --- Fixed mappings ----------------------------------------------------------------------------

uint64_t g_lowExec = 0;  // a low address where mapped code ran (for the 32-bit test)

void testFixedAddress(const char* what, uint64_t address, size_t len) {
    void* want = reinterpret_cast<void*>(address);
    if (!rangeFree(address, len)) {
        report("%-28s %#011llx: already mapped by someone else", what, (unsigned long long)address);
        verdict(false, "%s free at %#llx", what, (unsigned long long)address);
        return;
    }
    // Wine's way: anonymous mmap MAP_FIXED, then mprotect to add execute.
    void* p = mmap(want, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    bool mapped = p == want, rw = false, rwx = false, runs = false;
    int mprotectRet = -2;
    long r = 0;
    if (mapped) {
        rw = guarded(writeByte, p) && guarded(readByte, p, &r) && r == 0x5a;
        mprotectRet = mprotect(p, len, PROT_READ | PROT_WRITE | PROT_EXEC);
        if (mprotectRet == 0) {
            memcpy(p, kStub64, sizeof kStub64);
            rwx = true;
            runs = runsStub(p, &r);
        }
        munmap(p, len);
    }
    report("%-28s %#011llx: mmap %s, rw %s, mprotect rwx %d (errno %d), code %s", what, (unsigned long long)address,
           mapped ? "ok" : "refused", rw ? "ok" : "no", mprotectRet, mprotectRet ? errno : 0,
           runs ? "runs" : (rwx ? faultText() : "-"));
    // The kit's way: system flexible memory mapped read/write/execute in one call.
    void* q = want;
    const int32_t ret = sceKernelMapNamedSystemFlexibleMemory(&q, len, 7, 0x10, "wine probe");
    bool qruns = false;
    if (ret == 0 && q == want) {
        memcpy(q, kStub64, sizeof kStub64);
        qruns = runsStub(q, &r);
        sceKernelMunmap(q, len);
    } else if (ret == 0) {
        sceKernelMunmap(q, len);
    }
    report("%-28s %#011llx: system flexible rwx %#x at %p, code %s", what, (unsigned long long)address,
           (unsigned)ret, q, qruns ? "runs" : "no");
    if ((runs || qruns) && address < 0x100000000ull && address >= 0x10000000ull && !g_lowExec)
        g_lowExec = address;
    verdict(mapped && rw, "%s mappable at %#llx (code %s)", what, (unsigned long long)address,
            runs ? "runs via mprotect" : qruns ? "runs via system flexible rwx only" : "does not run");
}

void testFixedMappings() {
    report("== fixed mappings at Windows addresses");
    testFixedAddress("lowest Windows address", 0x10000, 64 * 1024);
    testFixedAddress("32-bit exe base", 0x400000, 4 * 1024 * 1024);
    testFixedAddress("32-bit DLL base", 0x10000000, 1024 * 1024);
    testFixedAddress("KUSER_SHARED_DATA", 0x7ffe0000, 64 * 1024);
    testFixedAddress("64-bit exe base", 0x140000000ull, 4 * 1024 * 1024);
    testFixedAddress("64-bit DLL base", 0x180000000ull, 1024 * 1024);
    // Wine reserves the low address space for 32-bit programs at start (its preloader does it on Linux).
    Gap biggest = {0, 0};
    for (int i = 0; i < g_lowGapCount; i++)
        if (g_lowGaps[i].end - g_lowGaps[i].start > biggest.end - biggest.start)
            biggest = g_lowGaps[i];
    const uint64_t start = (biggest.start + PAGE - 1) & ~(PAGE - 1);
    const uint64_t len = (biggest.end - start) & ~(PAGE - 1);
    if (len) {
        void* a = reinterpret_cast<void*>(start);
        const int32_t ret = sceKernelReserveVirtualRange(&a, len, 0x10, PAGE);
        report("reserve the biggest low gap %#llx + %llu MiB: %#x at %p", (unsigned long long)start,
               (unsigned long long)(len >> 20), (unsigned)ret, a);
        if (ret == 0)
            sceKernelMunmap(a, len);
        verdict(ret == 0 && len >= (1ull << 30), "a big block below 4 GiB can be reserved (%llu MiB)",
                (unsigned long long)(len >> 20));
    }
}

// --- Page granularity --------------------------------------------------------------------------

void testGranularity() {
    report("== protection granularity (PE sections and guard pages are 4 KiB)");
    void* p = mmap(nullptr, 64 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        report("mmap failed (errno %d)", errno);
        return;
    }
    char* base = static_cast<char*>(p);
    const int ret = mprotect(base + 4096, 4096, PROT_READ);
    report("mprotect one 4 KiB page read-only: %d (errno %d)", ret, ret ? errno : 0);
    char row[160];
    int n = snprintf(row, sizeof row, "writes:");
    bool exact = true;
    for (int k = 0; k < 6; k++) {
        const bool ok = guarded(writeByte, base + k * 4096);
        n += snprintf(row + n, sizeof row - n, " +%dK %s", k * 4, ok ? "ok" : "faults");
        exact = exact && (ok == (k != 1));
    }
    report("%s", row);
    SceKernelVirtualQueryInfo info;
    memset(&info, 0, sizeof info);
    sceKernelVirtualQuery(base + 4096, 0, &info, sizeof info);
    report("the kernel's view of +4K: %p-%p prot %#x", info.start, info.end, info.protection);
    verdict(exact, "4 KiB protection granularity (else Wine must merge section protections per 16 KiB)");
    munmap(p, 64 * 1024);
}

// --- Signals -----------------------------------------------------------------------------------

long ud2Rax(void*) {
    long v = 1;
    asm volatile("ud2" : "+a"(v));
    return v;
}
long int3Op(void*) {
    asm volatile("int3");
    return 7;
}
long divZero(void*) {
    long lo = 1, hi = 0;
    asm volatile("xor %%ecx, %%ecx\n\t.byte 0x48, 0xf7, 0xf1" : "+a"(lo), "+d"(hi) : : "rcx");  // div %rcx (3 bytes)
    return lo;
}

void testSignals() {
    report("== signals (Wine turns every fault into a Windows exception and resumes with a new context)");
    // A write to a read-only page, fixed by the handler and resumed: write watches, guard pages, stacks.
    void* p = mmap(nullptr, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    mprotect(p, PAGE, PROT_READ);
    g_fixBase = p;
    g_fixLen = PAGE;
    guarded(writeByte, p, nullptr, kFixProtection);
    const bool fixed = g_faults == 1 && *static_cast<uint8_t*>(p) == 0x5a && g_fault.addr == (uint64_t)p;
    report("write to read-only page: %s; resumed after fix: %s", faultText(), fixed ? "yes" : "no");
    report("context: cs %#llx, fs base %#llx, gs base %#llx (slots 92/93)", (unsigned long long)g_fault.cs,
           (unsigned long long)g_fault.ctxFsBase, (unsigned long long)g_fault.ctxGsBase);
    verdict(fixed && g_fault.trapno == 12 && (g_fault.err & 2),
            "page faults report address, trap 12 and the write bit, and resume after the handler fixes them");
    munmap(p, PAGE);

    // ud2: skip it from the handler and change rax in the context.
    g_skipBytes = 2;
    g_setRax = true;
    g_raxValue = 0x1234;
    long r = 0;
    guarded(ud2Rax, nullptr, &r, kSkip);
    g_setRax = false;
    report("ud2: %s; returned %#lx (handler set rax 0x1234)", faultText(), r);
    verdict(g_faults == 1 && r == 0x1234, "registers changed in the signal context are used when it resumes");

    g_skipBytes = 0;
    guarded(int3Op, nullptr, &r, kSkip);
    report("int3: %s; returned %ld", faultText(), r);
    g_skipBytes = 3;
    guarded(divZero, nullptr, &r, kSkip);
    report("divide by zero: %s; returned %ld", faultText(), r);
    verdict(g_fault.sig == kSigFpe, "divide by zero arrives as SIGFPE");
}

// --- GS base -----------------------------------------------------------------------------------

constexpr int kAmd64GetGsBase = 130, kAmd64SetGsBase = 131, kI386GetLdt = 0;

long readGs30(void*) {
    long v;
    asm volatile("mov %%gs:0x30, %0" : "=r"(v));
    return v;
}
long rdgsbase(void*) {
    long v = 0;
    asm volatile(".byte 0xf3, 0x48, 0x0f, 0xae, 0xc8" : "=a"(v));  // rdgsbase %rax
    return v;
}

uint64_t* makeTeb() {
    auto* teb = static_cast<uint64_t*>(memalign(4096, 8192));  // memalign is the kit's heap (wrapped)
    memset(teb, 0, 8192);
    teb[0x30 / 8] = reinterpret_cast<uint64_t>(teb);  // NtCurrentTeb() reads gs:[0x30]
    return teb;
}

std::atomic<int> g_threadGsOk{0};

void* gsThread(void* arg) {
    uint64_t* teb = makeTeb();
    uint64_t base = reinterpret_cast<uint64_t>(teb);
    long v = 0;
    const bool ok = sysarch(kAmd64SetGsBase, &base) == 0 && guarded(readGs30, nullptr, &v) && v == (long)teb;
    // the main thread's value must not have leaked into this thread, nor this one into it
    *static_cast<long*>(arg) = v;
    g_threadGsOk = ok ? 1 : -1;
    uint64_t zero = 0;
    sysarch(kAmd64SetGsBase, &zero);
    free(teb);
    return nullptr;
}

void testGsBase() {
    report("== GS base (x64 Windows code finds its TEB at gs:[0x30])");
    uint64_t orig = 0;
    const int g = sysarch(kAmd64GetGsBase, &orig);
    report("get gs base: %d (errno %d) = %#llx", g, g ? errno : 0, (unsigned long long)orig);
    long v = 0;
    report("rdgsbase instruction: %s", guarded(rdgsbase, nullptr, &v) ? "allowed" : faultText());

    uint64_t* teb = makeTeb();
    uint64_t base = reinterpret_cast<uint64_t>(teb);
    const int s = sysarch(kAmd64SetGsBase, &base);
    const bool read = s == 0 && guarded(readGs30, nullptr, &v) && v == (long)teb;
    report("set gs base %p: %d (errno %d); gs:[0x30] %s %#lx", teb, s, s ? errno : 0, read ? "=" : "fails,", v);
    if (read) {
        // kept across sleeps and thread switches
        bool kept = true;
        for (int i = 0; i < 200 && kept; i++) {
            if (i & 1)
                sched_yield();
            else
                usleep(500);
            kept = guarded(readGs30, nullptr, &v) && v == (long)teb;
        }
        report("kept across 200 sleeps/yields: %s", kept ? "yes" : "no");
        // per thread
        long other = 0;
        pthread_t t;
        pthread_create(&t, nullptr, gsThread, &other);
        pthread_join(t, nullptr);
        const bool mine = guarded(readGs30, nullptr, &v) && v == (long)teb;
        report("other thread set its own: %s; this thread still sees its own: %s",
               g_threadGsOk == 1 ? "yes" : "no", mine ? "yes" : "no");
        // through a signal handler
        g_readGsInHandler = true;
        g_skipBytes = 2;
        guarded(ud2Rax, nullptr, nullptr, kSkip);
        g_readGsInHandler = false;
        const bool afterSignal = guarded(readGs30, nullptr, &v) && v == (long)teb;
        report("in a signal handler gs:[0x30] = %#llx (context slot %#llx); after it returns: %s",
               (unsigned long long)g_fault.gsAtHandler, (unsigned long long)g_fault.ctxGsBase,
               afterSignal ? "kept" : "lost");
        verdict(kept && g_threadGsOk == 1 && mine && afterSignal, "the GS base can hold a per-thread TEB");
    } else {
        verdict(false, "the GS base can hold a per-thread TEB");
    }
    sysarch(kAmd64SetGsBase, &orig);
    free(teb);

    // the local descriptor table (16-bit and custom segments; Wine on FreeBSD uses it for i386)
    struct {
        uint32_t start;
        void* descs;
        uint32_t num;
    } ldt = {0, nullptr, 8};
    uint64_t descs[8];
    ldt.descs = descs;
    const int l = sysarch(kI386GetLdt, &ldt);
    report("i386_get_ldt: %d (errno %d)", l, l < 0 ? errno : 0);
}

// --- 32-bit mode -------------------------------------------------------------------------------

uint32_t lar(uint32_t sel, bool* valid) {
    uint32_t ar = 0;
    uint8_t ok = 0;
    asm volatile("lar %2, %0\n\tsetz %1" : "=r"(ar), "=qm"(ok) : "r"(sel), "0"(0u) : "cc");
    *valid = ok;
    return ar;
}

struct Enter32Args {
    uint64_t code, stack, cs;
};
long runEnter32(void* raw) {
    auto* a = static_cast<Enter32Args*>(raw);
    return static_cast<long>(enter32(a->code, a->stack, a->cs));
}

void test32Bit() {
    report("== 32-bit mode (Wine's new WoW64 runs 32-bit code inside the 64-bit process)");
    uint32_t candidates[16];
    int count = 0;
    for (uint32_t index = 1; index < 16; index++) {
        const uint32_t sel = index * 8 + 3;
        bool valid = false;
        const uint32_t ar = lar(sel, &valid);
        if (!valid)
            continue;
        const bool code = ar & (1u << 11), present = ar & (1u << 15), l = ar & (1u << 21), db = ar & (1u << 22);
        const unsigned dpl = (ar >> 13) & 3;
        report("selector %#04x: %s dpl %u%s%s%s", sel, code ? "code" : "data", dpl, present ? "" : " not-present",
               code ? (l ? " 64-bit" : (db ? " 32-bit" : " 16-bit")) : "", code ? "" : (db ? " 32-bit" : ""));
        if (code && present && !l && db && dpl == 3)
            candidates[count++] = sel;
    }
    if (count == 0) {
        // nothing readable with lar: try the usual places anyway
        candidates[count++] = 0x33;
        candidates[count++] = 0x23;
        candidates[count++] = static_cast<uint32_t>(g_cs64 - 0x10);
    }
    if (!g_lowExec) {
        // none of the Windows addresses ran code: any free low range will do for this test
        for (int i = 0; i < g_lowGapCount && !g_lowExec; i++) {
            const uint64_t s = (g_lowGaps[i].start + 0xFFFFF) & ~0xFFFFFull;
            if (s + 0x10000 <= g_lowGaps[i].end && s >= 0x100000)
                g_lowExec = s;
        }
        report("32-bit test page: %#llx (from the free low ranges)", (unsigned long long)g_lowExec);
    }
    if (!g_lowExec) {
        report("no free low address: 32-bit mode not tried");
        verdict(false, "32-bit code runs in this process");
        return;
    }
    const size_t len = 64 * 1024;
    void* want = reinterpret_cast<void*>(g_lowExec);
    void* low = want;
    int32_t ret = sceKernelMapNamedSystemFlexibleMemory(&low, len, 7, 0x10, "wine probe 32");
    if (ret != 0 || low != want) {
        low = mmap(want, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (low != want || mprotect(low, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            report("could not map the 32-bit code page again at %p", want);
            verdict(false, "32-bit code runs in this process");
            return;
        }
    }
    auto* code = static_cast<uint8_t*>(low);
    const uint32_t lo = static_cast<uint32_t>(g_lowExec);
    bool any = false;
    for (int i = 0; i < count; i++) {
        memset(code, 0xCC, 256);
        size_t k = 0;
        const uint8_t prologue[] = {
            0xB8, 0x00, 0x00, 0x00, 0x00,  // mov eax, 0
            0x48,                          // 32-bit: dec eax (-> 0xffffffff); 64-bit: a REX prefix
            0x90,                          // nop
            0xB9, 0x00, 0x00, 0x00, 0x02,  // mov ecx, 0x2000000
            0xE2, 0xFE,                    // loop $ (long enough to be preempted)
            0x68, 0x78, 0x56, 0x34, 0x12,  // push 0x12345678 (on the 32-bit stack)
            0x5B,                          // pop ebx
            0xEA,                          // jmp far cs64:lo+0x40
        };
        memcpy(code + k, prologue, sizeof prologue);
        k += sizeof prologue;
        const uint32_t back = lo + 0x40;
        memcpy(code + k, &back, 4);
        k += 4;
        const uint16_t cs64 = static_cast<uint16_t>(g_cs64);
        memcpy(code + k, &cs64, 2);
        // at lo+0x40, 64-bit: movabs rcx, probe32_back; jmp rcx
        uint8_t* stub = code + 0x40;
        stub[0] = 0x48;
        stub[1] = 0xB9;
        const uint64_t target = reinterpret_cast<uint64_t>(&probe32_back);
        memcpy(stub + 2, &target, 8);
        stub[10] = 0xFF;
        stub[11] = 0xE1;
        Enter32Args args = {g_lowExec, g_lowExec + len - 64, candidates[i]};
        long r = 0;
        const uint64_t t0 = nowUs();
        const bool ok = guarded(runEnter32, &args, &r);
        const uint64_t us = nowUs() - t0;
        const bool is32 = ok && (r & 0xffffffff) == 0xffffffff && (static_cast<uint64_t>(r) >> 32) == 0x12345678;
        report("far jump to cs %#x: %s, result %#lx (%s), %llu us", candidates[i], ok ? "returned" : faultText(), r,
               is32 ? "ran as 32-bit code" : "not 32-bit", (unsigned long long)us);
        if (is32) {
            any = true;
            break;
        }
    }
    if (low == want)
        munmap(low, len);
    verdict(any, "32-bit code runs in this process (32-bit games without a translator)");
}

// --- Thread signals ----------------------------------------------------------------------------

std::atomic<int> g_spinRun{1}, g_inHandler{0}, g_release{0};
std::atomic<uint64_t> g_spinCount{0};
uint64_t g_suspendedRip = 0;

void suspendHandler(int, void*, void* context) {
    auto* regs = reinterpret_cast<uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
    g_suspendedRip = regs[kRip];
    g_inHandler = 1;
    while (!g_release.load())
        asm volatile("pause");
    g_inHandler = 0;
}

void* spinner(void*) {
    while (g_spinRun.load(std::memory_order_relaxed))
        g_spinCount.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

std::atomic<int> g_parked{0};
std::atomic<int> g_unpark{0};
void* parked(void*) {
    g_parked.fetch_add(1);
    while (!g_unpark.load())
        usleep(1000);
    return nullptr;
}

void testThreads() {
    report("== threads (SuspendThread / GetThreadContext stop a thread with a signal)");
    BsdSigaction action, old;
    memset(&action, 0, sizeof action);
    action.handler = suspendHandler;
    action.flags = kSaSiginfo | kSaOnstack;
    bsd_sigaction(kSigUsr1, &action, &old);
    pthread_t t;
    pthread_create(&t, nullptr, spinner, nullptr);
    usleep(20000);
    const uint64_t t0 = nowUs();
    const int k = bsd_pthread_kill(t, kSigUsr1);
    while (!g_inHandler.load() && nowUs() - t0 < 1000000)
        sched_yield();
    const uint64_t latency = nowUs() - t0;
    const bool stopped = g_inHandler.load();
    const uint64_t c1 = g_spinCount.load();
    usleep(20000);
    const uint64_t c2 = g_spinCount.load();
    report("pthread_kill: %d; in its handler after %llu us: %s; rip %#llx; counter still while stopped: %s", k,
           (unsigned long long)latency, stopped ? "yes" : "no", (unsigned long long)g_suspendedRip,
           c1 == c2 ? "yes" : "no");
    g_release = 1;
    g_spinRun = 0;
    pthread_join(t, nullptr);
    bsd_sigaction(kSigUsr1, &old, nullptr);
    verdict(stopped && c1 == c2, "another thread can be stopped and its context read (SuspendThread)");

    // Windows games start many threads.
    constexpr int kWanted = 48;
    pthread_t threads[kWanted];
    int made = 0;
    for (; made < kWanted; made++)
        if (pthread_create(&threads[made], nullptr, parked, nullptr) != 0)
            break;
    const uint64_t w0 = nowUs();
    while (g_parked.load() < made && nowUs() - w0 < 2000000)
        usleep(1000);
    g_unpark = 1;
    for (int i = 0; i < made; i++)
        pthread_join(threads[i], nullptr);
    report("threads started at once: %d of %d", made, kWanted);
    verdict(made == kWanted, "%d threads at once", kWanted);
}

// --- IPC ---------------------------------------------------------------------------------------

struct BsdIovec {
    void* base;
    size_t len;
};
struct BsdMsghdr {
    void* name;
    uint32_t namelen;
    BsdIovec* iov;
    int iovlen;
    void* control;
    uint32_t controllen;
    int flags;
};

void testIpc() {
    report("== IPC (wineserver talks over Unix sockets and passes descriptors)");
    int sv[2] = {-1, -1}, other[2] = {-1, -1};
    const int r = socketpair(1 /*AF_UNIX*/, 1 /*SOCK_STREAM*/, 0, sv);
    const int r2 = socketpair(1, 1, 0, other);
    report("socketpair: %d (errno %d), %d", r, r ? errno : 0, r2);
    if (r != 0 || r2 != 0) {
        verdict(false, "Unix socket pairs");
        return;
    }
    // send other[0] across with SCM_RIGHTS (FreeBSD layout: 16-byte header, then the fd)
    alignas(8) uint8_t control[24];
    memset(control, 0, sizeof control);
    const uint32_t cmsgLen = 16 + 4;
    memcpy(control, &cmsgLen, 4);
    const int level = 0xffff /*SOL_SOCKET*/, type = 1 /*SCM_RIGHTS*/;
    memcpy(control + 4, &level, 4);
    memcpy(control + 8, &type, 4);
    memcpy(control + 16, &other[0], 4);
    char byte = 'w';
    BsdIovec iov = {&byte, 1};
    BsdMsghdr msg = {nullptr, 0, &iov, 1, control, sizeof control, 0};
    const ssize_t sent = bsd_sendmsg(sv[0], &msg, 0);
    const int sendErr = sent < 0 ? errno : 0;
    alignas(8) uint8_t rcontrol[24];
    memset(rcontrol, 0, sizeof rcontrol);
    char rbyte = 0;
    BsdIovec riov = {&rbyte, 1};
    BsdMsghdr rmsg = {nullptr, 0, &riov, 1, rcontrol, sizeof rcontrol, 0};
    const ssize_t got = sent == 1 ? bsd_recvmsg(sv[1], &rmsg, 0) : -1;
    int passed = -1;
    memcpy(&passed, rcontrol + 16, 4);
    bool works = false;
    if (got == 1 && passed >= 0) {
        const char ping = 'p';
        char pong = 0;
        works = write(passed, &ping, 1) == 1 && read(other[1], &pong, 1) == 1 && pong == 'p';
        close(passed);
    }
    report("sendmsg %zd (errno %d), recvmsg %zd, received fd %d, works through it: %s", sent, sendErr, got, passed,
           works ? "yes" : "no");
    verdict(works, "descriptors pass over Unix sockets (wineserver protocol)");
    close(sv[0]);
    close(sv[1]);
    close(other[0]);
    close(other[1]);
}

// --- Files -------------------------------------------------------------------------------------

void testFiles() {
    report("== files");
    FILE* f = fopen("/data/WinePS4/CaseTest.TXT", "w");
    if (f) {
        fputs("x", f);
        fclose(f);
    }
    struct stat st;
    const bool insensitive = stat("/data/WinePS4/casetest.txt", &st) == 0;
    report("/data is case-%s", insensitive ? "insensitive" : "sensitive (Wine handles that itself)");
    unlink("/data/WinePS4/CaseTest.TXT");

    // PE images are mapped from their files; code from a file mapping
    uint8_t page[PAGE];
    memset(page, 0xCC, sizeof page);
    memcpy(page, kStub64, sizeof kStub64);
    const int fd = open("/data/WinePS4/exec-test.bin", O_RDWR | O_CREAT | O_TRUNC, 0666);
    bool wrote = fd >= 0 && write(fd, page, sizeof page) == (ssize_t)sizeof page;
    void* m = wrote ? mmap(nullptr, PAGE, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    const int mapErr = m == MAP_FAILED ? errno : 0;
    long r = 0;
    const bool runs = m != MAP_FAILED && runsStub(m, &r);
    report("file mapped read+execute: %s (errno %d), code %s", m == MAP_FAILED ? "refused" : "ok", mapErr,
           runs ? "runs" : (m == MAP_FAILED ? "-" : faultText()));
    void* w = wrote ? mmap(nullptr, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    report("file mapped private read+write: %s", w == MAP_FAILED ? "refused" : "ok");
    verdict(runs, "code runs straight from a file mapping (else Wine copies images into exec memory)");
    if (m != MAP_FAILED)
        munmap(m, PAGE);
    if (w != MAP_FAILED)
        munmap(w, PAGE);
    if (fd >= 0)
        close(fd);
    unlink("/data/WinePS4/exec-test.bin");
}

}  // namespace

int main() {
    report("Wine probe v" PROBE_VERSION);
    const uint64_t t0 = nowUs();
    installHandlers();
    testSystem();
    testAddressMap();
    testFixedMappings();
    testGranularity();
    testSignals();
    testGsBase();
    test32Bit();
    testThreads();
    testIpc();
    testFiles();
    restoreHandlers();
    report("== summary (%llu ms)", (unsigned long long)((nowUs() - t0) / 1000));
    for (int i = 0; i < g_verdictCount; i++)
        report("%s", g_verdicts[i]);
    report("== done");
    return 0;
}
