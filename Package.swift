// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "PingBar",
    platforms: [
        .macOS(.v14),
    ],
    products: [
        .executable(
            name: "PingBar",
            targets: ["PingBarApp"]
        ),
        .library(
            name: "CPing",
            targets: ["CPing"]
        ),
    ],
    targets: [
        .executableTarget(
            name: "PingBarApp",
            dependencies: ["CPing"],
            path: "Sources/PingBarApp"
        ),
        .target(
            name: "CPing",
            publicHeadersPath: "include",
            linkerSettings: [
                .linkedLibrary("resolv"),
            ]
        ),
    ]
)
