//
//  ContentView.swift
//  gig
//
//  The iOS shell: a native SwiftUI toolbar (status + Settings/Reconnect/Log),
//  the Metal video surface (the shared C++ scene: live grid, signal scope,
//  tap-to-zoom), camera labels as SwiftUI overlays, and the full-screen
//  onboarding/status states (OnboardingViews.swift): first-run welcome ->
//  Local Network permission step -> settings; connection failures land on the
//  error screen with room for the reason and one-tap fixes -- including the
//  Local-Network-denied case, which raw sockets can't distinguish from an
//  unreachable host but the Bonjour probe can.
//
//  Lifecycle policy: on .background the session is torn down cleanly (iOS
//  suspends the process anyway) and reconnects automatically on return; the
//  screen stays awake while connected and active (it's a camera wall).
//

import SwiftUI
import UIKit

@MainActor
final class EngineModel: ObservableObject {
    @Published var connecting = false
    @Published var connected = false
    @Published var live = 0
    @Published var total = 0
    @Published var detail = ""            // failure reason ("" when healthy)
    @Published var pendingPin: PendingPin? // untrusted cert awaiting a decision

    private var resumeOnActive = false

    // Engine lifecycle operations (connect / disconnect / pin decisions) run off
    // the main thread AND strictly in submission order: each task awaits its
    // predecessor. Without this, a background-disconnect and a foreground-
    // reconnect race on the engine mutex (std::mutex is unfair), and the
    // disconnect can tear down the session the reconnect just built.
    private var lifecycleTask: Task<Void, Never>?
    // Bumped by every state-changing operation; a finishing connect whose
    // generation is stale (superseded by a background or a newer attempt) must
    // not overwrite the UI state.
    private var generation = 0

    private func runExclusive(_ op: @escaping @Sendable () async -> Void) {
        let previous = lifecycleTask
        lifecycleTask = Task.detached(priority: .userInitiated) {
            await previous?.value
            await op()
        }
    }

    func connect() {
        guard !connecting else { return }
        connecting = true
        detail = ""
        generation += 1
        let gen = generation
        runExclusive {
            let status = Engine.shared().connect()
            await MainActor.run { self.finishConnect(status, generation: gen) }
        }
    }

    // A deliberate user retry (Reconnect button / Try Again) is a fresh trust
    // decision: a previously declined certificate may prompt again.
    func retry() {
        guard !connecting else { return }
        connecting = true
        detail = ""
        generation += 1
        let gen = generation
        runExclusive {
            Engine.shared().resetDeclinedPins()
            let status = Engine.shared().connect()
            await MainActor.run { self.finishConnect(status, generation: gen) }
        }
    }

    // Settings were just saved: reconnect with the new config, SUPERSEDING any
    // in-flight attempt (no !connecting guard — the guarded connect() would
    // silently drop a save made while a slow connect runs; the generation bump
    // invalidates the old attempt's result and runExclusive keeps order). Also
    // desktop parity: a save-and-reconnect clears session pin declines.
    func applySettingsAndReconnect() {
        connecting = true
        detail = ""
        generation += 1
        let gen = generation
        runExclusive {
            Engine.shared().resetDeclinedPins()
            let status = Engine.shared().connect()
            await MainActor.run { self.finishConnect(status, generation: gen) }
        }
    }

    func refresh() {
        guard !connecting else { return }
        apply(Engine.shared().status(), fromConnect: false)
        checkPendingPin() // a cert can change mid-session (pinned cert rotated)
    }

    // MARK: certificate pin decision
    // The pin identity comes from the alert's presented value, NOT from
    // pendingPin: SwiftUI's isPresented write and the button action have no
    // documented ordering, so the action must not depend on live state.

    func acceptPendingPin(_ pin: PendingPin) {
        pendingPin = nil
        connecting = true
        detail = ""
        generation += 1
        let gen = generation
        runExclusive {
            let status = Engine.shared().acceptPendingPin(host: pin.host, fingerprint: pin.fingerprint)
            await MainActor.run { self.finishConnect(status, generation: gen) }
        }
    }

