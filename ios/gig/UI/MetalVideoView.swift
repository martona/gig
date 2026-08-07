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
        // Hero-mode pinch zoom + pan (the host/scene no-op both in grid view;
        // pan is also inert until pinched in, so tap semantics stay intact).
        let pinch = UIPinchGestureRecognizer(target: self, action: #selector(onPinch(_:)))
        pinch.delegate = self
        addGestureRecognizer(pinch)
        let pan = UIPanGestureRecognizer(target: self, action: #selector(onPan(_:)))
        pan.maximumNumberOfTouches = 2
        pan.delegate = self
        addGestureRecognizer(pan)
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

    // Both feed the host INCREMENTALLY: scale/translation reset to identity
    // after every event, so each callback carries just the delta since the
    // last one (what the scene's zoom/pan-by API expects). The began/ended
    // bracket around the whole CONTIGUOUS sequence (pinch and pan overlap
    // freely) drives the rubber-band physics: fingers down = rubbery
    // overshoot allowed, last finger up = spring back + fling.
    private var activeGestures = 0

    private func gestureBegan() {
        activeGestures += 1
        if activeGestures == 1 {
            VideoHost.shared().handleFocusGestureBegan()
        }
    }

    private func gestureEnded(velocity: CGPoint) {
        activeGestures = max(0, activeGestures - 1)
        if activeGestures == 0 {
            VideoHost.shared().handleFocusGestureEnded(velocity: velocity)
        }
    }

    @objc private func onPinch(_ gesture: UIPinchGestureRecognizer) {
        switch gesture.state {
        case .began:
            gestureBegan()
            fallthrough
        case .changed:
            VideoHost.shared().handlePinch(scale: gesture.scale, at: gesture.location(in: self))
            gesture.scale = 1.0
        case .ended, .cancelled, .failed:
            // No zoom fling: releasing a pinch springs; only the pan carries
            // momentum (and it usually ends last -- lone remaining finger).
            gestureEnded(velocity: .zero)
        default:
            break
        }
    }

    @objc private func onPan(_ gesture: UIPanGestureRecognizer) {
        switch gesture.state {
        case .began:
            gestureBegan()
            fallthrough
        case .changed:
            VideoHost.shared().handlePan(by: gesture.translation(in: self))
            gesture.setTranslation(.zero, in: self)
        case .ended, .cancelled, .failed:
            gestureEnded(velocity: gesture.velocity(in: self))
        default:
            break
        }
    }
}

extension VideoHostUIView: UIGestureRecognizerDelegate {
    // Pinch + pan must run together (the natural zoom-and-move gesture); the
    // tap stays exclusive so a pan can't also register as a tap.
    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer,
                           shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool {
        !(gestureRecognizer is UITapGestureRecognizer) && !(other is UITapGestureRecognizer)
    }
}
