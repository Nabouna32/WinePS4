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

## The app's budget, and the 7 again (v00.50, 2026-11-04)

v00.49's log showed two walls: dllhost.exe and steamservice.exe refused at 7 processes (Chrome's
leftover GoogleUpdater service started 3-4 processes at every boot), and Steam's web helper dying on
its own DLLs with "Too many open files" while it had 60 descriptors (a new process: 12), although
every process may have 12556. The PS5_MihawkOS project (Wine on the PS5, same Sony services:
github.com/mihawk-99/PS5_MihawkOS, docs/WINDOWS_PROCESSES.md and its history) measured both:

- **One budget of about 250 open files for an app and all its local processes**, folders included. A
  file counts once however many processes hold it; dups, copies passed over a socket, sockets, pipes,
  kqueues, anonymous shared memory and mapped files whose descriptor is closed cost nothing. It is the
  kernel's budget: Orbis budgets per app Dmem, Vmem, Fmem, CpuSet, **File**, Socket, Equeue, Pipe,
  Device, Thread, IpSocket, each with a total and its use (RPCSX's Budget.hpp; budget_getid 609,
  budget_get 570, budget_set 571).
- **7 local processes for an app**, a count (128 MB children stop at the same number), nested or not
  (a local process asking gets the same "with 7 alive"). Past it on the PS5: processes started by the
  jailbreak's payload loader (elfldr: rfork from its own process, the child leaves its budget with
  budget_set(0)), outside every app: 32 ran at once, each with its own file budget (636), ending
  themselves when their stream closed.

v00.50 does what the measurements say: wineserver **parks** idle files (server/fd.c: beyond 128
parkable ones the least recently used are closed and opened again by name at the same file pointer;
not while locked, polled, lent to a program until that thread's next request, or when a dup's partner
is open) and programs no longer cache file descriptors of files and folders; each process's folder log
is opened per use (2 files for good in each before). Chrome's updater starts on demand; Steam's
start-up probes, install scripts and overlay are refused (Settings > Apps); wine.log has the kernel's
budget table at start and in every census, and a probe: whether a vfork child of the app may
budget_set(0) (it then ends at once).

