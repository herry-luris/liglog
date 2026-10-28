#pragma once

#include "../interface.hpp"
#include "../term_ctl/term_ctl.hpp"

namespace lig::component {

/// Plain-text formatter. Output format:
///
///     <timestamp> - [<level>] <thread_id> (<file>:<line>) <funcname><body>
///
/// Each header field is controlled by `status`; the level tag is always
/// present. Does no coloring.
class TextFormatter final : public ::lig::interface::Formatter {
  public:
    struct Config {
        ::lig::Status status{};
    };

    TextFormatter() = default;
    TextFormatter(const Config &config) { status = config.status; }

    /// Write the formatted entry into `buf` (appended, not replaced).
    void format(const ::lig::LogEntry &entry, std::vector<std::byte> &buf) const final;
    ~TextFormatter() final = default;

  private:
    bool has_color_{false};
};

/// Create a TextFormatter. Ownership is returned as a unique_ptr-to-const,
/// matching IFormatterConfig::start's contract.
auto createTextFormatter(const component::TextFormatter::Config config = {})
    -> std::unique_ptr<const ::lig::interface::Formatter>;

/// Fluent builder for TextFormatter. Exposes timestamp / thread_id /
/// location / function_name setters from FormatterConfig.
template <typename Transport>
struct TextFormatterConfig final
    : public ::lig::interface::FormatterConfig<TextFormatterConfig<Transport>, Transport> {

    using Base = ::lig::interface::FormatterConfig<TextFormatterConfig<Transport>, Transport>;
    TextFormatterConfig(Transport &config) : Base{config} {}
    ~TextFormatterConfig() final = default;

    auto start() -> std::unique_ptr<const interface::Formatter> final {
        return createTextFormatter({.status = this->status_});
    }
};

/// Terminal formatter. Same layout as TextFormatter but wraps the level tag
/// in ANSI SGR escapes and emits a reset after it. Per-level styles are
/// configured through Config::states.
class TermFormatter final : public ::lig::interface::Formatter {
    constexpr static size_t LevelCategory = 6;

  public:
    struct Config {
        ::lig::Status status{};
        /// One TermStr::State per level (Trace..Fatal), used to colorize
        /// the `[<level>]` tag.
        std::array<::term_ctl::TermStr::State, LevelCategory> states{};
    };

    /// Write the formatted entry into `buf` (appended, not replaced).
    void format(const ::lig::LogEntry &entry, std::vector<std::byte> &buf) const final;
    ~TermFormatter() final = default;

    /// Build from a Config. There is no default constructor: TermFormatter
    /// pre-computes per-level escape strings in the constructor, so the
    /// Config (even an empty one) must be passed explicitly.
    TermFormatter(const Config &config);

  private:
    std::array<std::string, LevelCategory> styleStrs_{};
    std::string clearStr_{};
};

/// Create a TermFormatter. Ownership is returned as a unique_ptr-to-const.
auto createTermFormatter(const component::TermFormatter::Config config = {})
    -> std::unique_ptr<const ::lig::interface::Formatter>;

/// Fluent builder for TermFormatter. In addition to the common Status
/// setters, provides `state<Level>(state)` to set the per-level color.
template <typename Transport>
struct TermFormatterConfig final
    : public ::lig::interface::FormatterConfig<TermFormatterConfig<Transport>, Transport> {
    using Base = ::lig::interface::FormatterConfig<TermFormatterConfig<Transport>, Transport>;
    TermFormatterConfig(Transport &config) : Base{config} {}
    ~TermFormatterConfig() = default;

    /// Set the TermStr::State used for one level, e.g.
    ///     .state<lig::Level::Error>(red_on_bold)
    template <Level level> auto state(const ::term_ctl::TermStr::State &state) {
        config.states[static_cast<uint8_t>(level)] = state;
    }

    auto start() -> std::unique_ptr<const interface::Formatter> final {
        return createTermFormatter(config);
    }

  private:
    component::TermFormatter::Config config{};
};

}; // namespace lig::component
