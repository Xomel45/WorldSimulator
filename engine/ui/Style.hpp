#pragma once

#include <imgui.h>

namespace wsim::style {

// Общая палитра и примитивы свечения для всех экранов в стиле "Биолюминесценция"
// (главное меню, настройки, панель сил бога). Цвета задаются hex из макета.
ImU32 rgba(unsigned hex, float a = 1.0f);
unsigned lerpHex(unsigned a, unsigned b, float t);

inline constexpr unsigned kLeftBg  = 0x04100f;
inline constexpr unsigned kRightBg = 0x071a19;
inline constexpr unsigned kTopo    = 0x0f4d45;
inline constexpr unsigned kDishBg  = 0x031312;
inline constexpr unsigned kGlow    = 0x2ee6b0;
inline constexpr unsigned kMint    = 0x7dffc4;
inline constexpr unsigned kText    = 0xc9fff0;
inline constexpr unsigned kDim     = 0x4aa592;

// Полоса между радиусами с плавным переходом альфы: основа мягких свечений.
void gradientRing(ImDrawList* dl, ImVec2 c, float rIn, float rOut, ImU32 colIn, ImU32 colOut, int segs);
// Диск с мягким краем: альфа падает от a в центре до 0 на радиусе.
void radialGlow(ImDrawList* dl, ImVec2 c, float radius, unsigned hex, float a, int segs);

}  // namespace wsim::style
