#pragma once

/**
 * NetworkReceiver.h - UDP receiver for NDI Bridge
 *
 * Receives video and audio packets over UDP and reassembles fragmented frames.
 * Compatible with macOS Swift NetworkSender and Node.js sender.
 */

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>

#include "common/Protocol.h"

namespace ndi_bridge {

/**
 * Configuration for NetworkReceiver
 */
struct NetworkReceiverConfig {
    uint16_t port = 5990;
    size_t recvBufferSize = 8 * 1024 * 1024;  // 8MB receive buffer
};

/**
 * Statistics for NetworkReceiver
 */
struct NetworkReceiverStats {
    uint64_t bytesReceived = 0;
    uint64_t packetsReceived = 0;
    uint64_t videoFramesReceived = 0;
    uint64_t audioFramesReceived = 0;
    uint64_t framesDropped = 0;
    uint64_t invalidPackets = 0;
    // One-way latency estimate (send timestamp based)
    int64_t  latencySumMs = 0;
    uint64_t latencyCount = 0;
};

/**
 * Received video frame
 */
struct ReceivedVideoFrame {
    std::vector<uint8_t> data;
    uint64_t timestamp;
    bool isKeyframe;
    uint32_t sequenceNumber;
};

/**
 * Received audio frame
 */
struct ReceivedAudioFrame {
    std::vector<uint8_t> data;
    uint64_t timestamp;
    uint32_t sampleRate;
    uint8_t channels;
    uint32_t sequenceNumber;
};

/**
 * Callback types
 */
using OnVideoFrame = std::function<void(const ReceivedVideoFrame& frame)>;
using OnAudioFrame = std::function<void(const ReceivedAudioFrame& frame)>;
using OnReceiverError = std::function<void(const std::string& error)>;
using OnReceiverStats = std::function<void(const NetworkReceiverStats& stats)>;

/**
 * NetworkReceiver - Receive video/audio frames over UDP
 *
 * Uses POSIX sockets for cross-platform compatibility.
 * Automatically reassembles fragmented frames.
 * Runs a dedicated receive thread.
 */
class NetworkReceiver {
public:
    explicit NetworkReceiver(const NetworkReceiverConfig& config = NetworkReceiverConfig());
    ~NetworkReceiver();

    // Non-copyable
    NetworkReceiver(const NetworkReceiver&) = delete;
    NetworkReceiver& operator=(const NetworkReceiver&) = delete;

    /**
     * Start listening for packets
     * @return true if listening started successfully
     */
    bool startListening();

    /**
     * Start listening on specific port
     */
    bool startListening(uint16_t port);

    /**
     * Stop listening
     */
    void stop();

    /**
     * Check if listening
     */
    bool isListening() const { return listening_; }

    /**
     * Get current statistics
     */
    NetworkReceiverStats getStats() const;

    /**
     * Get video reassembler stats (fragment-level diagnostics)
     */
    FrameReassembler::Stats getVideoReassemblerStats() const { return videoReassembler_.getStats(); }

    /**
     * Get audio reassembler stats
     */
    FrameReassembler::Stats getAudioReassemblerStats() const { return audioReassembler_.getStats(); }

    /**
     * Reset statistics
     */
    void resetStats();

    /**
     * Set callbacks
     */
    void setOnVideoFrame(OnVideoFrame callback) { onVideoFrame_ = std::move(callback); }
    void setOnAudioFrame(OnAudioFrame callback) { onAudioFrame_ = std::move(callback); }
    void setOnError(OnReceiverError callback) { onError_ = std::move(callback); }
    void setOnStats(OnReceiverStats callback) { onStats_ = std::move(callback); }

    /**
     * Get current configuration
     */
    const NetworkReceiverConfig& getConfig() const { return config_; }

private:
    void receiveLoop();
    void processPacket(const uint8_t* data, size_t size, uint64_t recvTimestampNs);

    NetworkReceiverConfig config_;
    int socket_ = -1;
    std::atomic<bool> listening_{false};
    std::atomic<bool> shouldStop_{false};
    std::thread receiveThread_;

    // Separate reassemblers for video and audio
    FrameReassembler videoReassembler_;
    FrameReassembler audioReassembler_;

    // Statistics
    mutable std::mutex statsMutex_;
    NetworkReceiverStats stats_;

    // Callbacks
    OnVideoFrame onVideoFrame_;
    OnAudioFrame onAudioFrame_;
    OnReceiverError onError_;
    OnReceiverStats onStats_;
};

} // namespace ndi_bridge