    func declinePendingPin(_ pin: PendingPin) {
        pendingPin = nil
        runExclusive {
            Engine.shared().declinePendingPin(host: pin.host, fingerprint: pin.fingerprint)
        }
    }

    // MARK: scene-phase policy (background -> clean disconnect, resume on return)

    func enterBackground() {
        guard connected || connecting else { return }
        resumeOnActive = true
        generation += 1 // invalidate any in-flight connect's result
        connecting = false
        connected = false
        live = 0
        total = 0
        runExclusive {
            Engine.shared().disconnect()
        }
    }

    func becomeActive() {
        if resumeOnActive {
            resumeOnActive = false
            connect() // queued behind the background disconnect by runExclusive
        }
    }

    // TODO(onboarding-project): temporary, pairs with Forget Settings.
    func resetAfterForget() {
        generation += 1
        resumeOnActive = false
        connecting = false
        connected = false
        live = 0
        total = 0
        detail = ""
        pendingPin = nil
        runExclusive {
            // Full runtime wipe (session + auth cookie + TLS tickets + pin
            // session state), not just a disconnect — nothing from the
            // forgotten identity may leak into the fresh onboarding.
            Engine.shared().forgetRuntimeState()
        }
    }

    // MARK: internals

    private func finishConnect(_ status: EngineStatus, generation gen: Int) {
        guard gen == generation else { return } // superseded; don't touch UI state
        apply(status, fromConnect: true)
        checkPendingPin()
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
        configError = status.configError && !status.connected
    }

    @Published var configError = false // last failure was structural (Settings is the fix)

