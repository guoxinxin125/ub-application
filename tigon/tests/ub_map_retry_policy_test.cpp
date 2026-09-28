#include "common/UBMapRetryPolicy.h"

#include <chrono>
#include <stdexcept>

namespace {
void require(bool condition)
{
        if (!condition)
                throw std::runtime_error("UB map retry policy test failed");
}
}

int main()
{
        star::UBMapRetryPolicy peer_wait;
        require(peer_wait.delay() == std::chrono::milliseconds(100));
        for (int i = 0; i < 100; ++i)
                require(peer_wait.record_failure(true));
        require(peer_wait.other_errors() == 0);
        require(peer_wait.delay() == std::chrono::milliseconds(1000));

        star::UBMapRetryPolicy sdk_error;
        for (uint32_t i = 1; i < star::UBMapRetryPolicy::max_other_errors; ++i)
                require(sdk_error.record_failure(false));
        require(!sdk_error.record_failure(false));
        require(sdk_error.attempts() == star::UBMapRetryPolicy::max_other_errors);
        require(sdk_error.other_errors() == star::UBMapRetryPolicy::max_other_errors);

        star::UBMapRetryPolicy mixed;
        require(mixed.record_failure(false));
        require(mixed.record_failure(true));
        require(mixed.other_errors() == 1);
        require(mixed.delay() == std::chrono::milliseconds(200));
        return 0;
}
