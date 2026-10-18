#pragma once

#include <glm/vec2.hpp>

#include "ui/UiSound.hpp"

#include <array>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

struct ImFont;
struct ImDrawList;
struct ImVec2;

namespace wsim {

struct Settings;

enum class MenuAction { None, NewWorld, Continue, Save, LoadSave, DeleteSave, Quit };

// Строка страницы "Моды": что показать и что нажал игрок. Меню ничего не знает про
// загрузчик модов -- Application собирает строки и применяет `toggled`.
struct ModRow {
    std::string name;
    std::string meta;         // "v1.0 · автор · 3 силы"
    std::string description;
    std::string error;        // первая проблема, если есть
    bool        enabled   = false;
    bool        canToggle = true;   // false -- мод несовместим/сломан
    bool        toggled   = false;  // выставляет меню на клик
};

// Строка страницы "Загрузить": что показать. Для LoadSave/DeleteSave индекс строки в actionIndex().
struct SaveRow {
    std::string name;
    std::string subtitle;  // "18.10.2026 21:30 · 24 сущ. · 3 дома · игра 12 мин"
    std::string error;     // непусто -- файл не читается (его можно только удалить)
    bool        valid = true;
};

struct MenuFonts {
    ImFont* title = nullptr;  // Unbounded Bold
    ImFont* ui    = nullptr;  // Geologica Regular
};

// Главное меню: тёмная карта слева, светлая чашка с колонией справа, пункты
// списка с изолинией и фокусом при наведении. Рисуется целиком через
// ImDrawList, поверх всего экрана -- ничего не знает ни про World, ни про
// Vulkan: колония приходит позициями в круге [-1,1]^2.
class MainMenu {
public:
    explicit MainMenu(MenuFonts fonts) : m_fonts(fonts) {}

    // Один кадр меню. canContinue -- есть ли запущенная игра, к которой можно вернуться.
    // settings редактируются прямо здесь (страница "Настройки"), применяет их вызывающий.
    [[nodiscard]] MenuAction draw(float dt, bool canContinue, std::span<const glm::vec2> colony, Settings& settings,
                                  std::span<ModRow> mods, std::span<SaveRow> saves);

    // Строка (индекс в переданном списке) для LoadSave/DeleteSave.
    [[nodiscard]] int actionIndex() const noexcept { return m_actionIndex; }
    // Подсказка на странице "Загрузить": где лежат сохранения.
    void setSavesHint(std::string text) { m_savesHint = std::move(text); }

    // Звуки интерфейса: меню само ничего не играет, а сообщает событие -- игра отвечает звуком.
    void setSoundCallback(std::function<void(UiSound)> cb) { m_sfx = std::move(cb); }
    // Строка состояния на вкладке "Звук": устройство или причина, почему звука нет.
    void setAudioStatus(std::string text) { m_audioStatus = std::move(text); }

    // Подсказка на странице "Моды": куда класть папки модов.
    void setModsHint(std::string text) { m_modsHint = std::move(text); }

    // Возможности железа для страницы настроек: максимум MSAA (1/2/4/8) и есть ли ray_query.
    void setCaps(int maxMsaa, bool rtSupported) noexcept { m_maxMsaa = maxMsaa; m_rtSupported = rtSupported; }

    // Esc: открыта страница настроек -- вернуться в главное меню.
    [[nodiscard]] bool inSettings() const noexcept { return m_page != Page::Main; }  // настройки или загрузка: Esc возвращает в меню
    void               back() noexcept { m_page = Page::Main; }

    // Отладка (WSIM_MENU_PAGE): сразу открыть страницу -- 0 настройки (tab = вкладка), 1 загрузка.
    void debugOpen(int page, int tab) noexcept { m_page = page == 1 ? Page::Load : Page::Settings; m_tab = tab; }

private:
    static constexpr int kItemCount = 6;

    enum class Page { Main, Settings, Load };

    void drawSettings(ImDrawList* dl, ImVec2 size, float dt, Settings& settings, std::span<ModRow> mods, std::span<SaveRow> saves);

    [[nodiscard]] bool enabled(int index, bool canContinue) const noexcept;
    [[nodiscard]] int  step(int from, int dir, bool canContinue) const noexcept;

    MenuFonts                      m_fonts;
    Page                           m_page = Page::Main;
    int                            m_maxMsaa = 8;
    bool                           m_rtSupported = true;
    // Страница настроек: активный раздел, подсвеченная строка и плавная анимация
    // каждого элемента (ключ -- ImGuiID), чтобы переключатели и ползунки не щёлкали.
    int                            m_tab = 0;
    float                          m_rowDim = 0.0f;
    int                            m_dragRow = -1;
    int                            m_modScroll = 0, m_saveScroll = 0;
    std::string                    m_savesHint;
    int                            m_actionIndex = -1;
    MenuAction                     m_pendingAction = MenuAction::None;  // действие со страницы "Загрузить"
    int                            m_confirmDelete = -1;                // строка, ждущая подтверждения удаления
    double                         m_confirmUntil = 0.0;
    std::function<void(UiSound)>   m_sfx;
    std::string                    m_audioStatus;
    int                            m_prevSel = -1, m_prevHot = -1, m_prevTabHover = -1;
    double                         m_lastTickTime = 0.0;
    void sfx(UiSound s) const { if (m_sfx) m_sfx(s); }
    std::string                    m_modsHint;
    std::unordered_map<unsigned, float> m_anim;
    int                            m_sel = -1;      // -1 -- ничего не выбрано
    float                          m_dim = 0.0f;    // 0..1, насколько приглушены невыбранные пункты
    bool                           m_lastCanContinue = false;
    bool                           m_initialised = false;
    // Состояние бегства каждого существа (единицы u = 1% ширины экрана):
    // смещение от "родной" позиции из симуляции, скорость и накопленный страх.
    struct Runner {
        glm::vec2 off{0.0f};
        glm::vec2 vel{0.0f};
        float     fear = 0.0f;
    };
    std::vector<Runner>            m_runners;
    std::array<float, kItemCount>  m_focus{};  // 0..1, сглаженная "выбранность" каждого пункта
};

}  // namespace wsim
