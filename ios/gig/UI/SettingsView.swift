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

    // Staged working copy of the connection registry; committed wholesale on
    // Save (Cancel discards adds/edits/deletes alike, like the desktop
    // dialogs). `activeID` tracks the checked row by draft identity so it
    // survives deletes/reorders; mapped to an index at save time.
    @State private var connections: [ConnectionDraft] = []
    @State private var activeID: UUID?
    @State private var editorDraft: ConnectionDraft?
    @State private var dimLevel: Double = 60
    @State private var dimDelay = 600
    @State private var orbitStep: Double = 40
    @State private var activityView = false
    @State private var motionActivity = false
    @State private var activeOnly = true
    @State private var showBoxes = true
    @State private var labelSize = 0
    @State private var keepHiddenStreams = true
    @State private var hideOfflineCameras = false
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
                Section {
                    ForEach(connections) { item in
                        Button {
                            activeID = item.id
                        } label: {
                            HStack(spacing: 10) {
                                Image(systemName: item.id == activeID ? "checkmark.circle.fill" : "circle")
                                    .foregroundStyle(item.id == activeID ? Color.accentColor : Color.secondary)
                                Text(item.label)
                                    .foregroundStyle(.primary)
                                Spacer()
                                Button {
                                    editorDraft = item
                                } label: {
                                    Image(systemName: "pencil")
                                }
                                .buttonStyle(.borderless)
                            }
                        }
                        .swipeActions(edge: .trailing) {
                            Button(role: .destructive) {
                                deleteConnection(item)
                            } label: {
                                Label("Delete", systemImage: "trash")
                            }
                        }
                    }
                    Button {
                        editorDraft = ConnectionDraft()
                    } label: {
                        Label("Add Connection…", systemImage: "plus")
                    }
                } header: {
                    Text("Connections")
                } footer: {
                    Text("gig connects to the checked server. Tap a row to switch, the pencil to edit; swipe to delete. Changes apply on Save.")
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
                    toggleRow("Draw detection boxes", isOn: $showBoxes,
                              note: "Red pulses around a live detection; blue lingers where one just ended.")
                    VStack(alignment: .leading, spacing: 3) {
                        Picker("Label size", selection: $labelSize) {
                            Text("Normal").tag(0)
                            Text("Large").tag(1)
                            Text("Larger").tag(2)
                        }
                        Text("Applies to the tile labels and the all-quiet line — for walls watched from a distance.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                    .padding(.vertical, 2)
                    toggleRow("Keep hidden cameras streaming", isOn: $keepHiddenStreams,
                              note: "Off saves power; a hidden camera reconnects in a second or two when it appears.")
                    toggleRow("Hide offline cameras", isOn: $hideOfflineCameras,
                              note: "A camera with no video disappears from the wall; a status line appears if all are down.")
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
            .sheet(item: $editorDraft) { draft in
                ConnectionEditorView(
                    draft: draft,
                    isDuplicate: { edited in
                        connections.contains {
                            $0.id != edited.id && $0.identityKey == edited.identityKey
                        }
                    },
                    onCommit: { edited in
                        if let index = connections.firstIndex(where: { $0.id == edited.id }) {
                            connections[index] = edited
                        } else {
                            connections.append(edited)
                        }
                        if activeID == nil {
                            activeID = edited.id
                        }
                    })
            }
        }
    }

    private func deleteConnection(_ item: ConnectionDraft) {
        connections.removeAll { $0.id == item.id }
        if activeID == item.id {
            activeID = connections.first?.id
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
        connections = SettingsBridge.connections().map { ConnectionDraft(from: $0) }
        let active = Int(SettingsBridge.activeConnectionIndex())
        activeID = (active >= 0 && active < connections.count)
            ? connections[active].id
            : connections.first?.id
        if connections.isEmpty {
            // First run (or everything deleted): drop straight into the Add
            // editor instead of presenting an empty list.
            editorDraft = ConnectionDraft()
        }

        let s = SettingsBridge.current()
        dimLevel = Double(s.dimLevelPercent)
        dimDelay = s.dimDelaySeconds
        orbitStep = Double(s.orbitStepSeconds)
        activityView = s.activityView
        motionActivity = s.motionActivity
        activeOnly = s.activeOnly
        showBoxes = s.showBoxes
        labelSize = s.labelSize
        keepHiddenStreams = s.keepHiddenStreams
        hideOfflineCameras = s.hideOfflineCameras
    }

    private func save() {
        var items: [Connection] = []
        var activeIndex = -1
        for draft in connections {
            if draft.id == activeID {
                activeIndex = items.count
            }
            items.append(draft.asConnection())
        }
        SettingsBridge.saveConnections(items, activeIndex: activeIndex)

        let s = Settings()
        s.dimLevelPercent = Int(dimLevel)
        s.dimDelaySeconds = dimDelay
        s.orbitStepSeconds = Int(orbitStep)
        s.activityView = activityView
        s.motionActivity = motionActivity
        s.activeOnly = activeOnly
        s.showBoxes = showBoxes
        s.labelSize = labelSize
        s.keepHiddenStreams = keepHiddenStreams
        s.hideOfflineCameras = hideOfflineCameras
        SettingsBridge.save(s)
    }
}

// The settings sheet's staged working copy of one saved server. `storedID`
// rides through from the bridge so a commit can preserve the entry's no-UI
// ride-alongs (and reparent a URL edit); "" for a freshly added draft.
fileprivate struct ConnectionDraft: Identifiable, Equatable {
    let id = UUID()
    var baseURL = ""
    var user = ""
    var password = ""
    var insecure = false
    var storedID = ""

    init() {}

    init(from conn: Connection) {
        baseURL = conn.baseURL
        user = conn.user
        password = conn.password
        insecure = conn.insecure
        storedID = conn.storedID
    }

    func asConnection() -> Connection {
        let conn = Connection()
        conn.baseURL = baseURL
        conn.user = user
        conn.password = password
        conn.insecure = insecure
        conn.storedID = storedID
        return conn
    }

    // Label/identity come from the shared C++ (host:port formatting and the
    // URL hash), via a scratch bridge object -- no duplicated URL parsing.
    var label: String { asConnection().listLabel }
    var identityKey: String { asConnection().identityKey }
}

// Per-connection editor sheet: URL, credentials + the per-server insecure
// toggle. Validates a non-empty URL and rejects a duplicate of another
// entry's URL (identity is URL-only); commits into the PARENT's staged list
// only -- nothing persists until the settings sheet's Save.
fileprivate struct ConnectionEditorView: View {
    @Environment(\.dismiss) private var dismiss
    @State var draft: ConnectionDraft
    let isDuplicate: (ConnectionDraft) -> Bool
    let onCommit: (ConnectionDraft) -> Void
    @State private var problem: String?

    var body: some View {
        NavigationStack {
            Form {
                Section("Frigate") {
                    TextField("https://frigate.lan:8971/", text: $draft.baseURL)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .keyboardType(.URL)
                    TextField("user", text: $draft.user)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    SecureField("password", text: $draft.password)
                }
                Section {
                    Toggle("Insecure (skip verification)", isOn: $draft.insecure)
                } header: {
                    Text("TLS")
                } footer: {
                    Text("Certificates trusted by iOS work automatically. For a self-signed Frigate, just connect — gig offers to pin the certificate.")
                }
            }
            .navigationTitle("Connection")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        if draft.baseURL.trimmingCharacters(in: .whitespaces).isEmpty {
                            problem = "Enter the Frigate URL."
                        } else if isDuplicate(draft) {
                            problem = "A connection with this URL already exists."
                        } else {
                            onCommit(draft)
                            dismiss()
                        }
                    }
                }
            }
            .alert("Can’t save", isPresented: Binding(
                get: { problem != nil },
                set: { if !$0 { problem = nil } }
            )) {
                Button("OK", role: .cancel) {}
            } message: {
                Text(problem ?? "")
            }
        }
    }
}

#Preview {
    SettingsView(onSave: {})
}
