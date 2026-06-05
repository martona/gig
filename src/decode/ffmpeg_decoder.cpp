#include "decode/ffmpeg_decoder.h"

#include <array>
#include <cstdarg>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace {

struct FormatContextDeleter {
    void operator()(AVFormatContext* context) const
    {
        if (context) {
            avformat_close_input(&context);
        }
    }
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const
    {
        avcodec_free_context(&context);
    }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const
    {
        av_packet_free(&packet);
    }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const
    {
        av_frame_free(&frame);
    }
};

struct SwsContextDeleter {
    void operator()(SwsContext* context) const
    {
        sws_freeContext(context);
    }
};

struct BufferRefDeleter {
    void operator()(AVBufferRef* ref) const
    {
        av_buffer_unref(&ref);
    }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;
using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefDeleter>;

std::once_flag ffmpegNetworkInitOnce;

// FFmpeg polls this during blocking I/O (open/read); returning non-zero aborts
// promptly so stop() never waits the full rw_timeout to join the worker.
int interruptCallback(void* opaque)
{
    const auto* stopFlag = static_cast<const std::atomic_bool*>(opaque);
    return (stopFlag && stopFlag->load()) ? 1 : 0;
}

// The H.264 decoder spews recoverable-error spam ("non-existing PPS", "no
// frame!", concealment, ...) when fed live MPEG-TS whose slices reference a
// keyframe/parameter set it does not have yet -- and these go2rtc streams do it
// continuously, not just at startup. We already handle the underlying condition
// via return codes + the health supervisor, so drop these by message pattern
// while letting every other FFmpeg message (protocol, demux, genuine errors)
// reach stderr. Extend the list if a new decoder complaint shows up.
bool isSpuriousDecodeMessage(const char* fmt)
{
    if (!fmt) {
        return false;
    }
    static const char* const patterns[] = {
        "non-existing PPS",
        "non-existing SPS",
        "no frame!",
        "decode_slice_header error",
        "Reference picture missing",
        "mmco",
        "number of reference frames",
        "illegal memory management",
        "error while decoding MB",
        "concealing",
        "missing picture in access unit",
        "Invalid NAL unit size",
        "Error splitting the input into NAL units",
        "out of range intra chroma pred mode",
        "left block unavailable",
        "top block unavailable",
        "corrupted",
    };
    for (const char* pattern : patterns) {
        if (std::strstr(fmt, pattern) != nullptr) {
            return true;
        }
    }
    return false;
}

void ffmpegLogCallback(void* avcl, int level, const char* fmt, va_list args)
{
    if (level >= AV_LOG_ERROR && isSpuriousDecodeMessage(fmt)) {
        return;
    }
    av_log_default_callback(avcl, level, fmt, args);
}

struct HardwareDecodeState {
    AVPixelFormat pixelFormat = AV_PIX_FMT_NONE;
    AVHWDeviceType deviceType = AV_HWDEVICE_TYPE_NONE;
};

std::string ffmpegError(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

void throwIfFailed(int code, const char* action)
{
    if (code < 0) {
        throw std::runtime_error(std::string(action) + ": " + ffmpegError(code));
    }
}

void setOption(AVDictionary** options, const char* key, const std::string& value)
{
    if (!value.empty()) {
        av_dict_set(options, key, value.c_str(), 0);
    }
}

AVPixelFormat chooseHardwarePixelFormat(AVCodecContext* context, const AVPixelFormat* formats)
{
    const auto* hardwareState = static_cast<const HardwareDecodeState*>(context->opaque);
    if (hardwareState && hardwareState->pixelFormat != AV_PIX_FMT_NONE) {
        for (const AVPixelFormat* candidate = formats; *candidate != AV_PIX_FMT_NONE; ++candidate) {
            if (*candidate == hardwareState->pixelFormat) {
                return *candidate;
            }
        }

        return AV_PIX_FMT_NONE;
    }

    return formats[0];
}

void d3d11LockCallback(void* lockContext)
{
    static_cast<std::recursive_mutex*>(lockContext)->lock();
}

void d3d11UnlockCallback(void* lockContext)
{
    static_cast<std::recursive_mutex*>(lockContext)->unlock();
}

bool codecSupportsD3D11(const AVCodec* decoder)
{
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, i);
        if (!config) {
            return false;
        }

        if (config->device_type == AV_HWDEVICE_TYPE_D3D11VA
            && config->pix_fmt == AV_PIX_FMT_D3D11
            && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            return true;
        }
    }
}

