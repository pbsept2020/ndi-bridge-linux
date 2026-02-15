#pragma once

/**
 * Platform.h - Cross-platform abstractions for NDI Bridge
 *
 * Provides unified API for:
 *   - Socket types and operations (POSIX vs WinSock)
 *   - Byte-swap (GCC/Clang __builtin vs MSVC _byteswap)
 *   - Wall clock time (clock_gettime vs GetSystemTimePreciseAsFileTime)
 *   - WinSock initialization (WSAStartup/WSACleanup)
 */

#ifdef _WIN32
    // Windows — NOMINMAX must come before any Windows header to prevent
    // min/max macros from clashing with std::min/std::max
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")

    // Socket type abstraction
    using socket_t = SOCKET;
    constexpr socket_t INVALID_SOCKET_VAL = INVALID_SOCKET;

    inline int platform_close_socket(socket_t s) { return closesocket(s); }
    inline int platform_poll(WSAPOLLFD* fds, unsigned long nfds, int timeout) {
        return WSAPoll(fds, nfds, timeout);
    }

    // Error handling
    inline int platform_socket_errno() { return WSAGetLastError(); }
    inline const char* platform_socket_strerror(int err) {
        // Thread-local buffer for FormatMessage
        thread_local char buf[256];
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, err, 0, buf, sizeof(buf), nullptr);
        // Strip trailing \r\n
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\r' || buf[len-1] == '\n')) buf[--len] = '\0';
        return buf;
    }

    #define PLATFORM_EAGAIN       WSAEWOULDBLOCK
    #define PLATFORM_EWOULDBLOCK  WSAEWOULDBLOCK
    #define PLATFORM_EINTR        WSAEINTR
    #define PLATFORM_MSG_DONTWAIT 0  // Not needed on Windows non-blocking sockets

    // Non-blocking mode
    inline int platform_set_nonblocking(socket_t s) {
        u_long mode = 1;
        return ioctlsocket(s, FIONBIO, &mode);
    }

    // SO_REUSEPORT doesn't exist on Windows
    #define PLATFORM_HAS_REUSEPORT 0

#else
    // POSIX (macOS, Linux)
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <poll.h>
    #include <cerrno>
    #include <cstring>
    #include <ctime>

    // Socket type abstraction
    using socket_t = int;
    constexpr socket_t INVALID_SOCKET_VAL = -1;

    inline int platform_close_socket(socket_t s) { return close(s); }
    inline int platform_poll(struct pollfd* fds, nfds_t nfds, int timeout) {
        return poll(fds, nfds, timeout);
    }

    // Error handling
    inline int platform_socket_errno() { return errno; }
    inline const char* platform_socket_strerror(int err) { return strerror(err); }

    #define PLATFORM_EAGAIN       EAGAIN
    #define PLATFORM_EWOULDBLOCK  EWOULDBLOCK
    #define PLATFORM_EINTR        EINTR
    #define PLATFORM_MSG_DONTWAIT MSG_DONTWAIT

    // Non-blocking mode
    inline int platform_set_nonblocking(socket_t s) {
        int flags = fcntl(s, F_GETFL, 0);
        if (flags < 0) return -1;
        return fcntl(s, F_SETFL, flags | O_NONBLOCK);
    }

    #define PLATFORM_HAS_REUSEPORT 1

#endif

// ============================================================================
// Byte-swap utilities (MSVC vs GCC/Clang)
// ============================================================================

namespace ndi_bridge {
namespace platform {

#ifdef _MSC_VER
    // MSVC intrinsics
    #include <stdlib.h>
    inline uint16_t bswap16(uint16_t v) { return _byteswap_ushort(v); }
    inline uint32_t bswap32(uint32_t v) { return _byteswap_ulong(v); }
    inline uint64_t bswap64(uint64_t v) { return _byteswap_uint64(v); }
#else
    // GCC/Clang builtins
    inline uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }
    inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
    inline uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }
#endif

} // namespace platform

// ============================================================================
// Wall clock time (nanoseconds since epoch)
// ============================================================================

namespace platform {

inline uint64_t wallClockNs() {
#ifdef _WIN32
    // GetSystemTimePreciseAsFileTime: 100ns ticks since 1601-01-01
    // Subtract epoch offset to get Unix epoch (1970-01-01)
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t ticks = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    // Windows epoch offset: 11644473600 seconds = 116444736000000000 * 100ns
    constexpr uint64_t EPOCH_OFFSET = 116444736000000000ULL;
    return (ticks - EPOCH_OFFSET) * 100; // 100ns ticks → nanoseconds
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
#endif
}

} // namespace platform

// ============================================================================
// WinSock initialization (RAII)
// ============================================================================

#ifdef _WIN32

class WinSockInit {
public:
    static bool initialize() {
        if (initialized_) return true;
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) return false;
        initialized_ = true;
        return true;
    }

    static void cleanup() {
        if (initialized_) {
            WSACleanup();
            initialized_ = false;
        }
    }

private:
    static inline bool initialized_ = false;
};

#endif

} // namespace ndi_bridge
