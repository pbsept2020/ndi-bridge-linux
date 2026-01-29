/**
 * Minimal NDI receive test - exact same pattern as ndi-viewer-mac Swift code
 * Tests if the C++ binary can receive NDI video at all.
 */
#include <cstddef>
#include <Processing.NDI.Lib.h>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <unistd.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

static volatile bool running = true;
void sig(int) { running = false; }

int main(int argc, char* argv[]) {
    signal(SIGINT, sig);
    signal(SIGTERM, sig);

    const char* target = argc > 1 ? argv[1] : "P20064";

    printf("NDI Receive Test (mimics Swift viewer exactly)\n");
    printf("Target: %s\n\n", target);

    if (!NDIlib_initialize()) {
        fprintf(stderr, "NDI init failed\n");
        return 1;
    }
    printf("NDI SDK: %s\n", NDIlib_version());

    // === DISCOVER (same as Swift: create finder, poll, get sources) ===
    NDIlib_find_create_t findCreate = {};
    findCreate.show_local_sources = true;
    findCreate.p_groups = nullptr;
    findCreate.p_extra_ips = nullptr;

    NDIlib_find_instance_t finder = NDIlib_find_create_v2(&findCreate);
    if (!finder) {
        fprintf(stderr, "Failed to create finder\n");
        return 1;
    }

    printf("Discovering sources (5s)...\n");
    for (int i = 0; i < 50 && running; i++) {
        NDIlib_find_wait_for_sources(finder, 100);
    }

    uint32_t numSources = 0;
    const NDIlib_source_t* sources = NDIlib_find_get_current_sources(finder, &numSources);
    printf("Found %u sources:\n", numSources);

    int matchIdx = -1;
    for (uint32_t i = 0; i < numSources; i++) {
        printf("  [%u] %s -> %s\n", i,
               sources[i].p_ndi_name ? sources[i].p_ndi_name : "?",
               sources[i].p_url_address ? sources[i].p_url_address : "?");
        if (strstr(sources[i].p_ndi_name, target)) {
            matchIdx = (int)i;
        }
    }

    if (matchIdx < 0) {
        fprintf(stderr, "Source '%s' not found\n", target);
        NDIlib_find_destroy(finder);
        NDIlib_destroy();
        return 1;
    }

    printf("\n=== Connecting to: %s ===\n", sources[matchIdx].p_ndi_name);

    // === CONNECT (same as Swift NDIManager.connect) ===
    // Swift does: create receiver WITHOUT source, then recv_connect separately
    NDIlib_recv_create_v3_t recvSettings = {};
    recvSettings.color_format = NDIlib_recv_color_format_BGRX_BGRA;
    recvSettings.bandwidth = NDIlib_recv_bandwidth_highest;
    recvSettings.allow_video_fields = true;
    recvSettings.p_ndi_recv_name = nullptr;
    // Note: source_to_connect_to is left zeroed (like Swift does)

    NDIlib_recv_instance_t recv = NDIlib_recv_create_v3(&recvSettings);
    if (!recv) {
        fprintf(stderr, "Failed to create receiver\n");
        return 1;
    }

    // Connect with the original source pointer from finder
    NDIlib_recv_connect(recv, &sources[matchIdx]);
    printf("recv_connect done\n");

    // Check PTZ
    printf("PTZ supported: %s\n", NDIlib_recv_ptz_is_supported(recv) ? "yes" : "no");
    printf("Waiting for frames...\n\n");

    // === RECEIVE LOOP ===
    int videoCount = 0, audioCount = 0, noneCount = 0;
    int loopCount = 0;

    while (running) {
        NDIlib_video_frame_v2_t video;
        NDIlib_audio_frame_v2_t audio;
        NDIlib_metadata_frame_t meta;

        auto type = NDIlib_recv_capture_v2(recv, &video, &audio, &meta, 100);
        loopCount++;

        // Log connection count periodically
        if (loopCount <= 20 || loopCount % 50 == 0) {
            int nConn = NDIlib_recv_get_no_connections(recv);
            printf("[loop %d] type=%d connections=%d (v=%d a=%d none=%d)\n",
                   loopCount, (int)type, nConn, videoCount, audioCount, noneCount);
        }

        switch (type) {
            case NDIlib_frame_type_video:
                videoCount++;
                printf("*** VIDEO #%d: %dx%d stride=%d fourcc=0x%08X ***\n",
                       videoCount, video.xres, video.yres,
                       video.line_stride_in_bytes, video.FourCC);
                NDIlib_recv_free_video_v2(recv, &video);
                break;

            case NDIlib_frame_type_audio:
                audioCount++;
                if (audioCount <= 3) {
                    printf("AUDIO #%d: %dHz %dch %d samples\n",
                           audioCount, audio.sample_rate, audio.no_channels, audio.no_samples);
                }
                NDIlib_recv_free_audio_v2(recv, &audio);
                break;

            case NDIlib_frame_type_metadata:
                printf("META: %s\n", meta.p_data ? meta.p_data : "(null)");
                NDIlib_recv_free_metadata(recv, &meta);
                break;

            case NDIlib_frame_type_status_change:
                printf("STATUS CHANGE (connections=%d)\n",
                       NDIlib_recv_get_no_connections(recv));
                break;

            case NDIlib_frame_type_none:
                noneCount++;
                break;

            case NDIlib_frame_type_error:
                printf("ERROR\n");
                break;

            default:
                printf("UNKNOWN type=%d\n", (int)type);
                break;
        }

#ifdef __APPLE__
        // Pump CFRunLoop in case NDI needs it
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, false);
#endif
    }

    printf("\n=== DONE ===\n");
    printf("Totals: video=%d audio=%d none=%d loops=%d\n",
           videoCount, audioCount, noneCount, loopCount);

    NDIlib_recv_destroy(recv);
    NDIlib_find_destroy(finder);
    NDIlib_destroy();
    return 0;
}