// True if the renderer's D3D11 device is a software adapter (WARP / Basic Render
// Driver). Such adapters don't really do video decode, so we skip hardware and
// go straight to software -- the same guard the hitsc pikvm backend uses.
bool d3d11DeviceIsSoftwareAdapter(ID3D11Device* device)
{
    if (!device) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) {
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter1;
    if (FAILED(adapter.As(&adapter1))) {
        return false;
    }
    DXGI_ADAPTER_DESC1 desc = {};
    if (FAILED(adapter1->GetDesc1(&desc))) {
        return false;
    }
    return (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
}

void copyPlane(
    const std::uint8_t* source,
    int sourceStride,
    int widthBytes,
    int height,
    std::vector<std::uint8_t>& destination,
    int& destinationStride)
{
    destinationStride = widthBytes;
    destination.resize(static_cast<std::size_t>(widthBytes) * static_cast<std::size_t>(height));

    for (int y = 0; y < height; ++y) {
        std::memcpy(
            destination.data() + static_cast<std::size_t>(y) * destinationStride,
            source + static_cast<std::size_t>(y) * sourceStride,
            static_cast<std::size_t>(widthBytes));
    }
}

bool isFullRangeYuv(const AVFrame* frame)
{
    if (frame->color_range == AVCOL_RANGE_JPEG) {
        return true;
    }

    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    return descriptor && descriptor->name && std::strncmp(descriptor->name, "yuvj", 4) == 0;
}

bool isPlanarYuv4208Bit(AVPixelFormat pixelFormat)
{
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(pixelFormat);
    return descriptor
        && descriptor->nb_components == 3
        && descriptor->log2_chroma_w == 1
        && descriptor->log2_chroma_h == 1
        && descriptor->comp[0].depth == 8
        && descriptor->comp[1].depth == 8
        && descriptor->comp[2].depth == 8
        && (descriptor->flags & AV_PIX_FMT_FLAG_PLANAR);
}

bool emitNv12Frame(const AVFrame* frame, std::uint64_t frameIndex, const FfmpegDecoder::FrameCallback& callback)
{
    if (!frame->data[0] || !frame->data[1]) {
        return false;
    }

    const int chromaWidthBytes = ((frame->width + 1) / 2) * 2;
    const int chromaHeight = (frame->height + 1) / 2;

    VideoFrame output;
    output.format = VideoFrameFormat::NV12;
    output.width = frame->width;
    output.height = frame->height;
    output.fullRange = isFullRangeYuv(frame);
    output.index = frameIndex;

    copyPlane(frame->data[0], frame->linesize[0], frame->width, frame->height, output.planes[0], output.strides[0]);
    copyPlane(frame->data[1], frame->linesize[1], chromaWidthBytes, chromaHeight, output.planes[1], output.strides[1]);

    callback(std::move(output));
    return true;
}

bool emitD3D11Frame(
    const AVFrame* frame,
    std::uint64_t frameIndex,
    const std::shared_ptr<D3D11DecodeContext>& d3d11Context,
    const FfmpegDecoder::FrameCallback& callback)
{
    if (!frame->data[0]) {
        return false;
    }

    auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12) {
        return false;
    }

    AVFrame* cloned = av_frame_clone(frame);
    if (!cloned) {
        throw std::runtime_error("Could not clone D3D11 hardware frame.");
    }

    VideoFrame output;
    output.format = VideoFrameFormat::D3D11_NV12;
    output.width = cloned->width;
    output.height = cloned->height;
    output.fullRange = isFullRangeYuv(cloned);
    output.index = frameIndex;
    output.d3d11Texture = reinterpret_cast<ID3D11Texture2D*>(cloned->data[0]);
    output.d3d11ArraySlice = static_cast<int>(reinterpret_cast<std::intptr_t>(cloned->data[1]));
    if (d3d11Context) {
        output.d3d11Lock = d3d11Context->lock;
    }
    output.owner = std::shared_ptr<void>(cloned, [](void* pointer) {
        AVFrame* ownedFrame = static_cast<AVFrame*>(pointer);
        av_frame_free(&ownedFrame);
    });

    callback(std::move(output));
    return true;
}

bool emitYuv420Frame(const AVFrame* frame, std::uint64_t frameIndex, const FfmpegDecoder::FrameCallback& callback)
{
    if (!frame->data[0] || !frame->data[1] || !frame->data[2]) {
        return false;
    }

    const int chromaWidth = (frame->width + 1) / 2;
    const int chromaHeight = (frame->height + 1) / 2;

    VideoFrame output;
    output.format = VideoFrameFormat::YUV420P;
    output.width = frame->width;
    output.height = frame->height;
    output.fullRange = isFullRangeYuv(frame);
    output.index = frameIndex;

    copyPlane(frame->data[0], frame->linesize[0], frame->width, frame->height, output.planes[0], output.strides[0]);
    copyPlane(frame->data[1], frame->linesize[1], chromaWidth, chromaHeight, output.planes[1], output.strides[1]);
    copyPlane(frame->data[2], frame->linesize[2], chromaWidth, chromaHeight, output.planes[2], output.strides[2]);

    callback(std::move(output));
    return true;
}

