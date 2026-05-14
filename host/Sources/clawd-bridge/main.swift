// clawd-bridge — Mac daemon that bridges Cardputer apps to host-side processes.
//
// Connects to the Cardputer over BLE, then routes:
//   device → bridge:  chat.send → ClaudeSession (spawns `claude --print`)
//   bridge → device:  chunks/end/errors as chat.chunk/chat.end/error events

import Foundation

let namePrefix = ProcessInfo.processInfo.environment["CLAWD_NAME_PREFIX"] ?? "Claude-Cardputer"
let claudeCwd  = ProcessInfo.processInfo.environment["CLAWD_CHAT_CWD"]
    .map { URL(fileURLWithPath: ($0 as NSString).expandingTildeInPath) }
    ?? URL(fileURLWithPath: NSHomeDirectory())

let central = BLECentral(namePrefix: namePrefix)
let session = ClaudeSession(cwd: claudeCwd)

func send<T: Encodable>(_ value: T) {
    guard let line = Wire.encodeLine(value) else { return }
    central.send(line)
}

session.onChunk = { chunk in send(Wire.ChatChunk(text: chunk)) }
session.onEnd   = { tokens in send(Wire.ChatEnd(tokens: tokens)) }
session.onError = { msg in
    fputs("[bridge] chat error: \(msg)\n", stderr)
    send(Wire.WireError(where: "chat", msg: msg))
}

central.onLine = { line in
    guard let env = Wire.decodeInbound(line) else {
        print("← (unparseable) \(line)")
        return
    }
    if env.evt == "chat.send", let text = env.text {
        print("→ chat.send: \(text.prefix(80))")
        session.send(text)
    } else if env.cmd == "hello" || env.evt == nil && env.cmd == nil {
        // Silent — device-side hello or unrelated frame.
    } else {
        print("← \(line)")
    }
}

central.onReady = {
    print("[bridge] connected — chat cwd: \(claudeCwd.path)")
    send(Wire.Hello())
}

central.onDisconnect = {
    fputs("[bridge] device disconnected — rescanning\n", stderr)
}

print("[bridge] starting (looking for \(namePrefix)*)")
dispatchMain()
