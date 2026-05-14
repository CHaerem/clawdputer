// Parses the NDJSON event stream emitted by `claude -p --output-format
// stream-json --include-partial-messages`. We only care about three things:
// incremental text deltas, tool-use announcements, and the final result
// with token usage.

import Foundation

final class StreamJsonParser {
    var onText:    ((String) -> Void)?
    var onTool:    ((String) -> Void)?
    var onResult:  ((Int?) -> Void)?  // total output tokens, if reported

    private var buffer = Data()

    func feed(_ data: Data) {
        buffer.append(data)
        while let nl = buffer.firstIndex(of: 0x0A) {
            let lineData = buffer.subdata(in: buffer.startIndex..<nl)
            buffer.removeSubrange(buffer.startIndex...nl)
            parse(lineData)
        }
    }

    private func parse(_ data: Data) {
        guard !data.isEmpty,
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return }

        switch obj["type"] as? String {
        case "stream_event":
            handleStreamEvent(obj["event"] as? [String: Any] ?? [:])
        case "assistant":
            // Whole-message form (no --include-partial-messages). Pull tool
            // uses for status display; text is already covered by deltas
            // when streaming is on.
            if let msg     = obj["message"] as? [String: Any],
               let content = msg["content"] as? [[String: Any]] {
                for block in content where block["type"] as? String == "tool_use" {
                    if let name = block["name"] as? String { onTool?(name) }
                }
            }
        case "result":
            let usage = obj["usage"] as? [String: Any]
            let tokens = usage?["output_tokens"] as? Int
            onResult?(tokens)
        default:
            break
        }
    }

    private func handleStreamEvent(_ event: [String: Any]) {
        switch event["type"] as? String {
        case "content_block_delta":
            if let delta = event["delta"] as? [String: Any],
               delta["type"] as? String == "text_delta",
               let text = delta["text"] as? String {
                onText?(text)
            }
        case "content_block_start":
            if let block = event["content_block"] as? [String: Any],
               block["type"] as? String == "tool_use",
               let name = block["name"] as? String {
                onTool?(name)
            }
        default:
            break
        }
    }
}
