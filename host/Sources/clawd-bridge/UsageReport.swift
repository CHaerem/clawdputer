// Pulls together a Claude usage snapshot from local files / CLI output.
//
//  - stats-cache.json under $HOME/.claude/ tracks daily messageCount,
//    sessionCount, and toolCallCount.
//  - `claude --print --output-format json "/cost"` returns the current
//    cost + token totals (and reports "subscription" tier when the user
//    is on a plan rather than direct API billing).

import Foundation

struct UsageReport: Encodable {
    struct Bucket: Encodable {
        let messages: Int
        let sessions: Int
        let tools:    Int
    }
    struct Cost: Encodable {
        let usd:  Double
        let tier: String
    }
    struct Tokens: Encodable {
        let input:       Int
        let output:      Int
        let cacheRead:   Int
        let cacheCreate: Int
    }

    let evt:   String = "usage.response"
    let today: Bucket
    let week:  Bucket
    let month: Bucket
    let cost:  Cost
    let tokens: Tokens
    let asOf:  String
}

enum Usage {
    static func collect(completion: @escaping (UsageReport) -> Void) {
        let stats = readStatsCache()
        runCost { cost, tokens, tier in
            let today = stats.today
            let week  = stats.window(days: 7)
            let month = stats.window(days: 30)
            let report = UsageReport(
                today:  today,
                week:   week,
                month:  month,
                cost:   .init(usd: cost, tier: tier),
                tokens: tokens,
                asOf:   ISO8601DateFormatter.dateOnly.string(from: Date())
            )
            completion(report)
        }
    }

    // -------- stats-cache.json --------

    struct StatsCache {
        struct Day: Decodable {
            let date: String
            let messageCount: Int
            let sessionCount: Int
            let toolCallCount: Int?
        }
        let days: [Day]

        var today: UsageReport.Bucket {
            let key = ISO8601DateFormatter.dateOnly.string(from: Date())
            if let d = days.first(where: { $0.date == key }) {
                return .init(messages: d.messageCount,
                             sessions: d.sessionCount,
                             tools:    d.toolCallCount ?? 0)
            }
            return .init(messages: 0, sessions: 0, tools: 0)
        }

        func window(days nDays: Int) -> UsageReport.Bucket {
            let cutoff = Calendar.current.date(byAdding: .day, value: -nDays, to: Date())!
            var m = 0, s = 0, t = 0
            for d in days {
                guard let date = ISO8601DateFormatter.dateOnly.date(from: d.date),
                      date >= cutoff else { continue }
                m += d.messageCount
                s += d.sessionCount
                t += d.toolCallCount ?? 0
            }
            return .init(messages: m, sessions: s, tools: t)
        }
    }

    private static func readStatsCache() -> StatsCache {
        let path = (NSHomeDirectory() as NSString).appendingPathComponent(".claude/stats-cache.json")
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: path)) else {
            return StatsCache(days: [])
        }
        struct Top: Decodable { let dailyActivity: [StatsCache.Day]? }
        let top = try? JSONDecoder().decode(Top.self, from: data)
        return StatsCache(days: top?.dailyActivity ?? [])
    }

    // -------- claude --print "/cost" --------

    private static func runCost(completion: @escaping (Double, UsageReport.Tokens, String) -> Void) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        process.arguments = ["claude", "--print", "--output-format", "json", "/cost"]

        let stdout = Pipe()
        let stderr = Pipe()
        process.standardOutput = stdout
        process.standardError  = stderr

        process.terminationHandler = { _ in
            let data = stdout.fileHandleForReading.readDataToEndOfFile()
            var usd  = 0.0
            var tier = "unknown"
            var tokens = UsageReport.Tokens(input: 0, output: 0, cacheRead: 0, cacheCreate: 0)
            if let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                usd  = obj["total_cost_usd"] as? Double ?? 0
                if let result = obj["result"] as? String,
                   result.lowercased().contains("subscription") {
                    tier = "subscription"
                }
                if let u = obj["usage"] as? [String: Any] {
                    if let t = u["service_tier"] as? String, tier == "unknown" { tier = t }
                    tokens = UsageReport.Tokens(
                        input:       u["input_tokens"]                as? Int ?? 0,
                        output:      u["output_tokens"]               as? Int ?? 0,
                        cacheRead:   u["cache_read_input_tokens"]     as? Int ?? 0,
                        cacheCreate: u["cache_creation_input_tokens"] as? Int ?? 0
                    )
                }
            }
            DispatchQueue.main.async { completion(usd, tokens, tier) }
        }

        do { try process.run() } catch {
            completion(0, UsageReport.Tokens(input: 0, output: 0, cacheRead: 0, cacheCreate: 0), "error")
        }
    }
}

private extension ISO8601DateFormatter {
    static let dateOnly: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd"
        f.timeZone   = .current
        return f
    }()
}
