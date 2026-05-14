// Owns a `claude` CLI process. Each call to `send(_:)` spawns claude in
// print mode, feeds the prompt on stdin, and streams stdout chunks to the
// caller. After the first turn, subsequent spawns pass `--continue` so the
// conversation context survives across prompts.
//
// One spawn per turn keeps the failure modes simple — if the CLI crashes,
// the next turn starts a fresh process. A long-running interactive mode
// could be substituted later for lower per-turn latency.

import Foundation

final class ClaudeSession {
    enum Status { case idle, busy }

    private(set) var status: Status = .idle
    private(set) var hasSession     = false

    let cwd: URL

    var onChunk: ((String) -> Void)?
    var onEnd:   ((Int?) -> Void)?
    var onError: ((String) -> Void)?

    private var stderrBuf = Data()

    init(cwd: URL = URL(fileURLWithPath: NSHomeDirectory())) {
        self.cwd = cwd
    }

    func send(_ prompt: String) {
        guard status == .idle else {
            onError?("session busy — drop the prompt")
            return
        }
        status = .busy
        stderrBuf.removeAll()

        let process    = Process()
        process.executableURL          = URL(fileURLWithPath: "/usr/bin/env")
        var args: [String]             = ["claude", "--print"]
        if hasSession { args.append("--continue") }
        process.arguments              = args
        process.currentDirectoryURL    = cwd

        let stdin  = Pipe()
        let stdout = Pipe()
        let stderr = Pipe()
        process.standardInput  = stdin
        process.standardOutput = stdout
        process.standardError  = stderr

        stdout.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else { return }
            if let s = String(data: data, encoding: .utf8) {
                DispatchQueue.main.async { self?.onChunk?(s) }
            }
        }
        stderr.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else { return }
            DispatchQueue.main.async { self?.stderrBuf.append(data) }
        }

        process.terminationHandler = { [weak self] proc in
            DispatchQueue.main.async {
                guard let self = self else { return }
                stdout.fileHandleForReading.readabilityHandler = nil
                stderr.fileHandleForReading.readabilityHandler = nil
                self.status = .idle
                if proc.terminationStatus == 0 {
                    self.hasSession = true
                    self.onEnd?(nil)
                } else {
                    let msg = String(data: self.stderrBuf, encoding: .utf8)?
                        .trimmingCharacters(in: .whitespacesAndNewlines)
                        ?? "claude exited \(proc.terminationStatus)"
                    self.onError?(msg.isEmpty ? "claude exited \(proc.terminationStatus)" : msg)
                    self.onEnd?(nil)
                }
            }
        }

        do {
            try process.run()
            let promptData = (prompt + "\n").data(using: .utf8) ?? Data()
            try stdin.fileHandleForWriting.write(contentsOf: promptData)
            try stdin.fileHandleForWriting.close()
        } catch {
            status = .idle
            onError?("failed to start claude: \(error.localizedDescription)")
            onEnd?(nil)
        }
    }
}
