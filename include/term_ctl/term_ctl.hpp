#pragma once
#include <cassert>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace term_ctl {

// enum struct Style : uint8_t {
// 	Clear = 0, Heavy, Light, Italic, UnderLine, SlowShine, FastShine, Reverse, Hide, Delete,
// 	WeightClear = 22, ItalicClear, UnderLineClear, ShineClear, ReverseClear = 27, HideClear,
// DeleteClear, 	FgBlack = 30, FgRed, FgGreen, FgYellow, FgBlue, FgFuchsia, FgCyan,
// FgWhite, FgColor, FgClear, 	BgBlack = 40, BgRed, BgGreen, BgYellow, BgBlue, BgFuchsia,
// BgCyan, BgWhite, BgColor, BgClear, 	Border = 51, Circle, UpperLine, BorderClear,
// UpperLineClear, ULColor = 58, ULColorClear, 	FgLBlack = 90, FgLRed, FgLGreen, FgLYellow,
// FgLBlue, FgLFuchsia, FgLCyan, FgLWhite, 	BgLBlack = 100, BgLRed, BgLGreen, BgLYellow,
// BgLBlue, BgLFuchsia, BgLCyan, BgLWhite,
//
// };

// clang-format off
enum struct ColorMode { Default ,Color16, Color256, TrueColor };
struct TrueColor { uint8_t r{}, g{}, b{}; };
enum struct Color16 : uint8_t {
	Black = 30, Red, Green, Yellow, Blue, Fuchsia, Cyan, White,
	LBlack = 90, LRed, LGreen, LYellow, LBlue, LFuchsia, LCyan, LWhite, 
};
enum struct Weight : uint8_t { Default, Heavy, Light };
enum struct Shining : uint8_t { Default, Slow, Fast };
enum struct Border : uint8_t { Default, Border, Circle };
// clang-format on

inline auto append_bytes(char *pos, char *end, std::string_view src) noexcept -> char * {
    assert(pos + src.size() <= end);
    std::memcpy(pos, src.data(), src.size());
    return pos + src.size();
};
template <size_t N>
inline auto append_lit(char *pos, char *end, const char (&src)[N]) noexcept -> char * {
    assert(pos + N - 1 <= end);
    std::memcpy(pos, src, N - 1);
    return pos + N - 1;
};

class Color {
    constexpr static const char *FgColor256Str{"38;5;"};
    constexpr static const char *BgColor256Str{"48;5;"};
    constexpr static const char *UlColor256Str{"58;5;"};
    constexpr static const char *FgTrueColorStr{"38;2;"};
    constexpr static const char *BgTrueColorStr{"48;2;"};
    constexpr static const char *UlTrueColorStr{"58;2;"};
    constexpr static const char *FgDefaultStr{"39"};
    constexpr static const char *BgDefaultStr{"49"};
    constexpr static const char *UlDefaultStr{"59"};

    union Value {
        Color16 c16;
        uint8_t c256;
        TrueColor tc;
    };

  public:
    enum struct Type { Foreground, Background, Underline };

    auto mode() const noexcept -> ColorMode { return m_; }
    auto as_color16() const noexcept -> std::optional<Color16> {
        if (m_ == ColorMode::Color16) {
            return v_.c16;
        }
        return std::nullopt;
    }
    auto as_color256() const noexcept -> std::optional<uint8_t> {
        if (m_ == ColorMode::Color256) {
            return v_.c256;
        }
        return std::nullopt;
    }
    auto as_true_color() const noexcept -> std::optional<TrueColor> {
        if (m_ == ColorMode::TrueColor) {
            return v_.tc;
        }
        return std::nullopt;
    }
    void set_value() noexcept { m_ = ColorMode::Default; }
    void set_value(Color16 color) noexcept {
        m_ = ColorMode::Color16;
        v_.c16 = color;
    }
    void set_value(uint8_t color) noexcept {
        m_ = ColorMode::Color256;
        v_.c256 = color;
    }
    void set_value(TrueColor color) noexcept {
        m_ = ColorMode::TrueColor;
        v_.tc = color;
    }

