/**
 * JoinMode.cpp - NDI Bridge Join Mode Implementation
 *
 * Orchestrates: NetworkReceiver → VideoDecoder → NDISender
 */

#include "JoinMode.h"
#include "../common/Logger.h"
#include "../common/Protocol.h"

#include <cstring>
#include <chrono>

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

    VideoDecoderConfig decoderConfig;
    decoderConfig.outputFormat = OutputPixelFormat::BGRA;
    if (!decoder_->configure(decoderConfig)) {
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

    // Start async decode thread
    decodeRunning_ = true;
    decodeThread_ = std::thread(&JoinMode::decodeLoop, this);

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
            auto netStats = networkReceiver_ ? networkReceiver_->getStats() : NetworkReceiverStats{};
            // Get reassembler fragment stats for drop diagnosis
            auto videoReasmStats = networkReceiver_ ? networkReceiver_->getVideoReassemblerStats() : FrameReassembler::Stats{};
            uint64_t avgRecv = videoReasmStats.framesDropped > 0 ? videoReasmStats.totalFragmentsReceivedBeforeDrop / videoReasmStats.framesDropped : 0;
            uint64_t avgExpect = videoReasmStats.framesDropped > 0 ? videoReasmStats.totalFragmentsExpectedBeforeDrop / videoReasmStats.framesDropped : 0;
            double avgCompletion = videoReasmStats.totalFragmentsExpectedBeforeDrop > 0
                ? 100.0 * videoReasmStats.totalFragmentsReceivedBeforeDrop / videoReasmStats.totalFragmentsExpectedBeforeDrop : 0.0;
            double decAvgMs = decodeCount_ > 0 ? (totalDecodeTimeUs_.load() / (double)decodeCount_.load()) / 1000.0 : 0.0;
            double decMaxMs = maxDecodeTimeUs_.load() / 1000.0;
            log.debugf("Stats: pkts=%lu recv=%lu dropped=%lu(avg %lu/%lu frags %.0f%%) decoded=%lu output=%lu qdrop=%lu decode_ms=%.1f/%.1f audio=%lu time=%.1fs",
                      netStats.packetsReceived,
                      stats.videoFramesReceived,
                      netStats.framesDropped,
                      avgRecv, avgExpect, avgCompletion,
                      stats.videoFramesDecoded,
                      stats.videoFramesOutput,
                      videoFramesDroppedQueue_.load(),
                      decAvgMs, decMaxMs,
                      stats.audioFramesOutput,
                      stats.runTimeSeconds);
        }
    }

    // Cleanup
    stop();

    auto finalStats = getStats();
    auto finalNetStats = networkReceiver_ ? networkReceiver_->getStats() : NetworkReceiverStats{};
    auto finalReasmStats = networkReceiver_ ? networkReceiver_->getVideoReassemblerStats() : FrameReassembler::Stats{};
    double finalAvgCompletion = finalReasmStats.totalFragmentsExpectedBeforeDrop > 0
        ? 100.0 * finalReasmStats.totalFragmentsReceivedBeforeDrop / finalReasmStats.totalFragmentsExpectedBeforeDrop : 0.0;
    double finalDecAvgMs = decodeCount_ > 0 ? (totalDecodeTimeUs_.load() / (double)decodeCount_.load()) / 1000.0 : 0.0;
    double finalDecMaxMs = maxDecodeTimeUs_.load() / 1000.0;
    log.success("═══════════════════════════════════════════════════════");
    log.success("JOIN MODE STOPPED");
    log.successf("Duration: %.1f seconds", finalStats.runTimeSeconds);
    log.successf("Packets: %lu received, %lu invalid",
                 finalNetStats.packetsReceived, finalNetStats.invalidPackets);
    log.successf("Video: %lu received, %lu decoded, %lu output",
                 finalStats.videoFramesReceived,
                 finalStats.videoFramesDecoded,
                 finalStats.videoFramesOutput);
    log.successf("Dropped: %lu frames (avg completion %.0f%%, frags %lu/%lu) qdrop=%lu",
                 finalNetStats.framesDropped, finalAvgCompletion,
                 finalReasmStats.totalFragmentsReceivedBeforeDrop,
                 finalReasmStats.totalFragmentsExpectedBeforeDrop,
                 videoFramesDroppedQueue_.load());
    log.successf("Decode: avg=%.1fms max=%.1fms (%lu frames)",
                 finalDecAvgMs, finalDecMaxMs, decodeCount_.load());
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

    // Stop decode thread before flushing decoder
    if (decodeRunning_) {
        decodeRunning_ = false;
        decodeQueueCv_.notify_all();
        if (decodeThread_.joinable()) {
            decodeThread_.join();
        }
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

    // Push to async decode queue (non-blocking, ~1ms)
    {
        std::lock_guard<std::mutex> lock(decodeQueueMutex_);
        if (decodeQueue_.size() >= MAX_DECODE_QUEUE) {
            decodeQueue_.pop();
            videoFramesDroppedQueue_++;
        }
        decodeQueue_.push(frame);
    }
    decodeQueueCv_.notify_one();
}

