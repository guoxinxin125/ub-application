#include "common/UBScalarRead.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

int main()
{
        std::array<std::uint8_t, 2100> source{};
        std::array<std::uint8_t, 2100> target{};
        for (std::size_t i = 0; i < source.size(); ++i)
                source[i] = static_cast<std::uint8_t>((i * 37 + 19) & 0xff);

        for (std::size_t offset = 0; offset < 8; ++offset) {
                for (std::size_t length : {0U, 1U, 7U, 8U, 9U, 100U,
                                           1010U, 1024U, 2048U}) {
                        target.fill(0xa5);
                        star::UBScalarRead::copy(target.data() + 16,
                                                 source.data() + offset,
                                                 length);
                        for (std::size_t i = 0; i < length; ++i)
                                if (target[16 + i] != source[offset + i])
                                        return 1;
                        if (target[15] != 0xa5 || target[16 + length] != 0xa5)
                                return 2;
                }
        }
}