    template <Type type> auto to_buf(char *pos, char *end) const -> char * {
        if (m_ == ColorMode::Default) {
            static constexpr const std::string_view data = [&]() -> std::string_view {
                if constexpr (type == Type::Background) {
                    return BgDefaultStr;
                } else if constexpr (type == Type::Foreground) {
                    return FgDefaultStr;
                } else {
                    return UlDefaultStr;
                }
            }();
            pos = append_bytes(pos, end, data);
        } else if (m_ == ColorMode::Color16) {
            if constexpr (type == Type::Background) {
                pos = put_u8(pos, end, static_cast<uint8_t>(v_.c16) + 10);
            } else if constexpr (type == Type::Foreground) {
                pos = put_u8(pos, end, static_cast<uint8_t>(v_.c16));
            }
        } else if (m_ == ColorMode::Color256) {
            static constexpr const std::string_view data = [&]() -> std::string_view {
                if constexpr (type == Type::Background) {
                    return BgColor256Str;
                } else if constexpr (type == Type::Foreground) {
                    return FgColor256Str;
                } else {
                    return UlColor256Str;
                }
            }();
            pos = append_bytes(pos, end, data);
            pos = put_u8(pos, end, v_.c256);
        } else {
            static constexpr const std::string_view data = [&]() -> std::string_view {
                if constexpr (type == Type::Background) {
                    return BgTrueColorStr;
                } else if constexpr (type == Type::Foreground) {
                    return FgTrueColorStr;
                } else {
                    return UlTrueColorStr;
                }
            }();
            pos = append_bytes(pos, end, data);
            pos = put_u8(pos, end, v_.tc.r);
            pos = append_lit(pos, end, ";");
            pos = put_u8(pos, end, v_.tc.g);
            pos = append_lit(pos, end, ";");
            pos = put_u8(pos, end, v_.tc.b);
        }
        return pos;
    }

  private:
    ColorMode m_{};
    Value v_{};

    auto put_u8(char *pos, char *cut, uint8_t n) const -> char * {
        auto [end, err] = std::to_chars(pos, cut, n);
        return err == std::errc{} ? end : pos;
    }
};

class TermStr {
    static constexpr const uint8_t Capacity = 14;
    // clang-format off
    enum struct Order : uint16_t {
        Clear = 0, Weight = 1, Italic = 2, UnderLine = 4, Shining = 8, Reverse = 16,
        Hide = 32, DeleteLine = 64, FgColor = 128, BgColor = 256, Border = 512, UpperLine = 1024,
		UlColor = 2048
    };
    // clang-format on
    static_assert(Capacity >= 13, "Capacity must fit 11 non-clear orders");

    static constexpr const size_t MaxStrLength = 128;

    static constexpr const char *ClearStr{"0"};
    static constexpr const char *ItalicStr{"3"};
    static constexpr const char *ClearItalicStr{"23"};
    static constexpr const char *UnderLineStr{"4"};
    static constexpr const char *ClearUnderLineStr{"24"};
    static constexpr const char *ReverseStr{"7"};
    static constexpr const char *ClearReverseStr{"27"};
    static constexpr const char *HideStr{"8"};
    static constexpr const char *ClearHideStr{"28"};
    static constexpr const char *DeleteLineStr{"9"};
    static constexpr const char *ClearDeleteLineStr{"29"};
    static constexpr const char *UpperLineStr{"53"};
    static constexpr const char *ClearUpperLineStr{"55"};

    constexpr static const char *weight_str[3]{"22", "1", "2"};
    constexpr static const char *shining_str[3]{"25", "5", "6"};
    constexpr static const char *border_str[3]{"54", "51", "52"};

    static constexpr auto get_weight_str(Weight w) noexcept -> std::string_view {
        return weight_str[static_cast<uint8_t>(w)];
    };
    static constexpr auto get_shining_str(Shining s) noexcept -> std::string_view {
        return shining_str[static_cast<uint8_t>(s)];
    };
    static constexpr auto get_border_str(Border b) noexcept -> std::string_view {
        return border_str[static_cast<uint8_t>(b)];
    };

  public:
    struct State {
        Weight weight{};
        bool italic{};
        bool underline{};
        Shining shining{};
        bool reverse{};
        bool hidden{};
        bool deleteline{};
        bool upperline{};
        Border border{};
        Color fg_color{};
        Color bg_color{};
        Color ul_color{};
    };

    auto get_weight() const noexcept -> Weight { return state_.weight; }
    auto is_italic() const noexcept -> bool { return state_.italic; }
    auto has_underline() const noexcept -> bool { return state_.underline; }
    auto get_shining() const noexcept -> Shining { return state_.shining; }
    auto is_reverse() const noexcept -> bool { return state_.reverse; }
    auto is_hidden() const noexcept -> bool { return state_.hidden; }
    auto has_deleteline() const noexcept -> bool { return state_.deleteline; }
    auto has_upperline() const noexcept -> bool { return state_.upperline; }
    auto get_bg_color() const noexcept -> Color { return state_.bg_color; }
    auto get_fg_color() const noexcept -> Color { return state_.fg_color; }
    auto get_ul_color() const noexcept -> Color { return state_.ul_color; }
    auto get_border() const noexcept -> Border { return state_.border; }
    auto get_state() const noexcept -> const State & { return state_; }

#define BoolSetting(method, var, order)                                                        \
    auto method(bool enable = true) noexcept -> TermStr & {                                    \
        var = enable;                                                                          \
        in(order);                                                                             \
        return *this;                                                                          \
    }

