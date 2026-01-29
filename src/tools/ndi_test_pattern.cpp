/**
 * ndi_test_pattern - Generate NDI test pattern source
 *
 * Sends a bouncing ball on a dark background + optional 440Hz tone
 * as an NDI source on the local network.
 * The ball moves at constant speed — any stutter or jump = dropped frames.
 *
 * Usage:
 *   ndi_test_pattern [--name "Source Name"] [--resolution WxH] [--fps N]
 *
 * TODO (future): move all tools to a dedicated ndi-tools project or
 * reorganize src/tools/ with shared utilities if this grows.
 */

#include <cstddef>
#include <Processing.NDI.Lib.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <csignal>
#include <chrono>
#include <thread>
#include <vector>
#include <string>

#include "common/Version.h"

static volatile bool g_running = true;
static void signalHandler(int) { g_running = false; }

// Ball state
struct Ball {
    float x, y;     // center position
    float vx, vy;   // velocity in pixels per frame
    int radius;

    void init(int width, int height) {
        radius = height / 20;  // ~54px at 1080p
        x = static_cast<float>(width) / 4.0f;
        y = static_cast<float>(height) / 2.0f;
        vx = 6.0f;   // pixels per frame (~180px/s at 30fps)
        vy = 3.5f;
    }

    void update(int width, int height) {
        x += vx;
        y += vy;

        // Bounce off walls
        if (x - radius <= 0)      { x = static_cast<float>(radius); vx = -vx; }
        if (x + radius >= width)  { x = static_cast<float>(width - radius); vx = -vx; }
        if (y - radius <= 0)      { y = static_cast<float>(radius); vy = -vy; }
        if (y + radius >= height) { y = static_cast<float>(height - radius); vy = -vy; }
    }
};

static void generateFrame(uint8_t* data, int width, int height, int stride,
                           Ball& ball, int frameNum) {
    // Dark gray background (not pure black — easier to see on monitors)
    for (int y = 0; y < height; y++) {
        uint8_t* row = data + y * stride;
        for (int x = 0; x < width; x++) {
            row[x * 4 + 0] = 30;   // B
            row[x * 4 + 1] = 30;   // G
            row[x * 4 + 2] = 30;   // R
            row[x * 4 + 3] = 255;  // A
        }
    }

    // Draw ball (filled circle, white with green tint)
    int r2 = ball.radius * ball.radius;
    int bx = static_cast<int>(ball.x);
    int by = static_cast<int>(ball.y);

    int yMin = by - ball.radius; if (yMin < 0) yMin = 0;
    int yMax = by + ball.radius; if (yMax >= height) yMax = height - 1;
    int xMin = bx - ball.radius; if (xMin < 0) xMin = 0;
    int xMax = bx + ball.radius; if (xMax >= width) xMax = width - 1;

    for (int y = yMin; y <= yMax; y++) {
        uint8_t* row = data + y * stride;
        int dy = y - by;
        for (int x = xMin; x <= xMax; x++) {
            int dx = x - bx;
            int dist2 = dx * dx + dy * dy;
            if (dist2 <= r2) {
                // Smooth edge (simple antialiasing)
                float edge = static_cast<float>(r2 - dist2) / static_cast<float>(ball.radius * 4);
                if (edge > 1.0f) edge = 1.0f;
                uint8_t a = static_cast<uint8_t>(edge * 255);
                // Bright green ball
                row[x * 4 + 0] = static_cast<uint8_t>(50  * edge);  // B
                row[x * 4 + 1] = static_cast<uint8_t>(255 * edge);  // G
                row[x * 4 + 2] = static_cast<uint8_t>(100 * edge);  // R
                row[x * 4 + 3] = a;
            }
        }
    }

    // Frame counter in top-left: small white square that toggles every frame
    // (flicker = frames are arriving, frozen = stalled)
    if (frameNum % 2 == 0) {
        for (int y = 10; y < 26; y++) {
            uint8_t* row = data + y * stride;
            for (int x = 10; x < 26; x++) {
                row[x * 4 + 0] = 255;
                row[x * 4 + 1] = 255;
                row[x * 4 + 2] = 255;
                row[x * 4 + 3] = 255;
            }
        }
    }

    ball.update(width, height);
}

static void generateAudio(float* data, int sampleRate, int numSamples,
                           int channels, uint64_t& phase) {
    const double freq = 440.0;
    const double amplitude = 0.3;

    for (int s = 0; s < numSamples; s++) {
        float sample = static_cast<float>(
            amplitude * sin(2.0 * M_PI * freq * static_cast<double>(phase) / sampleRate)
        );
        for (int ch = 0; ch < channels; ch++) {
            data[ch * numSamples + s] = sample;
        }
        phase++;
    }
}

