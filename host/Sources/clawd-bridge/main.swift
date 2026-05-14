// clawd-bridge — Mac daemon that bridges Cardputer apps to host-side processes
// (claude CLI, ssh, …).
//
// MVP: connects to the Cardputer over BLE NUS, prints received lines, and
// forwards lines read from stdin to the device. No claude CLI integration
// yet — that lands once the dial tone is verified end-to-end.

import Foundation

let namePrefix = ProcessInfo.processInfo.environment["CLAWD_NAME_PREFIX"] ?? "Claude-Cardputer"

let central = BLECentral(namePrefix: namePrefix)

central.onLine = { line in
    // Print received lines on their own line; prefix so they're easy to spot
    // in an interleaved REPL.
    print("← \(line)")
}

central.onReady = {
    print("[bridge] connected — type a JSON line and press enter (ctrl-d to exit)")
    if let hello = Wire.encodeLine(Wire.Hello.make()) {
        central.send(hello)
    }

    Thread.detachNewThread {
        while let line = readLine() {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            guard !trimmed.isEmpty else { continue }
            DispatchQueue.main.async { central.send(trimmed) }
        }
        DispatchQueue.main.async {
            print("[bridge] stdin closed — exiting")
            exit(0)
        }
    }
}

central.onDisconnect = {
    fputs("[bridge] device disconnected\n", stderr)
}

print("[bridge] starting (looking for \(namePrefix)*)")
dispatchMain()
