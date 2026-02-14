/**
 * NDI Viewer - SDL2 GUI NDI source viewer with PTZ controls
 *
 * Features:
 * - Graphical source picker (no terminal needed)
 * - PTZ controls: arrows (pan/tilt), +/- (zoom), 1-9 (presets)
 * - Fullscreen toggle: F, F11, or double-click
 * - Visual PTZ indicator overlay
 * - OSD stream info (I), safe area (S), grid (G)
 * - Flip H/V, fill/fit (W), low bandwidth (L)
 * - Source switching (N/P), refresh (R), help (?/F1)
 * - Tally border (automatic from NDI metadata)
 */

#include <iostream>
#include <string>
#include <vector>
#include <atomic>
#include <csignal>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <cmath>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <Processing.NDI.Lib.h>

#include "common/Version.h"
static const char* VIEWER_VERSION = NDI_BRIDGE_VERSION;

// Global state
std::atomic<bool> g_running{true};

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

// Viewer state (replaces scattered globals)
struct ViewerState {
    bool fullscreen = false;
    bool showOSD = false;       // I
    bool showSafeArea = false;  // S
    bool showGrid = false;      // G
    bool showHelp = false;      // ? / F1
    bool flipH = false;         // H
    bool flipV = false;         // V
    bool fillMode = false;      // W
    bool lowBandwidth = false;  // L
    bool tallyProgram = false;  // auto (NDI metadata)
    bool tallyPreview = false;  // auto (NDI metadata)
    // Stream info for OSD
    int videoWidth = 0;
    int videoHeight = 0;
    int frameRateN = 0;
    int frameRateD = 1;
    int64_t timecode = -1;
    std::string sourceName;
    int currentSourceIndex = 0;
    bool connected = false;
};

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
const SDL_Color COLOR_OSD_BG = {0, 0, 0, 160};
const SDL_Color COLOR_TALLY_PROGRAM = {255, 0, 0, 255};
const SDL_Color COLOR_TALLY_PREVIEW = {0, 255, 0, 255};

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

// --- New helpers ---

// Format NDI timecode (100ns ticks) to HH:MM:SS:FF
std::string formatTimecode(int64_t tc, int rateN, int rateD) {
    if (tc < 0 || rateN <= 0 || rateD <= 0) return "--:--:--:--";
    double fps = (double)rateN / (double)rateD;
    if (fps <= 0) return "--:--:--:--";

    // NDI timecode is in 100ns units
    int64_t totalFrames = (int64_t)((double)tc / 1e7 * fps);
    int ff = (int)(totalFrames % (int)std::round(fps));
    int64_t totalSec = (int64_t)((double)tc / 1e7);
    int ss = (int)(totalSec % 60);
    int mm = (int)((totalSec / 60) % 60);
    int hh = (int)((totalSec / 3600) % 24);

    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d", hh, mm, ss, ff);
    return buf;
}

// Draw dashed rectangle outline
void drawDashedRect(SDL_Renderer* renderer, SDL_Rect rect, int dashLen, int gapLen) {
    auto drawDashedLine = [&](int x1, int y1, int x2, int y2) {
        int dx = (x2 > x1) ? 1 : (x2 < x1) ? -1 : 0;
        int dy = (y2 > y1) ? 1 : (y2 < y1) ? -1 : 0;
        int len = std::max(std::abs(x2 - x1), std::abs(y2 - y1));
        int pos = 0;
        while (pos < len) {
            int segEnd = std::min(pos + dashLen, len);
            SDL_RenderDrawLine(renderer,
                x1 + dx * pos, y1 + dy * pos,
                x1 + dx * segEnd, y1 + dy * segEnd);
            pos = segEnd + gapLen;
        }
    };

    // Top
    drawDashedLine(rect.x, rect.y, rect.x + rect.w, rect.y);
    // Bottom
    drawDashedLine(rect.x, rect.y + rect.h, rect.x + rect.w, rect.y + rect.h);
    // Left
    drawDashedLine(rect.x, rect.y, rect.x, rect.y + rect.h);
    // Right
    drawDashedLine(rect.x + rect.w, rect.y, rect.x + rect.w, rect.y + rect.h);
}

