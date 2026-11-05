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
