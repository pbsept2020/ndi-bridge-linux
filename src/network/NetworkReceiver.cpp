#include "network/NetworkReceiver.h"
#include "common/Logger.h"

#include <cstring>

namespace ndi_bridge {

NetworkReceiver::NetworkReceiver(const NetworkReceiverConfig& config)
    : config_(config)
{
    LOG_DEBUG("NetworkReceiver initialized");
}

NetworkReceiver::~NetworkReceiver() {
    stop();
    LOG_DEBUG("NetworkReceiver destroyed");
}

bool NetworkReceiver::startListening() {
    return startListening(config_.port);
}

bool NetworkReceiver::startListening(uint16_t port) {
    if (listening_) {
        LOG_ERROR("Already listening");
        return false;
    }

    config_.port = port;

    Logger::instance().infof("Starting UDP listener on port %u...", port);

    // Create UDP socket
    socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ == INVALID_SOCKET_VAL) {
        int err = platform_socket_errno();
        Logger::instance().errorf("Failed to create socket: %s", platform_socket_strerror(err));
        if (onError_) {
            onError_(std::string("Failed to create socket: ") + platform_socket_strerror(err));
        }
        return false;
    }

    // Set socket options
    int optval = 1;
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&optval), sizeof(optval));

#if PLATFORM_HAS_REUSEPORT
    setsockopt(socket_, SOL_SOCKET, SO_REUSEPORT,
               reinterpret_cast<const char*>(&optval), sizeof(optval));
#endif

    // Increase receive buffer size for better throughput
    int recvbuf = static_cast<int>(config_.recvBufferSize);
    setsockopt(socket_, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&recvbuf), sizeof(recvbuf));

    // Log actual buffer size granted by kernel
    int actualBuf = 0;
    socklen_t optLen = sizeof(actualBuf);
    getsockopt(socket_, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<char*>(&actualBuf), &optLen);
    Logger::instance().debugf("UDP recv buffer: requested=%dMB actual=%dMB",
        recvbuf / (1024*1024), actualBuf / (1024*1024));

    // Bind to port
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(socket_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        int err = platform_socket_errno();
        Logger::instance().errorf("Failed to bind to port %u: %s", port, platform_socket_strerror(err));
        platform_close_socket(socket_);
        socket_ = INVALID_SOCKET_VAL;
        if (onError_) {
            onError_(std::string("Failed to bind: ") + platform_socket_strerror(err));
        }
        return false;
    }

    // Start receive thread
    shouldStop_ = false;
    listening_ = true;
    receiveThread_ = std::thread(&NetworkReceiver::receiveLoop, this);

    Logger::instance().successf("Listening on UDP port %u", port);
    return true;
}

void NetworkReceiver::stop() {
    if (!listening_) {
        return;
    }

    LOG_INFO("Stopping receiver...");

    shouldStop_ = true;

    // Close socket to unblock recvfrom
    if (socket_ != INVALID_SOCKET_VAL) {
#ifdef _WIN32
        // On Windows, shutdown may fail on unconnected UDP sockets
        closesocket(socket_);
#else
        shutdown(socket_, SHUT_RDWR);
        close(socket_);
#endif
        socket_ = INVALID_SOCKET_VAL;
    }

    // Wait for receive thread
    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }

    listening_ = false;

    auto stats = getStats();
    Logger::instance().successf("Receiver stopped. Received: %lu bytes, %lu video frames, %lu audio frames",
                                stats.bytesReceived, stats.videoFramesReceived, stats.audioFramesReceived);
}

