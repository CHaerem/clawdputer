// JSON wire types for the bridge protocol. Mirrors protocol/WIRE.md.
//
// Encoded as one JSON object per line; the BLECentral handles framing.

import Foundation

enum Wire {
    struct Hello: Encodable {
        let cmd: String   = "hello"
        let bridge: String = "clawd-bridge"
        let ver: Int      = 1
    }

    struct ChatChunk: Encodable {
        let evt: String = "chat.chunk"
        let text: String
    }

    struct ChatStatus: Encodable {
        let evt: String = "chat.status"
        let text: String
    }

    struct ChatEnd: Encodable {
        let evt: String   = "chat.end"
        let tokens: Int?
    }

    struct WireError: Encodable {
        let evt: String  = "error"
        let `where`: String
        let msg: String
    }

    // Loose envelope for incoming lines — we only inspect a few fields.
    struct Inbound: Decodable {
        let evt: String?
        let cmd: String?
        let text: String?
        let app: String?
    }

    static let encoder = JSONEncoder()
    static let decoder = JSONDecoder()

    static func encodeLine<T: Encodable>(_ value: T) -> String? {
        guard let data = try? encoder.encode(value) else { return nil }
        return String(data: data, encoding: .utf8)
    }

    static func decodeInbound(_ line: String) -> Inbound? {
        guard let data = line.data(using: .utf8) else { return nil }
        return try? decoder.decode(Inbound.self, from: data)
    }
}
