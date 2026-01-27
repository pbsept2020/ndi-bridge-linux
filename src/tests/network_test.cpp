/**
 * network_test.cpp - Loopback test for NetworkSender and NetworkReceiver
 *
 * Tests UDP packet send/receive with fragmentation and reassembly.
 * Verifies protocol compatibility with macOS Swift version.
 */

#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstring>

#include "common/Logger.h"
#include "common/Protocol.h"
#include "network/NetworkSender.h"
#include "network/NetworkReceiver.h"

using namespace ndi_bridge;

std::atomic<int> videoFramesReceived{0};
std::atomic<int> audioFramesReceived{0};
std::atomic<bool> testPassed{true};

// Expected test data
std::vector<uint8_t> expectedVideoData;
std::vector<uint8_t> expectedAudioData;

void onVideoFrame(const ReceivedVideoFrame& frame) {
    Logger::instance().successf("Received VIDEO frame: seq=%u, size=%zu, keyframe=%s, ts=%lu",
                                frame.sequenceNumber, frame.data.size(),
                                frame.isKeyframe ? "YES" : "NO", frame.timestamp);

    // Verify data
    if (frame.data.size() != expectedVideoData.size()) {
        Logger::instance().errorf("Video size mismatch: expected %zu, got %zu",
                                  expectedVideoData.size(), frame.data.size());
        testPassed = false;
    } else if (std::memcmp(frame.data.data(), expectedVideoData.data(), frame.data.size()) != 0) {
        LOG_ERROR("Video data mismatch!");
        testPassed = false;
    } else {
        LOG_SUCCESS("Video data verified OK");
    }

    if (!frame.isKeyframe) {
        LOG_ERROR("Expected keyframe flag to be true");
        testPassed = false;
    }

    videoFramesReceived++;
}

void onAudioFrame(const ReceivedAudioFrame& frame) {
    Logger::instance().successf("Received AUDIO frame: seq=%u, size=%zu, rate=%u, ch=%u, ts=%lu",
                                frame.sequenceNumber, frame.data.size(),
                                frame.sampleRate, frame.channels, frame.timestamp);

    // Verify data
    if (frame.data.size() != expectedAudioData.size()) {
        Logger::instance().errorf("Audio size mismatch: expected %zu, got %zu",
                                  expectedAudioData.size(), frame.data.size());
        testPassed = false;
    } else if (std::memcmp(frame.data.data(), expectedAudioData.data(), frame.data.size()) != 0) {
        LOG_ERROR("Audio data mismatch!");
        testPassed = false;
    } else {
        LOG_SUCCESS("Audio data verified OK");
    }

    if (frame.sampleRate != 48000) {
        Logger::instance().errorf("Expected sampleRate 48000, got %u", frame.sampleRate);
        testPassed = false;
    }

    if (frame.channels != 2) {
        Logger::instance().errorf("Expected channels 2, got %u", frame.channels);
        testPassed = false;
    }

    audioFramesReceived++;
}

