#include "video/VideoDecoder.h"
#include "common/Logger.h"
#include "common/Protocol.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace ndi_bridge {

// Map our OutputPixelFormat to FFmpeg's
static AVPixelFormat toAVPixelFormat(OutputPixelFormat fmt) {
    switch (fmt) {
        case OutputPixelFormat::BGRA: return AV_PIX_FMT_BGRA;
        case OutputPixelFormat::UYVY: return AV_PIX_FMT_UYVY422;
        case OutputPixelFormat::NV12: return AV_PIX_FMT_NV12;
        case OutputPixelFormat::I420: return AV_PIX_FMT_YUV420P;
    }
    return AV_PIX_FMT_NONE;
}

static const char* outputFormatName(OutputPixelFormat fmt) {
    switch (fmt) {
        case OutputPixelFormat::BGRA: return "BGRA";
        case OutputPixelFormat::UYVY: return "UYVY";
        case OutputPixelFormat::NV12: return "NV12";
        case OutputPixelFormat::I420: return "I420";
    }
    return "Unknown";
}


VideoDecoder::VideoDecoder() {
    LOG_DEBUG("VideoDecoder initialized");
}

VideoDecoder::~VideoDecoder() {
    cleanup();
    LOG_DEBUG("VideoDecoder destroyed");
}

bool VideoDecoder::configure(const VideoDecoderConfig& config) {
    cleanup();
    config_ = config;

    Logger::instance().infof("Configuring decoder, output format=%s",
                             outputFormatName(config.outputFormat));

    if (!initDecoder()) {
        LOG_ERROR("Failed to initialize decoder");
        return false;
    }

    configured_ = true;
    LOG_SUCCESS("Decoder configured, waiting for SPS/PPS");
    return true;
}

bool VideoDecoder::initDecoder() {
    // Find H.264 decoder
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        LOG_ERROR("H.264 decoder not found");
        if (onError_) onError_("H.264 decoder not found");
        return false;
    }

    Logger::instance().infof("Using decoder: %s", codec->name);

    // Allocate codec context
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        LOG_ERROR("Failed to allocate codec context");
        return false;
    }

    // Configure for low-latency decoding
    codecCtx_->thread_count = 0;  // Auto-detect
    codecCtx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codecCtx_->flags2 |= AV_CODEC_FLAG2_FAST;

    // Open codec
    int ret = avcodec_open2(codecCtx_, codec, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Logger::instance().errorf("Failed to open decoder: %s", errbuf);
        if (onError_) onError_(std::string("Failed to open decoder: ") + errbuf);
        return false;
    }

    // Allocate frame for decoded output
    frame_ = av_frame_alloc();
    if (!frame_) {
        LOG_ERROR("Failed to allocate frame");
        return false;
    }

    // Allocate packet for input
    packet_ = av_packet_alloc();
    if (!packet_) {
        LOG_ERROR("Failed to allocate packet");
        return false;
    }

    return true;
}

bool VideoDecoder::initScaler(int width, int height) {
    // Clean up existing scaler
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    if (convertedFrame_) {
        av_frame_free(&convertedFrame_);
        convertedFrame_ = nullptr;
    }

    AVPixelFormat srcFormat = codecCtx_->pix_fmt;
    AVPixelFormat dstFormat = toAVPixelFormat(config_.outputFormat);

    // If formats match, no conversion needed
    if (srcFormat == dstFormat) {
        LOG_DEBUG("No pixel format conversion needed");
        return true;
    }

    Logger::instance().debugf("Creating scaler: %s -> %s",
                              av_get_pix_fmt_name(srcFormat),
                              outputFormatName(config_.outputFormat));

    swsCtx_ = sws_getContext(
        width, height, srcFormat,
        width, height, dstFormat,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!swsCtx_) {
        LOG_ERROR("Failed to create pixel format converter");
        return false;
    }

    // Allocate converted frame
    convertedFrame_ = av_frame_alloc();
    if (!convertedFrame_) {
        LOG_ERROR("Failed to allocate converted frame");
        return false;
    }

    convertedFrame_->format = dstFormat;
    convertedFrame_->width = width;
    convertedFrame_->height = height;

    int ret = av_frame_get_buffer(convertedFrame_, 0);
    if (ret < 0) {
        LOG_ERROR("Failed to allocate converted frame buffer");
        return false;
    }

    return true;
}

void VideoDecoder::cleanup() {
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
    }

    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }

    if (convertedFrame_) {
        av_frame_free(&convertedFrame_);
        convertedFrame_ = nullptr;
    }

    if (packet_) {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }

    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }

    configured_ = false;
    decoderReady_ = false;
    width_ = 0;
    height_ = 0;
    sps_.clear();
    pps_.clear();
    stats_ = Stats{};
}