void emitBgraFrame(
    const AVFrame* frame,
    std::uint64_t frameIndex,
    SwsContextPtr& swsContext,
    const FfmpegDecoder::FrameCallback& callback)
{
    const auto sourceFormat = static_cast<AVPixelFormat>(frame->format);
    SwsContext* converted = sws_getCachedContext(
        swsContext.release(),
        frame->width,
        frame->height,
        sourceFormat,
        frame->width,
        frame->height,
        AV_PIX_FMT_BGRA,
        SWS_FAST_BILINEAR,
        nullptr,
        nullptr,
        nullptr);

    if (!converted) {
        throw std::runtime_error("Could not create swscale context.");
    }
    swsContext.reset(converted);

    const int* coefficients = sws_getCoefficients(SWS_CS_ITU709);
    const int sourceRange = isFullRangeYuv(frame) ? 1 : 0;
    constexpr int destinationRange = 1;
    sws_setColorspaceDetails(
        swsContext.get(),
        coefficients,
        sourceRange,
        coefficients,
        destinationRange,
        0,
        1 << 16,
        1 << 16);

    VideoFrame output;
    output.format = VideoFrameFormat::BGRA;
    output.width = frame->width;
    output.height = frame->height;
    output.strides[0] = output.width * 4;
    output.index = frameIndex;
    output.planes[0].resize(static_cast<std::size_t>(output.strides[0]) * static_cast<std::size_t>(output.height));

    std::uint8_t* destinationData[4] = { output.planes[0].data(), nullptr, nullptr, nullptr };
    int destinationStride[4] = { output.strides[0], 0, 0, 0 };
    sws_scale(
        swsContext.get(),
        frame->data,
        frame->linesize,
        0,
        frame->height,
        destinationData,
        destinationStride);

    callback(std::move(output));
}

void emitDecodedFrame(
    const AVFrame* frame,
    std::uint64_t frameIndex,
    SwsContextPtr& swsContext,
    const FfmpegDecoder::FrameCallback& callback)
{
    const auto pixelFormat = static_cast<AVPixelFormat>(frame->format);
    if (pixelFormat == AV_PIX_FMT_NV12 && emitNv12Frame(frame, frameIndex, callback)) {
        return;
    }

    if (isPlanarYuv4208Bit(pixelFormat) && emitYuv420Frame(frame, frameIndex, callback)) {
        return;
    }

    emitBgraFrame(frame, frameIndex, swsContext, callback);
}

BufferRefPtr createD3D11DeviceContext(const std::shared_ptr<D3D11DecodeContext>& d3d11Context)
{
    if (!d3d11Context || !d3d11Context->device || !d3d11Context->lock) {
        return BufferRefPtr(nullptr);
    }

    AVBufferRef* rawDevice = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!rawDevice) {
        throw std::runtime_error("Could not allocate D3D11VA hardware device context.");
    }

    BufferRefPtr device(rawDevice);
    auto* deviceContext = reinterpret_cast<AVHWDeviceContext*>(device->data);
    auto* d3d11 = reinterpret_cast<AVD3D11VADeviceContext*>(deviceContext->hwctx);
    d3d11->device = d3d11Context->device;
    d3d11->device->AddRef();
    d3d11->lock = d3d11LockCallback;
    d3d11->unlock = d3d11UnlockCallback;
    d3d11->lock_ctx = d3d11Context->lock.get();
    d3d11->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;

    throwIfFailed(av_hwdevice_ctx_init(device.get()), "av_hwdevice_ctx_init(D3D11VA)");
    return device;
}

