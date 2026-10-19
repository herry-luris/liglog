#pragma once

#include "../interface.hpp"
#include "../term_ctl/term_ctl.hpp"

namespace lig::component {

// text_formatter
class TextFormatter final : public ::lig::interface::Formatter {
  public:
    struct Config {
        ::lig::Status status{};
    };

    TextFormatter() = default;
    TextFormatter(const Config &config) { status = config.status; }
    void format(const ::lig::LogEntry &entry, std::vector<std::byte> &buf) const final;
    ~TextFormatter() final = default;

  private:
    bool has_color_{false};
};

auto createTextFormatter(const component::TextFormatter::Config config = {})
    -> std::unique_ptr<const ::lig::interface::Formatter>;

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

// term_formatter
class TermFormatter final : public ::lig::interface::Formatter {
    constexpr static size_t LevelCategory = 6;

  public:
    struct Config {
        ::lig::Status status{};
        std::array<::term_ctl::TermStr::State, LevelCategory> states{};
    };

    void format(const ::lig::LogEntry &entry, std::vector<std::byte> &buf) const final;
    ~TermFormatter() final = default;

    TermFormatter() = default;
    TermFormatter(const Config &config);

  private:
    std::array<std::string, LevelCategory> styleStrs_{};
    std::string clearStr_{};
};

auto createTermFormatter(const component::TermFormatter::Config config = {})
    -> std::unique_ptr<const ::lig::interface::Formatter>;

template <typename Transport>
struct TermFormatterConfig final
    : public ::lig::interface::FormatterConfig<TermFormatterConfig<Transport>, Transport> {
    using Base = ::lig::interface::FormatterConfig<TermFormatterConfig<Transport>, Transport>;
    TermFormatterConfig(Transport &config) : Base{config} {}
    ~TermFormatterConfig() = default;

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
