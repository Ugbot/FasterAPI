/**
 * FasterAPI UDP Socket - High-level UDP abstraction
 *
 * Features:
 * - RAII wrapper around raw sockets
 * - Non-blocking I/O
 * - IPv4 and IPv6 support
 * - SO_REUSEPORT for multi-core scaling
 * - Integration with EventLoop
 */

#pragma once

#include <cstdint>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>

namespace fasterapi {
namespace net {

/**
 * UDP Socket abstraction
 *
 * Usage example:
 *
 *   UdpSocket socket;
 *   socket.bind("0.0.0.0", 443);
 *   socket.set_nonblocking();
 *   socket.set_reuseport();
 *
 *   uint8_t buffer[2048];
 *   struct sockaddr_in addr;
 *   ssize_t n = socket.recvfrom(buffer, sizeof(buffer), &addr);
 *
 *   socket.sendto(data, len, &addr);
 */
class UdpSocket {
public:
    /**
     * Create a new UDP socket (IPv4 by default)
     * @param ipv6 If true, create IPv6 socket, otherwise IPv4
     */
    explicit UdpSocket(bool ipv6 = false) noexcept;

    /**
     * Create a UDP socket from an existing file descriptor
     * Takes ownership of the fd.
     * @param fd File descriptor
     * @param af Address family (AF_INET or AF_INET6)
     */
    static UdpSocket from_fd(int fd, int af = AF_INET) noexcept;

    /**
     * Destructor closes the socket
     */
    ~UdpSocket() noexcept;

    // Non-copyable, movable
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    /**
     * Get the file descriptor
     */
    int fd() const noexcept { return fd_; }

    /**
     * Check if socket is valid
     */
    bool is_valid() const noexcept { return fd_ >= 0; }

    /**
     * Close the socket
     */
    void close() noexcept;

    /**
     * Set socket to non-blocking mode
     * @return 0 on success, -1 on error (check errno)
     */
    int set_nonblocking() noexcept;

    /**
     * Set SO_REUSEADDR
     * @return 0 on success, -1 on error
     */
    int set_reuseaddr() noexcept;

    /**
     * Set SO_REUSEPORT (for multi-core scaling)
     * @return 0 on success, -1 on error
     */
    int set_reuseport() noexcept;

    /**
     * Set receive buffer size
     * @param size Buffer size in bytes
     * @return 0 on success, -1 on error
     */
    int set_recv_buffer_size(int size) noexcept;

    /**
     * Set send buffer size
     * @param size Buffer size in bytes
     * @return 0 on success, -1 on error
     */
    int set_send_buffer_size(int size) noexcept;

    /**
     * Enable/disable IP_RECVTOS (receive Type of Service)
     * Used for QUIC ECN support
     * @return 0 on success, -1 on error
     */
    int set_recv_tos(bool enable = true) noexcept;

    /**
     * Enable/disable IP_PKTINFO (receive packet info)
     * Used for getting destination address
     * @return 0 on success, -1 on error
     */
    int set_recv_pktinfo(bool enable = true) noexcept;

    /**
     * Set IP Don't Fragment flag
     * Used for QUIC PMTU discovery
     * @return 0 on success, -1 on error
     */
    int set_dont_fragment(bool enable = true) noexcept;

    /**
     * Bind to local address
     * @param host Local IP address (or "0.0.0.0" for any)
     * @param port Local port
     * @return 0 on success, -1 on error
     */
    int bind(const std::string& host, uint16_t port) noexcept;

    /**
     * Send datagram to specified address
     * @param data Pointer to data
     * @param len Length of data
     * @param addr Destination address
     * @param flags Send flags (default 0)
     * @return Number of bytes sent, or -1 on error
     */
    ssize_t sendto(const void* data, size_t len, const struct sockaddr* addr,
                   socklen_t addrlen, int flags = 0) noexcept;

    /**
     * Send datagram to IPv4 address (convenience method)
     */
    ssize_t sendto(const void* data, size_t len, const struct sockaddr_in* addr,
                   int flags = 0) noexcept;

    /**
     * Send datagram to IPv6 address (convenience method)
     */
    ssize_t sendto(const void* data, size_t len, const struct sockaddr_in6* addr,
                   int flags = 0) noexcept;

    /**
     * Receive datagram
     * @param buffer Buffer to receive into
     * @param len Maximum bytes to receive
     * @param addr Will be filled with source address
     * @param addrlen Pointer to address length (in/out)
     * @param flags Receive flags (default 0)
     * @return Number of bytes received, or -1 on error
     */
    ssize_t recvfrom(void* buffer, size_t len, struct sockaddr* addr,
                     socklen_t* addrlen, int flags = 0) noexcept;

    /**
     * Receive datagram from IPv4 address (convenience method)
     */
    ssize_t recvfrom(void* buffer, size_t len, struct sockaddr_in* addr,
                     int flags = 0) noexcept;

    /**
     * Receive datagram from IPv6 address (convenience method)
     */
    ssize_t recvfrom(void* buffer, size_t len, struct sockaddr_in6* addr,
                     int flags = 0) noexcept;

    /**
     * Get local address
     */
    bool get_local_address(std::string& ip, uint16_t& port) const noexcept;

    /**
     * Release ownership of the file descriptor
     * Returns the fd and sets internal fd to -1
     */
    int release() noexcept;

    /**
     * Get address family
     */
    int address_family() const noexcept { return af_; }

private:
    int fd_;
    int af_;  // AF_INET or AF_INET6
};

} // namespace net
} // namespace fasterapi