    BoolSetting(italic, state_.italic, Order::Italic);
    BoolSetting(underline, state_.underline, Order::UnderLine);
    BoolSetting(reverse, state_.reverse, Order::Reverse);
    BoolSetting(hidden, state_.hidden, Order::Hide);
    BoolSetting(deleteline, state_.deleteline, Order::DeleteLine);
    BoolSetting(upperline, state_.upperline, Order::UpperLine);
#undef BoolSetting

#define ColorSetting(method, var, type, order)                                                 \
    auto method(type color) noexcept -> TermStr & {                                            \
        var.set_value(color);                                                                  \
        in(order);                                                                             \
        return *this;                                                                          \
    }

    ColorSetting(fg_color, state_.fg_color, Color16, Order::FgColor);
    ColorSetting(bg_color, state_.bg_color, Color16, Order::BgColor);
    ColorSetting(fg_color, state_.fg_color, uint8_t, Order::FgColor);
    ColorSetting(bg_color, state_.bg_color, uint8_t, Order::BgColor);
    ColorSetting(ul_color, state_.ul_color, uint8_t, Order::UlColor);
    ColorSetting(fg_color, state_.fg_color, TrueColor, Order::FgColor);
    ColorSetting(bg_color, state_.bg_color, TrueColor, Order::BgColor);
    ColorSetting(ul_color, state_.ul_color, TrueColor, Order::UlColor);
#undef ColorSetting

#define MethodSetting(method, var, type, order)                                                \
    auto method(type value) noexcept -> TermStr & {                                            \
        var = value;                                                                           \
        in(order);                                                                             \
        return *this;                                                                          \
    }
    MethodSetting(fg_color, state_.fg_color, Color, Order::FgColor);
    MethodSetting(bg_color, state_.bg_color, Color, Order::BgColor);
    MethodSetting(weight, state_.weight, Weight, Order::Weight);
    MethodSetting(shining, state_.shining, Shining, Order::Shining);
    MethodSetting(border, state_.border, Border, Order::Border);
#undef MethodSetting

    auto clear() noexcept -> TermStr & {
        state_ = State{};
        order_reset();
        in(Order::Clear);
        return *this;
    }
    auto fg_color() noexcept -> TermStr & {
        state_.fg_color.set_value();
        in(Order::FgColor);
        return *this;
    }
    auto bg_color() noexcept -> TermStr & {
        state_.bg_color.set_value();
        in(Order::BgColor);
        return *this;
    }
    auto ul_color() noexcept -> TermStr & {
        state_.ul_color.set_value();
        in(Order::UlColor);
        return *this;
    }
    auto ul_color(Color color) noexcept -> TermStr & {
        if (auto v = color.as_color256()) {
            ul_color(*v);
        } else if (auto v = color.as_true_color()) {
            ul_color(*v);
        } else if (color.mode() == ColorMode::Default) {
            ul_color();
        }
        return *this;
    }

    auto state(const State &state) -> TermStr & {
        weight(state.weight);
        italic(state.italic);
        underline(state.underline);
        shining(state.shining);
        reverse(state.reverse);
        hidden(state.hidden);
        deleteline(state.deleteline);
        fg_color(state.fg_color);
        bg_color(state.bg_color);
        ul_color(state.ul_color);
        upperline(state.upperline);
        border(state.border);
        return *this;
    }

    void snapshot_to(std::string &str) const {
        char buf[MaxStrLength];
        char *end = generate_snapshot_str(buf, buf + MaxStrLength);
        str.insert(str.end(), buf, end);
    }
    auto snapshot() const -> std::string {
        std::string res{};
        res.reserve(MaxStrLength);
        snapshot_to(res);
        return res;
    }

    template <bool isClean = true> void to(std::string &str) {
        char buf[MaxStrLength];
        char *end = generate_order_str(buf, buf + MaxStrLength);
        str.insert(str.end(), buf, end);
        if constexpr (isClean) {
            order_reset();
        }
    }
    template <bool isClean = true> auto drains() -> std::string {
        std::string res{};
        res.reserve(MaxStrLength);
        to<isClean>(res);
        return res;
    }

  private:
    State state_{};
    Order orders_[Capacity]{};
    uint16_t dirty{};
    uint8_t len_{};

