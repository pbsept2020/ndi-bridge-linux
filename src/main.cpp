/**
 * NDI Bridge Linux - Main Entry Point
 *
 * CLI interface for NDI Bridge with three modes:
 *   - discover: Find NDI sources on the network
 *   - host:     Capture NDI, encode, and stream over UDP
 *   - join:     Receive UDP stream, decode, and output as NDI
 *
 * Usage:
 *   ndi-bridge discover
 *   ndi-bridge host --auto [--target IP:PORT] [--bitrate MBPS]
 *   ndi-bridge join --name "Source Name" [--port PORT] [--buffer MS]
 */

#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <csignal>
#include <atomic>

#include "common/Logger.h"
#include "common/Protocol.h"
#include "host/HostMode.h"
#include "join/JoinMode.h"

// NDI SDK
#include <Processing.NDI.Lib.h>

using namespace ndi_bridge;

// Global flag for graceful shutdown
std::atomic<bool> g_running{true};

void signalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        LOG_INFO("Shutdown requested...");
        g_running = false;
    }
}

// Command-line argument parser
struct Config {
    enum class Mode { None, Discover, Host, Join };

    Mode mode = Mode::None;

    // Host mode options
    std::string source;         // NDI source name (empty = interactive)
    bool autoSelect = false;    // Auto-select first source
    std::string targetHost = "127.0.0.1";
    uint16_t targetPort = 5990;
    int bitrate = 8;            // Mbps

    // Join mode options
    std::string outputName = "NDI Bridge";
    uint16_t listenPort = 5990;
    int bufferMs = 0;           // Buffer delay in ms

    // Global options
    bool verbose = false;
    bool help = false;
};

void printUsage(const char* programName) {
    std::cout << "\n"
        "NDI Bridge Linux - Stream NDI over WAN\n"
        "\n"
        "Usage:\n"
        "  " << programName << " <mode> [options]\n"
        "\n"
        "Modes:\n"
        "  discover              Discover NDI sources on the network\n"
        "  host                  Capture NDI source and stream over UDP\n"
        "  join                  Receive UDP stream and output as NDI\n"
        "\n"
        "Host mode options:\n"
        "  --source <name>       NDI source name to capture\n"
        "  --auto                Auto-select first available source\n"
        "  --target <ip:port>    Target address (default: 127.0.0.1:5990)\n"
        "  --bitrate <mbps>      Video bitrate in Mbps (default: 8)\n"
        "\n"
        "Join mode options:\n"
        "  --name <name>         NDI output source name (default: NDI Bridge)\n"
        "  --port <port>         UDP listen port (default: 5990)\n"
        "  --buffer <ms>         Playback buffer delay in ms (default: 0)\n"
        "\n"
        "Global options:\n"
        "  -v, --verbose         Enable debug output\n"
        "  -h, --help            Show this help message\n"
        "\n"
        "Examples:\n"
        "  " << programName << " discover\n"
        "  " << programName << " host --auto\n"
        "  " << programName << " host --source 'OBS (Camera)' --target 192.168.1.100:5990\n"
        "  " << programName << " join --name 'Remote Camera' --port 5990\n"
        "\n";
}

Config parseArgs(int argc, char* argv[]) {
    Config config;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        // Modes
        if (arg == "discover") {
            config.mode = Config::Mode::Discover;
        } else if (arg == "host") {
            config.mode = Config::Mode::Host;
        } else if (arg == "join") {
            config.mode = Config::Mode::Join;
        }
        // Host options
        else if (arg == "--source" && i + 1 < argc) {
            config.source = argv[++i];
        } else if (arg == "--auto") {
            config.autoSelect = true;
        } else if (arg == "--target" && i + 1 < argc) {
            std::string target = argv[++i];
            size_t colonPos = target.rfind(':');
            if (colonPos != std::string::npos) {
                config.targetHost = target.substr(0, colonPos);
                config.targetPort = static_cast<uint16_t>(std::stoi(target.substr(colonPos + 1)));
            } else {
                config.targetHost = target;
            }
        } else if (arg == "--bitrate" && i + 1 < argc) {
            config.bitrate = std::stoi(argv[++i]);
        }
        // Join options
        else if (arg == "--name" && i + 1 < argc) {
            config.outputName = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            config.listenPort = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--buffer" && i + 1 < argc) {
            config.bufferMs = std::stoi(argv[++i]);
        }
        // Global options
        else if (arg == "-v" || arg == "--verbose") {
            config.verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            config.help = true;
        }
    }

    return config;
}

