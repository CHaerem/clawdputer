// Tiny HTTP proxy that fronts the GitHub release artifacts the device
// needs for self-update. The device can't reliably do a TLS handshake
// to github.com — Arduino-ESP32's mbedtls config wants a ~36 KB
// contiguous heap block and the WiFi driver permanently fragments the
// heap below that. So instead the bridge fetches over HTTPS on the Mac
// and re-serves the bytes to the device over plain HTTP on the LAN.
//
// Endpoints (both GET):
//   /firmware/version  → text/plain   (version.txt body)
//   /firmware/bin      → octet-stream (firmware.bin body, streamed)
//
// Advertised via Bonjour as `_clawd-ota._tcp.` on a random port; the
// firmware discovers it the same way it finds the chat bridge.

import Foundation
import Network

final class OTAProxy {
    private let listener: NWListener
    private var netService: NetService?
    private var announced  = false

    private let releaseBase =
        "https://github.com/CHaerem/clawdputer/releases/latest/download"

    init() throws {
        let params: NWParameters = .tcp
        params.acceptLocalOnly = false
        self.listener = try NWListener(using: params, on: .any)
        listener.newConnectionHandler = { [weak self] c in self?.accept(c) }
        listener.stateUpdateHandler   = { [weak self] state in
            if case .ready = state { self?.announce() }
        }
        listener.start(queue: .main)
    }

    private func announce() {
        guard !announced, let port = listener.port?.rawValue else { return }
        let svc = NetService(domain: "local.",
                             type:   "_clawd-ota._tcp.",
                             name:   "clawdputer-ota",
                             port:   Int32(port))
        svc.publish()
        netService = svc
        announced = true
        print("[ota-proxy] http listening on port \(port), announced as clawdputer-ota._clawd-ota._tcp.local.")
    }

    private func accept(_ c: NWConnection) {
        c.stateUpdateHandler = { [weak self] state in
            switch state {
            case .ready:        self?.readRequest(c, buffer: Data())
            case .failed(let e): fputs("[ota-proxy] conn failed: \(e)\n", stderr); c.cancel()
            default: break
            }
        }
        c.start(queue: .main)
    }

    private func readRequest(_ c: NWConnection, buffer: Data) {
        c.receive(minimumIncompleteLength: 1, maximumLength: 4096) { [weak self] data, _, isComplete, error in
            guard let self = self else { return }
            var buf = buffer
            if let d = data { buf.append(d) }
            if let end = buf.range(of: Data([0x0D, 0x0A, 0x0D, 0x0A])) {
                let head = String(data: buf.subdata(in: 0..<end.lowerBound),
                                  encoding: .utf8) ?? ""
                self.respond(head: head, conn: c)
                return
            }
            if isComplete || error != nil { c.cancel(); return }
            if buf.count > 8192 { c.cancel(); return }   // junk
            self.readRequest(c, buffer: buf)
        }
    }

    private func respond(head: String, conn: NWConnection) {
        let firstLine = head.split(separator: "\n").first.map(String.init) ?? ""
        let parts = firstLine.split(separator: " ")
        guard parts.count >= 2, parts[0] == "GET" else {
            sendStatus(conn, status: "400 Bad Request"); return
        }
        let path = String(parts[1])
        let (upstreamPath, ctype): (String, String)
        switch path {
        case "/firmware/version":
            upstreamPath = "version.txt"
            ctype        = "text/plain; charset=utf-8"
        case "/firmware/bin":
            upstreamPath = "firmware.bin"
            ctype        = "application/octet-stream"
        default:
            sendStatus(conn, status: "404 Not Found"); return
        }
        let url = URL(string: "\(releaseBase)/\(upstreamPath)")!
        print("[ota-proxy] \(path) → \(url.absoluteString)")
        proxy(url: url, conn: conn, contentType: ctype)
    }

    private func proxy(url: URL, conn: NWConnection, contentType: String) {
        // Use dataTask so URLSession transparently follows GitHub's 302 to
        // objects.githubusercontent.com and handles TLS for us. The 1.7 MB
        // firmware.bin fits comfortably in memory, so streaming isn't
        // worth the bookkeeping.
        var req = URLRequest(url: url)
        req.timeoutInterval = 30
        req.setValue("clawd-bridge", forHTTPHeaderField: "User-Agent")
        let task = URLSession.shared.dataTask(with: req) { [weak self] data, response, error in
            guard let self = self else { return }
            if let e = error {
                fputs("[ota-proxy] upstream error: \(e)\n", stderr)
                self.sendStatus(conn, status: "502 Bad Gateway"); return
            }
            guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
                let code = (response as? HTTPURLResponse)?.statusCode ?? 0
                fputs("[ota-proxy] upstream http \(code)\n", stderr)
                self.sendStatus(conn, status: "502 Bad Gateway"); return
            }
            let body = data ?? Data()
            print("[ota-proxy] served \(body.count) bytes")
            self.send(conn: conn, status: "200 OK", body: body, contentType: contentType)
        }
        task.resume()
    }

    private func sendStatus(_ conn: NWConnection, status: String) {
        send(conn: conn, status: status, body: Data(), contentType: "text/plain")
    }

    private func send(conn: NWConnection, status: String, body: Data,
                      contentType: String) {
        var hdr = "HTTP/1.1 \(status)\r\n"
        hdr   += "Content-Length: \(body.count)\r\n"
        hdr   += "Content-Type: \(contentType)\r\n"
        hdr   += "Connection: close\r\n\r\n"
        var resp = hdr.data(using: .utf8) ?? Data()
        resp.append(body)
        conn.send(content: resp, completion: .contentProcessed { _ in
            conn.cancel()
        })
    }
}
