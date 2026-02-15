#pragma once

/**
 * BridgeManager.h - Multi-pipeline orchestrator for NDI Bridge Web UI
 *
 * Manages multiple HostMode and JoinMode instances, each running in its own thread.
 * Provides NDI source discovery and pipeline lifecycle management.
 *
 * Thread-safety: all public methods are mutex-protected.
 */

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>

#include "../ndi/NDIReceiver.h"
#include "../host/HostMode.h"
#include "../join/JoinMode.h"

namespace ndi_bridge {

/**
 * Pipeline status for the web UI
 */
struct PipelineStatus {
    int id;
    std::string type;         // "host" or "join"
    std::string description;  // e.g., "Test Pattern LTC -> 63.181.214.196:5990"
    bool running;

    // Host stats
    uint64_t videoFramesReceived = 0;
    uint64_t videoFramesEncoded = 0;
    uint64_t videoFramesDropped = 0;  // queue drops
    uint64_t bytesSent = 0;

    // Join stats
    uint64_t videoFramesDecoded = 0;
    uint64_t videoFramesOutput = 0;
    uint64_t audioFramesOutput = 0;

    // Common
    double runTimeSeconds = 0.0;
};

/**
 * BridgeManager - Orchestrates multiple bridge pipelines
 */
class BridgeManager {
public:
    BridgeManager();
    ~BridgeManager();

    // Non-copyable
    BridgeManager(const BridgeManager&) = delete;
    BridgeManager& operator=(const BridgeManager&) = delete;

    /**
     * Discover NDI sources on the network
     * @param timeoutMs Discovery timeout
     * @return List of source names
     */
    std::vector<NDISource> discoverSources(int timeoutMs = 3000);

    /**
     * Add and start a host pipeline
     * @return Pipeline ID, or -1 on error
     */
    int addHost(const std::string& sourceName, const std::string& targetHost,
                uint16_t targetPort, int bitrateMbps = 8, size_t mtu = 1400);

    /**
     * Add and start a join pipeline
     * @return Pipeline ID, or -1 on error
     */
    int addJoin(const std::string& ndiName, uint16_t port);

    /**
     * Stop and remove a pipeline
     * @return true if found and stopped
     */
    bool stopPipeline(int id);

    /**
     * Stop all pipelines
     */
    void stopAll();

    /**
     * Get status of all pipelines
     */
    std::vector<PipelineStatus> getStatus();

    /**
     * Get number of active pipelines
     */
    int activePipelineCount();

private:
    struct HostPipeline {
        int id;
        std::string sourceName;
        std::string targetHost;
        uint16_t targetPort;
        std::unique_ptr<HostMode> host;
        std::thread thread;
        std::atomic<bool> running{false};
    };

    struct JoinPipeline {
        int id;
        std::string ndiName;
        uint16_t port;
        std::unique_ptr<JoinMode> join;
        std::thread thread;
        std::atomic<bool> running{false};
    };

    std::mutex mutex_;
    std::vector<std::unique_ptr<HostPipeline>> hosts_;
    std::vector<std::unique_ptr<JoinPipeline>> joins_;
    int nextId_ = 1;

    // Shared NDI finder for discovery
    std::unique_ptr<NDIReceiver> finder_;
};

} // namespace ndi_bridge
