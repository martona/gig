//
//  MetalVideoView.swift
//  gig
//
//  Stub Metal video surface: a CAMetalLayer-backed view that clears to near-black,
//  mirroring the macOS port's first milestone (a clear-only Metal stub before the
//  real renderer). The per-tile camera grid — porting render/metal_renderer.mm off
//  SDL to host its CAMetalLayer here, plus VideoToolbox zero-copy sampling — is the
//  next chunk. See ios/README.md.
//

import SwiftUI
import MetalKit

struct MetalVideoView: UIViewRepresentable {
    func makeUIView(context: Context) -> MetalClearUIView { MetalClearUIView() }
    func updateUIView(_ uiView: MetalClearUIView, context: Context) {}
}

final class MetalClearUIView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }

    private let device = MTLCreateSystemDefaultDevice()
    private lazy var queue = device?.makeCommandQueue()
    private var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = UIColor(white: 0.06, alpha: 1.0)
        metalLayer.device = device
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = true
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func layoutSubviews() {
        super.layoutSubviews()
        let scale = window?.screen.scale ?? UIScreen.main.scale
        metalLayer.contentsScale = scale
        metalLayer.drawableSize = CGSize(width: bounds.width * scale,
                                         height: bounds.height * scale)
        render()
    }

    private func render() {
        guard let queue, let drawable = metalLayer.nextDrawable() else { return }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = drawable.texture
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.06, green: 0.06, blue: 0.07, alpha: 1.0)
        pass.colorAttachments[0].storeAction = .store
        guard let buffer = queue.makeCommandBuffer(),
              let encoder = buffer.makeRenderCommandEncoder(descriptor: pass) else { return }
        encoder.endEncoding()
        buffer.present(drawable)
        buffer.commit()
    }
}
