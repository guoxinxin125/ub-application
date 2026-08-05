#ifndef UBSM_TEST_COMMON_H
#define UBSM_TEST_COMMON_H

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <ubs_mem.h>
#include <ubs_mem_def.h>

namespace ubsm_test {

constexpr uint64_t kMiB = 1024ULL * 1024ULL;
constexpr uint64_t kAllocationAlignment = 4ULL * kMiB;
constexpr uint64_t kCacheLineBytes = 64;
constexpr uint64_t kCacheLineWords =
    kCacheLineBytes / static_cast<uint64_t>(sizeof(uint64_t));
constexpr int kSdkLogLevel = 3; // ERROR: suppress SDK INFO and WARN logs.
// UBSM_FLAG_ONLY_IMPORT_NONCACHE - owner use cacheable mapping, remote use non-cacheable mapping
constexpr uint64_t kOneSidedFlags =
    UBSM_FLAG_ONLY_IMPORT_NONCACHE | UBSM_FLAG_WR_DELAY_COMP;

[[noreturn]] inline void fail(const std::string &message)
{
    throw std::runtime_error(message);
}

inline std::string errno_message(const std::string &operation)
{
    return operation + ": " + std::strerror(errno);
}

inline uint64_t parse_u64(const std::string &text, const std::string &option)
{
    size_t consumed = 0;
    unsigned long long value = 0;
    try {
        value = std::stoull(text, &consumed, 0);
    } catch (const std::exception &) {
        fail("invalid value for " + option + ": " + text);
    }
    if (consumed != text.size())
        fail("invalid value for " + option + ": " + text);
    return static_cast<uint64_t>(value);
}

inline uint16_t parse_port(const std::string &text)
{
    const uint64_t value = parse_u64(text, "--port");
    if (value == 0 || value > std::numeric_limits<uint16_t>::max())
        fail("--port must be in [1, 65535]");
    return static_cast<uint16_t>(value);
}

inline void validate_sizes(uint64_t region_bytes, uint64_t test_bytes)
{
    if (region_bytes < kAllocationAlignment ||
        region_bytes % kAllocationAlignment != 0) {
        fail("region size must be at least 4 MiB and a multiple of 4 MiB");
    }
    if (test_bytes == 0 || test_bytes > region_bytes)
        fail("--test-bytes must be in [1, region size]");
}

inline uint64_t region_bytes_from_mb(uint64_t region_mb)
{
    if (region_mb > std::numeric_limits<uint64_t>::max() / kMiB)
        fail("--region-mb is too large");
    return region_mb * kMiB;
}

inline int parse_positive_int(const std::string &text, const std::string &option)
{
    const uint64_t value = parse_u64(text, option);
    if (value == 0 || value > static_cast<uint64_t>(std::numeric_limits<int>::max()))
        fail(option + " must be in [1, INT_MAX]");
    return static_cast<int>(value);
}

inline uint32_t parse_u32(const std::string &text, const std::string &option)
{
    const uint64_t value = parse_u64(text, option);
    if (value > std::numeric_limits<uint32_t>::max())
        fail(option + " must fit in uint32_t");
    return static_cast<uint32_t>(value);
}

inline void validate_name(const std::string &name)
{
    if (name.empty() || name.size() >= MAX_SHM_NAME_LENGTH)
        fail("shared-memory name must contain 1 to 47 characters");
}

inline void check_ubsm(int result, const std::string &operation)
{
    if (result != UBSM_OK)
        fail(operation + " failed, UBS Memory error=" + std::to_string(result));
}

class SdkSession {
public:
    SdkSession()
    {
        // set logger level to 3 to suppress SDK INFO and WARN logs, only show ERROR logs
        check_ubsm(ubsmem_set_logger_level(kSdkLogLevel),
                   "ubsmem_set_logger_level");
        ubsmem_options_t options{};
        // init before ubsmem use
        check_ubsm(ubsmem_init_attributes(&options), "ubsmem_init_attributes");
        check_ubsm(ubsmem_initialize(&options), "ubsmem_initialize");
        active_ = true;
    }

    SdkSession(const SdkSession &) = delete;
    SdkSession &operator=(const SdkSession &) = delete;

    ~SdkSession()
    {
        if (active_) {
            const int result = ubsmem_finalize();
            if (result != UBSM_OK)
                std::cerr << "WARN: ubsmem_finalize failed, error=" << result << '\n';
        }
    }