CodecContextPtr openCodecContext(
    const AVCodec* decoder,
    AVStream* videoStream,
    HardwareDecodeState& hardwareState,
    BufferRefPtr& hardwareDevice,
    const std::shared_ptr<D3D11DecodeContext>& d3d11Context,
    bool softwareOnly)
{
    const bool softwareAdapter = d3d11Context && d3d11Context->device
        && d3d11DeviceIsSoftwareAdapter(d3d11Context->device);
    if (softwareAdapter && !softwareOnly) {
        std::cerr << "Renderer is on a software D3D11 adapter; skipping hardware decode.\n";
    }

    if (!softwareOnly && !softwareAdapter && d3d11Context && d3d11Context->device && d3d11Context->lock && codecSupportsD3D11(decoder)) {
        try {
            BufferRefPtr candidateDevice = createD3D11DeviceContext(d3d11Context);
            CodecContextPtr candidate(avcodec_alloc_context3(decoder));
            if (!candidate) {
                throw std::runtime_error("Could not allocate AVCodecContext.");
            }

            throwIfFailed(avcodec_parameters_to_context(candidate.get(), videoStream->codecpar), "avcodec_parameters_to_context");
            candidate->flags |= AV_CODEC_FLAG_LOW_DELAY;
            candidate->thread_count = 1;
            candidate->thread_type = 0;
            candidate->opaque = &hardwareState;
            candidate->get_format = chooseHardwarePixelFormat;
            candidate->hw_device_ctx = av_buffer_ref(candidateDevice.get());
            if (!candidate->hw_device_ctx) {
                throw std::runtime_error("Could not reference D3D11 hardware device context.");
            }

            hardwareState.pixelFormat = AV_PIX_FMT_D3D11;
            hardwareState.deviceType = AV_HWDEVICE_TYPE_D3D11VA;
            throwIfFailed(avcodec_open2(candidate.get(), decoder, nullptr), "avcodec_open2(D3D11VA)");

            std::cerr << "Hardware decode: d3d11va (d3d11, shared renderer device)\n";
            hardwareDevice = std::move(candidateDevice);
            return candidate;
        } catch (const std::exception& error) {
            std::cerr << "D3D11VA shared-device decode unavailable; falling back: "
                      << error.what() << "\n";
            hardwareState.pixelFormat = AV_PIX_FMT_NONE;
            hardwareState.deviceType = AV_HWDEVICE_TYPE_NONE;
        }
    }

    // Deliberately no other-hardware fallback. On Windows the shared-device
    // D3D11VA path above is the only zero-copy option, and D3D11VA already
    // covers every real GPU (it is vendor-agnostic). The standalone-device
    // alternatives (DXVA2, QSV, CUDA, ...) only add a system-memory round-trip
    // -- and on GPU-less VMs DXVA2 in particular *opens* successfully then
    // decodes garbage, which is worse than an honest fall-through to software.

    hardwareState.pixelFormat = AV_PIX_FMT_NONE;
    hardwareState.deviceType = AV_HWDEVICE_TYPE_NONE;

    CodecContextPtr codecContext(avcodec_alloc_context3(decoder));
    if (!codecContext) {
        throw std::runtime_error("Could not allocate AVCodecContext.");
    }

    throwIfFailed(avcodec_parameters_to_context(codecContext.get(), videoStream->codecpar), "avcodec_parameters_to_context");
    codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codecContext->thread_count = 1;
    throwIfFailed(avcodec_open2(codecContext.get(), decoder, nullptr), "avcodec_open2");

    std::cerr << (softwareOnly ? "Software decode (forced).\n" : "Hardware decode unavailable; using software decode.\n");
    return codecContext;
}

} // namespace

FfmpegDecoder::FfmpegDecoder(
    std::string url,
    TlsOptions tlsOptions,
    std::shared_ptr<D3D11DecodeContext> d3d11Context,
    FrameCallback frameCallback,
    bool softwareOnly)
    : url_(std::move(url))
    , tlsOptions_(std::move(tlsOptions))
    , d3d11Context_(std::move(d3d11Context))
    , frameCallback_(std::move(frameCallback))
    , softwareOnly_(softwareOnly)
{
}

FfmpegDecoder::~FfmpegDecoder()
{
    stop();
}

void FfmpegDecoder::start()
{
    if (worker_.joinable()) {
        return;
    }

    stopRequested_ = false;
    std::call_once(ffmpegNetworkInitOnce, [] {
        avformat_network_init();
        av_log_set_callback(ffmpegLogCallback);
    });

    worker_ = std::thread([this] {
        run();
    });
}

void FfmpegDecoder::stop()
{
    stopRequested_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }
}

