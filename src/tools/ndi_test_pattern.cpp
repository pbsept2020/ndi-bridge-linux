/**
 * ndi_test_pattern — NDI test pattern with LTC timecode
 *
 * Generates a bouncing green ball on a dark background + broadcast-grade
 * LTC timecode audio (SMPTE 12M via libltc) + professional 7-segment
 * timecode display overlay.
 *
 * Ball position, visual timecode, and audio LTC are all UTC-deterministic:
 * same time = same output on any machine.
 * Timecode is read from the UTC clock every frame (no drift).
 * Ball position is a pure function of UTC time (triangle wave bounce).
 *
 * Usage:
 *   ndi-test-pattern [--name "Source"] [--resolution WxH] [--fps N]
 *                    [--tc-fps 29.97] [--tc-start HH:MM:SS:FF]
 *                    [--no-audio] [--audio-440] [--no-tc-display]
 */

#include <cstddef>
#include <Processing.NDI.Lib.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <csignal>
#include <chrono>
#include <vector>
#include <string>
#include <ctime>

#include "common/Version.h"
#include "tools/tc_font.h"
#include "tools/tc_webcontrol.h"

#ifdef HAVE_LIBLTC
#include <ltc.h>
#endif

static volatile bool g_running = true;
static void signalHandler(int) { g_running = false; }

// ── Timecode framerate definitions ──────────────────────────────────

struct TCFramerate {
    const char* name;       // CLI name
    const char* label;      // display label (e.g. "29.97 DF")
    int num;                // framerate numerator
    int den;                // framerate denominator
    double fps;             // actual fps value
    bool dropFrame;         // NTSC drop-frame flag
    int ltcTvStd;           // LTC_TV_* constant for libltc
};

static const TCFramerate TC_FRAMERATES[] = {
    {"23.976", "23.976 fps",  24000, 1001, 23.976,  false, 0 /* LTC_TV_FILM_24 */},
    {"24",     "24 fps",      24,    1,    24.0,    false, 0},
    {"25",     "25 fps",      25,    1,    25.0,    false, 1 /* LTC_TV_625_50 */},
    {"29.97",  "29.97 DF",    30000, 1001, 29.97,   true,  2 /* LTC_TV_525_60 */},
    {"29.97ndf","29.97 NDF",  30000, 1001, 29.97,   false, 2},
    {"30",     "30 fps",      30,    1,    30.0,    false, 2},
    {"50",     "50 fps",      50,    1,    50.0,    false, 1},
    {"59.94",  "59.94 DF",    60000, 1001, 59.94,   true,  2},
    {"60",     "60 fps",      60,    1,    60.0,    false, 2},
};
static constexpr int NUM_TC_FRAMERATES = sizeof(TC_FRAMERATES) / sizeof(TC_FRAMERATES[0]);

static const TCFramerate* findTCFramerate(const char* name) {
    for (int i = 0; i < NUM_TC_FRAMERATES; i++) {
        if (strcmp(TC_FRAMERATES[i].name, name) == 0)
            return &TC_FRAMERATES[i];
    }
    return nullptr;
}

// Get index of a TCFramerate pointer in the array
static int tcFramerateIndex(const TCFramerate* rate) {
    if (!rate) return 2;  // default 25fps
    return static_cast<int>(rate - TC_FRAMERATES);
}

// Find TC framerate matching a video fps (integer only)
static const TCFramerate* findTCFramerateForFps(int videoFps) {
    for (int i = 0; i < NUM_TC_FRAMERATES; i++) {
        if (TC_FRAMERATES[i].den == 1 &&
            TC_FRAMERATES[i].num == videoFps &&
            !TC_FRAMERATES[i].dropFrame)
            return &TC_FRAMERATES[i];
    }
    return nullptr;  // not a standard TC framerate
}

// ── Timecode counter ────────────────────────────────────────────────

