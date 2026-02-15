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

#ifdef _WIN32
#include "common/Platform.h"
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "common/Logger.h"
#include "common/Protocol.h"
#include "host/HostMode.h"
#include "join/JoinMode.h"
#include "web/BridgeManager.h"
#include "web/BridgeWebControl.h"

// NDI SDK
#include <Processing.NDI.Lib.h>

using namespace ndi_bridge;

#include "common/Version.h"
static const char* BUILD_VERSION = NDI_BRIDGE_VERSION;
static const char* BUILD_DATE = __DATE__ " " __TIME__;

// Global flag for graceful shutdown
std::atomic<bool> g_running{true};

void signalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        LOG_INFO("Shutdown requested...");
        g_running = false;
    }
}

#ifdef _WIN32
BOOL WINAPI consoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        LOG_INFO("Shutdown requested...");
        g_running = false;
        return TRUE;
    }
    return FALSE;
}
#endif

// Command-line argument parser
struct Config {
    enum class Mode { None, Discover, Host, Join, WebUI };

    Mode mode = Mode::None;

    // Host mode options
    std::string source;         // NDI source name (empty = interactive)
    bool autoSelect = false;    // Auto-select first source
    std::string targetHost = "127.0.0.1";
    uint16_t targetPort = 5990;
    int bitrate = 8;            // Mbps
    size_t mtu = 1400;          // UDP MTU

    // Join mode options
    std::string outputName = "NDI Bridge";
    uint16_t listenPort = 5990;
    int bufferMs = 0;           // Buffer delay in ms

    // Web UI options
    int webPort = 8080;

    // Global options
    bool verbose = false;
    bool help = false;
    bool clean = false;        // Kill orphan processes before starting
};

void printUsage(const char* programName) {
    std::cout << "\n"
        "NDI Bridge X - Cross-platform NDI over WAN\n"
        "\n"
        "Usage:\n"
        "  " << programName << " <mode> [options]\n"
        "\n"
        "Modes:\n"
        "  discover              Discover NDI sources on the network\n"
        "  host                  Capture NDI source and stream over UDP\n"
        "  join                  Receive UDP stream and output as NDI\n"
        "  --web-ui              Launch web control interface\n"
        "\n"
        "Host mode options:\n"
        "  --source <name>       NDI source name to capture\n"
        "  --auto                Auto-select first available source\n"
        "  --target <ip:port>    Target address (default: 127.0.0.1:5990)\n"
        "  --bitrate <mbps>      Video bitrate in Mbps (default: 8)\n"
        "  --mtu <bytes>         UDP MTU size (default: 1400, use 1200 for VPN)\n"
        "\n"
        "Join mode options:\n"
        "  --name <name>         NDI output source name (default: NDI Bridge)\n"
        "  --port <port>         UDP listen port (default: 5990)\n"
        "  --buffer <ms>         Playback buffer delay in ms (default: 0)\n"
        "\n"
        "Web UI options:\n"
        "  --web-port <port>     HTTP port for web UI (default: 8080)\n"
        "  --clean               Kill orphan ndi-bridge-x processes before starting\n"
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
        "  " << programName << " --web-ui\n"
        "  " << programName << " --web-ui --web-port 9090\n"
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
        } else if (arg == "--web-ui") {
            config.mode = Config::Mode::WebUI;
        } else if (arg == "--web-port" && i + 1 < argc) {
            config.webPort = std::stoi(argv[++i]);
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
        } else if (arg == "--mtu" && i + 1 < argc) {
            config.mtu = static_cast<size_t>(std::stoi(argv[++i]));
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
        else if (arg == "--clean") {
            config.clean = true;
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
    hostConfig.mtu = config.mtu;
    hostConfig.autoSelectFirstSource = config.autoSelect;
    hostConfig.sourceName = config.source;

    // Create and start host mode
    HostMode host(hostConfig);
    return host.start(g_running);
}

// Kill orphan ndi-bridge-x processes (same binary, different PID)
void cleanOrphans() {
    auto& log = Logger::instance();
#ifdef _WIN32
    DWORD myPid = GetCurrentProcessId();
    log.infof("Cleaning orphan ndi-bridge-x processes (my PID: %lu)...", myPid);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "powershell -Command \"Get-Process ndi-bridge-x -ErrorAction SilentlyContinue "
        "| Where-Object { $_.Id -ne %lu } "
        "| ForEach-Object { Write-Host \\\"  killed PID $($_.Id)\\\"; Stop-Process -Force $_ }\" 2>nul",
        myPid);
    system(cmd);
#else
    pid_t myPid = getpid();
    log.infof("Cleaning orphan ndi-bridge-x processes (my PID: %d)...", (int)myPid);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "for pid in $(pgrep -f ndi-bridge-x 2>/dev/null); do "
        "[ \"$pid\" != \"%d\" ] && kill -9 \"$pid\" 2>/dev/null && echo \"  killed PID $pid\"; "
        "done", (int)myPid);
    system(cmd);
#endif
    log.success("Orphan cleanup done");
}

// Web UI mode: embedded HTTP server with multi-pipeline control
int runWebUI(const Config& config) {
    using namespace ndi_bridge;

    auto& log = Logger::instance();

    // Detect if we need sudo (macOS non-root)
    bool needsSudo = false;
#ifdef __APPLE__
    if (getuid() != 0) {
        needsSudo = true;
        LOG_INFO("[WARNING] Running without root privileges on macOS");
        LOG_INFO("[WARNING] Join via en7/SpeedFusion requires sudo. Relaunch with:");
        LOG_INFO("[WARNING]   sudo ./build-mac/ndi-bridge-x --web-ui");
    }
#endif

    log.info("Starting Web UI mode...");

    BridgeManager manager;
    BridgeWebControl web(&manager, config.webPort, needsSudo);

    if (!web.start()) {
        LOG_ERROR("Failed to start web server");
        return 1;
    }

    log.successf("Web UI available at %s", web.url().c_str());
    log.info("Press Ctrl+C to stop...");

    // Auto-open browser
#ifdef __APPLE__
    std::string openCmd = "open " + web.url();
    system(openCmd.c_str());
#elif defined(_WIN32)
    std::string openCmd = "start \"\" \"" + web.url() + "\"";
    system(openCmd.c_str());
#endif

    // Main loop
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Cleanup
    log.info("Shutting down web UI...");
    manager.stopAll();
    web.stop();

    return 0;
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
#ifndef _WIN32
    std::signal(SIGTERM, signalHandler);
#else
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
    // Initialize WinSock
    if (!ndi_bridge::WinSockInit::initialize()) {
        LOG_ERROR("Failed to initialize WinSock");
        return 1;
    }
#endif

    // Parse arguments
    Config config = parseArgs(argc, argv);

    // Configure logger
    Logger::instance().setVerbose(config.verbose);

    // Show help
    if (config.help || config.mode == Config::Mode::None) {
        printUsage(argv[0]);
        return config.help ? 0 : 1;
    }

    // Show version
    Logger::instance().successf("NDI Bridge %s (built %s)", BUILD_VERSION, BUILD_DATE);

    // Clean orphan processes if requested
    if (config.clean) {
        cleanOrphans();
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
        case Config::Mode::WebUI:
            result = runWebUI(config);
            break;
        default:
            LOG_ERROR("Invalid mode");
            result = 1;
    }

    // Cleanup
    NDIlib_destroy();
#ifdef _WIN32
    ndi_bridge::WinSockInit::cleanup();
#endif
    LOG_SUCCESS("NDI Bridge shutdown complete");

    return result;
}
