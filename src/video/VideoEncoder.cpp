#include "video/VideoEncoder.h"
#include "common/Logger.h"
#include "common/Protocol.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace ndi_bridge {

// Map our PixelFormat to FFmpeg's
static AVPixelFormat toAVPixelFormat(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::BGRA: return AV_PIX_FMT_BGRA;
        case PixelFormat::UYVY: return AV_PIX_FMT_UYVY422;
        case PixelFormat::NV12: return AV_PIX_FMT_NV12;
        case PixelFormat::I420: return AV_PIX_FMT_YUV420P;
    }
    return AV_PIX_FMT_NONE;
}

static const char* pixelFormatName(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::BGRA: return "BGRA";
        case PixelFormat::UYVY: return "UYVY";
        case PixelFormat::NV12: return "NV12";
        case PixelFormat::I420: return "I420";
    }
    return "Unknown";
}

VideoEncoder::VideoEncoder() {
    LOG_DEBUG("VideoEncoder initialized");
}

VideoEncoder::~VideoEncoder() {
    cleanup();
    LOG_DEBUG("VideoEncoder destroyed");
}

bool VideoEncoder::configure(const VideoEncoderConfig& config) {
    // Cleanup existing encoder
    cleanup();

    config_ = config;

    Logger::instance().infof("Configuring encoder: %dx%d @ %d fps, %d Mbps, input=%s",
                             config.width, config.height, config.fps,
                             config.bitrate / 1000000, pixelFormatName(config.inputFormat));

    if (!initEncoder()) {
        LOG_ERROR("Failed to initialize encoder");
        return false;
    }

    if (!initScaler()) {
        LOG_ERROR("Failed to initialize pixel format converter");
        cleanup();
        return false;
    }

    configured_ = true;
    LOG_SUCCESS("Encoder configured successfully");
    return true;
}

