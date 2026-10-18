#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wsim::audio {

using Pcm = std::vector<float>;  // моно, float [-1, 1]

// Все звуки игры -- процедурные: PCM рождается в коде, внешних файлов нет (подход
// AudioSystem из Podjezd: тот же набор приёмов -- осцилляторы, огибающие, шум через
// фильтры -- только без привязки к FMOD, поэтому синтез тестируется без звуковой карты).
enum class SoundId : int {
    // Интерфейс
    UiHover, UiClick, UiBack, UiToggle,
    // Силы бога и события мира
    Pop, Whoosh, Thud, Knock, Zap, Chime,
    // Голоса существ: три тембра на каждый смысл
    Alarm0, Alarm1, Alarm2,
    Coo0, Coo1, Coo2,
    Growl0, Growl1, Growl2,
    // Птицы (одиночные посвисты)
    Chirp0, Chirp1, Chirp2, Chirp3,
    // Бесшовные петли
    LoopWind, LoopWater, LoopMusic, LoopRumble,
    Count
};

inline constexpr int kSoundCount = static_cast<int>(SoundId::Count);

// "thud", "pop", ... -> SoundId; для скриптов модов. false -- имени нет (или это петля/голос существа).
[[nodiscard]] bool soundFromName(std::string_view name, SoundId& out) noexcept;

// Синтез одного звука с частотой дискретизации rate (Гц). Детерминирован.
[[nodiscard]] Pcm synthesize(SoundId id, int rate);

// Весь набор (индекс -- static_cast<int>(SoundId)).
[[nodiscard]] std::vector<Pcm> synthesizeAll(int rate);

[[nodiscard]] bool isLoop(SoundId id) noexcept;

// WAV 16-bit mono -- для прослушивания звуков глазами разработчика (WSIM_DUMP_SOUNDS).
bool writeWav16(const std::string& path, const Pcm& pcm, int rate);

}  // namespace wsim::audio
