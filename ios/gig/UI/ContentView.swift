//
//  ContentView.swift
//  gig
//
//  Scaffold UI: a settings form (Frigate URL + credentials) backed by the C++
//  SettingsStore, and a Connect action that drives the full native core
//  (gig::AppSession: login -> discover -> camera supervisor -> decode) through
//  GIGEngine. Pressing Connect exercises the whole iOS-linked stack (Boost/Beast,
//  OpenSSL, FFmpeg). The live camera grid is the remaining piece — frames decode
//  now, but MetalVideoView still clears the screen. See docs/README-IOS.md.
//

import SwiftUI

@MainActor
final class EngineModel: ObservableObject {
    @Published var connecting = false
    @Published var connected = false
    @Published var statusLine = "Not connected"

    func connect() {
        connecting = true
        statusLine = "Connecting…"
        Task.detached(priority: .userInitiated) {
            let s = Engine.shared().connect()
            await MainActor.run { self.apply(s) }
        }
    }

    func disconnect() {
        Engine.shared().disconnect()
        connected = false
        statusLine = "Not connected"
    }

    func refresh() {
        guard connected else { return }
        apply(Engine.shared().status())
    }

    private func apply(_ s: EngineStatus) {
        connecting = false
        connected = s.connected
        if s.connected {
            statusLine = "live \(s.liveCameraCount)/\(s.cameraCount) · \(s.decodedFrames) frames · \(s.ingestKbps) kbps"
        } else {
            statusLine = s.message == "ok" ? "Not connected" : "Failed: \(s.message)"
        }
    }
}

struct ContentView: View {
    @StateObject private var engine = EngineModel()

    @State private var baseURL = ""
    @State private var user = ""
    @State private var password = ""
    @State private var caPath = ""
    @State private var insecure = false

    private let ticker = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    var body: some View {
        NavigationStack {
            Form {
                Section("Video") {
                    MetalVideoView()
                        .frame(height: 200)
                        .clipShape(RoundedRectangle(cornerRadius: 12))
                        .listRowInsets(EdgeInsets())
                }

                Section("Status") {
                    Text(engine.statusLine)
                        .font(.footnote.monospaced())
                        .foregroundStyle(engine.connected ? .green : .secondary)
                    if engine.connecting {
                        ProgressView()
                    }
                    if engine.connected {
                        Button("Disconnect", role: .destructive) { engine.disconnect() }
                    } else {
                        Button("Connect") { save(); engine.connect() }
                            .disabled(engine.connecting)
                    }
                }

                Section("Frigate") {
                    TextField("https://frigate.lan:8971/", text: $baseURL)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .keyboardType(.URL)
                    TextField("user", text: $user)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    SecureField("password", text: $password)
                }

                Section("TLS") {
                    TextField("PEM CA path (optional)", text: $caPath)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    Toggle("Insecure (skip verification)", isOn: $insecure)
                    Text("iOS can't read the system trust store — use a PEM CA, a cert pin, or Insecure for a self-signed Frigate.")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }

                Section {
                    Button("Save settings") { save() }
                }
            }
            .navigationTitle("gig")
            .onAppear(perform: load)
            .onReceive(ticker) { _ in engine.refresh() }
        }
    }

    private func load() {
        let s = SettingsBridge.load()
        baseURL = s.baseURL
        user = s.user
        password = s.password
        caPath = s.caPath
        insecure = s.insecure
    }

    private func save() {
        let s = Settings()
        s.baseURL = baseURL
        s.user = user
        s.password = password
        s.caPath = caPath
        s.insecure = insecure
        SettingsBridge.save(s)
    }
}

#Preview {
    ContentView()
}
