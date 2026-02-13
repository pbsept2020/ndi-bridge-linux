#include "network/NetworkSender.h"
#include "common/Protocol.h"
#include "common/Logger.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <thread>
#include <chrono>

namespace ndi_bridge {

NetworkSender::NetworkSender(const NetworkSenderConfig& config)
    : config_(config)
{
    LOG_DEBUG("NetworkSender initialized");
}

NetworkSender::~NetworkSender() {
    disconnect();
    LOG_DEBUG("NetworkSender destroyed");
}

bool NetworkSender::connect() {
    return connect(config_.host, config_.port);
}

bool NetworkSender::connect(const std::string& host, uint16_t port) {
    if (connected_) {
        disconnect();
    }

    config_.host = host;
    config_.port = port;

    Logger::instance().infof("Connecting to %s:%u...", host.c_str(), port);

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

    // Increase send buffer size — match Mac's aggressive non-blocking pattern
    int sendbuf = 4 * 1024 * 1024;  // 4MB
    setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf));

    // Setup destination address
    struct sockaddr_in destAddr{};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &destAddr.sin_addr) <= 0) {
        Logger::instance().errorf("Invalid address: %s", host.c_str());
        close(socket_);
        socket_ = -1;
        if (onError_) {
            onError_("Invalid address: " + host);
        }
        return false;
    }

    // Connect UDP socket (sets default destination)
    if (::connect(socket_, reinterpret_cast<struct sockaddr*>(&destAddr), sizeof(destAddr)) < 0) {
        Logger::instance().errorf("Failed to connect: %s", strerror(errno));
        close(socket_);
        socket_ = -1;
        if (onError_) {
            onError_(std::string("Failed to connect: ") + strerror(errno));
        }
        return false;
    }

    // Set non-blocking mode for fire-and-forget UDP (match Mac .idempotent send)
    int flags = fcntl(socket_, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(socket_, F_SETFL, flags | O_NONBLOCK);
    }

    connected_ = true;
    Logger::instance().successf("Connected to %s:%u (non-blocking, pacing: %dus)",
        host.c_str(), port, config_.pacingDelayUs);

    if (onConnected_) {
        onConnected_(host + ":" + std::to_string(port));
    }

    return true;
}

void NetworkSender::disconnect() {
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
    connected_ = false;

    auto stats = getStats();
    Logger::instance().successf("Disconnected. Sent: %lu bytes, %lu packets, %lu frames",
                                stats.bytesSent, stats.packetsSent, stats.framesSent);
}

bool NetworkSender::sendVideo(const uint8_t* data, size_t size, bool isKeyframe, uint64_t timestamp) {
    if (!connected_) {
        LOG_ERROR("Cannot send video - not connected");
        return false;
    }

    // Use consistent payload size for fragmentation
    const size_t maxPayload = MAX_UDP_PAYLOAD;
    const uint16_t fragmentCount = Protocol::calculateFragmentCount(static_cast<uint32_t>(size));
    const uint32_t seqNum = ++sequenceNumber_;

    std::vector<uint8_t> packet(MAX_PACKET_SIZE);

    for (uint16_t i = 0; i < fragmentCount; i++) {
        size_t offset = i * maxPayload;
        size_t payloadSize = std::min(maxPayload, size - offset);

        // Create header
        PacketHeader header = Protocol::createVideoHeader(
            seqNum,
            timestamp,
            static_cast<uint32_t>(size),
            i,
            fragmentCount,
            static_cast<uint16_t>(payloadSize),
            isKeyframe
        );

        // Serialize header
        Protocol::serializeInto(header, packet.data());

        // Copy payload
        std::memcpy(packet.data() + HEADER_SIZE, data + offset, payloadSize);

        // Send packet
        if (!sendPacket(packet.data(), HEADER_SIZE + payloadSize)) {
            return false;
        }

        // Pace fragments to avoid overwhelming the network tunnel
        if (config_.pacingDelayUs > 0 && i + 1 < fragmentCount) {
            std::this_thread::sleep_for(std::chrono::microseconds(config_.pacingDelayUs));
        }
    }

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.framesSent++;
    }

    return true;
}

bool NetworkSender::sendAudio(const uint8_t* data, size_t size, uint64_t timestamp,
                              uint32_t sampleRate, uint8_t channels) {
    if (!connected_) {
        LOG_ERROR("Cannot send audio - not connected");
        return false;
    }

    // Use consistent payload size for fragmentation
    const size_t maxPayload = MAX_UDP_PAYLOAD;
    const uint16_t fragmentCount = Protocol::calculateFragmentCount(static_cast<uint32_t>(size));
    const uint32_t seqNum = ++sequenceNumber_;

    std::vector<uint8_t> packet(MAX_PACKET_SIZE);

    for (uint16_t i = 0; i < fragmentCount; i++) {
        size_t offset = i * maxPayload;
        size_t payloadSize = std::min(maxPayload, size - offset);

        // Create header
        PacketHeader header = Protocol::createAudioHeader(
            seqNum,
            timestamp,
            static_cast<uint32_t>(size),
            i,
            fragmentCount,
            static_cast<uint16_t>(payloadSize),
            sampleRate,
            channels
        );

        // Serialize header
        Protocol::serializeInto(header, packet.data());

        // Copy payload
        std::memcpy(packet.data() + HEADER_SIZE, data + offset, payloadSize);

        // Send packet
        if (!sendPacket(packet.data(), HEADER_SIZE + payloadSize)) {
            return false;
        }

        // Pace fragments to avoid overwhelming the network tunnel
        if (config_.pacingDelayUs > 0 && i + 1 < fragmentCount) {
            std::this_thread::sleep_for(std::chrono::microseconds(config_.pacingDelayUs));
        }
    }

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.framesSent++;
    }

    return true;
}

bool NetworkSender::sendRaw(const uint8_t* data, size_t size) {
    return sendPacket(data, size);
}

bool NetworkSender::sendPacket(const uint8_t* data, size_t size) {
    ssize_t sent = send(socket_, data, size, MSG_DONTWAIT);

    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Non-blocking socket: kernel buffer full — drop packet (real-time behavior)
            // This matches Mac's .idempotent send completion (fire-and-forget)
            return true;
        }
        Logger::instance().errorf("Send error: %s", strerror(errno));
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.sendErrors++;
        }
        if (onError_) {
            onError_(std::string("Send error: ") + strerror(errno));
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.bytesSent += sent;
        stats_.packetsSent++;
    }

    return true;
}

NetworkSenderStats NetworkSender::getStats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void NetworkSender::resetStats() {
    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_ = NetworkSenderStats{};
}

} // namespace ndi_bridge
