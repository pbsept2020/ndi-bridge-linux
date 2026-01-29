/**
 * NDI Viewer - SDL2 GUI NDI source viewer with PTZ controls
 *
 * Features:
 * - Graphical source picker (no terminal needed)
 * - PTZ controls: arrows (pan/tilt), +/- (zoom), 1-9 (presets)
 * - Fullscreen toggle: F, F11, or double-click
 * - Visual PTZ indicator overlay
 */

#include <iostream>
#include <string>
#include <vector>
#include <atomic>
#include <csignal>
#include <cstring>
#include <chrono>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <Processing.NDI.Lib.h>

#include "common/Version.h"
static const char* VIEWER_VERSION = NDI_BRIDGE_VERSION;

// Global state
std::atomic<bool> g_running{true};
bool g_fullscreen = false;

// PTZ state
float g_ptzSpeed = 0.5f;
bool g_ptzSupported = false;
std::string g_ptzMessage;
std::chrono::steady_clock::time_point g_ptzMessageTime;

// Audio state
SDL_AudioDeviceID g_audioDevice = 0;
int g_audioSampleRate = 0;
int g_audioChannels = 0;

// Font
TTF_Font* g_font = nullptr;
TTF_Font* g_fontSmall = nullptr;
TTF_Font* g_fontLarge = nullptr;

void signalHandler(int) {
    g_running = false;
}

// Colors
const SDL_Color COLOR_BG = {20, 20, 25, 255};
const SDL_Color COLOR_CARD = {35, 35, 45, 255};
const SDL_Color COLOR_CARD_HOVER = {50, 50, 65, 255};
const SDL_Color COLOR_CARD_SELECTED = {59, 130, 246, 255};
const SDL_Color COLOR_TEXT = {250, 250, 250, 255};
const SDL_Color COLOR_TEXT_DIM = {150, 150, 160, 255};
const SDL_Color COLOR_SUCCESS = {34, 197, 94, 255};
const SDL_Color COLOR_PTZ_BG = {0, 0, 0, 180};

// Initialize or reinitialize audio device
bool initAudio(int sampleRate, int channels) {
    if (g_audioDevice && g_audioSampleRate == sampleRate && g_audioChannels == channels) {
        return true;
    }

    if (g_audioDevice) {
        SDL_CloseAudioDevice(g_audioDevice);
        g_audioDevice = 0;
    }

    SDL_AudioSpec wanted = {};
    wanted.freq = sampleRate;
    wanted.format = AUDIO_F32SYS;
    wanted.channels = channels;
    wanted.samples = 1024;
    wanted.callback = nullptr;

    g_audioDevice = SDL_OpenAudioDevice(nullptr, 0, &wanted, nullptr, 0);
    if (!g_audioDevice) {
        std::cerr << "Failed to open audio: " << SDL_GetError() << "\n";
        return false;
    }

    g_audioSampleRate = sampleRate;
    g_audioChannels = channels;
    SDL_PauseAudioDevice(g_audioDevice, 0);
    return true;
}

void playAudio(const NDIlib_audio_frame_v2_t& frame) {
    if (!initAudio(frame.sample_rate, frame.no_channels)) return;

    int numSamples = frame.no_samples;
    int numChannels = frame.no_channels;
    std::vector<float> interleaved(numSamples * numChannels);

    for (int s = 0; s < numSamples; s++) {
        for (int c = 0; c < numChannels; c++) {
            float* channelData = frame.p_data + c * frame.channel_stride_in_bytes / sizeof(float);
            interleaved[s * numChannels + c] = channelData[s];
        }
    }

    SDL_QueueAudio(g_audioDevice, interleaved.data(), interleaved.size() * sizeof(float));
}

// PTZ functions
void checkPTZSupport(NDIlib_recv_instance_t recv) {
    g_ptzSupported = NDIlib_recv_ptz_is_supported(recv);
    std::cout << "PTZ supported: " << (g_ptzSupported ? "yes" : "no") << "\n";
}

void showPTZMessage(const std::string& msg) {
    g_ptzMessage = msg;
    g_ptzMessageTime = std::chrono::steady_clock::now();
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
    NDIlib_recv_ptz_recall_preset(recv, preset, 1.0f);
    showPTZMessage("Preset " + std::to_string(preset));
}

