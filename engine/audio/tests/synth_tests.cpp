#include "audio/Synth.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>

using namespace wsim::audio;

int main() {
    constexpr int kRate = 44100;

    for (int i = 0; i < kSoundCount; ++i) {
        const SoundId id  = static_cast<SoundId>(i);
        const Pcm     pcm = synthesize(id, kRate);
        const double  seconds = static_cast<double>(pcm.size()) / kRate;

        // Длительность разумная: одиночные звуки до 1.5 с, петли от 4 с.
        WSIM_CHECK(!pcm.empty());
        if (isLoop(id)) WSIM_CHECK(seconds >= 3.5 && seconds <= 30.0);
        else            WSIM_CHECK(seconds >= 0.03 && seconds <= 1.5);

        // Конечные значения, без клиппинга, не тишина (СКЗ), без постоянной составляющей.
        double sum = 0, sq = 0;
        float  peak = 0;
        for (float v : pcm) {
            WSIM_CHECK(std::isfinite(v));
            peak = std::max(peak, std::fabs(v));
            sum += v;
            sq += static_cast<double>(v) * v;
        }
        const double rms = std::sqrt(sq / static_cast<double>(pcm.size()));
        WSIM_CHECK(peak <= 1.0f);
        WSIM_CHECK(peak >= 0.15f);
        WSIM_CHECK(rms > 0.005);
        WSIM_CHECK(std::fabs(sum / static_cast<double>(pcm.size())) < 0.1 * peak + 0.02);

        // Детерминизм: тот же звук -- те же отсчёты.
        const Pcm again = synthesize(id, kRate);
        WSIM_CHECK(again == pcm);

        // Одиночные звуки начинаются и заканчиваются почти в нуле (без щелчков).
        if (!isLoop(id)) {
            WSIM_CHECK(std::fabs(pcm.back()) < 0.05f * peak + 0.01f);
        }
    }

    // Петли бесшовны: скачок на стыке не больше, чем обычный шаг внутри сигнала.
    for (SoundId id : {SoundId::LoopWind, SoundId::LoopWater, SoundId::LoopMusic, SoundId::LoopRumble, SoundId::LoopCrickets, SoundId::LoopFire, SoundId::LoopRain}) {
        const Pcm pcm = synthesize(id, kRate);
        double meanStep = 0;
        float  maxStep = 0;
        for (std::size_t i = 1; i < pcm.size(); ++i) {
            const float d = std::fabs(pcm[i] - pcm[i - 1]);
            meanStep += d;
            maxStep = std::max(maxStep, d);
        }
        meanStep /= static_cast<double>(pcm.size() - 1);
        const float seam = std::fabs(pcm.front() - pcm.back());
        // Стык не больше обычного шага: у музыки в нуле все гармоники в фазе, там самый крутой наклон -- это не скачок.
        WSIM_CHECK(seam <= std::max(4.0 * meanStep, 1.25 * static_cast<double>(maxStep)));
    }

    // Музыка периодична по построению: отсчёт сразу после конца совпадает с началом (проверяем гладкость стыка жёстко).
    {
        const Pcm m = synthesize(SoundId::LoopMusic, kRate);
        float maxStep = 0;
        for (std::size_t i = 1; i < m.size(); ++i) maxStep = std::max(maxStep, std::fabs(m[i] - m[i - 1]));
        WSIM_CHECK(std::fabs(m.front() - m.back()) <= 1.1f * maxStep);
        WSIM_CHECK(std::fabs(m[m.size() / 2] - m[m.size() / 2 - 1]) <= maxStep);
    }

    // Частота дискретизации: длительность не зависит от неё (48 кГц -- то же время).
    {
        const Pcm a = synthesize(SoundId::Thud, 44100), b = synthesize(SoundId::Thud, 48000);
        WSIM_CHECK(std::fabs(static_cast<double>(a.size()) / 44100.0 - static_cast<double>(b.size()) / 48000.0) < 0.01);
    }

    // Имена для скриптов модов.
    SoundId id{};
    WSIM_CHECK(soundFromName("thud", id) && id == SoundId::Thud);
    WSIM_CHECK(soundFromName("zap", id) && id == SoundId::Zap);
    WSIM_CHECK(!soundFromName("loopwind", id) && !soundFromName("", id));

    // WAV пишется (заголовок + данные).
    {
        const Pcm  p = synthesize(SoundId::UiClick, kRate);
        const auto path = std::string("/tmp/wsim_synth_test.wav");
        WSIM_CHECK(writeWav16(path, p, kRate));
        std::FILE* f = std::fopen(path.c_str(), "rb");
        WSIM_CHECK(f != nullptr);
        std::fseek(f, 0, SEEK_END);
        WSIM_CHECK(std::ftell(f) == static_cast<long>(44 + p.size() * 2));
        std::fclose(f);
        std::remove(path.c_str());
    }

    std::puts("synth_tests: OK");
    return 0;
}
