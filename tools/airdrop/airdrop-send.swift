// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ax210-airdrop-awdl-hackintosh contributors
//
// airdrop-send: open the system AirDrop share sheet for one file, to start a test
// transfer from the command line (pick the receiver in the sheet).
//
// Build: swiftc -O -o airdrop-send tools/airdrop/airdrop-send.swift
// Usage: ./airdrop-send <file>
//
// The process exits after 170 s, long enough for a large transfer to finish.
import AppKit

let args = CommandLine.arguments
guard args.count == 2 else { print("usage: airdrop-send <file>"); exit(64) }
let url = URL(fileURLWithPath: args[1])
guard FileManager.default.isReadableFile(atPath: url.path) else { print("cannot read \(url.path)"); exit(66) }
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
    guard let svc = NSSharingService(named: .sendViaAirDrop) else { print("no AirDrop service"); exit(1) }
    if !svc.canPerform(withItems: [url]) { print("AirDrop cannot send this item"); exit(1) }
    app.activate(ignoringOtherApps: true)
    svc.perform(withItems: [url])
    print("share sheet opened")
}
DispatchQueue.main.asyncAfter(deadline: .now() + 170) { exit(0) }
app.run()
