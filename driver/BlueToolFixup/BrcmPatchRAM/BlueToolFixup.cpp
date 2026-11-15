// Modified by the ax210-airdrop-awdl-hackintosh project, 2026-11 (strict, UUID-checked
// BLE advertising-sets patch table for macOS 15.8.1). See CHANGES-FROM-UPSTREAM.md. License: GPL-2.0.
//
//  BlueToolFixup.cpp
//  BrcmPatchRAM
//
//  Created by Dhinak G on 6/11/21.
//


#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_user.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/kern_version.hpp>
#include <Headers/kern_devinfo.hpp>

#include <mach-o/loader.h>


#define MODULE_SHORT "btlfx"


class EXPORT BlueToolFixup : public IOService {
    OSDeclareDefaultStructors(BlueToolFixup)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(BlueToolFixup, IOService)


IOService *BlueToolFixup::probe(IOService *provider, SInt32 *score) {
    return ADDPR(startSuccess) ? IOService::probe(provider, score) : nullptr;
}

bool BlueToolFixup::start(IOService *provider) {
    if (!IOService::start(provider)) {
        SYSLOG("init", "failed to start the parent");
        return false;
    }
    setProperty("VersionInfo", kextVersion);
    registerService();
    
    return true;
}


#pragma mark - Patches

static const uint8_t kSkipUpdateFilePathOriginal[] = "/etc/bluetool/SkipBluetoothAutomaticFirmwareUpdate";
static const uint8_t kSkipUpdateFilePathPatched[]  = "/System/Library/CoreServices/boot.efi";


// Workaround 12.4 Beta 3+ bug where macOS may detect the Bluetooth chipset twice
// Once as internal, and second as an external dongle:
// 'ERROR -- Third Party Dongle has the same address as the internal module'
// Mainly applicable for BCM2046 and BCM2070 chipsets (BT2.1)
static const uint8_t kSkipAddressCheckOriginal[] =
{
    0x48, 0x89, 0xF3,             // mov    rbx, rsi
    0xE8, 0x00, 0x00, 0x00, 0x00, // call   <somewhere>
    0x85, 0xC0,                   // test   eax, eax
    0x74, 0x1D,                   // je
};

static const uint8_t kSkipAddressCheckPatched[] =
{
    0x48, 0x89, 0xF3,             // mov        rbx, rsi
    0xE8, 0x00, 0x00, 0x00, 0x00, // call       <somewhere>
    0x85, 0xC0,                   // test       eax, eax
    0x72, 0x1D,                   // jb short
};

static const uint8_t kSkipAddressCheckMask[] =
{
    0xFF, 0xFF, 0xFF,
    0xFF, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF,
    0xFF, 0xFF,
};

static const uint8_t kVendorCheckOriginal[] =
{
    0x81, 0xFA,              // cmp edx
    0x5C, 0x0A, 0x00, 0x00,  // Vendor BRCM,
    0x74                     // jnz short
};

static const uint8_t kVendorCheckPatched[] =
{
    0x81, 0xFA,              // cmp edx
    0x5C, 0x0A, 0x00, 0x00,  // Vendor BRCM,
    0xEB                     // jmp short
};

// Workaround for bugged chipset range check that
// doesn't consider 0 "THIRD_PARTY_DONGLE" as valid.
// This patch allows bluetooth to turn back on after the first power cycle.
// See https://github.com/acidanthera/BrcmPatchRAM/pull/18 for more details.
static const uint8_t kBadChipsetCheckOriginal[] =
{
    0x81, 0xF9,              // cmp ecx
    0xCF, 0x07, 0x00, 0x00,  // int 1999
    0x72                     // jb short
};

static const uint8_t kBadChipsetCheckPatched[] =
{
    0x81, 0xF9,              // cmp ecx
    0xCF, 0x07, 0x00, 0x00,  // int 1999
    0xEB                     // jmp short
};

static const uint8_t kBadChipsetCheckOriginal13_3[] =
{
    0x81, 0xF9,              // cmp ecx
    0x9E, 0x0F, 0x00, 0x00,  // int 3998
    0x77, 0x1A               // ja short
};

static const uint8_t kBadChipsetCheckPatched13_3[] =
{
    0x90, 0x90,
    0x90, 0x90, 0x90, 0x90,
    0x90, 0x90
};

static const uint8_t kSkipInternalControllerNVRAMCheck13_3[] =
{
    0x41, 0x80, 0x00, 0x01, // xor     r15b, 1
    0x75, 0x00,             // jnz     short
    0x84, 0xDB,             // test    bl, bl
    0x75, 0x00              // jnz     short
};

static const uint8_t kSkipInternalControllerNVRAMCheckMask13_3[] =
{
    0xFF, 0xFF, 0x00, 0xFF,
    0xFF, 0x00,
    0xFF, 0xFF,
    0xFF, 0x00
};

static const uint8_t kSkipInternalControllerNVRAMCheckPatched13_3[] =
{
    0x90, 0x90, 0x90, 0x90,
    0x90, 0x90,
    0x90, 0x90,
    0x90, 0x90
};

static const uint8_t kBadChipsetCheckOriginal15_4[] =
{
    0x81, 0xF9,              // cmp ecx
    0x6F, 0x17, 0x00, 0x00,  // int 5999
    0x77, 0x30               // ja short
};

static const uint8_t kBadChipsetCheckPatched15_4[] =
{
    0x90, 0x90,
    0x90, 0x90, 0x90, 0x90,
    0x90, 0x90
};

// Optional universal workaround for bluetoothd Read Clock failures.
// Many third-party Bluetooth controllers (like Intel) fail Apple's HCI_Read_Clock check.
// When connecting to modern Apple headphones with W1/H1/H2 chips (e.g., AirPods Pro 2),
// this causes bluetoothd to abort A2DP audio bring-up in the Fast-Connect reconnect path.
// This results in the infamous "connected but no sound" issue for these devices.
// See: https://github.com/OpenIntelWireless/IntelBluetoothFirmware/issues/462
// We patch the Fast-Connect A2DP gate to unconditionally force the success path.
// Activated by boot-arg -btlfxa2dpcheck

// Apple FastConnect A2DP reconnect gate.
// Replaces "test r14d, r14d; je" with "xor r14d, r14d; jmp" to
// unconditionally take the audio-publish path.

// macOS 14.x/15.x/16.x (Sonoma/Sequoia/Tahoe)
//
// NOTE (behaviour intentionally unchanged): on macOS 15.8.1 (24H32, x86_64 LC_UUID
// 9D4727A2-1FC2-3777-BE26-7BAD0ED65EF0) this 15-byte pattern matches TWICE, on different pages,
// so both sites get patched (findAndReplace works per page):
//   0x1003ea091  test r14d; je; lea/mov os_log handle; mov esi, 0x10 (ERROR);
//                call -> outlined log "Failed to read BT clock"   <- the intended Read Clock gate
//                (same function region as "Fast Connect A2DP Report Connection attempt").
//   0x100229d2d  r14d = result of a virtual call (+0x1c8); mov esi, 0x11 (FAULT);
//                call -> outlined log "Unable to get firmware image file buffer"
//                (PCIe firmware loader path, near "PCIe successfully transferred firmware image file").
//                Not intended; it forces "success" if the firmware image buffer could not be obtained.
//                That path should not run with a USB dongle, so it is probably harmless, but it is a
//                false positive. Proposed (NOT applied) fix: extend the pattern with the os_log type,
//                i.e. append "48 89 DF BE 10 00 00 00" (mov rdi, rbx; mov esi, 0x10), which is unique on
//                15.8.1 (only 0x1003ea091). Needs re-checking on 14.x and 26.x before adopting.
static const uint8_t kFastConnectA2DPGateOriginal14_0[] =
{
    0x45, 0x85, 0xF6,
    0x74, 0x25,
    0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x8B, 0x18
};

static const uint8_t kFastConnectA2DPGatePatched14_0[] =
{
    0x45, 0x31, 0xF6,
    0xEB, 0x25,
    0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x8B, 0x18
};

static const uint8_t kFastConnectA2DPGateMask14_0[] =
{
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF
};

// macOS 12.x/13.x (Monterey/Ventura)
static const uint8_t kFastConnectA2DPGateOriginal12_0[] =
{
    0x85, 0xDB,
    0x74, 0x25,
    0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x8B, 0x18
};

static const uint8_t kFastConnectA2DPGatePatched12_0[] =
{
    0x31, 0xDB,
    0xEB, 0x25,
    0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x8B, 0x18
};

static const uint8_t kFastConnectA2DPGateMask12_0[] =
{
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF
};

// Optional: let bluetoothd use Bluetooth-spec (5.0) LE advertising sets on a THIRD_PARTY_DONGLE.
// bluetoothd decides "BT chip support for BT Spec advertisingsets" with a capability method
// (vtable +0x9c0 on 26.4.1) that only looks at the chipset enum, and chipset 0
// (THIRD_PARTY_DONGLE) is never in its list. With the flag off, LE_InitMultipleAdvertising
// falls back to the vendor (VSC) path, which also needs a non-zero chipset, so it returns
// 0x98 and LE advertising never starts ("Stack is not started"). That breaks
// AirDrop/Continuity discovery from iPhone.
// The patch replaces the capability call with "mov al, 1" at the only call site, so the
// rest (DisableSpecSets override, multi-advertising init) runs as Apple wrote it and sends
// standard HCI LE extended advertising commands, which BT 5.x controllers implement.
// Activated by boot-arg -btlfxadvsets

// macOS 26.4.1 (25E253) x86_64 bluetoothd, 0x10040e2ca
// mov rcx, [rax]; mov rdi, rax; call [rcx+0x9c0]; mov r14d, eax
static const uint8_t kSpecAdvSetsCapOriginal26[] =
{
    0x48, 0x8B, 0x08,
    0x48, 0x89, 0xC7,
    0xFF, 0x91, 0xC0, 0x09, 0x00, 0x00,
    0x41, 0x89, 0xC6
};

// mov rcx, [rax]; mov rdi, rax; mov al, 1; nop x4; mov r14d, eax
static const uint8_t kSpecAdvSetsCapPatched26[] =
{
    0x48, 0x8B, 0x08,
    0x48, 0x89, 0xC7,
    0xB0, 0x01, 0x90, 0x90, 0x90, 0x90,
    0x41, 0x89, 0xC6
};

// The extended scanning capability (vtable +0x9a8, 0x10040aae2 on 26.4.1). Apple enables it
// together with BT Spec advertising sets for every chipset it knows. Once the host has used
// LE extended advertising commands, the controller rejects legacy LE scan commands with
// Command Disallowed, so both must be on.
// mov rcx, [rax]; mov rdi, rax; call [rcx+0x9a8]; mov r14d, eax
static const uint8_t kExtScanCapOriginal26[] =
{
    0x48, 0x8B, 0x08,
    0x48, 0x89, 0xC7,
    0xFF, 0x91, 0xA8, 0x09, 0x00, 0x00,
    0x41, 0x89, 0xC6
};

// LE event mask (0x10017015c on 26.4.1): bluetoothd writes 0x1f (legacy LE events only) when
// the chipset is 0, so Extended Advertising Report, Advertising Set Terminated, etc. are
// masked and an extended scan never reports anything. Drop the cmove so the full mask
// (same as every Apple chipset) is used.
// test eax, eax; lea rax, [mask1f]; lea rdx, [maskFull]; cmove rdx, rax
static const uint8_t kLeEventMaskOriginal26[] =
{
    0x85, 0xC0,
    0x48, 0x8D, 0x05, 0xA0, 0xF2, 0x8F, 0x00,
    0x48, 0x8D, 0x15, 0xA1, 0xF2, 0x8F, 0x00,
    0x48, 0x0F, 0x44, 0xD0
};

static const uint8_t kLeEventMaskPatched26[] =
{
    0x85, 0xC0,
    0x48, 0x8D, 0x05, 0xA0, 0xF2, 0x8F, 0x00,
    0x48, 0x8D, 0x15, 0xA1, 0xF2, 0x8F, 0x00,
    0x0F, 0x1F, 0x40, 0x00
};

// Advertising set random address (0x10050ef2e on 26.4.1): for chipset <= 3 bluetoothd turns
// off LE Set Advertising Set Random Address, then enables sets whose own address type is
// random, which the controller rejects with Invalid HCI Command Parameters (0x12).
// Only skip that one call (jmp +7); the chipset == 0 block after it is left as is.
// cmp dword [rax+0x334], 3; ja +0x2a -> jmp +7
static const uint8_t kAdvSetAddrOriginal26[] =
{
    0x83, 0xB8, 0x34, 0x03, 0x00, 0x00, 0x03,
    0x77, 0x2A,
    0x31, 0xFF, 0xE8, 0xA2, 0x7D, 0xCE, 0xFF
};

static const uint8_t kAdvSetAddrPatched26[] =
{
    0x83, 0xB8, 0x34, 0x03, 0x00, 0x00, 0x03,
    0xEB, 0x07,
    0x31, 0xFF, 0xE8, 0xA2, 0x7D, 0xCE, 0xFF
};

// LE Extended Create Connection (0x10050eb5c on 26.4.1). With extended scanning on, the
// controller rejects legacy LE Create Connection (0x200d) with Command Disallowed, and after
// enough of those bluetoothd restarts itself ("Exceed max allowed consecutive command
// disallowed events"). Feed 1 into the setter of gEnableExtendedCreateConnectionCommand.
// movzx edi, byte [rbp-0xa2]; call ... -> mov edi, 1; nop2; call ...
static const uint8_t kExtCreateConnOriginal26[] =
{
    0x0F, 0xB6, 0xBD, 0x5E, 0xFF, 0xFF, 0xFF,
    0xE8, 0x2F, 0xB0, 0xCF, 0xFF
};

static const uint8_t kExtCreateConnPatched26[] =
{
    0xBF, 0x01, 0x00, 0x00, 0x00, 0x66, 0x90,
    0xE8, 0x2F, 0xB0, 0xCF, 0xFF
};

// Advertising set random address before every enable (0x1001f951b on 26.4.1). bluetoothd
// caches each set's random address and only sends LE Set Advertising Set Random Address
// when it changed. The Intel controller forgets the address when the set's parameters are
// rewritten (LE Set Extended Advertising Parameters with a random own address type), so
// the following enable fails with 0x12 over and over until the address rotates (in an
// HCI trace: 64693 failures without a fresh 0x2035, 0 with one). Always send it.
// mov edx, [rdx+rdi+3]; xor edx, [rax+rcx+0xd11]; or edx, r8d; je +0x80 -> nop6
static const uint8_t kAdvSetAddrResendOriginal26[] =
{
    0x8B, 0x54, 0x3A, 0x03,
    0x33, 0x94, 0x08, 0x11, 0x0D, 0x00, 0x00,
    0x44, 0x09, 0xC2,
    0x0F, 0x84, 0x80, 0x00, 0x00, 0x00
};

static const uint8_t kAdvSetAddrResendPatched26[] =
{
    0x8B, 0x54, 0x3A, 0x03,
    0x33, 0x94, 0x08, 0x11, 0x0D, 0x00, 0x00,
    0x44, 0x09, 0xC2,
    0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00
};

// ---------------------------------------------------------------------------------------------
// macOS 15.x Sequoia (Darwin 24.6.0), derived from 15.8.1 (24H32) x86_64 bluetoothd,
// LC_UUID 9D4727A2-1FC2-3777-BE26-7BAD0ED65EF0. Same six sites and same logic as 26.4.1; the
// vtable slots are 0x160 lower, the chipset field is at +0x2f8 instead of +0x334, and every
// rel32/disp32 differs. Patterns 1-2 are exact (the slot is the only distinguishing byte);
// 3-6 are masked (wildcards = 0x00 in find, 0x00 in mask). Each was checked to have exactly one
// match in the 15.8.1 x86_64 slice, inside a single 4 KiB page
// (research/verify-advsets.pl).
// Replace masks only cover the bytes that change, so wildcarded bytes are left as they are.
// ---------------------------------------------------------------------------------------------

// 1. BT Spec advertising sets capability, 0x1003a657c on 15.8.1
// mov rcx, [rax]; mov rdi, rax; call [rcx+0x860]; mov r14d, eax
static const uint8_t kSpecAdvSetsCapOriginal24[] =
{
    0x48, 0x8B, 0x08,
    0x48, 0x89, 0xC7,
    0xFF, 0x91, 0x60, 0x08, 0x00, 0x00,
    0x41, 0x89, 0xC6
};

// mov rcx, [rax]; mov rdi, rax; mov al, 1; nop x4; mov r14d, eax
static const uint8_t kSpecAdvSetsCapPatched24[] =
{
    0x48, 0x8B, 0x08,
    0x48, 0x89, 0xC7,
    0xB0, 0x01, 0x90, 0x90, 0x90, 0x90,
    0x41, 0x89, 0xC6
};

// 2. Extended scanning capability, 0x1003a2e46 on 15.8.1
// mov rcx, [rax]; mov rdi, rax; call [rcx+0x848]; mov r14d, eax  (patched: same as 1)
static const uint8_t kExtScanCapOriginal24[] =
{
    0x48, 0x8B, 0x08,
    0x48, 0x89, 0xC7,
    0xFF, 0x91, 0x48, 0x08, 0x00, 0x00,
    0x41, 0x89, 0xC6
};

// 3. LE event mask, 0x100144a37 on 15.8.1
// test eax, eax; lea rax, [mask1f]; lea rdx, [maskFull]; cmove rdx, rax -> drop the cmove
static const uint8_t kLeEventMaskOriginal24[] =
{
    0x85, 0xC0,
    0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x8D, 0x15, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x0F, 0x44, 0xD0
};

static const uint8_t kLeEventMaskMask24[] =
{
    0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF
};

static const uint8_t kLeEventMaskPatched24[] =
{
    0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0F, 0x1F, 0x40, 0x00
};

static const uint8_t kLeEventMaskPatchMask24[] =
{
    0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF
};

// 4. Advertising set random address, 0x10049d49e on 15.8.1
// cmp dword [rax+0x2f8], 3; ja +0x23; xor edi, edi; call setter -> jmp +7 (skip only that call)
static const uint8_t kAdvSetAddrOriginal24[] =
{
    0x83, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x03,
    0x77, 0x00,
    0x31, 0xFF, 0xE8
};

static const uint8_t kAdvSetAddrMask24[] =
{
    0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
    0xFF, 0x00,
    0xFF, 0xFF, 0xFF
};

static const uint8_t kAdvSetAddrPatched24[] =
{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xEB, 0x07,
    0x00, 0x00, 0x00
};

static const uint8_t kAdvSetAddrPatchMask24[] =
{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF,
    0x00, 0x00, 0x00
};

// 5. LE Extended Create Connection, 0x10049d0ee on 15.8.1
// movzx edi, byte [rbp-0xa2]; call setter(gEnableExtendedCreateConnectionCommand);
// movzx r15d, byte [rbp-...]  ->  mov edi, 1; nop2; call ...
static const uint8_t kExtCreateConnOriginal24[] =
{
    0x0F, 0xB6, 0xBD, 0x00, 0xFF, 0xFF, 0xFF,
    0xE8, 0x00, 0x00, 0x00, 0xFF,
    0x44, 0x0F, 0xB6, 0xBD
};

static const uint8_t kExtCreateConnMask24[] =
{
    0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF,
    0xFF, 0x00, 0x00, 0x00, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF
};

static const uint8_t kExtCreateConnPatched24[] =
{
    0xBF, 0x01, 0x00, 0x00, 0x00, 0x66, 0x90,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

static const uint8_t kExtCreateConnPatchMask24[] =
{
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

// 6. Advertising set random address resend, 0x1001ca5ac on 15.8.1
// mov esi, [rsi+rdi+3]; xor esi, [rcx+rdx+0xd11]; or esi, r8d; je +0x7f (short) -> nop2
static const uint8_t kAdvSetAddrResendOriginal24[] =
{
    0x8B, 0x00, 0x00, 0x03,
    0x33, 0x00, 0x00, 0x11, 0x0D, 0x00, 0x00,
    0x44, 0x09, 0x00,
    0x74, 0x00
};

static const uint8_t kAdvSetAddrResendMask24[] =
{
    0xFF, 0x00, 0x00, 0xFF,
    0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0x00,
    0xFF, 0x00
};

static const uint8_t kAdvSetAddrResendPatched24[] =
{
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00,
    0x66, 0x90
};

static const uint8_t kAdvSetAddrResendPatchMask24[] =
{
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00,
    0xFF, 0xFF
};

// ---------------------------------------------------------------------------------------------
// Per-macOS tables for -btlfxadvsets
// ---------------------------------------------------------------------------------------------

static constexpr size_t kAdvSetsPatchCount = 6;
static constexpr uint32_t kAdvSetsAllApplied = (1U << kAdvSetsPatchCount) - 1;

struct AdvSetsPatch {
    const char *name;           // logged as "[advsets] patched <name> in <path>"
    const uint8_t *find;
    const uint8_t *findMask;    // nullptr = exact match
    const uint8_t *replace;
    const uint8_t *replaceMask; // nullptr = replace every byte
    size_t size;                // find/findMask/replace/replaceMask all have this size
};

struct AdvSetsTable {
    const char *name;
    KernelVersion kernel;
    int minMinor, maxMinor;     // Darwin minor version range, inclusive
    const uint8_t *uuid;        // x86_64 LC_UUID of the bluetoothd the table was verified on, or nullptr
    const AdvSetsPatch *patches; // kAdvSetsPatchCount entries
};

#define ADVSETS_EXACT(n, f, r) { n, f, nullptr, r, nullptr, sizeof(f) }
#define ADVSETS_MASKED(n, f, fm, r, rm) { n, f, fm, r, rm, sizeof(f) }

static_assert(sizeof(kSpecAdvSetsCapOriginal24) == sizeof(kSpecAdvSetsCapPatched24), "size");
static_assert(sizeof(kExtScanCapOriginal24) == sizeof(kSpecAdvSetsCapPatched24), "size");
static_assert(sizeof(kLeEventMaskOriginal24) == sizeof(kLeEventMaskMask24) && sizeof(kLeEventMaskOriginal24) == sizeof(kLeEventMaskPatched24) && sizeof(kLeEventMaskOriginal24) == sizeof(kLeEventMaskPatchMask24), "size");
static_assert(sizeof(kAdvSetAddrOriginal24) == sizeof(kAdvSetAddrMask24) && sizeof(kAdvSetAddrOriginal24) == sizeof(kAdvSetAddrPatched24) && sizeof(kAdvSetAddrOriginal24) == sizeof(kAdvSetAddrPatchMask24), "size");
static_assert(sizeof(kExtCreateConnOriginal24) == sizeof(kExtCreateConnMask24) && sizeof(kExtCreateConnOriginal24) == sizeof(kExtCreateConnPatched24) && sizeof(kExtCreateConnOriginal24) == sizeof(kExtCreateConnPatchMask24), "size");
static_assert(sizeof(kAdvSetAddrResendOriginal24) == sizeof(kAdvSetAddrResendMask24) && sizeof(kAdvSetAddrResendOriginal24) == sizeof(kAdvSetAddrResendPatched24) && sizeof(kAdvSetAddrResendOriginal24) == sizeof(kAdvSetAddrResendPatchMask24), "size");
static_assert(sizeof(kSpecAdvSetsCapOriginal26) == sizeof(kSpecAdvSetsCapPatched26) && sizeof(kExtScanCapOriginal26) == sizeof(kSpecAdvSetsCapPatched26), "size");
static_assert(sizeof(kLeEventMaskOriginal26) == sizeof(kLeEventMaskPatched26) && sizeof(kAdvSetAddrOriginal26) == sizeof(kAdvSetAddrPatched26), "size");
static_assert(sizeof(kExtCreateConnOriginal26) == sizeof(kExtCreateConnPatched26) && sizeof(kAdvSetAddrResendOriginal26) == sizeof(kAdvSetAddrResendPatched26), "size");

// The log names are the ones the original -btlfxadvsets build printed, so the
// "[advsets] patched ..." lines look the same on every macOS version.
static const AdvSetsPatch kAdvSetsPatches24[kAdvSetsPatchCount] = {
    ADVSETS_EXACT("BT Spec advertising sets capability", kSpecAdvSetsCapOriginal24, kSpecAdvSetsCapPatched24),
    ADVSETS_EXACT("extended scanning capability", kExtScanCapOriginal24, kSpecAdvSetsCapPatched24),
    ADVSETS_MASKED("LE event mask", kLeEventMaskOriginal24, kLeEventMaskMask24, kLeEventMaskPatched24, kLeEventMaskPatchMask24),
    ADVSETS_MASKED("advertising set random address", kAdvSetAddrOriginal24, kAdvSetAddrMask24, kAdvSetAddrPatched24, kAdvSetAddrPatchMask24),
    ADVSETS_MASKED("extended create connection", kExtCreateConnOriginal24, kExtCreateConnMask24, kExtCreateConnPatched24, kExtCreateConnPatchMask24),
    ADVSETS_MASKED("advertising set address resend", kAdvSetAddrResendOriginal24, kAdvSetAddrResendMask24, kAdvSetAddrResendPatched24, kAdvSetAddrResendPatchMask24),
};

// 26.4.1 patterns unchanged (exact, no masks), applied exactly as the original build did.
static const AdvSetsPatch kAdvSetsPatches26[kAdvSetsPatchCount] = {
    ADVSETS_EXACT("BT Spec advertising sets capability", kSpecAdvSetsCapOriginal26, kSpecAdvSetsCapPatched26),
    ADVSETS_EXACT("extended scanning capability", kExtScanCapOriginal26, kSpecAdvSetsCapPatched26),
    ADVSETS_EXACT("LE event mask", kLeEventMaskOriginal26, kLeEventMaskPatched26),
    ADVSETS_EXACT("advertising set random address", kAdvSetAddrOriginal26, kAdvSetAddrPatched26),
    ADVSETS_EXACT("extended create connection", kExtCreateConnOriginal26, kExtCreateConnPatched26),
    ADVSETS_EXACT("advertising set address resend", kAdvSetAddrResendOriginal26, kAdvSetAddrResendPatched26),
};

#undef ADVSETS_EXACT
#undef ADVSETS_MASKED

// x86_64 LC_UUID of /usr/sbin/bluetoothd, macOS 15.8.1 (24H32)
static const uint8_t kBluetoothdUuid24H32[16] =
{
    0x9D, 0x47, 0x27, 0xA2, 0x1F, 0xC2, 0x37, 0x77,
    0xBE, 0x26, 0x7B, 0xAD, 0x0E, 0xD6, 0x5E, 0xF0
};

static const AdvSetsTable kAdvSetsTables[] = {
    // Darwin 24.6.x covers 15.6 - 15.8.x; only the 24H32 build is verified (UUID gate).
    { "macOS 15.8.1 (24H32)", KernelVersion::Sequoia, 6, 6, kBluetoothdUuid24H32, kAdvSetsPatches24 },
    // Original crazyhulk table. No UUID recorded for 25E253 yet (fill it in with
    // research/verify-advsets.pl on a 26.4.1 bluetoothd to make the gate strict there too).
    { "macOS 26.4.1 (25E253)", KernelVersion::Tahoe, 0, 99, nullptr, kAdvSetsPatches26 },
};

// All-or-nothing gate.
//
// Patches are applied from cs_validate_page, one 4 KiB page at a time, as bluetoothd pages are
// faulted in, so a page that was validated without a patch cannot be patched later. A pre-scan
// of the whole binary is not possible there either (the VM object lock is held; reading the
// file would deadlock). The decision is therefore taken up front from the Mach-O header page,
// which dyld touches before any bluetoothd code runs:
//   - the table's LC_UUID must equal the running bluetoothd's x86_64 LC_UUID. The six patterns
//     were verified offline to match exactly once in that build, so either all six get applied
//     (as their pages come in) or, on any other build, none is;
//   - if a page containing a patch site is validated before the header page was seen, nothing
//     is applied for this boot (Aborted).
// Tables without a UUID (26.4.1) and -btlfxadvsetsany fall back to the previous per-page
// behaviour (Unverified); the log then says so and reports how many of the six were applied.
enum AdvSetsState : uint32_t {
    AdvSetsDisabled = 0,   // boot-arg off or no table for this kernel
    AdvSetsPending,        // waiting for the bluetoothd Mach-O header page
    AdvSetsArmed,          // UUID verified, apply all
    AdvSetsUnverified,     // no UUID to check against, apply per page (legacy)
    AdvSetsRejected,       // UUID mismatch / not readable, apply none
    AdvSetsAborted,        // patch site seen before the header, apply none
};

static const AdvSetsTable *advSetsTable = nullptr;
static uint32_t advSetsState = AdvSetsDisabled;
static uint32_t advSetsApplied = 0;   // bit i = patch i applied at least once
static bool advSetsLoggedComplete = false;

static bool shouldPatchBoardId = false;
static bool shouldPatchAddress = false;
static bool shouldPatchNvramCheck = false;
static bool shouldPatchA2DPCheck = false;
static bool loggedA2DPCheckMiss = false;
static bool shouldPatchSpecAdvSets = false;

static constexpr size_t kBoardIdSize = sizeof("Mac-F60DEB81FF30ACF6");
static constexpr size_t kBoardIdSizeLegacy = sizeof("Mac-F22586C8");

static const char boardIdsWithUSBBluetooth[][kBoardIdSize] = {
    "Mac-F60DEB81FF30ACF6",
    "Mac-9F18E312C5C2BF0B",
    "Mac-937CB26E2E02BB01",
    "Mac-E43C1C25D4880AD6",
    "Mac-06F11FD93F0323C5",
    "Mac-06F11F11946D27C5",
    "Mac-A369DDC4E67F1C45",
    "Mac-FFE5EF870D7BA81A",
    "Mac-DB15BD556843C820",
    "Mac-B809C3757DA9BB8D",
    "Mac-65CE76090165799A",
    "Mac-4B682C642B45593E",
    "Mac-77F17D7DA9285301",
    "Mac-BE088AF8C5EB4FA2"
};

static mach_vm_address_t orig_cs_validate {};

#pragma mark - Kernel patching code

static inline void searchAndPatch(const void *haystack, size_t haystackSize, const char *path, const void *needle, size_t findSize, const void *patch, size_t replaceSize) {
    if (KernelPatcher::findAndReplace(const_cast<void *>(haystack), haystackSize, needle, findSize, patch, replaceSize))
        DBGLOG(MODULE_SHORT, "found string to patch at %s!", path);
}

template <size_t findSize, size_t replaceSize, typename T>
static inline void searchAndPatch(const void *haystack, size_t haystackSize, const char *path, const T (&needle)[findSize], const T (&patch)[replaceSize]) {
    searchAndPatch(haystack, haystackSize, path, needle, findSize * sizeof(T), patch, replaceSize * sizeof(T));
}

static inline void searchAndPatchWithMask(const void *haystack, size_t haystackSize, const char *path, const void *needle, size_t findSize, const void *findMask, size_t findMaskSize, const void *patch, size_t replaceSize, const void *patchMask, size_t replaceMaskSize) {
    if (KernelPatcher::findAndReplaceWithMask(const_cast<void *>(haystack), haystackSize, needle, findSize, findMask, findMaskSize, patch, replaceSize, patchMask, replaceMaskSize))
        DBGLOG(MODULE_SHORT, "found string to patch at %s!", path);
}

template <size_t findSize, size_t findMaskSize, size_t replaceSize, size_t replaceMaskSize, typename T>
static inline void searchAndPatchWithMask(const void *haystack, size_t haystackSize, const char *path, const T (&needle)[findSize], const T (&findMask)[findMaskSize], const T (&patch)[replaceSize], const T (&patchMask)[replaceMaskSize]) {
    searchAndPatchWithMask(haystack, haystackSize, path, needle, findSize * sizeof(T), findMask, findMaskSize * sizeof(T), patch, replaceSize * sizeof(T), patchMask, replaceMaskSize * sizeof(T));
}




static bool searchAndPatchNamedWithMask(const void *haystack, size_t haystackSize, const char *path, const char *name, const void *needle, size_t findSize, const void *findMask, size_t findMaskSize, const void *patch, size_t replaceSize, const void *patchMask, size_t replaceMaskSize) {
    if (!KernelPatcher::findAndReplaceWithMask(const_cast<void *>(haystack), haystackSize, needle, findSize, findMask, findMaskSize, patch, replaceSize, patchMask, replaceMaskSize))
        return false;

    SYSLOG(MODULE_SHORT, "[a2dpcheck] patched %s in %s", name, path);
    return true;
}

template <size_t findSize, size_t findMaskSize, size_t replaceSize, size_t replaceMaskSize, typename T>
static bool searchAndPatchNamedWithMask(const void *haystack, size_t haystackSize, const char *path, const char *name, const T (&needle)[findSize], const T (&findMask)[findMaskSize], const T (&patch)[replaceSize], const T (&patchMask)[replaceMaskSize]) {
    return searchAndPatchNamedWithMask(haystack, haystackSize, path, name, needle, findSize * sizeof(T), findMask, findMaskSize * sizeof(T), patch, replaceSize * sizeof(T), patchMask, replaceMaskSize * sizeof(T));
}

#pragma mark - -btlfxadvsets

static void selectAdvSetsTable(bool anyBuild) {
    int major = static_cast<int>(getKernelVersion());
    int minor = static_cast<int>(getKernelMinorVersion());
    for (size_t i = 0; i < arrsize(kAdvSetsTables); i++) {
        auto &t = kAdvSetsTables[i];
        if (getKernelVersion() == t.kernel && minor >= t.minMinor && minor <= t.maxMinor) {
            advSetsTable = &t;
            uint32_t state;
            if (t.uuid == nullptr) {
                state = AdvSetsUnverified;
                SYSLOG(MODULE_SHORT, "[advsets] selected table %s for Darwin %d.%d (no bluetoothd UUID recorded, per-page mode)", t.name, major, minor);
            } else if (anyBuild) {
                state = AdvSetsUnverified;
                SYSLOG(MODULE_SHORT, "[advsets] selected table %s for Darwin %d.%d (-btlfxadvsetsany: UUID check skipped, per-page mode)", t.name, major, minor);
            } else {
                state = AdvSetsPending;
                SYSLOG(MODULE_SHORT, "[advsets] selected table %s for Darwin %d.%d (waiting for bluetoothd UUID check)", t.name, major, minor);
            }
            __atomic_store_n(&advSetsState, state, __ATOMIC_RELEASE);
            return;
        }
    }
    __atomic_store_n(&advSetsState, AdvSetsDisabled, __ATOMIC_RELEASE);
    SYSLOG(MODULE_SHORT, "[advsets] no patch table for Darwin %d.%d, not patching", major, minor);
}

// Returns true if this page is the x86_64 Mach-O header of the binary. uuidFound tells whether
// LC_UUID was found entirely inside the page.
static bool readMachHeaderUuid(const void *data, uint8_t (&uuid)[16], bool &uuidFound) {
    uuidFound = false;
    auto header = static_cast<const mach_header_64 *>(data);
    if (header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_X86_64)
        return false;
    auto bytes = static_cast<const uint8_t *>(data);
    size_t off = sizeof(mach_header_64);
    for (uint32_t i = 0; i < header->ncmds; i++) {
        if (off + sizeof(load_command) > PAGE_SIZE)
            break;
        auto lc = reinterpret_cast<const load_command *>(bytes + off);
        if (lc->cmdsize < sizeof(load_command))
            break;
        if (lc->cmd == LC_UUID) {
            if (off + sizeof(uuid_command) <= PAGE_SIZE) {
                lilu_os_memcpy(uuid, reinterpret_cast<const uuid_command *>(lc)->uuid, sizeof(uuid));
                uuidFound = true;
            }
            break;
        }
        off += lc->cmdsize;
    }
    return true;
}

static void patchAdvSets(const void *data, const char *path) {
    uint32_t state = __atomic_load_n(&advSetsState, __ATOMIC_ACQUIRE);
    if (state == AdvSetsDisabled || state == AdvSetsRejected || state == AdvSetsAborted || advSetsTable == nullptr)
        return;

    if (state == AdvSetsPending) {
        uint8_t uuid[16] {};
        bool uuidFound = false;
        if (readMachHeaderUuid(data, uuid, uuidFound)) {
            uint32_t next;
            if (uuidFound && memcmp(uuid, advSetsTable->uuid, sizeof(uuid)) == 0) {
                next = AdvSetsArmed;
                SYSLOG(MODULE_SHORT, "[advsets] %s UUID matches table %s, applying all %u patches", path, advSetsTable->name, static_cast<unsigned>(kAdvSetsPatchCount));
            } else {
                next = AdvSetsRejected;
                if (uuidFound)
                    SYSLOG(MODULE_SHORT, "[advsets] %s UUID %02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X not verified for table %s, applying none",
                           path, uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
                           uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15], advSetsTable->name);
                else
                    SYSLOG(MODULE_SHORT, "[advsets] %s LC_UUID not found in the header page, applying none", path);
            }
            uint32_t expected = AdvSetsPending;
            __atomic_compare_exchange_n(&advSetsState, &expected, next, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
            // The header page itself holds no code, nothing else to do for it.
            return;
        }

        // Code page before the header page: if it holds a patch site we could never patch it
        // afterwards, so give up for this boot rather than end up with a partial set.
        for (size_t i = 0; i < kAdvSetsPatchCount; i++) {
            auto &p = advSetsTable->patches[i];
            size_t off = 0;
            if (KernelPatcher::findPattern(p.find, p.findMask, p.size, data, PAGE_SIZE, &off)) {
                uint32_t expected = AdvSetsPending;
                if (__atomic_compare_exchange_n(&advSetsState, &expected, AdvSetsAborted, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                    SYSLOG(MODULE_SHORT, "[advsets] %s site seen in %s before the Mach-O header was checked, applying none", p.name, path);
                return;
            }
        }
        return;
    }

    // Armed or Unverified: apply what is in this page.
    for (size_t i = 0; i < kAdvSetsPatchCount; i++) {
        auto &p = advSetsTable->patches[i];
        if (KernelPatcher::findAndReplaceWithMask(const_cast<void *>(data), PAGE_SIZE,
                                                  p.find, p.size, p.findMask, p.findMask ? p.size : 0,
                                                  p.replace, p.size, p.replaceMask, p.replaceMask ? p.size : 0)) {
            SYSLOG(MODULE_SHORT, "[advsets] patched %s in %s", p.name, path);
            uint32_t applied = __atomic_or_fetch(&advSetsApplied, 1U << i, __ATOMIC_ACQ_REL);
            if (applied == kAdvSetsAllApplied && !advSetsLoggedComplete) {
                advSetsLoggedComplete = true;
                SYSLOG(MODULE_SHORT, "[advsets] all %u patches applied (table %s%s)", static_cast<unsigned>(kAdvSetsPatchCount),
                       advSetsTable->name, state == AdvSetsUnverified ? ", unverified build" : "");
            } else if (state == AdvSetsUnverified) {
                SYSLOG(MODULE_SHORT, "[advsets] unverified build: %u of %u patches applied so far", static_cast<unsigned>(__builtin_popcount(applied)), static_cast<unsigned>(kAdvSetsPatchCount));
            }
        }
    }
}


#pragma mark - Patched functions

static void patched_cs_validate_page(vnode_t vp, memory_object_t pager, memory_object_offset_t page_offset, const void *data, int *validated_p, int *tainted_p, int *nx_p) {
    char path[PATH_MAX];
    int pathlen = PATH_MAX;
    FunctionCast(patched_cs_validate_page, orig_cs_validate)(vp, pager, page_offset, data, validated_p, tainted_p, nx_p);
    static constexpr size_t dirLength = sizeof("/usr/sbin/")-1;
    if (vn_getpath(vp, path, &pathlen) == 0 && UNLIKELY(strncmp(path, "/usr/sbin/", dirLength) == 0)) {
        if (strcmp(path + dirLength, "BlueTool") == 0) {
            searchAndPatch(data, PAGE_SIZE, path, kSkipUpdateFilePathOriginal, kSkipUpdateFilePathPatched);
            if (shouldPatchBoardId)
                searchAndPatch(data, PAGE_SIZE, path, boardIdsWithUSBBluetooth[0], kBoardIdSize, BaseDeviceInfo::get().boardIdentifier, kBoardIdSize);
        }
        else if (strcmp(path + dirLength, "bluetoothd") == 0) {
            searchAndPatch(data, PAGE_SIZE, path, kVendorCheckOriginal, kVendorCheckPatched);
            searchAndPatch(data, PAGE_SIZE, path, kBadChipsetCheckOriginal, kBadChipsetCheckPatched);
            if (getKernelVersion() >= KernelVersion::Ventura && getKernelVersion() < KernelVersion::Sequoia)
                searchAndPatch(data, PAGE_SIZE, path, kBadChipsetCheckOriginal13_3, kBadChipsetCheckPatched13_3);
            if (getKernelVersion() >= KernelVersion::Sequoia)
                searchAndPatch(data, PAGE_SIZE, path, kBadChipsetCheckOriginal15_4, kBadChipsetCheckPatched15_4);
            if (shouldPatchNvramCheck)
                searchAndPatchWithMask(data, PAGE_SIZE, path, kSkipInternalControllerNVRAMCheck13_3, sizeof(kSkipInternalControllerNVRAMCheck13_3), kSkipInternalControllerNVRAMCheckMask13_3, sizeof(kSkipInternalControllerNVRAMCheckMask13_3), kSkipInternalControllerNVRAMCheckPatched13_3, sizeof(kSkipInternalControllerNVRAMCheckPatched13_3), nullptr, 0);
            if (shouldPatchBoardId)
                searchAndPatch(data, PAGE_SIZE, path, boardIdsWithUSBBluetooth[0], kBoardIdSize, BaseDeviceInfo::get().boardIdentifier, kBoardIdSize);
            if (shouldPatchAddress)
                searchAndPatchWithMask(data, PAGE_SIZE, path, kSkipAddressCheckOriginal, kSkipAddressCheckMask, kSkipAddressCheckPatched, kSkipAddressCheckMask);
            if (shouldPatchA2DPCheck) {
                bool patched = false;
                
                // macOS 14.x/15.x/26.x (Sonoma/Sequoia/Tahoe) pattern
                if (getKernelVersion() >= KernelVersion::Sonoma) {
                    patched |= searchAndPatchNamedWithMask(data, PAGE_SIZE, path, "fast-connect A2DP BT-clock gate (macOS 14+)", kFastConnectA2DPGateOriginal14_0, kFastConnectA2DPGateMask14_0, kFastConnectA2DPGatePatched14_0, kFastConnectA2DPGateMask14_0);
                }
                // macOS 12.x/13.x (Monterey/Ventura) pattern
                else if (getKernelVersion() >= KernelVersion::Monterey) {
                    patched |= searchAndPatchNamedWithMask(data, PAGE_SIZE, path, "fast-connect A2DP BT-clock gate (macOS 12/13)", kFastConnectA2DPGateOriginal12_0, kFastConnectA2DPGateMask12_0, kFastConnectA2DPGatePatched12_0, kFastConnectA2DPGateMask12_0);
                }

                if (!patched && !loggedA2DPCheckMiss) {
                    loggedA2DPCheckMiss = true;
                    SYSLOG(MODULE_SHORT, "[a2dpcheck] enabled for bluetoothd, but no known FastConnect Gate pattern was seen yet");
                }
            }
            // -btlfxadvsets: per-macOS table, all-or-nothing gate (see patchAdvSets)
            if (shouldPatchSpecAdvSets)
                patchAdvSets(data, path);
        }
    }
}


#pragma mark - Patches on start/stop

static void pluginStart() {
    SYSLOG(MODULE_SHORT, "start");
    // There is no point in routing cs_validate_range, because this kext should only be running on Monterey+
    if (getKernelVersion() < KernelVersion::Monterey)
        return;
    lilu.onPatcherLoadForce([](void *user, KernelPatcher &patcher) {
        if (getKernelVersion() >= KernelVersion::Sonoma) {
            shouldPatchBoardId = checkKernelArgument("-btlfxboardid");
        } else {
            auto boardId = BaseDeviceInfo::get().boardIdentifier;
            auto boardIdSize = strlen(boardId) + 1;
            shouldPatchBoardId = boardIdSize == kBoardIdSize || boardIdSize == kBoardIdSizeLegacy;
            if (shouldPatchBoardId)
                for (size_t i = 0; i < arrsize(boardIdsWithUSBBluetooth); i++)
                    if (strcmp(boardIdsWithUSBBluetooth[i], boardId) == 0) {
                        shouldPatchBoardId = false;
                        break;
                    }
        }
        if ((getKernelVersion() == KernelVersion::Monterey && getKernelMinorVersion() >= 5) || getKernelVersion() > KernelVersion::Monterey)
            // 12.4 Beta 3+, XNU 21.5
            shouldPatchAddress = checkKernelArgument("-btlfxallowanyaddr");
        if (getKernelVersion() <= KernelVersion::Sonoma)
            shouldPatchNvramCheck = checkKernelArgument("-btlfxnvramcheck");
        shouldPatchA2DPCheck = checkKernelArgument("-btlfxa2dpcheck");
        shouldPatchSpecAdvSets = checkKernelArgument("-btlfxadvsets");
        if (shouldPatchSpecAdvSets) {
            SYSLOG(MODULE_SHORT, "[advsets] BT Spec LE advertising sets for third-party controllers enabled");
            selectAdvSetsTable(checkKernelArgument("-btlfxadvsetsany"));
        }
        if (shouldPatchA2DPCheck)
            SYSLOG(MODULE_SHORT, "[a2dpcheck] bluetoothd Read Clock workaround enabled (fast-connect A2DP bypass)");
        KernelPatcher::RouteRequest csRoute = KernelPatcher::RouteRequest("_cs_validate_page", patched_cs_validate_page, orig_cs_validate);
        if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &csRoute, 1))
            SYSLOG(MODULE_SHORT, "failed to route cs validation pages");
    });
}

// Boot args.
static const char *bootargOff[] {
    "-btlfxoff"
};
static const char *bootargDebug[] {
    "-btlfxdbg"
};
static const char *bootargBeta[] {
    "-btlfxbeta"
};

// Plugin configuration.
PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::Monterey,
    KernelVersion::Tahoe,
    pluginStart
};
