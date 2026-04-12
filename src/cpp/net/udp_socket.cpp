/**
 * FasterAPI UDP Socket - Implementation
 *
 * Cross-platform: POSIX (Linux/macOS) and Windows (Winsock2).
 * On Windows, the socket handle is stored as int for API consistency
 * but all Winsock calls use the proper SOCKET type.
 */

#include "udp_socket.h"
#include "event_loop.h"
#include <cstring>
#include <errno.h>

#ifdef _WIN32
// Windows implementation
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")

// Map WSA errors to errno for consistent error handling
static void set_errno_from_wsa() {
    int wsa_err = WSAGetLastError();
    switch (wsa_err) {
        case WSAEWOULDBLOCK:    errno = EAGAIN; break;
        case WSAEINPROGRESS:    errno = EINPROGRESS; break;
        case WSAEADDRINUSE:     errno = EADDRINUSE; break;
        case WSAEADDRNOTAVAIL:  errno = EADDRNOTAVAIL; break;
        case WSAECONNREFUSED:   errno = ECONNREFUSED; break;
        case WSAECONNRESET:     errno = ECONNRESET; break;
        case WSAENETUNREACH:    errno = ENETUNREACH; break;
        case WSAEHOSTUNREACH:   errno = EHOSTUNREACH; break;
        case WSAENOBUFS:        errno = ENOBUFS; break;
        case WSAEMSGSIZE:       errno = EMSGSIZE; break;
        case WSAEINVAL:         errno = EINVAL; break;
        case WSAENOTSOCK:       errno = ENOTSOCK; break;
        case WSAEBADF:          errno = EBADF; break;
        default:                errno = EIO; break;
    }
}

#else
// POSIX implementation
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>

// Platform-specific includes
#ifdef __APPLE__
#include <sys/types.h>
#endif

#ifdef __linux__
#include <linux/in.h>
#endif
#endif // _WIN32

namespace fasterapi {
namespace net {

// --------------------------------------------------------------------------
// Construction / destruction / move
// --------------------------------------------------------------------------

UdpSocket::UdpSocket(bool ipv6) noexcept
    : fd_(-1)
    , af_(ipv6 ? AF_INET6 : AF_INET)
{
#ifdef _WIN32
    SOCKET s = WSASocketW(af_, SOCK_DGRAM, IPPROTO_UDP,
                          nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (s != INVALID_SOCKET) {
        fd_ = static_cast<int>(s);
    }
#else
    fd_ = socket(af_, SOCK_DGRAM, 0);
#endif
}

UdpSocket UdpSocket::from_fd(int fd, int af) noexcept {
    UdpSocket socket(false);  // Create with dummy values
    socket.fd_ = fd;
    socket.af_ = af;

    // Try to determine address family from socket if not specified correctly
#ifdef _WIN32
    SOCKET s = static_cast<SOCKET>(fd);
    if (s != INVALID_SOCKET) {
        struct sockaddr_storage addr;
        int addr_len = sizeof(addr);
        if (getsockname(s, (struct sockaddr*)&addr, &addr_len) == 0) {
            socket.af_ = addr.ss_family;
        }
    }
#else
    if (fd >= 0) {
        struct sockaddr_storage addr;
        socklen_t addr_len = sizeof(addr);
        if (getsockname(fd, (struct sockaddr*)&addr, &addr_len) == 0) {
            socket.af_ = addr.ss_family;
        }
    }
#endif

    return socket;
}

UdpSocket::~UdpSocket() noexcept {
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : fd_(other.fd_)
    , af_(other.af_)
{
    other.fd_ = -1;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        af_ = other.af_;
        other.fd_ = -1;
    }
    return *this;
}

void UdpSocket::close() noexcept {
#ifdef _WIN32
    SOCKET s = static_cast<SOCKET>(fd_);
    if (s != INVALID_SOCKET) {
        ::closesocket(s);
        fd_ = static_cast<int>(INVALID_SOCKET);
    }
#else
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
}

// --------------------------------------------------------------------------
// Socket options
// --------------------------------------------------------------------------

int UdpSocket::set_nonblocking() noexcept {
#ifdef _WIN32
    u_long mode = 1;
    SOCKET s = static_cast<SOCKET>(fd_);
    if (ioctlsocket(s, FIONBIO, &mode) != 0) {
        set_errno_from_wsa();
        return -1;
    }
    return 0;
#else
    return EventLoop::set_nonblocking(fd_);
#endif
}

int UdpSocket::set_reuseaddr() noexcept {
#ifdef _WIN32
    BOOL val = TRUE;
    SOCKET s = static_cast<SOCKET>(fd_);
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&val), sizeof(val)) == SOCKET_ERROR) {
        set_errno_from_wsa();
        return -1;
    }
    return 0;
#else
    return EventLoop::set_reuseaddr(fd_);
#endif
}