void stopPTZ(NDIlib_recv_instance_t recv) {
    if (!g_ptzSupported) return;
    NDIlib_recv_ptz_pan_tilt_speed(recv, 0, 0);
    NDIlib_recv_ptz_zoom_speed(recv, 0);
}

// Text rendering helper
SDL_Texture* renderText(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, SDL_Color color) {
    if (!font || text.empty()) return nullptr;
    SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!surface) return nullptr;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    return texture;
}

void renderTextAt(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, int x, int y, SDL_Color color) {
    SDL_Texture* tex = renderText(renderer, font, text, color);
    if (!tex) return;
    int w, h;
    SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
    SDL_Rect dst = {x, y, w, h};
    SDL_RenderCopy(renderer, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
}

void renderTextCentered(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, int centerX, int y, SDL_Color color) {
    SDL_Texture* tex = renderText(renderer, font, text, color);
    if (!tex) return;
    int w, h;
    SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
    SDL_Rect dst = {centerX - w/2, y, w, h};
    SDL_RenderCopy(renderer, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
}

// Draw rounded rectangle
void drawRoundedRect(SDL_Renderer* renderer, SDL_Rect rect, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &rect);
}

// Source discovery
struct NDISourceInfo {
    std::string name;
    std::string displayName;
    NDIlib_source_t source;
};

std::vector<NDISourceInfo> discoverSources(int timeoutMs = 3000) {
    std::vector<NDISourceInfo> result;

    NDIlib_find_create_t findCreate = {};
    findCreate.show_local_sources = true;

    NDIlib_find_instance_t finder = NDIlib_find_create_v2(&findCreate);
    if (!finder) return result;

    int elapsed = 0;
    while (elapsed < timeoutMs && g_running) {
        NDIlib_find_wait_for_sources(finder, 100);
        elapsed += 100;
    }

    uint32_t numSources = 0;
    const NDIlib_source_t* sources = NDIlib_find_get_current_sources(finder, &numSources);

    for (uint32_t i = 0; i < numSources; i++) {
        NDISourceInfo info;
        info.name = sources[i].p_ndi_name ? sources[i].p_ndi_name : "";
        info.source = sources[i];

        // Extract display name (part in parentheses or full name)
        size_t paren = info.name.find('(');
        if (paren != std::string::npos) {
            info.displayName = info.name.substr(paren + 1);
            if (!info.displayName.empty() && info.displayName.back() == ')')
                info.displayName.pop_back();
        } else {
            info.displayName = info.name;
        }

        result.push_back(info);
    }

    // Don't destroy finder to keep pointers valid
    return result;
}

// Source picker GUI
int showSourcePicker(SDL_Window* window, SDL_Renderer* renderer, std::vector<NDISourceInfo>& sources) {
    int selected = -1;
    int hovered = -1;
    bool searching = true;

    // Initial search
    sources = discoverSources(2000);
    searching = false;

    auto lastRefresh = std::chrono::steady_clock::now();

    while (g_running && selected < 0) {
        // Refresh sources every 2 seconds
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastRefresh).count() >= 2) {
            auto newSources = discoverSources(500);
            if (newSources.size() != sources.size()) {
                sources = newSources;
            }
            lastRefresh = now;
        }

        // Handle events
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                g_running = false;
            } else if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                    g_running = false;
                } else if (event.key.keysym.sym == SDLK_RETURN && hovered >= 0) {
                    selected = hovered;
                } else if (event.key.keysym.sym == SDLK_DOWN) {
                    hovered = std::min(hovered + 1, (int)sources.size() - 1);
                } else if (event.key.keysym.sym == SDLK_UP) {
                    hovered = std::max(hovered - 1, 0);
                }
            } else if (event.type == SDL_MOUSEMOTION) {
                int y = event.motion.y;
                int startY = 120;
                int cardH = 70;
                int gap = 10;
                hovered = -1;
                for (size_t i = 0; i < sources.size(); i++) {
                    int cardY = startY + i * (cardH + gap);
                    if (y >= cardY && y < cardY + cardH) {
                        hovered = i;
                        break;
                    }
                }
            } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                if (hovered >= 0) {
                    selected = hovered;
                }
            }
        }

        // Render
        SDL_SetRenderDrawColor(renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, 255);
        SDL_RenderClear(renderer);

        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);

        // Title
        renderTextCentered(renderer, g_fontLarge, std::string("NDI Viewer ") + VIEWER_VERSION, winW / 2, 30, COLOR_TEXT);
        renderTextCentered(renderer, g_fontSmall, "Select a source", winW / 2, 75, COLOR_TEXT_DIM);

        // Source cards
        int startY = 120;
        int cardH = 70;
        int gap = 10;
        int margin = 30;

        if (sources.empty()) {
            renderTextCentered(renderer, g_font, searching ? "Searching..." : "No sources found",
                             winW / 2, startY + 30, COLOR_TEXT_DIM);
        } else {
            for (size_t i = 0; i < sources.size(); i++) {
                SDL_Rect card = {margin, startY + (int)i * (cardH + gap), winW - margin * 2, cardH};

                SDL_Color cardColor = COLOR_CARD;
                if ((int)i == hovered) cardColor = COLOR_CARD_HOVER;
                if ((int)i == selected) cardColor = COLOR_CARD_SELECTED;

                drawRoundedRect(renderer, card, cardColor);

                // Source name
                renderTextAt(renderer, g_font, sources[i].displayName, card.x + 20, card.y + 15, COLOR_TEXT);
                renderTextAt(renderer, g_fontSmall, sources[i].name, card.x + 20, card.y + 42, COLOR_TEXT_DIM);
            }
        }

        // Status bar
        std::string status = std::to_string(sources.size()) + " source" + (sources.size() != 1 ? "s" : "") + " found";
        renderTextCentered(renderer, g_fontSmall, status, winW / 2, winH - 40,
                          sources.empty() ? COLOR_TEXT_DIM : COLOR_SUCCESS);

        // Help text
        renderTextCentered(renderer, g_fontSmall, "Click to select  |  ESC to quit", winW / 2, winH - 20, COLOR_TEXT_DIM);

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    return selected;
}

