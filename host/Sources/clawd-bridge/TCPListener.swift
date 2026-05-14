// TCP fallback for the bridge: listen on an ephemeral port, advertise via
// Bonjour (mDNS) as `_clawd-bridge._tcp.`. The Cardputer can browse for
// the service over WiFi when BLE isn't available — line-buffered JSON,
// same wire protocol as the BLE link.
//
// One connected peer at a time. Subsequent connections close the previous
// one — the firmware only opens a single TCP session.

import Foundation
import Network

final class TCPListener {
    private let listener: NWListener
    private var connection: NWConnection?
    private var rxBuffer = Data()
    private var netService: NetService?

    var onLine:       ((String) -> Void)?
    var onConnected:  (() -> Void)?
    var onDisconnect: (() -> Void)?

    init() throws {
        let params: NWParameters = .tcp
        params.acceptLocalOnly = false
        params.allowFastOpen   = true
        self.listener = try NWListener(using: params, on: .any)

        listener.newConnectionHandler = { [weak self] c in self?.accept(c) }
        listener.stateUpdateHandler   = { state in
            print("[tcp] listener state: \(state)")
        }
        listener.start(queue: .main)
    }

    func announce() {
        guard let port = listener.port?.rawValue else { return }
        let svc = NetService(domain: "local.",
                             type:   "_clawd-bridge._tcp.",
                             name:   "clawdputer-bridge",
                             port:   Int32(port))
        svc.publish()
        netService = svc
        print("[tcp] listening on port \(port), announced as clawdputer-bridge._clawd-bridge._tcp.local.")
    }

    func send(_ line: String) {
        guard let conn = connection else { return }
        guard let data = (line + "\n").data(using: .utf8) else { return }
        conn.send(content: data, completion: .contentProcessed { error in
            if let e = error { fputs("[tcp] send error: \(e)\n", stderr) }
        })
    }

    var isConnected: Bool {
        connection?.state == .ready
    }

    private func accept(_ c: NWConnection) {
        if connection != nil {
            print("[tcp] closing previous connection for new peer")
            connection?.cancel()
        }
        connection = c
        c.stateUpdateHandler = { [weak self] s in
            switch s {
            case .ready:
                print("[tcp] peer connected")
                self?.onConnected?()
                self?.receive()
            case .failed(let err):
                fputs("[tcp] connection failed: \(err)\n", stderr)
                self?.connection = nil
                self?.onDisconnect?()
            case .cancelled:
                self?.connection = nil
                self?.onDisconnect?()
            default: break
            }
        }
        c.start(queue: .main)
    }

    private func receive() {
        connection?.receive(minimumIncompleteLength: 1, maximumLength: 4096) { [weak self] data, _, isComplete, error in
            guard let self = self else { return }
            if let data = data, !data.isEmpty {
                self.rxBuffer.append(data)
                while let nl = self.rxBuffer.firstIndex(of: 0x0A) {
                    let lineData = self.rxBuffer.subdata(in: self.rxBuffer.startIndex..<nl)
                    self.rxBuffer.removeSubrange(self.rxBuffer.startIndex...nl)
                    if let s = String(data: lineData, encoding: .utf8) {
                        self.onLine?(s)
                    }
                }
            }
            if let error = error {
                fputs("[tcp] receive error: \(error)\n", stderr)
                self.connection?.cancel()
                return
            }
            if isComplete {
                self.connection?.cancel()
                return
            }
            self.receive()
        }
    }
}