void FfmpegDecoder::run()
{
    while (!stopRequested_) {
        try {
            decodeOnce();
        } catch (const std::exception& error) {
            std::cerr << "Decoder error: " << error.what() << "\n";
        }

        if (!stopRequested_) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
}

void FfmpegDecoder::decodeOnce()
{
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rw_timeout", std::to_string(tlsOptions_.rwTimeoutUs).c_str(), 0);
    av_dict_set(&options, "tls_verify", tlsOptions_.verifyServer ? "1" : "0", 0);
    setOption(&options, "ca_file", tlsOptions_.caFile);
    setOption(&options, "cert_file", tlsOptions_.certFile);
    setOption(&options, "key_file", tlsOptions_.keyFile);

    AVFormatContext* rawFormatContext = avformat_alloc_context();
    if (!rawFormatContext) {
        av_dict_free(&options);
        throw std::runtime_error("Could not allocate AVFormatContext.");
    }

    rawFormatContext->flags |= AVFMT_FLAG_NOBUFFER;
    rawFormatContext->interrupt_callback.callback = &interruptCallback;
    rawFormatContext->interrupt_callback.opaque = &stopRequested_;

    std::cerr << "Opening stream: " << url_ << "\n";
    const int openResult = avformat_open_input(&rawFormatContext, url_.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (openResult < 0) {
        avformat_close_input(&rawFormatContext);
        throw std::runtime_error(std::string("avformat_open_input: ") + ffmpegError(openResult));
    }

    FormatContextPtr formatContext(rawFormatContext);
    throwIfFailed(avformat_find_stream_info(formatContext.get(), nullptr), "avformat_find_stream_info");

    const int videoStreamIndex = av_find_best_stream(formatContext.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    throwIfFailed(videoStreamIndex, "av_find_best_stream");

    AVStream* videoStream = formatContext->streams[videoStreamIndex];
    const AVCodec* decoder = avcodec_find_decoder(videoStream->codecpar->codec_id);
    if (!decoder) {
        throw std::runtime_error("No decoder found for the video stream.");
    }

    HardwareDecodeState hardwareState;
    BufferRefPtr hardwareDevice(nullptr);
    CodecContextPtr codecContext = openCodecContext(decoder, videoStream, hardwareState, hardwareDevice, d3d11Context_, softwareOnly_);

    std::cerr << "Video: " << avcodec_get_name(videoStream->codecpar->codec_id)
              << " " << codecContext->width << "x" << codecContext->height << "\n";

    PacketPtr packet(av_packet_alloc());
    FramePtr decodedFrame(av_frame_alloc());
    if (!packet || !decodedFrame) {
        throw std::runtime_error("Could not allocate packet/frame.");
    }

    SwsContextPtr swsContext(nullptr);

    while (!stopRequested_) {
        const int readResult = av_read_frame(formatContext.get(), packet.get());
        if (readResult == AVERROR(EAGAIN)) {
            continue;
        }
        if (readResult == AVERROR_EOF) {
            break;
        }
        throwIfFailed(readResult, "av_read_frame");

        if (packet->stream_index != videoStreamIndex) {
            av_packet_unref(packet.get());
            continue;
        }

        const int sendResult = avcodec_send_packet(codecContext.get(), packet.get());
        av_packet_unref(packet.get());
        if (sendResult == AVERROR(EAGAIN)) {
            continue;
        }
        if (sendResult == AVERROR_INVALIDDATA) {
            // Corrupt or mid-GOP packet (common right after joining a live
            // stream, before the first keyframe). Skip it and keep the stream
            // alive rather than tearing the whole decoder down and reconnecting.
            continue;
        }
        throwIfFailed(sendResult, "avcodec_send_packet");

        while (!stopRequested_) {
            const int receiveResult = avcodec_receive_frame(codecContext.get(), decodedFrame.get());
            if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
                break;
            }
            if (receiveResult == AVERROR_INVALIDDATA) {
                // Concealed/corrupt frame; wait for the next packet instead of
                // treating it as a fatal stream error.
                break;
            }
            throwIfFailed(receiveResult, "avcodec_receive_frame");

            const std::uint64_t frameIndex = ++frameIndex_;
            if (decodedFrame->format == AV_PIX_FMT_D3D11
                && emitD3D11Frame(decodedFrame.get(), frameIndex, d3d11Context_, frameCallback_)) {
                av_frame_unref(decodedFrame.get());
                continue;
            }

            if (hardwareState.pixelFormat != AV_PIX_FMT_NONE && decodedFrame->format == hardwareState.pixelFormat) {
                FramePtr transferredFrame(av_frame_alloc());
                if (!transferredFrame) {
                    throw std::runtime_error("Could not allocate hardware transfer frame.");
                }

                throwIfFailed(av_hwframe_transfer_data(transferredFrame.get(), decodedFrame.get(), 0), "av_hwframe_transfer_data");
                av_frame_copy_props(transferredFrame.get(), decodedFrame.get());
                emitDecodedFrame(transferredFrame.get(), frameIndex, swsContext, frameCallback_);
            } else {
                emitDecodedFrame(decodedFrame.get(), frameIndex, swsContext, frameCallback_);
            }

            av_frame_unref(decodedFrame.get());
        }
    }
}
