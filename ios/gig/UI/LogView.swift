//
//  LogView.swift
//  gig
//
//  The in-app log ring (gig::LogBuffer, the same buffer the desktop log view
//  reads) as a SwiftUI sheet: monospaced, auto-refreshing, Copy + Clear.
//

import SwiftUI
import UIKit

struct LogView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var text = ""

    private let ticker = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    var body: some View {
        NavigationStack {
            ScrollView([.vertical, .horizontal]) {
                Text(text.isEmpty ? "(log is empty)" : text)
                    .font(.caption2.monospaced())
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(10)
                    .textSelection(.enabled)
            }
            .background(Color(white: 0.05))
            .navigationTitle("Log")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }
                }
                ToolbarItemGroup(placement: .primaryAction) {
                    Button {
                        UIPasteboard.general.string = text
                    } label: {
                        Image(systemName: "doc.on.doc")
                    }
                    .accessibilityLabel("Copy")
                    Button {
                        LogBridge.clear()
                        text = ""
                    } label: {
                        Image(systemName: "trash")
                    }
                    .accessibilityLabel("Clear")
                }
            }
            .onAppear { text = LogBridge.snapshotText() }
            .onReceive(ticker) { _ in text = LogBridge.snapshotText() }
        }
    }
}

#Preview {
    LogView()
}
