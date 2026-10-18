#include "ui/Settings.hpp"

#include "platform/Paths.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace wsim {

void Settings::clamp() noexcept {
    msaa = (msaa >= 8) ? 8 : (msaa >= 4) ? 4 : (msaa >= 2) ? 2 : 0;
    viewRadius = std::clamp(viewRadius, 4, 64);
    panSpeed   = std::clamp(panSpeed, 4.0f, 30.0f);
    orbitSens  = std::clamp(orbitSens, 0.05f, 0.6f);
    zoomStep   = std::clamp(zoomStep, 0.5f, 5.0f);
    uiScale    = std::clamp(uiScale, 0.75f, 1.5f);
    volMaster    = std::clamp(volMaster, 0.0f, 1.0f);
    volAmbient   = std::clamp(volAmbient, 0.0f, 1.0f);
    volCreatures = std::clamp(volCreatures, 0.0f, 1.0f);
    volEffects   = std::clamp(volEffects, 0.0f, 1.0f);
    if (autosaveMinutes != 0 && autosaveMinutes != 2 && autosaveMinutes != 5 && autosaveMinutes != 10) autosaveMinutes = 5;
    upscaler    = std::clamp(upscaler, 0, 3);
    renderScale = std::clamp(renderScale, 0.5f, 1.0f);
    sharpness   = std::clamp(sharpness, 0.0f, 1.0f);
}

std::string Settings::defaultPath() {
    return (paths::configDir() / "WorldSimulator" / "settings.cfg").string();
}

Settings Settings::load(const std::string& path) {
    Settings s;
    std::ifstream in(path);
    std::string   line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        try {
            if      (key == "vsync")       s.vsync       = std::stoi(val) != 0;
            else if (key == "msaa")        s.msaa        = std::stoi(val);
            else if (key == "viewRadius")  s.viewRadius  = std::stoi(val);
            else if (key == "autoViewRadius") s.autoViewRadius = std::stoi(val) != 0;
            else if (key == "shadows")     s.shadows     = std::stoi(val) != 0;
            else if (key == "volMaster")   s.volMaster    = std::stof(val);
            else if (key == "volAmbient")  s.volAmbient   = std::stof(val);
            else if (key == "volCreatures") s.volCreatures = std::stof(val);
            else if (key == "volEffects")  s.volEffects   = std::stof(val);
            else if (key == "autosaveMinutes") s.autosaveMinutes = std::stoi(val);
            else if (key == "upscaler")    s.upscaler    = std::stoi(val);
            else if (key == "renderScale") s.renderScale = std::stof(val);
            else if (key == "sharpness")   s.sharpness   = std::stof(val);
            else if (key == "frameGen")    s.frameGen    = std::stoi(val) != 0;
            else if (key == "panSpeed")    s.panSpeed    = std::stof(val);
            else if (key == "orbitSens")   s.orbitSens   = std::stof(val);
            else if (key == "zoomStep")    s.zoomStep    = std::stof(val);
            else if (key == "invertOrbit") s.invertOrbit = std::stoi(val) != 0;
            else if (key == "uiScale")     s.uiScale     = std::stof(val);
            else if (key == "showDebug")   s.showDebug   = std::stoi(val) != 0;
        } catch (...) {
            // битое значение -- остаётся по умолчанию
        }
    }
    s.clamp();
    return s;
}

bool Settings::save(const std::string& path) const {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << "vsync=" << vsync << "\nmsaa=" << msaa << "\nviewRadius=" << viewRadius << "\nautoViewRadius=" << autoViewRadius
        << "\nshadows=" << shadows << "\nvolMaster=" << volMaster << "\nvolAmbient=" << volAmbient << "\nvolCreatures=" << volCreatures
        << "\nvolEffects=" << volEffects << "\nupscaler=" << upscaler << "\nrenderScale=" << renderScale << "\nsharpness=" << sharpness << "\nframeGen=" << frameGen
        << "\npanSpeed=" << panSpeed << "\norbitSens=" << orbitSens
        << "\nzoomStep=" << zoomStep << "\ninvertOrbit=" << invertOrbit << "\nuiScale=" << uiScale
        << "\nshowDebug=" << showDebug << "\nautosaveMinutes=" << autosaveMinutes << "\n";
    return static_cast<bool>(out);
}

}  // namespace wsim