    private func checkPendingPin() {
        guard pendingPin == nil else { return }
        pendingPin = Engine.shared().takePendingPin()
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
    // The first-run corridor: welcome -> permission step -> (settings sheet).
    // After that everything is stateless: config-empty shows welcome again,
    // failures show the error screen, a session shows video.
    private enum OnboardPhase { case welcome, permission, ready }

    @StateObject private var engine = EngineModel()
    @StateObject private var overlay = OverlayModel()
    @StateObject private var probe = LocalNetworkProbe()
    @Environment(\.scenePhase) private var scenePhase

    @State private var phase: OnboardPhase = .ready
    @State private var showSettings = false
    @State private var showLog = false
    @State private var didAutoStart = false

    private let ticker = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(spacing: 0) {
            toolbar
            videoArea
        }
        .background(Color(white: 0.03))
        .sheet(isPresented: $showSettings) {
            SettingsView(onSave: {
                if !SettingsBridge.current().baseURL.isEmpty {
                    engine.applySettingsAndReconnect()
                }
            }, onForget: {
                // TODO(onboarding-project): temporary. Store already wiped by the
                // sheet; reset the engine and restart first-run onboarding.
                engine.resetAfterForget()
                phase = .welcome
            })
        }
        .sheet(isPresented: $showLog) {
            LogView()
        }
        .alert(
            engine.pendingPin?.changed == true ? "PINNED CERTIFICATE CHANGED" : "Untrusted certificate",
            isPresented: Binding(
                get: { engine.pendingPin != nil },
                // Pure presentation state -- NO decision side effect here. SwiftUI
                // may write false BEFORE the tapped button's action runs, so a
                // decline here would override a Trust & Pin tap. On iOS an alert
                // is only dismissible via its buttons, which carry the decision.
                set: { if !$0 { engine.pendingPin = nil } }
            ),
            presenting: engine.pendingPin
        ) { pin in
            Button("Trust & Pin", role: .destructive) { engine.acceptPendingPin(pin) }
            Button("Don't Trust", role: .cancel) { engine.declinePendingPin(pin) }
        } message: { pin in
            Text(pinMessage(pin))
        }
        .onAppear(perform: autoStart)
        .onReceive(ticker) { _ in engine.refresh() }
        .onChange(of: engine.connected) { _ in updateIdleTimer() }
        .onChange(of: engine.connecting) { connecting in
            // Entering an error state: re-probe so a Local-Network denial shows
            // as itself instead of a generic "unreachable host".
            if !connecting && !engine.connected && phase == .ready && !engine.detail.isEmpty {
                probe.start()
            }
        }
        .onChange(of: probe.outcome) { outcome in
            // Permission step auto-advances on grant (or no-signal, e.g. simulator).
            guard phase == .permission, let outcome, outcome != .denied else { return }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) {
                if phase == .permission {
                    phase = .ready
                    showSettings = true
                }
            }
        }
        .onChange(of: scenePhase) { newPhase in
            switch newPhase {
            case .active:
                VideoHost.shared().start()
                engine.becomeActive()
            case .background:
                VideoHost.shared().stop()
                engine.enterBackground()
            default: // .inactive: app switcher / Control Center -- just pause rendering
                VideoHost.shared().stop()
            }
            updateIdleTimer()
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
                engine.retry()
            } label: {
                Image(systemName: "arrow.clockwise")
            }
            .disabled(engine.connecting || phase != .ready)
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
        if phase != .ready {
            Text("not configured")
                .foregroundStyle(.secondary)
        } else if engine.connecting {
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

    // MARK: video + overlays + full-screen states

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
            statusScreen
        }
        .ignoresSafeArea(edges: .bottom)
    }

    @ViewBuilder
    private var statusScreen: some View {
        switch phase {
        case .welcome:
            WelcomeView { phase = .permission }
        case .permission:
            PermissionStepView(probe: probe) {
                phase = .ready
                showSettings = true
            }
        case .ready:
            if engine.connecting {
                VStack(spacing: 10) {
                    ProgressView()
                    Text("Connecting…")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .allowsHitTesting(false)
            } else if !engine.connected {
                if SettingsBridge.current().baseURL.isEmpty {
                    // Config was emptied (e.g. saved blank): back to welcome, statelessly.
                    WelcomeView { showSettings = true }
                } else {
                    ErrorStateView(
                        detail: engine.detail,
                        configError: engine.configError,
                        localNetworkDenied: probe.outcome == .denied,
                        onRetry: { engine.retry() },
                        onOpenSettings: { showSettings = true },
                        onViewLog: { showLog = true }
                    )
                }
            }
        }
    }

    // MARK: helpers

    private func pinMessage(_ pin: PendingPin) -> String {
        var lines: [String] = []
        if pin.changed {
            lines.append("The pinned certificate for \(pin.host) has CHANGED. "
                + "This happens on a key rotation — or interception.")
            lines.append("was  \(String(pin.previousFingerprint.prefix(16)))…")
            lines.append("now  \(String(pin.fingerprint.prefix(16)))…")
        } else {
            lines.append("\(pin.host) presented a certificate that can't be verified"
                + (pin.reason.isEmpty ? "." : " (\(pin.reason))."))
            lines.append("SPKI \(String(pin.fingerprint.prefix(16)))…")
        }
        if !pin.subject.isEmpty { lines.append(pin.subject) }
        if !pin.expires.isEmpty { lines.append("Expires \(pin.expires)") }
        lines.append("Pinning trusts this exact certificate from now on.")
        return lines.joined(separator: "\n")
    }

    // Camera wall: keep the screen awake while actually showing video.
    private func updateIdleTimer() {
        UIApplication.shared.isIdleTimerDisabled = engine.connected && scenePhase == .active
    }

    private func autoStart() {
        guard !didAutoStart else { return }
        didAutoStart = true
        if SettingsBridge.current().baseURL.isEmpty {
            phase = .welcome // first run: welcome -> permission -> settings
        } else {
            phase = .ready
            engine.connect()
        }
    }
}

#Preview {
    ContentView()
}
