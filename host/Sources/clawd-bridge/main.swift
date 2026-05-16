// clawd-bridge — Mac daemon that bridges Cardputer apps to host-side processes.
//
// Connects to the Cardputer over BLE, then routes:
//   device → bridge:  chat.send → ClaudeSession (spawns `claude --print`)
//   bridge → device:  chunks/end/errors as chat.chunk/chat.end/error events

import Foundation

// Disable stdout buffering so launchd/log tailing sees output in realtime.
setbuf(stdout, nil)
setbuf(stderr, nil)

let namePrefix = ProcessInfo.processInfo.environment["CLAWD_NAME_PREFIX"] ?? "Claude-Cardputer"
let claudeCwd  = ProcessInfo.processInfo.environment["CLAWD_CHAT_CWD"]
    .map { URL(fileURLWithPath: ($0 as NSString).expandingTildeInPath) }
    ?? URL(fileURLWithPath: NSHomeDirectory())

let central       = BLECentral(namePrefix: namePrefix)
let session       = ClaudeSession(cwd: claudeCwd)
let subscriptions = Subscriptions()
let tcp: TCPListener? = {
    do {
        return try TCPListener()  // announce() fires automatically once .ready
    } catch {
        fputs("[bridge] TCP listener failed: \(error). BLE-only mode.\n", stderr)
        return nil
    }
}()
let otaProxy: OTAProxy? = {
    do {
        return try OTAProxy()
    } catch {
        fputs("[bridge] OTA proxy failed: \(error). Device OTA will need to reach GitHub directly.\n", stderr)
        return nil
    }
}()

func send<T: Encodable>(_ value: T) {
    guard let line = Wire.encodeLine(value) else { return }
    // Fan out to whichever transport currently has a peer. BLE is preferred
    // for low-latency local use; TCP picks up when WiFi is the only path.
    if central.isReady       { central.send(line) }
    if tcp?.isConnected == true { tcp?.send(line) }
}

session.onChunk  = { chunk in send(Wire.ChatChunk(text: chunk)) }
session.onStatus = { text  in send(Wire.ChatStatus(text: text)) }
session.onEnd    = { tokens in send(Wire.ChatEnd(tokens: tokens)) }
session.onError  = { msg in
    fputs("[bridge] chat error: \(msg)\n", stderr)
    send(Wire.WireError(where: "chat", msg: msg))
}

subscriptions.emit = { report in
    // Re-tag pull-response as a push update.
    struct UsageUpdate: Encodable {
        let evt = "usage.update"
        let today:  UsageReport.Bucket
        let week:   UsageReport.Bucket
        let month:  UsageReport.Bucket
        let cost:   UsageReport.Cost
        let tokens: UsageReport.Tokens
        let asOf:   String
    }
    send(UsageUpdate(today:  report.today,
                     week:   report.week,
                     month:  report.month,
                     cost:   report.cost,
                     tokens: report.tokens,
                     asOf:   report.asOf))
}

central.onLine = { line in
    guard let env = Wire.decodeInbound(line) else {
        print("← (unparseable) \(line)")
        return
    }
    if env.evt == "chat.send", let text = env.text {
        print("→ chat.send: \(text.prefix(80))")
        session.send(text)
    } else if env.evt == "usage.request" {
        print("→ usage.request")
        Usage.collect { report in send(report) }
    } else if env.cmd == "subscribe", let ch = env.channel {
        subscriptions.subscribe(ch)
    } else if env.cmd == "unsubscribe", let ch = env.channel {
        subscriptions.unsubscribe(ch)
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
    subscriptions.clearAll()
}

// Same line handler covers both transports. The router doesn't care which
// transport delivered the frame.
let lineRouter = central.onLine!
tcp?.onLine        = lineRouter
tcp?.onConnected   = {
    print("[bridge] TCP peer connected")
}
tcp?.onDisconnect  = {
    print("[bridge] TCP peer disconnected")
    subscriptions.clearAll()
}

print("[bridge] starting (looking for \(namePrefix)*)")
dispatchMain()
