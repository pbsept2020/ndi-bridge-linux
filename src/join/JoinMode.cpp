/**
 * JoinMode.cpp - NDI Bridge Join Mode Implementation
 *
 * Orchestrates: NetworkReceiver → VideoDecoder → NDISender
 */

#include "JoinMode.h"
#include "../common/Logger.h"

#include <cstring>

namespace ndi_bridge {

JoinMode::JoinMode(const JoinModeConfig& config)
    : config_(config)
{
    LOG_DEBUG("JoinMode created");
}

JoinMode::~JoinMode() {
    stop();
    LOG_DEBUG("JoinMode destroyed");
}

int JoinMode::start(std::atomic<bool>& running) {
    if (running_) {
        LOG_ERROR("Join mode is already running");
        return 1;
    }

    auto& log = Logger::instance();

    log.info("═══════════════════════════════════════════════════════");
    log.info("Starting JOIN MODE (Receiver)");
    log.successf("Listen port: %u", config_.listenPort);
    log.successf("NDI output: '%s'", config_.ndiOutputName.c_str());
    if (config_.bufferMs > 0) {
        log.successf("Buffer: %d ms delay", config_.bufferMs);
    } else {
        log.info("Buffer: disabled (real-time)");
    }
    log.info("═══════════════════════════════════════════════════════");

    // Step 1: Initialize decoder
    log.info("Step 1/3: Initializing H.264 decoder...");
    decoder_ = std::make_unique<VideoDecoder>();

    if (!decoder_->configure()) {
        LOG_ERROR("Failed to configure video decoder");
        return 1;
    }

    decoder_->setOnDecodedFrame([this](const DecodedFrame& frame) {
        onDecodedFrame(frame);
    });
    decoder_->setOnError([](const std::string& error) {
        Logger::instance().errorf("Decoder error: %s", error.c_str());
    });

    LOG_SUCCESS("Decoder ready (waiting for SPS/PPS)");

    // Step 2: Start NDI sender
    log.info("Step 2/3: Starting NDI output...");
    ndiSender_ = std::make_unique<NDISender>(config_.ndiOutputName);

    ndiSender_->setOnStarted([](const std::string& name) {
        Logger::instance().successf("NDI output broadcasting as '%s'", name.c_str());
    });
    ndiSender_->setOnError([](const std::string& error) {
        Logger::instance().errorf("NDI sender error: %s", error.c_str());
    });

    if (!ndiSender_->start(config_.outputWidth, config_.outputHeight)) {
        LOG_ERROR("Failed to start NDI sender");
        return 1;
    }

    // Step 3: Start network receiver
    log.info("Step 3/3: Starting network listener...");
    NetworkReceiverConfig recvConfig;
    recvConfig.port = config_.listenPort;

    networkReceiver_ = std::make_unique<NetworkReceiver>(recvConfig);

    networkReceiver_->setOnVideoFrame([this](const ReceivedVideoFrame& frame) {
        onVideoFrame(frame);
    });
    networkReceiver_->setOnAudioFrame([this](const ReceivedAudioFrame& frame) {
        onAudioFrame(frame);
    });
    networkReceiver_->setOnError([this](const std::string& error) {
        onNetworkError(error);
    });

    if (!networkReceiver_->startListening()) {
        LOG_ERROR("Failed to start network listener");
        ndiSender_->stop();
        return 1;
    }

    // Start buffer thread if buffering enabled
    if (config_.bufferMs > 0) {
        bufferRunning_ = true;
        bufferThread_ = std::thread(&JoinMode::bufferOutputLoop, this);
        log.successf("Buffer enabled: %d ms delay", config_.bufferMs);
    }

    running_ = true;
    startTime_ = std::chrono::steady_clock::now();

    log.success("═══════════════════════════════════════════════════════");
    log.success("JOIN MODE STARTED");
    log.successf("Waiting for incoming stream on port %u...", config_.listenPort);
    log.successf("NDI output available as '%s'", config_.ndiOutputName.c_str());
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
            log.debugf("Stats: recv=%lu decoded=%lu output=%lu audio=%lu time=%.1fs",
                      stats.videoFramesReceived,
                      stats.videoFramesDecoded,
                      stats.videoFramesOutput,
                      stats.audioFramesOutput,
                      stats.runTimeSeconds);
        }
    }

    // Cleanup
    stop();

    auto finalStats = getStats();
    log.success("═══════════════════════════════════════════════════════");
    log.success("JOIN MODE STOPPED");
    log.successf("Duration: %.1f seconds", finalStats.runTimeSeconds);
    log.successf("Video: %lu received, %lu decoded, %lu output",
                 finalStats.videoFramesReceived,
                 finalStats.videoFramesDecoded,
                 finalStats.videoFramesOutput);
    log.successf("Audio: %lu received, %lu output",
                 finalStats.audioFramesReceived, finalStats.audioFramesOutput);
    log.success("═══════════════════════════════════════════════════════");

    return 0;
}

