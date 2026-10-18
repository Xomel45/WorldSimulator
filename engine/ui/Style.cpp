#include "ui/Style.hpp"

#include <cmath>

namespace wsim::style {

// Swapchain у нас VK_FORMAT_B8G8R8A8_SRGB: GPU сам кодирует записанное значение
// в sRGB, а ImGui отдаёт цвета уже как sRGB. Без обратного перевода в линейное
// пространство всё выходит светлее и бледнее, чем в макете. Поэтому каждый
// канал hex-цвета здесь переводится sRGB -> linear.
ImU32 rgba(unsigned hex, float a) {
    auto lin = [](unsigned v) {
        const float c = static_cast<float>(v) / 255.0f;
        const float l = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        return static_cast<int>(l * 255.0f + 0.5f);
    };
    return IM_COL32(lin((hex >> 16) & 0xFF), lin((hex >> 8) & 0xFF), lin(hex & 0xFF), static_cast<int>(a * 255.0f + 0.5f));
}

unsigned lerpHex(unsigned a, unsigned b, float t) {
    auto ch = [&](int shift) {
        const float x = static_cast<float>((a >> shift) & 0xFF);
        const float y = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<unsigned>(x + (y - x) * t);
    };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}


// Полоса между радиусами rIn и rOut с плавным переходом цвета (альфы) по краям.
// Из таких полос собираются мягкие свечения: ступенчатые AddCircle-кольца
// давали видимые полосы, градиент по вершинам -- нет.
void gradientRing(ImDrawList* dl, ImVec2 c, float rIn, float rOut, ImU32 colIn, ImU32 colOut, int segs) {
    const ImVec2 uv   = ImGui::GetFontTexUvWhitePixel();
    const unsigned base = dl->_VtxCurrentIdx;
    dl->PrimReserve(segs * 6, segs * 4);
    for (int s = 0; s < segs; ++s) {
        const float a0 = 6.2831853f * static_cast<float>(s) / static_cast<float>(segs);
        const float a1 = 6.2831853f * static_cast<float>(s + 1) / static_cast<float>(segs);
        const ImVec2 d0(std::cos(a0), std::sin(a0)), d1(std::cos(a1), std::sin(a1));
        dl->PrimWriteVtx({c.x + d0.x * rIn,  c.y + d0.y * rIn},  uv, colIn);
        dl->PrimWriteVtx({c.x + d0.x * rOut, c.y + d0.y * rOut}, uv, colOut);
        dl->PrimWriteVtx({c.x + d1.x * rOut, c.y + d1.y * rOut}, uv, colOut);
        dl->PrimWriteVtx({c.x + d1.x * rIn,  c.y + d1.y * rIn},  uv, colIn);
        const auto b = static_cast<ImDrawIdx>(base + static_cast<unsigned>(s) * 4);
        dl->PrimWriteIdx(b);     dl->PrimWriteIdx(b + 1); dl->PrimWriteIdx(b + 2);
        dl->PrimWriteIdx(b);     dl->PrimWriteIdx(b + 2); dl->PrimWriteIdx(b + 3);
    }
}

// Диск с мягким краем: альфа падает от a в центре до 0 на радиусе, по квадратичной кривой.
void radialGlow(ImDrawList* dl, ImVec2 c, float radius, unsigned hex, float a, int segs) {
    constexpr int kSteps = 6;
    auto at = [&](float t) { return rgba(hex, a * (1.0f - t) * (1.0f - t)); };
    for (int k = 0; k < kSteps; ++k) {
        const float t0 = static_cast<float>(k) / kSteps, t1 = static_cast<float>(k + 1) / kSteps;
        gradientRing(dl, c, radius * t0, radius * t1, at(t0), at(t1), segs);
    }
}

}  // namespace wsim::style