static void printUsage(const char* prog) {
    printf("\n"
           "NDI Test Pattern Generator %s\n"
           "\n"
           "Usage: %s [options]\n"
           "\n"
           "Options:\n"
           "  --name <name>         NDI source name (default: Test Pattern)\n"
           "  --resolution <WxH>    Resolution (default: 1920x1080)\n"
           "  --fps <N>             Frame rate (default: 30)\n"
           "  --no-audio            Disable audio tone\n"
           "  -h, --help            Show this help\n"
           "\n"
           "Bouncing green ball on dark background.\n"
           "Any stutter or jump in the ball = dropped frames.\n"
           "Flickering white square (top-left) = frames arriving.\n"
           "\n",
           NDI_BRIDGE_VERSION, prog);
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::string name = "Test Pattern";
    int width = 1920, height = 1080;
    int fps = 30;
    bool audio = true;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "--name") && i + 1 < argc) {
            name = argv[++i];
        } else if ((arg == "--resolution") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &width, &height) != 2) {
                fprintf(stderr, "Invalid resolution format, use WxH\n");
                return 1;
            }
        } else if ((arg == "--fps") && i + 1 < argc) {
            fps = atoi(argv[++i]);
        } else if (arg == "--no-audio") {
            audio = false;
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
    }

    printf("NDI Test Pattern %s\n", NDI_BRIDGE_VERSION);
    printf("Source: '%s'  %dx%d @ %d fps  audio=%s\n\n",
           name.c_str(), width, height, fps, audio ? "440Hz" : "off");

    if (!NDIlib_initialize()) {
        fprintf(stderr, "Failed to initialize NDI\n");
        return 1;
    }
    printf("NDI SDK: %s\n", NDIlib_version());

    NDIlib_send_create_t sendCreate;
    sendCreate.p_ndi_name = name.c_str();
    sendCreate.p_groups = nullptr;
    sendCreate.clock_video = true;
    sendCreate.clock_audio = false;

    NDIlib_send_instance_t sender = NDIlib_send_create(&sendCreate);
    if (!sender) {
        fprintf(stderr, "Failed to create NDI sender\n");
        NDIlib_destroy();
        return 1;
    }

    printf("Broadcasting as '%s' on local network\n", name.c_str());
    printf("Press Ctrl+C to stop...\n\n");

    int stride = width * 4;
    std::vector<uint8_t> videoData(stride * height);

    const int sampleRate = 48000;
    const int channels = 2;
    int samplesPerFrame = sampleRate / fps;
    std::vector<float> audioData(samplesPerFrame * channels);
    uint64_t audioPhase = 0;

    NDIlib_video_frame_v2_t videoFrame = {};
    videoFrame.xres = width;
    videoFrame.yres = height;
    videoFrame.FourCC = NDIlib_FourCC_video_type_BGRA;
    videoFrame.frame_rate_N = fps * 1000;
    videoFrame.frame_rate_D = 1000;
    videoFrame.picture_aspect_ratio = static_cast<float>(width) / static_cast<float>(height);
    videoFrame.frame_format_type = NDIlib_frame_format_type_progressive;
    videoFrame.timecode = NDIlib_send_timecode_synthesize;
    videoFrame.p_data = videoData.data();
    videoFrame.line_stride_in_bytes = stride;

    NDIlib_audio_frame_v2_t audioFrame = {};
    audioFrame.sample_rate = sampleRate;
    audioFrame.no_channels = channels;
    audioFrame.no_samples = samplesPerFrame;
    audioFrame.timecode = NDIlib_send_timecode_synthesize;
    audioFrame.channel_stride_in_bytes = samplesPerFrame * sizeof(float);
    audioFrame.p_data = audioData.data();

    Ball ball;
    ball.init(width, height);

    int frameCount = 0;
    auto startTime = std::chrono::steady_clock::now();

    while (g_running) {
        generateFrame(videoData.data(), width, height, stride, ball, frameCount);
        NDIlib_send_send_video_v2(sender, &videoFrame);

        if (audio) {
            generateAudio(audioData.data(), sampleRate, samplesPerFrame,
                          channels, audioPhase);
            NDIlib_send_send_audio_v2(sender, &audioFrame);
        }

        frameCount++;

        if (frameCount % (fps * 5) == 0) {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - startTime).count();
            int conns = NDIlib_send_get_no_connections(sender, 0);
            printf("[%.0fs] %d frames sent, %d receiver(s) connected\n",
                   elapsed, frameCount, conns);
        }
    }

    printf("\nStopping... %d frames sent total\n", frameCount);

    NDIlib_send_destroy(sender);
    NDIlib_destroy();

    return 0;
}
