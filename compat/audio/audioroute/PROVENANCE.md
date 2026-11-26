# libaudioroute.so shim — provenance and design

`compat/audio/runtime/lib64/libaudioroute.so` is the only object in the marble
audio runtime that is built from this repository. Everything else in that
directory is an extraction from the device image (see
`compat/audio/runtime/manifest.json`).

## 1. Upstream sources (byte-verified)

Every file below was compared by SHA-256 against the upstream blob fetched on
2026-11-23; the bytes in this directory are identical to what the URL serves.

| Local file | Upstream URL | Branch/tag | SHA-256 | License |
| --- | --- | --- | --- | --- |
| `audio_route.c` | <https://android.googlesource.com/platform/system/media/+/refs/heads/main/audio_route/audio_route.c> | `refs/heads/main` | `b225150a9d34d97bbdf586f9c3b36705bc83f3a19d67b627e8d1994c032e6309` | Apache License 2.0 (header retained verbatim) |
| `include/audio_route/audio_route.h` | <https://android.googlesource.com/platform/system/media/+/refs/heads/main/audio_route/include/audio_route/audio_route.h> | `refs/heads/main` | `a77c7aa50beafab67ed6ada0de4bdeb010e011bfd0c09255bd90d948e8724c40` | Apache License 2.0 (header retained verbatim) |
| `include/tinyalsa/asoundlib.h` | <https://android.googlesource.com/platform/external/tinyalsa/+/refs/heads/main/include/tinyalsa/asoundlib.h> | `refs/heads/main` | `834c88686c5592555d8e9067d2a7950154789e34064f9edf703ddf5e3fb309d8` | AOSP BSD-3-Clause style (header retained verbatim) |
| `include/expat/expat.h` | <https://raw.githubusercontent.com/libexpat/libexpat/R_2_6_4/expat/lib/expat.h> | tag `R_2_6_4` | `0f750bc336e510d14ac9a3e63fc2399f60f3f04f0061c426e86751ed5fba90e4` | MIT (header retained verbatim) |
| `include/expat/expat_external.h` | same libexpat `R_2_6_4` tree, `expat/lib/expat_external.h` | tag `R_2_6_4` | `7ca9ed28dd5e08eac425931894fabd4c876db8f4be26f9b9a33c2e4f70a0d6c3` | MIT (header retained verbatim) |

The exact upstream **commit** of the two AOSP files could not be resolved:
`android.googlesource.com` answers `403 Please sign in to view the history
pages` for `+log` queries. The file content hash above is the identity we pin,
and it is reproducible with a plain blob fetch from `refs/heads/main`.
`refs/heads/android15-release` was fetched as well and is *different*
(`b89eacb7968bc2ddeb921be12e767e72fa7d684ce573b70a376cf40b0b3560eb`, 34374
bytes), i.e. the copy used here is newer than Android 15. That is deliberate:
the file is pure Bionic userspace code and every `mixer_*` symbol it needs is
present in the Android 15 `libtinyalsa.so` that ships in the runtime (see
`tests/test_marble_audio_runtime.py`).

`include/log/log.h` is **not** AOSP. It is a small MIT stand-in for
`<log/log.h>` (the NDK only ships `<android/log.h>`), see
[include/log/log.h](include/log/log.h).

## 2. Why an opaque-pointer shim instead of plain AOSP

The vendor splits `audio_route` across two libraries, and neither one exports
the full AOSP API:

* `libaudioroute_ext.so` (Qualcomm) exports only `audio_route_init_ext`,
  `audio_route_free_ext`, `audio_route_update_overlay_dynamic`,
  `dump_audio_route` — and it *imports* `audio_route_init`, `audio_route_free`
  and `audio_route_update_mixer` from `libaudioroute.so`.
* `libar-pal.so` imports `audio_route_init_ext` (from `_ext`) and then
  `audio_route_apply_and_update_path`, `audio_route_reset_and_update_path`,
  `audio_route_free` (from `libaudioroute.so`).

So the `struct audio_route *` that reaches our functions was allocated by
Qualcomm code. Disassembly of `libaudioroute_ext.so` (A15 marble dump) shows:

* `audio_route_init_ext` allocates its own state with `calloc(1, 0x28)` plus
  `mixer_open()`, i.e. it carries an AOSP-shaped 40-byte header in *this* build;
