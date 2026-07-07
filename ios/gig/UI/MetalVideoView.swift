//
//  MetalVideoView.swift
//  gig
//
//  Hosts the CAMetalLayer the shared C++ Metal scene renders into (via
//  VideoHost / GigRenderer.mm, CADisplayLink-driven). SwiftUI owns everything
//  around it: the toolbar, the camera-label overlay, and the sheets.
//

import SwiftUI
import UIKit

struct VideoSurfaceView: UIViewRepresentable {
    func makeUIView(context: Context) -> VideoHostUIView { VideoHostUIView() }
    func updateUIView(_ uiView: VideoHostUIView, context: Context) {}
}

final class VideoHostUIView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }

    private var attached = false

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = UIColor(white: 0.01, alpha: 1.0)
        isOpaque = true
        addGestureRecognizer(UITapGestureRecognizer(target: self, action: #selector(onTap(_:))))
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        let host = VideoHost.shared()
        if window != nil {
            if !attached {
                host.attach(layer as! CAMetalLayer)
                attached = true
            }
            pushSize()
            host.start()
        } else {
            // View left the hierarchy (not app lifecycle -- that's scenePhase in
            // ContentView). Stop the display link; the layer stays attached.
            host.stop()
        }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        pushSize()
    }

    private func pushSize() {
        let scale = window?.screen.scale ?? UIScreen.main.scale
        (layer as! CAMetalLayer).contentsScale = scale
        VideoHost.shared().setViewSize(bounds.size, scale: scale)
    }

    @objc private func onTap(_ gesture: UITapGestureRecognizer) {
        VideoHost.shared().handleTap(at: gesture.location(in: self))
    }
}
