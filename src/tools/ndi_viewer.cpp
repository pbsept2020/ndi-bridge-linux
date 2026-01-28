/**
 * NDI Viewer - Simple NDI source viewer using SDL2
 *
 * Usage: ndi-viewer [source_name]
 *   If no source specified, shows list and prompts for selection
 */

#include <iostream>
#include <string>
#include <vector>
#include <atomic>
#include <csignal>
#include <cstring>

#include <SDL2/SDL.h>
#include <Processing.NDI.Lib.h>

std::atomic<bool> g_running{true};

// PTZ state
float g_ptzSpeed = 0.5f;
bool g_ptzSupported = false;
bool g_fullscreen = false;

// Audio state
SDL_AudioDeviceID g_audioDevice = 0;
int g_audioSampleRate = 0;
int g_audioChannels = 0;

void signalHandler(int) {
    g_running = false;
}

// Initialize or reinitialize audio device when format changes
bool initAudio(int sampleRate, int channels) {
    if (g_audioDevice && g_audioSampleRate == sampleRate && g_audioChannels == channels) {
        return true; // Already initialized with same format
    }

    // Close existing device
    if (g_audioDevice) {
        SDL_CloseAudioDevice(g_audioDevice);
        g_audioDevice = 0;
    }

    SDL_AudioSpec wanted = {};
    wanted.freq = sampleRate;
    wanted.format = AUDIO_F32SYS;  // Float 32-bit native endian
    wanted.channels = channels;
    wanted.samples = 1024;
    wanted.callback = nullptr;  // Use SDL_QueueAudio

    g_audioDevice = SDL_OpenAudioDevice(nullptr, 0, &wanted, nullptr, 0);
    if (!g_audioDevice) {
        std::cerr << "Failed to open audio: " << SDL_GetError() << "\n";
        return false;
    }

    g_audioSampleRate = sampleRate;
    g_audioChannels = channels;

    // Start playback
    SDL_PauseAudioDevice(g_audioDevice, 0);

    std::cout << "Audio: " << sampleRate << " Hz, " << channels << " ch\n";
    return true;
}

// Convert NDI planar audio to interleaved for SDL
void playAudio(const NDIlib_audio_frame_v2_t& frame) {
    if (!initAudio(frame.sample_rate, frame.no_channels)) {
        return;
    }

    int numSamples = frame.no_samples;
    int numChannels = frame.no_channels;

    // Allocate interleaved buffer
    std::vector<float> interleaved(numSamples * numChannels);

    // Convert planar to interleaved
    for (int s = 0; s < numSamples; s++) {
        for (int c = 0; c < numChannels; c++) {
            // NDI planar: each channel is contiguous, stride between channels
            float* channelData = frame.p_data + c * frame.channel_stride_in_bytes / sizeof(float);
            interleaved[s * numChannels + c] = channelData[s];
        }
    }

    // Queue audio for playback
    SDL_QueueAudio(g_audioDevice, interleaved.data(), interleaved.size() * sizeof(float));
}

// PTZ functions
void checkPTZSupport(NDIlib_recv_instance_t recv) {
    g_ptzSupported = NDIlib_recv_ptz_is_supported(recv);
    std::cout << "PTZ supported: " << (g_ptzSupported ? "yes" : "no") << "\n";
}

void sendPTZ(NDIlib_recv_instance_t recv, float pan, float tilt) {
    if (!g_ptzSupported) return;
    NDIlib_recv_ptz_pan_tilt_speed(recv, pan, tilt);
}

void sendZoom(NDIlib_recv_instance_t recv, float speed) {
    if (!g_ptzSupported) return;
    NDIlib_recv_ptz_zoom_speed(recv, speed);
}

void recallPreset(NDIlib_recv_instance_t recv, int preset) {
    if (!g_ptzSupported) return;
    std::cout << "Recalling preset " << preset << "\n";
    NDIlib_recv_ptz_recall_preset(recv, preset, 1.0f);
}

void stopPTZ(NDIlib_recv_instance_t recv) {
    if (!g_ptzSupported) return;
    NDIlib_recv_ptz_pan_tilt_speed(recv, 0, 0);
    NDIlib_recv_ptz_zoom_speed(recv, 0);
}

