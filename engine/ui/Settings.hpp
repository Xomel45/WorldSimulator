#pragma once

#include <string>

namespace wsim {

// Пользовательские настройки. Чистые данные без зависимостей от Vulkan/ImGui:
// меню их редактирует, Application применяет к движку. Хранятся в простом
// текстовом файле "ключ=значение" в каталоге конфигурации пользователя.
struct Settings {
    // Графика
    bool  vsync      = true;
    int   msaa       = 4;      // 0 (выкл), 2, 4, 8 -- урезается до возможностей GPU
    bool  autoViewRadius = true;  // дальность сама подстраивается под FPS (viewRadius -- потолок)
    int   viewRadius = 24;     // чанков от камеры, 4..64 (чанки дальше 8 грубее, см. TerrainChunks)
    bool  shadows    = true;   // тени от солнца (каскадные карты)
    // Качество картинки: рендер в меньшем разрешении + апскейл (FSR 1.0 / NVIDIA Image Scaling)
    int   upscaler    = 0;      // 0 выкл, 1 FSR 1.0, 2 NIS, 3 Temporal (свой временной апскейлер; при масштабе 100% -- TAA)
    float renderScale = 0.75f;  // доля разрешения окна, 0.5..1 (при включённом апскейлере)
    float sharpness   = 0.5f;   // резкость после апскейла, 0..1
    bool  frameGen    = false;  // генерация кадров (только с Temporal и вертикальной синхронизацией)
    // Управление
    float panSpeed    = 12.0f;  // ед./с при distance=25
    float orbitSens   = 0.2f;   // градусов на пиксель
    float zoomStep    = 1.5f;   // ед. дистанции за шаг колеса
    bool  invertOrbit = false;  // инверсия вертикального вращения
    // Звук (0..1)
    float volMaster    = 0.8f;
    float volAmbient   = 0.6f;   // ветер, вода, птицы, музыка
    float volCreatures = 0.7f;   // голоса существ
    float volEffects   = 0.8f;   // силы бога и интерфейс
    // Интерфейс
    float uiScale   = 1.0f;     // масштаб отладочного окна
    bool  showDebug = true;     // отладочное окно (FPS и пр.)
    int   autosaveMinutes = 5;  // 0 -- выкл; иначе раз в столько минут в слот "autosave"

    bool operator==(const Settings&) const = default;

    // Подвести значения под допустимые диапазоны (после загрузки из файла).
    void clamp() noexcept;

    [[nodiscard]] static std::string defaultPath();
    [[nodiscard]] static Settings    load(const std::string& path);  // нет файла -- значения по умолчанию
    bool                             save(const std::string& path) const;
};

}  // namespace wsim
