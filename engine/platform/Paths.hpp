#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace wsim::paths {

namespace fs = std::filesystem;

// Каталог исполняемого файла (для переносимой раскладки: рядом с exe лежат shaders/, assets/, mods/).
[[nodiscard]] inline fs::path exeDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof buf / sizeof buf[0]));
    if (n > 0 && n < sizeof buf / sizeof buf[0]) return fs::path(buf).parent_path();
    return {};
#else
    std::error_code ec;
    const fs::path  p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : p.parent_path();
#endif
}

[[nodiscard]] inline std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

// Пользовательский каталог настроек: Linux $XDG_CONFIG_HOME или ~/.config, Windows %APPDATA%.
[[nodiscard]] inline fs::path configDir() {
#ifdef _WIN32
    if (const auto v = env("APPDATA"); !v.empty()) return v;
    if (const auto v = env("USERPROFILE"); !v.empty()) return fs::path(v) / "AppData" / "Roaming";
#else
    if (const auto v = env("XDG_CONFIG_HOME"); !v.empty()) return v;
    if (const auto v = env("HOME"); !v.empty()) return fs::path(v) / ".config";
#endif
    return ".";
}

// Пользовательский каталог данных (моды игрока): Linux $XDG_DATA_HOME или ~/.local/share, Windows %APPDATA%.
[[nodiscard]] inline fs::path dataDir() {
#ifdef _WIN32
    return configDir();
#else
    if (const auto v = env("XDG_DATA_HOME"); !v.empty()) return v;
    if (const auto v = env("HOME"); !v.empty()) return fs::path(v) / ".local" / "share";
    return ".";
#endif
}

}  // namespace wsim::paths