// Find and list NDI sources
std::vector<NDIlib_source_t> discoverSources(int timeoutMs = 3000) {
    std::vector<NDIlib_source_t> result;
    
    NDIlib_find_create_t findCreate = {};
    findCreate.show_local_sources = true;
    
    NDIlib_find_instance_t finder = NDIlib_find_create_v2(&findCreate);
    if (!finder) return result;
    
    // Wait for sources
    int elapsed = 0;
    while (elapsed < timeoutMs && g_running) {
        NDIlib_find_wait_for_sources(finder, 100);
        elapsed += 100;
    }
    
    uint32_t numSources = 0;
    const NDIlib_source_t* sources = NDIlib_find_get_current_sources(finder, &numSources);
    
    for (uint32_t i = 0; i < numSources; i++) {
        result.push_back(sources[i]);
    }
    
    // Note: We don't destroy finder here to keep source pointers valid
    // In production code, you'd copy the strings
    
    return result;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    // Initialize NDI
    if (!NDIlib_initialize()) {
        std::cerr << "Failed to initialize NDI\n";
        return 1;
    }
    
    std::cout << "NDI Viewer - Controls:\n";
    std::cout << "  Q/ESC: Quit\n";
    std::cout << "  F/F11: Toggle fullscreen\n";
    std::cout << "  Arrows: Pan/Tilt (PTZ)\n";
    std::cout << "  +/-: Zoom in/out (PTZ)\n";
    std::cout << "  1-9: Recall preset (PTZ)\n\n";
    
    // Discover sources
    std::cout << "Discovering NDI sources...\n";
    auto sources = discoverSources(3000);
    
    if (sources.empty()) {
        std::cerr << "No NDI sources found\n";
        NDIlib_destroy();
        return 1;
    }
    
    // Select source
    const NDIlib_source_t* selectedSource = nullptr;
    
    if (argc > 1) {
        // Find by name
        std::string targetName = argv[1];
        for (const auto& src : sources) {
            if (std::string(src.p_ndi_name).find(targetName) != std::string::npos) {
                selectedSource = &src;
                break;
            }
        }
        if (!selectedSource) {
            std::cerr << "Source '" << targetName << "' not found\n";
        }
    }
    
    if (!selectedSource) {
        // Show list
        std::cout << "\nAvailable sources:\n";
        for (size_t i = 0; i < sources.size(); i++) {
            std::cout << "  [" << (i + 1) << "] " << sources[i].p_ndi_name << "\n";
        }
        
        std::cout << "\nSelect source (1-" << sources.size() << "): ";
        int choice = 0;
        std::cin >> choice;
        
        if (choice < 1 || choice > (int)sources.size()) {
            std::cerr << "Invalid selection\n";
            NDIlib_destroy();
            return 1;
        }
        selectedSource = &sources[choice - 1];
    }
    
    std::cout << "Connecting to: " << selectedSource->p_ndi_name << "\n";
    
    // Create NDI receiver
    NDIlib_recv_create_v3_t recvCreate = {};
    recvCreate.source_to_connect_to = *selectedSource;
    recvCreate.color_format = NDIlib_recv_color_format_BGRX_BGRA;
    recvCreate.bandwidth = NDIlib_recv_bandwidth_highest;
    recvCreate.allow_video_fields = false;
    
    NDIlib_recv_instance_t receiver = NDIlib_recv_create_v3(&recvCreate);
    if (!receiver) {
        std::cerr << "Failed to create NDI receiver\n";
        NDIlib_destroy();
        return 1;
    }

    // Check PTZ support
    checkPTZSupport(receiver);

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        std::cerr << "SDL init failed: " << SDL_GetError() << "\n";
        NDIlib_recv_destroy(receiver);
        NDIlib_destroy();
        return 1;
    }
    
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    int currentWidth = 0, currentHeight = 0;
    
    std::cout << "Waiting for video...\n";
    
    // Main loop
    while (g_running) {
        // Handle SDL events
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    g_running = false;
                    break;

                case SDL_KEYDOWN:
                    switch (event.key.keysym.sym) {
                        case SDLK_ESCAPE:
                        case SDLK_q:
                            g_running = false;
                            break;
                        // PTZ controls
                        case SDLK_UP:
                            sendPTZ(receiver, 0, g_ptzSpeed);
                            break;
                        case SDLK_DOWN:
                            sendPTZ(receiver, 0, -g_ptzSpeed);
                            break;
                        case SDLK_LEFT:
                            sendPTZ(receiver, -g_ptzSpeed, 0);
                            break;
                        case SDLK_RIGHT:
                            sendPTZ(receiver, g_ptzSpeed, 0);
                            break;
                        // Zoom
                        case SDLK_PLUS:
                        case SDLK_KP_PLUS:
                        case SDLK_EQUALS:  // = key (+ without shift on US keyboard)
                            sendZoom(receiver, g_ptzSpeed);
                            break;
                        case SDLK_MINUS:
                        case SDLK_KP_MINUS:
                            sendZoom(receiver, -g_ptzSpeed);
                            break;
                        // Presets 1-9
                        case SDLK_1: case SDLK_2: case SDLK_3:
                        case SDLK_4: case SDLK_5: case SDLK_6:
                        case SDLK_7: case SDLK_8: case SDLK_9:
                            recallPreset(receiver, event.key.keysym.sym - SDLK_0);
                            break;
                        // Fullscreen toggle
                        case SDLK_f:
                        case SDLK_F11:
                            if (window) {
                                g_fullscreen = !g_fullscreen;
                                SDL_SetWindowFullscreen(window, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                            }
                            break;
                    }
                    break;

                case SDL_KEYUP:
                    switch (event.key.keysym.sym) {
                        case SDLK_UP:
                        case SDLK_DOWN:
                        case SDLK_LEFT:
                        case SDLK_RIGHT:
                            sendPTZ(receiver, 0, 0);
                            break;
                        case SDLK_PLUS:
                        case SDLK_KP_PLUS:
                        case SDLK_EQUALS:
                        case SDLK_MINUS:
                        case SDLK_KP_MINUS:
                            sendZoom(receiver, 0);
                            break;
                    }
                    break;
            }
        }
        
        // Capture NDI frame
        NDIlib_video_frame_v2_t videoFrame;
        NDIlib_audio_frame_v2_t audioFrame;
        NDIlib_metadata_frame_t metadataFrame;
        
        switch (NDIlib_recv_capture_v2(receiver, &videoFrame, &audioFrame, &metadataFrame, 100)) {
            case NDIlib_frame_type_video:
                // Create/resize window if needed
                if (!window || videoFrame.xres != currentWidth || videoFrame.yres != currentHeight) {
                    currentWidth = videoFrame.xres;
                    currentHeight = videoFrame.yres;
                    
                    if (texture) SDL_DestroyTexture(texture);
                    if (renderer) SDL_DestroyRenderer(renderer);
                    if (window) SDL_DestroyWindow(window);
                    
                    std::string title = std::string("NDI Viewer - ") + selectedSource->p_ndi_name;
                    window = SDL_CreateWindow(title.c_str(),
                        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                        currentWidth, currentHeight,
                        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
                    
                    renderer = SDL_CreateRenderer(window, -1, 
                        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
                    
                    texture = SDL_CreateTexture(renderer,
                        SDL_PIXELFORMAT_BGRA32,
                        SDL_TEXTUREACCESS_STREAMING,
                        currentWidth, currentHeight);
                    
                    std::cout << "Video: " << currentWidth << "x" << currentHeight << "\n";
                }
                
                // Update texture
                SDL_UpdateTexture(texture, nullptr, videoFrame.p_data, videoFrame.line_stride_in_bytes);
                
                // Render
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, nullptr, nullptr);
                SDL_RenderPresent(renderer);
                
                // Free NDI frame
                NDIlib_recv_free_video_v2(receiver, &videoFrame);
                break;
                
            case NDIlib_frame_type_audio:
                playAudio(audioFrame);
                NDIlib_recv_free_audio_v2(receiver, &audioFrame);
                break;
                
            case NDIlib_frame_type_metadata:
                NDIlib_recv_free_metadata(receiver, &metadataFrame);
                break;
                
            case NDIlib_frame_type_none:
                // No frame available
                break;
                
            case NDIlib_frame_type_error:
                std::cerr << "NDI connection lost\n";
                g_running = false;
                break;
                
            default:
                break;
        }
    }
    
    // Cleanup
    std::cout << "Shutting down...\n";

    if (g_audioDevice) SDL_CloseAudioDevice(g_audioDevice);
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    
    NDIlib_recv_destroy(receiver);
    NDIlib_destroy();
    
    return 0;
}
