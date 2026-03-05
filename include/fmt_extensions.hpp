#pragma once

#include <fmt/format.h>
#include <fmt/ranges.h>   // ← достаточно для 95% случаев: vector, array, map и т.д.

#include <vector>
#include <array>

// ─── Константы (можно вынести в Logger как static constexpr) ────────────────
constexpr size_t MAX_VISIBLE = 8;           // первые + последние
constexpr size_t MULTILINE_THRESHOLD = 15;  // после этого — вертикальный вывод

// ─── Универсальный formatter для любого range ────────────────────────────────
// (fmt/ranges.h уже делает базовое [1, 2, 3], мы добавляем обрезку и красоту)
template<std::ranges::range R>
    requires (!std::same_as<std::ranges::range_value_t<R>, char> &&
              !std::same_as<std::ranges::range_value_t<R>, wchar_t>)
struct fmt::formatter<R> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

    template<typename FormatContext>
    auto format(const R& r, FormatContext& ctx) const {
        auto out = ctx.out();
        auto sz = std::ranges::size(r);

        // Префикс: тип + размер
        using Val = std::ranges::range_value_t<R>;
        out = fmt::format_to(out, "{}({}): ", typeid(Val).name(), sz);

        if (sz == 0) return fmt::format_to(out, "[]");

        out = fmt::format_to(out, "[");

        if (sz <= MAX_VISIBLE) {
            // Полностью
            bool first = true;
            for (const auto& e : r) {
                if (!first) out = fmt::format_to(out, ", ");
                out = fmt::format_to(out, "{}", e);
                first = false;
            }
        } else {
            // Обрезка: начало + ... + конец
            auto it = r.begin();
            for (size_t i = 0; i < MAX_VISIBLE / 2; ++i, ++it) {
                if (i > 0) out = fmt::format_to(out, ", ");
                out = fmt::format_to(out, "{}", *it);
            }
            out = fmt::format_to(out, ", ..., ");
            auto rit = r.rbegin();
            for (size_t i = 0; i < MAX_VISIBLE / 2; ++i, ++rit) {
                if (i > 0) out = fmt::format_to(out, ", ");
                out = fmt::format_to(out, "{}", *rit);
            }
        }

        return fmt::format_to(out, "]");
    }
};

// ─── Опционально: многострочный режим для очень больших ──────────────────────
#if 0  // включи, если хочешь
template<std::ranges::range R>
struct fmt::formatter<R> {
    // ... тот же parse ...

    template<typename FormatContext>
    auto format(const R& r, FormatContext& ctx) const {
        auto out = ctx.out();
        auto sz = std::ranges::size(r);
        using Val = std::ranges::range_value_t<R>;

        out = fmt::format_to(out, "{}({}):\n", typeid(Val).name(), sz);

        size_t i = 0;
        for (const auto& e : r) {
            out = fmt::format_to(out, "  [{:3}] {}\n", i++, e);
        }
        return out;
    }
};
#endif