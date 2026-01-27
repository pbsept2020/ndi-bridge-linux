/**
 * codec_test.cpp - Encode/Decode roundtrip test
 *
 * Tests the full pipeline: BGRA → H.264 encode → H.264 decode → BGRA
 * Verifies dimensions and basic data integrity.
 */

#include <iostream>
#include <cstring>
#include <chrono>
#include <cmath>

#include "common/Logger.h"
#include "common/Protocol.h"

#ifdef HAVE_FFMPEG
#include "video/VideoEncoder.h"
#include "video/VideoDecoder.h"
#endif

using namespace ndi_bridge;

// Test state
int framesEncoded = 0;
int framesDecoded = 0;
bool dimensionsMatch = false;
int decodedWidth = 0;
int decodedHeight = 0;

#ifdef HAVE_FFMPEG

// Store encoded frames for decoding
std::vector<EncodedFrame> encodedFrames;

void onEncodedFrame(const EncodedFrame& frame) {
    framesEncoded++;
    encodedFrames.push_back(frame);

    Logger::instance().debugf("Encoded frame %d: %zu bytes, keyframe=%s",
                              framesEncoded, frame.data.size(),
                              frame.isKeyframe ? "YES" : "NO");
}

void onDecodedFrame(const DecodedFrame& frame) {
    framesDecoded++;
    decodedWidth = frame.width;
    decodedHeight = frame.height;

    Logger::instance().debugf("Decoded frame %d: %dx%d, %zu bytes, stride=%d",
                              framesDecoded, frame.width, frame.height,
                              frame.data.size(), frame.stride);
}

#endif

int main() {
    Logger::instance().setVerbose(true);

    std::cout << "\n";
    LOG_INFO("=== NDI Bridge Codec Roundtrip Test ===");
    std::cout << "\n";

#ifndef HAVE_FFMPEG
    LOG_ERROR("FFmpeg not available - codec test skipped");
    return 1;
#else

    // Test parameters
    const int width = 1280;
    const int height = 720;
    const int numFrames = 5;

    // Create test frame with gradient (BGRA format)
    std::vector<uint8_t> testFrame(width * height * 4);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            testFrame[idx + 0] = static_cast<uint8_t>(x * 255 / width);      // B
            testFrame[idx + 1] = static_cast<uint8_t>(y * 255 / height);     // G
            testFrame[idx + 2] = 128;                                         // R
            testFrame[idx + 3] = 255;                                         // A
        }
    }

    Logger::instance().infof("Test frame: %dx%d BGRA (%zu bytes)", width, height, testFrame.size());

    // ========== ENCODING ==========
    LOG_INFO("--- Encoding Phase ---");

    VideoEncoderConfig encConfig;
    encConfig.width = width;
    encConfig.height = height;
    encConfig.bitrate = 4000000;  // 4 Mbps
    encConfig.fps = 30;
    encConfig.keyframeInterval = 2;  // Keyframe every 2 frames for testing
    encConfig.inputFormat = PixelFormat::BGRA;

    VideoEncoder encoder;
    encoder.setOnEncodedFrame(onEncodedFrame);
    encoder.setOnError([](const std::string& error) {
        Logger::instance().errorf("Encoder error: %s", error.c_str());
    });

    if (!encoder.configure(encConfig)) {
        LOG_ERROR("Failed to configure encoder");
        return 1;
    }

    // Encode frames
    auto startEncode = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < numFrames; i++) {
        // Modify frame slightly for variation
        for (int y = 0; y < 50; y++) {
            for (int x = 0; x < 50; x++) {
                int idx = (y * width + x) * 4;
                testFrame[idx + 2] = static_cast<uint8_t>((i * 50) % 256);  // R
            }
        }

        uint64_t timestamp = i * (TIMESTAMP_RESOLUTION / encConfig.fps);

        if (!encoder.encode(testFrame.data(), testFrame.size(), timestamp)) {
            Logger::instance().errorf("Failed to encode frame %d", i);
            return 1;
        }
    }

    encoder.flush();

    auto endEncode = std::chrono::high_resolution_clock::now();
    auto encodeDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endEncode - startEncode);

    Logger::instance().successf("Encoded %d frames in %ld ms", framesEncoded, encodeDuration.count());

    if (encodedFrames.empty()) {
        LOG_ERROR("No frames were encoded!");
        return 1;
    }

    // ========== DECODING ==========
    std::cout << "\n";
    LOG_INFO("--- Decoding Phase ---");

    VideoDecoderConfig decConfig;
    decConfig.outputFormat = OutputPixelFormat::BGRA;

    VideoDecoder decoder;
    decoder.setOnDecodedFrame(onDecodedFrame);
    decoder.setOnError([](const std::string& error) {
        Logger::instance().errorf("Decoder error: %s", error.c_str());
    });

    if (!decoder.configure(decConfig)) {
        LOG_ERROR("Failed to configure decoder");
        return 1;
    }

    // Decode all encoded frames
    auto startDecode = std::chrono::high_resolution_clock::now();

    for (const auto& frame : encodedFrames) {
        if (!decoder.decode(frame.data.data(), frame.data.size(), frame.timestamp)) {
            LOG_ERROR("Failed to decode frame");
            return 1;
        }
    }

    decoder.flush();

    auto endDecode = std::chrono::high_resolution_clock::now();
    auto decodeDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endDecode - startDecode);

    Logger::instance().successf("Decoded %d frames in %ld ms", framesDecoded, decodeDuration.count());

    // ========== RESULTS ==========
    std::cout << "\n";
    LOG_INFO("=== Test Results ===");

    auto encStats = encoder.getStats();
    auto decStats = decoder.getStats();

    Logger::instance().infof("Encoder: %lu frames, %lu keyframes, %lu bytes",
                             encStats.framesEncoded, encStats.keyframesEncoded, encStats.bytesEncoded);
    Logger::instance().infof("Decoder: %lu frames, %lu keyframes, %lu errors",
                             decStats.framesDecoded, decStats.keyframesDecoded, decStats.decodeErrors);
    Logger::instance().infof("Decoded dimensions: %dx%d", decodedWidth, decodedHeight);

    // Verify results
    bool testPassed = true;

    if (framesDecoded == 0) {
        LOG_ERROR("No frames were decoded!");
        testPassed = false;
    }

    if (decodedWidth != width) {
        Logger::instance().errorf("Width mismatch: expected %d, got %d", width, decodedWidth);
        testPassed = false;
    }

    if (decodedHeight != height) {
        Logger::instance().errorf("Height mismatch: expected %d, got %d", height, decodedHeight);
        testPassed = false;
    }

    if (decStats.decodeErrors > 0) {
        Logger::instance().errorf("Decoder had %lu errors", decStats.decodeErrors);
        testPassed = false;
    }

    // Check that we decoded most frames (some may be buffered)
    if (framesDecoded < framesEncoded - 1) {
        Logger::instance().errorf("Too few frames decoded: %d encoded, %d decoded",
                                  framesEncoded, framesDecoded);
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

#endif // HAVE_FFMPEG
}
