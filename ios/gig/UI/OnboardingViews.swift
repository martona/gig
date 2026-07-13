//
//  OnboardingViews.swift
//  gig
//
//  The full-screen states that replace video when there is none to show: the
//  first-run welcome, the Local Network permission step, and the connection
//  error screen. The iOS counterparts of the desktop renderers' status panel.
//

import SwiftUI
import UIKit

// First run: nothing configured yet.
struct WelcomeView: View {
    var onSetUp: () -> Void

    var body: some View {
        VStack(spacing: 12) {
            Spacer()
            Image(systemName: "square.grid.2x2")
                .font(.system(size: 44))
                .foregroundStyle(Color(red: 0.90, green: 0.95, blue: 1.0))
            Text("gig")
                .font(.largeTitle.weight(.semibold))
            Text("Live multi-camera viewer for Frigate.\nPoint it at your Frigate server to get started.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Spacer().frame(height: 16)
            Button(action: onSetUp) {
                Text("Set Up Connection")
                    .font(.body.weight(.semibold))
                    .padding(.horizontal, 24)
                    .padding(.vertical, 10)
            }
            .buttonStyle(.borderedProminent)
            Spacer()
            Spacer()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding(.horizontal, 24)
    }
}

// The pre-permission explainer + probe: triggers the Local Network prompt at a
// moment we designed, then verifies the outcome deterministically.
struct PermissionStepView: View {
    @ObservedObject var probe: LocalNetworkProbe
    var onContinue: () -> Void

    var body: some View {
        VStack(spacing: 12) {
            Spacer()
            Image(systemName: probe.outcome == .denied ? "wifi.exclamationmark" : "wifi")
                .font(.system(size: 44))
                .foregroundStyle(probe.outcome == .denied ? .orange : Color(red: 0.90, green: 0.95, blue: 1.0))

            if probe.outcome == .denied {
                Text("Local Network access is off")
                    .font(.title3.weight(.semibold))
                Text("gig can't reach devices on your network without it. iOS only asks once — turn it on in Settings, then come back.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                Spacer().frame(height: 16)
                Button {
                    if let url = URL(string: UIApplication.openSettingsURLString) {
                        UIApplication.shared.open(url)
                    }
                } label: {
                    Text("Open iOS Settings")
                        .font(.body.weight(.semibold))
                        .padding(.horizontal, 24)
                        .padding(.vertical, 10)
                }
                .buttonStyle(.borderedProminent)
                Button("Check Again") { probe.start() }
                    .padding(.top, 4)
                Button("Continue Anyway", action: onContinue)
                    .foregroundStyle(.secondary)
                    .padding(.top, 4)
            } else {
                Text("Local network access")
                    .font(.title3.weight(.semibold))
                Text("gig streams cameras from your Frigate server over your local network. iOS will ask for permission — please allow it.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                Spacer().frame(height: 16)
                if probe.outcome == nil {
                    ProgressView()
                } else {
                    // granted / undetermined: advance handled by the owner's onChange.
                    Image(systemName: "checkmark.circle")
                        .font(.title2)
                        .foregroundStyle(.green)
                }
            }
            Spacer()
            Spacer()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding(.horizontal, 24)
        .onAppear { probe.start() }
    }
}

// A connect failed and no session is up: full room for the reason + one-tap fixes.
struct ErrorStateView: View {
    let detail: String
    let configError: Bool
    let localNetworkDenied: Bool
    var onRetry: () -> Void
    var onOpenSettings: () -> Void
    var onViewLog: () -> Void

    var body: some View {
        VStack(spacing: 12) {
            Spacer()
            Image(systemName: localNetworkDenied ? "wifi.exclamationmark" : "video.slash")
                .font(.system(size: 40))
                .foregroundStyle(.orange)

            Text(localNetworkDenied
                ? "Local Network access is off"
                : (configError ? "The connection settings need attention." : "Can't reach the Frigate server."))
                .font(.title3.weight(.semibold))
                .multilineTextAlignment(.center)

            if localNetworkDenied {
                Text("iOS is blocking gig from your local network, so the server can't be reached. Turn Local Network on for gig in Settings.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            } else if !detail.isEmpty {
                Text(detail)
                    .font(.footnote.monospaced())
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .lineLimit(6)
            }

            Spacer().frame(height: 16)

            if localNetworkDenied {
                Button {
                    if let url = URL(string: UIApplication.openSettingsURLString) {
                        UIApplication.shared.open(url)
                    }
                } label: {
                    Text("Open iOS Settings")
                        .font(.body.weight(.semibold))
                        .padding(.horizontal, 24)
                        .padding(.vertical, 10)
                }
                .buttonStyle(.borderedProminent)
                Button("Try Again", action: onRetry)
                    .padding(.top, 4)
            } else if configError {
                Button {
                    onOpenSettings()
                } label: {
                    Text("Open Settings")
                        .font(.body.weight(.semibold))
                        .padding(.horizontal, 24)
                        .padding(.vertical, 10)
                }
                .buttonStyle(.borderedProminent)
                Button("Try Again", action: onRetry)
                    .padding(.top, 4)
            } else {
                Button {
                    onRetry()
                } label: {
                    Text("Try Again")
                        .font(.body.weight(.semibold))
                        .padding(.horizontal, 24)
                        .padding(.vertical, 10)
                }
                .buttonStyle(.borderedProminent)
                Button("Open Settings", action: onOpenSettings)
                    .padding(.top, 4)
            }
            Button("View Log", action: onViewLog)
                .font(.footnote)
                .foregroundStyle(.secondary)
                .padding(.top, 8)
            Spacer()
            Spacer()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding(.horizontal, 24)
    }
}