bool VideoEncoder::initEncoder() {
    const AVCodec* codec = nullptr;
    hwAccelActive_ = false;

#ifdef __APPLE__
    // Try VideoToolbox hardware encoder first on macOS
    if (config_.useHardwareAccel) {
        codec = avcodec_find_encoder_by_name("h264_videotoolbox");
        if (codec) {
            Logger::instance().info("Trying hardware encoder: h264_videotoolbox");
            hwAccelActive_ = true;
        }
    }
#endif

    if (!codec) {
        // Fall back to libx264 software encoder
        codec = avcodec_find_encoder_by_name("libx264");
        if (!codec) {
            codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        }
    }

    if (!codec) {
        LOG_ERROR("H.264 encoder not found");
        if (onError_) onError_("H.264 encoder not found");
        return false;
    }

    Logger::instance().infof("Using encoder: %s", codec->name);

    // Allocate codec context
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        LOG_ERROR("Failed to allocate codec context");
        return false;
    }

    // Configure codec — common settings
    codecCtx_->width = config_.width;
    codecCtx_->height = config_.height;
    codecCtx_->time_base = AVRational{1, static_cast<int>(TIMESTAMP_RESOLUTION)};  // 10M ticks/sec
    codecCtx_->framerate = AVRational{config_.fps, 1};
    codecCtx_->color_range = AVCOL_RANGE_JPEG;  // Full range (0-255) — NDI is full range
    codecCtx_->colorspace = AVCOL_SPC_BT709;
    codecCtx_->color_primaries = AVCOL_PRI_BT709;
    codecCtx_->color_trc = AVCOL_TRC_BT709;
    codecCtx_->bit_rate = config_.bitrate;
    codecCtx_->gop_size = config_.keyframeInterval;
    codecCtx_->max_b_frames = 0;  // No B-frames for low latency
    codecCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    AVDictionary* opts = nullptr;
    int ret;

    if (hwAccelActive_) {
        // VideoToolbox settings
        codecCtx_->pix_fmt = AV_PIX_FMT_NV12;  // VideoToolbox prefers NV12
        av_dict_set(&opts, "realtime", "1", 0);
        av_dict_set(&opts, "allow_sw", "1", 0);  // Fall back to software if HW unavailable
        av_dict_set(&opts, "profile", "high", 0);
        av_dict_set(&opts, "level", "4.1", 0);

        ret = avcodec_open2(codecCtx_, codec, &opts);
        av_dict_free(&opts);

        if (ret < 0) {
            // VideoToolbox failed — fall back to libx264
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Logger::instance().infof("VideoToolbox failed (%s), falling back to libx264", errbuf);

            avcodec_free_context(&codecCtx_);
            hwAccelActive_ = false;

            codec = avcodec_find_encoder_by_name("libx264");
            if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
            if (!codec) {
                LOG_ERROR("H.264 encoder not found (fallback)");
                return false;
            }

            codecCtx_ = avcodec_alloc_context3(codec);
            if (!codecCtx_) return false;

            // Re-apply common settings
            codecCtx_->width = config_.width;
            codecCtx_->height = config_.height;
            codecCtx_->time_base = AVRational{1, static_cast<int>(TIMESTAMP_RESOLUTION)};
            codecCtx_->framerate = AVRational{config_.fps, 1};
            codecCtx_->color_range = AVCOL_RANGE_JPEG;
            codecCtx_->colorspace = AVCOL_SPC_BT709;
            codecCtx_->color_primaries = AVCOL_PRI_BT709;
            codecCtx_->color_trc = AVCOL_TRC_BT709;
            codecCtx_->bit_rate = config_.bitrate;
            codecCtx_->gop_size = config_.keyframeInterval;
            codecCtx_->max_b_frames = 0;
            codecCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            // Fall through to x264 setup below
        }
    }

    if (!hwAccelActive_) {
        // libx264 settings
        codecCtx_->pix_fmt = AV_PIX_FMT_YUV420P;
        codecCtx_->rc_max_rate = config_.bitrate * 3 / 2;
        codecCtx_->rc_buffer_size = config_.bitrate / config_.fps;
        codecCtx_->thread_count = 0;

        opts = nullptr;
        av_dict_set(&opts, "preset", config_.preset.c_str(), 0);
        av_dict_set(&opts, "tune", config_.tune.c_str(), 0);
        av_dict_set(&opts, "profile", config_.profile.c_str(), 0);
        av_dict_set(&opts, "colorprim", "bt709", 0);
        av_dict_set(&opts, "transfer", "bt709", 0);
        av_dict_set(&opts, "colormatrix", "bt709", 0);
        av_dict_set(&opts, "fullrange", "on", 0);
        av_dict_set(&opts, "rc-lookahead", "0", 0);
        av_dict_set(&opts, "sync-lookahead", "0", 0);
        // NOTE: Do NOT set sliced-threads=0 here — tune=zerolatency enables
        // sliced-threads=1 which gives multi-thread performance with ZERO added
        // frame latency. Disabling it forces frame-level threading that adds
        // N-1 frames of buffering delay.

        ret = avcodec_open2(codecCtx_, codec, &opts);
        av_dict_free(&opts);

        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Logger::instance().errorf("Failed to open encoder: %s", errbuf);
            if (onError_) onError_(std::string("Failed to open encoder: ") + errbuf);
            return false;
        }
    }

    Logger::instance().successf("Encoder opened: %s (%s)", codec->name,
                                 hwAccelActive_ ? "hardware" : "software");

    // Allocate frame for input
    frame_ = av_frame_alloc();
    if (!frame_) {
        LOG_ERROR("Failed to allocate frame");
        return false;
    }

    frame_->format = codecCtx_->pix_fmt;
    frame_->width = codecCtx_->width;
    frame_->height = codecCtx_->height;

    ret = av_frame_get_buffer(frame_, 0);
    if (ret < 0) {
        LOG_ERROR("Failed to allocate frame buffer");
        return false;
    }

    // Allocate packet for output
    packet_ = av_packet_alloc();
    if (!packet_) {
        LOG_ERROR("Failed to allocate packet");
        return false;
    }

    return true;
}

