#pragma once

namespace google
{
class LogSink {
    public:
        template <class T> LogSink &operator<<(const T &) { return *this; }
};
} // namespace google

#define INFO 0
#define WARNING 1
#define ERROR 2
#define LOG(level) ::google::LogSink()
#define CHECK(condition) if (condition) {} else ::google::LogSink()
#define DCHECK(condition) CHECK(condition)
