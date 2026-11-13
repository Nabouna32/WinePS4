# WinePS4 networking: public dependency map

Status: source-level audit of the public WinePS4 tree. This document separates the network behavior already implemented in WinePS4 from the parts that still cross the unpublished `ps4-kit` boundary. It is not a claim of a working replacement or a hardware validation.

## Executive finding

WinePS4 does **not** outsource all networking to `ps4-kit`. Its network path is a layered mix:

1. BSD sockets and socket options reach the Orbis/FreeBSD kernel through the target C library.
2. Wine's Winsock layer (`ws2_32`) and NSI provider (`nsiproxy.sys`) translate Windows networking requests.
3. WinePS4 supplies PS4-specific interface, route, hostname and network-settings code.
4. The remaining private-kit dependencies are concentrated around module loading and DNS resolution, plus the target sysroot/runtime ABI.

This is a useful narrowing of the reconstruction problem: the network stack is not one opaque proprietary subsystem.

## Call graph and responsibilities

| Layer | Public WinePS4 evidence | Responsibility | Private-kit coupling |
|---|---|---|---|
| Windows Winsock | `app/CMakeLists.txt`: `ws2_32` Unix half from `dlls/ws2_32/unixlib.c` | Windows socket API to Wine's Unix socket calls | Target libc/socket ABI |
| Windows NSI | `dlls/nsiproxy.sys/{ip,ndis,nsi,tcp,udp}.c` compiled as `nsiproxy` | Adapter enumeration, IP configuration, routes, TCP/UDP tables | Uses WinePS4 interface/route helpers |
| DNS API | `dlls/dnsapi/ps4.c` (source in the Wine patch/build tree) | Reads configured DNS server addresses and supplies Windows adapter DNS data | Calls `ps4_dns_servers()`; resolver path uses target libc |
| Interface discovery | `port/ps4_ifaddrs.c` | Implements `getifaddrs`, `if_nameindex`, index/name conversion, link-layer info | Raw FreeBSD ioctl/sysctl ABI, not a ps4-kit abstraction |
| Routes | `port/ps4_ifaddrs.c` | Reads FreeBSD routing sysctl; falls back to networks on interfaces and gateway | Raw FreeBSD `__sysctl`; fallback uses NetCtl data |
| Network settings | `port/ps4_ifaddrs.c` | Reads default gateway and primary/secondary DNS from `sceNetCtlGetInfo` | Loads Net and NetCtl system modules through `ps4_kit_load_module` |
| Host identity | `port/ps4_net.c`, `port/ps4_main.c` | Maps console System Name to Windows computer name; resolves that name to loopback | Ordinary libc resolver for all other names |
| Socket transport | `port/include/ps4_wine_compat.h` and Wine's socket code | Corrects Linux-vs-FreeBSD socket constants and structure layouts | OpenOrbis headers/libc must match kernel ABI |

## Concrete private interfaces found

The public code in `port/ps4_ifaddrs.c` declares and calls:

- `ps4_kit_load_module(uint32_t id, const char *name)`
- `sceNetCtlInit(void)`
- `sceNetCtlGetInfo(int32_t code, void *info)`

It uses system-module IDs:

- Net: `0x8000001c`
- NetCtl: `0x80000009`

And NetCtl info selectors:

- default route: `16`
- primary DNS: `17`
- secondary DNS: `18`

The info payload is treated as a text IPv4 address, parsed with `inet_pton(AF_INET, ...)`. These constants and payload assumptions are observations from the WinePS4 source, not independently confirmed ABI declarations. They must be checked against public headers and a legitimate runtime before being promoted into a compatibility implementation.

The other explicit coupling is the resolver: `port/ps4_net.c` delegates non-console hostnames to `getaddrinfo()` / `gethostbyname()`, and comments identify the underlying resolver as part of ps4-kit. The console's own name is intercepted and mapped to `127.0.0.1`, so Wine startup does not wait on DNS for that special case.

## Why the FreeBSD compatibility layer matters

The target kernel is FreeBSD-derived, while OpenOrbis' userland headers include Linux/musl-shaped definitions in some networking areas. WinePS4 explicitly corrects:

- `sockaddr_in6`, `sockaddr_storage`, `msghdr` and `cmsghdr` layouts;
- `CMSG_*` alignment and message flags;
- socket/IP/IPv6/TCP option numbers;
- FreeBSD-specific ioctl request numbers and interface flag values;
- `SIOCGIFCONF`, `SIOCGIFFLAGS`, `SIOCGIFBRDADDR`, `SIOCGIFNETMASK`.

The source comments record concrete regressions caused by wrong constants: Steam's `IP_TOS` was interpreted as `IP_OPTIONS`, causing socket setup and ping failures. This is strong evidence that a replacement must validate ABI values, not just provide similarly named functions.

## Reconstruction plan

### A. Keep the already-public WinePS4 logic

Do not rewrite `ps4_net.c` or `ps4_ifaddrs.c` merely to make them look like a kit. Their public implementation is part of the application and already expresses the required behavior. Preserve provenance and test it separately.

### B. Replace module loading behind a narrow adapter

Define an application-owned interface with explicit outcomes:

- load Net system module;
- load NetCtl system module;
- report already-loaded / load failure distinctly;
- never attempt a system-module call from process classes where WinePS4 currently suppresses it (`ps4_outside_process`).

Do not guess a replacement for `ps4_kit_load_module` until the OpenOrbis module-loading API, required initialization order, and process restrictions are verified from public headers/source. Keep the adapter injectable so host-side tests can exercise failure paths without a PS4.

### C. Isolate NetCtl

Wrap `sceNetCtlInit` and `sceNetCtlGetInfo` behind a small WinePS4-owned provider. Validate return codes, string termination, address family, and empty/unavailable values. The current source treats the NetCtl result as dotted-decimal IPv4 text and falls back to gateway/no DNS; keep that behavior explicit rather than silently assuming IPv6 support.

### D. Make resolver behavior explicit

Determine whether the target libc already provides a functional resolver on the chosen OpenOrbis sysroot. If not, implement a separate resolver provider using documented public interfaces; do not silently depend on private ps4-kit resolver internals. Test numeric addresses, localhost, the console hostname, successful DNS, no DNS configured, timeout, and malformed responses.

### E. Validate kernel-facing network ABI

Add compile-time size/offset assertions and small target probes for the FreeBSD structures and ioctl constants. Compare interface enumeration and route results against a trusted network configuration on hardware. The raw syscall paths are kernel ABI code and must not be “ported” by replacing constants with Linux values.

## Acceptance tests (to implement)

Host-side tests can cover parsing and fallback logic without a console:

- NetCtl text parsing: valid IPv4, empty string, non-NUL-terminated input, invalid address, error return.
- DNS selection: primary only, secondary only, both, neither, gateway fallback, no gateway.
- Hostname special case: configured name (case-insensitive) maps to loopback; unrelated names delegate.
- Route fallback: each IPv4 interface network is represented; default gateway is associated only with a matching interface.
- Process guard: outside-process mode does not load Net/NetCtl.

Target-side probes are still required for ioctl/sysctl structure layouts, module loading, NetCtl selectors and real DNS queries.

## Current confidence and blockers

| Finding | Confidence |
|---|---|
| WinePS4 uses BSD sockets and a FreeBSD-derived kernel interface | High: explicit public source |
| WinePS4 implements its own interface and route enumeration | High: `port/ps4_ifaddrs.c` |
| NetCtl provides gateway and DNS values via selectors 16–18 | Medium: explicit source assumptions, ABI not independently verified |
| `ps4_kit_load_module` is a private-kit boundary | High: direct external symbol call |
| General hostname resolution is supplied by the target libc/kit resolver path | Medium: source comments, implementation not available in this repository |
| A drop-in, complete network replacement is ready | **No** |

## Source files to keep in view

- `app/CMakeLists.txt`
- `port/ps4_net.c`
- `port/ps4_ifaddrs.c`
- `port/include/ps4_wine_compat.h`
- `port/ps4_main.c`
- `dlls/nsiproxy.sys/` and `dlls/dnsapi/ps4.c` in the Wine source/patch build tree