bool VideoEncoder::initScaler() {
    AVPixelFormat srcFormat = toAVPixelFormat(config_.inputFormat);
    AVPixelFormat dstFormat = codecCtx_->pix_fmt;  // NV12 for VideoToolbox, YUV420P for x264

    // If input matches encoder format, no scaling needed
    if (srcFormat == dstFormat) {
        LOG_DEBUG("No pixel format conversion needed");
        return true;
    }

    Logger::instance().debugf("Creating scaler: %s -> %s",
                              pixelFormatName(config_.inputFormat),
                              av_get_pix_fmt_name(dstFormat));

    swsCtx_ = sws_getContext(
        config_.width, config_.height, srcFormat,
        config_.width, config_.height, dstFormat,
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
    convertedFrame_->width = config_.width;
    convertedFrame_->height = config_.height;

    int ret = av_frame_get_buffer(convertedFrame_, 0);
    if (ret < 0) {
        LOG_ERROR("Failed to allocate converted frame buffer");
        return false;
    }

    return true;
}

void VideoEncoder::cleanup() {
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
    hwAccelActive_ = false;
    frameNumber_ = 0;
    stats_ = Stats{};
}

bool VideoEncoder::encode(const uint8_t* data, size_t size, uint64_t timestamp) {
    (void)size;  // Size used for validation, stride calculated from config
    // Calculate default stride based on pixel format
    int stride;
    switch (config_.inputFormat) {
        case PixelFormat::BGRA:
            stride = config_.width * 4;
            break;
        case PixelFormat::UYVY:
            stride = config_.width * 2;
            break;
        case PixelFormat::NV12:
        case PixelFormat::I420:
            stride = config_.width;
            break;
        default:
            stride = config_.width * 4;
    }

    return encodeWithStride(data, stride, timestamp);
}

bool VideoEncoder::encodeWithStride(const uint8_t* data, int stride, uint64_t timestamp) {
    if (!configured_) {
        LOG_ERROR("Encoder not configured");
        return false;
    }

    // Convert pixel format if necessary
    AVFrame* frameToEncode = frame_;

    if (swsCtx_) {
        if (!convertPixelFormat(data, stride)) {
            LOG_ERROR("Pixel format conversion failed");
            return false;
        }
        frameToEncode = convertedFrame_;
    } else {
        // Direct copy for YUV420P input
        int ret = av_frame_make_writable(frame_);
        if (ret < 0) {
            LOG_ERROR("Failed to make frame writable");
            return false;
        }

        // Copy data to frame
        const uint8_t* srcSlice[4] = {data, nullptr, nullptr, nullptr};
        int srcStride[4] = {stride, 0, 0, 0};

        av_image_copy(frame_->data, frame_->linesize,
                      srcSlice, srcStride,
                      static_cast<AVPixelFormat>(frame_->format),
                      frame_->width, frame_->height);
    }

    // Set timestamp
    frameToEncode->pts = static_cast<int64_t>(timestamp);

    // Force keyframe if requested
    if (forceNextKeyframe_ || frameNumber_ == 0 ||
        (config_.keyframeInterval > 0 && frameNumber_ % config_.keyframeInterval == 0)) {
        frameToEncode->pict_type = AV_PICTURE_TYPE_I;
        forceNextKeyframe_ = false;
    } else {
        frameToEncode->pict_type = AV_PICTURE_TYPE_NONE;
    }

    frameNumber_++;

    // Send frame to encoder
    int ret = avcodec_send_frame(codecCtx_, frameToEncode);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Logger::instance().errorf("Failed to send frame: %s", errbuf);
        return false;
    }

    // Receive encoded packets
    while (ret >= 0) {
        ret = avcodec_receive_packet(codecCtx_, packet_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Logger::instance().errorf("Failed to receive packet: %s", errbuf);
            return false;
        }

        processEncodedPacket(packet_);
        av_packet_unref(packet_);
    }

    return true;
}

bool VideoEncoder::convertPixelFormat(const uint8_t* srcData, int srcStride) {
    int ret = av_frame_make_writable(convertedFrame_);
    if (ret < 0) {
        return false;
    }

    // Setup source data pointers based on pixel format
    const uint8_t* srcSlice[4] = {nullptr, nullptr, nullptr, nullptr};
    int srcStrides[4] = {0, 0, 0, 0};

    switch (config_.inputFormat) {
        case PixelFormat::BGRA:
            srcSlice[0] = srcData;
            srcStrides[0] = srcStride;
            break;

        case PixelFormat::UYVY:
            srcSlice[0] = srcData;
            srcStrides[0] = srcStride;
            break;

        case PixelFormat::NV12:
            srcSlice[0] = srcData;
            srcSlice[1] = srcData + config_.width * config_.height;
            srcStrides[0] = srcStride;
            srcStrides[1] = srcStride;
            break;

        case PixelFormat::I420:
            srcSlice[0] = srcData;
            srcSlice[1] = srcData + config_.width * config_.height;
            srcSlice[2] = srcData + config_.width * config_.height * 5 / 4;
            srcStrides[0] = srcStride;
            srcStrides[1] = srcStride / 2;
            srcStrides[2] = srcStride / 2;
            break;
    }

    // Convert
    int height = sws_scale(
        swsCtx_,
        srcSlice, srcStrides,
        0, config_.height,
        convertedFrame_->data, convertedFrame_->linesize
    );

    return height == config_.height;
}

