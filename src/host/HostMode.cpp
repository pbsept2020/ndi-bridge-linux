/**
 * HostMode.cpp - NDI Bridge Host Mode Implementation
 *
 * Orchestrates: NDIReceiver → VideoEncoder → NetworkSender
 */

#include "HostMode.h"
#include "../common/Logger.h"

#include <iostream>
#include <algorithm>
#include <cstring>

namespace ndi_bridge {

HostMode::HostMode(const HostModeConfig& config)
    : config_(config)
{
    LOG_DEBUG("HostMode created");
}

HostMode::~HostMode() {
    stop();
    LOG_DEBUG("HostMode destroyed");
}

int HostMode::start(std::atomic<bool>& running) {
    if (running_) {
        LOG_ERROR("Host mode is already running");
        return 1;
    }

    auto& log = Logger::instance();

    log.info("═══════════════════════════════════════════════════════");
    log.info("Starting HOST MODE (Sender)");
    log.successf("Target: %s:%u", config_.targetHost.c_str(), config_.targetPort);
    log.successf("Bitrate: %d Mbps", config_.bitrateMbps);
    log.info("═══════════════════════════════════════════════════════");

    // Step 1: Initialize NDI Receiver
    log.info("Step 1/5: Initializing NDI receiver...");
    ndiReceiver_ = std::make_unique<NDIReceiver>();

    if (!NDIReceiver::initializeNDI()) {
        LOG_ERROR("Failed to initialize NDI SDK");
        return 1;
    }

    // Configure callbacks
    ndiReceiver_->setOnVideoFrame([this](const NDIVideoFrame& frame) {
        onVideoFrame(frame);
    });
    ndiReceiver_->setOnAudioFrame([this](const NDIAudioFrame& frame) {
        onAudioFrame(frame);
    });
    ndiReceiver_->setOnError([this](const std::string& error) {
        onNDIError(error);
    });

    // Step 2: Discover sources
    log.info("Step 2/5: Discovering NDI sources...");
    auto sources = ndiReceiver_->discoverSources(config_.sourceDiscoveryTimeoutMs);

    if (sources.empty()) {
        LOG_ERROR("No NDI sources found on the network");
        return 1;
    }

    log.successf("Found %zu NDI source(s)", sources.size());

    // Step 3: Select source
    log.info("Step 3/5: Selecting NDI source...");
    selectedSource_ = selectSource(sources);

    if (selectedSource_.name.empty()) {
        LOG_ERROR("No source selected");
        return 1;
    }

    log.successf("Selected: %s", selectedSource_.name.c_str());

    // Connect to source
    if (!ndiReceiver_->connect(selectedSource_)) {
        log.errorf("Failed to connect to source: %s", selectedSource_.name.c_str());
        return 1;
    }

    // Step 4: Initialize encoder (will be configured on first frame)
    log.info("Step 4/5: Preparing H.264 encoder...");
    encoder_ = std::make_unique<VideoEncoder>();

    encoder_->setOnEncodedFrame([this](const EncodedFrame& frame) {
        onEncodedFrame(frame);
    });
    encoder_->setOnError([](const std::string& error) {
        Logger::instance().errorf("Encoder error: %s", error.c_str());
    });

    // Step 5: Initialize network sender
    log.info("Step 5/5: Connecting to network...");
    NetworkSenderConfig senderConfig;
    senderConfig.host = config_.targetHost;
    senderConfig.port = config_.targetPort;

    networkSender_ = std::make_unique<NetworkSender>(senderConfig);

    if (!networkSender_->connect()) {
        LOG_ERROR("Failed to create network socket");
        return 1;
    }

    // Start receiving
    running_ = true;
    startTime_ = std::chrono::steady_clock::now();
    ndiReceiver_->startReceiving();

    log.success("═══════════════════════════════════════════════════════");
    log.success("HOST MODE STARTED");
    log.successf("Streaming: %s → %s:%u",
                 selectedSource_.name.c_str(),
                 config_.targetHost.c_str(),
                 config_.targetPort);
    log.success("═══════════════════════════════════════════════════════");
    log.info("Press Ctrl+C to stop...");

    // Main loop - just wait for shutdown signal
    while (running && running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Periodic stats (every 5 seconds in verbose mode)
        static int counter = 0;
        if (++counter >= 50 && Logger::instance().isVerbose()) {
            counter = 0;
            auto stats = getStats();
            log.debugf("Stats: video=%lu audio=%lu encoded=%lu sent=%.2f MB time=%.1fs",
                      stats.videoFramesReceived,
                      stats.audioFramesReceived,
                      stats.videoFramesEncoded,
                      stats.bytesSent / (1024.0 * 1024.0),
                      stats.runTimeSeconds);
        }
    }

    // Cleanup
    stop();

    auto finalStats = getStats();
    log.success("═══════════════════════════════════════════════════════");
    log.success("HOST MODE STOPPED");
    log.successf("Duration: %.1f seconds", finalStats.runTimeSeconds);
    log.successf("Video frames: %lu received, %lu encoded",
                 finalStats.videoFramesReceived, finalStats.videoFramesEncoded);
    log.successf("Audio frames: %lu", finalStats.audioFramesReceived);
    log.successf("Data sent: %.2f MB", finalStats.bytesSent / (1024.0 * 1024.0));
    log.success("═══════════════════════════════════════════════════════");

    return 0;
}

void HostMode::stop() {
    if (!running_) return;

    LOG_INFO("Stopping Host Mode...");
    running_ = false;

    if (ndiReceiver_) {
        ndiReceiver_->stopReceiving();
        ndiReceiver_->disconnect();
    }

    if (encoder_) {
        encoder_->flush();
    }

    if (networkSender_) {
        networkSender_->disconnect();
    }
}

std::vector<NDISource> HostMode::listSources() {
    if (!ndiReceiver_) {
        ndiReceiver_ = std::make_unique<NDIReceiver>();
        NDIReceiver::initializeNDI();
    }
    return ndiReceiver_->discoverSources(config_.sourceDiscoveryTimeoutMs);
}

HostMode::Stats HostMode::getStats() const {
    Stats stats;
    stats.videoFramesReceived = videoFramesReceived_;
    stats.audioFramesReceived = audioFramesReceived_;
    stats.videoFramesEncoded = videoFramesEncoded_;

    if (networkSender_) {
        stats.bytesSent = networkSender_->getStats().bytesSent;
    }

    if (running_) {
        auto now = std::chrono::steady_clock::now();
        stats.runTimeSeconds = std::chrono::duration<double>(now - startTime_).count();
    }

    return stats;
}

// ============================================================================
// Callbacks
// ============================================================================

void HostMode::onVideoFrame(const NDIVideoFrame& frame) {
    videoFramesReceived_++;

    // Configure encoder on first frame (auto-detect resolution/fps)
    if (!encoderConfigured_) {
        VideoEncoderConfig encConfig;
        encConfig.width = frame.width;
        encConfig.height = frame.height;
        encConfig.bitrate = config_.bitrateMbps * 1000000;

        // Determine framerate
        if (frame.frameRateD > 0 && frame.frameRateN > 0) {
            encConfig.fps = frame.frameRateN / frame.frameRateD;
            encConfig.keyframeInterval = encConfig.fps; // Keyframe every second
        }

        // Determine input format from FourCC
        // NDI uses UYVY (0x59565955) or BGRA (0x41524742)
        if (frame.fourcc == 0x59565955 || frame.fourcc == 0x56595559) {  // UYVY or YUYV
            encConfig.inputFormat = PixelFormat::UYVY;
        } else {
            encConfig.inputFormat = PixelFormat::BGRA;
        }

        Logger::instance().infof("Video: %dx%d @ %d fps, format=0x%08X",
                                  frame.width, frame.height, encConfig.fps, frame.fourcc);
        Logger::instance().infof("Encoder: %s preset, %d Mbps",
                                  encConfig.preset.c_str(), config_.bitrateMbps);

        if (!encoder_->configure(encConfig)) {
            LOG_ERROR("Failed to configure encoder");
            return;
        }
        encoderConfigured_ = true;
        LOG_SUCCESS("Encoder configured");
    }

    // Encode the frame
    encoder_->encodeWithStride(frame.data.data(), frame.stride,
                               static_cast<uint64_t>(frame.timestamp));
}

void HostMode::onAudioFrame(const NDIAudioFrame& frame) {
    audioFramesReceived_++;

    if (!networkSender_ || !networkSender_->isConnected()) {
        return;
    }

    // Convert float samples to bytes and send directly (passthrough)
    const uint8_t* audioData = reinterpret_cast<const uint8_t*>(frame.data.data());
    size_t audioSize = frame.data.size() * sizeof(float);

    networkSender_->sendAudio(audioData, audioSize,
                              static_cast<uint64_t>(frame.timestamp),
                              static_cast<uint32_t>(frame.sampleRate),
                              static_cast<uint8_t>(frame.channels));
}

void HostMode::onEncodedFrame(const EncodedFrame& frame) {
    videoFramesEncoded_++;

    if (!networkSender_ || !networkSender_->isConnected()) {
        return;
    }

    // Send encoded video over network
    networkSender_->sendVideo(frame.data.data(), frame.data.size(),
                              frame.isKeyframe, frame.timestamp);
}

void HostMode::onNDIError(const std::string& error) {
    Logger::instance().errorf("NDI error: %s", error.c_str());

    // Could implement reconnection logic here
}

// ============================================================================
// Source Selection
// ============================================================================

NDISource HostMode::selectSource(const std::vector<NDISource>& sources) {
    // Filter out excluded patterns
    std::vector<NDISource> filtered;
    for (const auto& src : sources) {
        if (!matchesExcludePattern(src.name)) {
            filtered.push_back(src);
        }
    }

    if (filtered.empty()) {
        LOG_ERROR("All sources filtered out by exclude patterns");
        Logger::instance().info("Available sources before filtering:");
        for (const auto& src : sources) {
            Logger::instance().infof("  - %s", src.name.c_str());
        }
        return NDISource{};
    }

    // Specific source name requested?
    if (!config_.sourceName.empty()) {
        for (const auto& src : sources) {  // Search in all sources, not just filtered
            if (src.name.find(config_.sourceName) != std::string::npos) {
                return src;
            }
        }
        Logger::instance().errorf("Source '%s' not found", config_.sourceName.c_str());
        Logger::instance().info("Available sources:");
        for (const auto& src : sources) {
            Logger::instance().infof("  - %s", src.name.c_str());
        }
        return NDISource{};
    }

    // Auto-select first?
    if (config_.autoSelectFirstSource) {
        Logger::instance().infof("Auto-selecting: %s", filtered[0].name.c_str());
        return filtered[0];
    }

    // Interactive selection
    return promptUserSelection(filtered);
}

NDISource HostMode::promptUserSelection(const std::vector<NDISource>& sources) {
    std::cout << "\n";
    std::cout << "╔═══════════════════════════════════════════════════════╗\n";
    std::cout << "║          SELECT NDI SOURCE                            ║\n";
    std::cout << "╠═══════════════════════════════════════════════════════╣\n";

    for (size_t i = 0; i < sources.size(); i++) {
        std::string name = sources[i].name;
        if (name.length() > 45) {
            name = name.substr(0, 42) + "...";
        }
        printf("║  [%zu] %-47s ║\n", i + 1, name.c_str());
    }

    std::cout << "╚═══════════════════════════════════════════════════════╝\n";
    std::cout << "Enter source number (1-" << sources.size() << "): ";
    std::cout.flush();

    std::string input;
    if (!std::getline(std::cin, input)) {
        LOG_ERROR("Failed to read input");
        return NDISource{};
    }

    try {
        int choice = std::stoi(input);
        if (choice >= 1 && choice <= static_cast<int>(sources.size())) {
            return sources[choice - 1];
        }
    } catch (...) {
        // Invalid input
    }

    LOG_ERROR("Invalid selection");
    return NDISource{};
}

bool HostMode::matchesExcludePattern(const std::string& name) const {
    for (const auto& pattern : config_.excludePatterns) {
        // Case-insensitive search
        std::string nameLower = name;
        std::string patternLower = pattern;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
        std::transform(patternLower.begin(), patternLower.end(), patternLower.begin(), ::tolower);

        if (nameLower.find(patternLower) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace ndi_bridge
