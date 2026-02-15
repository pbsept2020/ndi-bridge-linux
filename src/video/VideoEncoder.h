#pragma once

/**
 * VideoEncoder.h - H.264 video encoder using FFmpeg/libx264
 *
 * Encodes raw video frames (BGRA, UYVY, or NV12) to H.264 Annex-B format.
 * Optimized for low-latency streaming with ultrafast preset.
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
 * Supported input pixel formats
 */
enum class PixelFormat {
    BGRA,       // 32-bit BGRA (NDI default on some platforms)
    UYVY,       // 16-bit packed YUV 4:2:2 (NDI native)
    NV12,       // Planar YUV 4:2:0 (encoder native)
    I420        // Planar YUV 4:2:0 (alias YUV420P)
};

/**
 * Encoder configuration
 */
struct VideoEncoderConfig {
    int width = 1920;
    int height = 1080;
    int bitrate = 8000000;          // 8 Mbps
    int fps = 60;
    int keyframeInterval = 60;      // Keyframe every N frames (1 second at 60fps)
    PixelFormat inputFormat = PixelFormat::UYVY;

    // x264 specific (ignored for hardware encoders)
    std::string preset = "ultrafast";
    std::string tune = "zerolatency";
    std::string profile = "high";

    // Hardware acceleration
    bool useHardwareAccel = true;   // Try hardware encoder first (VideoToolbox on macOS)

    // Presets
    static VideoEncoderConfig hd1080p60() {
        return VideoEncoderConfig{1920, 1080, 8000000, 60, 60, PixelFormat::UYVY};
    }

    static VideoEncoderConfig hd720p30() {
        return VideoEncoderConfig{1280, 720, 4000000, 30, 30, PixelFormat::UYVY};
    }
};

/**
 * Encoded frame data
 */
struct EncodedFrame {
    std::vector<uint8_t> data;      // H.264 Annex-B data (with start codes)
    bool isKeyframe;
    uint64_t timestamp;             // PTS in 10M ticks/sec
    uint64_t duration;              // Duration in 10M ticks/sec
};

/**
 * Callback types
 */
using OnEncodedFrame = std::function<void(const EncodedFrame& frame)>;
using OnEncoderError = std::function<void(const std::string& error)>;

/**
 * VideoEncoder - H.264 encoder using FFmpeg/libx264
 *
 * Features:
 * - Software encoding with libx264
 * - Automatic pixel format conversion (BGRA/UYVY → NV12)
 * - H.264 Annex-B output with SPS/PPS on keyframes
 * - Low-latency optimized (ultrafast + zerolatency)
 */
class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();

    // Non-copyable
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    /**
     * Configure the encoder
     * @return true if configuration successful
     */
    bool configure(const VideoEncoderConfig& config);

    /**
     * Check if encoder is configured
     */
    bool isConfigured() const { return configured_; }

    /**
     * Encode a video frame
     * @param data Raw pixel data
     * @param size Data size in bytes
     * @param timestamp PTS in 10M ticks/sec (NDI timestamp format)
     * @return true if encoding started (result via callback)
     */
    bool encode(const uint8_t* data, size_t size, uint64_t timestamp);

    /**
     * Encode with explicit stride
     * @param data Raw pixel data
     * @param stride Line stride in bytes
     * @param timestamp PTS in 10M ticks/sec
     */
    bool encodeWithStride(const uint8_t* data, int stride, uint64_t timestamp);

    /**
     * Force next frame to be a keyframe
     */
    void forceKeyframe();

    /**
     * Flush encoder (get any pending frames)
     */
    void flush();

    /**
     * Reset encoder state
     */
    void reset();

    /**
     * Get current configuration
     */
    const VideoEncoderConfig& getConfig() const { return config_; }

    /**
     * Set callbacks
     */
    void setOnEncodedFrame(OnEncodedFrame callback) { onEncodedFrame_ = std::move(callback); }
    void setOnError(OnEncoderError callback) { onError_ = std::move(callback); }

    /**
     * Get statistics
     */
    struct Stats {
        uint64_t framesEncoded = 0;
        uint64_t bytesEncoded = 0;
        uint64_t keyframesEncoded = 0;
    };
    Stats getStats() const { return stats_; }

private:
    bool initEncoder();
    bool initScaler();
    void cleanup();
    bool convertPixelFormat(const uint8_t* srcData, int srcStride);
    void processEncodedPacket(AVPacket* packet);

    VideoEncoderConfig config_;
    bool configured_ = false;
    bool forceNextKeyframe_ = false;
    bool hwAccelActive_ = false;
    uint64_t frameNumber_ = 0;

    // FFmpeg contexts
    AVCodecContext* codecCtx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* swsCtx_ = nullptr;

    // Intermediate buffer for pixel format conversion
    AVFrame* convertedFrame_ = nullptr;

    // Callbacks
    OnEncodedFrame onEncodedFrame_;
    OnEncoderError onError_;

    // Statistics
    Stats stats_;
};

} // namespace ndi_bridge
