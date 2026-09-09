#pragma once

// Компайл-тайм XOR-обфускация строковых литералов.
//
// В .rodata бинаря лежит только шифротекст; открытый текст материализуется
// лишь временно, в буфер вызывающего. Защищает от статического `strings` (и,
// частично, от дампа памяти, если сразу затирать буфер). Ключ выводится из
// позиции символа — на каждый байт свой, паттерн не виден.
//
// Использование:
//   static constexpr wsim::ObfString k{"license-key-XXXX"};
//   char buf[decltype(k)::size()];
//   doThing(k.decrypt(buf));           // const char*, живёт пока жив buf
//   std::fill(std::begin(buf), std::end(buf), 0);   // затереть после использования
//
// Замечание: для НЕ секретных строк (заголовок окна, UI) смысла нет — это против
// реверса чувствительных литералов (ключи, эндпоинты, анти-тампер маркеры).

#include <array>
#include <cstddef>

namespace wsim {

template <std::size_t N>
class ObfString {
public:
    consteval explicit ObfString(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i)
            m_enc[i] = static_cast<char>(s[i] ^ key(i));
    }

    // Расшифровать во внешний буфер (>= size() байт); возвращает его же.
    // Владелец буфера отвечает за время жизни и, при желании, затирание.
    const char* decrypt(char* out) const {
        for (std::size_t i = 0; i < N; ++i)
            out[i] = static_cast<char>(m_enc[i] ^ key(i));
        return out;
    }

    [[nodiscard]] static constexpr std::size_t size() noexcept { return N; }

private:
    // Позиционный ключ: на каждый индекс свой байт (не единый XOR-паттерн).
    static constexpr char key(std::size_t i) noexcept {
        return static_cast<char>(0xA5u ^ (i * 131u + 13u));
    }

    std::array<char, N> m_enc{};
};

}  // namespace wsim