struct TimecodeState {
    int hh, mm, ss, ff;
    bool dropFrame;
    int maxFrames;  // frames per second (rounded: 24, 25, 30, 50, 60)

    void initFromUTC(double tcFps, bool df) {
        dropFrame = df;
        maxFrames = static_cast<int>(std::ceil(tcFps - 0.01));

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm;
        gmtime_r(&ts.tv_sec, &tm);

        hh = tm.tm_hour;
        mm = tm.tm_min;
        ss = tm.tm_sec;
        ff = static_cast<int>(ts.tv_nsec * static_cast<int64_t>(maxFrames) / 1000000000LL);
        if (ff >= maxFrames) ff = maxFrames - 1;
    }

    void initFromString(const char* str, double tcFps, bool df) {
        dropFrame = df;
        maxFrames = static_cast<int>(std::ceil(tcFps - 0.01));

        int h = 0, m = 0, s = 0, f = 0;
        // Try HH:MM:SS:FF or HH:MM:SS;FF
        if (sscanf(str, "%d:%d:%d:%d", &h, &m, &s, &f) == 4 ||
            sscanf(str, "%d:%d:%d;%d", &h, &m, &s, &f) == 4) {
            hh = h; mm = m; ss = s; ff = f;
        } else {
            fprintf(stderr, "WARNING: invalid tc-start format '%s', using 00:00:00:00\n", str);
            hh = mm = ss = ff = 0;
        }
    }

    void increment() {
        ff++;
        if (ff >= maxFrames) {
            ff = 0;
            ss++;
            if (ss >= 60) {
                ss = 0;
                mm++;
                if (mm >= 60) {
                    mm = 0;
                    hh++;
                    if (hh >= 24) {
                        hh = 0;
                    }
                }
            }

            // Drop-frame: skip frames 0 and 1 at the start of each minute
            // EXCEPT minutes divisible by 10
            if (dropFrame && ff == 0 && ss == 0 && (mm % 10) != 0) {
                ff = 2;  // skip 0 and 1
            }
        }
    }

    // Re-read UTC clock every frame (no drift)
    void syncFromUTC() {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm;
        gmtime_r(&ts.tv_sec, &tm);

        hh = tm.tm_hour;
        mm = tm.tm_min;
        ss = tm.tm_sec;
        ff = static_cast<int>(ts.tv_nsec * static_cast<int64_t>(maxFrames) / 1000000000LL);
        if (ff >= maxFrames) ff = maxFrames - 1;
    }
};

// ── Ball color ─────────────────────────────────────────────────────

struct BallColor {
    uint8_t r, g, b;
    const char* name;
};

static const BallColor BALL_COLORS[] = {
    {100, 255,  50, "green"},
    {255,  60,  60, "red"},
    { 60, 120, 255, "blue"},
    {255, 240,  40, "yellow"},
    {  0, 255, 255, "cyan"},
    {255,  50, 255, "magenta"},
    {255, 255, 255, "white"},
};
static constexpr int NUM_BALL_COLORS = sizeof(BALL_COLORS) / sizeof(BALL_COLORS[0]);

static const BallColor* findBallColor(const char* name) {
    for (int i = 0; i < NUM_BALL_COLORS; i++) {
        if (strcmp(BALL_COLORS[i].name, name) == 0)
            return &BALL_COLORS[i];
    }
    return nullptr;
}

// ── UTC-deterministic ball ──────────────────────────────────────────

// Triangle wave: bounce between 0 and amplitude, period = 2×amplitude.
// Pure function — same input = same output on any machine.
static double triangleWave(double distance, double amplitude) {
    if (amplitude <= 0.0) return 0.0;
    double period = 2.0 * amplitude;
    double phase = fmod(distance, period);
    if (phase < 0.0) phase += period;
    return (phase <= amplitude) ? phase : period - phase;
}

