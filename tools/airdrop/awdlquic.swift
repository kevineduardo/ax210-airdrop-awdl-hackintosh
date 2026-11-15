// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ax210-airdrop-awdl-hackintosh contributors
//
// awdlquic: QUIC throughput test between two Macs over one interface (default awdl0),
// using Network.framework with peer-to-peer enabled, like AirDrop.
//
// Build:  swiftc -O -o awdlquic tools/airdrop/awdlquic.swift
// Server: ./awdlquic server <identity.p12> <password>
// Client: ./awdlquic client [ifname|any]
//
// The server advertises _awdlquic._udp over Bonjour; the client browses for it,
// connects over the requested interface, uploads 12 MB and downloads 12 MB back,
// and prints the rate of each direction. Make a throwaway identity for the server:
//   openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 2 -subj /CN=awdlquic
//   openssl pkcs12 -export -inkey key.pem -in cert.pem -out id.p12 -passout pass:test
// Test tool only: the client accepts any server certificate.
import Foundation
import Network
import Security

let alpn = "awdlquic", serviceType = "_awdlquic._udp", N = 12_000_000
let args = CommandLine.arguments
let isServer = args.count > 1 && args[1] == "server"
setvbuf(stdout, nil, _IONBF, 0)

func params(_ identity: sec_identity_t?) -> NWParameters {
    let q = NWProtocolQUIC.Options(alpn: [alpn])
    q.direction = .bidirectional
    q.idleTimeout = 30_000
    let sec = q.securityProtocolOptions
    if let id = identity { sec_protocol_options_set_local_identity(sec, id) }
    sec_protocol_options_set_verify_block(sec, { _, _, done in done(true) }, DispatchQueue.global())
    let p = NWParameters(quic: q)
    p.includePeerToPeer = true
    return p
}

func now() -> Double { Double(DispatchTime.now().uptimeNanoseconds) / 1e9 }

func sendAll(_ c: NWConnection, _ left: Int, _ done: @escaping () -> Void) {
    if left == 0 { done(); return }
    let n = min(left, 65536)
    c.send(content: Data(count: n), completion: .contentProcessed { e in
        if let e = e { print("send error \(e)"); if isServer { return }; exit(2) }
        sendAll(c, left - n, done)
    })
}

func recvAll(_ c: NWConnection, _ got: Int, _ first: @escaping () -> Void, _ done: @escaping () -> Void) {
    c.receive(minimumIncompleteLength: 1, maximumLength: 1 << 20) { d, _, _, e in
        let n = d?.count ?? 0
        if got == 0 && n > 0 { first() }
        if got + n >= N { done(); return }
        if let e = e { print("recv error \(e) after \(got + n) bytes"); if isServer { return }; exit(3) }
        recvAll(c, got + n, first, done)
    }
}

func loadIdentity(_ path: String, _ password: String) -> sec_identity_t {
    guard let data = try? Data(contentsOf: URL(fileURLWithPath: path)) else { print("cannot read \(path)"); exit(1) }
    var items: CFArray?
    var o: [String: Any] = [kSecImportExportPassphrase as String: password]
    if #available(macOS 15.0, *) { o[kSecImportToMemoryOnly as String] = true }
    let st = SecPKCS12Import(data as CFData, o as CFDictionary, &items)
    guard st == errSecSuccess, let arr = items as? [[String: Any]], let first = arr.first,
          let ref = first[kSecImportItemIdentity as String] else { print("p12 import failed (\(st))"); exit(1) }
    guard let id = sec_identity_create(ref as! SecIdentity) else { print("no identity in \(path)"); exit(1) }
    return id
}

if args.count >= 4 && args[1] == "server" {
    let l: NWListener
    do { l = try NWListener(using: params(loadIdentity(args[2], args[3]))) } catch { print("listener: \(error)"); exit(1) }
    l.service = NWListener.Service(name: "awdlquic", type: serviceType)
    l.stateUpdateHandler = { print("listener \($0) port \(l.port?.rawValue ?? 0)") }
    l.newConnectionHandler = { c in
        c.stateUpdateHandler = { s in
            print("conn \(s) path \(c.currentPath?.availableInterfaces.first?.name ?? "?")")
            if case .ready = s {
                let t0 = now()
                recvAll(c, 0, {}, {
                    let t1 = now()
                    print(String(format: "server received %d bytes in %.2fs", N, t1 - t0))
                    sendAll(c, N) { print(String(format: "server queued %d bytes back in %.2fs", N, now() - t1)) }
                })
            }
        }
        c.start(queue: .main)
    }
    l.start(queue: .main)
} else if args.count >= 2 && args[1] == "client" {
    let want = args.count >= 3 ? args[2] : "awdl0"
    var started = false
    let b = NWBrowser(for: .bonjour(type: serviceType, domain: nil), using: params(nil))
    b.browseResultsChangedHandler = { results, _ in
        for r in results where !started {
            let names = r.interfaces.map { $0.name }
            print("found \(r.endpoint) on \(names)")
            guard want == "any" || names.contains(want) else { continue }
            started = true
            let p = params(nil)
            if want != "any", let i = r.interfaces.first(where: { $0.name == want }) { p.requiredInterface = i }
            let c = NWConnection(to: r.endpoint, using: p)
            let tc = now()
            c.stateUpdateHandler = { s in
                print("conn \(s)")
                if case .failed = s { exit(4) }
                if case .ready = s {
                    let t0 = now()
                    let via = c.currentPath?.availableInterfaces.map { $0.name }.joined(separator: ",") ?? "?"
                    print(String(format: "ready in %.0f ms via %@", (t0 - tc) * 1000, via))
                    var t1 = 0.0
                    sendAll(c, N) { print(String(format: "client queued %d bytes in %.2fs", N, now() - t0)) }
                    recvAll(c, 0, { t1 = now() }, {
                        let t2 = now()
                        print(String(format: "UPLOAD   %d bytes in %.2fs = %.0f kB/s", N, t1 - t0, Double(N) / (t1 - t0) / 1000))
                        print(String(format: "DOWNLOAD %d bytes in %.2fs = %.0f kB/s", N, t2 - t1, Double(N) / (t2 - t1) / 1000))
                        exit(0)
                    })
                }
            }
            c.start(queue: .main)
        }
    }
    b.stateUpdateHandler = { print("browser \($0)") }
    b.start(queue: .main)
    DispatchQueue.main.asyncAfter(deadline: .now() + 90) { print("timeout: no server found or transfer incomplete"); exit(5) }
} else {
    print("usage: awdlquic server <identity.p12> <password> | awdlquic client [ifname|any]")
    exit(64)
}
dispatchMain()
