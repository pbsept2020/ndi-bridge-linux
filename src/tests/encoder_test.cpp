/**
 * encoder_test.cpp - Test for VideoEncoder
 *
 * Tests H.264 encoding with FFmpeg/libx264.
 * Generates a dummy frame and verifies output is valid H.264 Annex-B.
 */

#include <iostream>
#include <cstring>
#include <chrono>

#include "common/Logger.h"
#include "common/Protocol.h"

#ifdef HAVE_FFMPEG
#include "video/VideoEncoder.h"
#endif

using namespace ndi_bridge;

// Test results
int framesEncoded = 0;
int keyframesEncoded = 0;
size_t totalBytesEncoded = 0;
bool hasValidStartCode = false;

#ifdef HAVE_FFMPEG
void onEncodedFrame(const EncodedFrame& frame) {
    framesEncoded++;
    totalBytesEncoded += frame.data.size();

    if (frame.isKeyframe) {
        keyframesEncoded++;
    }

    // Check for Annex-B start code (00 00 00 01 or 00 00 01)
    if (frame.data.size() >= 4) {
        if (frame.data[0] == 0x00 && frame.data[1] == 0x00) {
            if (frame.data[2] == 0x01 ||
                (frame.data[2] == 0x00 && frame.data[3] == 0x01)) {
                hasValidStartCode = true;
            }
        }
    }

    // Parse NAL units
    int nalCount = 0;
    for (size_t i = 0; i + 4 < frame.data.size(); i++) {
        if (frame.data[i] == 0x00 && frame.data[i+1] == 0x00 &&
            frame.data[i+2] == 0x00 && frame.data[i+3] == 0x01) {
            nalCount++;
            uint8_t nalType = frame.data[i+4] & 0x1F;

            const char* typeName = "?";
            switch (nalType) {
                case 1: typeName = "non-IDR"; break;
                case 5: typeName = "IDR"; break;
                case 6: typeName = "SEI"; break;
                case 7: typeName = "SPS"; break;
                case 8: typeName = "PPS"; break;
                case 9: typeName = "AUD"; break;
            }

            Logger::instance().debugf("  NAL #%d: type=%d (%s)", nalCount, nalType, typeName);
        }
    }

    Logger::instance().successf("Encoded frame %d: %zu bytes, keyframe=%s, NALs=%d, ts=%lu",
                                framesEncoded, frame.data.size(),
                                frame.isKeyframe ? "YES" : "NO",
                                nalCount, frame.timestamp);
}
#endif

int main() {
    Logger::instance().setVerbose(true);

    std::cout << "\n";
    LOG_INFO("=== NDI Bridge Video Encoder Test ===");
    std::cout << "\n";

#ifndef HAVE_FFMPEG
    LOG_ERROR("FFmpeg not available - encoder test skipped");
    LOG_INFO("Install FFmpeg with: sudo apt install libavcodec-dev libavutil-dev libswscale-dev");
    return 1;
#else

    // Test configuration
    const int width = 1920;
    const int height = 1080;
    const int numFrames = 10;

    // Create test frame with color gradient (BGRA format)
    std::vector<uint8_t> testFrame(width * height * 4);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            testFrame[idx + 0] = static_cast<uint8_t>(x * 255 / width);      // B
            testFrame[idx + 1] = static_cast<uint8_t>(y * 255 / height);     // G
            testFrame[idx + 2] = static_cast<uint8_t>((x + y) * 255 / (width + height)); // R
            testFrame[idx + 3] = 255;  // A
        }
    }

    Logger::instance().infof("Test frame: %dx%d BGRA (%zu bytes)", width, height, testFrame.size());

    // Configure encoder
    VideoEncoderConfig config;
    config.width = width;
    config.height = height;
    config.bitrate = 8000000;  // 8 Mbps
    config.fps = 30;
    config.keyframeInterval = 5;  // Keyframe every 5 frames for testing
    config.inputFormat = PixelFormat::BGRA;

    VideoEncoder encoder;
    encoder.setOnEncodedFrame(onEncodedFrame);
    encoder.setOnError([](const std::string& error) {
        Logger::instance().errorf("Encoder error: %s", error.c_str());
    });

    LOG_INFO("Configuring encoder...");
    if (!encoder.configure(config)) {
        LOG_ERROR("Failed to configure encoder");
        return 1;
    }

    // Encode multiple frames
    LOG_INFO("Encoding frames...");
    auto startTime = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < numFrames; i++) {
        // Modify frame slightly to create variation
        for (int y = 0; y < 100; y++) {
            for (int x = 0; x < 100; x++) {
                int idx = (y * width + x) * 4;
                testFrame[idx + 0] = static_cast<uint8_t>((i * 25) % 256);
                testFrame[idx + 1] = static_cast<uint8_t>((i * 50) % 256);
            }
        }

        uint64_t timestamp = i * (TIMESTAMP_RESOLUTION / config.fps);  // Frame timestamp

        if (!encoder.encode(testFrame.data(), testFrame.size(), timestamp)) {
            Logger::instance().errorf("Failed to encode frame %d", i);
            return 1;
        }
    }

    // Flush encoder
    LOG_INFO("Flushing encoder...");
    encoder.flush();

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    std::cout << "\n";
    LOG_INFO("=== Test Results ===");

    auto stats = encoder.getStats();
    Logger::instance().infof("Frames encoded: %lu", stats.framesEncoded);
    Logger::instance().infof("Keyframes: %lu", stats.keyframesEncoded);
    Logger::instance().infof("Total bytes: %lu", stats.bytesEncoded);
    Logger::instance().infof("Encoding time: %ld ms", duration.count());

    if (stats.framesEncoded > 0) {
        double avgBitrate = static_cast<double>(stats.bytesEncoded * 8) /
                           (static_cast<double>(stats.framesEncoded) / config.fps);
        Logger::instance().infof("Average bitrate: %.2f Mbps", avgBitrate / 1000000);
    }

    std::cout << "\n";

    // Verify results
    bool testPassed = true;

    if (framesEncoded != numFrames) {
        Logger::instance().errorf("Expected %d frames, got %d", numFrames, framesEncoded);
        testPassed = false;
    }

    if (keyframesEncoded < 2) {  // Should have at least 2 keyframes (frame 0, frame 5)
        Logger::instance().errorf("Expected at least 2 keyframes, got %d", keyframesEncoded);
        testPassed = false;
    }

    if (!hasValidStartCode) {
        LOG_ERROR("No valid Annex-B start codes found");
        testPassed = false;
    }

    if (totalBytesEncoded == 0) {
        LOG_ERROR("No data was encoded");
        testPassed = false;
    }

    if (testPassed) {
        LOG_SUCCESS("=== ALL TESTS PASSED ===");
        return 0;
    } else {
        LOG_ERROR("=== TESTS FAILED ===");
        return 1;
    }

#endif // HAVE_FFMPEG
}