void VideoEncoder::processEncodedPacket(AVPacket* packet) {
    bool isKeyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;

    // Build Annex-B data
    std::vector<uint8_t> annexBData;
    annexBData.reserve(packet->size + 256);  // Extra space for SPS/PPS

    // For keyframes, prepend SPS/PPS from extradata
    if (isKeyframe && codecCtx_->extradata && codecCtx_->extradata_size > 0) {
        const uint8_t* extra = codecCtx_->extradata;
        int extraSize = codecCtx_->extradata_size;

        if (extraSize > 7 && extra[0] == 1) {
            // AVCC format - parse and convert to Annex-B
            int numSPS = extra[5] & 0x1F;
            int offset = 6;

            // Extract SPS
            for (int i = 0; i < numSPS && offset + 2 < extraSize; i++) {
                int spsLen = (extra[offset] << 8) | extra[offset + 1];
                offset += 2;

                if (offset + spsLen <= extraSize) {
                    // Add Annex-B start code
                    annexBData.push_back(0x00);
                    annexBData.push_back(0x00);
                    annexBData.push_back(0x00);
                    annexBData.push_back(0x01);
                    annexBData.insert(annexBData.end(), extra + offset, extra + offset + spsLen);
                    offset += spsLen;
                }
            }

            // Extract PPS
            if (offset < extraSize) {
                int numPPS = extra[offset++];
                for (int i = 0; i < numPPS && offset + 2 < extraSize; i++) {
                    int ppsLen = (extra[offset] << 8) | extra[offset + 1];
                    offset += 2;

                    if (offset + ppsLen <= extraSize) {
                        annexBData.push_back(0x00);
                        annexBData.push_back(0x00);
                        annexBData.push_back(0x00);
                        annexBData.push_back(0x01);
                        annexBData.insert(annexBData.end(), extra + offset, extra + offset + ppsLen);
                        offset += ppsLen;
                    }
                }
            }
        } else if (extraSize >= 4 && extra[0] == 0 && extra[1] == 0 &&
                   (extra[2] == 1 || (extra[2] == 0 && extra[3] == 1))) {
            // Already Annex-B format - just copy the extradata
            annexBData.insert(annexBData.end(), extra, extra + extraSize);
        }
    }

    // Convert packet data from AVCC to Annex-B
    // libx264 outputs in Annex-B by default when AV_CODEC_FLAG_GLOBAL_HEADER is set
    // But let's handle both cases
    const uint8_t* data = packet->data;
    int size = packet->size;

    // Check if already Annex-B (starts with 00 00 00 01 or 00 00 01)
    bool isAnnexB = (size >= 4 && data[0] == 0 && data[1] == 0 &&
                    (data[2] == 1 || (data[2] == 0 && data[3] == 1)));

    if (isAnnexB) {
        // Already Annex-B, just append
        annexBData.insert(annexBData.end(), data, data + size);
    } else {
        // AVCC format - convert to Annex-B
        int offset = 0;
        while (offset + 4 <= size) {
            // Read NAL unit length (4 bytes, big-endian)
            uint32_t nalLen = (static_cast<uint32_t>(data[offset]) << 24) |
                              (static_cast<uint32_t>(data[offset + 1]) << 16) |
                              (static_cast<uint32_t>(data[offset + 2]) << 8) |
                              static_cast<uint32_t>(data[offset + 3]);
            offset += 4;

            if (offset + nalLen > static_cast<uint32_t>(size)) break;

            // Add Annex-B start code
            annexBData.push_back(0x00);
            annexBData.push_back(0x00);
            annexBData.push_back(0x00);
            annexBData.push_back(0x01);

            // Add NAL unit
            annexBData.insert(annexBData.end(), data + offset, data + offset + nalLen);
            offset += nalLen;
        }
    }

    // Update stats
    stats_.framesEncoded++;
    stats_.bytesEncoded += annexBData.size();
    if (isKeyframe) {
        stats_.keyframesEncoded++;
    }

    // Call callback
    if (onEncodedFrame_) {
        EncodedFrame frame;
        frame.data = std::move(annexBData);
        frame.isKeyframe = isKeyframe;
        frame.timestamp = static_cast<uint64_t>(packet->pts);
        frame.duration = static_cast<uint64_t>(packet->duration);
        onEncodedFrame_(frame);
    }
}

void VideoEncoder::forceKeyframe() {
    forceNextKeyframe_ = true;
    LOG_DEBUG("Keyframe forced for next frame");
}

void VideoEncoder::flush() {
    if (!configured_) return;

    // Send flush signal
    avcodec_send_frame(codecCtx_, nullptr);

    // Receive remaining packets
    int ret;
    while ((ret = avcodec_receive_packet(codecCtx_, packet_)) >= 0) {
        processEncodedPacket(packet_);
        av_packet_unref(packet_);
    }

    LOG_DEBUG("Encoder flushed");
}

void VideoEncoder::reset() {
    if (codecCtx_) {
        avcodec_flush_buffers(codecCtx_);
    }
    frameNumber_ = 0;
    forceNextKeyframe_ = true;  // Force keyframe on next encode
    LOG_DEBUG("Encoder reset");
}

} // namespace ndi_bridge