int UdpSocket::set_reuseport() noexcept {
#ifdef _WIN32
    // Windows does not have SO_REUSEPORT.
    // SO_REUSEADDR on Windows for UDP behaves similarly to SO_REUSEPORT on Linux:
    // it allows multiple sockets to bind to the same port.
    return set_reuseaddr();
#else
    return EventLoop::set_reuseport(fd_);
#endif
}

int UdpSocket::set_recv_buffer_size(int size) noexcept {
#ifdef _WIN32
    SOCKET s = static_cast<SOCKET>(fd_);
    if (setsockopt(s, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&size), sizeof(size)) == SOCKET_ERROR) {
        set_errno_from_wsa();
        return -1;
    }
    return 0;
#else
    if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0) {
        return -1;
    }
    return 0;
#endif
}

int UdpSocket::set_send_buffer_size(int size) noexcept {
#ifdef _WIN32
    SOCKET s = static_cast<SOCKET>(fd_);
    if (setsockopt(s, SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char*>(&size), sizeof(size)) == SOCKET_ERROR) {
        set_errno_from_wsa();
        return -1;
    }
    return 0;
#else
    if (setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0) {
        return -1;
    }
    return 0;
#endif
}

int UdpSocket::set_recv_tos(bool enable) noexcept {
    int val = enable ? 1 : 0;

#ifdef _WIN32
    SOCKET s = static_cast<SOCKET>(fd_);
    if (af_ == AF_INET) {
        // Windows supports IP_RECVTOS on Vista+ via IP_ECN
        // Use IP_PKTINFO + SIO_SET_QOS for ECN on older Windows.
        // On Windows 10+, IP_RECVTOS is supported (value 36).
        constexpr int WIN_IP_RECVTOS = 36;
        DWORD dval = enable ? 1 : 0;
        if (setsockopt(s, IPPROTO_IP, WIN_IP_RECVTOS,
                       reinterpret_cast<const char*>(&dval), sizeof(dval)) == SOCKET_ERROR) {
            // Not critical - may not be supported on older Windows
            return 0;
        }
    } else {
        DWORD dval = enable ? 1 : 0;
        if (setsockopt(s, IPPROTO_IPV6, IPV6_RECVTCLASS,
                       reinterpret_cast<const char*>(&dval), sizeof(dval)) == SOCKET_ERROR) {
            return 0;
        }
    }
#elif defined(__linux__)
    if (af_ == AF_INET) {
        if (setsockopt(fd_, IPPROTO_IP, IP_RECVTOS, &val, sizeof(val)) < 0) {
            return -1;
        }
    } else {
        if (setsockopt(fd_, IPPROTO_IPV6, IPV6_RECVTCLASS, &val, sizeof(val)) < 0) {
            return -1;
        }
    }
#elif defined(__APPLE__)
    if (af_ == AF_INET) {
        if (setsockopt(fd_, IPPROTO_IP, IP_RECVTOS, &val, sizeof(val)) < 0) {
            return -1;
        }
    } else {
        if (setsockopt(fd_, IPPROTO_IPV6, IPV6_RECVTCLASS, &val, sizeof(val)) < 0) {
            return -1;
        }
    }
#endif

    return 0;
}