void JoinMode::decodeLoop() {
    LOG_DEBUG("Decode thread started");
    while (decodeRunning_) {
        ReceivedVideoFrame frame;
        {
            std::unique_lock<std::mutex> lock(decodeQueueMutex_);
            decodeQueueCv_.wait(lock, [this] {
                return !decodeQueue_.empty() || !decodeRunning_;
            });
            if (!decodeRunning_ && decodeQueue_.empty()) break;
            frame = std::move(decodeQueue_.front());
            decodeQueue_.pop();
        }
        if (decoder_) {
            auto t0 = std::chrono::steady_clock::now();
            decoder_->decode(frame.data.data(), frame.data.size(), frame.timestamp);
            auto t1 = std::chrono::steady_clock::now();
            uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            totalDecodeTimeUs_ += us;
            decodeCount_++;
            uint64_t prevMax = maxDecodeTimeUs_.load();
            while (us > prevMax && !maxDecodeTimeUs_.compare_exchange_weak(prevMax, us)) {}
        }
    }
    LOG_DEBUG("Decode thread stopped");
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

    // Debug: log first frame data to verify content
    if (videoFramesDecoded_.load() <= 3) {
        // Check if data is all zeros
        bool allZero = true;
        for (size_t i = 0; i < std::min(frame.data.size(), (size_t)100); i++) {
            if (frame.data[i] != 0) { allZero = false; break; }
        }
        Logger::instance().debugf("Frame %lu: %dx%d stride=%d size=%zu fmt=%d first_bytes=[%02x %02x %02x %02x %02x %02x %02x %02x] allZero=%s",
            videoFramesDecoded_.load(), frame.width, frame.height, frame.stride,
            frame.data.size(), (int)frame.format,
            frame.data.size() > 0 ? frame.data[0] : 0,
            frame.data.size() > 1 ? frame.data[1] : 0,
            frame.data.size() > 2 ? frame.data[2] : 0,
            frame.data.size() > 3 ? frame.data[3] : 0,
            frame.data.size() > 4 ? frame.data[4] : 0,
            frame.data.size() > 5 ? frame.data[5] : 0,
            frame.data.size() > 6 ? frame.data[6] : 0,
            frame.data.size() > 7 ? frame.data[7] : 0,
            allZero ? "YES" : "no");
    }

    // Determine NDI format from decoded frame format
    NDIVideoFormat ndiFormat;
    switch (frame.format) {
        case OutputPixelFormat::UYVY:
            ndiFormat = NDIVideoFormat::UYVY;
            break;
        case OutputPixelFormat::I420:
            ndiFormat = NDIVideoFormat::I420;
            break;
        default:
            ndiFormat = NDIVideoFormat::BGRA;
            break;
    }

    if (config_.bufferMs > 0) {
        // Buffered mode: enqueue for delayed playback
        std::lock_guard<std::mutex> lock(videoBufferMutex_);

        BufferedVideoFrame buffered;
        buffered.data = frame.data;
        buffered.width = frame.width;
        buffered.height = frame.height;
        buffered.stride = frame.stride;
        buffered.ndiFormat = ndiFormat;
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
                                          frame.stride, frame.ndiFormat, frame.timestamp);
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