    void in(Order order) {
        const uint16_t mask = static_cast<uint16_t>(order);
        if (dirty & mask) {
            for (uint8_t i = 0; i < len_; ++i) {
                if (orders_[i] == order) {
                    for (uint8_t j = i + 1; j < len_; ++j) {
                        orders_[j - 1] = orders_[j];
                    }
                    len_--;
                    break;
                }
            }
        }
        orders_[len_] = order;
        len_++;
        dirty |= mask;
    }
    void order_reset() {
        len_ = 0;
        dirty = 0;
    }

    auto generate_order_str(char *pos, char *end) const -> char * {
        if (len_ == 0) {
            return pos;
        }
        pos = append_lit(pos, end, "\033[");
        for (uint8_t i = 0; i < len_; ++i) {
            if (i != 0) {
                pos = append_lit(pos, end, ";");
            }
            pos = write_order(orders_[i], pos, end);
        }
        pos = append_lit(pos, end, "m");
        return pos;
    }

    auto write_order(Order order, char *pos, char *end) const -> char * {
        // clang-format off
        switch (order) {
			case Order::Clear: return append_bytes(pos, end, ClearStr);
			case Order::Weight: return append_bytes(pos, end, get_weight_str(state_.weight));
			case Order::Italic: return state_.italic ? append_bytes(pos, end, ItalicStr) : append_bytes(pos, end, ClearItalicStr);
			case Order::UnderLine: return state_.underline ? append_bytes(pos, end, UnderLineStr) : append_bytes(pos, end, ClearUnderLineStr);
			case Order::Shining: return append_bytes(pos, end, get_shining_str(state_.shining));
			case Order::Reverse: return state_.reverse ? append_bytes(pos, end, ReverseStr) : append_bytes(pos, end, ClearReverseStr);
			case Order::Hide: return state_.hidden ? append_bytes(pos, end, HideStr) : append_bytes(pos, end, ClearHideStr);
			case Order::DeleteLine: return state_.deleteline ? append_bytes(pos, end, DeleteLineStr) : append_bytes(pos, end, ClearDeleteLineStr);
			case Order::FgColor: return state_.fg_color.to_buf<Color::Type::Foreground>(pos, end);
			case Order::BgColor: return state_.bg_color.to_buf<Color::Type::Background>(pos, end);
			case Order::UlColor: return state_.ul_color.to_buf<Color::Type::Underline>(pos, end);
			case Order::Border: return append_bytes(pos, end, get_border_str(state_.border));
			case Order::UpperLine: return state_.upperline ? append_bytes(pos, end, UpperLineStr) : append_bytes(pos, end, ClearUpperLineStr);
        }
        // clang-format on

        return pos; // this entry will never been reach
    }

    auto generate_snapshot_str(char *pos, char *end) const -> char * {
        pos = append_lit(pos, end, "\033[0");
        if (state_.weight != Weight::Default) {
            pos = append_lit(pos, end, ";");
            pos = append_bytes(pos, end, get_weight_str(state_.weight));
        }
        if (state_.italic) {
            pos = append_lit(pos, end, ";");
            pos = append_bytes(pos, end, ItalicStr);
        }
        if (state_.underline) {
            pos = append_lit(pos, end, ";");
            pos = append_bytes(pos, end, UnderLineStr);
        }
        if (state_.shining != Shining::Default) {
            pos = append_lit(pos, end, ";");
            pos = append_bytes(pos, end, get_shining_str(state_.shining));
        }
        if (state_.reverse) {
            pos = append_lit(pos, end, ";");
            pos = append_bytes(pos, end, ReverseStr);
        }
        if (state_.hidden) {
            pos = append_lit(pos, end, ";");
            pos = append_bytes(pos, end, HideStr);
        }
        if (state_.deleteline) {
            pos = append_lit(pos, end, ";");
            pos = append_bytes(pos, end, DeleteLineStr);
        }
        if (state_.fg_color.mode() != ColorMode::Default) {
            pos = append_lit(pos, end, ";");
            pos = state_.fg_color.to_buf<Color::Type::Foreground>(pos, end);
        }
        if (state_.bg_color.mode() != ColorMode::Default) {
            pos = append_lit(pos, end, ";");
            pos = state_.bg_color.to_buf<Color::Type::Background>(pos, end);
        }
        if (state_.ul_color.mode() != ColorMode::Default) {
            pos = append_lit(pos, end, ";");
            pos = state_.ul_color.to_buf<Color::Type::Underline>(pos, end);
        }
        if (state_.border != Border::Default) {
            pos = append_lit(pos, end, ";");
            pos = append_bytes(pos, end, get_border_str(state_.border));
        }
        if (state_.upperline) {
            pos = append_lit(pos, end, ";");
            pos = append_bytes(pos, end, UpperLineStr);
        }
        pos = append_lit(pos, end, "m");
        return pos;
    }
};

} // namespace term_ctl
