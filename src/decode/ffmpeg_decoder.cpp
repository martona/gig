#include "decode/ffmpeg_decoder.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
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

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

std::once_flag ffmpegNetworkInitOnce;

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

    CodecContextPtr codecContext(avcodec_alloc_context3(decoder));
    if (!codecContext) {
        throw std::runtime_error("Could not allocate AVCodecContext.");
    }

    throwIfFailed(avcodec_parameters_to_context(codecContext.get(), videoStream->codecpar), "avcodec_parameters_to_context");
    codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codecContext->thread_count = 1;
    throwIfFailed(avcodec_open2(codecContext.get(), decoder, nullptr), "avcodec_open2");

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

            const auto sourceFormat = static_cast<AVPixelFormat>(decodedFrame->format);
            SwsContext* converted = sws_getCachedContext(
                swsContext.release(),
                decodedFrame->width,
                decodedFrame->height,
                sourceFormat,
                decodedFrame->width,
                decodedFrame->height,
                AV_PIX_FMT_BGRA,
                SWS_FAST_BILINEAR,
                nullptr,
                nullptr,
                nullptr);

            if (!converted) {
                throw std::runtime_error("Could not create swscale context.");
            }
            swsContext.reset(converted);

            VideoFrame output;
            output.width = decodedFrame->width;
            output.height = decodedFrame->height;
            output.stride = output.width * 4;
            output.index = ++frameIndex_;
            output.bgra.resize(static_cast<std::size_t>(output.stride) * static_cast<std::size_t>(output.height));

            std::uint8_t* destinationData[4] = { output.bgra.data(), nullptr, nullptr, nullptr };
            int destinationStride[4] = { output.stride, 0, 0, 0 };
            sws_scale(
                swsContext.get(),
                decodedFrame->data,
                decodedFrame->linesize,
                0,
                decodedFrame->height,
                destinationData,
                destinationStride);

            frameCallback_(std::move(output));
            av_frame_unref(decodedFrame.get());
        }
    }
}
