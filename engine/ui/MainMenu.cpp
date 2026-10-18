#include "ui/MainMenu.hpp"

#include "ui/Settings.hpp"
#include "ui/Style.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <glm/geometric.hpp>
#include <vector>

namespace wsim {

using namespace wsim::style;

namespace {

struct ItemDef {
    const char* label;
    const char* hint;  // пометка справа от недоступного пункта
};

constexpr ItemDef kItems[] = {
    {"Новый мир", nullptr},
    {"Продолжить", nullptr},
    {"Сохранить", nullptr},
    {"Загрузить", nullptr},
    {"Настройки", nullptr},
    {"Выйти", nullptr},
};
constexpr int kNewWorld = 0, kContinue = 1, kSave = 2, kLoad = 3, kSettings = 4, kQuit = 5;

// Изолинии из макета (viewBox 160x90, "slice"): кольца холма и три береговые линии.
void drawTopo(ImDrawList* dl, ImVec2 size) {
    const float s  = std::max(size.x / 160.0f, size.y / 90.0f);
    const float ox = (size.x - 160.0f * s) * 0.5f;
    const float oy = (size.y - 90.0f * s) * 0.5f;
    auto P = [&](float x, float y) { return ImVec2(ox + x * s, oy + y * s); };
    const ImU32 col = rgba(kTopo);
    const float th  = std::max(1.0f, 0.25f * s * 0.6f);

    struct Ring { float cx, cy, rx, ry; };
    constexpr Ring rings[] = {{108, 46, 52, 34}, {108, 46, 44, 28}, {106, 45, 36, 22},
                              {104, 44, 28, 16}, {103, 43, 20, 10}, {102, 42, 12, 5.5f}};
    for (const auto& r : rings) dl->AddEllipse(P(r.cx, r.cy), ImVec2(r.rx * s, r.ry * s), col, 0.0f, 96, th);

    auto curve = [&](float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3) {
        dl->AddBezierCubic(P(x0, y0), P(x1, y1), P(x2, y2), P(x3, y3), col, th, 48);
    };
    curve(0, 70, 30, 60, 50, 80, 90, 72);   curve(90, 72, 130, 64, 140, 84, 160, 74);
    curve(0, 78, 30, 68, 50, 88, 90, 80);   curve(90, 80, 130, 72, 140, 90, 160, 82);
    curve(0, 18, 30, 8, 60, 24, 100, 12);   curve(100, 12, 140, 0, 140, 14, 160, 6);
}

void drawGlowText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, const char* text, unsigned color,
                  float glowStrength, float glowRadius) {
    if (glowStrength > 0.01f) {
        constexpr int kDirs = 8;
        for (int ring = 1; ring <= 2; ++ring) {
            const float rad = glowRadius * static_cast<float>(ring);
            for (int d = 0; d < kDirs; ++d) {
                const float a = 6.2831853f * static_cast<float>(d) / kDirs;
                dl->AddText(font, size, ImVec2(pos.x + std::cos(a) * rad, pos.y + std::sin(a) * rad), rgba(kGlow, 0.10f * glowStrength / static_cast<float>(ring)), text);
            }
        }
    }
    dl->AddText(font, size, pos, rgba(color), text);
}

}  // namespace

bool MainMenu::enabled(int index, bool canContinue) const noexcept {
    return index == kNewWorld || index == kLoad || index == kSettings || index == kQuit || ((index == kContinue || index == kSave) && canContinue);
}

int MainMenu::step(int from, int dir, bool canContinue) const noexcept {
    int i = from;
    for (int n = 0; n < kItemCount; ++n) {
        i = (i + dir + kItemCount) % kItemCount;
        if (enabled(i, canContinue)) return i;
    }
    return from;
}

