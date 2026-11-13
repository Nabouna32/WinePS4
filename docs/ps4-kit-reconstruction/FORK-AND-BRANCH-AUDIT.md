# Fork and branch audit for the PS4 kit reconstruction

Date: 2026-11-13

This pass checks public GitHub search results and compares source-file blob SHAs.
A matching blob SHA proves that the specific file is identical; it does not
prove that the entire repository or every branch is identical.

## 1. Direct consumers

| Upstream | Public fork found | Comparison | Finding |
|---|---|---|---|
| [WinePS4](https://github.com/iHaiDeeZ/WinePS4) | [Nabouna32/WinePS4](https://github.com/Nabouna32/WinePS4) | This reconstruction branch is our own fork/PR | No other public fork appeared in the targeted search |
| [MGR-PS4](https://github.com/iHaiDeeZ/MGR-PS4) | No relevant fork found | Search results with similar names were not the MGR port | No alternate ps4-kit reconstruction surfaced |
| [MP3-PS4](https://github.com/iHaiDeeZ/MP3-PS4) | No relevant fork found | Search results with similar names were unrelated | No alternate ps4-kit reconstruction surfaced |
| [TTT2-PS4](https://github.com/iHaiDeeZ/TTT2-PS4) | [Stayhye/TTT2-PS4](https://github.com/Stayhye/TTT2-PS4) | CREDITS.md blob SHA is identical in both: a4aafa5a8f185cf3c0c8e1585e8212f403e5ab00. The fork README still pins kit ff30ff6. | No alternate kit or newer allocator/WSI implementation found in the inspected files |
| [ConkerLR-PS4](https://github.com/iHaiDeeZ/ConkerLR-PS4) | [kleffeesantos-cyber/ConkerLR-PS4](https://github.com/kleffeesantos-cyber/ConkerLR-PS4) | CREDITS.md blob SHA is identical in both: d86eefcf29dcedaed5669ea547a5c6ad250b0248. The fork README still pins kit 87a517c and the build script sources ~/ps4-kit/toolchain/env.sh. | No alternate kit or runtime implementation found in the inspected files |
| [Godot PS4 branch](https://github.com/iHaiDeeZ/godot/tree/ps4) | No PS4-specific fork found in targeted search | Generic Godot forks are not evidence of a PS4-port fork | Public branch remains the useful direct-consumer contract |

## 2. Public runtime-source forks

### DolphinPS4

The following public forks expose the same allocator source blob as the upstream
DolphinPS4 main branch:

| Repository | File | Blob SHA |
|---|---|---|
| [iHaiDeeZ/DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4) | port/ps4_dlmalloc.c | f42999e5596d4652607a2c31a137a3a9531fd093 |
| [tehrzky/DolphinPS4](https://github.com/tehrzky/DolphinPS4) | port/ps4_dlmalloc.c | f42999e5596d4652607a2c31a137a3a9531fd093 |
| [kleffeesantos-cyber/DolphinPS4](https://github.com/kleffeesantos-cyber/DolphinPS4) | port/ps4_dlmalloc.c | f42999e5596d4652607a2c31a137a3a9531fd093 |
| [Glec6/DolphinPS4](https://github.com/Glec6/DolphinPS4) | port/ps4_dlmalloc.c | f42999e5596d4652607a2c31a137a3a9531fd093 |

This means these copies do not provide an independently changed heap adapter
in that file. One additional search result, Mrexel55/dolphinps4, did not expose
the expected port/ps4_dlmalloc.c path through the repository contents API; no
conclusion is drawn from that 404.

### HENkaku-PS4

Three public copies were checked:

- [maroomaroo284-beep/HENkaku-PS4](https://github.com/maroomaroo284-beep/HENkaku-PS4)
- [wigginsr287-rgb/HENkaku-PS4](https://github.com/wigginsr287-rgb/HENkaku-PS4)
- [4ShinOni/HENkaku-PS4](https://github.com/4ShinOni/HENkaku-PS4)

All three expose the same port/ps4_dlmalloc.c blob SHA as upstream HENkaku:
f42999e5596d4652607a2c31a137a3a9531fd093. No alternate allocator adapter
was found in the inspected file.

## 3. Separate/ambiguous fork results

- PS4SX2 search returns numerous same-name repositories. The sampled
  Phoenixx1202, Stayhye and tehrzky copies did not expose the distinctive
  ps4/build-ps4.sh or ps4/Makefile.ps4 paths used by iHaiDeeZ/PS4SX2. They do
  not reveal the CEMUPS4 source lineage in this pass.
- realfitrex-bot/PS4Liberty is a public fork of the discontinued port and its
  ps4/README.md confirms the same experimental runtime lineage. It is useful
  as a public mirror/reference, not an alternate ps4-kit implementation.
- JarnoVos/SeriousSamPS4-RADV is a separate downstream project, not a fork of
  DolphinPS4. It independently uses the DolphinPS4 RADV/Mesa recipe and has a
  distinct toolchain note: clang 21 compiles, PacBrew LLD 12.0.1 links.

## 4. Conclusion

The inspected forks mostly mirror upstream files rather than reconstructing
the missing private kit. The strongest independent information continues to
come from downstream integrations (M33, HENkaku, Conker, SSBU, SMW,
SeriousSamPS4-RADV) and from exact source/commit history, not from a fork that
already contains the missing ps4-kit.
