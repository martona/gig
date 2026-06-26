#include "decode/ffmpeg_decoder.h"

#include "log.hpp"
#include "net/tls_client.hpp"

#include <array>
#include <cstdarg>
#include <chrono>
#include <condition_variable>
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
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/mem.h>
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

struct AvioContextDeleter {
    void operator()(AVIOContext* context) const
    {
        if (context) {
            // FFmpeg may have reallocated the buffer we passed avio_alloc_context,
            // so free whatever it currently owns, then the context struct itself.
            av_freep(&context->buffer);
            avio_context_free(&context);
        }
    }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;
using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefDeleter>;
using AvioContextPtr = std::unique_ptr<AVIOContext, AvioContextDeleter>;

std::once_flag ffmpegLogInitOnce;

// FFmpeg polls this during blocking I/O (open/read); returning non-zero aborts
// promptly so stop() never waits the full rw_timeout to join the worker.
int interruptCallback(void* opaque)
{
    const auto* stopFlag = static_cast<const std::atomic_bool*>(opaque);
    return (stopFlag && stopFlag->load()) ? 1 : 0;
}

// What the AVIO read callback needs: the byte source plus an optional activity
// sink. Bundled because avio_alloc_context carries a single opaque pointer.
struct ReadContext {
    gig::MediaStream* stream;
    std::atomic<std::uint64_t>* byteSink; // may be null
};

// AVIO read callback: pull decrypted bytes from our MediaStream. FFmpeg wants a
// positive byte count, AVERROR_EOF at a clean end, or another negative AVERROR on
// error/abort -- never 0.
int readPacket(void* opaque, std::uint8_t* buf, int size)
{
    auto* ctx = static_cast<ReadContext*>(opaque);
    const int result = ctx->stream->read(buf, size);
    if (result > 0) {
        if (ctx->byteSink) {
            // Cheap "we are receiving" signal for the UI -- counts pre-keyframe
            // bytes, which is exactly the window where no frame can be shown yet.
            ctx->byteSink->fetch_add(static_cast<std::uint64_t>(result), std::memory_order_relaxed);
        }
        return result;
    }
    return (result == 0) ? AVERROR_EOF : AVERROR_EXIT;
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
    if (level > AV_LOG_INFO) {
        return; // drop verbose/debug/trace
    }

    char line[1024];
    int printPrefix = 1;
    av_log_format_line2(avcl, level, fmt, args, line, sizeof(line), &printPrefix);

    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    if (text.empty()) {
        return;
    }

    const gig::LogLevel mapped = (level <= AV_LOG_ERROR)   ? gig::LogLevel::Error
        : (level <= AV_LOG_WARNING)                        ? gig::LogLevel::Warning
                                                           : gig::LogLevel::Info;
    gig::writeLog(mapped, text);
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
        gig::logWarning() << "Renderer is on a software D3D11 adapter; skipping hardware decode.";
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

            gig::logInfo() << "Hardware decode: d3d11va (d3d11, shared renderer device)";
            hardwareDevice = std::move(candidateDevice);
            return candidate;
        } catch (const std::exception& error) {
            gig::logWarning() << "D3D11VA shared-device decode unavailable; falling back: "
                              << error.what();
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

    gig::logInfo() << (softwareOnly ? "Software decode (forced)." : "Hardware decode unavailable; using software decode.");
    return codecContext;
}

} // namespace

FfmpegDecoder::FfmpegDecoder(
    std::string url,
    std::shared_ptr<gig::TlsClient> tlsClient,
    std::shared_ptr<D3D11DecodeContext> d3d11Context,
    FrameCallback frameCallback,
    bool softwareOnly,
    std::chrono::milliseconds startupDelay,
    std::atomic<std::uint64_t>* byteSink)
    : url_(std::move(url))
    , tlsClient_(std::move(tlsClient))
    , d3d11Context_(std::move(d3d11Context))
    , frameCallback_(std::move(frameCallback))
    , softwareOnly_(softwareOnly)
    , startupDelay_(startupDelay)
    , byteSink_(byteSink)
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
    std::call_once(ffmpegLogInitOnce, [] {
        // No avformat_network_init(): we no longer use any FFmpeg network
        // protocol -- bytes arrive through our custom AVIO. Just install the log
        // filter that drops the recoverable-decode-error spam.
        av_log_set_callback(ffmpegLogCallback);
    });

    worker_ = std::thread([this] {
        run();
    });
}

