# OpenCore: quirks and kernel patches

*Engineering notes, November 2026. Verified on macOS 15.8.1 (24H32) and/or 26.7.2 (25G332) as stated per section.*

Everything in this note was found and checked on 26.7.2 (25G332), Darwin kernel 25.6, OpenCore REL-108.

## 1. `PowerTimeoutKernelPanic` breaks networking on kernel 25.6

**What the quirk is meant to do.** `IOService::ackTimerTick` panics when a `com.apple.*` driver doesn't answer a power-state change in time. The quirk disables that panic.

**How it does it (`OcAppleKernelLib/CommonPatches.c`).**

1. It tries an inline patch in the function.
2. If the pattern doesn't match, it falls back (`mPowerStateTimeoutPanicMasterPatch`): it replaces the first occurrence of `"com.apple\0"` in the kernel with `"not.apple\0"`, so the driver-name comparison fails.

**What goes wrong on 25.6.** `ackTimerTick` now uses `strlen`/`strncmp` with the literal, the inline pattern no longer matches, and the fallback kicks in. The linker shares that literal (`0xffffff8000bf0f00` on 25G332) with `pf_check_compatible_anchor`. Result: pf no longer recognizes Apple's anchors, and user-space networking is turned off for the whole machine (full chain in [skywalk-pf-quic.md](skywalk-pf-quic.md)). This affects any network card, not just the AX210.

**How to confirm on a machine.** `sysctl net.filter_state` returns `pf` with the stock `pf.conf`; `strings OpenCore.efi | grep not.apple`; with DTrace, `strcmp(com.apple, not.apple)` shows up while `pfctl` runs.

**Fix used.**

- `Kernel > Quirks > PowerTimeoutKernelPanic = false`.
- A custom patch with the same intended effect that leaves the literal alone:

| Field | Value |
|---|---|
| Arch | x86_64 |
| Base | `__ZN9IOService12ackTimerTickEv` |
| Limit | 4096 |
| Count | 1 |
| Find | `85 C0 74 00 4C 8D 3D 00 00 00 00 4C 89 FF E8` |
| Mask | `FF FF FF 00 FF FF FF 00 00 00 00 FF FF FF FF` |
| Replace | `00 00 66 90 00 …` |
| ReplaceMask | `00 00 FF FF 00 …` |
| MinKernel / MaxKernel | 25.0.0 / 25.99.99 |

`Replace` and `ReplaceMask` are 15 bytes like `Find`; the `…` stands for zeros. The patch turns the `je` (`74 xx`) after `test eax,eax` into `66 90` (2-byte nop): the "it's a com.apple driver → panic" branch is never taken. Offset 0x5C1 in the function on 25G332; the pattern matches only once within the limit.

This is worth reporting to OpenCore: falling back to replacing a string literal is dangerous on any kernel where the literal is shared.

## 2. How to write a patch like this

1. Disassemble the function (`tools/kc/kcdis.py`) and pick bytes that occur only once within `Limit` from `Base`. Mask relative displacements (they change in every build).
2. Use `Base` with the symbol and a small `Limit` instead of searching the whole kernel.
3. `Count = 1` and a tight `MinKernel`/`MaxKernel` range: a patch that stops matching is better than one that matches somewhere else.
4. Simulate offline before rebooting: apply Find/Mask to the kernel on disk and count the matches.
5. Confirm in the running kernel. With DTrace: `tracemem` at the function address + offset. In the case above, `… 85 c0 66 90 4c 8d 3d …` (originally `85 c0 74 51`).

## 3. Editing `config.plist` without breaking it

- `plutil -insert Kernel.Patch.` with an empty index corrupts the file. Always give the index (`Kernel.Patch.25`).
- Copy `config.plist` somewhere safe before every change and run `plutil -lint` afterwards.
- While testing, change only a spare EFI and keep the main one on the last good configuration; bring them in line once the change is validated.
- `csr-active-config`: on the test machine the normal value is `430A0000`; `630A0000` adds the DTrace permission. Put it back after investigating and confirm with `csrutil status`.
- Put `boot-args` in `NVRAM > Delete` so the value in `config.plist` always wins.

## 4. When OpenCore refuses a kext

`Prelinked injection … Invalid Parameter` in the OpenCore log almost always means an import that does not resolve. The machine boots without that driver. `tools/check-kext.sh` checks for this before installing; keeping the OpenCore log written to a file helps you see it afterwards.

## 5. Note: AMD patch "Remove non-monotonic time panic"

The patch "Visual | thread_invoke, thread_dispatch | Remove non-monotonic time panic | 12.0+" (the same as upstream AMD_Vanilla, `Count 2`) first matches, on kernel 25.6, in `_kperf_lazy_wait_sample` and `_vmx_hv_support`, not in the functions in its title. Found by simulating its Find/Mask offline against the kernel on disk. It was not changed, and the machine was not tested without it; this is here for anyone reviewing the AMD patches on Tahoe.

## 6. Other notes

- `kern.msgbuf` does not go above 131072 with `msgbuf=` in boot-args; with `-v` and a driver that logs a lot, the start of `dmesg` is lost. The kexts in this repo avoid per-command logging in Release builds (`iwx_debug 0`).
- Don't mount the sealed snapshot of the running system (`mount_apfs -s`): it left the kernel failing to decompress binaries (`decmpfs … err 22`, SIGBUS in `diskutil`, `networksetup`) until the next reboot.
