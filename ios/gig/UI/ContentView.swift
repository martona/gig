//
//  ContentView.swift
//  gig
//
//  The iOS shell: a native SwiftUI toolbar (status + Settings/Reconnect/Log,
//  the desktop imgui toolbar's replacement), the Metal video surface (the
//  shared C++ scene: live grid, signal scope, tap-to-zoom), and the camera
//  labels as SwiftUI overlays -- positioned from the scene's tile layout and
//  hidden while the zoom animation runs (they can't track the in-scene
//  transition). Settings and the log ring are sheets.
//

import SwiftUI

@MainActor
final class EngineModel: ObservableObject {
    @Published var connecting = false
    @Published var connected = false
    @Published var live = 0
    @Published var total = 0
    @Published var detail = ""   // failure reason ("" when healthy)

    func connect() {
        guard !connecting else { return }
        connecting = true
        detail = ""
        Task.detached(priority: .userInitiated) {
            let status = Engine.shared().connect()
            await MainActor.run { self.apply(status, fromConnect: true) }
        }
    }

    func refresh() {
        guard !connecting else { return }
        apply(Engine.shared().status(), fromConnect: false)
    }

    private func apply(_ status: EngineStatus, fromConnect: Bool) {
        connecting = false
        connected = status.connected
        live = status.liveCameraCount
        total = status.cameraCount
        if fromConnect && !status.connected && status.message != "ok" && status.message != "working" {
            detail = status.message
        } else if status.connected {
            detail = ""
        }
    }
}

@MainActor
final class OverlayModel: ObservableObject {
    struct Label: Identifiable {
        let id: Int
        let text: String
        let rect: CGRect
    }

    @Published var labels: [Label] = []
    @Published var zoomed = false

    init() {
        VideoHost.shared().onOverlayChanged = { [weak self] in
            // The display link runs on the main thread; hop anyway so this stays
            // correct if that ever changes.
            DispatchQueue.main.async { self?.reload() }
        }
        reload()
    }

    private func reload() {
        let host = VideoHost.shared()
        labels = host.visibleLabels().map {
            Label(id: $0.index, text: $0.text, rect: $0.rect)
        }
        zoomed = host.zoomed
    }
}

struct ContentView: View {
    @StateObject private var engine = EngineModel()
    @StateObject private var overlay = OverlayModel()
    @Environment(\.scenePhase) private var scenePhase

    @State private var showSettings = false
    @State private var showLog = false
    @State private var didAutoStart = false

    private let ticker = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(spacing: 0) {
            toolbar
            if !engine.connected && !engine.detail.isEmpty {
                banner
            }
            videoArea
        }
        .background(Color(white: 0.03))
        .sheet(isPresented: $showSettings) {
            SettingsView { engine.connect() }
        }
        .sheet(isPresented: $showLog) {
            LogView()
        }
        .onAppear(perform: autoStart)
        .onReceive(ticker) { _ in engine.refresh() }
        .onChange(of: scenePhase) { phase in
            // Render only while active; the session itself stays up (iOS suspends
            // the process shortly after backgrounding anyway; the health poll
            // self-heals on return). A deliberate pause-on-background policy for
            // the decoders is a follow-up.
            switch phase {
            case .active: VideoHost.shared().start()
            default: VideoHost.shared().stop()
            }
        }
    }

    // MARK: toolbar (native replacement for the desktop imgui toolbar)

    private var toolbar: some View {
        HStack(spacing: 14) {
            Text("gig")
                .font(.headline)
            statusText
                .font(.footnote.monospacedDigit())
            Spacer()
            Button {
                showSettings = true
            } label: {
                Image(systemName: "gearshape")
            }
            .accessibilityLabel("Settings")
            Button {
                engine.connect()
            } label: {
                Image(systemName: "arrow.clockwise")
            }
            .disabled(engine.connecting)
            .accessibilityLabel("Reconnect")
            Button {
                showLog = true
            } label: {
                Image(systemName: "list.bullet")
            }
            .accessibilityLabel("Log")
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(.bar)
    }

    @ViewBuilder
    private var statusText: some View {
        if engine.connecting {
            HStack(spacing: 6) {
                ProgressView().controlSize(.small)
                Text("connecting")
                    .foregroundStyle(.secondary)
            }
        } else if engine.connected {
            Text("\(engine.live)/\(engine.total) live")
                .foregroundStyle(engine.total > 0 && engine.live == engine.total
                    ? .green : (engine.live == 0 ? .red : .orange))
        } else {
            Text("disconnected")
                .foregroundStyle(.red)
        }
    }

    private var banner: some View {
        Text(engine.detail)
            .font(.caption)
            .lineLimit(2)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 14)
            .padding(.vertical, 6)
            .background(Color(red: 0.42, green: 0.10, blue: 0.10))
            .foregroundStyle(Color(red: 1.0, green: 0.95, blue: 0.92))
    }

    // MARK: video + label overlay

    private var videoArea: some View {
        ZStack(alignment: .topLeading) {
            VideoSurfaceView()
            ForEach(overlay.labels) { label in
                Text(label.text)
                    .font(.caption.monospaced())
                    .foregroundStyle(Color(red: 0.90, green: 0.95, blue: 1.0))
                    .padding(.horizontal, 6)
                    .padding(.vertical, 3)
                    .background(.black.opacity(0.55), in: RoundedRectangle(cornerRadius: 4))
                    .offset(x: label.rect.minX + 4, y: label.rect.minY + 4)
                    .allowsHitTesting(false)
            }
            if !engine.connected && !engine.connecting {
                VStack(spacing: 8) {
                    Image(systemName: "video.slash")
                        .font(.largeTitle)
                    Text("Not connected")
                }
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .allowsHitTesting(false)
            }
        }
        .ignoresSafeArea(edges: .bottom)
    }

    // MARK: startup

    private func autoStart() {
        guard !didAutoStart else { return }
        didAutoStart = true
        // Desktop parity: connect at startup when configured; first run (no base
        // URL yet) opens settings instead of failing.
        if SettingsBridge.current().baseURL.isEmpty {
            showSettings = true
        } else {
            engine.connect()
        }
    }
}

#Preview {
    ContentView()
}
