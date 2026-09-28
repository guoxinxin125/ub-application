#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace star {

// Bound SDK calls while allowing a peer's region to appear later. Errors
// other than NOT_FOUND may be brief during startup, but must not be retried
// for the entire peer timeout.
class UBMapRetryPolicy {
public:
        static constexpr uint32_t max_other_errors = 16;

        bool record_failure(bool not_found)
        {
                ++attempts_;
                if (!not_found && ++other_errors_ >= max_other_errors)
                        return false;
                return true;
        }

        std::chrono::milliseconds delay() const
        {
                if (attempts_ == 0)
                        return std::chrono::milliseconds(100);
                const uint32_t shift = std::min<uint32_t>(attempts_ - 1, 4);
                return std::chrono::milliseconds(
                        std::min<uint32_t>(100U << shift, 1000U));
        }

        uint32_t attempts() const { return attempts_; }
        uint32_t other_errors() const { return other_errors_; }

private:
        uint32_t attempts_ = 0;
        uint32_t other_errors_ = 0;
};

} // namespace star