Past 7 on the PS4, if the probe says a child may leave the budget: a process forked from the app that
leaves it, runs from a copy of wineps4.bin outside /app0 (the sandbox's unmount waited for ours, v00.4x),
uses no direct memory and ends itself when its control socket closes; the panic at the app's close
(v00.30-00.42) is the test it has to pass, on a console that may crash.

## Past the 7, and Chromium's 32 GiB (v00.51, 2026-11-04)

v00.50's log: the probe's child left the app's budget (budget_set 0 gave 0, its budget then 0);
steamservice.exe was refused at 7 twice; Steam's web helper died at every start in chrome_elf.dll:
`mmap 0+0x800000000 prot 0 failed: Out of memory`. Chromium reserves 32 GiB less 64 KiB for its
memory pools, and without an address the PS4 looks only in its own part of the address space below
32 GiB (0x400000-0x7ffffc000, shadPS4's SYSTEM_MANAGED area; the app's own starts at 64 GiB,
0x1000000000, and ends at 0xfc00000000, where the VM layer's probe found its limit): it never fits.

v00.51:
- port/ps4_wine_vm.c: a reservation the kernel refuses without an address goes down from the top of
  the app's part, a GiB at a time, in pieces of 4 GiB at most, each where it was asked for (fixed
  reservations in pieces too, unmapped in pieces); wine.log says so each time. A GiB reserved whole
  has no page table until a page of it is backed (a table is 256 KiB a GiB: 8 MiB for Chromium's).
- Processes outside the PS4's 7 (port/ps4_spawn_desk.c, port/ps4_wine_port.c ps4_spawn_outside):
  the desktop vforks, the child keeps only its socket to the desktop (descriptor 3; every other one
  closed one by one, no closefrom: an unknown system call is a SIGSYS), leaves the app's budget,
  execs /app0/wineps4.bin --wineps4-outside, and takes the same hand-off as a local process (HELLO,
  arguments/environment/descriptors, READY, STAT every second, EXIT). Windows' background programs
  and helpers start there first (services, winedevice, rpcss, plugplay, svchost, dllhost, msiexec,
  conhost, the command-line tools; Steam's steamservice and error reporters; Google's updater and
  crash handlers; games' crash reporters), any program once the 7 run. The 7 then hold explorer.exe
  and the programs: Steam, its web helper and a game have room. A first one that does not start is
  tried again in the app's budget, then none start outside (wine.log says each).
- What made the forked processes of v00.30-00.42 bring the PS4 down was the app's direct memory in
  processes the system doesn't count as the app's (klog: "gbase_free_vm_space_2 has mapped memory
  vmid:1" while the app was suspended, then "Fatal trap 12"; they all ran in vm#1/dmem#1). So these
  map none: the program heap (ps4-kit's new regular_only option: no direct memory, no system pool,
  no overflow heaps; 96 MiB at most) and every Wine page (256 MiB at most, WINEPS4_OUTSIDE_MAX_MB)
  come from regular flexible memory, which nothing limits there but the console (a forked child once
  asked for 4 GiB of it and the PS4 froze). They tell they are outside by their parent: the desktop
  writes its pid to /data/WinePS4/.desktop-pid; local processes are the system's children.
- They end with the desktop: their socket's end (the desktop gone, however) makes them exit at once;
  Power > Exit kills them with the rest. Closing from the PS4 menu is the test they have to pass.
- Settings > Apps > "More than 7 processes (test)" (WINEPS4_OUTSIDE=0 turns it off); Task Manager
  marks them "(outside the 7)" and its Performance page counts both.

## Steam's window, and the outside processes' memory (v00.52, 2026-11-04)

v00.51's log (logs/v00.51, with Steam's own logs): the processes outside the 7 started (services,
winedevice and rpcss outside, budget 0, "regular flexible memory runs code"), and Chromium's 32 GiB
reservations went high (four at 0xf3c0000000 and below). Steam's web helper then ran: it made its
window, loaded Steam's UI (steamloopback.host/index.html) and tried to reach steam.exe over a
websocket on 127.0.0.1, which steam.exe rejected every time (transport_steamui.txt: "Websocket
connection from: https://steamloopback.host ... TCP connection request ... Connection rejected"),
until it gave up and restarted itself. steam.exe takes the connection only from its own web helper,
whose process it finds in the TCP table (GetExtendedTcpTable's owning pid; Wine's wineserver keeps
that table, get_tcp_connections), and the table comes through nsiproxy.sys, whose Unix half was not
built: every program also had "GetAdaptersAddresses failed: 2".

The processes outside the 7 share one pool of regular flexible memory, ~420 MiB at the start, the
local processes have their own (803 MiB each): services.exe took ~160 MiB of it (a 96 MiB heap,
mapped whole), winedevice ~130, and rpcss.exe (COM), steamservice.exe and Steam's error reporter
found 29, 5 and 7 MiB: none started.

v00.52:
- nsiproxy.so built in (app/CMakeLists.txt), with port/ps4_ifaddrs.c: getifaddrs, if_nameindex,
  if_nametoindex, if_indextoname from FreeBSD's SIOCGIFCONF (the C library's are musl's and ask a
  netlink socket the PS4 doesn't have), FreeBSD's ioctl numbers through the raw system call (the
  headers have Linux's), and ndis.c's hardware address from the link level entries (Wine patch).
  The service starts with Windows again (it was on demand since v00.44), in MountMgr's
  winedevice.exe (same group): no process more.
- The outside processes' heap: 16 MiB, then overflow heaps of 16 MiB of regular flexible memory as
  it fills (ps4-kit: regular_only now has them; a bigger request gets one its size), 7 at most.
- Each outside process sends the pool's free memory with its figures; below 80 MiB no other starts
  outside: it goes among the 7.

## The crash: the PS4's own memory (v00.53, 2026-11-04)

v00.52 closed itself 72 s in, with no crash.log and wine.log cut short. GoldHEN's kernel log
(logs/v00.52/klog.txt) says why: a new local process (pid 395, Steam's) died as the PS4's C library
started in it, "# reason: Failed to create the system memory" (0xa002008f, ENOMEM), and the PS4
closes the whole app when one of its processes crashes (AppCrash, a core dump, every local process
killed, then the desktop). SceShellCore's memory table just before it listed the processes outside
the 7 as the system's (no title id), 107-119 MiB each: four of them had taken the pool the PS4
itself lives on down to 13 MiB (their own figure). Good news in the same log: they held none of the
app's direct memory, they all ended with the desktop, and the close was clean (no gbase panic).
nsiproxy loaded (eth0, lo0, wlan0, wlan1), GetAdaptersAddresses then failed with 1 instead of 2: the
IPv4 route table's "not implemented"; and it fails as a whole too when DnsQueryConfig has no servers
(dnsapi had no Unix half).

v00.53:
- The desktop's C heap in direct memory (384 MiB at most), out of the system's pool.
- Outside processes: no more of their memory once the pool would go below 128 MiB (port/
  ps4_wine_vm.c), a new one only with 256 MiB of it free, 3 overflow heaps at most; fresh flexible
  memory is no longer zero-filled by hand (the kernel's is zero; writing it made every page backed
  the console's at once); each one's memory in wine.log as it ends.
- nsiproxy's IPv4 route table (FreeBSD's routing sysctl through the raw system call, else the
  interfaces' networks and the PS4's gateway) and dnsapi's Unix half (dlls/dnsapi/ps4.c: DNS
  servers from sceNetCtl, A queries through getaddrinfo as DNS messages), so GetAdaptersAddresses
  succeeds. NetCtl is loaded first (ps4-kit: a call into an unloaded module ends the app); not in
  processes outside the 7.