* it calls our `audio_route_init` **only** on the failure path where `fopen()`
  of the mixer XML fails;
* after parsing the XML it calls our `audio_route_update_mixer()` with **its
  own** pointer.

An AOSP-shaped allocation in one vendor build is not an ABI contract: QTI can
add fields, and a wrong offset dereference would crash inside PAL with a stack
trace that is very hard to attribute. Therefore every entry point in
[shim.c](shim.c) treats the incoming pointer as **opaque**:

* the caller pointer is never dereferenced (`(void)ar;` only);
* the real AOSP state is a private singleton, created on first use from
  `HMOS_AUDIO_CARD` (default `0`, marble only exposes `ukee-mtp-snd-card`) and
  `HMOS_AUDIO_MIXER_XML` (default
  `/vendor/etc/audio/sku_ukee/mixer_paths_ukee_mtp.xml`);
* `audio_route.c` is compiled into a private `hmos_ar_*` namespace
  (`-Daudio_route_init=hmos_ar_init` … plus `#undef` in `shim.c`), so AOSP code
  and the exported wrappers can never collide (`-fvisibility=hidden` and
  [exports.map](exports.map) keep everything else local);
* init failure (missing XML, unopenable card, XML parse error) returns
  `-ENOENT`, a NULL path name returns `-EINVAL`, and exactly one
  `AUDIOROUTE_SHIM_INIT_FAIL card=… xml=…` line is written to stderr. Nothing in
  the shim calls `abort()`; `LOG_ALWAYS_FATAL` in the log shim is downgraded to
  `ALOGE` because its only use site already falls through to `return 0`.

## 3. Known limitations (not hidden)

1. The overlay/dynamic state that `_ext` keeps for
   `audio_route_update_overlay_dynamic` / `dump_audio_route` is not applied
   through this shim. Both sides parse the same `mixer_paths_ukee_mtp.xml`, so
   the paths this shim applies are the same routes, but QTI overlay deltas are
   ignored.
2. Mixer writes that `_ext` performs on its own struct are not forwarded; the
   shim applies AOSP's initial-values pass plus every requested path itself.
3. The singleton is not teardown-concurrent-safe: `audio_route_free()` clears
   the pointer before freeing, but an in-flight `apply` on another thread could
   still race. PAL serialises resource-manager calls, so this is accepted for
   bring-up and must be revisited if PAL ever tears down while streaming.
4. **未验证 on device**: as of this commit the shim has only been validated on
   the host (exports, NEEDED closure, symbol coverage, NDK/AArch64 header) — the
   first `pal_server` start with `PAL_SERVER_READY=1` is still pending.

## 4. Build

```sh
python3 tools/build/build_current.py --out work/marble-audio-build audioroute
```

Target inside `tools/build/build_current.py`:

* NDK r27d (`27.3.13750724`), overridable with `HMOS_NDK` (default
  `/home/lazybones/android-ndk-r27d`), API level overridable with
  `HMOS_NDK_API` (default `33`);
* `--target=aarch64-linux-android33`, `-fPIC -shared -fvisibility=hidden`,
  `-Wl,-z,defs`, `-Wl,--version-script=compat/audio/audioroute/exports.map`;
* links `libtinyalsa.so`, `libexpat.so`, `liblog.so`, `libdl.so`, `libc.so` from
  the shipped Bionic runtime (`HMOS_AUDIO_BIONIC`, default
  `work/marble-bionic/lib64`) so the result loads against exactly the libraries
  that ship in `compat/audio/runtime/lib64`.

Exported symbols of the built object (verified by
`tests/test_marble_audio_runtime.py`): `audio_route_init`, `audio_route_free`,
`audio_route_update_mixer`, `audio_route_apply_and_update_path`,
`audio_route_reset_and_update_path` — the five names the vendor objects import,
nothing else.

## 5. License summary

* `audio_route.c`, `audio_route.h`: Apache License 2.0, headers retained.
* `tinyalsa/asoundlib.h`: AOSP BSD-style header retained.
* `expat/*`: MIT header retained.
* `shim.c`, `include/log/log.h`: MIT (`SPDX-License-Identifier: MIT`).
