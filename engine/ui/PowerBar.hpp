#pragma once

#include "ui/MainMenu.hpp"  // MenuFonts
#include "ui/UiSound.hpp"

#include <functional>

#include <span>
#include <string>
#include <vector>

namespace wsim {

struct PowerBarItem {
    std::string name;
    std::string description;
    std::string source;  // "base" или id мода -- у моддерских сил под названием мелкая пометка
};

// Панель сил бога внизу экрана в игре: ряд "таблеток" с горячими клавишами 1..9,
// ползунок радиуса кисти. Тот же язык, что у меню: выбранная сила светится и
// подчёркнута волнистой изолинией, остальные приглушены. Ничего не знает про
// sim -- получает только названия, возвращает выбор и радиус.
class PowerBar {
public:
    explicit PowerBar(MenuFonts fonts) : m_fonts(fonts) {}
    void setSoundCallback(std::function<void(UiSound)> cb) { m_sfx = std::move(cb); }

    // Рисует панель; возвращает true, пока курсор над ней (тогда мир кистью не трогаем).
    [[nodiscard]] bool draw(float dt, std::span<const PowerBarItem> items, int& selected, float& radius, float minRadius,
                            float maxRadius);

private:
    MenuFonts          m_fonts;
    std::vector<float> m_focus;
    std::function<void(UiSound)> m_sfx;
    int                m_prevHover = -1;
    float              m_tip = 0.0f;
    int                m_tipItem = -1;
};

}  // namespace wsim