// Compute video destination rect (fit or fill mode)
SDL_Rect computeVideoRect(int winW, int winH, int videoW, int videoH, bool fillMode) {
    if (videoW <= 0 || videoH <= 0) return {0, 0, winW, winH};

    float videoAspect = (float)videoW / (float)videoH;
    float windowAspect = (float)winW / (float)winH;

    SDL_Rect dst;
    if (fillMode) {
        // Fill: crop to fill entire window
        if (windowAspect > videoAspect) {
            dst.w = winW;
            dst.h = (int)(winW / videoAspect);
            dst.x = 0;
            dst.y = (winH - dst.h) / 2;
        } else {
            dst.h = winH;
            dst.w = (int)(winH * videoAspect);
            dst.x = (winW - dst.w) / 2;
            dst.y = 0;
        }
    } else {
        // Fit: letterbox/pillarbox
        if (windowAspect > videoAspect) {
            dst.h = winH;
            dst.w = (int)(winH * videoAspect);
            dst.x = (winW - dst.w) / 2;
            dst.y = 0;
        } else {
            dst.w = winW;
            dst.h = (int)(winW / videoAspect);
            dst.x = 0;
            dst.y = (winH - dst.h) / 2;
        }
    }
    return dst;
}

// Draw OSD panel (top-left, semi-transparent)
void drawOSD(SDL_Renderer* renderer, TTF_Font* font, TTF_Font* fontSmall, const ViewerState& state, const SDL_Rect& videoRect) {
    if (!font || !fontSmall) return;
    (void)videoRect;

    int x = 10, y = 30;
    int panelW = 260, panelH = 90;

    // Background
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Rect bg = {x, y, panelW, panelH};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
    SDL_RenderFillRect(renderer, &bg);

    SDL_Color white = {255, 255, 255, 255};
    SDL_Color dim = {180, 180, 180, 255};

    // Source name
    renderTextAt(renderer, fontSmall, state.sourceName, x + 8, y + 6, white);

    // Resolution + FPS
    char resFps[64];
    double fps = (state.frameRateD > 0) ? (double)state.frameRateN / state.frameRateD : 0;
    snprintf(resFps, sizeof(resFps), "%dx%d @ %.2f fps", state.videoWidth, state.videoHeight, fps);
    renderTextAt(renderer, fontSmall, resFps, x + 8, y + 26, dim);

    // Timecode
    std::string tc = formatTimecode(state.timecode, state.frameRateN, state.frameRateD);
    renderTextAt(renderer, font, tc, x + 8, y + 48, white);

    // Mode indicators
    std::string modes;
    if (state.flipH) modes += "FlipH ";
    if (state.flipV) modes += "FlipV ";
    if (state.fillMode) modes += "Fill ";
    if (state.lowBandwidth) modes += "LowBW ";
    if (!modes.empty()) {
        renderTextAt(renderer, fontSmall, modes, x + 8, y + 72, {255, 200, 50, 255});
    }
}

// Draw safe area overlays (action safe 90%, title safe 80%)
void drawSafeArea(SDL_Renderer* renderer, const SDL_Rect& videoRect) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 80);

    // Action safe: 5% inset (90%)
    int ax = videoRect.x + videoRect.w * 5 / 100;
    int ay = videoRect.y + videoRect.h * 5 / 100;
    int aw = videoRect.w * 90 / 100;
    int ah = videoRect.h * 90 / 100;
    SDL_Rect actionSafe = {ax, ay, aw, ah};
    drawDashedRect(renderer, actionSafe, 8, 6);

    // Title safe: 10% inset (80%)
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 50);
    int tx = videoRect.x + videoRect.w * 10 / 100;
    int ty = videoRect.y + videoRect.h * 10 / 100;
    int tw = videoRect.w * 80 / 100;
    int th = videoRect.h * 80 / 100;
    SDL_Rect titleSafe = {tx, ty, tw, th};
    drawDashedRect(renderer, titleSafe, 6, 8);
}