void NetworkReceiver::receiveLoop() {
    std::vector<uint8_t> buffer(MAX_PACKET_SIZE);
    struct sockaddr_in senderAddr{};
    socklen_t senderLen = sizeof(senderAddr);

    // Use poll/WSAPoll for timeout-based receive
#ifdef _WIN32
    WSAPOLLFD pfd;
#else
    struct pollfd pfd;
#endif
    pfd.fd = socket_;
    pfd.events = POLLIN;

    while (!shouldStop_) {
        // Poll with 10ms timeout (reduce jitter vs 100ms)
        int ret = platform_poll(&pfd, 1, 10);

        if (ret < 0) {
            int err = platform_socket_errno();
            if (err == PLATFORM_EINTR) continue;
            if (!shouldStop_) {
                Logger::instance().errorf("Poll error: %s", platform_socket_strerror(err));
            }
            break;
        }

        if (ret == 0) {
            // Timeout, check shouldStop and continue
            continue;
        }

        if (!(pfd.revents & POLLIN)) {
            continue;
        }

#ifdef _WIN32
        int received = recvfrom(socket_, reinterpret_cast<char*>(buffer.data()),
                                static_cast<int>(buffer.size()), 0,
                                reinterpret_cast<struct sockaddr*>(&senderAddr), &senderLen);
#else
        ssize_t received = recvfrom(socket_, buffer.data(), buffer.size(), 0,
                                    reinterpret_cast<struct sockaddr*>(&senderAddr), &senderLen);
#endif

        if (received < 0) {
            int err = platform_socket_errno();
            if (err == PLATFORM_EINTR || err == PLATFORM_EAGAIN) continue;
            if (!shouldStop_) {
                Logger::instance().errorf("Receive error: %s", platform_socket_strerror(err));
            }
            break;
        }

        if (received > 0) {
            uint64_t recvNs = Protocol::wallClockNs();
            processPacket(buffer.data(), static_cast<size_t>(received), recvNs);
        }
    }
}

void NetworkReceiver::processPacket(const uint8_t* data, size_t size, uint64_t recvTimestampNs) {
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.packetsReceived++;
        stats_.bytesReceived += size;
    }

    // Parse header
    auto headerOpt = Protocol::deserialize(data, size);
    if (!headerOpt) {
        Logger::instance().debugf("Invalid packet: failed to deserialize (size=%zu)", size);
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.invalidPackets++;
        }
        return;
    }

    const PacketHeader& header = *headerOpt;

    // Measure one-way latency on first fragment of each frame
    if (header.sendTimestamp > 0 && header.fragmentIndex == 0) {
        int64_t deltaMs = static_cast<int64_t>((recvTimestampNs - header.sendTimestamp) / 1000000);
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.latencySumMs += deltaMs;
        stats_.latencyCount++;
    }

    // Validate header
    if (!Protocol::isValid(header)) {
        Logger::instance().debugf("Invalid packet: validation failed - %s", Protocol::describe(header).c_str());
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.invalidPackets++;
        }
        return;
    }

    // Get payload
    const uint8_t* payload = data + HEADER_SIZE;
    size_t payloadSize = size - HEADER_SIZE;

    // Use appropriate reassembler
    FrameReassembler& reassembler = header.isVideo() ? videoReassembler_ : audioReassembler_;

    auto frameOpt = reassembler.addPacket(header, payload, payloadSize);

    if (frameOpt) {
        const auto& frame = *frameOpt;

        if (frame.type == MediaType::Video) {
            {
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.videoFramesReceived++;
            }

            if (onVideoFrame_) {
                ReceivedVideoFrame vf;
                vf.data = std::move(frame.data);
                vf.timestamp = frame.timestamp;
                vf.isKeyframe = frame.isKeyframe;
                vf.sequenceNumber = frame.sequenceNumber;
                onVideoFrame_(vf);
            }
        } else {
            {
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.audioFramesReceived++;
            }

            if (onAudioFrame_) {
                ReceivedAudioFrame af;
                af.data = std::move(frame.data);
                af.timestamp = frame.timestamp;
                af.sampleRate = frame.sampleRate;
                af.channels = frame.channels;
                af.sequenceNumber = frame.sequenceNumber;
                onAudioFrame_(af);
            }
        }
    }

    // Update dropped frames count from reassembler stats
    auto videoStats = videoReassembler_.getStats();
    auto audioStats = audioReassembler_.getStats();
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.framesDropped = videoStats.framesDropped + audioStats.framesDropped;
    }
}

NetworkReceiverStats NetworkReceiver::getStats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void NetworkReceiver::resetStats() {
    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_ = NetworkReceiverStats{};
    videoReassembler_.reset();
    audioReassembler_.reset();
}

} // namespace ndi_bridge
