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
    @State private var confirmForget = false

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
                    TextField("PEM CA path (optional)", text: $caPath)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    Toggle("Insecure (skip verification)", isOn: $insecure)
                } header: {
                    Text("TLS")
                } footer: {
                    Text("Certificates trusted by iOS (public CAs or an installed CA profile) work automatically. For a self-signed Frigate, just connect — gig offers to pin the certificate.")
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
        }
    }

    private func load() {
        let s = SettingsBridge.current()
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
    SettingsView {}
}