// Draw rule-of-thirds grid
void drawGrid(SDL_Renderer* renderer, const SDL_Rect& videoRect) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 100);

    int x = videoRect.x, y = videoRect.y;
    int w = videoRect.w, h = videoRect.h;

    // Vertical lines at 1/3 and 2/3
    SDL_RenderDrawLine(renderer, x + w / 3, y, x + w / 3, y + h);
    SDL_RenderDrawLine(renderer, x + 2 * w / 3, y, x + 2 * w / 3, y + h);

    // Horizontal lines at 1/3 and 2/3
    SDL_RenderDrawLine(renderer, x, y + h / 3, x + w, y + h / 3);
    SDL_RenderDrawLine(renderer, x, y + 2 * h / 3, x + w, y + 2 * h / 3);

    // Center cross (20px arms)
    int cx = x + w / 2, cy = y + h / 2;
    SDL_RenderDrawLine(renderer, cx - 10, cy, cx + 10, cy);
    SDL_RenderDrawLine(renderer, cx, cy - 10, cx, cy + 10);
}

// Draw tally border
void drawTallyBorder(SDL_Renderer* renderer, int winW, int winH, const ViewerState& state) {
    if (!state.tallyProgram && !state.tallyPreview) return;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    SDL_Color color = state.tallyProgram ? COLOR_TALLY_PROGRAM : COLOR_TALLY_PREVIEW;
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);

    int t = 4; // border thickness
    SDL_Rect top = {0, 0, winW, t};
    SDL_Rect bottom = {0, winH - t, winW, t};
    SDL_Rect left = {0, 0, t, winH};
    SDL_Rect right = {winW - t, 0, t, winH};

    SDL_RenderFillRect(renderer, &top);
    SDL_RenderFillRect(renderer, &bottom);
    SDL_RenderFillRect(renderer, &left);
    SDL_RenderFillRect(renderer, &right);
}

// Draw help overlay (centered panel listing keyboard shortcuts)
void drawHelpOverlay(SDL_Renderer* renderer, TTF_Font* font, TTF_Font* fontSmall, int winW, int winH) {
    if (!font || !fontSmall) return;

    int panelW = 340, panelH = 420;
    int px = (winW - panelW) / 2;
    int py = (winH - panelH) / 2;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Rect bg = {px, py, panelW, panelH};
    SDL_SetRenderDrawColor(renderer, 20, 20, 30, 230);
    SDL_RenderFillRect(renderer, &bg);

    // Border
    SDL_SetRenderDrawColor(renderer, 100, 100, 120, 200);
    SDL_RenderDrawRect(renderer, &bg);

    SDL_Color white = {255, 255, 255, 255};
    SDL_Color dim = {160, 160, 170, 255};
    SDL_Color accent = {100, 180, 255, 255};

    int y = py + 12;
    int lineH = 22;

    renderTextCentered(renderer, font, "Keyboard Shortcuts", winW / 2, y, white);
    y += lineH + 8;

    struct { const char* key; const char* desc; } shortcuts[] = {
        {"F / F11",   "Toggle fullscreen"},
        {"I",         "OSD stream info"},
        {"S",         "Safe area overlay"},
        {"G",         "Rule of thirds grid"},
        {"H",         "Flip horizontal"},
        {"V",         "Flip vertical"},
        {"W",         "Fill / fit toggle"},
        {"L",         "Low bandwidth toggle"},
        {"N",         "Next source"},
        {"P",         "Previous source"},
        {"R",         "Refresh sources"},
        {"Arrows",    "PTZ pan/tilt"},
        {"+/-",       "PTZ zoom"},
        {"1-9",       "PTZ presets"},
        {"Q / Esc",   "Quit"},
    };

    for (auto& s : shortcuts) {
        renderTextAt(renderer, fontSmall, s.key, px + 20, y, accent);
        renderTextAt(renderer, fontSmall, s.desc, px + 120, y, dim);
        y += lineH;
    }

    y += 8;
    renderTextCentered(renderer, fontSmall, "Press any key to dismiss", winW / 2, y, dim);
}

