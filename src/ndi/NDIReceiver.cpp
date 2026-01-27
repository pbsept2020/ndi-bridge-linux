#include "ndi/NDIReceiver.h"
#include "common/Logger.h"

#include <Processing.NDI.Lib.h>
#include <cstring>

namespace ndi_bridge {

// FourCC constants
static constexpr uint32_t FOURCC_BGRA = NDIlib_FourCC_video_type_BGRA;
static constexpr uint32_t FOURCC_BGRX = NDIlib_FourCC_video_type_BGRX;
static constexpr uint32_t FOURCC_UYVY = NDIlib_FourCC_video_type_UYVY;

NDIReceiver::NDIReceiver() {
    LOG_DEBUG("NDIReceiver created");
}

NDIReceiver::~NDIReceiver() {
    stopReceiving();
    disconnect();

    if (finder_) {
        NDIlib_find_destroy(static_cast<NDIlib_find_instance_t>(finder_));
        finder_ = nullptr;
    }

    LOG_DEBUG("NDIReceiver destroyed");
}

bool NDIReceiver::initializeNDI() {
    if (!NDIlib_initialize()) {
        LOG_ERROR("Failed to initialize NDI SDK");
        return false;
    }
    LOG_SUCCESS("NDI SDK initialized");
    return true;
}

void NDIReceiver::destroyNDI() {
    NDIlib_destroy();
    LOG_DEBUG("NDI SDK destroyed");
}

void NDIReceiver::configure(const NDIReceiverConfig& config) {
    config_ = config;
    Logger::instance().debugf("NDIReceiver configured: name=%s, BGRA=%s",
                              config.receiverName.c_str(),
                              config.preferBGRA ? "yes" : "no");
}

std::vector<NDISource> NDIReceiver::discoverSources(int timeoutMs) {
    // Create finder if not exists
    if (!finder_) {
        NDIlib_find_create_t findSettings;
        findSettings.show_local_sources = true;
        findSettings.p_groups = nullptr;
        findSettings.p_extra_ips = nullptr;

        finder_ = NDIlib_find_create_v2(&findSettings);
        if (!finder_) {
            LOG_ERROR("Failed to create NDI finder");
            return {};
        }
    }

    Logger::instance().infof("Searching for NDI sources (%d ms)...", timeoutMs);

    // Wait for sources
    NDIlib_find_wait_for_sources(static_cast<NDIlib_find_instance_t>(finder_), timeoutMs);

    // Get current sources
    uint32_t numSources = 0;
    const NDIlib_source_t* sources = NDIlib_find_get_current_sources(
        static_cast<NDIlib_find_instance_t>(finder_),
        &numSources
    );

    // Store for later use (for connection)
    currentSources_ = const_cast<void*>(static_cast<const void*>(sources));
    numSources_ = static_cast<int>(numSources);

    // Build result
    cachedSources_.clear();
    cachedSources_.reserve(numSources);

    for (uint32_t i = 0; i < numSources; i++) {
        NDISource source;
        source.name = sources[i].p_ndi_name ? sources[i].p_ndi_name : "";
        source.address = sources[i].p_url_address ? sources[i].p_url_address : "";
        cachedSources_.push_back(source);

        Logger::instance().infof("  Found: %s", source.name.c_str());
    }

    Logger::instance().successf("Found %d NDI source(s)", static_cast<int>(numSources));
    return cachedSources_;
}

bool NDIReceiver::connect(const std::string& sourceName) {
    // Find source by name
    for (const auto& source : cachedSources_) {
        if (source.name == sourceName) {
            return connect(source);
        }
    }

    Logger::instance().errorf("NDI source not found: %s", sourceName.c_str());
    return false;
}

bool NDIReceiver::connect(const NDISource& source) {
    disconnect();

    // Find the source in cached sources
    const NDIlib_source_t* ndiSource = nullptr;

    if (currentSources_ && numSources_ > 0) {
        const NDIlib_source_t* sources = static_cast<const NDIlib_source_t*>(currentSources_);
        for (int i = 0; i < numSources_; i++) {
            if (sources[i].p_ndi_name && source.name == sources[i].p_ndi_name) {
                ndiSource = &sources[i];
                break;
            }
        }
    }

    if (!ndiSource) {
        Logger::instance().errorf("Source not in cache: %s", source.name.c_str());
        return false;
    }

    // Create receiver
    NDIlib_recv_create_v3_t recvSettings;
    memset(&recvSettings, 0, sizeof(recvSettings));

    recvSettings.source_to_connect_to = *ndiSource;
    recvSettings.color_format = config_.preferBGRA
        ? NDIlib_recv_color_format_BGRX_BGRA
        : NDIlib_recv_color_format_UYVY_BGRA;
    recvSettings.bandwidth = config_.lowBandwidth
        ? NDIlib_recv_bandwidth_lowest
        : NDIlib_recv_bandwidth_highest;
    recvSettings.allow_video_fields = false;
    recvSettings.p_ndi_recv_name = config_.receiverName.c_str();

    receiver_ = NDIlib_recv_create_v3(&recvSettings);
    if (!receiver_) {
        LOG_ERROR("Failed to create NDI receiver");
        return false;
    }

    connected_ = true;
    connectedSourceName_ = source.name;

    Logger::instance().successf("Connected to NDI source: %s", source.name.c_str());
    return true;
}

void NDIReceiver::disconnect() {
    stopReceiving();

    if (receiver_) {
        NDIlib_recv_destroy(static_cast<NDIlib_recv_instance_t>(receiver_));
        receiver_ = nullptr;
    }

    connected_ = false;
    connectedSourceName_.clear();
}

void NDIReceiver::startReceiving() {
    if (!connected_ || receiving_) {
        return;
    }

    receiving_ = true;
    receiveThread_ = std::thread(&NDIReceiver::receiveLoop, this);

    LOG_INFO("Started receiving NDI frames");
}

void NDIReceiver::stopReceiving() {
    if (!receiving_) {
        return;
    }

    receiving_ = false;

    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }

