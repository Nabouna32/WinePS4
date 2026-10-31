# WinePS4 notes

## Probe v00.01 results (.90, FW 13.52, 2026-10-31) - logs/probe-v00.01.log

Works:
- Fixed mappings at 0x10000, 0x10000000, 0x7ffe0000 (KUSER_SHARED_DATA), 0x140000000, 0x180000000:
  plain `mmap(MAP_FIXED)` + `mprotect(RWX)` runs code (no special exec API needed).
- 4089 MiB below 4 GiB reserved in one call (0x6e0000-0x100000000): WoW64 address space is there.
- Faults: SIGSEGV with si_addr, trapno 12, err (write bit); SIGILL/SIGTRAP/SIGFPE. Changing rip/rax in
  the context and returning works (Wine's exception dispatch model).
- GS base: sysarch(AMD64_SET_GSBASE) per thread, kept across sleeps and threads; gs:[0x30] TEB works.
- 32-bit mode: selector 0x33 is a DPL-3 32-bit code segment; `lretq` to it ran 32-bit code and a far
  jump back to 0x43 returned. Preemption in 32-bit mode is fine. => Wine's new WoW64, no translator.
- pthread_kill(SIGUSR1) stops a thread in 31 us with its context (SuspendThread).
- 48 threads at once; socketpair + SCM_RIGHTS descriptor passing (wineserver protocol).

Problems:
- **The eboot sits at 0x400000-0x6e0000**: 32-bit exes without relocations (most pre-2010 games) need
  0x400000. The Wine eboot will be bigger. Need: load the eboot elsewhere (ET_SCE_EXEC at another
  link base?), or a tiny eboot + Wine in a module loaded high.
- **16 KiB protection granularity**: mprotect of one 4 KiB page changes the whole 16 KiB page.
  Wine must use the union of the section protections per 16 KiB page (host page size handling).
- **No exec from file mappings** (PROT_EXEC file map succeeds but faults with err 0x15): copy PE images
  into anonymous memory (Wine already can).
- **GS inside a signal handler is not the TEB** (reads 0 at gs:[0x30]; mcontext slot 93 is 0); it is
  back after the handler returns. Wine's handlers must get the TEB another way (pthread TLS) and
  re-set GS if they need it.
- No LDT (i386_get_ldt EINVAL): no 16-bit programs.
- CPU: SSE4.2 + AVX1, no AVX2/FMA/BMI2/FSGSBASE. Memory: 4608 MiB direct, ~442 MiB flexible free.
- VirtualQuery does not list system modules (libkernel etc.), so "free" ranges are only free of the
  app's own mappings; mapping MAP_FIXED there still worked for every address tried.
