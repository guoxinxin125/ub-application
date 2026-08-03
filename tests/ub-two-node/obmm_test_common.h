#pragma once

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

extern "C" {
#include <libobmm.h>
}

namespace obmm_test {

constexpr uint32_t protocol_magic = 0x4f424d4d; // "OBMM"
constexpr uint32_t protocol_version = 1;

enum class Control : uint32_t {
    importer_ready = 1,
    exporter_written = 2,
    importer_read_ok = 3,
    importer_written = 4,
    exporter_read_ok = 5,
    importer_cleaned = 6,
};

struct ExportDescription {
    uint64_t addr = 0;
    uint64_t length = 0;
    uint32_t tokenid = 0;
    uint32_t dcna = 0;
    uint8_t deid[16]{};
};

struct NcMapping {
    int fd = -1;
    void *address = MAP_FAILED;
    size_t length = 0;
};

[[noreturn]] inline void fail(const std::string &message)
{
    throw std::runtime_error(message);
}

inline std::string errno_message(const std::string &operation)
{
    return operation + ": " + std::strerror(errno);
}

inline uint64_t parse_u64(const char *text, const char *name)
{
    errno = 0;
    char *end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0')
        fail(std::string("invalid value for ") + name + ": " + text);
    return static_cast<uint64_t>(value);
}

inline uint32_t parse_u32(const char *text, const char *name)
{
    const uint64_t value = parse_u64(text, name);
    if (value > std::numeric_limits<uint32_t>::max())
        fail(std::string(name) + " exceeds 32 bits");
    return static_cast<uint32_t>(value);
}

inline uint16_t parse_port(const char *text)
{
    const uint64_t value = parse_u64(text, "--port");
    if (value == 0 || value > 65535)
        fail("--port must be in [1, 65535]");
    return static_cast<uint16_t>(value);
}

inline uint64_t mib_to_bytes(uint64_t size_mib)
{
    constexpr uint64_t mib = 1024 * 1024;
    if (size_mib == 0 || size_mib > std::numeric_limits<uint64_t>::max() / mib)
        fail("invalid or overflowing --region-mb");
    return size_mib * mib;
}

inline void encode_local_eid(uint8_t destination[16], uint32_t eid)
{
    std::memset(destination, 0, 16);
    // The vendor adaptor in this OBMM tree reads the sysfs EID as a host-endian
    // 32-bit integer and stores it in the low four bytes of the 128-bit field.
    std::memcpy(destination, &eid, sizeof(eid));
}

inline std::string shmdev_path(mem_id id)
{
    return "/dev/obmm_shmdev" +
           std::to_string(static_cast<unsigned long long>(id));
}

inline NcMapping map_nc(mem_id id, size_t length)
{
    NcMapping mapping;
    mapping.length = length;
    const std::string path = shmdev_path(id);
    mapping.fd = open(path.c_str(), O_RDWR | O_SYNC);
    if (mapping.fd < 0)
        fail(errno_message("open " + path));
    mapping.address = mmap(nullptr, length, PROT_READ | PROT_WRITE,
                           MAP_SHARED, mapping.fd, 0);
    if (mapping.address == MAP_FAILED) {
        const std::string message = errno_message("mmap " + path + " as NC");
        close(mapping.fd);
        mapping.fd = -1;
        fail(message);
    }
    return mapping;
}

inline void unmap_nc(NcMapping &mapping)
{
    int first_error = 0;
    if (mapping.address != MAP_FAILED) {
        if (munmap(mapping.address, mapping.length) != 0)
            first_error = errno;
        mapping.address = MAP_FAILED;
    }
    if (mapping.fd >= 0) {
        if (close(mapping.fd) != 0 && first_error == 0)
            first_error = errno;
        mapping.fd = -1;
    }
    if (first_error != 0) {
        errno = first_error;
        fail(errno_message("unmap/close OBMM shmdev"));
    }
}

inline void best_effort_unmap(NcMapping &mapping)
{
    if (mapping.address != MAP_FAILED)
        munmap(mapping.address, mapping.length);
    if (mapping.fd >= 0)
        close(mapping.fd);
    mapping.address = MAP_FAILED;
    mapping.fd = -1;
}

inline uint8_t pattern_byte(uint64_t index, uint8_t seed)
{
    return static_cast<uint8_t>((index * 131U + seed * 17U +
                                 (index >> 7)) & 0xffU);
}

inline void write_pattern(void *address, size_t length, uint8_t seed)
{
    auto *bytes = static_cast<volatile uint8_t *>(address);
    for (size_t i = 0; i < length; ++i)
        bytes[i] = pattern_byte(i, seed);
    __sync_synchronize();
}

inline void verify_pattern(const void *address, size_t length, uint8_t seed,
                           const char *phase)
{
    __sync_synchronize();
    const auto *bytes = static_cast<const volatile uint8_t *>(address);
    for (size_t i = 0; i < length; ++i) {
        const uint8_t expected = pattern_byte(i, seed);
        const uint8_t actual = bytes[i];
        if (actual != expected) {
            fail(std::string(phase) + " mismatch at offset " +
                 std::to_string(i) + ": expected " +
                 std::to_string(expected) + ", got " +
                 std::to_string(actual));
        }
    }
}

inline uint64_t host_to_be64(uint64_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (static_cast<uint64_t>(htonl(static_cast<uint32_t>(value))) << 32) |
           htonl(static_cast<uint32_t>(value >> 32));
#else
    return value;
#endif
}

inline void send_all(int fd, const void *buffer, size_t length)
{
    const auto *bytes = static_cast<const uint8_t *>(buffer);
    while (length > 0) {
        const ssize_t sent = send(fd, bytes, length, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            fail(errno_message("send"));
        }
        if (sent == 0)
            fail("send returned zero");
        bytes += sent;
        length -= static_cast<size_t>(sent);
    }
}

inline void receive_all(int fd, void *buffer, size_t length)
{
    auto *bytes = static_cast<uint8_t *>(buffer);
    while (length > 0) {
        const ssize_t received = recv(fd, bytes, length, 0);
        if (received < 0) {
            if (errno == EINTR)
                continue;
            fail(errno_message("recv"));
        }
        if (received == 0)
            fail("peer closed the TCP control connection");
        bytes += received;
        length -= static_cast<size_t>(received);
    }
}

inline void send_u32(int fd, uint32_t value)
{
    value = htonl(value);
    send_all(fd, &value, sizeof(value));
}

inline uint32_t receive_u32(int fd)
{
    uint32_t value;
    receive_all(fd, &value, sizeof(value));
    return ntohl(value);
}

inline void send_u64(int fd, uint64_t value)
{
    value = host_to_be64(value);
    send_all(fd, &value, sizeof(value));
}

inline uint64_t receive_u64(int fd)
{
    uint64_t value;
    receive_all(fd, &value, sizeof(value));
    return host_to_be64(value);
}

inline void send_control(int fd, Control value)
{
    send_u32(fd, static_cast<uint32_t>(value));
}

inline void expect_control(int fd, Control expected)
{
    const auto actual = static_cast<Control>(receive_u32(fd));
    if (actual != expected)
        fail("unexpected TCP control message: expected " +
             std::to_string(static_cast<uint32_t>(expected)) + ", got " +
             std::to_string(static_cast<uint32_t>(actual)));
}

inline void send_description(int fd, const ExportDescription &description)
{
    send_u32(fd, protocol_magic);
    send_u32(fd, protocol_version);
    send_u64(fd, description.addr);
    send_u64(fd, description.length);
    send_u32(fd, description.tokenid);
    send_u32(fd, description.dcna);
    send_all(fd, description.deid, sizeof(description.deid));
}

inline ExportDescription receive_description(int fd)
{
    if (receive_u32(fd) != protocol_magic)
        fail("invalid protocol magic");
    if (receive_u32(fd) != protocol_version)
        fail("incompatible protocol version");
    ExportDescription description;
    description.addr = receive_u64(fd);
    description.length = receive_u64(fd);
    description.tokenid = receive_u32(fd);
    description.dcna = receive_u32(fd);
    receive_all(fd, description.deid, sizeof(description.deid));
    return description;
}

inline sockaddr_in make_address(const std::string &ip, uint16_t port)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1)
        fail("invalid IPv4 address: " + ip);
    return address;
}

