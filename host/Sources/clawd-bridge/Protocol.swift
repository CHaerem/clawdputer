// JSON wire types for the bridge protocol. Mirrors protocol/WIRE.md.
//
// Encoded as one JSON object per line; the BLECentral handles framing.

import Foundation

enum Wire {
    struct Hello: Codable {
        let cmd: String   // "hello"
        let bridge: String
        let ver: Int
        static func make() -> Hello { .init(cmd: "hello", bridge: "clawd-bridge", ver: 1) }
    }

    struct Focus: Codable {
        let cmd: String   // "focus"
        let app: String
    }

    struct ChatSend: Codable {
        let evt: String   // "chat.send"
        let text: String
    }

    static let encoder: JSONEncoder = {
        let e = JSONEncoder()
        e.outputFormatting = []
        return e
    }()

    static func encodeLine<T: Encodable>(_ value: T) -> String? {
        guard let data = try? encoder.encode(value) else { return nil }
        return String(data: data, encoding: .utf8)
    }
}