int UdpSocket::set_recv_pktinfo(bool enable) noexcept {
    int val = enable ? 1 : 0;

#ifdef _WIN32
    SOCKET s = static_cast<SOCKET>(fd_);
    DWORD dval = enable ? 1 : 0;
    if (af_ == AF_INET) {
        if (setsockopt(s, IPPROTO_IP, IP_PKTINFO,
                       reinterpret_cast<const char*>(&dval), sizeof(dval)) == SOCKET_ERROR) {
            set_errno_from_wsa();
            return -1;
        }
    } else {
        if (setsockopt(s, IPPROTO_IPV6, IPV6_PKTINFO,
                       reinterpret_cast<const char*>(&dval), sizeof(dval)) == SOCKET_ERROR) {
            set_errno_from_wsa();
            return -1;
        }
    }
#else
    if (af_ == AF_INET) {
#ifdef __linux__
        if (setsockopt(fd_, IPPROTO_IP, IP_PKTINFO, &val, sizeof(val)) < 0) {
            return -1;
        }
#elif defined(__APPLE__)
        if (setsockopt(fd_, IPPROTO_IP, IP_RECVDSTADDR, &val, sizeof(val)) < 0) {
            return -1;
        }
#endif
    } else {
#ifdef __linux__
        if (setsockopt(fd_, IPPROTO_IPV6, IPV6_RECVPKTINFO, &val, sizeof(val)) < 0) {
            return -1;
        }
#elif defined(__APPLE__)
        // macOS doesn't support IPV6_PKTINFO - not critical
        return 0;
#endif
    }
#endif

    return 0;
}

int UdpSocket::set_dont_fragment(bool enable) noexcept {
#ifdef _WIN32
    SOCKET s = static_cast<SOCKET>(fd_);
    DWORD dval = enable ? 1 : 0;
    if (af_ == AF_INET) {
        // Windows uses IP_DONTFRAGMENT
        if (setsockopt(s, IPPROTO_IP, IP_DONTFRAGMENT,
                       reinterpret_cast<const char*>(&dval), sizeof(dval)) == SOCKET_ERROR) {
            set_errno_from_wsa();
            return -1;
        }
    } else {
        // IPv6 on Windows: IPV6_DONTFRAG
        constexpr int WIN_IPV6_DONTFRAG = 14;
        if (setsockopt(s, IPPROTO_IPV6, WIN_IPV6_DONTFRAG,
                       reinterpret_cast<const char*>(&dval), sizeof(dval)) == SOCKET_ERROR) {
            // Not critical on all Windows versions
            return 0;
        }
    }
    return 0;
#else
    int val = enable ? 1 : 0;
    if (af_ == AF_INET) {
#ifdef __linux__
        val = enable ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT;
        if (setsockopt(fd_, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val)) < 0) {
            return -1;
        }
#elif defined(__APPLE__)
        if (setsockopt(fd_, IPPROTO_IP, IP_DONTFRAG, &val, sizeof(val)) < 0) {
            return -1;
        }
#endif
    } else {
#ifdef __linux__
        val = enable ? IPV6_PMTUDISC_DO : IPV6_PMTUDISC_DONT;
        if (setsockopt(fd_, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &val, sizeof(val)) < 0) {
            return -1;
        }
#elif defined(__APPLE__)
        // macOS doesn't have IPv6 DF control
        return 0;
#endif
    }
    return 0;
#endif
}

// --------------------------------------------------------------------------
// Bind
// --------------------------------------------------------------------------

