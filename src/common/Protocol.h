#pragma once

/**
 * Protocol.h - NDI Bridge UDP Protocol Header
 *
 * 38-byte Big-Endian header format compatible with macOS Swift version.
 * This header precedes all UDP packets in the NDI Bridge protocol.
 *
 * Layout (MUST match Swift MediaPacketHeader exactly):
 *   Offset | Field          | Type   | Description
 *   -------|----------------|--------|---------------------------
 *   0-3    | magic          | U32    | 0x4E444942 ("NDIB")
 *   4      | version        | U8     | Protocol version (2)
 *   5      | mediaType      | U8     | 0=video, 1=audio
 *   6      | sourceId       | U8     | Source ID (multi-source future)
 *   7      | flags          | U8     | Bit 0 = keyframe (video)
 *   8-11   | sequenceNumber | U32    | Frame sequence number
 *   12-19  | timestamp      | U64    | PTS (10,000,000 ticks/sec)
 *   20-23  | totalSize      | U32    | Total frame size in bytes
 *   24-25  | fragmentIndex  | U16    | Current fragment (0-based)
 *   26-27  | fragmentCount  | U16    | Total fragments
 *   28-29  | payloadSize    | U16    | Payload size in this packet
 *   30-33  | sampleRate     | U32    | Audio: sample rate (48000)
 *   34     | channels       | U8     | Audio: channel count (2)
 *   35-37  | reserved       | U8[3]  | Padding to 38 bytes
 */

#include <cstdint>
#include <cstring>
#include <vector>
#include <optional>
#include <string>

namespace ndi_bridge {

// Protocol constants
constexpr uint32_t PROTOCOL_MAGIC = 0x4E444942;  // "NDIB"
constexpr uint8_t  PROTOCOL_VERSION = 2;
constexpr size_t   HEADER_SIZE = 38;
constexpr size_t   DEFAULT_MTU = 1200;           // Safe for WireGuard/Tailscale (MTU ~1280)
constexpr size_t   MAX_UDP_PAYLOAD = DEFAULT_MTU - HEADER_SIZE;  // 1362 bytes
constexpr size_t   MAX_PACKET_SIZE = DEFAULT_MTU;

// Timestamp resolution: 10,000,000 ticks per second (same as NDI)
constexpr uint64_t TIMESTAMP_RESOLUTION = 10000000;

enum class MediaType : uint8_t {
    Video = 0,
    Audio = 1
};

/**
 * PacketHeader - 38-byte protocol header
 *
 * All multi-byte fields are stored in Big-Endian (network byte order).
 * This structure MUST match Swift MediaPacketHeader for cross-platform compatibility.
 */
#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;           // 0-3:   "NDIB" (0x4E444942)
    uint8_t  version;         // 4:     Protocol version
    uint8_t  mediaType;       // 5:     0=video, 1=audio
    uint8_t  sourceId;        // 6:     Source ID (for multi-source)
    uint8_t  flags;           // 7:     Bit 0 = keyframe (video)
    uint32_t sequenceNumber;  // 8-11:  Frame sequence
    uint64_t timestamp;       // 12-19: PTS (10M ticks/sec)
    uint32_t totalSize;       // 20-23: Total frame size
    uint16_t fragmentIndex;   // 24-25: Fragment index (0-based)
    uint16_t fragmentCount;   // 26-27: Total fragments
    uint16_t payloadSize;     // 28-29: This packet's payload size
    uint32_t sampleRate;      // 30-33: Audio sample rate
    uint8_t  channels;        // 34:    Audio channels
    uint8_t  reserved[3];     // 35-37: Reserved (padding)

    // Helper methods
    bool isKeyframe() const { return (flags & 0x01) != 0; }
    bool isVideo() const { return mediaType == static_cast<uint8_t>(MediaType::Video); }
    bool isAudio() const { return mediaType == static_cast<uint8_t>(MediaType::Audio); }
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == HEADER_SIZE,
              "PacketHeader must be exactly 38 bytes");

/**
 * Byte-order conversion utilities (host <-> network/big-endian)
 */