    void finalize()
    {
        if (!active_)
            return;
        check_ubsm(ubsmem_finalize(), "ubsmem_finalize");
        active_ = false;
    }

private:
    bool active_ = false;
};

class SharedMemory {
public:
    SharedMemory(std::string name, size_t size) : name_(std::move(name)), size_(size) {}

    SharedMemory(const SharedMemory &) = delete;
    SharedMemory &operator=(const SharedMemory &) = delete;

    ~SharedMemory()
    {
        unmap_noexcept();
        deallocate_noexcept();
    }

    void allocate()
    {
        check_ubsm(ubsmem_shmem_allocate("default", name_.c_str(), size_,
                                         S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP,
                                         kOneSidedFlags),
                   "ubsmem_shmem_allocate(" + name_ + ")");
        allocated_ = true;
    }

    void allocate_with_provider(const std::string &host_name, uint32_t socket_id,
                                uint32_t numa_id, uint32_t port_id)
    {
        if (host_name.empty() || host_name.size() >= MAX_HOST_NAME_DESC_LENGTH)
            fail("provider hostname must contain 1 to 63 characters");
        ubs_mem_provider_t provider{};
        std::memcpy(provider.host_name, host_name.c_str(), host_name.size() + 1);
        provider.socket_id = socket_id;
        provider.numa_id = numa_id;
        provider.port_id = port_id;
        check_ubsm(ubsmem_shmem_allocate_with_provider(
                       &provider, name_.c_str(), size_,
                       S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP,
                       kOneSidedFlags),
                   "ubsmem_shmem_allocate_with_provider(" + name_ + ")");
        allocated_ = true;
    }

    void map()
    {
        void *result = nullptr;
        check_ubsm(ubsmem_shmem_map(nullptr, size_, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, name_.c_str(), 0, &result),
                   "ubsmem_shmem_map(" + name_ + ")");
        if (result == nullptr || result == MAP_FAILED)
            fail("ubsmem_shmem_map returned an invalid address");
        address_ = result;
    }

    void unmap()
    {
        if (address_ == nullptr)
            return;
        check_ubsm(ubsmem_shmem_unmap(address_, size_),
                   "ubsmem_shmem_unmap(" + name_ + ")");
        address_ = nullptr;
    }

    void deallocate()
    {
        if (!allocated_)
            return;
        check_ubsm(ubsmem_shmem_deallocate(name_.c_str()),
                   "ubsmem_shmem_deallocate(" + name_ + ")");
        allocated_ = false;
    }

    volatile uint8_t *bytes() const
    {
        return static_cast<volatile uint8_t *>(address_);
    }

    volatile uint64_t *word() const
    {
        return static_cast<volatile uint64_t *>(address_);
    }

private:
    void unmap_noexcept() noexcept
    {
        if (address_ == nullptr)
            return;
        const int result = ubsmem_shmem_unmap(address_, size_);
        if (result != UBSM_OK)
            std::cerr << "WARN: cleanup unmap failed for " << name_
                      << ", error=" << result << '\n';
        else
            address_ = nullptr;
    }

    void deallocate_noexcept() noexcept
    {
        if (!allocated_)
            return;
        const int result = ubsmem_shmem_deallocate(name_.c_str());
        if (result != UBSM_OK)
            std::cerr << "WARN: cleanup deallocate failed for " << name_
                      << ", error=" << result << '\n';
        else
            allocated_ = false;
    }

