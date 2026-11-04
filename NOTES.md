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

## More than 7 processes (study, v00.49, 2026-11-04)

The limit: every Windows process is a PS4 local process (`sceSystemServiceAddLocalProcess`), and the
PS4 refuses the 8th one an app asks for (0x80aa0008); 7 ran at once in v00.43's test. Windows' own take
4 of them for good: services.exe, winedevice.exe (mountmgr.sys), explorer.exe (the desktop window) and
rpcss.exe (COM). That leaves 3 for programs, and Chromium programs start one process per page, one for
the GPU, one for the network, a crash handler...: Steam (steam.exe + steamwebhelper.exe and its
children), Chrome and its installer (GoogleUpdater's processes) all ran out.

What was looked at:
- **fork / rfork / vfork + execve work** (process probe, 2026-11-01), but those processes are not the
  app's: closing the app from the PS4 menu kills only the app's own process, and the forked ones keep
  its VM and direct memory: "gbase_free_vm_space_2 has mapped memory", a page fault in the kernel and
  the sandbox's unmount stuck (a kernel panic, v00.30-00.42). Not safe as it is.
- **pdfork** (FreeBSD 9's process descriptors, syscall 518): its child gets SIGKILL when the descriptor
  closes, that is when the app dies, so forked processes would end with it. Untested: the PS4 may not
  have the call, and if the kill comes later than the PS4's teardown of the app it is still a panic.
  Only as an opt-in experiment on a console that may crash.
- **sceSystemServiceAddLocalProcessForJvm / ForPsmKit**: other kinds of local processes, maybe with
  their own count; signatures unknown (the PS4's own browser and PSM use them).
- **Fewer of Windows' own**: rpcss.exe could start when COM needs it and end when idle (1 more slot);
  explorer.exe owns the desktop window and the shell, it stays; mountmgr is the only winedevice driver.

Done in v00.49: **Chromium in one process**. A Chromium browser's first process (chrome.exe,
msedge.exe, steamwebhelper.exe, or an exe next to libcef.dll / v8_context_snapshot.bin /
chrome_100_percent.pak: CEF and Electron programs) gets `--single-process --no-sandbox --in-process-gpu
--disable-gpu --disable-breakpad` (ntdll's NtCreateUserProcess for one a program starts, the desktop's
launch for one started from Files). Steam is then 2 processes. Settings > Apps > "Chromium programs in
one process" turns it off (WINEPS4_CHROMIUM_ONE_PROCESS=0).

Next, by cost: rpcss started on demand and ended when idle; then a pdfork probe pkg, if a console that
may panic is acceptable for the test.