int main() {
    Logger::instance().setVerbose(true);

    std::cout << "\n";
    LOG_INFO("=== NDI Bridge Network Loopback Test ===");
    std::cout << "\n";

    // Test 1: Protocol header serialization
    LOG_INFO("Test 1: Protocol header serialization");
    {
        PacketHeader header = Protocol::createVideoHeader(42, 123456789, 5000, 2, 5, 1000, true);
        auto serialized = Protocol::serialize(header);

        if (serialized.size() != HEADER_SIZE) {
            Logger::instance().errorf("Header size mismatch: expected %zu, got %zu",
                                      HEADER_SIZE, serialized.size());
            return 1;
        }

        auto deserialized = Protocol::deserialize(serialized.data(), serialized.size());
        if (!deserialized) {
            LOG_ERROR("Failed to deserialize header");
            return 1;
        }

        if (deserialized->magic != PROTOCOL_MAGIC ||
            deserialized->version != PROTOCOL_VERSION ||
            deserialized->sequenceNumber != 42 ||
            deserialized->timestamp != 123456789 ||
            deserialized->totalSize != 5000 ||
            deserialized->fragmentIndex != 2 ||
            deserialized->fragmentCount != 5 ||
            deserialized->payloadSize != 1000 ||
            !deserialized->isKeyframe()) {
            LOG_ERROR("Deserialized header values mismatch");
            LOG_INFO(Protocol::describe(*deserialized).c_str());
            return 1;
        }

        LOG_SUCCESS("Protocol serialization OK");
    }

    std::cout << "\n";

    // Test 2: Loopback send/receive
    LOG_INFO("Test 2: UDP loopback send/receive");

    const uint16_t testPort = 15990;  // Use non-standard port to avoid conflicts

    // Create test data (larger than MTU to test fragmentation)
    // Video: 5000 bytes (will be fragmented into 4 packets)
    expectedVideoData.resize(5000);
    for (size_t i = 0; i < expectedVideoData.size(); i++) {
        expectedVideoData[i] = static_cast<uint8_t>(i % 256);
    }

    // Audio: 8192 bytes (typical audio frame, will be fragmented into 6 packets)
    expectedAudioData.resize(8192);
    for (size_t i = 0; i < expectedAudioData.size(); i++) {
        expectedAudioData[i] = static_cast<uint8_t>((i * 7) % 256);
    }

    Logger::instance().infof("Video test data: %zu bytes (%u fragments)",
                             expectedVideoData.size(),
                             Protocol::calculateFragmentCount(static_cast<uint32_t>(expectedVideoData.size())));
    Logger::instance().infof("Audio test data: %zu bytes (%u fragments)",
                             expectedAudioData.size(),
                             Protocol::calculateFragmentCount(static_cast<uint32_t>(expectedAudioData.size())));

    // Start receiver
    NetworkReceiverConfig recvConfig;
    recvConfig.port = testPort;
    NetworkReceiver receiver(recvConfig);

    receiver.setOnVideoFrame(onVideoFrame);
    receiver.setOnAudioFrame(onAudioFrame);
    receiver.setOnError([](const std::string& error) {
        Logger::instance().errorf("Receiver error: %s", error.c_str());
        testPassed = false;
    });

    if (!receiver.startListening()) {
        LOG_ERROR("Failed to start receiver");
        return 1;
    }

    // Give receiver time to bind
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Start sender
    NetworkSenderConfig sendConfig;
    sendConfig.host = "127.0.0.1";
    sendConfig.port = testPort;
    NetworkSender sender(sendConfig);

    if (!sender.connect()) {
        LOG_ERROR("Failed to connect sender");
        receiver.stop();
        return 1;
    }

    // Send video frame (keyframe)
    LOG_INFO("Sending video frame...");
    uint64_t videoTimestamp = 1000000;  // 100ms in 10M ticks/sec
    if (!sender.sendVideo(expectedVideoData.data(), expectedVideoData.size(), true, videoTimestamp)) {
        LOG_ERROR("Failed to send video");
        testPassed = false;
    }

    // Small delay between sends
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send audio frame
    LOG_INFO("Sending audio frame...");
    uint64_t audioTimestamp = 1000000;
    if (!sender.sendAudio(expectedAudioData.data(), expectedAudioData.size(), audioTimestamp, 48000, 2)) {
        LOG_ERROR("Failed to send audio");
        testPassed = false;
    }

    // Wait for frames to be received
    LOG_INFO("Waiting for frames...");
    for (int i = 0; i < 20; i++) {  // Wait up to 2 seconds
        if (videoFramesReceived >= 1 && audioFramesReceived >= 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Cleanup
    sender.disconnect();
    receiver.stop();

    std::cout << "\n";

    // Report results
    LOG_INFO("=== Test Results ===");

    auto senderStats = sender.getStats();
    auto receiverStats = receiver.getStats();

    Logger::instance().infof("Sender: %lu bytes, %lu packets, %lu frames",
                             senderStats.bytesSent, senderStats.packetsSent, senderStats.framesSent);
    Logger::instance().infof("Receiver: %lu bytes, %lu packets, %lu video frames, %lu audio frames",
                             receiverStats.bytesReceived, receiverStats.packetsReceived,
                             receiverStats.videoFramesReceived, receiverStats.audioFramesReceived);

    if (videoFramesReceived < 1) {
        LOG_ERROR("No video frames received!");
        testPassed = false;
    }

    if (audioFramesReceived < 1) {
        LOG_ERROR("No audio frames received!");
        testPassed = false;
    }

    std::cout << "\n";

    if (testPassed) {
        LOG_SUCCESS("=== ALL TESTS PASSED ===");
        return 0;
    } else {
        LOG_ERROR("=== TESTS FAILED ===");
        return 1;
    }
}
