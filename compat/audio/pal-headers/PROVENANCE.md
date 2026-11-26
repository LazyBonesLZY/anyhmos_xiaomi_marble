# PAL headers — provenance

`tools/build/build_current.py` builds `libhmos_pal_server.so` and
`libhmos_pal_lifecycle.so` against the PAL API headers in this directory. They
are vendored here so the build no longer depends on an out-of-tree checkout
(previously `/root/hmos-reference/pal-sm8550-a13/inc`, which is not present on a
clean machine). Override with `HMOS_PAL_INCLUDE` if a different tree is wanted.

## Files

| Local file | Source | SHA-256 | License |
| --- | --- | --- | --- |
| `inc/PalApi.h` | `android_vendor_qcom_opensource_arpal-lx`, branch `lineage-24.0-caf-sm8450`, path `inc/PalApi.h` | `636b10dfd3b37a6d3b62cfa47ce39cc64755030d1d6baf29ddf492494bdb75df` | BSD-3-Clause, `Copyright (c) 2019-2021, The Linux Foundation` (header retained verbatim) |
| `inc/PalDefs.h` | same tree, path `inc/PalDefs.h` | `3926de9b6ae65b93666c435eccc6f736c61d0faefb087a7d888102ee1c5bab40` | BSD-3-Clause, `Copyright (c) 2019-2021, The Linux Foundation` (header retained verbatim) |

## Provenance chain

1. Upstream project recorded by the snapshot's `.gitupstream`:
   <https://git.codelinaro.org/clo/la/platform/vendor/qcom/opensource/arpal-lx>
2. The snapshot was taken from the LineageOS mirror
   (`android_vendor_qcom_opensource_arpal-lx`, branch `lineage-24.0-caf-sm8450`).
   The local tarball is `work/marble-pal-headers/arpal.tar.gz`; the two files
   above are byte-identical to the `inc/` entries inside it (verified by
   SHA-256).
3. **未验证**: the exact CodeLinaro commit SHA behind that mirror snapshot was
   not resolved, and no attempt was made to prove these headers match the
   `libar-pal.so` binary running on marble. What *is* verified is that the two
   shims which use them compile and link cleanly against the marble Bionic
   runtime, and that `pal_server.c` only needs `PalApi.h` (which pulls in
   `PalDefs.h`).

Only the two headers needed by `compat/audio/pal_server.c` are vendored; the
rest of the arpal tree (sessions, streams, resource manager) is intentionally
not imported.
