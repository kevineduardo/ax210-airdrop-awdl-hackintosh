// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ax210-airdrop-awdl-hackintosh contributors
//
// awdlctl: read the AirportItlwm AWDL counters and change the AWDL TX switches at
// run time, so options can be compared A/B without a reboot.
//
// Build:  make -C tools/awdlctl        (or: clang -O2 -framework IOKit -framework CoreFoundation -o awdlctl awdlctl.c)
// Usage:  awdlctl show
//         sudo awdlctl pump on|off | hometx on|off | follow on|off
//         sudo awdlctl lead <us> | dwellx <TU> | pumpus <us>
//
// Changing a value needs an administrator (the driver checks the caller's privilege).
// The driver only accepts the keys while -itlawdllisten is set.
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

enum awdlctl_kind { KIND_NONE, KIND_BOOL, KIND_NUM };

struct awdlctl_cmd {
    enum awdlctl_kind kind;
    const char *key;    // IORegistry property written to the driver
    long value;
};

static const struct {
    const char *name;
    enum awdlctl_kind kind;
    const char *key;
    long min, max;      // accepted range for numbers (the driver clamps again)
} kCommands[] = {
    { "pump",   KIND_BOOL, "AWDLTxPumpOn",     0, 1 },
    { "hometx", KIND_BOOL, "AWDLHomeTxOn",     0, 1 },
    { "follow", KIND_BOOL, "AWDLFollowPeerOn", 0, 1 },
    { "lead",   KIND_NUM,  "AWDLListenLeadUs", 1000, 60000 },
    { "dwellx", KIND_NUM,  "AWDLDwellExtraTU", 0, 200 },
    { "pumpus", KIND_NUM,  "AWDLPumpUs",       250, 10000 },
};

static const char *const kShowKeys[] = {
    "AWDLTxModes", "AWDLTxPump", "AWDLRocDelayHistMs", "AWDLRocStartVsSlotMs",
    "AWDLTxVsDwell", "AWDLTxOkFailByBand", "AWDLDataLatencyMs", "AWDLDataLatencyMaxMs",
    "AWDLTxFwOk", "AWDLTxFwFail", "AWDLTxFwBusy", "AWDLDataTx", "AWDLDataTxDeferred",
    "AWDLDataTxPending", "AWDLDataMcast", "AWDLSyncMaster", "AWDLPeerSync", "AWDLChanSeq",
    "AWDLPeerChanSeq", "AWDLListenByChan",
};

static void usage(FILE *f)
{
    fprintf(f,
        "usage: awdlctl show\n"
        "       awdlctl pump on|off | hometx on|off | follow on|off\n"
        "       awdlctl lead <us 1000..60000> | dwellx <TU 0..200> | pumpus <us 250..10000>\n");
}

// Parses argv into cmd. Returns 0 on success, 2 on a usage error.
// Pure function (no IOKit), so tests/unit can exercise it on any host.
int awdlctl_parse(int argc, char **argv, struct awdlctl_cmd *cmd)
{
    cmd->kind = KIND_NONE;
    cmd->key = NULL;
    cmd->value = 0;
    if (argc < 2)
        return 2;
    if (!strcmp(argv[1], "show"))
        return argc == 2 ? 0 : 2;
    if (argc != 3)
        return 2;
    for (size_t i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); i++) {
        if (strcmp(argv[1], kCommands[i].name))
            continue;
        cmd->kind = kCommands[i].kind;
        cmd->key = kCommands[i].key;
        if (kCommands[i].kind == KIND_BOOL) {
            if (!strcmp(argv[2], "on")) cmd->value = 1;
            else if (!strcmp(argv[2], "off")) cmd->value = 0;
            else return 2;
            return 0;
        }
        char *end = NULL;
        errno = 0;
        long v = strtol(argv[2], &end, 10);
        if (errno || end == argv[2] || *end != '\0' || v < kCommands[i].min || v > kCommands[i].max)
            return 2;
        cmd->value = v;
        return 0;
    }
    return 2;
}

#ifndef AWDLCTL_NO_MAIN
static kern_return_t setProp(io_service_t s, const struct awdlctl_cmd *cmd)
{
    CFStringRef k = CFStringCreateWithCString(NULL, cmd->key, kCFStringEncodingUTF8);
    CFTypeRef v;
    if (cmd->kind == KIND_BOOL)
        v = CFRetain(cmd->value ? kCFBooleanTrue : kCFBooleanFalse);
    else
        v = CFNumberCreate(NULL, kCFNumberLongType, &cmd->value);
    kern_return_t kr = IORegistryEntrySetCFProperty(s, k, v);
    CFRelease(k);
    CFRelease(v);
    return kr;
}

static void show(io_service_t s, const char *key)
{
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, k, NULL, 0);
    CFRelease(k);
    char buf[512] = "";
    if (v && CFGetTypeID(v) == CFStringGetTypeID())
        CFStringGetCString(v, buf, sizeof(buf), kCFStringEncodingUTF8);
    else if (v && CFGetTypeID(v) == CFNumberGetTypeID()) {
        long long n = 0;
        CFNumberGetValue(v, kCFNumberLongLongType, &n);
        snprintf(buf, sizeof(buf), "%lld", n);
    } else if (v && CFGetTypeID(v) == CFBooleanGetTypeID())
        snprintf(buf, sizeof(buf), "%s", CFBooleanGetValue(v) ? "true" : "false");
    printf("%-24s %s\n", key, buf);
    if (v) CFRelease(v);
}

int main(int argc, char **argv)
{
    if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        usage(stdout);
        return 0;
    }
    struct awdlctl_cmd cmd;
    if (awdlctl_parse(argc, argv, &cmd)) {
        usage(stderr);
        return 2;
    }
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AirportItlwmAWDLInterface"));
    if (!s) {
        fprintf(stderr, "AirportItlwmAWDLInterface not found (is awdl0 created? boot with -itlawdl)\n");
        return 1;
    }
    kern_return_t kr = 0;
    if (cmd.kind != KIND_NONE) {
        kr = setProp(s, &cmd);
        if (kr)
            fprintf(stderr, "set %s failed: 0x%x%s\n", cmd.key, kr,
                kr == kIOReturnNotPrivileged ? " (run with sudo)" : "");
    }
    for (size_t i = 0; i < sizeof(kShowKeys) / sizeof(kShowKeys[0]); i++)
        show(s, kShowKeys[i]);
    IOObjectRelease(s);
    return kr ? 1 : 0;
}
#endif
