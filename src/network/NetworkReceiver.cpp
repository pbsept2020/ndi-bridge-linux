#include "network/NetworkReceiver.h"
#include "common/Logger.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <cerrno>

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
    if (socket_ < 0) {
        Logger::instance().errorf("Failed to create socket: %s", strerror(errno));
        if (onError_) {
            onError_(std::string("Failed to create socket: ") + strerror(errno));
        }
        return false;
    }

    // Set socket options
    int optval = 1;
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    setsockopt(socket_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

    // Increase receive buffer size for better throughput
    int recvbuf = static_cast<int>(config_.recvBufferSize);
    setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &recvbuf, sizeof(recvbuf));

    // Log actual buffer size granted by kernel
    int actualBuf = 0;
    socklen_t optLen = sizeof(actualBuf);
    getsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &actualBuf, &optLen);
    Logger::instance().debugf("UDP recv buffer: requested=%dMB actual=%dMB",
        recvbuf / (1024*1024), actualBuf / (1024*1024));

    // Bind to port
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(socket_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        Logger::instance().errorf("Failed to bind to port %u: %s", port, strerror(errno));
        close(socket_);
        socket_ = -1;
        if (onError_) {
            onError_(std::string("Failed to bind: ") + strerror(errno));
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
    if (socket_ >= 0) {
        shutdown(socket_, SHUT_RDWR);
        close(socket_);
        socket_ = -1;
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

    // Use poll for timeout-based receive
    struct pollfd pfd;
    pfd.fd = socket_;
    pfd.events = POLLIN;

    while (!shouldStop_) {
        // Poll with 100ms timeout
        int ret = poll(&pfd, 1, 100);

        if (ret < 0) {
            if (errno == EINTR) continue;
            if (!shouldStop_) {
                Logger::instance().errorf("Poll error: %s", strerror(errno));
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

        ssize_t received = recvfrom(socket_, buffer.data(), buffer.size(), 0,
                                    reinterpret_cast<struct sockaddr*>(&senderAddr), &senderLen);

        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (!shouldStop_) {
                Logger::instance().errorf("Receive error: %s", strerror(errno));
            }
            break;
        }

        if (received > 0) {
            processPacket(buffer.data(), static_cast<size_t>(received));
        }
    }
}

void NetworkReceiver::processPacket(const uint8_t* data, size_t size) {
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

    // Validate header
    if (!Protocol::isValid(header)) {
        Logger::instance().debugf("Invalid packet: validation failed - %s", Protocol::describe(header).c_str());
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.invalidPackets++;
        }
        return;
    }

    // Debug: uncomment to see all packets
    // Logger::instance().debugf("Packet: %s", Protocol::describe(header).c_str());

    // Get payload
    const uint8_t* payload = data + HEADER_SIZE;
    size_t payloadSize = size - HEADER_SIZE;

    // Use appropriate reassembler
    FrameReassembler& reassembler = header.isVideo() ? videoReassembler_ : audioReassembler_;

    auto frameOpt = reassembler.addPacket(header, payload, payloadSize);

    // Debug: uncomment to see fragment progress
    // if (!frameOpt) {
    //     auto stats = reassembler.getStats();
    //     Logger::instance().debugf("Fragment %u/%u received for seq %u",
    //                               header.fragmentIndex + 1, header.fragmentCount,
    //                               header.sequenceNumber);
    // }

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
