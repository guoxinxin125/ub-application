#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace star {

// Copy from a remotely imported NC mapping to ordinary local memory using
// scalar loads. Volatile prevents the compiler from replacing the remote
// loads with vector/REP memcpy; it does not provide synchronization. Callers
// must continue to acquire the tuple lock before reading its value.
class UBScalarRead {
public:
        static bool enabled()
        {
                static const bool value = [] {
                        const char *env = std::getenv("TIGON_UB_SCALAR_READ");
                        return env == nullptr || !(env[0] == '0' && env[1] == '\0');
                }();
                return value;
        }

        static void copy(void *local, const void *remote, std::size_t size)
        {
                if (!enabled()) {
                        std::memcpy(local, remote, size);
                        return;
                }

                auto *dst = static_cast<unsigned char *>(local);
                auto *src = static_cast<const volatile unsigned char *>(remote);
                // A tuple value is not necessarily 8-byte aligned. Only use
                // word loads after reaching an aligned remote address.
                while (size && (reinterpret_cast<std::uintptr_t>(src) & 7U)) {
                        *dst++ = *src++;
                        --size;
                }

#if defined(__GNUC__) || defined(__clang__)
                using AliasWord = std::uint64_t __attribute__((__may_alias__));
                while (size >= sizeof(AliasWord)) {
                        const AliasWord word =
                                *reinterpret_cast<const volatile AliasWord *>(src);
                        // This memcpy touches only local memory; the remote
                        // access above remains one volatile scalar load.
                        std::memcpy(dst, &word, sizeof(word));
                        src += sizeof(word);
                        dst += sizeof(word);
                        size -= sizeof(word);
                }
#endif
                while (size != 0) {
                        *dst++ = *src++;
                        --size;
                }
        }
};

} // namespace star
