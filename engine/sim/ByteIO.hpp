#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wsim {

// Двоичный буфер для сохранений: явный little-endian, поэтому файл одинаков на любой платформе.
class ByteWriter {
public:
    void u8(std::uint8_t v) { m_buf.push_back(v); }
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) m_buf.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    void u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) m_buf.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
    void f32(float v) { std::uint32_t b; std::memcpy(&b, &v, 4); u32(b); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void vec3(const glm::vec3& v) { f32(v.x); f32(v.y); f32(v.z); }
    void str(std::string_view s) { u32(static_cast<std::uint32_t>(s.size())); m_buf.insert(m_buf.end(), s.begin(), s.end()); }
    void floats(std::span<const float> v) { u32(static_cast<std::uint32_t>(v.size())); for (float f : v) f32(f); }
    void raw(std::span<const std::uint8_t> v) { m_buf.insert(m_buf.end(), v.begin(), v.end()); }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return m_buf; }
    [[nodiscard]] std::vector<std::uint8_t>        take() noexcept { return std::move(m_buf); }

private:
    std::vector<std::uint8_t> m_buf;
};

// Чтение с проверкой границ: любая попытка выйти за буфер или заказать слишком много -- ошибка
// (ok() == false), дальше все чтения возвращают нули. Так битый/подделанный файл не роняет игру.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> data) : m_data(data) {}

    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    [[nodiscard]] std::size_t remaining() const noexcept { return m_data.size() - m_pos; }
    void fail() noexcept { m_ok = false; }

    std::uint8_t u8() { return need(1) ? m_data[m_pos++] : 0; }
    std::uint32_t u32() {
        if (!need(4)) return 0;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(m_data[m_pos++]) << (8 * i);
        return v;
    }
    std::uint64_t u64() {
        if (!need(8)) return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(m_data[m_pos++]) << (8 * i);
        return v;
    }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    float f32() { const std::uint32_t b = u32(); float v; std::memcpy(&v, &b, 4); return v; }
    bool boolean() { return u8() != 0; }
    glm::vec3 vec3() { const float x = f32(), y = f32(), z = f32(); return {x, y, z}; }
    std::string str(std::size_t maxLen = 4096) {
        const std::uint32_t n = u32();
        if (n > maxLen || !need(n)) { m_ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(m_data.data() + m_pos), n);
        m_pos += n;
        return s;
    }
    // Счётчик элементов с потолком: заказанное больше max или больше, чем осталось байт на такие элементы, -- ошибка.
    std::uint32_t count(std::size_t max, std::size_t minBytesPerItem) {
        const std::uint32_t n = u32();
        if (n > max || static_cast<std::size_t>(n) * minBytesPerItem > remaining()) { m_ok = false; return 0; }
        return n;
    }
    std::vector<float> floats(std::size_t max) {
        const std::uint32_t n = count(max, 4);
        std::vector<float> v;
        v.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) v.push_back(f32());
        return v;
    }
    std::span<const std::uint8_t> bytes(std::size_t n) {
        if (!need(n)) return {};
        const auto s = m_data.subspan(m_pos, n);
        m_pos += n;
        return s;
    }

private:
    bool need(std::size_t n) {
        if (!m_ok || m_pos + n > m_data.size()) { m_ok = false; return false; }
        return true;
    }
    std::span<const std::uint8_t> m_data;
    std::size_t                   m_pos = 0;
    bool                          m_ok  = true;
};

// CRC-32 (IEEE) для проверки целостности файла.
[[nodiscard]] inline std::uint32_t crc32(std::span<const std::uint8_t> data) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::uint8_t b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

}  // namespace wsim
