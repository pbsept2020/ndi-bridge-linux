#pragma once

/**
 * NetworkSender.h - UDP sender for NDI Bridge
 *
 * Sends video and audio frames over UDP with automatic fragmentation.
 * Compatible with macOS Swift NetworkSender and Node.js receiver.
 */

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>

namespace ndi_bridge {

/**
 * Configuration for NetworkSender
 */
struct NetworkSenderConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 5990;
    size_t mtu = 1400;  // Safe MTU for UDP payload
};

/**
 * Statistics for NetworkSender
 */
struct NetworkSenderStats {
    uint64_t bytesSent = 0;
    uint64_t packetsSent = 0;
    uint64_t framesSent = 0;
    uint64_t sendErrors = 0;
};

/**
 * Callback types
 */
using OnSenderConnected = std::function<void(const std::string& endpoint)>;
using OnSenderError = std::function<void(const std::string& error)>;
using OnSenderStats = std::function<void(const NetworkSenderStats& stats)>;

/**
 * NetworkSender - Send video/audio frames over UDP
 *
 * Uses POSIX sockets for cross-platform compatibility.
 * Automatically fragments large frames to fit within MTU.
 */
class NetworkSender {
public:
    explicit NetworkSender(const NetworkSenderConfig& config = NetworkSenderConfig());
    ~NetworkSender();

    // Non-copyable
    NetworkSender(const NetworkSender&) = delete;
    NetworkSender& operator=(const NetworkSender&) = delete;

    /**
     * Connect to target endpoint
     * For UDP this just sets up the socket and destination address.
     * @return true if socket created successfully
     */
    bool connect();

    /**
     * Connect to specific host and port
     */
    bool connect(const std::string& host, uint16_t port);

    /**
     * Disconnect and close socket
     */
    void disconnect();

    /**
     * Check if connected
     */
    bool isConnected() const { return connected_; }

    /**
     * Send video frame
     * @param data H.264 Annex-B encoded video data
     * @param size Size in bytes
     * @param isKeyframe True if this is a keyframe
     * @param timestamp PTS in 10M ticks/sec
     * @return true if sent successfully
     */
    bool sendVideo(const uint8_t* data, size_t size, bool isKeyframe, uint64_t timestamp);

    /**
     * Send audio frame
     * @param data PCM 32-bit float planar audio data
     * @param size Size in bytes
     * @param timestamp PTS in 10M ticks/sec
     * @param sampleRate Sample rate (typically 48000)
     * @param channels Number of channels (typically 2)
     * @return true if sent successfully
     */
    bool sendAudio(const uint8_t* data, size_t size, uint64_t timestamp,
                   uint32_t sampleRate, uint8_t channels);

    /**
     * Send raw packet (no fragmentation)
     */
    bool sendRaw(const uint8_t* data, size_t size);

    /**
     * Get current statistics
     */
    NetworkSenderStats getStats() const;

    /**
     * Reset statistics
     */
    void resetStats();

    /**
     * Set callbacks
     */
    void setOnConnected(OnSenderConnected callback) { onConnected_ = std::move(callback); }
    void setOnError(OnSenderError callback) { onError_ = std::move(callback); }
    void setOnStats(OnSenderStats callback) { onStats_ = std::move(callback); }

    /**
     * Get current configuration
     */
    const NetworkSenderConfig& getConfig() const { return config_; }

private:
    bool sendPacket(const uint8_t* data, size_t size);

    NetworkSenderConfig config_;
    int socket_ = -1;
    std::atomic<bool> connected_{false};

    // Sequence number for frames (incremented per frame, not per packet)
    std::atomic<uint32_t> sequenceNumber_{0};

    // Statistics
    mutable std::mutex statsMutex_;
    NetworkSenderStats stats_;

    // Callbacks
    OnSenderConnected onConnected_;
    OnSenderError onError_;
    OnSenderStats onStats_;
};

} // namespace ndi_bridge
