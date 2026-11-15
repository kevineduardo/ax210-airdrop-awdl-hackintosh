# Contributing

Thanks for helping. Test reports, bug fixes, support for new macOS builds, other Intel cards and other Wi-Fi chips are all welcome.

## The rules

1. **Tests are required.** Every pull request that changes behaviour adds or updates a test, and `tests/run.sh` must pass locally and in CI. See [what counts as a test](#tests) below.
2. **Disclose AI use.** If any part of your contribution (code, docs, analysis, commit messages) was produced with an AI tool, say so in the pull request: the tool, the model and version (for example "Claude Opus 5.5", "GPT-5", "Gemini 3 Pro"), and what it was used for. You remain responsible for every line: you must understand it, and driver changes must be tested on real hardware by a human. Undisclosed AI-generated contributions will be closed.
3. **No Apple material.** Do not add Apple binaries, kexts, kernel collections, `bluetoothd`, extracted symbol tables, disassembly or decompiled code, or headers copied from Apple SDKs/KDKs. Interface headers must contain only what interoperability requires, written by you, with a note of how you verified them. See [docs/macos-driver-development.md](docs/macos-driver-development.md#legal-boundaries-we-follow).
4. **Licenses.** Changes under `driver/` are GPL-2.0 (like upstream). Add or keep the two-line modification notice at the top of every source file you change there (the license check enforces it). Everything else is MIT and new files need an `SPDX-License-Identifier: MIT` header. By contributing you agree your work is released under these licenses.
5. **No personal data.** Redact MAC addresses, serial numbers (SMBIOS, MLB, ROM, UUID), network names and device names from logs, configs and docs.

## Tests

Run everything that does not need a build:

```sh
tests/run.sh            # static checks + unit tests
tests/run.sh kext DIR   # post-build checks on every AirportItlwm.kext under DIR
```

What exists today:

| Suite | Covers |
|---|---|
| `tests/static/check_boot_args.py` | Every `itlawdl*` boot-arg read by the driver is documented in `docs/boot-args.md`, and the other way around |
| `tests/static/check_licenses.py` | SPDX headers in our files; GPL modification notice in every changed driver source file |
| shellcheck, `zsh -n`, `perl -c`, `plutil -lint` | Scripts and plists |
| `tests/unit/test_kcsyms.py` | Kernel collection parser and vtable decoding (synthetic Mach-O, no Apple files) |
| `tests/unit/test_advsets_tables.py` | BlueToolFixup patch tables are consistent; the verifier searches the same patterns as the kext |
| `tests/unit/test_awdlctl.c` | `awdlctl` argument parsing and ranges |
| CI build matrix | AirportItlwm Sequoia/Tahoe × Debug/Release, BlueToolFixup Debug/Release, `awdlctl` |
| `tools/check-kext.sh` (CI, after every build) | One `ieee80211com`/`ieee80211_node`/... layout across all compile units; layout fingerprint |

What a test means for each kind of change:

- **Tools, scripts, parsers:** unit tests in `tests/unit/` (Python `unittest` or a small C program, wired into `tests/run.sh`).
- **New boot-arg or run-time switch:** document it in `docs/boot-args.md` (the static check fails otherwise) and, if it is a run-time switch, extend `awdlctl` and its test.
- **New BlueToolFixup table or pattern:** the table test must pass; include the `verify-advsets.pl` result (UUID and "exactly one match" per pattern, no binary) in the PR.
- **Driver logic that can be separated from IOKit** (timeline math, frame parsing, counters): move it into a plain function and unit-test it on the host. New pure code in the AWDL path should come with host tests.
- **Driver changes that need hardware:** CI must build all targets and pass `check-kext.sh`, and the PR must include a hardware test report: macOS build, card, boot-args, what you tested, before/after numbers from `scripts/monitor-awdl.sh` (redacted). A maintainer may ask for more tests before merging.

## Building

You need Xcode 16 (CI uses 16.4) and these third-party pieces, which are not stored in the repo:

```sh
git clone https://github.com/acidanthera/MacKernelSDK driver/AirportItlwm/MacKernelSDK
git -C driver/AirportItlwm/MacKernelSDK checkout 3f750085caa17ec3a7880f11c11bf4f48cd6a164
cp -R driver/AirportItlwm/MacKernelSDK driver/BlueToolFixup/MacKernelSDK
# Lilu 1.7.2 DEBUG from https://github.com/acidanthera/Lilu/releases → driver/BlueToolFixup/Lilu.kext
```

Then follow the `xcodebuild` commands in [.github/workflows/ci.yml](.github/workflows/ci.yml). Do a clean build after changing any header under `itl80211/`.

## Pull requests

- One topic per PR. Explain the problem, the change, and how you tested it. Fill in the PR template.
- Keep commits focused, with messages that say why.
- Upstream-worthy fixes to itlwm or BlueToolFixup should also go to the upstream projects; link the upstream PR.

## Reporting bugs and test results

Use the issue templates. A test report from a new machine, even "it doesn't work, here are the logs", is a real contribution.

## Conduct

Be kind and assume good faith. Harassment or personal attacks are not tolerated; maintainers may remove comments and block users who do this.