std::vector<VideoDecoder::NALUnit> VideoDecoder::parseNALUnits(const uint8_t* data, size_t size) {
    std::vector<NALUnit> nalUnits;

    if (size < 4) return nalUnits;

    size_t i = 0;
    size_t nalStart = 0;
    bool foundFirst = false;

    while (i < size - 3) {
        // Check for start code (0x00 0x00 0x01 or 0x00 0x00 0x00 0x01)
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            int startCodeLen = 0;

            if (data[i + 2] == 0x01) {
                startCodeLen = 3;
            } else if (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
                startCodeLen = 4;
            }

            if (startCodeLen > 0) {
                // Save previous NAL unit (if any)
                if (foundFirst && nalStart < i) {
                    NALUnit nal;
                    nal.data = data + nalStart;
                    nal.size = i - nalStart;
                    nal.type = data[nalStart] & 0x1F;
                    nalUnits.push_back(nal);
                }

                // Move past start code
                i += startCodeLen;
                nalStart = i;
                foundFirst = true;
                continue;
            }
        }
        i++;
    }

    // Add last NAL unit
    if (foundFirst && nalStart < size) {
        NALUnit nal;
        nal.data = data + nalStart;
        nal.size = size - nalStart;
        nal.type = data[nalStart] & 0x1F;
        nalUnits.push_back(nal);
    }

    return nalUnits;
}

bool VideoDecoder::decode(const uint8_t* data, size_t size, uint64_t timestamp) {
    if (!configured_) {
        LOG_ERROR("Decoder not configured");
        return false;
    }

    // Parse NAL units from Annex-B stream
    auto nalUnits = parseNALUnits(data, size);

    for (const auto& nal : nalUnits) {
        if (!processNALUnit(nal, timestamp)) {
            return false;
        }
    }

    return true;
}

bool VideoDecoder::processNALUnit(const NALUnit& nal, uint64_t timestamp) {
    switch (nal.type) {
        case NAL_TYPE_SPS:
            Logger::instance().debugf("Received SPS (%zu bytes)", nal.size);
            sps_.assign(nal.data, nal.data + nal.size);
            // SPS/PPS will be sent with the next IDR frame
            return true;

        case NAL_TYPE_PPS:
            Logger::instance().debugf("Received PPS (%zu bytes)", nal.size);
            pps_.assign(nal.data, nal.data + nal.size);
            decoderReady_ = !sps_.empty();
            if (decoderReady_) {
                LOG_SUCCESS("Decoder ready (SPS/PPS received)");
            }
            return true;

        case NAL_TYPE_IDR:
            stats_.keyframesDecoded++;
            return decodeNALUnit(nal, timestamp);

        case NAL_TYPE_NON_IDR:
            return decodeNALUnit(nal, timestamp);

        case NAL_TYPE_SEI:
            // Skip SEI
            return true;

        default:
            Logger::instance().debugf("Skipping NAL type %d", nal.type);
            return true;
    }
}

bool VideoDecoder::decodeNALUnit(const NALUnit& nal, uint64_t timestamp) {
    if (!decoderReady_ && nal.type != NAL_TYPE_IDR) {
        // Need SPS/PPS first, skip non-IDR frames
        LOG_DEBUG("Waiting for keyframe (SPS/PPS)");
        return true;
    }

    // Build packet with Annex-B start code
    // FFmpeg H.264 decoder expects Annex-B format
    std::vector<uint8_t> packetData;

    // For IDR frames, prepend SPS and PPS
    if (nal.type == NAL_TYPE_IDR && !sps_.empty() && !pps_.empty()) {
        packetData.reserve(sps_.size() + pps_.size() + nal.size + 12);

        // Add SPS with start code
        packetData.push_back(0x00);
        packetData.push_back(0x00);
        packetData.push_back(0x00);
        packetData.push_back(0x01);
        packetData.insert(packetData.end(), sps_.begin(), sps_.end());

        // Add PPS with start code
        packetData.push_back(0x00);
        packetData.push_back(0x00);
        packetData.push_back(0x00);
        packetData.push_back(0x01);
        packetData.insert(packetData.end(), pps_.begin(), pps_.end());
    } else {
        packetData.reserve(nal.size + 4);
    }

    // Add start code
    packetData.push_back(0x00);
    packetData.push_back(0x00);
    packetData.push_back(0x00);
    packetData.push_back(0x01);

    // Add NAL data
    packetData.insert(packetData.end(), nal.data, nal.data + nal.size);

    // Set packet data
    av_packet_unref(packet_);
    packet_->data = packetData.data();
    packet_->size = static_cast<int>(packetData.size());
    packet_->pts = static_cast<int64_t>(timestamp);
    packet_->dts = packet_->pts;

    // Send packet to decoder
    int ret = avcodec_send_packet(codecCtx_, packet_);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
            // Decoder is full, try to receive frames first
        } else if (ret == AVERROR_EOF) {
            return true;
        } else {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Logger::instance().errorf("Failed to send packet: %s", errbuf);
            stats_.decodeErrors++;
            return false;
        }
    }

    // Receive decoded frames
    while (ret >= 0) {
        ret = avcodec_receive_frame(codecCtx_, frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Logger::instance().errorf("Failed to receive frame: %s", errbuf);
            stats_.decodeErrors++;
            return false;
        }

        processDecodedFrame(frame_, timestamp);
        av_frame_unref(frame_);
    }

    return true;
}