inline void configure_socket_timeout(int fd, int timeout_sec)
{
    timeval timeout{};
    timeout.tv_sec = timeout_sec;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0)
        fail(errno_message("setsockopt timeout"));
}

inline int accept_importer(const std::string &bind_ip, uint16_t port,
                           int timeout_sec)
{
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
        fail(errno_message("socket"));
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const sockaddr_in address = make_address(bind_ip, port);
    if (bind(listener, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0) {
        const std::string message = errno_message("bind");
        close(listener);
        fail(message);
    }
    if (listen(listener, 1) != 0) {
        const std::string message = errno_message("listen");
        close(listener);
        fail(message);
    }
    std::cout << "waiting for importer on " << bind_ip << ':' << port
              << std::endl;
    int connection;
    do {
        connection = accept(listener, nullptr, nullptr);
    } while (connection < 0 && errno == EINTR);
    const int saved_errno = errno;
    close(listener);
    errno = saved_errno;
    if (connection < 0)
        fail(errno_message("accept"));
    configure_socket_timeout(connection, timeout_sec);
    return connection;
}

inline int connect_to_exporter(const std::string &owner_ip, uint16_t port,
                               int timeout_sec)
{
    const sockaddr_in address = make_address(owner_ip, port);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(timeout_sec);
    int last_error = ECONNREFUSED;
    while (std::chrono::steady_clock::now() < deadline) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            fail(errno_message("socket"));
        if (connect(fd, reinterpret_cast<const sockaddr *>(&address),
                    sizeof(address)) == 0) {
            configure_socket_timeout(fd, timeout_sec);
            return fd;
        }
        last_error = errno;
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    errno = last_error;
    fail(errno_message("connect to exporter"));
}

inline double elapsed_ns(std::chrono::steady_clock::time_point start,
                         uint64_t iterations)
{
    const auto duration = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double, std::nano>(duration).count() /
           static_cast<double>(iterations);
}

} // namespace obmm_test
