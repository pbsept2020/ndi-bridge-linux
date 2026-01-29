#pragma once

/**
 * VideoDecoder.h - H.264 video decoder using FFmpeg
 *
 * Decodes H.264 Annex-B encoded video to raw pixel buffers.
 * Automatically detects SPS/PPS and creates decoder context.
 */

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>

// Forward declarations for FFmpeg types
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace ndi_bridge {

/**
 * Supported output pixel formats
 */
enum class OutputPixelFormat {
    BGRA,       // 32-bit BGRA (NDI default on some platforms)
    UYVY,       // 16-bit packed YUV 4:2:2 (NDI native)
    NV12,       // Planar YUV 4:2:0
    I420        // Planar YUV 4:2:0 (alias YUV420P)
};

/**
 * Decoder configuration
 */
struct VideoDecoderConfig {
    OutputPixelFormat outputFormat = OutputPixelFormat::BGRA;
    bool useHardwareAccel = false;  // Future: VAAPI, VDPAU, etc.
};

/**
 * Decoded frame data
 */
struct DecodedFrame {
    std::vector<uint8_t> data;      // Raw pixel data
    int width;
    int height;
    int stride;                     // Line stride in bytes
    uint64_t timestamp;             // PTS in 10M ticks/sec
    OutputPixelFormat format;
};

/**
 * Callback types
 */
using OnDecodedFrame = std::function<void(const DecodedFrame& frame)>;
using OnDecoderError = std::function<void(const std::string& error)>;

/**
 * VideoDecoder - H.264 decoder using FFmpeg
 *
 * Features:
 * - Software decoding with FFmpeg
 * - Automatic SPS/PPS detection from Annex-B stream
 * - Automatic pixel format conversion to BGRA
 * - Handles fragmented NAL units
 */
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    // Non-copyable
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    /**
     * Configure the decoder
     * @return true if configuration successful
     */
    bool configure(const VideoDecoderConfig& config = VideoDecoderConfig());

    /**
     * Check if decoder is ready (has received SPS/PPS)
     */
    bool isReady() const { return decoderReady_; }

    /**
     * Get video dimensions (valid after first frame decoded)
     */
    int getWidth() const { return width_; }
    int getHeight() const { return height_; }

    /**
     * Decode H.264 Annex-B data
     * @param data H.264 Annex-B encoded data (with start codes)
     * @param size Data size in bytes
     * @param timestamp PTS in 10M ticks/sec (NDI timestamp format)
     * @return true if decoding started (result via callback)
     */
    bool decode(const uint8_t* data, size_t size, uint64_t timestamp);

    /**
     * Flush decoder (get any pending frames)
     */
    void flush();

    /**
     * Reset decoder state
     */
    void reset();

    /**
     * Get current configuration
     */
    const VideoDecoderConfig& getConfig() const { return config_; }

    /**
     * Set callbacks
     */
    void setOnDecodedFrame(OnDecodedFrame callback) { onDecodedFrame_ = std::move(callback); }
    void setOnError(OnDecoderError callback) { onError_ = std::move(callback); }

    /**
     * Get statistics
     */
    struct Stats {
        uint64_t framesDecoded = 0;
        uint64_t keyframesDecoded = 0;
        uint64_t decodeErrors = 0;
    };
    Stats getStats() const { return stats_; }

private:
    // NAL unit types
    static constexpr uint8_t NAL_TYPE_NON_IDR = 1;
    static constexpr uint8_t NAL_TYPE_IDR = 5;
    static constexpr uint8_t NAL_TYPE_SEI = 6;
    static constexpr uint8_t NAL_TYPE_SPS = 7;
    static constexpr uint8_t NAL_TYPE_PPS = 8;

    struct NALUnit {
        const uint8_t* data;
        size_t size;
        uint8_t type;
    };

    bool initDecoder();
    bool initScaler(int width, int height, int srcPixelFormat);
    void cleanup();
    std::vector<NALUnit> parseNALUnits(const uint8_t* data, size_t size);
    bool processNALUnit(const NALUnit& nal, uint64_t timestamp);
    bool decodeNALUnit(const NALUnit& nal, uint64_t timestamp);
    void processDecodedFrame(AVFrame* frame, uint64_t timestamp);

    VideoDecoderConfig config_;
    bool configured_ = false;
    bool decoderReady_ = false;
    int width_ = 0;
    int height_ = 0;

    // Parameter sets
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;

    // FFmpeg contexts
    AVCodecContext* codecCtx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* convertedFrame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* swsCtx_ = nullptr;

    // Callbacks
    OnDecodedFrame onDecodedFrame_;
    OnDecoderError onError_;

    // Statistics
    Stats stats_;
};

} // namespace ndi_bridge
