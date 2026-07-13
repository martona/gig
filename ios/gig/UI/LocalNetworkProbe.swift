//
//  LocalNetworkProbe.swift
//  gig
//
//  Deterministic Local Network permission trigger + detector. iOS has no API to
//  request or query the Local Network permission; the only reliable mechanics:
//   * doing local network activity TRIGGERS the system prompt, and
//   * a Bonjour browse reports an explicit policy-denied state when access is off
//     (kDNSServiceErr_PolicyDenied, -65570) — unlike raw sockets, which fail with
//     the same "no route to host" as a genuinely unreachable server.
//  So: advertise a throwaway Bonjour service (NWListener) and browse for it
//  (NWBrowser). Seeing our own service = granted; the policy-denied state =
//  denied; neither within the timeout = undetermined (proceed — e.g. simulator,
//  where the permission isn't enforced). The service type must be declared in
//  Info.plist under NSBonjourServices.
//
//  Once denied, iOS never re-prompts — the only fix is the app's page in the
//  Settings app (UIApplication.openSettingsURLString), which the UI deep-links.
//

import Foundation
import Network

@MainActor
final class LocalNetworkProbe: ObservableObject {
    enum Outcome {
        case granted
        case denied
        case undetermined // no signal (simulator / exotic network); don't block
    }

    @Published var outcome: Outcome?

    private static let serviceType = "_gigprobe._tcp"
    // kDNSServiceErr_PolicyDenied from <dns_sd.h> (not reliably exposed to Swift).
    private static let policyDenied = DNSServiceErrorType(-65570)
    private var browser: NWBrowser?
    private var listener: NWListener?
    private var timeoutTask: Task<Void, Never>?

    /// Idempotent while running; safe to call again after finish() to re-probe.
    func start() {
        guard browser == nil else { return }
        outcome = nil

        // Advertise ourselves so the browse has something to find on grant.
        if let listener = try? NWListener(using: .tcp) {
            listener.service = NWListener.Service(name: "gig-\(UUID().uuidString.prefix(8))",
                                                  type: Self.serviceType)
            listener.newConnectionHandler = { connection in connection.cancel() }
            listener.stateUpdateHandler = { _ in }
            listener.start(queue: .main)
            self.listener = listener
        }

        let browser = NWBrowser(for: .bonjour(type: Self.serviceType, domain: nil), using: .tcp)
        browser.stateUpdateHandler = { [weak self] state in
            guard case let .waiting(error) = state else { return }
            if case let .dns(code) = error, code == Self.policyDenied {
                Task { @MainActor in self?.finish(.denied) }
            }
        }
        browser.browseResultsChangedHandler = { [weak self] results, _ in
            if !results.isEmpty {
                Task { @MainActor in self?.finish(.granted) }
            }
        }
        browser.start(queue: .main)
        self.browser = browser

        timeoutTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 5_000_000_000)
            self?.finish(.undetermined)
        }
    }

    func cancel() {
        finishInternal(nil)
    }

    private func finish(_ result: Outcome) {
        finishInternal(result)
    }

    private func finishInternal(_ result: Outcome?) {
        guard browser != nil || listener != nil else { return }
        timeoutTask?.cancel()
        timeoutTask = nil
        browser?.cancel()
        browser = nil
        listener?.cancel()
        listener = nil
        if let result {
            outcome = result
        }
    }
}
