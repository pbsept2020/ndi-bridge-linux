#pragma once

/**
 * NDISender.h - NDI video/audio sender
 *
 * Broadcasts video and audio frames as an NDI source on the local network.
 * Equivalent to ndi-bridge-mac/Sources/NDIBridge/Join/NDISender.swift
 */

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>

namespace ndi_bridge {

/**
 * NDI Sender configuration
 */
struct NDISenderConfig {
    std::string sourceName = "NDI Bridge Output";
    int width = 1920;
    int height = 1080;
    int frameRateN = 30000;     // Frame rate numerator
    int frameRateD = 1001;      // Frame rate denominator (30000/1001 = 29.97fps)
};

/**
 * Supported video formats for sending
 */
enum class NDIVideoFormat {
    BGRA,       // 32-bit BGRA
    UYVY,       // 16-bit packed YUV 4:2:2
    I420        // Planar YUV 4:2:0 (3 planes: Y, U, V)
};

/**
 * Callback types
 */
using OnNDISenderStarted = std::function<void(const std::string& name)>;
using OnNDISenderError = std::function<void(const std::string& error)>;

/**
 * NDISender - Broadcasts video/audio as NDI source
 *
 * Features:
 * - Configurable source name
 * - Supports BGRA and UYVY video formats
 * - PCM 32-bit float planar audio
 * - Automatic frame rate detection from timestamps
 */
class NDISender {
public:
    explicit NDISender(const std::string& sourceName = "NDI Bridge Output");
    ~NDISender();

    // Non-copyable
    NDISender(const NDISender&) = delete;
    NDISender& operator=(const NDISender&) = delete;

    /**
     * Initialize NDI SDK (must be called before start)
     * Can be called multiple times safely
     */
    static bool initializeNDI();

    /**
     * Cleanup NDI SDK
     */
    static void destroyNDI();

    /**
     * Start the NDI sender
     * @param width Initial video width
     * @param height Initial video height
     * @param frameRate Initial frame rate (e.g., 29.97, 59.94)
     * @return true if started successfully
     */
    bool start(int width = 1920, int height = 1080, double frameRate = 29.97);

    /**
     * Stop the NDI sender
     */
    void stop();

    /**
     * Check if sender is running
     */
    bool isRunning() const { return running_; }

    /**
     * Send a video frame
     * @param data Raw pixel data
     * @param width Frame width
     * @param height Frame height
     * @param stride Line stride in bytes
     * @param format Pixel format
     * @param timestamp PTS in 10M ticks/sec (NDI timestamp format)
     * @return true if sent successfully
     */
    bool sendVideo(const uint8_t* data, int width, int height, int stride,
                   NDIVideoFormat format, uint64_t timestamp);

    /**
     * Send an audio frame
     * @param data PCM 32-bit float planar audio data
     * @param size Data size in bytes
     * @param sampleRate Sample rate (typically 48000)
     * @param channels Number of channels (typically 2)
     * @param numSamples Number of samples per channel
     * @param timestamp PTS in 10M ticks/sec
     * @return true if sent successfully
     */
    bool sendAudio(const uint8_t* data, size_t size, uint32_t sampleRate,
                   uint8_t channels, int numSamples, uint64_t timestamp);

    /**
     * Update source name (requires restart)
     */
    void setSourceName(const std::string& name);

    /**
     * Get source name
     */
    const std::string& getSourceName() const { return sourceName_; }

    /**
     * Set callbacks
     */
    void setOnStarted(OnNDISenderStarted callback) { onStarted_ = std::move(callback); }
    void setOnError(OnNDISenderError callback) { onError_ = std::move(callback); }

    /**
     * Get statistics
     */
    struct Stats {
        uint64_t videoFramesSent = 0;
        uint64_t audioFramesSent = 0;
    };
    Stats getStats() const { return stats_; }

private:
    void detectFrameRate(uint64_t timestamp);

    std::string sourceName_;
    NDISenderConfig config_;
    std::atomic<bool> running_{false};

    // NDI handles (opaque)
    void* sender_ = nullptr;

    // Double-buffer for async video send (NDI keeps reference until next send)
    std::vector<uint8_t> asyncVideoBuf_[2];
    int currentBuf_ = 0;

    // Frame rate detection
    uint64_t lastTimestamp_ = 0;
    std::vector<double> frameIntervals_;
    bool frameRateDetected_ = false;

    // Statistics
    Stats stats_;

    // Callbacks
    OnNDISenderStarted onStarted_;
    OnNDISenderError onError_;
};

} // namespace ndi_bridge