namespace endian {

inline uint16_t hton16(uint16_t value) {
    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        return __builtin_bswap16(value);
    #else
        return value;
    #endif
}

inline uint32_t hton32(uint32_t value) {
    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        return __builtin_bswap32(value);
    #else
        return value;
    #endif
}

inline uint64_t hton64(uint64_t value) {
    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        return __builtin_bswap64(value);
    #else
        return value;
    #endif
}

inline uint16_t ntoh16(uint16_t value) { return hton16(value); }
inline uint32_t ntoh32(uint32_t value) { return hton32(value); }
inline uint64_t ntoh64(uint64_t value) { return hton64(value); }

} // namespace endian

/**
 * Protocol - Serialize/deserialize packet headers
 */
class Protocol {
public:
    /**
     * Create a packet header for video data
     */
    static PacketHeader createVideoHeader(
        uint32_t sequenceNumber,
        uint64_t timestamp,
        uint32_t totalSize,
        uint16_t fragmentIndex,
        uint16_t fragmentCount,
        uint16_t payloadSize,
        bool isKeyframe = false
    );

    /**
     * Create a packet header for audio data
     */
    static PacketHeader createAudioHeader(
        uint32_t sequenceNumber,
        uint64_t timestamp,
        uint32_t totalSize,
        uint16_t fragmentIndex,
        uint16_t fragmentCount,
        uint16_t payloadSize,
        uint32_t sampleRate,
        uint8_t channels
    );

    /**
     * Serialize header to network byte order (Big-Endian)
     */
    static std::vector<uint8_t> serialize(const PacketHeader& header);

    /**
     * Serialize header directly into a buffer
     * @param header The header to serialize
     * @param buffer Destination buffer (must be at least HEADER_SIZE bytes)
     */
    static void serializeInto(const PacketHeader& header, uint8_t* buffer);

    /**
     * Deserialize header from network byte order
     * @return Header if valid, nullopt if invalid (wrong magic/version)
     */
    static std::optional<PacketHeader> deserialize(const uint8_t* data, size_t size);

    /**
     * Validate a packet header
     */
    static bool isValid(const PacketHeader& header);

    /**
     * Get human-readable description
     */
    static std::string describe(const PacketHeader& header);

    /**
     * Calculate number of fragments needed for a frame
     */
    static uint16_t calculateFragmentCount(uint32_t totalSize);

    /**
     * Convert nanoseconds to protocol timestamp (10M ticks/sec)
     */
    static uint64_t nsToTimestamp(uint64_t nanoseconds);

    /**
     * Convert protocol timestamp to nanoseconds
     */
    static uint64_t timestampToNs(uint64_t timestamp);
};

/**
 * FrameReassembler - Reassemble fragmented frames
 *
 * Handles out-of-order packets and detects missing fragments.
 */
class FrameReassembler {
public:
    struct Frame {
        MediaType type;
        uint32_t sequenceNumber;
        uint64_t timestamp;
        std::vector<uint8_t> data;
        bool isKeyframe;      // Video only
        uint32_t sampleRate;  // Audio only
        uint8_t channels;     // Audio only
    };

    /**
     * Add a received packet
     * @return Complete frame if reassembly finished, nullopt otherwise
     */
    std::optional<Frame> addPacket(const PacketHeader& header,
                                   const uint8_t* payload,
                                   size_t payloadSize);

    /**
     * Reset reassembler state
     */
    void reset();

    /**
     * Get statistics
     */
    struct Stats {
        uint64_t framesCompleted = 0;
        uint64_t framesDropped = 0;
        uint64_t packetsReceived = 0;
        uint64_t packetsDuplicate = 0;
    };
    Stats getStats() const { return stats_; }

private:
    struct PendingFrame {
        MediaType type;
        uint32_t sequenceNumber;
        uint64_t timestamp;
        uint32_t totalSize;
        uint16_t fragmentCount;
        uint8_t flags;
        uint32_t sampleRate;
        uint8_t channels;
        std::vector<bool> received;
        std::vector<uint8_t> data;
        uint16_t receivedCount = 0;
    };

    std::optional<PendingFrame> pending_;
    Stats stats_;
};

} // namespace ndi_bridge
