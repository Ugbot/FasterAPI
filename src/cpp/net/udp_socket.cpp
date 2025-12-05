/**
 * FasterAPI UDP Socket - Implementation
 */

#include "udp_socket.h"
#include "event_loop.h"
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <cstring>
#include <errno.h>

// Platform-specific includes
#ifdef __APPLE__
#include <sys/types.h>
#endif

#ifdef __linux__
#include <linux/in.h>
#endif

namespace fasterapi {
namespace net {

UdpSocket::UdpSocket(bool ipv6) noexcept
    : fd_(-1)
    , af_(ipv6 ? AF_INET6 : AF_INET)
{
    fd_ = socket(af_, SOCK_DGRAM, 0);
}

UdpSocket UdpSocket::from_fd(int fd, int af) noexcept {
    UdpSocket socket(false);  // Create with dummy values
    socket.fd_ = fd;
    socket.af_ = af;

    // Try to determine address family from socket if not specified correctly
    if (fd >= 0) {
        struct sockaddr_storage addr;
        socklen_t addr_len = sizeof(addr);
        if (getsockname(fd, (struct sockaddr*)&addr, &addr_len) == 0) {
            socket.af_ = addr.ss_family;
        }
    }

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
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

int UdpSocket::set_nonblocking() noexcept {
    return EventLoop::set_nonblocking(fd_);
}

int UdpSocket::set_reuseaddr() noexcept {
    return EventLoop::set_reuseaddr(fd_);
}

int UdpSocket::set_reuseport() noexcept {
    return EventLoop::set_reuseport(fd_);
}

int UdpSocket::set_recv_buffer_size(int size) noexcept {
    if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0) {
        return -1;
    }
    return 0;
}

int UdpSocket::set_send_buffer_size(int size) noexcept {
    if (setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0) {
        return -1;
    }
    return 0;
}

int UdpSocket::set_recv_tos(bool enable) noexcept {
    int val = enable ? 1 : 0;

#ifdef __linux__
    // Linux uses IP_RECVTOS for IPv4
    if (af_ == AF_INET) {
        if (setsockopt(fd_, IPPROTO_IP, IP_RECVTOS, &val, sizeof(val)) < 0) {
            return -1;
        }
    } else {
        // IPv6 uses IPV6_RECVTCLASS
        if (setsockopt(fd_, IPPROTO_IPV6, IPV6_RECVTCLASS, &val, sizeof(val)) < 0) {
            return -1;
        }
    }
#elif defined(__APPLE__)
    // macOS uses IP_RECVTOS for IPv4
    if (af_ == AF_INET) {
        if (setsockopt(fd_, IPPROTO_IP, IP_RECVTOS, &val, sizeof(val)) < 0) {
            return -1;
        }
    } else {
        // IPv6 uses IPV6_RECVTCLASS
        if (setsockopt(fd_, IPPROTO_IPV6, IPV6_RECVTCLASS, &val, sizeof(val)) < 0) {
            return -1;
        }
    }
#endif

    return 0;
}

int UdpSocket::set_recv_pktinfo(bool enable) noexcept {
    int val = enable ? 1 : 0;

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
        // IPv6 uses IPV6_RECVPKTINFO on Linux, not available on macOS
#ifdef __linux__
        if (setsockopt(fd_, IPPROTO_IPV6, IPV6_RECVPKTINFO, &val, sizeof(val)) < 0) {
            return -1;
        }
#elif defined(__APPLE__)
        // macOS doesn't support IPV6_PKTINFO - not critical
        return 0;
#endif
    }

    return 0;
}

int UdpSocket::set_dont_fragment(bool enable) noexcept {
    int val = enable ? 1 : 0;

    if (af_ == AF_INET) {
#ifdef __linux__
        // Linux uses IP_MTU_DISCOVER
        val = enable ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT;
        if (setsockopt(fd_, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val)) < 0) {
            return -1;
        }
#elif defined(__APPLE__)
        // macOS uses IP_DONTFRAG
        if (setsockopt(fd_, IPPROTO_IP, IP_DONTFRAG, &val, sizeof(val)) < 0) {
            return -1;
        }
#endif
    } else {
        // IPv6: Linux uses IPV6_MTU_DISCOVER, macOS doesn't support this
#ifdef __linux__
        val = enable ? IPV6_PMTUDISC_DO : IPV6_PMTUDISC_DONT;
        if (setsockopt(fd_, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &val, sizeof(val)) < 0) {
            return -1;
        }
#elif defined(__APPLE__)
        // macOS doesn't have IPv6 DF control - not critical for basic operation
        return 0;
#endif
    }

    return 0;
}

int UdpSocket::bind(const std::string& host, uint16_t port) noexcept {
    if (fd_ < 0) {
        errno = EBADF;
        return -1;
    }

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

        if (::bind(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            return -1;
        }
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

        if (::bind(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            return -1;
        }
    }

    return 0;
}

ssize_t UdpSocket::sendto(const void* data, size_t len, const struct sockaddr* addr,
                          socklen_t addrlen, int flags) noexcept {
    if (fd_ < 0) {
        errno = EBADF;
        return -1;
    }

    return ::sendto(fd_, data, len, flags, addr, addrlen);
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
    if (fd_ < 0) {
        errno = EBADF;
        return -1;
    }

    return ::recvfrom(fd_, buffer, len, flags, addr, addrlen);
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

bool UdpSocket::get_local_address(std::string& ip, uint16_t& port) const noexcept {
    if (fd_ < 0) {
        return false;
    }

    if (af_ == AF_INET) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);

        if (getsockname(fd_, (struct sockaddr*)&addr, &addr_len) < 0) {
            return false;
        }

        char ip_str[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str)) == nullptr) {
            return false;
        }

        ip = ip_str;
        port = ntohs(addr.sin_port);
    } else {
        struct sockaddr_in6 addr;
        socklen_t addr_len = sizeof(addr);

        if (getsockname(fd_, (struct sockaddr*)&addr, &addr_len) < 0) {
            return false;
        }

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