MenuAction MainMenu::draw(float dt, bool canContinue, std::span<const glm::vec2> colony, Settings& settings,
                          std::span<ModRow> mods, std::span<SaveRow> saves) {
    // Выбора нет, пока мышь не навелась на пункт или не нажата стрелка: тогда
    // ничего не подсвечено и остальные пункты не приглушены.
    if (!m_initialised || canContinue != m_lastCanContinue) {
        m_sel = -1;
        m_lastCanContinue = canContinue;
        m_initialised = true;
    }

    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float  u    = size.x * 0.01f;  // 1% ширины, как cqw в макете
    MenuAction   action = MenuAction::None;

    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##mainmenu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ── Фон: тёмная карта, поверх неё светлая половина по диагонали ──────────
    dl->AddRectFilled({0, 0}, size, rgba(kLeftBg));
    drawTopo(dl, size);
    {
        const ImVec2 quad[] = {{size.x * 0.52f, 0}, {size.x, 0}, {size.x, size.y}, {size.x * 0.44f, size.y}};
        dl->AddConvexPolyFilled(quad, 4, rgba(kRightBg));
    }

    if (m_page != Page::Main) {
        m_pendingAction = MenuAction::None;
        drawSettings(dl, size, dt, settings, mods, saves);
        ImGui::End();
        ImGui::PopStyleVar(2);
        return m_pendingAction;
    }

    // ── Чашка с колонией ────────────────────────────────────────────────────
    const float  R = 20.0f * u;
    const ImVec2 c(size.x - 25.0f * u, size.y * 0.5f);
    dl->AddCircleFilled(c, R, rgba(kDishBg), 128);
    // Мягкий обод: свечение внутрь и наружу плюс тонкая линия с размытыми краями.
    gradientRing(dl, c, R - 3.0f * u, R, rgba(kGlow, 0.0f), rgba(kGlow, 0.22f), 128);
    gradientRing(dl, c, R, R + 3.0f * u, rgba(kGlow, 0.14f), rgba(kGlow, 0.0f), 128);
    gradientRing(dl, c, R - 0.35f * u, R, rgba(kGlow, 0.0f), rgba(kGlow, 0.55f), 128);
    gradientRing(dl, c, R, R + 0.35f * u, rgba(kGlow, 0.55f), rgba(kGlow, 0.0f), 128);

    // Курсор внутри чашки -- угроза: существа сами убегают от него с инерцией
    // (разгоняются, по-разному пугливы, скользят вдоль обода), а когда страх
    // проходит, неспешно возвращаются на свои места. Это только оформление
    // меню: симуляция и мозг существ тут не участвуют.
    m_runners.resize(colony.size());
    const float  sdt = std::min(dt, 0.05f);
    const ImVec2 mp  = ImGui::GetIO().MousePos;
    const bool   mouseInDish = mp.x > -1.0e6f && std::hypot(mp.x - c.x, mp.y - c.y) < R;
    for (std::size_t i = 0; i < colony.size(); ++i) {
        const float  r = u * (0.6f + 0.7f * std::fmod(static_cast<float>(i) * 0.37f, 1.0f));
        // Круглого clip-а в ImGui нет (только прямоугольный), поэтому радиус
        // размещения урезается так, чтобы внешний ореол (3r) целиком лежал
        // внутри обода и не вылезал за "лупу".
        const float  reach = std::max(0.0f, R - r * 3.0f - 0.4f * u);
        const ImVec2 base(c.x + colony[i].x * reach, c.y + colony[i].y * reach);
        Runner&      run = m_runners[i];

        const float nerve = std::fmod(static_cast<float>(i) * 0.61f, 1.0f);  // 0 -- трусишка, 1 -- хладнокровный
        ImVec2 p(base.x + run.off.x * u, base.y + run.off.y * u);

        // Страх: быстро нарастает у курсора, медленно спадает.
        const float seeRadius = (6.0f + 5.0f * (1.0f - nerve)) * u;
        float       threat    = 0.0f;
        glm::vec2   away(0.0f);
        if (mouseInDish) {
            const float vx = p.x - mp.x, vy = p.y - mp.y, dist = std::hypot(vx, vy);
            if (dist < seeRadius) {
                threat = 1.0f - dist / seeRadius;
                away   = dist > 1.0e-3f ? glm::vec2(vx, vy) / dist : glm::vec2(std::cos(static_cast<float>(i)), std::sin(static_cast<float>(i)));
                // Каждый бежит чуть под своим углом, а не строго от курсора: стая рассыпается.
                const float ang = (nerve - 0.5f) * 0.9f, ca = std::cos(ang), sa = std::sin(ang);
                away = glm::vec2(away.x * ca - away.y * sa, away.x * sa + away.y * ca);
            }
        }
        run.fear += (threat - run.fear) * (1.0f - std::exp(-sdt * (threat > run.fear ? 12.0f : 2.0f)));

        // Бег: ускорение от курсора; без страха -- пружина домой и сильное трение.
        const float maxSpeed = 16.0f * (0.75f + 0.5f * (1.0f - nerve));
        run.vel += away * (run.fear * 90.0f) * sdt;
        if (run.fear < 0.08f) run.vel += -run.off * (2.5f * sdt);
        run.vel *= std::exp(-sdt * (run.fear > 0.1f ? 1.2f : 3.0f));
        if (const float sp = glm::length(run.vel); sp > maxSpeed) run.vel *= maxSpeed / sp;
        run.off += run.vel * sdt;
        p = ImVec2(base.x + run.off.x * u, base.y + run.off.y * u);

        // Обод: не пробиваем, а скользим вдоль него (гасим только скорость наружу).
        const float ox = p.x - c.x, oy = p.y - c.y, len = std::hypot(ox, oy);
        if (len > reach && len > 1.0e-3f) {
            const glm::vec2 n(ox / len, oy / len);
            p = ImVec2(c.x + n.x * reach, c.y + n.y * reach);
            run.off = glm::vec2(p.x - base.x, p.y - base.y) / u;
            run.vel -= n * std::max(0.0f, glm::dot(run.vel, n));
        }
        const float panic = run.fear;
        radialGlow(dl, p, r * 3.0f, kMint, 0.20f + 0.15f * panic, 24);
        radialGlow(dl, p, r * 1.25f, kMint, 0.65f, 24);
        radialGlow(dl, p, r * 0.5f, kMint, 1.0f, 16);
    }

    // ── Название и подпись ──────────────────────────────────────────────────
    const float titleSize = std::max(24.0f, 3.9f * u);
    drawGlowText(dl, m_fonts.title, titleSize, {5.0f * u, 6.0f * u}, "World", kText, 1.0f, 0.12f * u);
    drawGlowText(dl, m_fonts.title, titleSize, {5.0f * u, 6.0f * u + titleSize * 1.02f}, "Simulator", kText, 1.0f, 0.12f * u);
    {
        char tag[48];
        std::snprintf(tag, sizeof tag, "популяция %zu", colony.size());
        const float ts = std::max(11.0f, 1.0f * u);
        const ImVec2 tw = m_fonts.ui->CalcTextSizeA(ts, FLT_MAX, 0.0f, tag);
        dl->AddText(m_fonts.ui, ts, {size.x - 6.0f * u - tw.x, 6.0f * u}, rgba(kDim), tag);
    }

    // ── Пункты списка ───────────────────────────────────────────────────────
    const float baseSize = std::max(14.0f, 2.0f * u);
    const float itemH    = 3.4f * u;
    const float stepY    = 3.9f * u;
    const float top      = size.y - 6.0f * u - (kItemCount * itemH + (kItemCount - 1) * 0.5f * u);

    // Клавиатура: стрелки/W/S двигают выбор, Enter/Пробел подтверждает.
    // Из состояния "ничего не выбрано" вниз идём к первому пункту, вверх -- к последнему.
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_S)) m_sel = step(m_sel < 0 ? -1 : m_sel, +1, canContinue);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_W)) m_sel = step(m_sel < 0 ? 0 : m_sel, -1, canContinue);
    const bool keyConfirm = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) || ImGui::IsKeyPressed(ImGuiKey_Space);
    const bool mouseMoved = ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f;

    bool anyHover = false;
    m_dim += ((m_sel >= 0 ? 1.0f : 0.0f) - m_dim) * (1.0f - std::exp(-dt * 14.0f));
    for (int i = 0; i < kItemCount; ++i) {
        const bool   on = enabled(i, canContinue);
        const float  y0 = top + stepY * static_cast<float>(i);
        const ImVec2 baseText = m_fonts.ui->CalcTextSizeA(baseSize, FLT_MAX, 0.0f, kItems[i].label);
        const float  hitW = baseText.x * 1.12f + 3.0f * u;

        ImGui::SetCursorScreenPos({5.0f * u, y0});
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##item", {hitW, itemH}) && on;
        if (on && ImGui::IsItemHovered()) {
            anyHover = true;
            if (mouseMoved) m_sel = i;
        }
        ImGui::PopID();
        if (clicked || (keyConfirm && on && m_sel == i)) {
            sfx(UiSound::Click);
            if (i == kSettings) { m_page = Page::Settings; m_tab = 0; }
            else if (i == kLoad) { m_page = Page::Load; m_saveScroll = 0; m_confirmDelete = -1; }
            else if (i == kSave) action = MenuAction::Save;
            else if (i == kNewWorld) action = MenuAction::NewWorld;
            else if (i == kContinue) action = MenuAction::Continue;
            else if (i == kQuit) action = MenuAction::Quit;
        }

        float& t = m_focus[static_cast<std::size_t>(i)];
        t += ((m_sel == i && on ? 1.0f : 0.0f) - t) * (1.0f - std::exp(-dt * 14.0f));

        const float  size_i = baseSize * (1.0f + 0.12f * t);
        const ImVec2 ts     = m_fonts.ui->CalcTextSizeA(size_i, FLT_MAX, 0.0f, kItems[i].label);
        const ImVec2 pos(5.0f * u, y0 + itemH * 0.5f - ts.y * 0.5f);
        // Остальные пункты тускнеют только пока что-то выбрано (m_dim плавно 0..1).
        const float  alpha = (1.0f - 0.7f * m_dim * (1.0f - t)) * (on ? 1.0f : 0.55f);

        const unsigned color = lerpHex(kText, kMint, t);
        if (t > 0.01f)
            for (int ring = 1; ring <= 2; ++ring)
                for (int d = 0; d < 8; ++d) {
                    const float a = 0.785398f * static_cast<float>(d), rad = 0.12f * u * static_cast<float>(ring);
                    dl->AddText(m_fonts.ui, size_i, {pos.x + std::cos(a) * rad, pos.y + std::sin(a) * rad}, rgba(kMint, 0.10f * t * alpha / static_cast<float>(ring)), kItems[i].label);
                }
        dl->AddText(m_fonts.ui, size_i, pos, rgba(color, alpha), kItems[i].label);

        // Волнистая изолиния под выбранным пунктом, проявляется слева направо.
        if (t > 0.01f) {
            const float x0 = pos.x, wMax = ts.x, w = wMax * t, yb = pos.y + ts.y + 0.2f * u;
            const float wavelen = 3.2f * u, amp = 0.25f * u;
            std::vector<ImVec2> pts;
            for (float x = 0.0f; x < w; x += 3.0f) pts.emplace_back(x0 + x, yb + std::sin(x / wavelen * 6.2831853f) * amp);
            pts.emplace_back(x0 + w, yb + std::sin(w / wavelen * 6.2831853f) * amp);
            if (pts.size() >= 2) dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), rgba(kMint, alpha), ImDrawFlags_None, std::max(1.5f, 0.18f * u));
        }

        if (!on && kItems[i].hint) {
            const float hs = std::max(10.0f, 1.0f * u);
            dl->AddText(m_fonts.ui, hs, {pos.x + ts.x + 1.2f * u, y0 + itemH * 0.5f - hs * 0.5f}, rgba(kDim, 0.8f), kItems[i].hint);
        }
    }

    // Курсор сдвинулся и ушёл с пунктов -- выбор снимается (подсветка и линия гаснут).
    if (mouseMoved && !anyHover) m_sel = -1;
    if (m_sel >= 0 && m_sel != m_prevSel) sfx(UiSound::Hover);
    m_prevSel = m_sel;

    ImGui::End();
    ImGui::PopStyleVar(2);
    return action;
}