// Frames elapsed since midnight UTC — raw calculation, not affected by drop-frame.
static uint32_t utcTotalFrames(int fps) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    int secSinceMidnight = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    int ff = static_cast<int>(ts.tv_nsec * static_cast<int64_t>(fps) / 1000000000LL);
    if (ff >= fps) ff = fps - 1;
    return static_cast<uint32_t>(secSinceMidnight * fps + ff);
}

struct Ball {
    float x, y;
    float vx, vy;
    int radius;

    void init(int width, int height) {
        radius = height / 20;
        vx = 6.0f;
        vy = 3.5f;
        // Initial position will be set by computeFromUTC()
        x = static_cast<float>(radius);
        y = static_cast<float>(radius);
    }

    void computeFromUTC(int width, int height, uint32_t totalFrames) {
        double bounceW = static_cast<double>(width - 2 * radius);
        double bounceH = static_cast<double>(height - 2 * radius);
        x = static_cast<float>(radius + triangleWave(
            static_cast<double>(totalFrames) * static_cast<double>(vx), bounceW));
        y = static_cast<float>(radius + triangleWave(
            static_cast<double>(totalFrames) * static_cast<double>(vy), bounceH));
    }
};

// ── Video frame generation ──────────────────────────────────────────

static void generateFrame(uint8_t* data, int width, int height, int stride,
                           const Ball& ball, uint32_t totalFrames,
                           const BallColor& color, const TimecodeState& tc,
                           bool showTC, bool dropFrame, const char* fpsLabel) {
    // Dark gray background
    for (int y = 0; y < height; y++) {
        uint8_t* row = data + y * stride;
        for (int x = 0; x < width; x++) {
            row[x * 4 + 0] = 30;   // B
            row[x * 4 + 1] = 30;   // G
            row[x * 4 + 2] = 30;   // R
            row[x * 4 + 3] = 255;  // A
        }
    }

    // Draw ball
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
                float edge = static_cast<float>(r2 - dist2) / static_cast<float>(ball.radius * 4);
                if (edge > 1.0f) edge = 1.0f;
                row[x * 4 + 0] = static_cast<uint8_t>(color.b * edge);  // B
                row[x * 4 + 1] = static_cast<uint8_t>(color.g * edge);  // G
                row[x * 4 + 2] = static_cast<uint8_t>(color.r * edge);  // R
                row[x * 4 + 3] = static_cast<uint8_t>(255 * edge);  // A
            }
        }
    }

    // Frame counter flash (top-left white square toggles every UTC frame)
    if (totalFrames % 2 == 0) {
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

    // Timecode display
    if (showTC) {
        tc::drawTimecodeDisplay(data, stride, width, height,
                                tc.hh, tc.mm, tc.ss, tc.ff,
                                dropFrame, static_cast<int>(totalFrames), fpsLabel);
    }

}

// ── Audio generation ────────────────────────────────────────────────

static void generateSine440(float* data, int sampleRate, int numSamples,
                             int channel, int totalChannels, uint64_t& phase) {
    const double freq = 440.0;
    const double amplitude = 0.3;

    for (int s = 0; s < numSamples; s++) {
        float sample = static_cast<float>(
            amplitude * sin(2.0 * M_PI * freq * static_cast<double>(phase) / sampleRate)
        );
        data[channel * numSamples + s] = sample;
        phase++;
    }
}

// ── Audio sample count for non-integer framerates ───────────────────
// For 29.97 fps: 48000/29.97 = 1601.6, so we alternate 1601/1602
// This struct tracks the cumulative error to avoid drift.

struct AudioSampleCounter {
    double samplesPerFrame;   // exact (e.g. 1601.6016...)
    double accumulator;       // cumulative fractional part

    void init(int sampleRate, double tcFps) {
        samplesPerFrame = static_cast<double>(sampleRate) / tcFps;
        accumulator = 0.0;
    }

    int nextFrameSamples() {
        accumulator += samplesPerFrame;
        int n = static_cast<int>(accumulator);
        accumulator -= n;
        return n;
    }
};

// ── Usage ───────────────────────────────────────────────────────────

