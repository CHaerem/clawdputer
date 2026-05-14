// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "clawd-bridge",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(
            name: "clawd-bridge",
            path: "Sources/clawd-bridge"
        )
    ]
)
