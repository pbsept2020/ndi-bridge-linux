#include "common/Protocol.h"
#include "common/Logger.h"
#include <sstream>
#include <iomanip>

namespace ndi_bridge {

PacketHeader Protocol::createVideoHeader(
    uint32_t sequenceNumber,
    uint64_t timestamp,
    uint32_t totalSize,
    uint16_t fragmentIndex,
    uint16_t fragmentCount,
    uint16_t payloadSize,
    bool isKeyframe
) {
    PacketHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.mediaType = static_cast<uint8_t>(MediaType::Video);
    header.sourceId = 0;
    header.flags = isKeyframe ? 0x01 : 0x00;
    header.sequenceNumber = sequenceNumber;
    header.timestamp = timestamp;
    header.totalSize = totalSize;
    header.fragmentIndex = fragmentIndex;
    header.fragmentCount = fragmentCount;
    header.payloadSize = payloadSize;
    header.sampleRate = 0;
    header.channels = 0;
    std::memset(header.reserved, 0, sizeof(header.reserved));
    return header;
}

PacketHeader Protocol::createAudioHeader(
    uint32_t sequenceNumber,
    uint64_t timestamp,
    uint32_t totalSize,
    uint16_t fragmentIndex,
    uint16_t fragmentCount,
    uint16_t payloadSize,
    uint32_t sampleRate,
    uint8_t channels
) {
    PacketHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.mediaType = static_cast<uint8_t>(MediaType::Audio);
    header.sourceId = 0;
    header.flags = 0;
    header.sequenceNumber = sequenceNumber;
    header.timestamp = timestamp;
    header.totalSize = totalSize;
    header.fragmentIndex = fragmentIndex;
    header.fragmentCount = fragmentCount;
    header.payloadSize = payloadSize;
    header.sampleRate = sampleRate;
    header.channels = channels;
    std::memset(header.reserved, 0, sizeof(header.reserved));
    return header;
}

std::vector<uint8_t> Protocol::serialize(const PacketHeader& header) {
    std::vector<uint8_t> buffer(HEADER_SIZE);
    serializeInto(header, buffer.data());
    return buffer;
}

void Protocol::serializeInto(const PacketHeader& header, uint8_t* buffer) {
    // Convert to network byte order (Big-Endian) and write
    // This MUST match Swift MediaPacketHeader.toData() exactly
    uint32_t magic = endian::hton32(header.magic);
    uint32_t seqNum = endian::hton32(header.sequenceNumber);
    uint64_t timestamp = endian::hton64(header.timestamp);
    uint32_t totalSize = endian::hton32(header.totalSize);
    uint16_t fragIndex = endian::hton16(header.fragmentIndex);
    uint16_t fragCount = endian::hton16(header.fragmentCount);
    uint16_t payloadSize = endian::hton16(header.payloadSize);
    uint32_t sampleRate = endian::hton32(header.sampleRate);

    std::memcpy(buffer + 0, &magic, 4);       // 0-3: magic
    buffer[4] = header.version;               // 4: version
    buffer[5] = header.mediaType;             // 5: mediaType
    buffer[6] = header.sourceId;              // 6: sourceId
    buffer[7] = header.flags;                 // 7: flags
    std::memcpy(buffer + 8, &seqNum, 4);      // 8-11: sequenceNumber
    std::memcpy(buffer + 12, &timestamp, 8);  // 12-19: timestamp
    std::memcpy(buffer + 20, &totalSize, 4);  // 20-23: totalSize
    std::memcpy(buffer + 24, &fragIndex, 2);  // 24-25: fragmentIndex
    std::memcpy(buffer + 26, &fragCount, 2);  // 26-27: fragmentCount
    std::memcpy(buffer + 28, &payloadSize, 2);// 28-29: payloadSize
    std::memcpy(buffer + 30, &sampleRate, 4); // 30-33: sampleRate
    buffer[34] = header.channels;             // 34: channels
    std::memset(buffer + 35, 0, 3);           // 35-37: reserved
}

std::optional<PacketHeader> Protocol::deserialize(const uint8_t* data, size_t size) {
    if (size < HEADER_SIZE) {
        return std::nullopt;
    }

    PacketHeader header{};

    // Read and convert from network byte order
    uint32_t magic;
    std::memcpy(&magic, data + 0, 4);
    header.magic = endian::ntoh32(magic);

    if (header.magic != PROTOCOL_MAGIC) {
        return std::nullopt;
    }

    header.version = data[4];
    if (header.version != PROTOCOL_VERSION) {
        return std::nullopt;
    }

    header.mediaType = data[5];
    header.sourceId = data[6];
    header.flags = data[7];

    uint32_t seqNum;
    std::memcpy(&seqNum, data + 8, 4);
    header.sequenceNumber = endian::ntoh32(seqNum);

    uint64_t timestamp;
    std::memcpy(&timestamp, data + 12, 8);
    header.timestamp = endian::ntoh64(timestamp);

    uint32_t totalSize;
    std::memcpy(&totalSize, data + 20, 4);
    header.totalSize = endian::ntoh32(totalSize);

    uint16_t fragIndex;
    std::memcpy(&fragIndex, data + 24, 2);
    header.fragmentIndex = endian::ntoh16(fragIndex);

    uint16_t fragCount;
    std::memcpy(&fragCount, data + 26, 2);
    header.fragmentCount = endian::ntoh16(fragCount);

    uint16_t payloadSize;
    std::memcpy(&payloadSize, data + 28, 2);
    header.payloadSize = endian::ntoh16(payloadSize);

    uint32_t sampleRate;
    std::memcpy(&sampleRate, data + 30, 4);
    header.sampleRate = endian::ntoh32(sampleRate);

    header.channels = data[34];
    std::memset(header.reserved, 0, 3);

    return header;
}

bool Protocol::isValid(const PacketHeader& header) {
    return header.magic == PROTOCOL_MAGIC &&
           header.version == PROTOCOL_VERSION &&
           header.fragmentIndex < header.fragmentCount &&
           header.payloadSize <= MAX_UDP_PAYLOAD;
}

std::string Protocol::describe(const PacketHeader& header) {
    std::ostringstream ss;
    ss << "PacketHeader { ";
    ss << "magic=0x" << std::hex << header.magic << std::dec;
    ss << ", v" << static_cast<int>(header.version);
    ss << ", type=" << (header.mediaType == 0 ? "video" : "audio");
    if (header.mediaType == 0 && header.isKeyframe()) {
        ss << " [KEY]";
    }
    ss << ", seq=" << header.sequenceNumber;
    ss << ", ts=" << header.timestamp;
    ss << ", size=" << header.totalSize;
    ss << ", frag=" << header.fragmentIndex << "/" << header.fragmentCount;
    ss << ", payload=" << header.payloadSize;
    if (header.mediaType == 1) {
        ss << ", rate=" << header.sampleRate;
        ss << ", ch=" << static_cast<int>(header.channels);
    }
    ss << " }";
    return ss.str();
}

uint16_t Protocol::calculateFragmentCount(uint32_t totalSize) {
    return static_cast<uint16_t>((totalSize + MAX_UDP_PAYLOAD - 1) / MAX_UDP_PAYLOAD);
}

uint64_t Protocol::nsToTimestamp(uint64_t nanoseconds) {
    // Convert nanoseconds to 10M ticks/sec
    // 1 second = 1,000,000,000 ns = 10,000,000 ticks
    // ticks = ns * 10,000,000 / 1,000,000,000 = ns / 100
    return nanoseconds / 100;
}

uint64_t Protocol::timestampToNs(uint64_t timestamp) {
    // Convert 10M ticks/sec to nanoseconds
    return timestamp * 100;
}

// FrameReassembler implementation

std::optional<FrameReassembler::Frame> FrameReassembler::addPacket(
    const PacketHeader& header,
    const uint8_t* payload,
    size_t payloadSize
) {
    stats_.packetsReceived++;

    // Start new frame if sequence number changed or no pending frame
    if (!pending_.has_value() || pending_->sequenceNumber != header.sequenceNumber) {
        // If we had a pending frame, it's now dropped
        if (pending_.has_value() && pending_->receivedCount < pending_->fragmentCount) {
            stats_.framesDropped++;
            stats_.totalFragmentsReceivedBeforeDrop += pending_->receivedCount;
            stats_.totalFragmentsExpectedBeforeDrop += pending_->fragmentCount;
            Logger::instance().debugf("DROPPED frame seq=%u: got %u/%u fragments (%.0f%%)",
                pending_->sequenceNumber, pending_->receivedCount, pending_->fragmentCount,
                100.0 * pending_->receivedCount / pending_->fragmentCount);
        }

        // Initialize new pending frame
        PendingFrame pf;
        pf.type = static_cast<MediaType>(header.mediaType);
        pf.sequenceNumber = header.sequenceNumber;
        pf.timestamp = header.timestamp;
        pf.totalSize = header.totalSize;
        pf.fragmentCount = header.fragmentCount;
        pf.flags = header.flags;
        pf.sampleRate = header.sampleRate;
        pf.channels = header.channels;
        pf.received.resize(header.fragmentCount, false);
        pf.data.resize(header.totalSize, 0);
        pf.receivedCount = 0;
        pending_ = std::move(pf);
    }

    auto& pf = pending_.value();

    // Validate fragment
    if (header.fragmentIndex >= pf.fragmentCount) {
        return std::nullopt;
    }

    // Check for duplicate
    if (pf.received[header.fragmentIndex]) {
        stats_.packetsDuplicate++;
        return std::nullopt;
    }

    // Copy payload data
    size_t offset = static_cast<size_t>(header.fragmentIndex) * MAX_UDP_PAYLOAD;
    size_t copySize = std::min(payloadSize, static_cast<size_t>(header.payloadSize));
    if (offset + copySize <= pf.data.size()) {
        std::memcpy(pf.data.data() + offset, payload, copySize);
        pf.received[header.fragmentIndex] = true;
        pf.receivedCount++;
    }

    // Check if frame is complete
    if (pf.receivedCount == pf.fragmentCount) {
        Frame frame;
        frame.type = pf.type;
        frame.sequenceNumber = pf.sequenceNumber;
        frame.timestamp = pf.timestamp;
        frame.data = std::move(pf.data);
        frame.isKeyframe = (pf.flags & 0x01) != 0;
        frame.sampleRate = pf.sampleRate;
        frame.channels = pf.channels;

        pending_.reset();
        stats_.framesCompleted++;

        return frame;
    }

    return std::nullopt;
}

void FrameReassembler::reset() {
    pending_.reset();
    stats_ = Stats{};
}

} // namespace ndi_bridge
