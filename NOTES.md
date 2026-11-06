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

## Steam's login: ECDSA certificates (v00.54, 2026-11-05)

v00.53 ran without a crash and Steam showed its login page (logs/v00.53): the UI's websocket to
steam.exe opened (nsiproxy's TCP table), GetAdaptersAddresses no longer failed (routes, DNS servers
192.168.0.1 from sceNetCtl), the outside pool started at 656 MiB with the desktop's heap in direct
memory. The login failed: Steam's connectivity test and its server list (HTTPS) worked, but every
connection to its login servers (WebSocket CMs, cmp*.steamserver.net) failed within a second, and
the console log said why: "Crypto API failed certificate check, error flags 0x00000008"
(CERT_TRUST_IS_NOT_SIGNATURE_VALID). Their chain is Let's Encrypt's ECDSA one (leaf P-256, YE1 and
Root YE P-384, ecdsa-with-SHA384, up to ISRG Root X2 and X1, both in the package's roots). Wine's
crypt32 checks RSA signatures itself (rsaenh) but ECDSA ones through bcrypt's Unix half, which is
GnuTLS's and not built: "key_asymmetric_create no encryption support".

v00.54: bcrypt's Unix half for the PS4 on the sysroot's OpenSSL 3.5 libcrypto (dlls/bcrypt/ps4.c,
Wine patch): RSA and ECC public keys imported, ECDSA and RSA (PKCS #1, PSS) signatures verified, keys
duplicated and destroyed, AES CBC/ECB/CFB8/GCM (GCM's tag on decryption from the plaintext encrypted
again). Key generation, private keys, signing, RSA encryption, export and key agreement return
STATUS_NOT_IMPLEMENTED. The eboot grows from 13 to 18.7 MB.

## Socket option numbers, and the USB keyboard (v00.55, 2026-11-05)

v00.54 (logs/v00.54): the certificate errors were gone and Steam's login server connection
completed (ConnectionCompleted, 162.254.192.99:27018, WebSocket), 30 s after the UI asked; the log
ended 10 s later without a result. Every quick ping of a login server still failed in the second
Steam asserted "Unable to set default socket options, error 10022" (WSAEINVAL). OpenOrbis'
netinet/in.h and netinet/tcp.h have Linux's IP, IPv6 and TCP option numbers (its SOL_SOCKET and
MSG_ ones are FreeBSD's or corrected already): IP_TOS 1 is FreeBSD's IP_OPTIONS, IP_TTL 2 its
IP_HDRINCL, IPV6_V6ONLY 26 its IPV6_CHECKSUM, TCP_KEEPIDLE 4 its TCP_NOPUSH.

v00.55:
- port/include/ps4_wine_compat.h: FreeBSD 9's IP_, IPV6_ and TCP_ numbers for all of Wine's Unix
  code; the options it lacks undefined so Wine takes its BSD ways (IP_RECVDSTADDR for IP_PKTINFO,
  IP_DONTFRAG/IPV6_DONTFRAG for MTU discovery); source-specific multicast undefined (musl's
  ip_mreq_source has another field order). TCP_INFO is FreeBSD's 32: wineserver's TCP table reads
  each socket's state with it (it failed before, so every connection was "established"), so
  port/include/netinet/tcp_fsm.h has FreeBSD's TCPS_ numbers (the server's own are Linux's).
- The USB keyboard ("Keyboard" mode) typed nothing in the desktop's Notepad or in Steam. It opens
  and reads (keyboard opened, connected, "the system has the keyboard" for a second), the layout
  agrees with OpenOrbis' OrbisKeyboardData, the routing and win32u's kbdus fallback look right: not
  found by reading. Settings > Diagnostics > Keyboard log (WINEPS4_KEY_LOG, on by default for now)
  logs the shape of each new read (count, slots, high bits, keys the table doesn't know) and where
  the presses went each second, never which keys.

## Steam's text, the PS4's keyboard for text fields, and Crash Bandicoot (v00.56, 2026-11-05)

v00.55 (logs/v00.55): the socket option numbers worked. Steam's quick pings of the login servers
succeed (174 ms), it connects to one and the sign-in page's QR code comes through it; polling that
sign-in fails ("Failed to poll auth session. Result 2. Transport Error: 2") only when the server ends
the connection, about 70 s after it was made ('Try another CM': nobody signed in on it). The user's
screenshot showed the sign-in window with its boxes, check box, button and QR code but no text at
all. Blink's last-resort font (Arial, Sans, MS UI Gothic, Microsoft Sans Serif, Segoe UI, Calibri,
Times New Roman, Courier New) was found nowhere (cef_log: remote_font_face_source.cc(392)
NOTREACHED, its loading fallback): DirectWrite's system fonts are the registry's Fonts key, and the
prefix had only Wine's Tahoma, Marlett, Symbol, Webdings and Wingdings (no C:\windows\Fonts at all).

The USB keyboard: libSceKeyboard's reads said a keyboard was connected but never had a key in the
whole session. The library has sceKeyboardSetProcessFocus: the system gives the keys to the process
it focuses. Apps get keyboards from libSceIme's keyboard (sceImeKeyboardOpen, events in sceImeUpdate).

Crash Bandicoot N. Sane Trilogy: it started (process 472), connected to the display,
then wrote nothing for 147 s, and ended (code 0) as Steam was started. It never asked for a D3D11
device in that time, and none could be made: Wine has no Vulkan here and wined3d's OpenGL finds no
pixel format (steam.exe's own D3D check: "Wine was built without Vulkan support", "Failed to find a
suitable pixel format"). 3D games need a GPU path (RADV, winevulkan, DXVK): not in v00.56.

v00.56:
- app/windows-fonts: Liberation 2.1.5 (Sans, Serif, Mono; SIL OFL 1.1, Ubuntu's fonts-liberation)
  beside Wine's own fonts in the package (/app0/wine/share/wine/fonts), so win32u lists them in the
  registry, where DirectWrite looks. port/ps4_main.c map_font_names adds the names programs ask for
  to HKCU\Software\Wine\Fonts\Replacements in user.reg before wineserver loads it (only the ones the
  key lacks): Arial, Helvetica, Segoe UI, Microsoft Sans Serif, Verdana, Calibri: Liberation Sans;
  Times New Roman, Georgia: Liberation Serif; Courier New, Consolas, Lucida Console: Liberation Mono.
- The USB keyboard through libSceIme's keyboard (system module 0x95; every user's keyboard, else the
  first user's): its presses and releases of USB HID usages make the held keys libSceKeyboard's reads
  gave (a key pressed and let go between two frames is held for one), so Windows' keys, its repeat
  and the lock lights are made as before; libSceKeyboard only if it doesn't open. What was typed while
  the system's keyboard dialog was up is dropped (it went there). The keyboard log adds a line a
  second of libSceIme's counts (presses, releases, repeats; never which keys).
- The PS4's keyboard opens by itself for a text field clicked (Settings > Bluetooth & devices, on by
  default; not while a USB keyboard types straight into programs). In a program: within 3 s of a
  click, its display driver tells of a caret shown or moved near the click (win32u's
  SetIMECompositionRect: Windows' text boxes), or of Chromium's text services focusing a text field
  (msctf's ThreadMgr_SetFocus, Wine patch: the focused document's context has a text store that
  isn't read-only; with its keyboard disabled it is a password field, and the PS4's keyboard hides
  what's typed): NotifyIMEStatus with PS4_TEXT_STATUS, PS4_DISPLAY_TEXT to the desktop. In the
  desktop's Notepad: half a second after a click in the text that chose nothing (a double click
  still chooses a word).
- A field already focused when clicked (Steam's sign-in focuses its first box itself) gives no news:
  one still focused 1.2 s after a click on its process's window, with no news meanwhile, opens the
  keyboard too. If the system refuses its keyboard dialog while libSceIme's keyboard is open, that
  keyboard is closed, the dialog tried again, and the keyboard opened again when the dialog closes.
- Programs saw no free memory at all (GlobalMemoryStatusEx: neither ntdll's Linux nor its BSD
  branch is built here), so Chromium was in critical memory pressure all the time ("Discarding
  multiple pages" every 5 s in cef_log). ntdll's get_performance_info has a PS4 branch (Wine patch):
  the app's direct memory is the RAM, its largest free run and the flexible memory free what's
  available (ps4_vm_memory_status).

## The web helper's crashes, the password keyboard (v00.57, 2026-11-05)

v00.56 (logs/v00.56): the fonts worked (text in Steam), the PS4's keyboard opened for the user name
by itself, and libSceIme's keyboard worked (2 keyboards, real presses; in PS4 + Keyboard mode they
are not sent to programs, by design). The password field's keyboard never opened: sceImeDialogInit
refused with 0x80bc0030 (INVALID_PARAM) each time: the password option takes the Basic Latin type
only (shadPS4's ime_dialog.cpp: option 4 needs type 1 or above Mail). Steam's web helper died four
times: once an access violation (a read at FFFFFFFFFFE01000, PartitionAlloc freeing near null; winedbg
came up), three times 0xE0000008 (Chromium's out-of-memory exit), each when the app's files were used
up ("files 257 of 257"): wineserver held 263 files, about 100 of them the temporary files of
sections (Chromium's shared memory) and about 70 Chromium's GPU shader caches on disk (GPUCache,
GrShaderCache, ShaderCache, GraphiteDawnCache, DawnWebGPUCache, DawnGraphiteCache: five files each,
some open three times) with the GPU off. A steam.exe started meanwhile crashed in virtual_init
(crash.log: an anonymous mmap failed, MAP_FAILED used: fault at 0xfffff), memory being short.
"Unable to set default socket options, error 10022" stayed: FreeBSD takes no IPPROTO_IP option on
an IPv6 socket (ip6_ctloutput: EINVAL), Windows does on a dual-stack one.

v00.57:
- The PS4's keyboard for a password: type Basic Latin with the password option.
- Chromium programs get --disable-gpu-shader-disk-cache (ntdll's and the desktop's switches).
- wineserver's sections in anonymous shared memory (port ps4_anon_shm: shm_open( SHM_ANON ) by its
  system call, 482) instead of temporary files, when the kernel allows it (else as before, logged).
- ntdll's do_setsockopt: an IPPROTO_IP option refused (EINVAL) on an IPv6 socket is IP_TOS as
  IPV6_TCLASS, IP_TTL as IPV6_UNICAST_HOPS, the rest taken as done; options the kernel still
  refuses are logged, each once (Settings > Diagnostics > Socket option log, WINEPS4_SOCKET_LOG).
- GlobalMemoryStatusEx's available memory leaves out the 256 MiB kept for the app.

## 3D games: Vulkan on the PS4's GPU, Direct3D through DXVK (v00.58, 2026-11-05)

Wine had no Vulkan here ("Wine was built without Vulkan support") and wined3d's OpenGL no pixel
format, so no Direct3D device could be made (Crash Bandicoot, Steam's own D3D check). ps4-kit has
the pieces the TTT2 and SSBU ports drew with: RADV (Mesa's Vulkan driver on GNM, ps4_kit_link_mesa)
and gpu/ps4_vk_wsi.cpp (a headless surface and swapchains flipped on VideoOut). A program's process
has no VideoOut (the desktop's), so the WSI got a window mode (ps4-kit 195da48: set_window_present):
swapchains of the size asked for, linear images in host-visible memory the CPU reads (cached where
there is such a type), and each present waits for the GPU and hands the frame to a hook.

v00.58:
- RADV and the kit's WSI linked into the app; port/include/config.h has SONAME_LIBVULKAN, and
  port/ps4_unixlibs.c gives win32u's vulkan.c a built-in libvulkan.so.1 (vkGetInstanceProcAddr and
  vkGetDeviceProcAddr through the kit's, which keeps its swapchain and drops binary semaphores from
  submits); winevulkan's Unix half is a module of the app (app/CMakeLists.txt).
- wineps4.drv's vulkan.c (Wine 11's driver interface: pVulkanInit): a window's surface is the kit's
  headless one (VK_KHR_win32_surface mapped to VK_EXT_headless_surface), and the hook sends each
  frame to the desktop as PIXELS of the top-level window at the client area's place (one write when
  the rows are packed; PS4_PIXELS_RGBA when the swapchain is R8G8B8A8). Vulkan log (Settings >
  Diagnostics, WINEPS4_VULKAN_LOG): the WSI's lines, frames a second among them, in wine.log.
- win32u's D3DKMT makes no Vulkan instance (Wine patch): its adapters' and display devices' GPU
  lookup would open the GPU in every process that asks (explorer, Steam).
- DXVK 3.1.1 (the official release, the version TTT2 ran on this RADV) in the package (/app0/wine/
  dxvk), copied into the prefix's system32 and syswow64 at start (install_dxvk); user.reg defaults
  (add_user_defaults, the font names' code made general): d3d8, d3d9, d3d10core, d3d11, dxgi native
  first; steam.exe and steamwebhelper.exe keep Wine's. Settings > Apps > 3D games on the PS4's GPU
  (WINEPS4_DXVK=0: WINEDLLOVERRIDES gives programs Wine's own again). RADV's shader cache in
  /data/WinePS4/shadercache (MESA_SHADER_CACHE_DIR).
- A frame costs a GPU wait and a copy through the desktop's socket (8 MB at 1080p): a first path,
  to see games at all; sharing the frames' memory with the desktop is the next one.

## Steam closed and the app froze: memory, not the 7 processes (v00.59, 2026-11-05)

v00.58 (logs/v00.58): Steam's window came and went, then the app froze. The app's direct memory
(4.6 GiB, shared by all its processes) ran out at 100.8 s ("direct memory used up, 304 MiB left"):
Steam's web helper died of it (0xE0000008), winedbg could not start (crash.log: virtual_init, an
anonymous mmap failed and MAP_FAILED was used, fault 0xfffff), and at 139 s every process stopped
at once: the processes were still there (klog), nothing logged. The desktop writes the PS4's input
into a pipe the wineserver reads, and the pipe's write end blocked: a server that stopped reading
froze the desktop with it. Across versions: v00.55 never ran out (Chromium was told nothing was
free and stayed small), v00.56 and v00.58 did (told the truth since v00.56, it grew), and Steam
started hardwareupdater.exe twice and conhost.exe in local slots (the pool of the processes
outside the 7 had 146 MiB left: its error reporter, restarted after each assert, held 64 MiB of it
each time). Each local process maps a 256 MiB C library heap of direct memory at its start, used or
not. No Vulkan or DXVK ran: v00.58's user.reg defaults were written with single backslashes (a
heredoc halved them), so its DLL overrides went into top-level keys "SoftwareWineDllOverrides".

The 7 processes the PS4 runs for an app are not what failed: processes past them have run outside
the app since v00.51. Several Windows processes in one PS4 process ("a lightweight kernel") would
not help: Wine gives each Windows process its own address space and ntdll, and the memory they need
is the same in one process or seven.

v00.59:
- Program processes' C library heap: 64 MiB, and 64 MiB more at a time as it fills (8 at most;
  ps4-kit a306bf5's overflow_direct_max), not 256 MiB each.
- A new program starts outside the 7 first while the app's free direct memory is under 384 MiB
  (ps4_spawn_desk.c, ps4_vm_direct_free): it takes the system's pool instead.
- Chromium's browsers are told of the app's free memory less 1 GiB (ps4_vm_set_memory_margin),
  and start with --enable-low-end-device-mode.
- Left out: Steam's steamerrorreporter(64).exe and hardwareupdater.exe.
- The desktop's input to the server never blocks (server/ps4_input.inc): dropped while the pipe is
  full, said once after 5 s, with the desktop's threads in hang.log when the hang report is on.
- user.reg's keys with their backslashes doubled again (tested on the PC on the console's user.reg),
  v00.58's misnamed keys removed: DXVK's overrides apply now.

## The app closed: program heaps in the PS4's own pool (v00.60, 2026-11-05)

v00.59 (logs/v00.59): Steam opened, asked to install its service, and the PS4 closed the app
("Failed to create the system memory", AppCrash 0xa002008f, as in v00.53). Every program's heap said
"dlmalloc on 128 MiB of system flexible memory": ps4-kit's direct-first heap never tried a size
under 256 MiB, so v00.59's 64 MiB direct heap was skipped and each program took 128 MiB of the
system's pool instead, the one the PS4 itself and the processes outside the 7 live on. services.exe
(outside) could not make its threads ("RPCRT4_new_client failed to create thread, error 8"), so
Steam could not open the service manager ("BOpenSCMgr failed (GLE 1726)") and offered to install
its service; then the pool ran out for good. (v00.58's misnamed user.reg keys were removed and
DXVK's overrides added, as meant. Crash Bandicoot ran 6 s and ended, DXVK never loaded: no
dxgi/d3d11 logs.)

v00.60:
- ps4-kit bb4a45c: a direct_max under 256 MiB is tried as the main heap itself.
- Program processes never take the system pool (system_pool_max 0): one with no heap fails alone;
  their automatic arenas 2 MiB, 6 at most.

## Iron Snout's "Invalid platform", and programs' sound (v00.61, 2026-11-06)

v00.60 (logs/v00.60): Steam signed in and downloaded Iron Snout (32-bit); launching it said "Invalid
platform" (Steam's console_log: CreatingProcess, then AppError_29). IronSnout.exe has a shared
writable section (.mydata, 8 bytes at 0x71b000: a one-instance counter). wineserver copies such
sections into a temp file with pwrite, and since v00.57 that file is anonymous shared memory, which
the PS4's kernel (FreeBSD 9) takes no read, write or pwrite on (ESPIPE): the image failed with
STATUS_INVALID_IMAGE_FORMAT, which a Windows program sees as an .exe for another platform. ntdll
could not have mapped it either: a 4 KiB section in one of the PS4's 16 KiB pages, other sections in
the same page ("unaligned shared mapping not supported"). The same log: no audio driver at all ("No
driver from pulse,alsa,oss,coreaudio"), Steam's "Unable to set default socket options, error 10022"
still there though no option do_setsockopt sets was refused, 4000 of its 6000 lines Chromium's and
Steam's stub messages, and two processes that died starting (virtual_init's anonymous mmap failed,
fault 0xfffff, as in v00.58: direct memory was down to 1.3 GiB at the end).

v00.61:
- wineserver writes a shared section's data through a mapping of the shared memory when pwrite is
  refused, and grows shared memory with ftruncate (grow_file: the session mapping's growth too, which
  v00.57's shared memory had broken the same way).
- ntdll gives a shared section that does not fill 16 KiB pages of its own a private copy, like .data:
  its data is not shared between processes (a one-instance counter reads 0 in each).
- A program that does not start is in wine.log with its status ("wineps4: starting ... failed").
- Sound. wineps4.drv is also Wine's audio driver: mmdevapi loads it as "ps4" (user.reg's
  Software\Wine\Drivers, Audio), and its unix call table is audio.c's (mmdevapi's 37 calls, then the
  display driver's init at 37). A WASAPI stream (DirectSound, winmm's wave out, XAudio2 and Media
  Foundation are on WASAPI) keeps its buffer in the program's memory, as wineoss does, and its timer
  copies what was written into a ring in shared memory, 50 ms ahead at most (port/include/ps4_audio.h);
  the ring's descriptor goes to the desktop over /data/WinePS4/.audio. The desktop's mixer
  (port/ps4_audio_desk.c) takes 256 frames of each running stream every 5.3 ms, folds its channels to
  stereo, interpolates its rate to 48 kHz, applies its volumes and plays the sum through one
  sceAudioOut port (16-bit stereo, the system user). Mix format 48 kHz float stereo; any rate from 1 to
  384 kHz, 1 to 8 channels, 8 to 32-bit PCM and float taken as they are. No capture endpoint, no MIDI
  devices. A PC simulation of the ring's two sides (scratchpad) played in order without gaps with the
  program's timer up to 30 ms late, and after Stop/Reset/Start. Settings > Diagnostics > Audio log.
- Steam's 10022, the suspect: SIO_KEEPALIVE_VALS failed as a whole when the kernel refused
  TCP_KEEPIDLE (FreeBSD 9 may not have it); keepalive now stays on at the kernel's timing. Refusals of
  that, TCP_KEEPINTVL, SO_KEEPALIVE and IPV6_V6ONLY (neither goes through do_setsockopt) are logged.
- WINEDEBUG leaves out the fixmes of manipulation, dwmapi, appbar, media and bcrypt unless set
  (Settings > Diagnostics > Wine debug messages sets it).
- Settings > Diagnostics > Performance log: every 10 s, each process's share of the CPU, the desktop's
  drawing time and the direct memory free, for "heavy and slow" next time.

## The freeze 100 s into Steam (v00.62, 2026-11-06)

v00.61 (logs/v00.61): the user started Steam 210 s after the app and the app froze about 100 s later,
no game started. wine.log stops at 313 s: Steam's lines (a few a second, no burst), the desktop's
open-files line due at 315 s and the Performance log's due at 320 s never came, so the whole desktop
process went quiet at once, not one program. Nothing before it: direct memory 1776 MiB free, the
desktop drawing at 14 ms a frame, Steam's web helper at 36%. crash.log was v00.60's (no crash), and
the kernel's log had moved on to another app before it was read. No v00.61 change had run that could
be it (no audio stream, no shared section, no failed start). What is new for the desktop's loop is a
line on stderr every 10 s (the Performance log) and ps4_process_list (the spawner's lock); and every
process's stderr, the desktop's threads and wineserver's included, is one pipe that one thread empties
into wine.log and the Terminal: if that thread stops, everything that writes a line stops with it.
Also seen: the kernel refuses TCP_KEEPIDLE/TCP_KEEPINTVL (level 6, options 256 and 512, "Protocol not
available"), now taken as done, yet Steam's 10022 asserts stayed (65): another call fails.

v00.62:
- Settings > Diagnostics > Freeze report (on): the desktop's loop and the log thread each move a count;
  10 s without (the log thread with output waiting in its pipe) puts the stack of each of the desktop's
  threads into hang.log (the desktop's loop, the log thread, wineserver, the spawner's monitor and
  broker, the display's and the mixer's threads registered for it) and says so in wine.log, written
  straight to the file; with wineserver's requests in the last 9 s. Once a stand, three at most.
- Nothing waits for the log pipe: its write end is non-blocking (a line that does not fit is lost;
  Wine's NtWriteFile polls and retries, so programs' console output is not); the log thread hands the
  Terminal its text under a lock of its own, never the one the desktop's loop draws with; the
  Performance log writes straight to wine.log and adds the pool outside the 7; the spawner writes its
  lines after letting its lock go.
- Settings window 1000 high (10 switches).

## Iron Snout's Direct3D 11, the second freeze, quick frames, shortcuts (v00.63, 2026-11-06)

v00.62 (logs/v00.62, Steam's logs in steam/): Iron Snout started from Steam at last (v00.61's shared
section fix: "AppID 424280 adding PID 772 as a tracked process", the section's private copy in
wine.log), DXVK found the GPU, then "vkMapMemory2EXT failed: -5" and "Failed to create D3D11 device:
Failed to map Vulkan memory": a 32-bit process maps every Vulkan memory below 4 GiB with
VK_EXT_map_memory_placed (win32u), and ps4-kit's RADV refused every placed map (its PS4 path returned
NULL for a fixed address). Every 32-bit game on DXVK. 100 s later the desktop froze: the freeze report
fired ("the desktop's loop still for 10 s, the log thread for 0 s (reading, 0 bytes waiting)", so not
the log pipe), and its dump wrote nothing: it began with fopen (malloc), and stdio kept the rest until
fclose (a user closing the app by then loses it all). Performance log: the desktop's frame 12-14 ms
whenever anything of the screen changed, and every pointer move copied the whole 8 MiB scene to the
screen; Steam's web helper 0.5-2.5 of the 7 cores.

v00.63:
- ps4-kit gpu 941b771 (Mesa patch 0030): placed maps: the buffer's direct memory mapped once more at
  the application's address (sceKernelMapDirectMemory, fixed), back to a reservation on unmap and on
  destroy. win32u (Wine ef060f4) only reserves the placed range on the PS4: committed pages are backed
  with direct memory (and zeroed) at once, which the driver's mapping then covers: a whole DXVK chunk
  (up to 256 MiB) wasted for each.
- The freeze report's dump: no stdio, no malloc, the thread table without its lock, SIGXCPU by the
  kernel's thr_kill, hang.log opened by a system call (FreeBSD's flags: the C library's headers have
  Linux's) and written a line a thread, 200 ms for each; the report says the desktop's heap use. The
  Performance log too ("the desktop's heap N of M MiB filled").
- Quick frames: when only programs' pixels changed (winwindows tracks each window's changed rect, and
  whether any window came, went, moved or was raised), and they lie in a window's own part (below its
  title bar, inside its frame and bottom corners, above the taskbar unless full-screen) with no other
  window or its shadow over them, and nothing of the desktop's is open over the programs (menu, Start,
  dialog, toast, copy, dragged files, a desktop window on top), they are copied into the scene as they
  are (winwin_patch) instead of the scene being drawn again. The screen's two buffers each keep what
  they lack of the scene (all, or rects) and where the pointer was drawn: a frame copies only those, a
  pointer move over an unchanged scene two small rects. The Performance log counts the quick frames.
- Shortcuts: "Create shortcut on the desktop" in Files' right-click menu (anything not already on the
  desktop) and on a program's taskbar button (its process's program): a Shell Link (.lnk: LinkInfo's
  local path, the working folder, as Windows writes one), named without .exe, "(2)" and on when taken;
  a .lnk or .url is copied. A shortcut to a folder opens Files there.
- Internet shortcuts (.url, Steam's games on the desktop): their IconFile's icon on the desktop and in
  Files (with the shortcut arrow), their name without .url; opened, a steam: address goes to Steam's
  steam.exe (it hands it to the Steam that runs, or starts Steam and then the game), any other to
  start.exe. A .lnk's icon location is used when it names one. Shortcuts are read once and again when
  they change (the desktop read every .lnk from the disk on every frame it drew).

## The freeze as a game starts, Steam in Big Picture (v00.64, 2026-11-06)

v00.63 (logs/v00.63): Iron Snout started from Steam (process 631, and x86launcher.exe, which ended at
81.48), and from about 82 s everything stood: Steam's processes wrote nothing more (their own hang
reports in hang.log, still running after 20 and 50 s, every thread in a system call), wineserver took
no request for 9 s, and the desktop's loop stood with its thread waiting for the windows' lock
(winwin_text_events). Steam's lines stopped first: wineserver stood first, the desktop after it. The
report fired at 93 s and its dump stopped after four of the desktop's threads: the fifth never
answered (v00.63 waited for it), or the dump stood itself (snprintf takes ps4-kit's stdio lock). Who
held the windows' lock, and where wineserver stood, is not known yet: v00.64 is made to tell.

v00.64:
- The freeze report after 6 s still. Its text and the dump are built without printf
  (port/include/ps4_line.h); each thread has 50 ms to answer ("no answer" else) and its line is
  written at once. It says who holds the windows' lock (the thread, and where it took it:
  WindowsLock in shell/winwindows.cpp) and, per thread, whether it is inside the heap or stdio
  (ps4-kit's per-thread depth counters, read by the signal handler on the thread itself).
- wineserver says where its thread is (Wine server/fd.c, request.c, trace.c): waiting for its
  programs, in a request (its name and the process that made it), on a file's event, running its
  timers. Its thread may stand where no signal reaches it; the report reads these instead. After the
  dump, the app's memory (direct, flexible, the pool outside the 7): asking the kernel may stand too.
- Steam in Big Picture: Settings > Apps, "Steam in Big Picture" (on by default;
  WINEPS4_STEAM_BIGPICTURE=0 off), applies when Steam next starts: steam.exe gets -gamepadui.
- Nothing of the desktop is drawn under a program's full-screen window (Big Picture, a game): no
  wallpaper, icons, desktop windows or taskbar, and what changes under it makes no frame (its damage is
  dropped). The desktop's own memory stays as it was; its drawing and the frames it copies go.
- 32-bit programs get 4 GiB (large address aware) as under Proton (PROTON_FORCE_LARGE_ADDRESS_AWARE,
  on by default there): Wine's ntdll, virtual_set_large_address_space (WINEPS4_LARGE_ADDRESS_AWARE=0
  gives a program the 2 GiB its header asks for).

What else of Proton fits the PS4 (not done): fast waits (Proton's fsync/ntsync: Windows' events and
mutexes waited on in shared memory, not through wineserver; FreeBSD's _umtx_op on shared memory could
do it), games' display modes (a game's resolution change scaled to the screen, FSR as Proton's
fullscreen hack has it), the controller as an XInput pad (Big Picture and games), wine-mono and
wine-gecko (.NET programs and launchers, HTML in programs), Media Foundation through GStreamer (games'
videos), vkd3d-proton (Direct3D 12: needs more of RADV on the PS4's GPU). Proton's steam.exe bridge
(lsteamclient) is not needed: Windows' own Steam runs here.

## The freeze again, from the kernel's side (v00.65, 2026-11-06)

v00.64 (logs/v00.64; user: "in big mode, it froze, not sure if the controller was supported"): Steam
in Big Picture ran 50 s (its sounds played), then Iron Snout started from it (process 653, 534.14),
x86launcher.exe came and went (654, 534.71 to 535.37) and from about 538 s everything stood: the same
order as v00.63's. Iron Snout's last line was its win32u at 535.25, before its window (no display
HELLO); v00.62's got to DXVK 6 s after that point, so this is earlier than Direct3D. The report at
544.46: wineserver 0 requests in 5 s, "now on a file's event" (outside any request), the windows'
lock free, the heap 51 MiB. Its dump: the log thread (read), the audio listener (poll) and the
display listener (accept) answered; two threads never did (most likely the desktop's loop and
wineserver's, by the table's order); then the dump itself stood before the next thread, and the
memory line after it was never written. Threads that don't answer a signal wait in the kernel. The
user closed the app before 20 s, and the Hang report is off by default: no program wrote anything.
klog (GoldHEN's, read after) held nothing of it. The controller: programs see none. winebus is not
built (its root device's Service is taken out at start), and the pad only moves the desktop's pointer.

v00.65:
- port/ps4_kstate.c: where the kernel has each thread of the app, without a signal: __sysctl
  kern.proc.pid with threads (FreeBSD 9's kinfo_proc, 1088 bytes, read at its offsets: state, wait
  message, kernel lock name, seconds asleep, run time, thread name). A line a thread into hang.log, a
  line a process into wine.log ("wineps4: kernel: the desktop (pid N, M threads): 30 umtxn, 9 select,
  ..."). The desktop asks at a freeze on a thread of its own (the signals' dump and it don't wait for
  each other), for itself and every program it started (their pids and names in the heartbeat page).
- /data/WinePS4/.heartbeat: a shared page with the desktop's loop count, wineserver's request count,
  the app's processes and their names. Every local and outside process looks at it each second from
  its stats thread; the first to see the loop still for 10 s, when the desktop's own report didn't
  get through, asks for every process of the app (asking wires the asker's buffer, which takes its
  address space's lock: a desktop whose address space stands can't ask for itself). Off with the
  Freeze report.
- The dump's signal handler takes its stack's bounds from the thread table (pthread_attr_get_np at
  registration) instead of asking the kernel, which may be what stands.
- The desktop's threads are named (desktop, log, wineserver, freeze watch, kernel report, display,
  display client, audio, audio mixer, spawn monitor, spawn broker, reaper, files; stats in programs),
  so the kernel's view says which is which.

## The cause: the app's flexible memory runs out (v00.66, 2026-11-06)

v00.65 (logs/v00.65, with the PS4's kernel log captured live: klog.txt): the same freeze, 6 s after
Iron Snout started from Big Picture. The kernel report got nothing: kern.proc.pid with threads is
ENOENT on the PS4 for every process. The signals' dump stood after four threads again. The kernel log
says why. ps4-kit logs the flexible memory free as each process starts its heap, and for the local
processes it is one pool that falls as they start and comes back as they end: the desktop 395 MiB,
wineboot 306, explorer 251, then 741 after wineboot ended, steam.exe 739, steamwebhelper.exe 677, Iron
Snout 222, x86launcher.exe 169. SceShellCore's FMEM table had the web helper at 356 then 465 MiB (64
GiB of address space: Chromium's), steam.exe about 147, the desktop 86, Iron Snout 79 while loading.
The pool is about 1 GiB at most (448 MiB when the app started, more as the PS4 had it), the app's
direct memory 4608 MiB with 1.4 GiB of it free. The PS4 doesn't refuse a page when the pool is gone:
the thread that touched it waits in the kernel, and one after another every process of the app stood
(threads that answer no signal, a dump that stands on its own stack's next page). v00.63's and
v00.64's freezes came at the same point. In a local process the VM layer already backs Windows'
memory with direct memory; what takes flexible memory is every anonymous mapping outside it, shared
mappings (Wine's sections: Chromium's shared memory) and each process's own data (the binary's 12.6
MiB of data and bss, stacks).

v00.66:
- x86launcher.exe runs outside the 7 (Steam's helper as a 32-bit game starts: half a second, 53 MiB
  of the pool at the worst moment).
- The memory guard (Settings > Diagnostics, on): four times a second the desktop reads the pool's
  free size; under 192 MiB it tells who holds it, under 96 MiB it ends the web helper that holds the
  most (steamwebhelper.exe, epicwebhelper.exe, qtwebengineprocess.exe, msedgewebview2.exe,
  cefsharp.browsersubprocess.exe), which its launcher starts again, once in 30 s. Without printf or
  the desktop's locks; the toast after the kill.
- Each program's mappings every 5 s (sceKernelVirtualQuery: flexible ones; shared ones, neither
  flexible nor direct) in its stats; the Performance log says the pool's free size and its four
  largest holders, flexible+shared ("flexible memory 222 MiB free (the desktop 86+2 MiB,
  steamwebhelper.exe 300+165, ...)").
- The kernel report: the pool's free size first, and once the forms of kern.proc this kernel answers
  (pid_td, pid, all, proc) with their errors or sizes.

Next, by what the census shows: Wine's sections in direct memory shared between the app's processes
(the pool's largest part if Chromium's is shared memory), or the app's flexible size in the
executable's process parameters (sceKernelFlexibleMemorySize; ours asks nothing, SDK 4.508, so
shadPS4's loader would also drop the extended memory region).

## Iron Snout reaches Direct3D, and a freeze the guard didn't see (v00.67, 2026-11-06)

v00.66 (logs/v00.66, Steam's logs in steam/; klog in logs/v00.65/klog.txt, captured live): Iron Snout
got further than ever. Its window connected (113.68), DXVK found RADV, and Direct3D 11 failed as in
v00.62, now on "placed cpu map of bo 8 at 0x7310000 (0x8000 bytes) failed": the kit's placed map
(Mesa 0030) maps a buffer's direct memory a second time, which the PS4 refuses unless the process
turned direct memory aliasing on (sceKernelEnableDmemAliasing; shadPS4's turns off the kernel's check
that the memory isn't mapped already). The memory guard warned at 184, 181 and 191 MiB free (the
largest holders: steamwebhelper.exe 229 to 456 MiB flexible, steam.exe 126, IronSnout.exe 53 to 100,
explorer.exe 67, the desktop 94 to 101; nothing "shared": VirtualQuery calls sections flexible, or
there are none). x86launcher.exe started among the 7 anyway: the pool outside had 247 MiB, under the
256 it needs. Then at 139 s everything stood, 250 MiB free at the last look (130 s): wine.log ends at
138.95 and Steam's console_log at the same second (its lines every 3 s stop); no freeze report, no
guard line, no program's report. The PS4 had the app running for the next ten minutes (no exit in the
klog), and GoldHEN's FTP read every file. The heartbeat page, read over FTP, stayed at loop count
14960, told 0, watcher 0: the desktop's loop, its freeze watch and every program's stats thread had
stopped. The freeze watch stored into that page each second, and a file's page that was written back
faults on the next store: a fault that waits stands with everything else.

v00.67:
- ps4-kit Mesa patch 0031 (kit 53cf427): a placed map turns the process's direct memory aliasing on
  first (sceKernelEnableDmemAliasing, its result logged once); a failed one says the kernel's error.
- No heartbeat page (no form of kern.proc with threads answers on the PS4: the programs' watch had
  nothing to tell); kern.proc's forms probed as the desktop starts.
- The freeze watch, the kernel report and the memory guard touch their stack pages as they start.
- Seen without a signal: the desktop's loop marks its phase (reading the controller, the system
  keyboard, the programs' output, starting a program, windows and input, the performance log,
  drawing, presenting, the pointer, idle), and threads mark the memory call they are in (the memory
  layer's mmap, mmap (shared), munmap, mprotect, madvise; wineserver's shared memory for a section).
  The freeze report's line says the loop's phase and every thread in a memory call for a second or
  more.
- A program with 192 MiB of flexible mappings or more tells what they are once a minute, by name,
  memory type, protection and kind ("wineps4: memory: steamwebhelper.exe's flexible mappings ...").

## The keyboard for games, the web helper's crash, windows over Big Picture (v00.68, 2026-11-06)

v00.67 (logs/v00.67, Steam's logs in steam/): Iron Snout ran. DXVK 3.1.1 made its Direct3D 11 device
on RADV (feature level 11_1; "radv/ps4: direct memory aliasing on (0)": the placed maps worked), its
1760x990 swapchain flipped 60 times a second and the desktop drew 60 frames a second. The user: not in
full screen, no input from the keyboard, Big Picture reopened when the game opened.
- The keyboard: libSceIme's keyboard read every press, and the keyboard log said where they went: "0
  to programs (the top one: Iron Snout) ... 2 not sent (PS4 + Keyboard mode)". Settings' default
  keyboard mode (desktop.ini: keyboard=ps4) gave a USB keyboard's keys to the desktop's windows and the
  PS4's keyboard dialog only; programs got text through the dialog.
- Big Picture: Steam's web helper (process 734) crashed 3.4 s after the game started: "Unhandled page
  fault on read access to 0000000000001000 at address 000000FBF39C3833 (thread 0320)", just as that
  thread's sound stream stopped (AvRevertMmThreadCharacteristics: Chromium's WASAPI thread ending). A
  read at 0x1000 is PartitionAlloc freeing a pointer of the first 2 MiB (super page 0's metadata page),
  as v00.57's read at FFFFFFFFFFE01000. Wine's debugger came up: winedbg.exe (739) and its console's
  conhost.exe (740), two of the 7 processes and 58 + 67 MiB of flexible memory (the console stayed to
  the end), its trace in that console window, not in wine.log. The web helper ended 6.6 s after its
  fault; Steam started it again (741, at 495 s) and its Big Picture window came over the game. At
  524.96 the memory guard ended it (93 MiB free) and Steam didn't start it again.
- Full screen is the game's own setting: AppData\Local\IronSnout\ironSteam.ini has fullScreen=
  "0.000000" (GameMaker, data.win; it chose a 1760x990 window). The display driver gives one mode
  (1920x1080, 60 Hz); win32u adds lower virtual modes and takes a change of mode without scaling.
- The desktop kept full-screen windows over all the others (stacked()): a windowed game started from
  Big Picture would have stayed under it.
- The memory breakdown: steamwebhelper.exe's flexible mappings were 223 MiB in 631 read-write ones
  named after their caller, map_file_into_view's ps4_vm_mmap (0x6005a2b5): MAP_SHARED mappings of an
  fd, wineserver's sections in shared memory (ps4_anon_shm), Chromium's shared memory; 34 MiB in 35
  read-only ones; "executable" 37 MiB is the eboot's text. Sections in direct memory would leave the
  flexible pool to the rest.
- kern.proc's forms here: pid_td fails (ENOENT); pid, all and proc answer (records of 1096 bytes).

v00.68:
- A USB keyboard types into the program with the focus, as on a PC; the keyboard mode setting is gone
  (Settings > Bluetooth & devices says how it works). The desktop's own windows, menus and dialogs take
  its keys when they are in front (not when a program's full-screen window covers them), the PS4's
  keyboard dialog while it is up. Alt+Tab brings the next program window to the front (Alt+Shift+Tab
  the one before; the Tab isn't sent, as Windows keeps it). The PS4's keyboard doesn't open by itself
  for a text field while a USB keyboard is connected. The keyboard log counts Alt+Tab's switches.
- Program windows stack as they were shown or raised, full-screen ones too. The active window, when it
  is full-screen, goes over the taskbar with what is over it (its menus, tooltips); a window raised
  over a full-screen one is over it, and the taskbar over both.
- Wine cff9836 (kernelbase's UnhandledExceptionFilter): a crash's trace in wine.log, "wine: <the
  fault> (thread T), in module+0xoffset: the program ends (Settings > Diagnostics > Crash trace)",
  its registers and up to 24 calls (x64 by unwind data, x86 by frame pointers), each as
  module+0xoffset (the module file's name from wineserver, else its exports' name, else its base).
  The program ends at once (EXCEPTION_EXECUTE_HANDLER): no winedbg, no console. Settings > Diagnostics
  > Crash trace (WINEPS4_CRASH_TRACE=0: Wine's debugger as before). kernelbase.dll built in the host's
  WoW64 tree (x64 and x86) and copied into install/wine-wow64.

## Programs' shared memory in direct memory (v00.69, 2026-11-06)

Made while v00.68 was being tested (no log of it yet). v00.67's census: 223 MiB of the web helper's
flexible mappings were Wine's sections (wineserver's shared memory objects, ps4_anon_shm: Chromium's
shared memory), which come out of the flexible memory the local processes share (about 1 GiB), while
the app's direct memory had 1.1 to 1.4 GiB free.

v00.69:
- Wine 8c96b86 (server/mapping.c, fd.c, protocol.def; ntdll's virtual.c): an unnamed section that a
  local process makes (not SEC_RESERVE, 256 MiB at most) gets its memory from the app's direct memory,
  allocated and zeroed by wineserver in the desktop (port/ps4_wine_vm.c ps4_section_alloc). Its fd is
  a shared memory object of no size, for wineserver's bookkeeping, and holds that memory: it goes back
  to the app 2 s after the fd ends (a server timeout: the last view's munmap comes after its
  unmap_view). get_mapping_info's reply says where (direct_offset, direct_size; protocol 931), and
  virtual_map_section maps a view from there (ps4_vm_map_section: sceKernelMapDirectMemory over the
  view's reserved pages, which the VM layer then leaves to the kernel); a copy-on-write view gets a
  copy in pages of its own; a view off a 16 KiB boundary is refused. Named sections (another process
  opens them, maybe one outside the 7), SEC_RESERVE ones, those of processes outside the 7 (they map no
  direct memory: v00.42) and every one while the app's largest free run of direct memory is under 640
  MiB plus its size stay shared memory.
- Whether a process may map direct memory another one allocated wasn't known. The desktop writes a
  token into a page of direct memory (WINEPS4_SECTION_PROBE: its offset and the token); each local
  process turns its direct memory aliasing on, maps that page as it starts (after READY) and reads it,
  and the first that reads the token writes back. wineserver puts sections in direct memory only
  after that. wine.log says which: "sections: the desktop's direct memory maps here", or "does not map
  here (the kernel's error)", or "maps here but reads otherwise"; then "sections: the first in direct
  memory", "the app's direct memory is down to N MiB" (and "in direct memory again"), and any view a
  process could not map.
- The Performance log's direct memory figure says what sections hold of it ("; sections in it N MiB
  (count)").
- Settings > Diagnostics > Direct shared memory (WINEPS4_SECTIONS_DIRECT=0: shared memory as before;
  read as WinePS4 starts). The Diagnostics page is in two columns: with 12 switches one under the
  other, Crash trace (v00.68) was under the taskbar and Memory guard half under it.
- To read in its log: the test's line; the census's flexible holders (steamwebhelper.exe should lose
  most of its 223 MiB of flexible mappings); the memory guard's warnings; any view not mapped. If the
  PS4 comes down as the app is suspended with sections mapped in several processes, the switch turns
  it off.
