import AppKit
import CPing
import Foundation
import SwiftUI

@MainActor
final class PingMonitor: ObservableObject {
    struct LabelStyle {
        let text: String
        let foreground: NSColor
        let pillBackground: NSColor
    }

    private struct ProbeSample {
        let timestamp: Date
        let latencyMs: Int?
    }

    @Published var latencyMs: Int?
    @Published var ipAddress = "-"
    @Published var dnsServer = "-"
    @Published var packetLossText = "-"
    @Published var jitterText = "-"

    private var timer: Timer?
    private var probeInFlight = false
    private var publicIPLastRefresh = Date.distantPast
    private var probeSamples: [ProbeSample] = []

    init() {
        startTimer()
        tick()
    }

    private func startTimer() {
        // Drive a single probe every second.
        timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.tick()
            }
        }
    }

    private func tick() {
        guard !probeInFlight else {
            return
        }

        probeInFlight = true
        let shouldRefreshPublicIP = Date().timeIntervalSince(publicIPLastRefresh) >= 30 || ipAddress == "-"
        let cachedPublicIP = ipAddress

        // Run probe and network reads off-main so the menu stays responsive.
        Task { [weak self] in
            let snapshot = await Self.measureSnapshot(
                refreshPublicIP: shouldRefreshPublicIP,
                cachedPublicIP: cachedPublicIP
            )
            guard let self else { return }
            latencyMs = snapshot.latency
            ipAddress = snapshot.ipAddress
            dnsServer = snapshot.dnsServer
            recordProbe(latencyMs: snapshot.latency)
            if snapshot.didRefreshPublicIP {
                publicIPLastRefresh = Date()
            }
            probeInFlight = false
        }
    }

    private struct ProbeSnapshot {
        let latency: Int?
        let ipAddress: String
        let dnsServer: String
        let didRefreshPublicIP: Bool
    }

    private static func measureSnapshot(refreshPublicIP: Bool, cachedPublicIP: String) async -> ProbeSnapshot {
        await Task.detached(priority: .utility) {
            // Probe 1.1.1.1 with a 1s receive timeout via raw ICMP socket helper.
            let milliseconds = cp_ping_once_ms("1.1.1.1", 1000)
            let latency = milliseconds >= 0 ? Int(milliseconds.rounded()) : nil

            // Refresh public IPv4 on a slower cadence to avoid hammering the probe endpoint.
            var ipAddress = cachedPublicIP
            var didRefreshPublicIP = false
            if refreshPublicIP {
                didRefreshPublicIP = true
                var publicIPBuffer = [CChar](repeating: 0, count: 64)
                let publicIPResult = publicIPBuffer.withUnsafeMutableBufferPointer { ptr in
                    cp_public_ipv4(ptr.baseAddress, ptr.count, 1200)
                }
                if publicIPResult == 0 {
                    ipAddress = decodeCStringBuffer(publicIPBuffer)
                }
            }

            // Resolve primary DNS using the C resolver helper.
            var dnsBuffer = [CChar](repeating: 0, count: 64)
            let dnsResult = dnsBuffer.withUnsafeMutableBufferPointer { ptr in
                cp_primary_dns_ipv4(ptr.baseAddress, ptr.count)
            }
            let dnsServer = dnsResult == 0 ? decodeCStringBuffer(dnsBuffer) : "-"

            return ProbeSnapshot(
                latency: latency,
                ipAddress: ipAddress,
                dnsServer: dnsServer,
                didRefreshPublicIP: didRefreshPublicIP
            )
        }.value
    }

    nonisolated private static func decodeCStringBuffer(_ buffer: [CChar]) -> String {
        // Convert a NUL-terminated C buffer into a Swift string safely.
        let bytes = buffer.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }
        return String(decoding: bytes, as: UTF8.self)
    }

    private func recordProbe(latencyMs: Int?) {
        // Keep a rolling 60-second probe window for loss/jitter summaries.
        let now = Date()
        probeSamples.append(ProbeSample(timestamp: now, latencyMs: latencyMs))
        let cutoff = now.addingTimeInterval(-60)
        probeSamples.removeAll { $0.timestamp < cutoff }
        updateRollingStats()
    }

    private func updateRollingStats() {
        guard !probeSamples.isEmpty else {
            packetLossText = "-"
            jitterText = "-"
            return
        }

        // Packet loss is failed probes over all probes in the 60-second window.
        let total = probeSamples.count
        let failed = probeSamples.filter { $0.latencyMs == nil }.count
        let lossPercent = (Double(failed) * 100.0) / Double(total)
        packetLossText = String(format: "%.1f%%", lossPercent)

        // Jitter is mean absolute delta between consecutive successful RTT samples.
        let successful = probeSamples.compactMap(\.latencyMs)
        guard successful.count >= 2 else {
            jitterText = "-"
            return
        }
        var deltaSum = 0
        for idx in 1..<successful.count {
            deltaSum += abs(successful[idx] - successful[idx - 1])
        }
        let jitter = Double(deltaSum) / Double(successful.count - 1)
        jitterText = String(format: "%.1f ms", jitter)
    }

    var labelStyle: LabelStyle {
        guard let latencyMs else {
            return LabelStyle(
                text: "∞",
                foreground: .systemRed,
                pillBackground: NSColor.systemRed.withAlphaComponent(0.22)
            )
        }

        if latencyMs > 100 {
            return LabelStyle(
                text: "\(latencyMs)ms",
                foreground: .white,
                pillBackground: .systemRed
            )
        }

        if latencyMs > 50 {
            return LabelStyle(
                text: "\(latencyMs)ms",
                foreground: .white,
                pillBackground: .systemOrange
            )
        }

        return LabelStyle(
            text: "\(latencyMs)ms",
            foreground: .white,
            pillBackground: .clear
        )
    }

    var labelImage: NSImage {
        Self.makeLabelImage(for: labelStyle)
    }

    nonisolated private static func makeLabelImage(for style: LabelStyle) -> NSImage {
        // Keep the label one point smaller than the earlier design.
        let font = NSFont.monospacedDigitSystemFont(ofSize: 11, weight: .semibold)
        let textAttributes: [NSAttributedString.Key: Any] = [
            .font: font,
            .foregroundColor: style.foreground,
        ]

        // Measure text and keep constant capsule insets across states.
        let textSize = (style.text as NSString).size(withAttributes: textAttributes)
        let horizontalPadding: CGFloat = 8
        let verticalPadding: CGFloat = 2
        let imageWidth = ceil(textSize.width + (horizontalPadding * 2))
        let imageHeight = ceil(max(textSize.height + (verticalPadding * 2), 16))
        let imageSize = NSSize(width: imageWidth, height: imageHeight)

        // Render both background and text into a non-template bitmap image.
        let image = NSImage(size: imageSize)
        image.lockFocus()

        let pillRect = NSRect(origin: .zero, size: imageSize)
        let capsule = NSBezierPath(
            roundedRect: pillRect,
            xRadius: imageHeight / 2,
            yRadius: imageHeight / 2
        )
        style.pillBackground.setFill()
        capsule.fill()

        let textOrigin = NSPoint(
            x: floor((imageWidth - textSize.width) / 2),
            y: floor((imageHeight - textSize.height) / 2)
        )
        (style.text as NSString).draw(at: textOrigin, withAttributes: textAttributes)

        image.unlockFocus()
        image.isTemplate = false
        return image
    }
}

@main
struct PingBarApp: App {
    @StateObject private var pingMonitor = PingMonitor()

    init() {
        // Keep the app in the menu bar only (hide Dock presence).
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    var body: some Scene {
        // Show live latency directly in the menu bar label.
        MenuBarExtra {
            // Show currently observed public IP and active DNS resolver.
            Text("IP: \(pingMonitor.ipAddress)")
            Text("DNS: \(pingMonitor.dnsServer)")
            Text("Loss (60s): \(pingMonitor.packetLossText)")
            Text("Jitter (60s): \(pingMonitor.jitterText)")

            Divider()

            // Exit the menu bar app immediately.
            Button("Quit") {
                NSApplication.shared.terminate(nil)
            }
        } label: {
            // Use a rendered image so color + pill styling survives menu-bar label templating.
            Image(nsImage: pingMonitor.labelImage)
                .renderingMode(.original)
        }
    }
}