// Main viewer
void runViewer(SDL_Window* window, SDL_Renderer* renderer, const NDISourceInfo& source) {
    std::cout << "Connecting to: " << source.name << "\n";

    // Create NDI receiver
    NDIlib_recv_create_v3_t recvCreate = {};
    recvCreate.source_to_connect_to = source.source;
    recvCreate.color_format = NDIlib_recv_color_format_BGRX_BGRA;
    recvCreate.bandwidth = NDIlib_recv_bandwidth_highest;
    recvCreate.allow_video_fields = false;

    NDIlib_recv_instance_t receiver = NDIlib_recv_create_v3(&recvCreate);
    if (!receiver) {
        std::cerr << "Failed to create NDI receiver\n";
        return;
    }

    checkPTZSupport(receiver);

    SDL_Texture* videoTexture = nullptr;
    int videoWidth = 0, videoHeight = 0;
    NDIlib_FourCC_video_type_e videoFourCC = (NDIlib_FourCC_video_type_e)0;
    bool connected = false;

    Uint32 lastClickTime = 0;

    // Set window title with version (updated later with format info)
    std::string baseTitle = std::string("NDI Viewer ") + VIEWER_VERSION + " - " + source.displayName;
    SDL_SetWindowTitle(window, baseTitle.c_str());

    while (g_running) {
        // Handle events
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    g_running = false;
                    break;

                case SDL_MOUSEBUTTONDOWN:
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        Uint32 now = SDL_GetTicks();
                        if (now - lastClickTime < 300) {
                            // Double-click: toggle fullscreen
                            g_fullscreen = !g_fullscreen;
                            SDL_SetWindowFullscreen(window, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                        }
                        lastClickTime = now;
                    }
                    break;

                case SDL_KEYDOWN:
                    switch (event.key.keysym.sym) {
                        case SDLK_ESCAPE:
                            if (g_fullscreen) {
                                g_fullscreen = false;
                                SDL_SetWindowFullscreen(window, 0);
                            } else {
                                g_running = false;
                            }
                            break;
                        case SDLK_q:
                            g_running = false;
                            break;
                        case SDLK_f:
                        case SDLK_F11:
                            g_fullscreen = !g_fullscreen;
                            SDL_SetWindowFullscreen(window, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                            break;
                        // PTZ
                        case SDLK_UP:
                            sendPTZ(receiver, 0, g_ptzSpeed);
                            showPTZMessage("Tilt Up");
                            break;
                        case SDLK_DOWN:
                            sendPTZ(receiver, 0, -g_ptzSpeed);
                            showPTZMessage("Tilt Down");
                            break;
                        case SDLK_LEFT:
                            sendPTZ(receiver, -g_ptzSpeed, 0);
                            showPTZMessage("Pan Left");
                            break;
                        case SDLK_RIGHT:
                            sendPTZ(receiver, g_ptzSpeed, 0);
                            showPTZMessage("Pan Right");
                            break;
                        case SDLK_PLUS:
                        case SDLK_KP_PLUS:
                        case SDLK_EQUALS:
                            sendZoom(receiver, g_ptzSpeed);
                            showPTZMessage("Zoom In");
                            break;
                        case SDLK_MINUS:
                        case SDLK_KP_MINUS:
                            sendZoom(receiver, -g_ptzSpeed);
                            showPTZMessage("Zoom Out");
                            break;
                        case SDLK_1: case SDLK_2: case SDLK_3:
                        case SDLK_4: case SDLK_5: case SDLK_6:
                        case SDLK_7: case SDLK_8: case SDLK_9:
                            recallPreset(receiver, event.key.keysym.sym - SDLK_0);
                            break;
                    }
                    break;

                case SDL_KEYUP:
                    switch (event.key.keysym.sym) {
                        case SDLK_UP: case SDLK_DOWN: case SDLK_LEFT: case SDLK_RIGHT:
                            sendPTZ(receiver, 0, 0);
                            break;
                        case SDLK_PLUS: case SDLK_KP_PLUS: case SDLK_EQUALS:
                        case SDLK_MINUS: case SDLK_KP_MINUS:
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

        switch (NDIlib_recv_capture_v2(receiver, &videoFrame, &audioFrame, &metadataFrame, 50)) {
            case NDIlib_frame_type_video:
                connected = true;
                if (videoFrame.xres != videoWidth || videoFrame.yres != videoHeight) {
                    videoWidth = videoFrame.xres;
                    videoHeight = videoFrame.yres;

                    if (videoTexture) SDL_DestroyTexture(videoTexture);
                    videoTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                        SDL_TEXTUREACCESS_STREAMING, videoWidth, videoHeight);
                    std::cout << "Video: " << videoWidth << "x" << videoHeight
                              << " stride=" << videoFrame.line_stride_in_bytes
                              << " FourCC=0x" << std::hex << videoFrame.FourCC << std::dec << "\n";

                    if (!g_fullscreen) {
                        SDL_SetWindowSize(window, videoWidth, videoHeight);
                        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                    }
                }

                SDL_UpdateTexture(videoTexture, nullptr, videoFrame.p_data, videoFrame.line_stride_in_bytes);
                NDIlib_recv_free_video_v2(receiver, &videoFrame);
                break;

            case NDIlib_frame_type_audio:
                playAudio(audioFrame);
                NDIlib_recv_free_audio_v2(receiver, &audioFrame);
                break;

            case NDIlib_frame_type_metadata:
                NDIlib_recv_free_metadata(receiver, &metadataFrame);
                break;

            case NDIlib_frame_type_error:
                std::cerr << "NDI connection lost\n";
                connected = false;
                break;

            default:
                break;
        }

        // Render
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        if (videoTexture && videoWidth > 0 && videoHeight > 0) {
            // Preserve aspect ratio (letterbox/pillarbox)
            int winW, winH;
            SDL_GetWindowSize(window, &winW, &winH);

            float videoAspect = (float)videoWidth / (float)videoHeight;
            float windowAspect = (float)winW / (float)winH;

            SDL_Rect dstRect;
            if (windowAspect > videoAspect) {
                // Window wider than video → pillarbox
                dstRect.h = winH;
                dstRect.w = (int)(winH * videoAspect);
                dstRect.x = (winW - dstRect.w) / 2;
                dstRect.y = 0;
            } else {
                // Window taller than video → letterbox
                dstRect.w = winW;
                dstRect.h = (int)(winW / videoAspect);
                dstRect.x = 0;
                dstRect.y = (winH - dstRect.h) / 2;
            }

            SDL_RenderCopy(renderer, videoTexture, nullptr, &dstRect);
        } else {
            int winW, winH;
            SDL_GetWindowSize(window, &winW, &winH);
            renderTextCentered(renderer, g_font, connected ? "Receiving..." : "Connecting...",
                             winW / 2, winH / 2, COLOR_TEXT);
        }

        // PTZ overlay
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_ptzMessageTime).count();
        if (elapsed < 1500 && !g_ptzMessage.empty() && g_font) {
            int winW, winH;
            SDL_GetWindowSize(window, &winW, &winH);

            // Background pill
            int textW = g_ptzMessage.length() * 12 + 40;
            SDL_Rect pillRect = {winW / 2 - textW / 2, winH - 80, textW, 40};
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
            SDL_RenderFillRect(renderer, &pillRect);

            renderTextCentered(renderer, g_font, g_ptzMessage, winW / 2, winH - 70, COLOR_TEXT);
        }

        // PTZ indicator (if supported)
        if (g_ptzSupported && g_fontSmall) {
            renderTextAt(renderer, g_fontSmall, "PTZ", 10, 10, COLOR_SUCCESS);
        }

        SDL_RenderPresent(renderer);
    }

    // Cleanup
    if (videoTexture) SDL_DestroyTexture(videoTexture);
    if (g_audioDevice) SDL_CloseAudioDevice(g_audioDevice);
    g_audioDevice = 0;
    NDIlib_recv_destroy(receiver);
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Initialize NDI
    if (!NDIlib_initialize()) {
        std::cerr << "Failed to initialize NDI\n";
        return 1;
    }

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        std::cerr << "SDL init failed: " << SDL_GetError() << "\n";
        NDIlib_destroy();
        return 1;
    }

    // Initialize TTF
    if (TTF_Init() < 0) {
        std::cerr << "TTF init failed: " << TTF_GetError() << "\n";
        SDL_Quit();
        NDIlib_destroy();
        return 1;
    }

    // Load fonts
    const char* fontPaths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        nullptr
    };

    for (int i = 0; fontPaths[i] && !g_font; i++) {
        g_font = TTF_OpenFont(fontPaths[i], 18);
        if (g_font) {
            g_fontSmall = TTF_OpenFont(fontPaths[i], 13);
            g_fontLarge = TTF_OpenFont(fontPaths[i], 32);
        }
    }

    if (!g_font) {
        std::cerr << "Warning: Could not load font, text will not display\n";
    }

    // Create window
    SDL_Window* window = SDL_CreateWindow((std::string("NDI Viewer ") + VIEWER_VERSION).c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        500, 450,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

    if (!window) {
        std::cerr << "Failed to create window: " << SDL_GetError() << "\n";
        TTF_Quit();
        SDL_Quit();
        NDIlib_destroy();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    if (!renderer) {
        std::cerr << "Failed to create renderer: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        NDIlib_destroy();
        return 1;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Check command line for source name
    std::vector<NDISourceInfo> sources;
    int selectedSource = -1;

    if (argc > 1) {
        std::string targetName = argv[1];
        sources = discoverSources(3000);
        for (size_t i = 0; i < sources.size(); i++) {
            if (sources[i].name.find(targetName) != std::string::npos) {
                selectedSource = i;
                break;
            }
        }
        if (selectedSource < 0) {
            std::cerr << "Source '" << targetName << "' not found, showing picker\n";
        }
    }

    // Show source picker if no source specified
    if (selectedSource < 0) {
        selectedSource = showSourcePicker(window, renderer, sources);
    }

    // Run viewer
    if (selectedSource >= 0 && selectedSource < (int)sources.size() && g_running) {
        runViewer(window, renderer, sources[selectedSource]);
    }

    // Cleanup
    if (g_fontLarge) TTF_CloseFont(g_fontLarge);
    if (g_fontSmall) TTF_CloseFont(g_fontSmall);
    if (g_font) TTF_CloseFont(g_font);
    TTF_Quit();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    NDIlib_destroy();

    std::cout << "Bye!\n";
    return 0;
}