// ─── Страница "Настройки" ─────────────────────────────────────────────────────
// Та же анимация, что у пунктов главного меню: наведённый элемент вырастает,
// светится и получает волнистую изолинию слева направо, остальные тускнеют.
// Переключатели, сегменты и ползунки не щёлкают, а плавно переезжают (m_anim).
void MainMenu::drawSettings(ImDrawList* dl, ImVec2 size, float dt, Settings& st, std::span<ModRow> mods, std::span<SaveRow> saves) {
    const bool isLoad = m_page == Page::Load;
    const float  u  = size.x * 0.01f;
    ImGuiIO&     io = ImGui::GetIO();
    const ImVec2 mp = io.MousePos;
    const bool   mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    // Плавное приближение значения к цели; первый раз сразу встаёт на цель (без разгона из нуля).
    auto anim = [&](unsigned key, float target, float rate = 14.0f) {
        auto [it, fresh] = m_anim.try_emplace(key, target);
        float& v = it->second;
        if (!fresh) v += (target - v) * (1.0f - std::exp(-dt * rate));
        return v;
    };
    auto mixHex = [](unsigned a, unsigned b, float t) { return lerpHex(a, b, std::clamp(t, 0.0f, 1.0f)); };

    // Текст с ореолом (как у выбранного пункта меню) и волнистая изолиния под ним.
    auto glowLabel = [&](float fs, ImVec2 pos, const char* text, float t, float alpha, unsigned base) {
        if (t > 0.01f)
            for (int ring = 1; ring <= 2; ++ring)
                for (int d = 0; d < 8; ++d) {
                    const float a = 0.785398f * static_cast<float>(d), rad = 0.12f * u * static_cast<float>(ring);
                    dl->AddText(m_fonts.ui, fs, {pos.x + std::cos(a) * rad, pos.y + std::sin(a) * rad}, rgba(kMint, 0.10f * t * alpha / static_cast<float>(ring)), text);
                }
        dl->AddText(m_fonts.ui, fs, pos, rgba(mixHex(base, kMint, t), alpha), text);
    };
    auto isoline = [&](float x0, float yb, float wMax, float t, float alpha) {
        if (t <= 0.01f) return;
        const float w = wMax * t, wavelen = 3.2f * u, amp = 0.22f * u;
        std::vector<ImVec2> pts;
        for (float x = 0.0f; x < w; x += 3.0f) pts.emplace_back(x0 + x, yb + std::sin(x / wavelen * 6.2831853f) * amp);
        pts.emplace_back(x0 + w, yb + std::sin(w / wavelen * 6.2831853f) * amp);
        if (pts.size() >= 2) dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), rgba(kMint, alpha), ImDrawFlags_None, std::max(1.5f, 0.16f * u));
    };

    // Диагональная кромка со свечением.
    dl->AddLine({size.x * 0.52f, 0}, {size.x * 0.44f, size.y}, rgba(kGlow, 0.10f), std::max(4.0f, 0.6f * u));
    dl->AddLine({size.x * 0.52f, 0}, {size.x * 0.44f, size.y}, rgba(kGlow, 0.35f), std::max(1.0f, 0.1f * u));

    // ── Левая колонка: заголовок и разделы ──────────────────────────────────
    const float titleSize = std::max(22.0f, 3.3f * u);
    drawGlowText(dl, m_fonts.title, titleSize, {6.6f * u, 5.8f * u}, isLoad ? "Загрузить" : "Настройки", kText, 1.0f, 0.12f * u);
    dl->AddText(m_fonts.ui, std::max(11.0f, 1.1f * u), {6.6f * u, 5.8f * u + titleSize * 1.05f + 1.0f * u},
                rgba(kDim), isLoad ? "Новые сначала; игра сохраняется сама" : "Применяются сразу и сохраняются автоматически");

    static constexpr const char* kTabs[] = {"Графика", "Качество", "Управление", "Звук", "Интерфейс", "Моды"};
    constexpr int kTabCount = 6;
    constexpr int kGfxTab = 0, kQualityTab = 1, kCtlTab = 2, kSoundTab = 3, kUiTab = 4, kModsTab = 5, kModsVisible = 5;
    if (!isLoad && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_S))) m_tab = (m_tab + 1) % kTabCount;
    if (!isLoad && (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_W)))   m_tab = (m_tab - 1 + kTabCount) % kTabCount;

    const float tabSize = std::max(15.0f, 2.1f * u), tabTop = 16.0f * u, tabPitch = 4.4f * u;
    for (int i = 0; i < (isLoad ? 0 : kTabCount); ++i) {
        const float y0 = tabTop + tabPitch * static_cast<float>(i);
        ImGui::SetCursorScreenPos({6.6f * u, y0});
        ImGui::PushID(100 + i);
        if (ImGui::InvisibleButton("##tab", {24.0f * u, tabPitch - 0.4f * u})) { if (m_tab != i) sfx(UiSound::Click); m_tab = i; }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && m_prevTabHover != i) { sfx(UiSound::Hover); m_prevTabHover = i; }
        else if (!hovered && m_prevTabHover == i) m_prevTabHover = -1;
        ImGui::PopID();

        const float t     = anim(0x1000u + static_cast<unsigned>(i), m_tab == i ? 1.0f : (hovered ? 0.7f : 0.0f));
        const float fs    = tabSize * (1.0f + 0.12f * t);
        const ImVec2 ts   = m_fonts.ui->CalcTextSizeA(fs, FLT_MAX, 0.0f, kTabs[i]);
        const float alpha = 0.55f + 0.45f * t;
        const ImVec2 pos(6.6f * u, y0 + (tabPitch - 0.4f * u) * 0.5f - ts.y * 0.5f);
        glowLabel(fs, pos, kTabs[i], t, alpha, kText);
        isoline(pos.x, pos.y + ts.y + 0.2f * u, ts.x, t, alpha);
    }
    dl->AddText(m_fonts.ui, std::max(11.0f, 1.05f * u), {6.6f * u, size.y - 6.0f * u - 1.4f * u}, rgba(kDim), "Esc — назад в меню");

    // ── Правая колонка: строки параметров ───────────────────────────────────
    const float x0 = 54.9f * u, W = 38.9f * u, xr = x0 + W;
    const float rowH = 6.6f * u, lblSize = std::max(13.0f, 1.39f * u), hintSize = std::max(10.5f, 0.97f * u);
    const float rowTop = 5.8f * u + (isLoad || m_tab == kSoundTab || m_tab == kModsTab ? 3.6f * u : 0.0f);
    const int   modCount = static_cast<int>(mods.size()), saveCount = static_cast<int>(saves.size());
    constexpr int rowCounts[kTabCount] = {5, 4, 4, 4, 3, 0};
    const int rows = isLoad ? std::min(kModsVisible, saveCount) : (m_tab == kModsTab ? std::min(kModsVisible, modCount) : rowCounts[m_tab]);
    if (isLoad && saveCount > kModsVisible && mp.x > x0 - 2.0f * u && io.MouseWheel != 0.0f) m_saveScroll -= io.MouseWheel > 0.0f ? 1 : -1;
    m_saveScroll = std::clamp(m_saveScroll, 0, std::max(0, saveCount - kModsVisible));
    // Колесо над правой колонкой листает моды, если их больше, чем помещается.
    if (m_tab == kModsTab && modCount > kModsVisible && mp.x > x0 - 2.0f * u && io.MouseWheel != 0.0f)
        m_modScroll -= io.MouseWheel > 0.0f ? 1 : -1;
    m_modScroll = std::clamp(m_modScroll, 0, std::max(0, modCount - kModsVisible));

    if (!mouseDown) m_dragRow = -1;
    int hot = m_dragRow;
    if (hot < 0 && mp.x > x0 - 1.0f * u && mp.x < xr + 1.0f * u && mp.y >= rowTop && mp.y < rowTop + rowH * static_cast<float>(rows))
        hot = static_cast<int>((mp.y - rowTop) / rowH);
    m_rowDim += ((hot >= 0 ? 1.0f : 0.0f) - m_rowDim) * (1.0f - std::exp(-dt * 14.0f));
    if (hot >= 0 && hot != m_prevHot && m_dragRow < 0) sfx(UiSound::Hover);
    m_prevHot = hot;

    struct Row { int idx; unsigned key; float yc, alpha, t; };
    // Строка, не влезающая в ширину, обрезается с многоточием.
    auto fit = [&](std::string text, float maxW, float fs) {
        if (m_fonts.ui->CalcTextSizeA(fs, FLT_MAX, 0.0f, text.c_str()).x <= maxW) return text;
        while (!text.empty()) {
            do text.pop_back(); while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80);  // не рвём UTF-8
            if (m_fonts.ui->CalcTextSizeA(fs, FLT_MAX, 0.0f, (text + "…").c_str()).x <= maxW) break;
        }
        return text + "…";
    };
    auto rowBegin = [&](int idx, const char* label, const char* hint, unsigned hintColor = kDim) {
        const unsigned key = 0x2000u + static_cast<unsigned>(isLoad ? 15 : m_tab) * 16u + static_cast<unsigned>(idx);
        const float    y   = rowTop + rowH * static_cast<float>(idx);
        const float    t   = anim(key, hot == idx ? 1.0f : 0.0f);
        const float    alpha = 1.0f - 0.5f * m_rowDim * (1.0f - t);
        const float    fs  = lblSize * (1.0f + 0.07f * t);
        const ImVec2   pos(x0, y + 1.5f * u);
        const ImVec2   ts = m_fonts.ui->CalcTextSizeA(fs, FLT_MAX, 0.0f, label);
        glowLabel(fs, pos, label, t, alpha, kText);
        isoline(pos.x, pos.y + ts.y + 0.12f * u, ts.x, t, alpha);
        dl->AddText(m_fonts.ui, hintSize, {x0, pos.y + lblSize * 1.2f + 0.8f * u}, rgba(hintColor, alpha), hint);
        dl->AddLine({x0, y + rowH}, {xr, y + rowH}, rgba(0x0f2f2b, alpha), 1.0f);
        return Row{idx, key * 64u, y + rowH * 0.5f, alpha, t};
    };

    auto toggle = [&](const Row& r, bool& v, bool enabled = true) {
        const float w = 3.9f * u, h = 2.1f * u;
        const ImVec2 p0(xr - w, r.yc - h * 0.5f), p1(xr, r.yc + h * 0.5f);
        ImGui::SetCursorScreenPos(p0);
        ImGui::PushID(static_cast<int>(r.key));
        const bool clicked = ImGui::InvisibleButton("##tg", {w, h}) && enabled;
        const bool hovered = ImGui::IsItemHovered() && enabled;
        ImGui::PopID();
        if (clicked) { v = !v; sfx(UiSound::Toggle); }
        const float a  = anim(r.key + 1, v ? 1.0f : 0.0f, 16.0f);
        const float hv = anim(r.key + 2, hovered ? 1.0f : 0.0f);
        const float al = r.alpha * (enabled ? 1.0f : 0.35f);
        if (a > 0.02f)
            for (int k = 1; k <= 3; ++k)
                dl->AddRect({p0.x - 0.25f * u * k, p0.y - 0.25f * u * k}, {p1.x + 0.25f * u * k, p1.y + 0.25f * u * k},
                            rgba(kGlow, 0.13f * a * al * (1.0f - 0.28f * (k - 1))), h * 0.5f + 0.25f * u * k, 0, 1.5f);
        dl->AddRectFilled(p0, p1, rgba(mixHex(0x0a2321, 0x0f473a, a), al), h * 0.5f);
        dl->AddRect(p0, p1, rgba(mixHex(mixHex(0x1c4a43, kGlow, a), kMint, hv * 0.5f), al), h * 0.5f, 0, std::max(1.0f, 0.1f * u));
        const float kr = h * 0.34f, kx = p0.x + h * 0.5f + (w - h) * a;
        if (a > 0.02f) radialGlow(dl, {kx, r.yc}, kr * 2.4f, kMint, 0.5f * a * al, 24);
        dl->AddCircleFilled({kx, r.yc}, kr, rgba(mixHex(kDim, kMint, a), al), 24);
    };

    auto segmented = [&](const Row& r, const char* const* labels, const int* values, int count, int& v, int maxValue) {
        const float h = 2.6f * u, gap = 0.4f * u, padX = 1.1f * u, fs = std::max(12.0f, 1.2f * u);
        float widths[6], total = -gap;
        for (int k = 0; k < count; ++k) {
            widths[k] = std::max(4.0f * u, m_fonts.ui->CalcTextSizeA(fs, FLT_MAX, 0.0f, labels[k]).x + padX * 2.0f);
            total += widths[k] + gap;
        }
        float x = xr - total;
        for (int k = 0; k < count; ++k) {
            const bool ok = values[k] <= maxValue;
            const ImVec2 p0(x, r.yc - h * 0.5f), p1(x + widths[k], r.yc + h * 0.5f);
            ImGui::SetCursorScreenPos(p0);
            ImGui::PushID(static_cast<int>(r.key) + k);
            if (ImGui::InvisibleButton("##sg", {widths[k], h}) && ok) { if (v != values[k]) sfx(UiSound::Click); v = values[k]; }
            const bool hovered = ImGui::IsItemHovered() && ok;
            ImGui::PopID();
            const float t  = anim(r.key + 8 + static_cast<unsigned>(k) * 2, v == values[k] ? 1.0f : 0.0f);
            const float hv = anim(r.key + 9 + static_cast<unsigned>(k) * 2, hovered ? 1.0f : 0.0f);
            const float al = r.alpha * (ok ? 1.0f : 0.35f);
            if (t > 0.02f)
                for (int g = 1; g <= 3; ++g)
                    dl->AddRect({p0.x - 0.2f * u * g, p0.y - 0.2f * u * g}, {p1.x + 0.2f * u * g, p1.y + 0.2f * u * g},
                                rgba(kGlow, 0.12f * t * al * (1.0f - 0.3f * (g - 1))), 0.7f * u + 0.2f * u * g, 0, 1.5f);
            dl->AddRectFilled(p0, p1, rgba(mixHex(kRightBg, 0x0f473a, t), al), 0.7f * u);
            dl->AddRect(p0, p1, rgba(mixHex(mixHex(0x1c4a43, kGlow, t), kMint, hv * 0.5f), al), 0.7f * u, 0, std::max(1.0f, 0.1f * u));
            const ImVec2 ts = m_fonts.ui->CalcTextSizeA(fs, FLT_MAX, 0.0f, labels[k]);
            dl->AddText(m_fonts.ui, fs, {x + (widths[k] - ts.x) * 0.5f, r.yc - ts.y * 0.5f},
                        rgba(mixHex(mixHex(kDim, kText, hv), kMint, t), al), labels[k]);
            x += widths[k] + gap;
        }
    };

    auto slider = [&](const Row& r, float& v, float lo, float hi, float step, bool enabled = true) {
        const float w = 20.8f * u, x0s = xr - w;
        ImGui::SetCursorScreenPos({x0s - 1.0f * u, r.yc - 1.4f * u});
        ImGui::PushID(static_cast<int>(r.key));
        ImGui::InvisibleButton("##sl", {w + 2.0f * u, 2.8f * u});
        const bool active  = ImGui::IsItemActive() && enabled;
        const bool hovered = (ImGui::IsItemHovered() || active) && enabled;
        ImGui::PopID();
        if (active) {
            m_dragRow = r.idx;
            float nv = lo + std::clamp((mp.x - x0s) / w, 0.0f, 1.0f) * (hi - lo);
            if (step > 0.0f) nv = std::round(nv / step) * step;
            nv = std::clamp(nv, lo, hi);
            // Тики при движении ползунка: не чаще раза в 70 мс, чтобы не трещало.
            if (nv != v && ImGui::GetTime() - m_lastTickTime > 0.07) { sfx(UiSound::Hover); m_lastTickTime = ImGui::GetTime(); }
            v = nv;
        }
        const float disp = anim(r.key + 1, v, 24.0f);
        const float hv   = anim(r.key + 2, hovered ? 1.0f : 0.0f);
        const float al   = r.alpha * (enabled ? 1.0f : 0.35f);
        const float frac = std::clamp((disp - lo) / (hi - lo), 0.0f, 1.0f);
        const float th   = std::max(2.0f, 0.2f * u), tx = x0s + frac * w;
        dl->AddRectFilled({x0s, r.yc - th * 0.5f}, {x0s + w, r.yc + th * 0.5f}, rgba(0x12413b, al), th * 0.5f);
        dl->AddRectFilled({x0s, r.yc - th * 0.5f}, {tx, r.yc + th * 0.5f}, rgba(kMint, al), th * 0.5f);
        const float tr = 0.55f * u * (1.0f + 0.25f * hv + (active ? 0.15f : 0.0f));
        if (enabled) radialGlow(dl, {tx, r.yc}, tr * (2.6f + 1.2f * hv), kGlow, (0.35f + 0.3f * hv) * al, 24);
        dl->AddCircleFilled({tx, r.yc}, tr, rgba(kMint, al), 24);
    };

    char buf[96];
    if (isLoad) {
        const float ny = 5.8f * u + 0.4f * u;
        const std::string hintLine = fit(m_savesHint.empty() ? "Сохранения лежат в папке игры" : m_savesHint, W, hintSize);
        dl->AddText(m_fonts.ui, hintSize, {x0, ny}, rgba(kDim), hintLine.c_str());
        dl->AddText(m_fonts.ui, hintSize, {x0, ny + hintSize * 1.5f}, rgba(kDim, 0.7f), "Клик по строке — загрузить. В игре: F5 — быстрое сохранение, F9 — быстрая загрузка");
        if (saveCount == 0) dl->AddText(m_fonts.ui, lblSize, {x0, rowTop + 1.5f * u}, rgba(kText, 0.8f), "Сохранений пока нет");
        const double now = ImGui::GetTime();
        if (m_confirmDelete >= 0 && now > m_confirmUntil) m_confirmDelete = -1;
        for (int k = 0; k < rows; ++k) {
            const int  idx = m_saveScroll + k;
            SaveRow&   sv  = saves[static_cast<std::size_t>(idx)];
            const bool bad = !sv.valid;
            const std::string line = fit(bad ? sv.error : sv.subtitle, W - 10.5f * u, hintSize);
            const auto r = rowBegin(k, sv.name.c_str(), line.c_str(), bad ? 0xff9a6b : kDim);

            // Клик по строке (кроме кнопки "Удалить") загружает сохранение.
            ImGui::SetCursorScreenPos({x0, rowTop + rowH * static_cast<float>(k)});
            ImGui::PushID(7000 + k);
            if (ImGui::InvisibleButton("##save", {W - 9.5f * u, rowH}) && !bad) {
                m_pendingAction = MenuAction::LoadSave;
                m_actionIndex   = idx;
                sfx(UiSound::Click);
            }
            ImGui::PopID();

            // "Удалить" справа: первый клик просит подтверждения, второй (в течение 3 с) удаляет.
            const bool confirming = m_confirmDelete == idx;
            const char* delText = confirming ? "Точно удалить?" : "Удалить";
            const float delFs = std::max(11.0f, 1.05f * u);
            const ImVec2 dts  = m_fonts.ui->CalcTextSizeA(delFs, FLT_MAX, 0.0f, delText);
            ImGui::SetCursorScreenPos({xr - dts.x - 1.0f * u, r.yc - 1.6f * u});
            ImGui::PushID(7100 + k);
            const bool delClicked = ImGui::InvisibleButton("##del", {dts.x + 2.0f * u, 3.2f * u});
            const bool delHover   = ImGui::IsItemHovered();
            ImGui::PopID();
            if (delClicked) {
                if (confirming) { m_pendingAction = MenuAction::DeleteSave; m_actionIndex = idx; m_confirmDelete = -1; sfx(UiSound::Back); }
                else            { m_confirmDelete = idx; m_confirmUntil = now + 3.0; sfx(UiSound::Click); }
            }
            const float dt01 = anim(r.key + 3, (delHover || confirming) ? 1.0f : 0.0f);
            const unsigned col = confirming ? 0xff9a6b : mixHex(kDim, kMint, dt01);
            dl->AddText(m_fonts.ui, delFs, {xr - dts.x, r.yc - dts.y * 0.5f}, rgba(col, r.alpha), delText);
        }
        if (saveCount > kModsVisible) {
            char more[96];
            std::snprintf(more, sizeof more, "%d–%d из %d, колесо — прокрутка", m_saveScroll + 1, m_saveScroll + rows, saveCount);
            dl->AddText(m_fonts.ui, hintSize, {x0, rowTop + rowH * static_cast<float>(rows) + 0.8f * u}, rgba(kDim, 0.8f), more);
        }
    } else switch (m_tab) {
        case kGfxTab: {
            auto r = rowBegin(0, "Вертикальная синхронизация", "Убирает разрывы кадра, ограничивает FPS частотой монитора");
            toggle(r, st.vsync);
            r = rowBegin(1, "Сглаживание", "MSAA: гладкие края рельефа и существ");
            static constexpr const char* labels[] = {"Выкл", "2×", "4×", "8×"};
            static constexpr int         values[] = {0, 2, 4, 8};
            segmented(r, labels, values, 4, st.msaa, m_maxMsaa);
            r = rowBegin(2, "Автодальность", "Дальность прорисовки сама подстраивается под мощность ПК и FPS");
            toggle(r, st.autoViewRadius);
            std::snprintf(buf, sizeof buf, st.autoViewRadius ? "Потолок: %d чанков (%d м), дальше — грубее" : "%d чанков, %d м от камеры (дальше 8 — грубее)",
                          st.viewRadius, st.viewRadius * 32);
            r = rowBegin(3, st.autoViewRadius ? "Максимальная дальность" : "Дальность прорисовки", buf);
            float vr = static_cast<float>(st.viewRadius);
            slider(r, vr, 4.0f, 64.0f, 1.0f);
            st.viewRadius = static_cast<int>(vr);
            r = rowBegin(4, "Тени", "Рельеф и существа отбрасывают тени от солнца");
            toggle(r, st.shadows);
            break;
        }
        case kQualityTab: {
            // Рендер в меньшем разрешении и растяжение: бесплатный FPS на слабых видеокартах.
            auto r = rowBegin(0, "Апскейлер", "Рендер в меньшем разрешении");
            static constexpr const char* upLabels[] = {"Выкл", "FSR 1.0", "NIS", "Temporal"};
            static constexpr int         upValues[] = {0, 1, 2, 3};
            segmented(r, upLabels, upValues, 4, st.upscaler, 3);
            const bool upOn = st.upscaler != 0;
            std::snprintf(buf, sizeof buf, upOn ? "Сцена рисуется в %.0f%% разрешения окна" : "Включите апскейлер, чтобы менять масштаб", st.renderScale * 100.0f);
            r = rowBegin(1, "Масштаб рендера", buf);
            slider(r, st.renderScale, 0.5f, 1.0f, 0.05f, upOn);
            r = rowBegin(2, "Резкость", upOn ? "Усиление чёткости после растяжения" : "Работает вместе с апскейлером");
            slider(r, st.sharpness, 0.0f, 1.0f, 0.05f, upOn);
            // Генерация кадров: между настоящими кадрами показывается построенный по движению (нужны Temporal и vsync)
            const bool fgOk = st.upscaler == 3 && st.vsync;
            r = rowBegin(3, "Генерация кадров",
                         fgOk ? "Промежуточные кадры: плавнее при низком FPS, выше задержка"
                              : "Нужны Temporal и вертикальная синхронизация");
            toggle(r, st.frameGen, fgOk);
            break;
        }
        case kCtlTab: {
            std::snprintf(buf, sizeof buf, "WASD: %.0f ед./с", st.panSpeed);
            auto r = rowBegin(0, "Скорость панорамы", buf);
            slider(r, st.panSpeed, 4.0f, 30.0f, 1.0f);
            std::snprintf(buf, sizeof buf, "ПКМ + мышь: %.2f°/пикс.", st.orbitSens);
            r = rowBegin(1, "Чувствительность вращения", buf);
            slider(r, st.orbitSens, 0.05f, 0.6f, 0.01f);
            std::snprintf(buf, sizeof buf, "Колесо: %.1f ед. за шаг", st.zoomStep);
            r = rowBegin(2, "Шаг зума", buf);
            slider(r, st.zoomStep, 0.5f, 5.0f, 0.1f);
            r = rowBegin(3, "Инвертировать вращение", "Мышь вверх — камера вниз");
            toggle(r, st.invertOrbit);
            break;
        }
        case kSoundTab: {
            // Строка состояния: на какое устройство идёт звук (или почему его нет).
            const float ny = 5.8f * u + 0.4f * u;
            const std::string line = fit("Вывод: " + (m_audioStatus.empty() ? std::string("—") : m_audioStatus), W, hintSize);
            dl->AddText(m_fonts.ui, hintSize, {x0, ny}, rgba(kDim), line.c_str());
            dl->AddText(m_fonts.ui, hintSize, {x0, ny + hintSize * 1.5f}, rgba(kDim, 0.7f), "Все звуки создаются кодом, внешних файлов нет");
            const bool live = m_audioStatus.rfind("нет звукового", 0) != 0 && m_audioStatus.rfind("отключён", 0) != 0;
            struct Row { const char* name; const char* hint; float* v; };
            const Row rowsDef[] = {
                {"Общая громкость", "Всё сразу", &st.volMaster},
                {"Окружение", "Ветер, вода, птицы и музыка", &st.volAmbient},
                {"Существа", "Голоса: тревога, зов, рык", &st.volCreatures},
                {"Силы бога и интерфейс", "Удары, стройка, клики меню", &st.volEffects},
            };
            for (int k = 0; k < 4; ++k) {
                std::snprintf(buf, sizeof buf, "%s: %.0f%%", rowsDef[k].hint, *rowsDef[k].v * 100.0f);
                const auto r = rowBegin(k, rowsDef[k].name, buf);
                slider(r, *rowsDef[k].v, 0.0f, 1.0f, 0.05f, live);
            }
            break;
        }
        case kModsTab: {
            const float ny = 5.8f * u + 0.4f * u;
            const std::string hintLine = fit(m_modsHint.empty() ? "Моды лежат в папке mods/ рядом с игрой" : m_modsHint, W, hintSize);
            dl->AddText(m_fonts.ui, hintSize, {x0, ny}, rgba(kDim), hintLine.c_str());
            dl->AddText(m_fonts.ui, hintSize, {x0, ny + hintSize * 1.5f}, rgba(kDim, 0.7f), "Положите папку мода туда и откройте эту страницу заново");
            if (modCount == 0) {
                dl->AddText(m_fonts.ui, lblSize, {x0, rowTop + 1.5f * u}, rgba(kText, 0.8f), "Модов не найдено");
            }
            for (int k = 0; k < rows; ++k) {
                ModRow& m = mods[static_cast<std::size_t>(m_modScroll + k)];
                const bool bad = !m.error.empty();
                std::string line = fit(bad ? m.error : (m.description.empty() ? m.meta : m.meta + " · " + m.description), W - 6.5f * u, hintSize);
                auto r = rowBegin(k, m.name.c_str(), line.c_str(), bad ? 0xff9a6b : kDim);
                const bool before = m.enabled;
                bool       on     = m.enabled;
                toggle(r, on, m.canToggle);
                if (on != before) { m.enabled = on; m.toggled = true; }
            }
            if (modCount > kModsVisible) {
                char more[96];
                std::snprintf(more, sizeof more, "%d–%d из %d, колесо — прокрутка", m_modScroll + 1, m_modScroll + rows, modCount);
                dl->AddText(m_fonts.ui, hintSize, {x0, rowTop + rowH * static_cast<float>(rows) + 0.8f * u}, rgba(kDim, 0.8f), more);
            }
            break;
        }
        default: {
            std::snprintf(buf, sizeof buf, "Размер текста отладочного окна: %.0f%%", st.uiScale * 100.0f);
            auto r = rowBegin(0, "Масштаб интерфейса", buf);
            slider(r, st.uiScale, 0.75f, 1.5f, 0.05f);
            r = rowBegin(1, "Отладочное окно", "FPS, камера, население");
            toggle(r, st.showDebug);
            r = rowBegin(2, "Автосохранение", "Как часто мир пишется в слот «autosave»");
            static constexpr const char* asLabels[] = {"Выкл", "2 мин", "5 мин", "10 мин"};
            static constexpr int         asValues[] = {0, 2, 5, 10};
            segmented(r, asLabels, asValues, 4, st.autosaveMinutes, 10);
            break;
        }
    }

    // ── Нижние кнопки: "Сбросить раздел" и "Назад" ─────────────────────────
    const float btnH = 3.4f * u, btnBottom = size.y - 6.0f * u, btnY = btnBottom - btnH;
    if (!isLoad) {
        const char*  txt = "Сбросить раздел";
        const float  fs  = std::max(12.0f, 1.2f * u);
        const ImVec2 ts  = m_fonts.ui->CalcTextSizeA(fs, FLT_MAX, 0.0f, txt);
        ImGui::SetCursorScreenPos({x0, btnY});
        ImGui::PushID(9001);
        const bool noReset = m_tab == kModsTab || isLoad;
        const bool clicked = ImGui::InvisibleButton("##reset", {ts.x + 1.0f * u, btnH}) && !noReset;
        const bool hovered = ImGui::IsItemHovered() && !noReset;
        ImGui::PopID();
        const float t = anim(0x3000u, hovered ? 1.0f : 0.0f);
        const ImVec2 pos(x0, btnY + btnH * 0.5f - ts.y * 0.5f);
        const float al = noReset ? 0.35f : 1.0f;
        glowLabel(fs, pos, txt, t, al, kDim);
        isoline(pos.x, pos.y + ts.y + 0.2f * u, ts.x, t, al);
        if (clicked) {
            sfx(UiSound::Click);
            const Settings d;
            switch (m_tab) {
                case kGfxTab: st.vsync = d.vsync; st.msaa = d.msaa; st.viewRadius = d.viewRadius; st.autoViewRadius = d.autoViewRadius; st.shadows = d.shadows; break;
                case kQualityTab: st.upscaler = d.upscaler; st.renderScale = d.renderScale; st.sharpness = d.sharpness; st.frameGen = d.frameGen; break;
                case kSoundTab: st.volMaster = d.volMaster; st.volAmbient = d.volAmbient; st.volCreatures = d.volCreatures; st.volEffects = d.volEffects; break;
                case kCtlTab: st.panSpeed = d.panSpeed; st.orbitSens = d.orbitSens; st.zoomStep = d.zoomStep; st.invertOrbit = d.invertOrbit; break;
                default: st.uiScale = d.uiScale; st.showDebug = d.showDebug; st.autosaveMinutes = d.autosaveMinutes; break;
            }
        }
    }
    {
        const float  bw = 9.5f * u;
        const ImVec2 p0(xr - bw, btnY), p1(xr, btnBottom);
        ImGui::SetCursorScreenPos(p0);
        ImGui::PushID(9002);
        if (ImGui::InvisibleButton("##back", {bw, btnH})) { m_page = Page::Main; sfx(UiSound::Back); }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const float hv = anim(0x3001u, hovered ? 1.0f : 0.0f);
        for (int g = 1; g <= 4; ++g)
            dl->AddRectFilled({p0.x - 0.3f * u * g, p0.y - 0.3f * u * g}, {p1.x + 0.3f * u * g, p1.y + 0.3f * u * g},
                              rgba(kGlow, (0.10f + 0.08f * hv) * (1.0f - 0.2f * (g - 1))), btnH * 0.5f + 0.3f * u * g);
        dl->AddRectFilled(p0, p1, rgba(mixHex(kGlow, kMint, hv)), btnH * 0.5f);
        const float  fs = std::max(13.0f, 1.3f * u);
        const ImVec2 ts = m_fonts.ui->CalcTextSizeA(fs, FLT_MAX, 0.0f, "Назад");
        dl->AddText(m_fonts.ui, fs, {p0.x + (bw - ts.x) * 0.5f, p0.y + (btnH - ts.y) * 0.5f}, rgba(kLeftBg), "Назад");
    }
}

}  // namespace wsim