    LOG_DEBUG("Stopped receiving NDI frames");
}

NDIVideoFrame NDIReceiver::captureVideoFrame(int timeoutMs) {
    NDIVideoFrame result;

    if (!receiver_) {
        return result;
    }

    NDIlib_video_frame_v2_t videoFrame;
    NDIlib_audio_frame_v3_t audioFrame;

    auto frameType = NDIlib_recv_capture_v3(
        static_cast<NDIlib_recv_instance_t>(receiver_),
        &videoFrame,
        &audioFrame,
        nullptr,
        static_cast<uint32_t>(timeoutMs)
    );

    if (frameType == NDIlib_frame_type_video) {
        result.width = videoFrame.xres;
        result.height = videoFrame.yres;
        result.stride = videoFrame.line_stride_in_bytes;
        result.fourcc = videoFrame.FourCC;
        result.frameRateN = videoFrame.frame_rate_N;
        result.frameRateD = videoFrame.frame_rate_D;
        result.timestamp = videoFrame.timestamp;

        // Calculate data size
        size_t dataSize = static_cast<size_t>(videoFrame.line_stride_in_bytes) *
                          static_cast<size_t>(videoFrame.yres);
        result.data.resize(dataSize);
        std::memcpy(result.data.data(), videoFrame.p_data, dataSize);

        // Free the frame
        NDIlib_recv_free_video_v2(
            static_cast<NDIlib_recv_instance_t>(receiver_),
            &videoFrame
        );

        stats_.videoFramesReceived++;
    } else if (frameType == NDIlib_frame_type_audio) {
        // Free audio frame if received
        NDIlib_recv_free_audio_v3(
            static_cast<NDIlib_recv_instance_t>(receiver_),
            &audioFrame
        );
    }

    return result;
}

void NDIReceiver::receiveLoop() {
    NDIlib_video_frame_v2_t videoFrame;
    NDIlib_audio_frame_v3_t audioFrame;

    while (receiving_) {
        auto frameType = NDIlib_recv_capture_v3(
            static_cast<NDIlib_recv_instance_t>(receiver_),
            &videoFrame,
            &audioFrame,
            nullptr,
            100  // 100ms timeout
        );

        switch (frameType) {
            case NDIlib_frame_type_video:
                if (onVideoFrame_) {
                    NDIVideoFrame frame;
                    frame.width = videoFrame.xres;
                    frame.height = videoFrame.yres;
                    frame.stride = videoFrame.line_stride_in_bytes;
                    frame.fourcc = videoFrame.FourCC;
                    frame.frameRateN = videoFrame.frame_rate_N;
                    frame.frameRateD = videoFrame.frame_rate_D;
                    frame.timestamp = videoFrame.timestamp;

                    size_t dataSize = static_cast<size_t>(videoFrame.line_stride_in_bytes) *
                                      static_cast<size_t>(videoFrame.yres);
                    frame.data.resize(dataSize);
                    std::memcpy(frame.data.data(), videoFrame.p_data, dataSize);

                    onVideoFrame_(frame);
                    stats_.videoFramesReceived++;
                }

                NDIlib_recv_free_video_v2(
                    static_cast<NDIlib_recv_instance_t>(receiver_),
                    &videoFrame
                );
                break;

            case NDIlib_frame_type_audio:
                if (onAudioFrame_) {
                    NDIAudioFrame frame;
                    frame.sampleRate = audioFrame.sample_rate;
                    frame.channels = audioFrame.no_channels;
                    frame.samplesPerChannel = audioFrame.no_samples;
                    frame.timestamp = audioFrame.timestamp;

                    // Check if planar (channel_stride > 0) or interleaved
                    frame.isPlanar = (audioFrame.channel_stride_in_bytes > 0);

                    if (frame.isPlanar) {
                        // Planar format: channel_stride bytes per channel
                        size_t totalSamples = static_cast<size_t>(audioFrame.no_samples) *
                                              static_cast<size_t>(audioFrame.no_channels);
                        frame.data.resize(totalSamples);

                        // Copy each channel
                        for (int ch = 0; ch < audioFrame.no_channels; ch++) {
                            const float* src = reinterpret_cast<const float*>(
                                audioFrame.p_data + ch * audioFrame.channel_stride_in_bytes
                            );
                            float* dst = frame.data.data() + ch * audioFrame.no_samples;
                            std::memcpy(dst, src, audioFrame.no_samples * sizeof(float));
                        }
                    } else {
                        // Interleaved format
                        size_t totalSamples = static_cast<size_t>(audioFrame.no_samples) *
                                              static_cast<size_t>(audioFrame.no_channels);
                        frame.data.resize(totalSamples);
                        std::memcpy(frame.data.data(),
                                    reinterpret_cast<const float*>(audioFrame.p_data),
                                    totalSamples * sizeof(float));
                    }

                    onAudioFrame_(frame);
                    stats_.audioFramesReceived++;
                }

                NDIlib_recv_free_audio_v3(
                    static_cast<NDIlib_recv_instance_t>(receiver_),
                    &audioFrame
                );
                break;

            case NDIlib_frame_type_none:
                // No frame available, continue
                break;

            case NDIlib_frame_type_error:
                if (onError_) {
                    onError_("NDI receive error");
                }
                break;

            default:
                break;
        }
    }
}

} // namespace ndi_bridge