static void printUsage(const char* prog) {
    printf("\n"
           "NDI Test Pattern Generator %s — with LTC Timecode\n"
           "\n"
           "Usage: %s [options]\n"
           "\n"
           "Options:\n"
           "  --name <name>              NDI source name (default: Test Pattern LTC)\n"
           "  --resolution <WxH>         Resolution (default: 1920x1080)\n"
           "  --fps <N>                  Video frame rate for NDI (default: 25)\n"
           "  --tc-fps <rate>            Timecode frame rate (default: same as --fps if standard, else 25)\n"
           "  --tc-start <HH:MM:SS:FF>  Start timecode (default: UTC time of day)\n"
           "  --no-audio                 Disable all audio\n"
           "  --audio-440                Add 440Hz sine on channel 2 (debug)\n"
           "  --ball-color <color>       Ball color: green red blue yellow cyan magenta white\n"
           "  --no-tc-display            Disable visual timecode bar (keep audio LTC only)\n"
           "  -h, --help                 Show this help\n"
           "\n"
           "Timecode framerates (--tc-fps):\n"
           "  23.976     23.976 fps (cinema NTSC / Film)\n"
           "  24         24 fps (cinema)\n"
           "  25         25 fps (PAL/SECAM) — default\n"
           "  29.97      29.97 fps (NTSC drop-frame)\n"
           "  29.97ndf   29.97 fps (NTSC non-drop-frame)\n"
           "  30         30 fps (progressive)\n"
           "  50         50 fps (PAL high frame rate)\n"
           "  59.94      59.94 fps (NTSC high frame rate)\n"
           "  60         60 fps (progressive high frame rate)\n"
           "\n"
           "Bouncing green ball — UTC-deterministic (same time = same position).\n"
           "Ball jump/teleport = dropped frames. Smooth = healthy.\n"
           "Flickering white square (top-left) = frames arriving.\n"
           "LTC timecode (ch1=silence or 440Hz, ch2=LTC per SMPTE convention).\n"
           "\n",
           NDI_BRIDGE_VERSION, prog);
}

