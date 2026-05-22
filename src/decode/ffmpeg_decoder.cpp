#include "decode/ffmpeg_decoder.h"

#include <array>
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

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
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

CodecContextPtr openCodecContext(
    const AVCodec* decoder,
    AVStream* videoStream,
    HardwareDecodeState& hardwareState,
    BufferRefPtr& hardwareDevice)
{
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, i);
        if (!config) {
            break;
        }

        if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
            || config->device_type == AV_HWDEVICE_TYPE_NONE
            || config->pix_fmt == AV_PIX_FMT_NONE) {
            continue;
        }

        AVBufferRef* rawDevice = nullptr;
        const int createDeviceResult = av_hwdevice_ctx_create(&rawDevice, config->device_type, nullptr, nullptr, 0);
        if (createDeviceResult < 0) {
            continue;
        }

        BufferRefPtr candidateDevice(rawDevice);
        CodecContextPtr candidate(avcodec_alloc_context3(decoder));
        if (!candidate) {
            throw std::runtime_error("Could not allocate AVCodecContext.");
        }

        throwIfFailed(avcodec_parameters_to_context(candidate.get(), videoStream->codecpar), "avcodec_parameters_to_context");
        candidate->flags |= AV_CODEC_FLAG_LOW_DELAY;
        candidate->thread_count = 1;
        candidate->opaque = &hardwareState;
        candidate->get_format = chooseHardwarePixelFormat;
        candidate->hw_device_ctx = av_buffer_ref(candidateDevice.get());
        if (!candidate->hw_device_ctx) {
            throw std::runtime_error("Could not reference hardware device context.");
        }

        hardwareState.pixelFormat = config->pix_fmt;
        hardwareState.deviceType = config->device_type;

        const int openResult = avcodec_open2(candidate.get(), decoder, nullptr);
        if (openResult >= 0) {
            std::cerr << "Hardware decode: " << av_hwdevice_get_type_name(config->device_type)
                      << " (" << av_get_pix_fmt_name(config->pix_fmt) << ")\n";
            hardwareDevice = std::move(candidateDevice);
            return candidate;
        }

        std::cerr << "Hardware decode candidate failed: "
                  << av_hwdevice_get_type_name(config->device_type)
                  << " (" << ffmpegError(openResult) << ")\n";
    }

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

    std::cerr << "Hardware decode unavailable; using software decode.\n";
    return codecContext;
}

} // namespace

FfmpegDecoder::FfmpegDecoder(std::string url, TlsOptions tlsOptions, FrameCallback frameCallback)
    : url_(std::move(url))
    , tlsOptions_(std::move(tlsOptions))
    , frameCallback_(std::move(frameCallback))
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
    CodecContextPtr codecContext = openCodecContext(decoder, videoStream, hardwareState, hardwareDevice);

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
        throwIfFailed(sendResult, "avcodec_send_packet");

        while (!stopRequested_) {
            const int receiveResult = avcodec_receive_frame(codecContext.get(), decodedFrame.get());
            if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
                break;
            }
            throwIfFailed(receiveResult, "avcodec_receive_frame");

            if (hardwareState.pixelFormat != AV_PIX_FMT_NONE && decodedFrame->format == hardwareState.pixelFormat) {
                FramePtr transferredFrame(av_frame_alloc());
                if (!transferredFrame) {
                    throw std::runtime_error("Could not allocate hardware transfer frame.");
                }

                throwIfFailed(av_hwframe_transfer_data(transferredFrame.get(), decodedFrame.get(), 0), "av_hwframe_transfer_data");
                av_frame_copy_props(transferredFrame.get(), decodedFrame.get());
                emitDecodedFrame(transferredFrame.get(), ++frameIndex_, swsContext, frameCallback_);
            } else {
                emitDecodedFrame(decodedFrame.get(), ++frameIndex_, swsContext, frameCallback_);
            }

            av_frame_unref(decodedFrame.get());
        }
    }
}
