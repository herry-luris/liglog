#include "component/formatter.hpp"
#include "interface.hpp"
#include <chrono>

namespace lig::component {

// common utils
static inline void put_str(std::vector<std::byte> &buf, std::string_view s) {
    const auto *p = reinterpret_cast<const std::byte *>(s.data());
    buf.insert(buf.end(), p, p + s.size());
}
template <typename T> static void put_int(std::vector<std::byte> &buf, T n) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>, "T must be unsigned num");
    char tmp[24]{};
    auto [ptr, ec] = std::to_chars(tmp, tmp + sizeof(tmp), n);
    assert(ec == std::errc{});
    put_str(buf, std::string_view{tmp, static_cast<size_t>(ptr - tmp)});
}

// uniform format: now - [level] thread_id (filename:line) funcname:
static inline void put_timestamp(std::vector<std::byte> &buf, bool cond,
                                 std::chrono::system_clock::time_point now) {
    if (cond) {
        char tmp[32]{};
        auto end = std::format_to(tmp, "{:%Y-%m-%d %H:%M:%S}", now);
        put_str(buf, std::string_view{tmp, static_cast<size_t>(end - tmp)});
    }
}

static inline void put_position(std::vector<std::byte> &buf, const ::lig::Status &status,
                                const ::lig::LogEntry &entry) {
    if (status.hasThreadId()) {
        put_str(buf, " ");
        put_int(buf, entry.head.thread_id);
    }
    if (status.hasLocation()) {
        put_str(buf, " (");
        put_str(buf, entry.head.filename ? entry.head.filename : "<unknown>");
        put_str(buf, ":");
        put_int(buf, entry.head.line);
        put_str(buf, ")");
    }
    if (status.hasFuncname()) {
        put_str(buf, " ");
        put_str(buf, entry.head.funcname ? entry.head.funcname : "<unknown>");
    }
}

// TextFormatter
void TextFormatter::format(const ::lig::LogEntry &entry, std::vector<std::byte> &buf) const {
    put_timestamp(buf, status.hasTimeStamp(), entry.head.now);
    put_str(buf, " - [");
    put_str(buf, get_level_string(entry.head.level));
    put_str(buf, "]");
    put_position(buf, status, entry);
    put_str(buf, entry.body);
}

auto createTextFormatter(const component::TextFormatter::Config config)
    -> std::unique_ptr<const ::lig::interface::Formatter> {
    auto formatter = std::make_unique<component::TextFormatter>(config);
    return std::unique_ptr<const ::lig::interface::Formatter>(formatter.release());
}

// TermFormatter
TermFormatter::TermFormatter(const Config &config) {
    thread_local ::term_ctl::TermStr term{};
    status = config.status;
    for (size_t i = 0; i < LevelCategory; ++i) {
        styleStrs_[i] = term.state(config.states[i]).snapshot();
    }
    clearStr_ = term.clear().drains();
}
void TermFormatter::format(const ::lig::LogEntry &entry, std::vector<std::byte> &buf) const {
    put_timestamp(buf, status.hasTimeStamp(), entry.head.now);
    put_str(buf, " - ");
    put_str(buf, styleStrs_[static_cast<uint8_t>(entry.head.level)]);
    put_str(buf, "[");
    put_str(buf, get_level_string(entry.head.level));
    put_str(buf, "]");
    put_str(buf, clearStr_);
    put_position(buf, status, entry);
    put_str(buf, entry.body);
}

auto createTermFormatter(const component::TermFormatter::Config config)
    -> std::unique_ptr<const ::lig::interface::Formatter> {
    auto formatter = std::make_unique<component::TermFormatter>(config);
    return std::unique_ptr<const ::lig::interface::Formatter>(formatter.release());
}
} // namespace lig::component
