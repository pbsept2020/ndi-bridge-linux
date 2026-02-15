#pragma once

/**
 * NDIReceiver.h - NDI video/audio receiver
 *
 * Discovers NDI sources on the network and captures video/audio frames.
 * Video is received in BGRA or UYVY format.
 * Audio is received as PCM 32-bit float.
 */

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <thread>

namespace ndi_bridge {

/**
 * NDI Source information
 */
struct NDISource {
    std::string name;       // Full NDI name (e.g., "HOSTNAME (Source Name)")
    std::string address;    // URL address (may be empty)
};

/**
 * NDI Video frame data
 */
struct NDIVideoFrame {
    int width;
    int height;
    int stride;             // Line stride in bytes
    uint32_t fourcc;        // FourCC pixel format
    int frameRateN;         // Frame rate numerator
    int frameRateD;         // Frame rate denominator
    int64_t timestamp;      // Timestamp in 100ns intervals
    std::vector<uint8_t> data;
};

/**
 * NDI Audio frame data
 */
struct NDIAudioFrame {
    int sampleRate;         // e.g., 48000
    int channels;           // Number of channels
    int samplesPerChannel;  // Number of samples per channel
    int64_t timestamp;      // Timestamp in 100ns intervals
    std::vector<float> data; // Interleaved or planar float samples
    bool isPlanar;          // true if planar, false if interleaved
};

/**
 * Callback types
 */
using OnNDIVideoFrame = std::function<void(NDIVideoFrame frame)>;
using OnNDIAudioFrame = std::function<void(const NDIAudioFrame& frame)>;
using OnNDIError = std::function<void(const std::string& error)>;

/**
 * NDIReceiver configuration
 */
struct NDIReceiverConfig {
    std::string receiverName = "NDI Bridge Receiver";
    bool preferBGRA = true;     // true = BGRA, false = UYVY
    bool lowBandwidth = false;  // Use low bandwidth mode
};

/**
 * NDIReceiver - Receives NDI video and audio streams
 */
class NDIReceiver {
public:
    NDIReceiver();
    ~NDIReceiver();

    // Non-copyable
    NDIReceiver(const NDIReceiver&) = delete;
    NDIReceiver& operator=(const NDIReceiver&) = delete;

    /**
     * Initialize NDI SDK (must be called before any other NDI operations)
     * @return true if successful
     */
    static bool initializeNDI();

    /**
     * Cleanup NDI SDK (call when done with all NDI operations)
     */
    static void destroyNDI();

    /**
     * Configure the receiver
     */
    void configure(const NDIReceiverConfig& config);

    /**
     * Discover NDI sources on the network
     * @param timeoutMs How long to wait for sources
     * @return List of discovered sources
     */
    std::vector<NDISource> discoverSources(int timeoutMs = 3000);

    /**
     * Connect to an NDI source by name
     * @param sourceName Full NDI source name
     * @return true if connection initiated
     */
    bool connect(const std::string& sourceName);

    /**
     * Connect to an NDI source
     * @param source Source from discoverSources()
     * @return true if connection initiated
     */
    bool connect(const NDISource& source);

    /**
     * Prepare source for deferred connection (actual connect on receive thread)
     * Required on macOS: NDI SDK needs connect + capture on same thread.
     */
    void prepareConnect(const NDISource& source);

    /**
     * Disconnect from current source
     */
    void disconnect();

    /**
     * Check if connected to a source
     */
    bool isConnected() const { return connected_; }

    /**
     * Get the name of the connected source
     */
    std::string getSourceName() const { return connectedSourceName_; }

    /**
     * Start receiving frames (calls callbacks on receive thread)
     */
    void startReceiving();

    /**
     * Stop receiving frames
     */
    void stopReceiving();

    /**
     * Check if currently receiving
     */
    bool isReceiving() const { return receiving_; }

    /**
     * Capture a single video frame (blocking)
     * @param timeoutMs Timeout in milliseconds
     * @return Video frame, or empty if timeout/error
     */
    NDIVideoFrame captureVideoFrame(int timeoutMs = 1000);

    /**
     * Set callbacks
     */
    void setOnVideoFrame(OnNDIVideoFrame callback) { onVideoFrame_ = std::move(callback); }
    void setOnAudioFrame(OnNDIAudioFrame callback) { onAudioFrame_ = std::move(callback); }
    void setOnError(OnNDIError callback) { onError_ = std::move(callback); }

    /**
     * Get statistics
     */
    struct Stats {
        uint64_t videoFramesReceived = 0;
        uint64_t audioFramesReceived = 0;
        uint64_t droppedFrames = 0;
    };
    Stats getStats() const { return stats_; }

private:
    void receiveLoop();

    NDIReceiverConfig config_;
    void* finder_ = nullptr;
    void* receiver_ = nullptr;
    std::vector<NDISource> cachedSources_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> receiving_{false};
    std::string connectedSourceName_;

    std::thread receiveThread_;

    // Callbacks
    OnNDIVideoFrame onVideoFrame_;
    OnNDIAudioFrame onAudioFrame_;
    OnNDIError onError_;

    // Statistics
    Stats stats_;

    // For source lookup
    void* currentSources_ = nullptr;
    int numSources_ = 0;

    // Deferred connect (connection happens on receive thread)
    NDISource pendingSource_;
    int pendingSourceIdx_ = -1;
    bool hasPendingConnect_ = false;
};

} // namespace ndi_bridge