// Parse NDI tally metadata XML
void parseTallyMetadata(const char* xml, ViewerState& state) {
    if (!xml) return;
    std::string s(xml);

    // Simple parser for <ndi_tally_echo on_program="true" on_preview="false"/>
    state.tallyProgram = (s.find("on_program=\"true\"") != std::string::npos);
    state.tallyPreview = (s.find("on_preview=\"true\"") != std::string::npos);
}

// Source discovery
struct NDISourceInfo {
    std::string name;
    std::string displayName;
    NDIlib_source_t source;
};

std::vector<NDISourceInfo> getSourcesFromFinder(NDIlib_find_instance_t finder) {
    std::vector<NDISourceInfo> result;
    if (!finder) return result;

    uint32_t numSources = 0;
    const NDIlib_source_t* sources = NDIlib_find_get_current_sources(finder, &numSources);

    for (uint32_t i = 0; i < numSources; i++) {
        NDISourceInfo info;
        info.name = sources[i].p_ndi_name ? sources[i].p_ndi_name : "";
        info.source = sources[i];

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
    return result;
}

// Source picker GUI
int showSourcePicker(SDL_Window* window, SDL_Renderer* renderer, NDIlib_find_instance_t finder, std::vector<NDISourceInfo>& sources) {
    int selected = -1;
    int hovered = -1;

    // Wait for initial discovery
    NDIlib_find_wait_for_sources(finder, 2000);
    sources = getSourcesFromFinder(finder);

    auto lastRefresh = std::chrono::steady_clock::now();

    while (g_running && selected < 0) {
        // Refresh sources every 2 seconds
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastRefresh).count() >= 2) {
            NDIlib_find_wait_for_sources(finder, 200);
            auto newSources = getSourcesFromFinder(finder);
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
        renderTextCentered(renderer, g_fontSmall, "Select a source  |  ? for help", winW / 2, 75, COLOR_TEXT_DIM);

        // Source cards
        int startY = 120;
        int cardH = 70;
        int gap = 10;
        int margin = 30;

        if (sources.empty()) {
            renderTextCentered(renderer, g_font, "Searching...",
                             winW / 2, startY + 30, COLOR_TEXT_DIM);
        } else {
            for (size_t i = 0; i < sources.size(); i++) {
                SDL_Rect card = {margin, startY + (int)i * (cardH + gap), winW - margin * 2, cardH};

                SDL_Color cardColor = COLOR_CARD;
                if ((int)i == hovered) cardColor = COLOR_CARD_HOVER;
                if ((int)i == selected) cardColor = COLOR_CARD_SELECTED;

                drawRoundedRect(renderer, card, cardColor);

                renderTextAt(renderer, g_font, sources[i].displayName, card.x + 20, card.y + 15, COLOR_TEXT);
                renderTextAt(renderer, g_fontSmall, sources[i].name, card.x + 20, card.y + 42, COLOR_TEXT_DIM);
            }
        }

        // Status bar
        std::string status = std::to_string(sources.size()) + " source" + (sources.size() != 1 ? "s" : "") + " found";
        renderTextCentered(renderer, g_fontSmall, status, winW / 2, winH - 40,
                          sources.empty() ? COLOR_TEXT_DIM : COLOR_SUCCESS);

        renderTextCentered(renderer, g_fontSmall, "Click to select  |  ESC to quit", winW / 2, winH - 20, COLOR_TEXT_DIM);

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    return selected;
}

// Create NDI receiver for a given source
NDIlib_recv_instance_t createReceiver(const NDIlib_source_t& source, bool lowBandwidth) {
    NDIlib_recv_create_v3_t recvCreate = {};
    recvCreate.source_to_connect_to = source;
    recvCreate.color_format = NDIlib_recv_color_format_BGRX_BGRA;
    recvCreate.bandwidth = lowBandwidth ? NDIlib_recv_bandwidth_lowest : NDIlib_recv_bandwidth_highest;
    recvCreate.allow_video_fields = false;
    return NDIlib_recv_create_v3(&recvCreate);
}

// Main viewer
void runViewer(SDL_Window* window, SDL_Renderer* renderer, NDIlib_find_instance_t finder, int sourceIndex) {
    // Get current sources
    std::vector<NDISourceInfo> sources = getSourcesFromFinder(finder);
    if (sourceIndex < 0 || sourceIndex >= (int)sources.size()) return;

    ViewerState state;
    state.currentSourceIndex = sourceIndex;
    state.sourceName = sources[sourceIndex].displayName;

    std::cout << "Connecting to: " << sources[sourceIndex].name << "\n";

    NDIlib_recv_instance_t receiver = createReceiver(sources[sourceIndex].source, state.lowBandwidth);
    if (!receiver) {
        std::cerr << "Failed to create NDI receiver\n";
        return;
    }

    checkPTZSupport(receiver);

    SDL_Texture* videoTexture = nullptr;
    int texWidth = 0, texHeight = 0;
    NDIlib_FourCC_video_type_e videoFourCC = (NDIlib_FourCC_video_type_e)0;

    Uint32 lastClickTime = 0;

    // Temporary overlay messages
    std::string overlayMsg;
    std::chrono::steady_clock::time_point overlayMsgTime;

    auto showOverlayMsg = [&](const std::string& msg) {
        overlayMsg = msg;
        overlayMsgTime = std::chrono::steady_clock::now();
    };

    // Set window title
    auto updateTitle = [&]() {
        std::string title = std::string("NDI Viewer ") + VIEWER_VERSION + " - " + state.sourceName;
        if (state.lowBandwidth) title += " [LowBW]";
        SDL_SetWindowTitle(window, title.c_str());
    };
    updateTitle();

    // Switch to a different source
    auto switchSource = [&](int newIndex) {
        sources = getSourcesFromFinder(finder);
        if (sources.empty()) {
            showOverlayMsg("No sources available");
            return;
        }
        // Wrap around
        if (newIndex >= (int)sources.size()) newIndex = 0;
        if (newIndex < 0) newIndex = (int)sources.size() - 1;

        state.currentSourceIndex = newIndex;
        state.sourceName = sources[newIndex].displayName;
        state.connected = false;
        state.timecode = -1;

        // Destroy old receiver, create new
        stopPTZ(receiver);
        NDIlib_recv_destroy(receiver);
        receiver = createReceiver(sources[newIndex].source, state.lowBandwidth);
        if (receiver) {
            checkPTZSupport(receiver);
            std::cout << "Switched to: " << sources[newIndex].name << "\n";
            showOverlayMsg("Source: " + state.sourceName);
        }

        // Reset texture
        if (videoTexture) { SDL_DestroyTexture(videoTexture); videoTexture = nullptr; }
        texWidth = 0; texHeight = 0;
        state.videoWidth = 0; state.videoHeight = 0;

        updateTitle();
    };

    // Recreate receiver (for bandwidth toggle)
    auto recreateReceiver = [&]() {
        sources = getSourcesFromFinder(finder);
        if (state.currentSourceIndex >= 0 && state.currentSourceIndex < (int)sources.size()) {
            stopPTZ(receiver);
            NDIlib_recv_destroy(receiver);
            receiver = createReceiver(sources[state.currentSourceIndex].source, state.lowBandwidth);
            if (receiver) checkPTZSupport(receiver);
        }
    };

    // OSD throttle
    auto lastOSDUpdate = std::chrono::steady_clock::now();

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
                            state.fullscreen = !state.fullscreen;
                            SDL_SetWindowFullscreen(window, state.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                        }
                        lastClickTime = now;
                    }
                    break;

                case SDL_KEYDOWN:
                    // Dismiss help overlay on any key
                    if (state.showHelp && event.key.keysym.sym != SDLK_QUESTION
                        && event.key.keysym.sym != SDLK_F1
                        && event.key.keysym.sym != SDLK_SLASH) {
                        state.showHelp = false;
                        break;
                    }

                    switch (event.key.keysym.sym) {
                        case SDLK_ESCAPE:
                            if (state.showHelp) {
                                state.showHelp = false;
                            } else if (state.fullscreen) {
                                state.fullscreen = false;
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
                            state.fullscreen = !state.fullscreen;
                            SDL_SetWindowFullscreen(window, state.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                            break;

                        // New features
                        case SDLK_i:
                            state.showOSD = !state.showOSD;
                            showOverlayMsg(state.showOSD ? "OSD On" : "OSD Off");
                            break;
                        case SDLK_s:
                            state.showSafeArea = !state.showSafeArea;
                            showOverlayMsg(state.showSafeArea ? "Safe Area On" : "Safe Area Off");
                            break;
                        case SDLK_g:
                            state.showGrid = !state.showGrid;
                            showOverlayMsg(state.showGrid ? "Grid On" : "Grid Off");
                            break;
                        case SDLK_h:
                            state.flipH = !state.flipH;
                            showOverlayMsg(state.flipH ? "Flip H On" : "Flip H Off");
                            break;
                        case SDLK_v:
                            state.flipV = !state.flipV;
                            showOverlayMsg(state.flipV ? "Flip V On" : "Flip V Off");
                            break;
                        case SDLK_w:
                            state.fillMode = !state.fillMode;
                            showOverlayMsg(state.fillMode ? "Fill Mode" : "Fit Mode");
                            break;
                        case SDLK_l:
                            state.lowBandwidth = !state.lowBandwidth;
                            showOverlayMsg(state.lowBandwidth ? "Low Bandwidth" : "High Bandwidth");
                            recreateReceiver();
                            updateTitle();
                            break;
                        case SDLK_n:
                            switchSource(state.currentSourceIndex + 1);
                            break;
                        case SDLK_p:
                            switchSource(state.currentSourceIndex - 1);
                            break;
                        case SDLK_r: {
                            showOverlayMsg("Refreshing...");
                            // Destroy and recreate finder to force mDNS rescan
                            NDIlib_find_wait_for_sources(finder, 500);
                            sources = getSourcesFromFinder(finder);
                            showOverlayMsg("Refreshed: " + std::to_string(sources.size()) + " sources");
                            break;
                        }
                        case SDLK_QUESTION:
                        case SDLK_F1:
                            state.showHelp = !state.showHelp;
                            break;
                        case SDLK_SLASH:
                            // '?' is Shift+/ on most keyboards
                            if (event.key.keysym.mod & KMOD_SHIFT) {
                                state.showHelp = !state.showHelp;
                            }
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
                state.connected = true;
                state.videoWidth = videoFrame.xres;
                state.videoHeight = videoFrame.yres;
                state.frameRateN = videoFrame.frame_rate_N;
                state.frameRateD = videoFrame.frame_rate_D;
                state.timecode = videoFrame.timecode;

                if (videoFrame.xres != texWidth || videoFrame.yres != texHeight ||
                    videoFrame.FourCC != videoFourCC) {
                    texWidth = videoFrame.xres;
                    texHeight = videoFrame.yres;
                    videoFourCC = videoFrame.FourCC;

                    if (videoTexture) SDL_DestroyTexture(videoTexture);
                    videoTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                        SDL_TEXTUREACCESS_STREAMING, texWidth, texHeight);
                    std::cout << "Video: " << texWidth << "x" << texHeight
                              << " stride=" << videoFrame.line_stride_in_bytes
                              << " FourCC=0x" << std::hex << videoFrame.FourCC << std::dec << "\n";

                    if (!state.fullscreen) {
                        SDL_SetWindowSize(window, texWidth, texHeight);
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
                if (metadataFrame.p_data) {
                    parseTallyMetadata(metadataFrame.p_data, state);
                }
                NDIlib_recv_free_metadata(receiver, &metadataFrame);
                break;

            case NDIlib_frame_type_error:
                std::cerr << "NDI connection lost\n";
                state.connected = false;
                break;

            default:
                break;
        }

        // === Render ===
        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);

        // 1. Clear
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        // 2. Video frame
        SDL_Rect videoRect = {0, 0, winW, winH};
        if (videoTexture && texWidth > 0 && texHeight > 0) {
            videoRect = computeVideoRect(winW, winH, texWidth, texHeight, state.fillMode);

            // Compute flip
            SDL_RendererFlip flip = SDL_FLIP_NONE;
            if (state.flipH && state.flipV)
                flip = (SDL_RendererFlip)(SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL);
            else if (state.flipH)
                flip = SDL_FLIP_HORIZONTAL;
            else if (state.flipV)
                flip = SDL_FLIP_VERTICAL;

            SDL_RenderCopyEx(renderer, videoTexture, nullptr, &videoRect, 0.0, nullptr, flip);
        } else {
            renderTextCentered(renderer, g_font, state.connected ? "Receiving..." : "Connecting...",
                             winW / 2, winH / 2, COLOR_TEXT);
        }

        // 3. Safe area
        if (state.showSafeArea && texWidth > 0) {
            drawSafeArea(renderer, videoRect);
        }

        // 4. Grid
        if (state.showGrid && texWidth > 0) {
            drawGrid(renderer, videoRect);
        }

        // 5. Tally border
        drawTallyBorder(renderer, winW, winH, state);

        // 6. OSD
        if (state.showOSD) {
            drawOSD(renderer, g_font, g_fontSmall, state, videoRect);
        }

        // 7. PTZ indicator
        if (g_ptzSupported && g_fontSmall) {
            renderTextAt(renderer, g_fontSmall, "PTZ", 10, 10, COLOR_SUCCESS);
        }

        // 8. PTZ message overlay
        auto now = std::chrono::steady_clock::now();
        auto ptzElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_ptzMessageTime).count();
        if (ptzElapsed < 1500 && !g_ptzMessage.empty() && g_font) {
            int textW = g_ptzMessage.length() * 12 + 40;
            SDL_Rect pillRect = {winW / 2 - textW / 2, winH - 80, textW, 40};
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
            SDL_RenderFillRect(renderer, &pillRect);
            renderTextCentered(renderer, g_font, g_ptzMessage, winW / 2, winH - 70, COLOR_TEXT);
        }

        // 8b. General overlay message (source switch, mode change, etc.)
        auto overlayElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - overlayMsgTime).count();
        if (overlayElapsed < 1500 && !overlayMsg.empty() && g_font) {
            int textW = overlayMsg.length() * 10 + 40;
            SDL_Rect pillRect = {winW / 2 - textW / 2, 50, textW, 36};
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
            SDL_RenderFillRect(renderer, &pillRect);
            renderTextCentered(renderer, g_fontSmall, overlayMsg, winW / 2, 56, COLOR_TEXT);
        }

        // 9. Help overlay
        if (state.showHelp) {
            drawHelpOverlay(renderer, g_font, g_fontSmall, winW, winH);
        }

        // 10. Present
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

    // Create persistent NDI finder
    NDIlib_find_create_t findCreate = {};
    findCreate.show_local_sources = true;
    NDIlib_find_instance_t finder = NDIlib_find_create_v2(&findCreate);
    if (!finder) {
        std::cerr << "Failed to create NDI finder\n";
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        NDIlib_destroy();
        return 1;
    }

    // Check command line for source name
    std::vector<NDISourceInfo> sources;
    int selectedSource = -1;

    if (argc > 1) {
        std::string targetName = argv[1];
        // Wait for sources
        NDIlib_find_wait_for_sources(finder, 3000);
        sources = getSourcesFromFinder(finder);
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
        selectedSource = showSourcePicker(window, renderer, finder, sources);
    }

    // Run viewer
    if (selectedSource >= 0 && selectedSource < (int)sources.size() && g_running) {
        runViewer(window, renderer, finder, selectedSource);
    }

    // Cleanup
    NDIlib_find_destroy(finder);

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
