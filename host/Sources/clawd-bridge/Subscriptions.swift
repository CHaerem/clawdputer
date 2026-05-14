// Tracks which channels the connected device is subscribed to, and runs
// the file-system watcher that drives the `usage` channel. Watching is
// debounced — file editors typically rewrite stats-cache.json multiple
// times in close succession, and we'd rather collect once than spawn
// `claude /cost` three times in a row.

import Foundation

final class Subscriptions {
    enum Channel: String {
        case usage
    }

    private(set) var active: Set<Channel> = []
    private var watcher: FileWatcher?
    private var debounce: DispatchWorkItem?

    var emit: ((UsageReport) -> Void)?

    func subscribe(_ name: String) {
        guard let channel = Channel(rawValue: name) else {
            fputs("[sub] unknown channel: \(name)\n", stderr)
            return
        }
        if active.contains(channel) { return }
        active.insert(channel)
        print("[sub] subscribed to \(channel.rawValue)")

        switch channel {
        case .usage:
            startUsageWatcher()
        }
    }

    func unsubscribe(_ name: String) {
        guard let channel = Channel(rawValue: name) else { return }
        active.remove(channel)
        print("[sub] unsubscribed from \(channel.rawValue)")
        if channel == .usage && !active.contains(.usage) {
            watcher?.cancel()
            watcher = nil
        }
    }

    func clearAll() {
        active.removeAll()
        watcher?.cancel()
        watcher = nil
        debounce?.cancel()
        debounce = nil
    }

    private func startUsageWatcher() {
        let path = (NSHomeDirectory() as NSString).appendingPathComponent(".claude/stats-cache.json")
        watcher = FileWatcher(path: path) { [weak self] in
            self?.usageChanged()
        }
    }

    private func usageChanged() {
        debounce?.cancel()
        let work = DispatchWorkItem { [weak self] in
            Usage.collect { [weak self] report in
                guard let self = self, self.active.contains(.usage) else { return }
                self.emit?(report)
            }
        }
        debounce = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0, execute: work)
    }
}

// Minimal DispatchSource-based file watcher. Re-arms automatically on
// rename/replace (editors often write to a temp file and rename), so a
// single call to start() survives common edit patterns.
final class FileWatcher {
    private let path: String
    private let handler: () -> Void
    private var source: DispatchSourceFileSystemObject?
    private var fd: Int32 = -1

    init(path: String, handler: @escaping () -> Void) {
        self.path = path
        self.handler = handler
        start()
    }

    deinit { cancel() }

    func cancel() {
        source?.cancel()
        source = nil
        if fd >= 0 { close(fd); fd = -1 }
    }

    private func start() {
        fd = open(path, O_EVTONLY)
        guard fd >= 0 else {
            fputs("[watch] open failed for \(path)\n", stderr)
            // Try again in 5 seconds — file may not exist yet.
            DispatchQueue.main.asyncAfter(deadline: .now() + 5) { [weak self] in
                self?.start()
            }
            return
        }
        let src = DispatchSource.makeFileSystemObjectSource(
            fileDescriptor: fd,
            eventMask: [.write, .extend, .delete, .rename],
            queue: .main
        )
        src.setEventHandler { [weak self] in
            guard let self = self else { return }
            let events = src.data
            self.handler()
            if events.contains(.delete) || events.contains(.rename) {
                // File replaced — close and re-open against the new inode.
                self.cancel()
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
                    self.start()
                }
            }
        }
        src.resume()
        source = src
        print("[watch] watching \(path)")
    }
}