int UdpSocket::bind(const std::string& host, uint16_t port) noexcept {
#ifdef _WIN32
    SOCKET s = static_cast<SOCKET>(fd_);
    if (s == INVALID_SOCKET) {
        errno = EBADF;
        return -1;
    }
#else
    if (fd_ < 0) {
        errno = EBADF;
        return -1;
    }
#endif

    if (af_ == AF_INET) {
        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        if (host == "0.0.0.0" || host.empty()) {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else {
            if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
                errno = EINVAL;
                return -1;
            }
        }

#ifdef _WIN32
        if (::bind(s, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            set_errno_from_wsa();
            return -1;
        }
#else
        if (::bind(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            return -1;
        }
#endif
    } else {
        struct sockaddr_in6 addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(port);

        if (host == "::" || host.empty()) {
            addr.sin6_addr = in6addr_any;
        } else {
            if (inet_pton(AF_INET6, host.c_str(), &addr.sin6_addr) <= 0) {
                errno = EINVAL;
                return -1;
            }
        }

#ifdef _WIN32
        if (::bind(s, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            set_errno_from_wsa();
            return -1;
        }
#else
        if (::bind(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            return -1;
        }
#endif
    }

    return 0;
}

// --------------------------------------------------------------------------
// Send / Receive (synchronous API)
// --------------------------------------------------------------------------

ssize_t UdpSocket::sendto(const void* data, size_t len, const struct sockaddr* addr,
                          socklen_t addrlen, int flags) noexcept {
#ifdef _WIN32
    SOCKET s = static_cast<SOCKET>(fd_);
    if (s == INVALID_SOCKET) {
        errno = EBADF;
        return -1;
    }
    int result = ::sendto(s, static_cast<const char*>(data),
                          static_cast<int>(len), flags, addr, addrlen);
    if (result == SOCKET_ERROR) {
        set_errno_from_wsa();
        return -1;
    }
    return static_cast<ssize_t>(result);
#else
    if (fd_ < 0) {
        errno = EBADF;
        return -1;
    }
    return ::sendto(fd_, data, len, flags, addr, addrlen);
#endif
}

ssize_t UdpSocket::sendto(const void* data, size_t len, const struct sockaddr_in* addr,
                          int flags) noexcept {
    return sendto(data, len, (const struct sockaddr*)addr, sizeof(*addr), flags);
}

ssize_t UdpSocket::sendto(const void* data, size_t len, const struct sockaddr_in6* addr,
                          int flags) noexcept {
    return sendto(data, len, (const struct sockaddr*)addr, sizeof(*addr), flags);
}

ssize_t UdpSocket::recvfrom(void* buffer, size_t len, struct sockaddr* addr,
                            socklen_t* addrlen, int flags) noexcept {
#ifdef _WIN32
    SOCKET s = static_cast<SOCKET>(fd_);
    if (s == INVALID_SOCKET) {
        errno = EBADF;
        return -1;
    }
    int al = addrlen ? static_cast<int>(*addrlen) : 0;
    int result = ::recvfrom(s, static_cast<char*>(buffer),
                            static_cast<int>(len), flags, addr, &al);
    if (addrlen) *addrlen = static_cast<socklen_t>(al);
    if (result == SOCKET_ERROR) {
        set_errno_from_wsa();
        return -1;
    }
    return static_cast<ssize_t>(result);
#else
    if (fd_ < 0) {
        errno = EBADF;
        return -1;
    }
    return ::recvfrom(fd_, buffer, len, flags, addr, addrlen);
#endif
}

ssize_t UdpSocket::recvfrom(void* buffer, size_t len, struct sockaddr_in* addr,
                            int flags) noexcept {
    socklen_t addrlen = sizeof(*addr);
    return recvfrom(buffer, len, (struct sockaddr*)addr, &addrlen, flags);
}

ssize_t UdpSocket::recvfrom(void* buffer, size_t len, struct sockaddr_in6* addr,
                            int flags) noexcept {
    socklen_t addrlen = sizeof(*addr);
    return recvfrom(buffer, len, (struct sockaddr*)addr, &addrlen, flags);
}

// --------------------------------------------------------------------------
// Utility
// --------------------------------------------------------------------------

bool UdpSocket::get_local_address(std::string& ip, uint16_t& port) const noexcept {
#ifdef _WIN32
    SOCKET s = static_cast<SOCKET>(fd_);
    if (s == INVALID_SOCKET) {
        return false;
    }
#else
    if (fd_ < 0) {
        return false;
    }
#endif

    if (af_ == AF_INET) {
        struct sockaddr_in addr;
#ifdef _WIN32
        int addr_len = sizeof(addr);
        if (getsockname(s, (struct sockaddr*)&addr, &addr_len) == SOCKET_ERROR) {
            return false;
        }
#else
        socklen_t addr_len = sizeof(addr);
        if (getsockname(fd_, (struct sockaddr*)&addr, &addr_len) < 0) {
            return false;
        }
#endif

        char ip_str[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str)) == nullptr) {
            return false;
        }

        ip = ip_str;
        port = ntohs(addr.sin_port);
    } else {
        struct sockaddr_in6 addr;
#ifdef _WIN32
        int addr_len = sizeof(addr);
        if (getsockname(s, (struct sockaddr*)&addr, &addr_len) == SOCKET_ERROR) {
            return false;
        }
#else
        socklen_t addr_len = sizeof(addr);
        if (getsockname(fd_, (struct sockaddr*)&addr, &addr_len) < 0) {
            return false;
        }
#endif

        char ip_str[INET6_ADDRSTRLEN];
        if (inet_ntop(AF_INET6, &addr.sin6_addr, ip_str, sizeof(ip_str)) == nullptr) {
            return false;
        }

        ip = ip_str;
        port = ntohs(addr.sin6_port);
    }

    return true;
}

int UdpSocket::release() noexcept {
    int fd = fd_;
    fd_ = -1;
    return fd;
}

} // namespace net
} // namespace fasterapi
