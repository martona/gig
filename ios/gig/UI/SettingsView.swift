//
//  SettingsView.swift
//  gig
//
//  Frigate connection settings, backed by the C++ SettingsStore (NSUserDefaults +
//  Keychain) through SettingsBridge. Save persists and reconnects (the desktop
//  settings dialog's apply-live behavior).
//

import SwiftUI

struct SettingsView: View {
    /// Live idle-dim preview: factor in 0..1 while the slider moves; negative to
    /// resume normal idle-driven dimming (call on disappear).
    var dimPreview: (CGFloat) -> Void = { _ in }
    /// Called after a successful save; the caller reconnects with the new config.
    var onSave: () -> Void
    /// TODO(onboarding-project): temporary. Called after Forget Settings wiped the
    /// store; the caller resets the engine and restarts onboarding.
    var onForget: () -> Void = {}

    @Environment(\.dismiss) private var dismiss

    @State private var baseURL = ""
    @State private var user = ""
    @State private var password = ""
    @State private var caPath = ""
    @State private var insecure = false
    @State private var dimLevel: Double = 60
    @State private var dimDelay = 600
    @State private var orbitStep: Double = 40
    @State private var activityView = false
    @State private var motionActivity = false
    @State private var activeOnly = false
    @State private var keepHiddenStreams = true
    @State private var confirmForget = false

    // Delay choices (seconds); matches the desktop dropdown.
    private static let dimDelays: [(Int, String)] = [
        (0, "Never"), (300, "5 minutes"), (600, "10 minutes"), (900, "15 minutes"),
        (1800, "30 minutes"), (3600, "1 hour"), (7200, "2 hours"),
        (14400, "4 hours"), (28800, "8 hours"),
    ]

    var body: some View {
        NavigationStack {
            Form {
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

                Section {
                    Toggle("Insecure (skip verification)", isOn: $insecure)
                } header: {
                    Text("TLS")
                } footer: {
                    Text("Certificates trusted by iOS work automatically. For a self-signed Frigate, just connect — gig offers to pin the certificate.")
                }

                Section {
                    VStack(alignment: .leading, spacing: 3) {
                        Picker("Show", selection: $activityView) {
                            Text("All cameras").tag(false)
                            Text("Active cameras only").tag(true)
                        }
                        Text("Active-only keeps the wall empty until a camera sees something. Tap anywhere to peek at every camera.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                    .padding(.vertical, 2)
                    toggleRow("Raw motion counts as activity", isOn: $motionActivity,
                              note: "Noisy on windy days — moving shadows and foliage count too.")
                    toggleRow("Ignore stationary objects", isOn: $activeOnly,
                              note: "Parked cars and settled packages stop counting ~10 seconds after they stop moving.")
                    toggleRow("Keep hidden cameras streaming", isOn: $keepHiddenStreams,
                              note: "Off saves power; a hidden camera reconnects in a second or two when it appears.")
                } header: {
                    Text("View")
                } footer: {
                    Text("Activity also wakes the display from idle dim.")
                }

                Section {
                    VStack(alignment: .leading, spacing: 4) {
                        HStack {
                            Text("Dim to")
                            Spacer()
                            Text("\(Int(dimLevel))%").foregroundStyle(.secondary)
                        }
                        // Live preview: dims the real video behind the sheet as
                        // the slider moves; released -> resumes idle-driven dimming.
                        Slider(value: $dimLevel, in: 10...100, step: 5) { editing in
                            dimPreview(editing ? CGFloat(dimLevel / 100.0) : -1)
                        }
                        .onChange(of: dimLevel) { v in dimPreview(CGFloat(v / 100.0)) }
                    }
                    Picker("Dim after", selection: $dimDelay) {
                        ForEach(Self.dimDelays, id: \.0) { Text($0.1).tag($0.0) }
                    }
                    VStack(alignment: .leading, spacing: 3) {
                        HStack {
                            Text("Pixel-shift step")
                            Spacer()
                            Text("\(Int(orbitStep)) s").foregroundStyle(.secondary)
                        }
                        Slider(value: $orbitStep, in: 1...120, step: 1)
                        Text("The image drifts ~1px per step to spread OLED wear — lower is more motion.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                    .padding(.vertical, 2)
                } header: {
                    Text("Screen protection")
                } footer: {
                    Text("Reduces brightness when idle to limit OLED burn-in.")
                }

                // TODO(onboarding-project): temporary section; remove when done.
                Section {
                    Button("Forget Settings…", role: .destructive) { confirmForget = true }
                } footer: {
                    Text("Erases everything and restarts first-run setup.")
                }
            }
            .confirmationDialog("Forget ALL settings?", isPresented: $confirmForget, titleVisibility: .visible) {
                Button("Forget Settings", role: .destructive) {
                    SettingsBridge.forgetAll()
                    dismiss()
                    onForget()
                }
                Button("Cancel", role: .cancel) {}
            } message: {
                Text("This erases the server, credentials and certificate pins, and restarts first-run setup.")
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        save()
                        dismiss()
                        onSave()
                    }
                }
            }
            .onAppear(perform: load)
            .onDisappear { dimPreview(-1) } // resume idle-driven dimming
        }
    }

    // A toggle with its own one-line explanation underneath -- the section
    // footer stays short instead of narrating every switch in one paragraph.
    private func toggleRow(_ title: String, isOn: Binding<Bool>, note: String) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Toggle(title, isOn: isOn)
            Text(note)
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
        .padding(.vertical, 2)
    }

    private func load() {
        let s = SettingsBridge.current()
        baseURL = s.baseURL
        user = s.user
        password = s.password
        caPath = s.caPath
        insecure = s.insecure
        dimLevel = Double(s.dimLevelPercent)
        dimDelay = s.dimDelaySeconds
        orbitStep = Double(s.orbitStepSeconds)
        activityView = s.activityView
        motionActivity = s.motionActivity
        activeOnly = s.activeOnly
        keepHiddenStreams = s.keepHiddenStreams
    }

    private func save() {
        let s = Settings()
        s.baseURL = baseURL
        s.user = user
        s.password = password
        s.caPath = caPath
        s.insecure = insecure
        s.dimLevelPercent = Int(dimLevel)
        s.dimDelaySeconds = dimDelay
        s.orbitStepSeconds = Int(orbitStep)
        s.activityView = activityView
        s.motionActivity = motionActivity
        s.activeOnly = activeOnly
        s.keepHiddenStreams = keepHiddenStreams
        SettingsBridge.save(s)
    }
}

#Preview {
    SettingsView(onSave: {})
}
