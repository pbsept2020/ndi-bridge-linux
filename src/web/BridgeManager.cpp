/**
 * BridgeManager.cpp - Multi-pipeline orchestrator implementation
 */

#include "BridgeManager.h"
#include "../common/Logger.h"

namespace ndi_bridge {

BridgeManager::BridgeManager() {
    LOG_DEBUG("BridgeManager created");
}

BridgeManager::~BridgeManager() {
    stopAll();
    LOG_DEBUG("BridgeManager destroyed");
}

std::vector<NDISource> BridgeManager::discoverSources(int timeoutMs) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!finder_) {
        finder_ = std::make_unique<NDIReceiver>();
    }
    return finder_->discoverSources(timeoutMs);
}

int BridgeManager::addHost(const std::string& sourceName, const std::string& targetHost,
                           uint16_t targetPort, int bitrateMbps, size_t mtu) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto pipeline = std::make_unique<HostPipeline>();
    pipeline->id = nextId_++;
    pipeline->sourceName = sourceName;
    pipeline->targetHost = targetHost;
    pipeline->targetPort = targetPort;

    HostModeConfig config;
    config.sourceName = sourceName;
    config.targetHost = targetHost;
    config.targetPort = targetPort;
    config.bitrateMbps = bitrateMbps;
    config.mtu = mtu;

    pipeline->host = std::make_unique<HostMode>(config);
    pipeline->running = true;

    int id = pipeline->id;

    // Start in a dedicated thread
    pipeline->thread = std::thread([p = pipeline.get()]() {
        Logger::instance().infof("Pipeline #%d (host) thread started", p->id);
        p->host->start(p->running);
        p->running = false;
        Logger::instance().infof("Pipeline #%d (host) thread ended", p->id);
    });

    hosts_.push_back(std::move(pipeline));

    Logger::instance().successf("Added host pipeline #%d: %s -> %s:%u",
                                 id, sourceName.c_str(), targetHost.c_str(), targetPort);
    return id;
}

int BridgeManager::addJoin(const std::string& ndiName, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto pipeline = std::make_unique<JoinPipeline>();
    pipeline->id = nextId_++;
    pipeline->ndiName = ndiName;
    pipeline->port = port;

    JoinModeConfig config;
    config.ndiOutputName = ndiName;
    config.listenPort = port;

    pipeline->join = std::make_unique<JoinMode>(config);
    pipeline->running = true;

    int id = pipeline->id;

    // Start in a dedicated thread
    pipeline->thread = std::thread([p = pipeline.get()]() {
        Logger::instance().infof("Pipeline #%d (join) thread started", p->id);
        p->join->start(p->running);
        p->running = false;
        Logger::instance().infof("Pipeline #%d (join) thread ended", p->id);
    });

    joins_.push_back(std::move(pipeline));

    Logger::instance().successf("Added join pipeline #%d: '%s' on port %u",
                                 id, ndiName.c_str(), port);
    return id;
}

bool BridgeManager::stopPipeline(int id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Search in hosts
    for (auto it = hosts_.begin(); it != hosts_.end(); ++it) {
        if ((*it)->id == id) {
            Logger::instance().infof("Stopping host pipeline #%d", id);
            (*it)->running = false;
            (*it)->host->stop();
            if ((*it)->thread.joinable()) {
                (*it)->thread.join();
            }
            hosts_.erase(it);
            Logger::instance().successf("Host pipeline #%d stopped and removed", id);
            return true;
        }
    }

    // Search in joins
    for (auto it = joins_.begin(); it != joins_.end(); ++it) {
        if ((*it)->id == id) {
            Logger::instance().infof("Stopping join pipeline #%d", id);
            (*it)->running = false;
            (*it)->join->stop();
            if ((*it)->thread.joinable()) {
                (*it)->thread.join();
            }
            joins_.erase(it);
            Logger::instance().successf("Join pipeline #%d stopped and removed", id);
            return true;
        }
    }

    Logger::instance().errorf("Pipeline #%d not found", id);
    return false;
}

void BridgeManager::stopAll() {
    std::lock_guard<std::mutex> lock(mutex_);

    LOG_INFO("Stopping all pipelines...");

    // Signal all to stop
    for (auto& h : hosts_) h->running = false;
    for (auto& j : joins_) j->running = false;

    // Stop components
    for (auto& h : hosts_) h->host->stop();
    for (auto& j : joins_) j->join->stop();

    // Join threads
    for (auto& h : hosts_) {
        if (h->thread.joinable()) h->thread.join();
    }
    for (auto& j : joins_) {
        if (j->thread.joinable()) j->thread.join();
    }

    hosts_.clear();
    joins_.clear();

    LOG_SUCCESS("All pipelines stopped");
}

std::vector<PipelineStatus> BridgeManager::getStatus() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<PipelineStatus> result;

    for (const auto& h : hosts_) {
        PipelineStatus ps;
        ps.id = h->id;
        ps.type = "host";
        ps.description = h->sourceName + " -> " + h->targetHost + ":" + std::to_string(h->targetPort);
        ps.running = h->running.load();

        if (h->host) {
            auto stats = h->host->getStats();
            ps.videoFramesReceived = stats.videoFramesReceived;
            ps.videoFramesEncoded = stats.videoFramesEncoded;
            ps.videoFramesDropped = stats.videoFramesDropped;
            ps.bytesSent = stats.bytesSent;
            ps.runTimeSeconds = stats.runTimeSeconds;
        }

        result.push_back(ps);
    }

    for (const auto& j : joins_) {
        PipelineStatus ps;
        ps.id = j->id;
        ps.type = "join";
        ps.description = "\"" + j->ndiName + "\" :" + std::to_string(j->port);
        ps.running = j->running.load();

        if (j->join) {
            auto stats = j->join->getStats();
            ps.videoFramesReceived = stats.videoFramesReceived;
            ps.videoFramesDecoded = stats.videoFramesDecoded;
            ps.videoFramesOutput = stats.videoFramesOutput;
            ps.audioFramesOutput = stats.audioFramesOutput;
            ps.runTimeSeconds = stats.runTimeSeconds;
        }

        result.push_back(ps);
    }

    return result;
}

int BridgeManager::activePipelineCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto& h : hosts_) if (h->running) count++;
    for (const auto& j : joins_) if (j->running) count++;
    return count;
}

} // namespace ndi_bridge
