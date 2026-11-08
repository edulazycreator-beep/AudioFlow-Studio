// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "AudioFlow",
    platforms: [.macOS(.v13)],
    products: [
        .library(name: "AudioFlowKit", targets: ["AudioFlowKit"]),
        .executable(name: "afctl", targets: ["afctl"]),
    ],
    targets: [
        .target(name: "AudioFlowKit", path: "Sources/AudioFlowKit"),
        .executableTarget(name: "afctl", dependencies: ["AudioFlowKit"], path: "Sources/afctl"),
        .testTarget(name: "AudioFlowKitTests", dependencies: ["AudioFlowKit"], path: "Tests/AudioFlowKitTests"),
    ]
)