void VideoDecoder::processDecodedFrame(AVFrame* frame, uint64_t timestamp) {
    // Update dimensions if changed
    if (width_ != frame->width || height_ != frame->height) {
        width_ = frame->width;
        height_ = frame->height;
        Logger::instance().infof("Video dimensions: %dx%d", width_, height_);

        // Reinitialize scaler for new dimensions
        if (!initScaler(width_, height_)) {
            LOG_ERROR("Failed to initialize scaler for new dimensions");
            return;
        }
    }

    stats_.framesDecoded++;

    // Convert pixel format if needed
    AVFrame* outputFrame = frame;
    if (swsCtx_ && convertedFrame_) {
        int ret = av_frame_make_writable(convertedFrame_);
        if (ret < 0) {
            LOG_ERROR("Failed to make converted frame writable");
            return;
        }

        sws_scale(
            swsCtx_,
            frame->data, frame->linesize,
            0, frame->height,
            convertedFrame_->data, convertedFrame_->linesize
        );

        outputFrame = convertedFrame_;
    }

    // Build output frame
    if (onDecodedFrame_) {
        DecodedFrame decodedFrame;
        decodedFrame.width = width_;
        decodedFrame.height = height_;
        decodedFrame.timestamp = timestamp;
        decodedFrame.format = config_.outputFormat;

        // Calculate output size and stride
        if (config_.outputFormat == OutputPixelFormat::BGRA ||
            config_.outputFormat == OutputPixelFormat::UYVY) {
            // Packed formats
            decodedFrame.stride = outputFrame->linesize[0];
            size_t dataSize = static_cast<size_t>(decodedFrame.stride) * height_;
            decodedFrame.data.resize(dataSize);
            std::memcpy(decodedFrame.data.data(), outputFrame->data[0], dataSize);
        } else {
            // Planar formats (NV12, I420)
            decodedFrame.stride = outputFrame->linesize[0];

            if (config_.outputFormat == OutputPixelFormat::NV12) {
                // Y plane + interleaved UV plane
                size_t ySize = static_cast<size_t>(outputFrame->linesize[0]) * height_;
                size_t uvSize = static_cast<size_t>(outputFrame->linesize[1]) * height_ / 2;
                decodedFrame.data.resize(ySize + uvSize);
                std::memcpy(decodedFrame.data.data(), outputFrame->data[0], ySize);
                std::memcpy(decodedFrame.data.data() + ySize, outputFrame->data[1], uvSize);
            } else {
                // I420: Y + U + V planes
                size_t ySize = static_cast<size_t>(outputFrame->linesize[0]) * height_;
                size_t uSize = static_cast<size_t>(outputFrame->linesize[1]) * height_ / 2;
                size_t vSize = static_cast<size_t>(outputFrame->linesize[2]) * height_ / 2;
                decodedFrame.data.resize(ySize + uSize + vSize);
                std::memcpy(decodedFrame.data.data(), outputFrame->data[0], ySize);
                std::memcpy(decodedFrame.data.data() + ySize, outputFrame->data[1], uSize);
                std::memcpy(decodedFrame.data.data() + ySize + uSize, outputFrame->data[2], vSize);
            }
        }

        onDecodedFrame_(decodedFrame);
    }
}

void VideoDecoder::flush() {
    if (!configured_) return;

    // Send flush signal
    avcodec_send_packet(codecCtx_, nullptr);

    // Receive remaining frames
    int ret;
    while ((ret = avcodec_receive_frame(codecCtx_, frame_)) >= 0) {
        processDecodedFrame(frame_, 0);
        av_frame_unref(frame_);
    }

    LOG_DEBUG("Decoder flushed");
}

void VideoDecoder::reset() {
    if (codecCtx_) {
        avcodec_flush_buffers(codecCtx_);
    }
    sps_.clear();
    pps_.clear();
    decoderReady_ = false;
    LOG_DEBUG("Decoder reset");
}

} // namespace ndi_bridge
