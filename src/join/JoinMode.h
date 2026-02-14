#pragma once

/**
 * JoinMode.h - NDI Bridge Join Mode Orchestrator
 *
 * Receives UDP stream → Decodes H.264 → Outputs as NDI source
 * Audio is passed through as PCM (no decoding).
 *
 * Equivalent to ndi-bridge-mac/Sources/NDIBridge/Join/JoinMode.swift
 */

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

#include "../network/NetworkReceiver.h"
#include "../video/VideoDecoder.h"
#include "../ndi/NDISender.h"

namespace ndi_bridge {

/**
 * Join mode configuration
 */
struct JoinModeConfig {
    uint16_t listenPort = 5990;
    std::string ndiOutputName = "NDI Bridge Output";
    int outputWidth = 1920;
    int outputHeight = 1080;
    int bufferMs = 0;       // 0 = real-time, >0 = delay in ms
};

/**
 * Buffered video frame for delayed playback
 */
struct BufferedVideoFrame {
    std::vector<uint8_t> data;
    int width;
    int height;
    int stride;
    NDIVideoFormat ndiFormat = NDIVideoFormat::I420;
    uint64_t timestamp;
    uint64_t playTime;      // When to play (system clock)
};

/**
 * Buffered audio frame for delayed playback
 */
struct BufferedAudioFrame {
    std::vector<uint8_t> data;
    uint32_t sampleRate;
    uint8_t channels;
    int numSamples;
    uint64_t timestamp;
    uint64_t playTime;      // When to play (system clock)
};

/**
 * JoinMode - Main orchestrator for receiver mode
 *
 * Pipeline:
 *   NetworkReceiver (video) → VideoDecoder → NDISender
 *   NetworkReceiver (audio) → NDISender (passthrough)
 */
class JoinMode {
public:
    explicit JoinMode(const JoinModeConfig& config = JoinModeConfig());
    ~JoinMode();

    // Non-copyable
    JoinMode(const JoinMode&) = delete;
    JoinMode& operator=(const JoinMode&) = delete;

    /**
     * Start join mode
     * @param running Reference to running flag for graceful shutdown
     * @return 0 on success, error code otherwise
     */
    int start(std::atomic<bool>& running);

    /**
     * Stop join mode
     */
    void stop();

    /**
     * Check if running
     */
    bool isRunning() const { return running_; }

    /**
     * Update NDI output name (requires restart)
     */
    void setOutputName(const std::string& name);

    /**
     * Get statistics
     */
    struct Stats {
        uint64_t videoFramesReceived = 0;
        uint64_t audioFramesReceived = 0;
        uint64_t videoFramesDecoded = 0;
        uint64_t videoFramesOutput = 0;
        uint64_t audioFramesOutput = 0;
        double runTimeSeconds = 0.0;
    };
    Stats getStats() const;

private:
    // Callbacks wired to components
    void onVideoFrame(const ReceivedVideoFrame& frame);
    void onAudioFrame(const ReceivedAudioFrame& frame);
    void onDecodedFrame(const DecodedFrame& frame);
    void onNetworkError(const std::string& error);

    // Async decode
    void decodeLoop();
    static constexpr size_t MAX_DECODE_QUEUE = 90; // 3 seconds at 30fps

    // Buffer management
    void bufferOutputLoop();
    void processBufferedFrames();

    // Configuration
    JoinModeConfig config_;

    // Components
    std::unique_ptr<NetworkReceiver> networkReceiver_;
    std::unique_ptr<VideoDecoder> decoder_;
    std::unique_ptr<NDISender> ndiSender_;

    // State
    std::atomic<bool> running_{false};
    bool decoderConfigured_ = false;

    // Async decode queue
    std::queue<ReceivedVideoFrame> decodeQueue_;
    std::mutex decodeQueueMutex_;
    std::condition_variable decodeQueueCv_;
    std::thread decodeThread_;
    std::atomic<bool> decodeRunning_{false};

    // Buffering
    std::mutex videoBufferMutex_;
    std::mutex audioBufferMutex_;
    std::queue<BufferedVideoFrame> videoBuffer_;
    std::queue<BufferedAudioFrame> audioBuffer_;
    std::thread bufferThread_;
    std::atomic<bool> bufferRunning_{false};
    uint64_t bufferStartTime_ = 0;
    uint64_t firstFrameTimestamp_ = 0;
    bool bufferSynced_ = false;

    // Statistics
    std::chrono::steady_clock::time_point startTime_;
    std::atomic<uint64_t> videoFramesReceived_{0};
    std::atomic<uint64_t> audioFramesReceived_{0};
    std::atomic<uint64_t> videoFramesDecoded_{0};
    std::atomic<uint64_t> videoFramesOutput_{0};
    std::atomic<uint64_t> audioFramesOutput_{0};

    // Decode timing
    std::atomic<uint64_t> totalDecodeTimeUs_{0};
    std::atomic<uint64_t> maxDecodeTimeUs_{0};
    std::atomic<uint64_t> decodeCount_{0};
    std::atomic<uint64_t> videoFramesDroppedQueue_{0};
};

} // namespace ndi_bridge