// Initialize NDI library
bool initNDI() {
    if (!NDIlib_initialize()) {
        LOG_ERROR("Failed to initialize NDI library");
        LOG_ERROR("Make sure NDI SDK is installed and LD_LIBRARY_PATH is set");
        return false;
    }
    LOG_SUCCESS("NDI library initialized");
    return true;
}

// Discover mode: list NDI sources
int runDiscover() {
    LOG_INFO("Discovering NDI sources...");

    // Create finder
    NDIlib_find_create_t findCreate;
    findCreate.show_local_sources = true;
    findCreate.p_groups = nullptr;
    findCreate.p_extra_ips = nullptr;

    NDIlib_find_instance_t finder = NDIlib_find_create_v2(&findCreate);
    if (!finder) {
        LOG_ERROR("Failed to create NDI finder");
        return 1;
    }

    LOG_INFO("Waiting for sources (5 seconds)...");

    // Wait and collect sources
    uint32_t numSources = 0;
    const NDIlib_source_t* sources = nullptr;

    for (int i = 0; i < 50 && g_running; i++) {
        NDIlib_find_wait_for_sources(finder, 100);
        sources = NDIlib_find_get_current_sources(finder, &numSources);
    }

    if (numSources == 0) {
        LOG_INFO("No NDI sources found");
    } else {
        Logger::instance().successf("Found %u NDI source(s):", numSources);
        for (uint32_t i = 0; i < numSources; i++) {
            std::cout << "  [" << (i + 1) << "] " << sources[i].p_ndi_name << "\n";
            if (Logger::instance().isVerbose()) {
                std::cout << "      URL: " << (sources[i].p_url_address ? sources[i].p_url_address : "N/A") << "\n";
            }
        }
    }

    NDIlib_find_destroy(finder);
    return 0;
}

// Host mode: capture NDI and stream
int runHost(const Config& config) {
    // Configure host mode
    HostModeConfig hostConfig;
    hostConfig.targetHost = config.targetHost;
    hostConfig.targetPort = config.targetPort;
    hostConfig.bitrateMbps = config.bitrate;
    hostConfig.autoSelectFirstSource = config.autoSelect;
    hostConfig.sourceName = config.source;

    // Create and start host mode
    HostMode host(hostConfig);
    return host.start(g_running);
}

// Join mode: receive stream and output NDI
int runJoin(const Config& config) {
    // Configure join mode
    JoinModeConfig joinConfig;
    joinConfig.listenPort = config.listenPort;
    joinConfig.ndiOutputName = config.outputName;
    joinConfig.bufferMs = config.bufferMs;

    // Create and start join mode
    JoinMode join(joinConfig);
    return join.start(g_running);
}

int main(int argc, char* argv[]) {
    // Setup signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Parse arguments
    Config config = parseArgs(argc, argv);

    // Configure logger
    Logger::instance().setVerbose(config.verbose);

    // Show help
    if (config.help || config.mode == Config::Mode::None) {
        printUsage(argv[0]);
        return config.help ? 0 : 1;
    }

    // Initialize NDI
    if (!initNDI()) {
        return 1;
    }

    // Run selected mode
    int result = 0;
    switch (config.mode) {
        case Config::Mode::Discover:
            result = runDiscover();
            break;
        case Config::Mode::Host:
            result = runHost(config);
            break;
        case Config::Mode::Join:
            result = runJoin(config);
            break;
        default:
            LOG_ERROR("Invalid mode");
            result = 1;
    }

    // Cleanup
    NDIlib_destroy();
    LOG_SUCCESS("NDI Bridge shutdown complete");

    return result;
}
