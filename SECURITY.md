# Security

These kexts run in the kernel and parse frames sent over the air by nearby devices. Treat them as experimental.

If you find a vulnerability (for example a crash or memory corruption triggered by a crafted AWDL or 802.11 frame), please **do not open a public issue**. Report it privately through GitHub: *Security → Report a vulnerability* on this repository. Include the macOS build, the kext build (UUID or release), and how to reproduce it.

We will acknowledge the report within a week. Fixes to code that also exists upstream (OpenIntelWireless/itlwm, crazyhulk/itlwm, BrcmPatchRAM) will be coordinated with those projects.

Design choices that limit exposure:

- All AWDL code is off unless `-itlawdl` is set.
- Run-time switches can only be changed by an administrator.
- BlueToolFixup's 15.8.1 table patches `bluetoothd` only when its UUID matches the verified build.
