/**
 * NDISender.cpp - NDI video/audio sender implementation
 *
 * Uses NDI SDK to broadcast video and audio frames.
 */

#include "NDISender.h"
#include "../common/Logger.h"

#include <Processing.NDI.Lib.h>
#include <cstring>

namespace ndi_bridge {

// Static initialization flag
static bool s_ndiInitialized = false;

NDISender::NDISender(const std::string& sourceName)
    : sourceName_(sourceName)
{
    LOG_DEBUG("NDISender created");
}

NDISender::~NDISender() {
    stop();
    LOG_DEBUG("NDISender destroyed");
}

bool NDISender::initializeNDI() {
    if (s_ndiInitialized) {
        return true;
    }

    if (!NDIlib_initialize()) {
        LOG_ERROR("Failed to initialize NDI SDK");
        return false;
    }

    s_ndiInitialized = true;
    LOG_SUCCESS("NDI SDK initialized");
    return true;
}

void NDISender::destroyNDI() {
    if (s_ndiInitialized) {
        NDIlib_destroy();
        s_ndiInitialized = false;
        LOG_DEBUG("NDI SDK destroyed");
    }
}

bool NDISender::start(int width, int height, double frameRate) {
    if (running_) {
        LOG_ERROR("NDI sender already running");
        return false;
    }

    auto& log = Logger::instance();
    log.infof("Starting NDI sender: '%s'", sourceName_.c_str());
    log.infof("Output format: %dx%d @ %.2f fps", width, height, frameRate);

    // Store configuration
    config_.width = width;
    config_.height = height;

    // Convert frame rate to fraction
    if (frameRate > 59 && frameRate < 61) {
        config_.frameRateN = 60000;
        config_.frameRateD = 1001;
    } else if (frameRate > 49 && frameRate < 51) {
        config_.frameRateN = 50000;
        config_.frameRateD = 1000;
    } else if (frameRate > 29 && frameRate < 31) {
        config_.frameRateN = 30000;
        config_.frameRateD = 1001;
    } else if (frameRate > 24 && frameRate < 26) {
        config_.frameRateN = 24000;
        config_.frameRateD = 1001;
    } else {
        config_.frameRateN = static_cast<int>(frameRate * 1000);
        config_.frameRateD = 1000;
    }

    // Initialize NDI if needed
    if (!initializeNDI()) {
        return false;
    }

    // Create sender
    NDIlib_send_create_t sendCreate;
    sendCreate.p_ndi_name = sourceName_.c_str();
    sendCreate.p_groups = nullptr;
    sendCreate.clock_video = true;  // Use video timing
    sendCreate.clock_audio = false;

    sender_ = NDIlib_send_create(&sendCreate);
    if (!sender_) {
        LOG_ERROR("Failed to create NDI sender");
        return false;
    }

    running_ = true;
    stats_ = Stats{};
    lastTimestamp_ = 0;
    frameIntervals_.clear();
    frameRateDetected_ = false;

    log.successf("NDI sender started: '%s' broadcasting on local network", sourceName_.c_str());

    if (onStarted_) {
        onStarted_(sourceName_);
    }

    return true;
}

void NDISender::stop() {
    if (!running_) {
        return;
    }

    LOG_INFO("Stopping NDI sender...");

    running_ = false;

    if (sender_) {
        NDIlib_send_destroy(static_cast<NDIlib_send_instance_t>(sender_));
        sender_ = nullptr;
    }

    Logger::instance().successf("NDI sender stopped. Video: %lu, Audio: %lu",
                                 stats_.videoFramesSent, stats_.audioFramesSent);
}

bool NDISender::sendVideo(const uint8_t* data, int width, int height, int stride,
                          NDIVideoFormat format, uint64_t timestamp) {
    if (!running_ || !sender_) {
        return false;
    }

    // Detect frame rate from timestamps
    detectFrameRate(timestamp);

    // Prepare NDI video frame
    NDIlib_video_frame_v2_t videoFrame;
    videoFrame.xres = width;
    videoFrame.yres = height;
    videoFrame.frame_rate_N = config_.frameRateN;
    videoFrame.frame_rate_D = config_.frameRateD;
    videoFrame.picture_aspect_ratio = static_cast<float>(width) / static_cast<float>(height);
    videoFrame.frame_format_type = NDIlib_frame_format_type_progressive;
    videoFrame.timecode = NDIlib_send_timecode_synthesize;
    videoFrame.p_data = const_cast<uint8_t*>(data);
    videoFrame.line_stride_in_bytes = stride;
    videoFrame.p_metadata = nullptr;
    videoFrame.timestamp = static_cast<int64_t>(timestamp);

    // Set FourCC based on format
    switch (format) {
        case NDIVideoFormat::BGRA:
            videoFrame.FourCC = NDIlib_FourCC_video_type_BGRA;
            break;
        case NDIVideoFormat::UYVY:
            videoFrame.FourCC = NDIlib_FourCC_video_type_UYVY;
            break;
    }

    // Send the frame
    NDIlib_send_send_video_v2(static_cast<NDIlib_send_instance_t>(sender_), &videoFrame);

    stats_.videoFramesSent++;
    return true;
}

bool NDISender::sendAudio(const uint8_t* data, size_t size, uint32_t sampleRate,
                          uint8_t channels, int numSamples, uint64_t timestamp) {
    if (!running_ || !sender_) {
        return false;
    }

    // NDI expects 32-bit float planar audio
    NDIlib_audio_frame_v2_t audioFrame;
    audioFrame.sample_rate = static_cast<int>(sampleRate);
    audioFrame.no_channels = channels;
    audioFrame.no_samples = numSamples;
    audioFrame.timecode = NDIlib_send_timecode_synthesize;
    audioFrame.p_data = reinterpret_cast<float*>(const_cast<uint8_t*>(data));
    audioFrame.channel_stride_in_bytes = numSamples * sizeof(float);
    audioFrame.p_metadata = nullptr;
    audioFrame.timestamp = static_cast<int64_t>(timestamp);

    // Send the frame
    NDIlib_send_send_audio_v2(static_cast<NDIlib_send_instance_t>(sender_), &audioFrame);

    stats_.audioFramesSent++;
    return true;
}

void NDISender::setSourceName(const std::string& name) {
    bool wasRunning = running_;

    if (wasRunning) {
        stop();
    }

    sourceName_ = name;

    if (wasRunning) {
        start(config_.width, config_.height,
              static_cast<double>(config_.frameRateN) / config_.frameRateD);
    }
}

void NDISender::detectFrameRate(uint64_t timestamp) {
    if (frameRateDetected_) {
        return;
    }

    if (lastTimestamp_ > 0 && frameIntervals_.size() < 10) {
        // Timestamp is in 100ns intervals (10MHz)
        double intervalNs = static_cast<double>(timestamp - lastTimestamp_) * 100.0;
        double intervalMs = intervalNs / 1000000.0;

        // Sanity check (5-200 fps range)
        if (intervalMs > 5 && intervalMs < 200) {
            frameIntervals_.push_back(intervalMs);

            if (frameIntervals_.size() == 10) {
                // Calculate average frame rate
                double avgInterval = 0;
                for (double i : frameIntervals_) {
                    avgInterval += i;
                }
                avgInterval /= frameIntervals_.size();
                double detectedFps = 1000.0 / avgInterval;

                // Set frame rate fraction
                if (detectedFps > 59 && detectedFps < 61) {
                    config_.frameRateN = 60000;
                    config_.frameRateD = 1001;
                } else if (detectedFps > 49 && detectedFps < 51) {
                    config_.frameRateN = 50000;
                    config_.frameRateD = 1000;
                } else if (detectedFps > 29 && detectedFps < 31) {
                    config_.frameRateN = 30000;
                    config_.frameRateD = 1001;
                } else if (detectedFps > 24 && detectedFps < 26) {
                    config_.frameRateN = 24000;
                    config_.frameRateD = 1001;
                } else {
                    config_.frameRateN = static_cast<int>(detectedFps * 1000);
                    config_.frameRateD = 1000;
                }

                frameRateDetected_ = true;
                Logger::instance().infof("Detected frame rate: %.2f fps -> %d/%d",
                                          detectedFps, config_.frameRateN, config_.frameRateD);
            }
        }
    }

    lastTimestamp_ = timestamp;
}

} // namespace ndi_bridge
