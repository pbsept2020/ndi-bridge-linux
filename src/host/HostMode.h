#pragma once

/**
 * HostMode.h - NDI Bridge Host Mode Orchestrator
 *
 * Captures NDI source → Encodes H.264 → Sends over UDP
 * Audio is passed through as PCM (no encoding).
 *
 * Equivalent to ndi-bridge-mac/Sources/NDIBridge/Host/HostMode.swift
 */

#include <string>
#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <thread>

#include "../ndi/NDIReceiver.h"
#include "../video/VideoEncoder.h"
#include "../network/NetworkSender.h"

namespace ndi_bridge {

/**
 * Host mode configuration
 */
struct HostModeConfig {
    std::string targetHost = "127.0.0.1";
    uint16_t targetPort = 5990;
    int bitrateMbps = 8;                    // Video bitrate in Mbps
    size_t mtu = 1400;                      // UDP MTU (reduce for VPN tunnels)
    bool autoSelectFirstSource = false;     // Auto-select first source
    std::string sourceName;                 // Specific source name (empty = interactive)
    std::vector<std::string> excludePatterns = {"Bridge"};  // Patterns to exclude
    int sourceDiscoveryTimeoutMs = 5000;    // Discovery timeout
};

/**
 * HostMode - Main orchestrator for sender mode
 *
 * Pipeline:
 *   NDIReceiver (video) → VideoEncoder → NetworkSender
 *   NDIReceiver (audio) → NetworkSender (passthrough)
 */
class HostMode {
public:
    explicit HostMode(const HostModeConfig& config = HostModeConfig());
    ~HostMode();

    // Non-copyable
    HostMode(const HostMode&) = delete;
    HostMode& operator=(const HostMode&) = delete;

    /**
     * Start host mode
     * @param running Reference to running flag for graceful shutdown
     * @return 0 on success, error code otherwise
     */
    int start(std::atomic<bool>& running);

    /**
     * Stop host mode
     */
    void stop();

    /**
     * Check if running
     */
    bool isRunning() const { return running_; }

    /**
     * List available NDI sources (for external use)
     */
    std::vector<NDISource> listSources();

    /**
     * Get statistics
     */
    struct Stats {
        uint64_t videoFramesReceived = 0;
        uint64_t audioFramesReceived = 0;
        uint64_t videoFramesEncoded = 0;
        uint64_t videoFramesDropped = 0;
        uint64_t bytesSent = 0;
        double runTimeSeconds = 0.0;
    };
    Stats getStats() const;

private:
    // Callbacks wired to components
    void onVideoFrame(const NDIVideoFrame& frame);
    void onAudioFrame(const NDIAudioFrame& frame);
    void onEncodedFrame(const EncodedFrame& frame);
    void onNDIError(const std::string& error);

    // Async encode thread
    void encodeLoop();

    // Source selection helpers
    NDISource selectSource(const std::vector<NDISource>& sources);
    NDISource promptUserSelection(const std::vector<NDISource>& sources);
    bool matchesExcludePattern(const std::string& name) const;

    // Configuration
    HostModeConfig config_;

    // Components
    std::unique_ptr<NDIReceiver> ndiReceiver_;
    std::unique_ptr<VideoEncoder> encoder_;
    std::unique_ptr<NetworkSender> networkSender_;

    // State
    std::atomic<bool> running_{false};
    NDISource selectedSource_;
    bool encoderConfigured_ = false;

    // Async encode queue (bounded, drop-oldest policy)
    static constexpr size_t MAX_QUEUE_SIZE = 3;
    std::queue<NDIVideoFrame> frameQueue_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::thread encodeThread_;

    // Statistics
    std::chrono::steady_clock::time_point startTime_;
    std::atomic<uint64_t> videoFramesReceived_{0};
    std::atomic<uint64_t> audioFramesReceived_{0};
    std::atomic<uint64_t> videoFramesEncoded_{0};
    std::atomic<uint64_t> videoFramesDropped_{0};
};

} // namespace ndi_bridge
