#include "ui/PowerBar.hpp"

#include "ui/Style.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace wsim {

using namespace wsim::style;

bool PowerBar::draw(float dt, std::span<const PowerBarItem> items, int& selected, float& radius, float minRadius,
                    float maxRadius) {
    if (items.empty()) return false;
    m_focus.resize(items.size(), 0.0f);
    selected = std::clamp(selected, 0, static_cast<int>(items.size()) - 1);

    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float  u      = screen.x * 0.01f;
    // Масштаб панели: если сил много (моды), всё сжимается, чтобы уместиться в ~96% ширины.
    const float sliderW = 11.0f * u, sliderBlock = sliderW + 8.5f * u;  // подпись + ползунок радиуса
    float k = 1.0f;
    float fs = 0, keyFs = 0, padX = 0, gap = 0, barPad = 0, h = 0;
    std::vector<float> widths(items.size());
    float total = 0.0f;
    for (int pass = 0; pass < 6; ++pass) {
        fs = std::max(10.0f, 1.3f * u * k);
        keyFs = std::max(8.0f, 0.85f * u * k);
        padX = 1.5f * u * k; gap = 0.5f * u * k; barPad = 1.0f * u; h = 3.3f * u;
        total = 0.0f;
        for (std::size_t i = 0; i < items.size(); ++i) {
            widths[i] = m_fonts.ui->CalcTextSizeA(fs, FLT_MAX, 0.0f, items[i].name.c_str()).x + padX * 2.0f + 1.4f * u * k;
            total += widths[i] + (i ? gap : 0.0f);
        }
        const float need = total + sliderBlock + barPad * 2.0f + 1.5f * u, have = screen.x * 0.96f;
        if (need > have && k > 0.5f) k = std::max(0.5f, k * have / need * 0.985f); else break;  // сжимаем, пока не влезет (несколько проходов: ширина текста не строго линейна)
    }
    const float barW = total + sliderBlock + barPad * 2.0f + 1.5f * u, barH = h + barPad * 2.0f;
    const ImVec2 p0((screen.x - barW) * 0.5f, screen.y - barH - 2.2f * u), p1(p0.x + barW, p0.y + barH);

    const ImVec2 mp = ImGui::GetIO().MousePos;
    const bool   over = mp.x >= p0.x - 0.5f * u && mp.x <= p1.x + 0.5f * u && mp.y >= p0.y - 0.5f * u && mp.y <= p1.y + 0.5f * u;

    ImGui::SetNextWindowPos(p0);
    ImGui::SetNextWindowSize({barW, barH});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##powerbar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Подложка: тёмная капсула с мягким свечением по краю.
    for (int g = 3; g >= 1; --g)
        dl->AddRect({p0.x - 0.2f * u * g, p0.y - 0.2f * u * g}, {p1.x + 0.2f * u * g, p1.y + 0.2f * u * g},
                    rgba(kGlow, 0.10f * (1.0f - 0.25f * (g - 1))), h * 0.5f + 0.2f * u * g, 0, 1.5f);
    dl->AddRectFilled(p0, p1, rgba(kRightBg, 0.92f), (barH) * 0.5f);
    dl->AddRect(p0, p1, rgba(kGlow, 0.35f), barH * 0.5f, 0, std::max(1.0f, 0.1f * u));

    int hovered = -1;
    float x = p0.x + barPad + 0.5f * u;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const int    idx = static_cast<int>(i);
        const ImVec2 a(x, p0.y + barPad), b(x + widths[i], p0.y + barPad + h);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(idx);
        if (ImGui::InvisibleButton("##p", {widths[i], h})) { if (selected != idx && m_sfx) m_sfx(UiSound::Click); selected = idx; }
        if (ImGui::IsItemHovered()) hovered = idx;
        ImGui::PopID();

        float& t = m_focus[i];
        const float target = selected == idx ? 1.0f : (hovered == idx ? 0.55f : 0.0f);
        t += (target - t) * (1.0f - std::exp(-dt * 14.0f));

        const float alpha = 0.55f + 0.45f * t;
        if (t > 0.02f) {
            dl->AddRectFilled(a, b, rgba(kGlow, 0.16f * t), h * 0.5f);
            for (int g = 1; g <= 3; ++g)
                dl->AddRect({a.x - 0.2f * u * g, a.y - 0.2f * u * g}, {b.x + 0.2f * u * g, b.y + 0.2f * u * g},
                            rgba(kGlow, 0.13f * t * (1.0f - 0.3f * (g - 1))), h * 0.5f + 0.2f * u * g, 0, 1.5f);
            dl->AddRect(a, b, rgba(kGlow, 0.8f * t), h * 0.5f, 0, std::max(1.0f, 0.1f * u));
        }

        // Горячая клавиша (1..9) и название.
        char key[4] = "";
        if (i < 9) std::snprintf(key, sizeof key, "%zu", i + 1);
        const float  fsi = fs * (1.0f + 0.06f * t);
        const ImVec2 ts  = m_fonts.ui->CalcTextSizeA(fsi, FLT_MAX, 0.0f, items[i].name.c_str());
        const ImVec2 kts = m_fonts.ui->CalcTextSizeA(keyFs, FLT_MAX, 0.0f, key);
        const float  cy  = (a.y + b.y) * 0.5f;
        dl->AddText(m_fonts.ui, keyFs, {a.x + padX * 0.55f, cy - kts.y * 0.5f}, rgba(kDim, alpha), key);
        const ImVec2 tp(a.x + padX + 0.6f * u, cy - ts.y * 0.5f);
        if (t > 0.02f)
            for (int d = 0; d < 8; ++d) {
                const float ang = 0.785398f * static_cast<float>(d), rad = 0.12f * u;
                dl->AddText(m_fonts.ui, fsi, {tp.x + std::cos(ang) * rad, tp.y + std::sin(ang) * rad}, rgba(kMint, 0.10f * t), items[i].name.c_str());
            }
        dl->AddText(m_fonts.ui, fsi, tp, rgba(lerpHex(kText, kMint, t), alpha), items[i].name.c_str());
        // Волнистая изолиния под выбранной силой.
        if (t > 0.02f) {
            const float yb = tp.y + ts.y + 0.05f * u, w = ts.x * t, wavelen = 2.4f * u, amp = 0.16f * u;
            std::vector<ImVec2> pts;
            for (float xx = 0.0f; xx < w; xx += 3.0f) pts.emplace_back(tp.x + xx, yb + std::sin(xx / wavelen * 6.2831853f) * amp);
            pts.emplace_back(tp.x + w, yb + std::sin(w / wavelen * 6.2831853f) * amp);
            if (pts.size() >= 2) dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), rgba(kMint, alpha), ImDrawFlags_None, std::max(1.2f, 0.12f * u));
        }
        x += widths[i] + gap;
    }

    if (hovered >= 0 && hovered != m_prevHover && m_sfx) m_sfx(UiSound::Hover);
    m_prevHover = hovered;

    // Радиус кисти: подпись и ползунок (тот же вид, что в настройках).
    {
        const float sx0 = p1.x - barPad - 1.0f * u - sliderW, yc = p0.y + barH * 0.5f;
        char lbl[32];
        std::snprintf(lbl, sizeof lbl, "радиус %.0f", radius);
        const ImVec2 lts = m_fonts.ui->CalcTextSizeA(keyFs * 1.15f, FLT_MAX, 0.0f, lbl);
        dl->AddText(m_fonts.ui, keyFs * 1.15f, {sx0 - lts.x - 1.0f * u, yc - lts.y * 0.5f}, rgba(kDim), lbl);

        ImGui::SetCursorScreenPos({sx0 - 0.8f * u, yc - 1.3f * u});
        ImGui::PushID(9900);
        ImGui::InvisibleButton("##radius", {sliderW + 1.6f * u, 2.6f * u});
        const bool active = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (active && maxRadius > minRadius)
            radius = minRadius + std::clamp((mp.x - sx0) / sliderW, 0.0f, 1.0f) * (maxRadius - minRadius);
        const float frac = maxRadius > minRadius ? std::clamp((radius - minRadius) / (maxRadius - minRadius), 0.0f, 1.0f) : 0.0f;
        const float th = std::max(2.0f, 0.18f * u), tx = sx0 + frac * sliderW, tr = 0.5f * u * (hov || active ? 1.2f : 1.0f);
        dl->AddRectFilled({sx0, yc - th * 0.5f}, {sx0 + sliderW, yc + th * 0.5f}, rgba(0x12413b), th * 0.5f);
        dl->AddRectFilled({sx0, yc - th * 0.5f}, {tx, yc + th * 0.5f}, rgba(kMint), th * 0.5f);
        radialGlow(dl, {tx, yc}, tr * 2.8f, kGlow, hov || active ? 0.6f : 0.4f, 24);
        dl->AddCircleFilled({tx, yc}, tr, rgba(kMint), 24);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);

    // Подсказка к наведённой силе: описание над панелью.
    const bool tipWanted = hovered >= 0 && !items[static_cast<std::size_t>(hovered)].description.empty();
    if (tipWanted) m_tipItem = hovered;
    m_tip += ((tipWanted ? 1.0f : 0.0f) - m_tip) * (1.0f - std::exp(-dt * 12.0f));
    if (m_tip > 0.02f && m_tipItem >= 0 && m_tipItem < static_cast<int>(items.size())) {
        const PowerBarItem& it = items[static_cast<std::size_t>(m_tipItem)];
        std::string text = it.description;
        if (it.source != "base" && !it.source.empty()) text += "  ·  мод: " + it.source;
        const float tfs = std::max(12.0f, 1.05f * u);
        const ImVec2 ts = m_fonts.ui->CalcTextSizeA(tfs, FLT_MAX, 0.0f, text.c_str());
        const ImVec2 q0((screen.x - ts.x) * 0.5f - 1.0f * u, p0.y - ts.y - 2.4f * u), q1(q0.x + ts.x + 2.0f * u, q0.y + ts.y + 1.0f * u);
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddRectFilled(q0, q1, rgba(kRightBg, 0.92f * m_tip), 0.8f * u);
        fg->AddRect(q0, q1, rgba(kGlow, 0.35f * m_tip), 0.8f * u);
        fg->AddText(m_fonts.ui, tfs, {q0.x + 1.0f * u, q0.y + 0.5f * u}, rgba(kText, m_tip), text.c_str());
    }
    return over;
}

}  // namespace wsim