// ── Main ────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::string name = "Test Pattern LTC";
    int width = 1920, height = 1080;
    int videoFps = 25;
    const char* tcFpsStr = nullptr;
    const char* tcStartStr = nullptr;
    bool audioEnabled = true;
    bool audio440 = false;
    bool showTCDisplay = true;
    const BallColor* ballColor = &BALL_COLORS[0];  // green

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
            videoFps = atoi(argv[++i]);
        } else if ((arg == "--tc-fps") && i + 1 < argc) {
            tcFpsStr = argv[++i];
        } else if ((arg == "--tc-start") && i + 1 < argc) {
            tcStartStr = argv[++i];
        } else if (arg == "--no-audio") {
            audioEnabled = false;
        } else if (arg == "--audio-440") {
            audio440 = true;
        } else if ((arg == "--ball-color") && i + 1 < argc) {
            ballColor = findBallColor(argv[++i]);
            if (!ballColor) {
                fprintf(stderr, "Unknown ball color: %s\n", argv[i]);
                fprintf(stderr, "Valid: green red blue yellow cyan magenta white\n");
                return 1;
            }
        } else if (arg == "--no-tc-display") {
            showTCDisplay = false;
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            printUsage(argv[0]);
            return 1;
        }
    }

    // Resolve TC framerate
    const TCFramerate* tcRate = nullptr;
    if (tcFpsStr) {
        tcRate = findTCFramerate(tcFpsStr);
        if (!tcRate) {
            fprintf(stderr, "Unknown timecode framerate: %s\n", tcFpsStr);
            fprintf(stderr, "Valid values: 23.976, 24, 25, 29.97, 29.97ndf, 30, 50, 59.94, 60\n");
            return 1;
        }
    } else {
        // Default: use video fps if it's a standard TC rate, else 25
        tcRate = findTCFramerateForFps(videoFps);
        if (!tcRate) {
            tcRate = findTCFramerate("25");
        }
    }

    printf("NDI Test Pattern %s\n", NDI_BRIDGE_VERSION);
    printf("Source: '%s'  %dx%d @ %d fps  TC: %s  audio=%s\n\n",
           name.c_str(), width, height, videoFps,
           tcRate->label,
           !audioEnabled ? "off" : (audio440 ? "LTC+440Hz" : "LTC"));

    if (!NDIlib_initialize()) {
        fprintf(stderr, "Failed to initialize NDI\n");
        return 1;
    }
    printf("NDI SDK: %s\n", NDIlib_version());

    // ── NDI sender ──
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

    // ── Web Control server ──
    TCWebSettings webSettings;
    TCWebControl webControl(&webSettings);

    int currentFpsIndex = tcFramerateIndex(tcRate);
    webSettings.tcFpsIndex.store(currentFpsIndex);
    webSettings.ltcGainDb.store(-3.0f);
    webSettings.audio440.store(audio440);

    if (webControl.start()) {
        printf("Web control: %s\n", webControl.url().c_str());

        // Register web control URL with NDI
        std::string ndiMeta = webControl.ndiMetadataXml();
        NDIlib_metadata_frame_t metaFrame;
        metaFrame.p_data = const_cast<char*>(ndiMeta.c_str());
        metaFrame.length = static_cast<int>(ndiMeta.size());
        metaFrame.timecode = NDIlib_send_timecode_synthesize;
        NDIlib_send_add_connection_metadata(sender, &metaFrame);
    } else {
        printf("WARNING: web control server failed to start\n");
    }

    // ── Timecode state ──
    TimecodeState tcState;
    if (tcStartStr) {
        tcState.initFromString(tcStartStr, tcRate->fps, tcRate->dropFrame);
    } else {
        tcState.initFromUTC(tcRate->fps, tcRate->dropFrame);
    }
    printf("TC start: %02d:%02d:%02d%c%02d (%s)\n",
           tcState.hh, tcState.mm, tcState.ss,
           tcRate->dropFrame ? ';' : ':', tcState.ff,
           tcRate->label);
    webSettings.dropFrame.store(tcRate->dropFrame);

    // ── LTC encoder setup ──
    float currentGainDb = -3.0f;

#ifdef HAVE_LIBLTC
    LTCEncoder* ltcEnc = ltc_encoder_create(48000, tcRate->fps,
        static_cast<enum LTC_TV_STANDARD>(tcRate->ltcTvStd), LTC_USE_DATE);
    if (!ltcEnc) {
        fprintf(stderr, "Failed to create LTC encoder\n");
        webControl.stop();
        NDIlib_send_destroy(sender);
        NDIlib_destroy();
        return 1;
    }
    ltc_encoder_set_volume(ltcEnc, currentGainDb);

    // Set initial timecode
    SMPTETimecode smpteTc;
    memset(&smpteTc, 0, sizeof(smpteTc));
    smpteTc.hours = static_cast<unsigned char>(tcState.hh);
    smpteTc.mins  = static_cast<unsigned char>(tcState.mm);
    smpteTc.secs  = static_cast<unsigned char>(tcState.ss);
    smpteTc.frame = static_cast<unsigned char>(tcState.ff);

    // Set date from current UTC
    {
        time_t now = time(nullptr);
        struct tm tm;
        gmtime_r(&now, &tm);
        smpteTc.years  = static_cast<unsigned char>(tm.tm_year % 100);
        smpteTc.months = static_cast<unsigned char>(tm.tm_mon + 1);
        smpteTc.days   = static_cast<unsigned char>(tm.tm_mday);
    }

    ltc_encoder_set_timecode(ltcEnc, &smpteTc);

    // Set drop-frame flag directly in the LTC frame
    if (tcRate->dropFrame) {
        LTCFrame ltcFrame;
        ltc_encoder_get_frame(ltcEnc, &ltcFrame);
        ltcFrame.dfbit = 1;
        ltc_encoder_set_frame(ltcEnc, &ltcFrame);
    }
    printf("LTC encoder initialized at 48kHz, %.0f dBFS\n", currentGainDb);