void JoinMode::stop() {
    if (!running_) return;

    LOG_INFO("Stopping Join Mode...");
    running_ = false;

    // Stop buffer thread
    if (bufferRunning_) {
        bufferRunning_ = false;
        if (bufferThread_.joinable()) {
            bufferThread_.join();
        }
    }

    if (networkReceiver_) {
        networkReceiver_->stop();
    }

    if (decoder_) {
        decoder_->flush();
    }

    if (ndiSender_) {
        ndiSender_->stop();
    }
}

void JoinMode::setOutputName(const std::string& name) {
    config_.ndiOutputName = name;
    if (ndiSender_) {
        ndiSender_->setSourceName(name);
    }
}

JoinMode::Stats JoinMode::getStats() const {
    Stats stats;
    stats.videoFramesReceived = videoFramesReceived_;
    stats.audioFramesReceived = audioFramesReceived_;
    stats.videoFramesDecoded = videoFramesDecoded_;
    stats.videoFramesOutput = videoFramesOutput_;
    stats.audioFramesOutput = audioFramesOutput_;

    if (running_) {
        auto now = std::chrono::steady_clock::now();
        stats.runTimeSeconds = std::chrono::duration<double>(now - startTime_).count();
    }

    return stats;
}

// ============================================================================
// Callbacks
// ============================================================================

void JoinMode::onVideoFrame(const ReceivedVideoFrame& frame) {
    videoFramesReceived_++;

    // Decode the received H.264 frame
    if (decoder_) {
        decoder_->decode(frame.data.data(), frame.data.size(), frame.timestamp);
    }
}

void JoinMode::onAudioFrame(const ReceivedAudioFrame& frame) {
    audioFramesReceived_++;

    if (!ndiSender_ || !ndiSender_->isRunning()) {
        return;
    }

    // Calculate number of samples per channel
    // Audio is 32-bit float, so 4 bytes per sample
    int numSamples = static_cast<int>(frame.data.size() / (frame.channels * sizeof(float)));

    if (config_.bufferMs > 0) {
        // Buffered mode: enqueue for delayed playback
        std::lock_guard<std::mutex> lock(audioBufferMutex_);

        BufferedAudioFrame buffered;
        buffered.data = frame.data;
        buffered.sampleRate = frame.sampleRate;
        buffered.channels = frame.channels;
        buffered.numSamples = numSamples;
        buffered.timestamp = frame.timestamp;

        // Calculate play time
        if (!bufferSynced_ && firstFrameTimestamp_ == 0) {
            firstFrameTimestamp_ = frame.timestamp;
            bufferStartTime_ = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count()
            );
            bufferSynced_ = true;
        }

        // Play time = buffer start + (frame timestamp - first frame timestamp) + buffer delay
        uint64_t relativeTime = (frame.timestamp - firstFrameTimestamp_) / 10; // 100ns to us
        buffered.playTime = bufferStartTime_ + relativeTime + (config_.bufferMs * 1000);

        audioBuffer_.push(buffered);
    } else {
        // Real-time mode: send directly to NDI
        ndiSender_->sendAudio(frame.data.data(), frame.data.size(),
                              frame.sampleRate, frame.channels, numSamples,
                              frame.timestamp);
        audioFramesOutput_++;
    }
}