void FfmpegDecoder::stop()
{
    {
        // Flip the flag under startupMutex_ so a worker waiting out its startup
        // stagger can't miss the wake (no lost notify).
        std::lock_guard<std::mutex> lock(startupMutex_);
        stopRequested_ = true;
    }
    startupCv_.notify_all();
    {
        // Wake an in-flight MediaStream read so the worker doesn't block until the
        // read timeout; the interrupt_callback covers FFmpeg's own blocking.
        std::lock_guard<std::mutex> lock(streamMutex_);
        if (activeStream_) {
            activeStream_->cancel();
        }
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

void FfmpegDecoder::run()
{
    if (startupDelay_.count() > 0) {
        // Stagger the initial connect so earlier cameras' TLS handshakes seed the
        // shared session cache before this one connects -- more resumption now,
        // and fewer client-cert signings under Phase 1. Applies once per decoder
        // lifetime; transient in-loop reconnects below are not delayed.
        // Interruptible: stop() flips stopRequested_ and notifies startupCv_.
        std::unique_lock<std::mutex> lock(startupMutex_);
        startupCv_.wait_for(lock, startupDelay_, [this] { return stopRequested_.load(); });
    }

    while (!stopRequested_) {
        try {
            decodeOnce();
        } catch (const std::exception& error) {
            gig::logError() << "Decoder error: " << error.what();
        }

        if (!stopRequested_) {
            // Interruptible reconnect backoff: stop() flips stopRequested_ and
            // notifies startupCv_, so shutdown stays prompt even while a failing
            // stream is between retries. A plain sleep_for here made the worker
            // block the join for the full 2s (e.g. a down camera, or a bad URL).
            std::unique_lock<std::mutex> lock(startupMutex_);
            startupCv_.wait_for(lock, std::chrono::seconds(2), [this] { return stopRequested_.load(); });
        }
    }
}

void FfmpegDecoder::decodeOnce()
{
    // We terminate TLS ourselves; FFmpeg only ever sees decrypted MPEG-TS bytes
    // pulled from this connection through the readPacket AVIO callback below.
    std::unique_ptr<gig::MediaStream> stream = tlsClient_->open(url_, stopRequested_);

    // Publish the stream so stop() can cancel an in-flight read. The guard clears
    // the pointer before the stream is destroyed. The locals below are declared so
    // destruction runs format -> avio -> guard -> stream: FFmpeg stops reading
    // before the AVIO and socket go away, and activeStream_ is cleared first.
    {
        std::lock_guard<std::mutex> lock(streamMutex_);
        activeStream_ = stream.get();
    }
    struct ActiveStreamGuard {
        FfmpegDecoder* decoder;
        ~ActiveStreamGuard()
        {
            std::lock_guard<std::mutex> lock(decoder->streamMutex_);
            decoder->activeStream_ = nullptr;
        }
    } activeStreamGuard{ this };

    // Declared before the AVIO so it outlives it (destruction is format -> avio ->
    // readContext -> guard -> stream); readPacket reads through it.
    ReadContext readContext { stream.get(), byteSink_ };

    constexpr int ioBufferSize = 64 * 1024;
    auto* ioBuffer = static_cast<unsigned char*>(av_malloc(ioBufferSize));
    if (!ioBuffer) {
        throw std::runtime_error("Could not allocate AVIO buffer.");
    }
    AVIOContext* rawAvioContext = avio_alloc_context(
        ioBuffer, ioBufferSize, /*write_flag=*/0, &readContext, &readPacket, nullptr, nullptr);
    if (!rawAvioContext) {
        av_free(ioBuffer);
        throw std::runtime_error("Could not allocate AVIO context.");
    }
    rawAvioContext->seekable = 0;
    AvioContextPtr avioContext(rawAvioContext);

    const AVInputFormat* inputFormat = av_find_input_format("mpegts");
    if (!inputFormat) {
        throw std::runtime_error("FFmpeg is missing the mpegts demuxer.");
    }

    AVFormatContext* rawFormatContext = avformat_alloc_context();
    if (!rawFormatContext) {
        throw std::runtime_error("Could not allocate AVFormatContext.");
    }
    rawFormatContext->pb = avioContext.get();
    rawFormatContext->flags |= AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_CUSTOM_IO;
    rawFormatContext->interrupt_callback.callback = &interruptCallback;
    rawFormatContext->interrupt_callback.opaque = &stopRequested_;
    FormatContextPtr formatContext(rawFormatContext);

    // Force the mpegts demuxer (the stream is non-seekable, so it must not be
    // probed by seeking). On failure avformat_open_input frees the context and
    // NULLs our pointer but leaves pb untouched (AVFMT_FLAG_CUSTOM_IO), so the
    // AVIO + stream still unwind via RAII.
    gig::logInfo() << "Opening stream: " << url_;
    AVFormatContext* openTarget = formatContext.release();
    const int openResult = avformat_open_input(&openTarget, nullptr, inputFormat, nullptr);
    formatContext.reset(openTarget);
    if (openResult < 0) {
        throw std::runtime_error(std::string("avformat_open_input: ") + ffmpegError(openResult));
    }

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

    gig::logInfo() << "Video: " << avcodec_get_name(videoStream->codecpar->codec_id)
                   << " " << codecContext->width << "x" << codecContext->height;

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