    std::string name_;
    size_t size_;
    void *address_ = nullptr;
    bool allocated_ = false;
};

inline uint8_t pattern_byte(uint64_t index, uint8_t seed)
{
    return static_cast<uint8_t>(seed ^ static_cast<uint8_t>(index * 131U) ^
                                static_cast<uint8_t>(index >> 7U));
}

inline void write_pattern(volatile uint8_t *address, uint64_t length, uint8_t seed)
{
    for (uint64_t i = 0; i < length; ++i)
        address[i] = pattern_byte(i, seed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

inline void verify_pattern(volatile const uint8_t *address, uint64_t length,
                           uint8_t seed, const std::string &description)
{
    std::atomic_thread_fence(std::memory_order_seq_cst);
    for (uint64_t i = 0; i < length; ++i) {
        const uint8_t expected = pattern_byte(i, seed);
        const uint8_t actual = address[i];
        if (actual != expected) {
            fail(description + " mismatch at byte " + std::to_string(i) +
                 ": expected=" + std::to_string(expected) +
                 ", actual=" + std::to_string(actual));
        }
    }
}

inline uint64_t cacheline_word_value(uint64_t sequence, uint64_t word_index)
{
    return sequence ^ (0x9e3779b97f4a7c15ULL * (word_index + 1));
}

inline void write_cacheline(volatile uint64_t *words, uint64_t sequence)
{
    for (uint64_t word = 0; word < kCacheLineWords; ++word)
        words[word] = cacheline_word_value(sequence, word);
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

inline void verify_cacheline(volatile const uint64_t *words, uint64_t sequence,
                             const std::string &description)
{
    std::atomic_thread_fence(std::memory_order_seq_cst);
    for (uint64_t word = 0; word < kCacheLineWords; ++word) {
        const uint64_t expected = cacheline_word_value(sequence, word);
        const uint64_t actual = words[word];
        if (actual != expected) {
            fail(description + " mismatch at word " + std::to_string(word) +
                 ": expected=" + std::to_string(expected) +
                 ", actual=" + std::to_string(actual));
        }
    }
}

inline double benchmark_checked_cacheline_load(volatile uint64_t *words,
                                                uint64_t iterations,
                                                uint64_t sequence)
{
    verify_cacheline(words, sequence, "64-byte load before benchmark");
    uint64_t sinks[kCacheLineWords]{};
    const auto start = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < iterations; ++i) {
        for (uint64_t word = 0; word < kCacheLineWords; ++word)
            sinks[word] ^= words[word];
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    verify_cacheline(words, sequence, "64-byte load after benchmark");
    uint64_t sink = 0;
    for (uint64_t word = 0; word < kCacheLineWords; ++word)
        sink ^= sinks[word];
    if (sink == std::numeric_limits<uint64_t>::max())
        std::cerr << "unreachable sink=" << sink << '\n';
    return std::chrono::duration<double, std::nano>(elapsed).count() /
           static_cast<double>(iterations);
}

inline double benchmark_fenced_cacheline_store(volatile uint64_t *words,
                                                uint64_t iterations)
{
    const auto start = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < iterations; ++i) {
        for (uint64_t word = 0; word < kCacheLineWords; ++word)
            words[word] = cacheline_word_value(i, word);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    verify_cacheline(words, iterations - 1, "64-byte fenced store");
    return std::chrono::duration<double, std::nano>(elapsed).count() /
           static_cast<double>(iterations);
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

inline int accept_remote(const std::string &bind_ip, uint16_t port, int timeout_sec)
{
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
        fail(errno_message("socket"));
    int reuse = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const sockaddr_in address = make_address(bind_ip, port);
    if (bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        const std::string message = errno_message("bind");
        close(listener);
        fail(message);
    }
    if (listen(listener, 1) != 0) {
        const std::string message = errno_message("listen");
        close(listener);
        fail(message);
    }
    std::cout << "waiting for remote on " << bind_ip << ':' << port << std::endl;
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

inline int connect_to_owner(const std::string &owner_ip, uint16_t port, int timeout_sec)
{
    const sockaddr_in address = make_address(owner_ip, port);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(timeout_sec);
    int last_error = ECONNREFUSED;
    while (std::chrono::steady_clock::now() < deadline) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            fail(errno_message("socket"));
        if (connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0) {
            configure_socket_timeout(fd, timeout_sec);
            return fd;
        }
        last_error = errno;
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    errno = last_error;
    fail(errno_message("connect to owner"));
}

inline void send_stage(int fd, uint8_t stage)
{
    ssize_t sent;
    do {
        sent = send(fd, &stage, sizeof(stage), MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent != static_cast<ssize_t>(sizeof(stage)))
        fail(sent < 0 ? errno_message("send stage") : "short send of stage byte");
}

inline void expect_stage(int fd, uint8_t expected)
{
    uint8_t actual = 0;
    ssize_t received;
    do {
        received = recv(fd, &actual, sizeof(actual), MSG_WAITALL);
    } while (received < 0 && errno == EINTR);
    if (received != static_cast<ssize_t>(sizeof(actual)))
        fail(received < 0 ? errno_message("receive stage") : "connection closed while receiving stage");
    if (actual != expected)
        fail("unexpected synchronization stage: expected=" +
             std::to_string(expected) + ", actual=" + std::to_string(actual));
}

inline void print_latency(const std::string &name, double value)
{
    std::cout << std::fixed << std::setprecision(2) << name << '=' << value << '\n';
}

} // namespace ubsm_test

#endif