#else
    printf("WARNING: libltc not available — audio will be silence (or 440Hz with --audio-440)\n");
    printf("Install with: brew install libltc (Mac) or sudo apt install libltc-dev (Linux)\n");
#endif

    printf("Press Ctrl+C to stop...\n\n");

    // ── Buffers ──
    int stride = width * 4;
    std::vector<uint8_t> videoData(stride * height);

    const int sampleRate = 48000;
    const int channels = 2;  // ch1=silence (or 440Hz), ch2=LTC (SMPTE convention: last channel)
    AudioSampleCounter sampleCounter;
    sampleCounter.init(sampleRate, tcRate->fps);

    uint64_t sinePhase = 0;
    bool ltcMuted = false;

    NDIlib_video_frame_v2_t videoFrame = {};
    videoFrame.xres = width;
    videoFrame.yres = height;
    videoFrame.FourCC = NDIlib_FourCC_video_type_BGRA;
    videoFrame.frame_rate_N = videoFps * 1000;
    videoFrame.frame_rate_D = 1000;
    videoFrame.picture_aspect_ratio = static_cast<float>(width) / static_cast<float>(height);
    videoFrame.frame_format_type = NDIlib_frame_format_type_progressive;
    videoFrame.timecode = NDIlib_send_timecode_synthesize;
    videoFrame.p_data = videoData.data();
    videoFrame.line_stride_in_bytes = stride;

    Ball ball;
    ball.init(width, height);

    int frameCount = 0;
    auto startTime = std::chrono::steady_clock::now();

    while (g_running) {
        // ── Check web control settings changes ──
        if (webSettings.changed.exchange(false)) {
            int newFpsIndex = webSettings.tcFpsIndex.load();
            if (newFpsIndex != currentFpsIndex && newFpsIndex >= 0 && newFpsIndex < NUM_TC_FRAMERATES) {
                tcRate = &TC_FRAMERATES[newFpsIndex];
                tcState.maxFrames = static_cast<int>(std::ceil(tcRate->fps - 0.01));
                tcState.dropFrame = tcRate->dropFrame;
                sampleCounter.init(sampleRate, tcRate->fps);
                webSettings.dropFrame.store(tcRate->dropFrame);

#ifdef HAVE_LIBLTC
                // Recreate LTC encoder with new framerate
                ltc_encoder_free(ltcEnc);
                ltcEnc = ltc_encoder_create(48000, tcRate->fps,
                    static_cast<enum LTC_TV_STANDARD>(tcRate->ltcTvStd), LTC_USE_DATE);
                if (ltcEnc) {
                    ltc_encoder_set_volume(ltcEnc, webSettings.ltcGainDb.load());

                    SMPTETimecode tc;
                    memset(&tc, 0, sizeof(tc));
                    tc.hours = static_cast<unsigned char>(tcState.hh);
                    tc.mins  = static_cast<unsigned char>(tcState.mm);
                    tc.secs  = static_cast<unsigned char>(tcState.ss);
                    tc.frame = static_cast<unsigned char>(tcState.ff);
                    ltc_encoder_set_timecode(ltcEnc, &tc);

                    if (tcRate->dropFrame) {
                        LTCFrame lf;
                        ltc_encoder_get_frame(ltcEnc, &lf);
                        lf.dfbit = 1;
                        ltc_encoder_set_frame(ltcEnc, &lf);
                    }
                }
#endif
                currentFpsIndex = newFpsIndex;
                printf("[WEB] TC framerate changed to %s\n", tcRate->label);
            }

            float newGain = webSettings.ltcGainDb.load();
            if (newGain != currentGainDb) {
                currentGainDb = newGain;
#ifdef HAVE_LIBLTC
                if (ltcEnc) ltc_encoder_set_volume(ltcEnc, currentGainDb);
#endif
            }

            ltcMuted = webSettings.ltcMuted.load();
            audio440 = webSettings.audio440.load();
        }

        // ── Update web settings with current state ──
        webSettings.hh.store(tcState.hh);
        webSettings.mm.store(tcState.mm);
        webSettings.ss.store(tcState.ss);
        webSettings.ff.store(tcState.ff);
        webSettings.frameCount.store(frameCount);

        // 1. Compute UTC-deterministic ball position + generate video frame
        uint32_t totalFrames = utcTotalFrames(videoFps);
        ball.computeFromUTC(width, height, totalFrames);
        generateFrame(videoData.data(), width, height, stride,
                      ball, totalFrames, *ballColor, tcState,
                      showTCDisplay, tcRate->dropFrame, tcRate->label);

        // 2. Send video
        NDIlib_send_send_video_v2(sender, &videoFrame);

        // 3. Generate and send audio
        if (audioEnabled) {
            int samplesThisFrame = sampleCounter.nextFrameSamples();
            std::vector<float> audioData(samplesThisFrame * channels, 0.0f);

#ifdef HAVE_LIBLTC
            if (!ltcMuted && ltcEnc) {
                // Encode one LTC frame
                ltc_encoder_encode_frame(ltcEnc);
                ltcsnd_sample_t* ltcBuf = nullptr;
                int ltcLen = ltc_encoder_get_bufferptr(ltcEnc, &ltcBuf, 1);

                int copyLen = std::min(ltcLen, samplesThisFrame);
                for (int i = 0; i < copyLen; i++) {
                    audioData[(channels - 1) * samplesThisFrame + i] =
                        (static_cast<float>(ltcBuf[i]) - 128.0f) / 128.0f;
                }
            }
#endif

            // Channel 1: 440Hz sine if requested, else silence
            if (audio440) {
                generateSine440(audioData.data(), sampleRate, samplesThisFrame,
                                0, channels, sinePhase);
            }

            NDIlib_audio_frame_v2_t audioFrame = {};
            audioFrame.sample_rate = sampleRate;
            audioFrame.no_channels = channels;
            audioFrame.no_samples = samplesThisFrame;
            audioFrame.timecode = NDIlib_send_timecode_synthesize;
            audioFrame.channel_stride_in_bytes = samplesThisFrame * static_cast<int>(sizeof(float));
            audioFrame.p_data = audioData.data();

            NDIlib_send_send_audio_v2(sender, &audioFrame);
        }

        // 4. Sync timecode from UTC clock (no drift — both Mac and EC2 agree)
        tcState.syncFromUTC();
#ifdef HAVE_LIBLTC
        if (ltcEnc) {
            SMPTETimecode stc;
            memset(&stc, 0, sizeof(stc));
            stc.hours = tcState.hh;
            stc.mins  = tcState.mm;
            stc.secs  = tcState.ss;
            stc.frame = tcState.ff;
            ltc_encoder_set_timecode(ltcEnc, &stc);
        }
#endif

        frameCount++;

        // Status log every 5 seconds
        if (frameCount % (videoFps * 5) == 0) {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - startTime).count();
            int conns = NDIlib_send_get_no_connections(sender, 0);
            webSettings.receivers.store(conns);
            printf("[%.0fs] %d frames sent, TC %02d:%02d:%02d%c%02d, %d receiver(s)\n",
                   elapsed, frameCount,
                   tcState.hh, tcState.mm, tcState.ss,
                   tcRate->dropFrame ? ';' : ':', tcState.ff,
                   conns);
        }
    }

    printf("\nStopping... %d frames sent total\n", frameCount);

    webControl.stop();

#ifdef HAVE_LIBLTC
    if (ltcEnc) ltc_encoder_free(ltcEnc);
#endif

    NDIlib_send_destroy(sender);
    NDIlib_destroy();

    return 0;
}