void JoinMode::onDecodedFrame(const DecodedFrame& frame) {
    videoFramesDecoded_++;

    if (!ndiSender_ || !ndiSender_->isRunning()) {
        return;
    }

    // Determine NDI format from decoded frame format
    NDIVideoFormat ndiFormat = NDIVideoFormat::BGRA;
    if (frame.format == OutputPixelFormat::UYVY) {
        ndiFormat = NDIVideoFormat::UYVY;
    }

    if (config_.bufferMs > 0) {
        // Buffered mode: enqueue for delayed playback
        std::lock_guard<std::mutex> lock(videoBufferMutex_);

        BufferedVideoFrame buffered;
        buffered.data = frame.data;
        buffered.width = frame.width;
        buffered.height = frame.height;
        buffered.stride = frame.stride;
        buffered.timestamp = frame.timestamp;

        // Calculate play time
        if (!bufferSynced_ && firstFrameTimestamp_ == 0) {
            firstFrameTimestamp_ = frame.timestamp;
            bufferStartTime_ = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count()
            );
            bufferSynced_ = true;
        }

        // Play time = buffer start + (frame timestamp - first frame timestamp) + buffer delay
        uint64_t relativeTime = (frame.timestamp - firstFrameTimestamp_) / 10; // 100ns to us
        buffered.playTime = bufferStartTime_ + relativeTime + (config_.bufferMs * 1000);

        videoBuffer_.push(buffered);
    } else {
        // Real-time mode: send directly to NDI
        ndiSender_->sendVideo(frame.data.data(), frame.width, frame.height,
                              frame.stride, ndiFormat, frame.timestamp);
        videoFramesOutput_++;
    }
}

void JoinMode::onNetworkError(const std::string& error) {
    Logger::instance().errorf("Network error: %s", error.c_str());
}

// ============================================================================
// Buffer Management
// ============================================================================

void JoinMode::bufferOutputLoop() {
    LOG_DEBUG("Buffer output thread started");

    while (bufferRunning_) {
        processBufferedFrames();
        std::this_thread::sleep_for(std::chrono::microseconds(500)); // Check every 0.5ms
    }

    LOG_DEBUG("Buffer output thread stopped");
}

void JoinMode::processBufferedFrames() {
    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );

    // Process video frames
    {
        std::lock_guard<std::mutex> lock(videoBufferMutex_);
        while (!videoBuffer_.empty()) {
            auto& frame = videoBuffer_.front();
            if (frame.playTime <= now) {
                // Time to play this frame
                if (ndiSender_ && ndiSender_->isRunning()) {
                    ndiSender_->sendVideo(frame.data.data(), frame.width, frame.height,
                                          frame.stride, NDIVideoFormat::BGRA, frame.timestamp);
                    videoFramesOutput_++;
                }
                videoBuffer_.pop();
            } else {
                break; // Not time yet for next frame
            }
        }
    }

    // Process audio frames
    {
        std::lock_guard<std::mutex> lock(audioBufferMutex_);
        while (!audioBuffer_.empty()) {
            auto& frame = audioBuffer_.front();
            if (frame.playTime <= now) {
                // Time to play this frame
                if (ndiSender_ && ndiSender_->isRunning()) {
                    ndiSender_->sendAudio(frame.data.data(), frame.data.size(),
                                          frame.sampleRate, frame.channels,
                                          frame.numSamples, frame.timestamp);
                    audioFramesOutput_++;
                }
                audioBuffer_.pop();
            } else {
                break; // Not time yet for next frame
            }
        }
    }
}

} // namespace ndi_bridge
