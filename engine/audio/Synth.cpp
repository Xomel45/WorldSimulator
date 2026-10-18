#include "audio/Synth.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace wsim::audio {

namespace {

constexpr float kPi  = std::numbers::pi_v<float>;
constexpr float kTau = 2.0f * kPi;

// Маленький детерминированный шум (xorshift), один на звук: результат одинаков на любой машине.
struct Noise {
    std::uint32_t s;
    explicit Noise(std::uint32_t seed) : s(seed ? seed : 1u) {}
    float next() {  // [-1, 1]
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return static_cast<float>(s & 0xFFFFFF) / static_cast<float>(0x800000) - 1.0f;
    }
};

// Однополюсный ФНЧ: a ~ 0..1 (меньше -- глуше).
struct OnePole {
    float y = 0;
    float process(float x, float a) { y += a * (x - y); return y; }
};

// Фильтр с переменным состоянием: из одного прохода получаем НЧ/полосу/ВЧ. f = 2 sin(pi fc / fs), q = 1/Q.
struct Svf {
    float lp = 0, bp = 0;
    struct Out { float lp, bp, hp; };
    Out process(float x, float f, float q) {
        const float hp = x - lp - q * bp;
        bp += f * hp;
        lp += f * bp;
        return {lp, bp, hp};
    }
};

float svfCoef(float fc, int rate) { return 2.0f * std::sin(kPi * std::min(fc, 0.45f * static_cast<float>(rate)) / static_cast<float>(rate)); }

// Атака линейно, затем экспоненциальный спад.
float envAD(float t, float attack, float decay) {
    return t < attack ? t / std::max(attack, 1.0e-6f) : std::exp(-(t - attack) / decay);
}
// Колокол sin^2 на отрезке [0, dur].
float window(float t, float dur) {
    if (t < 0.0f || t > dur) return 0.0f;
    const float s = std::sin(kPi * t / dur);
    return s * s;
}

Pcm blank(float seconds, int rate) { return Pcm(static_cast<std::size_t>(seconds * static_cast<float>(rate)), 0.0f); }

void normalize(Pcm& x, float peak) {
    float m = 1.0e-9f;
    for (float v : x) m = std::max(m, std::fabs(v));
    const float k = peak / m;
    for (float& v : x) v *= k;
}

// Мягкое ограничение: без щелчков от клиппинга.
void softClip(Pcm& x) {
    for (float& v : x) v = std::tanh(v);
}

// Петля без шва: берём запас длиной fade, последний кусок подмешиваем в начало (равная мощность).
// Результат короче на fade; конец плавно переходит в начало, потому что out[0] == x[L-fade] --
// естественный преемник последнего отсчёта.
Pcm makeSeamless(const Pcm& x, std::size_t fade) {
    const std::size_t L = x.size();
    Pcm out(L - fade);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (i < fade) {
            const float w = static_cast<float>(i) / static_cast<float>(fade);
            out[i] = x[i] * std::sin(0.5f * kPi * w) + x[L - fade + i] * std::cos(0.5f * kPi * w);
        } else {
            out[i] = x[i];
        }
    }
    return out;
}

// ─── Интерфейс ───────────────────────────────────────────────────────────────
Pcm uiHover(int rate) {
    Pcm x = blank(0.06f, rate);
    float ph = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float t = static_cast<float>(i) / rate;
        ph += kTau * (1700.0f + 500.0f * t / 0.06f) / rate;
        x[i] = std::sin(ph) * envAD(t, 0.003f, 0.012f);
    }
    normalize(x, 0.22f);
    return x;
}

Pcm uiClick(int rate) {
    Pcm x = blank(0.1f, rate);
    Noise n(11);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float t = static_cast<float>(i) / rate;
        const float tone = std::sin(kTau * 880.0f * t) + 0.6f * std::sin(kTau * 1320.0f * t);
        x[i] = tone * envAD(t, 0.001f, 0.022f) + n.next() * 0.5f * std::exp(-t / 0.002f);
    }
    normalize(x, 0.32f);
    return x;
}

Pcm uiBack(int rate) {
    Pcm x = blank(0.13f, rate);
    float ph = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float t = static_cast<float>(i) / rate;
        ph += kTau * (820.0f - 400.0f * t / 0.13f) / rate;
        x[i] = (std::sin(ph) + 0.3f * std::sin(2.0f * ph)) * envAD(t, 0.002f, 0.03f);
    }
    normalize(x, 0.3f);
    return x;
}

Pcm uiToggle(int rate) {
    Pcm x = blank(0.09f, rate);
    float ph = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float t = static_cast<float>(i) / rate;
        ph += kTau * (1050.0f + 350.0f * t / 0.09f) / rate;
        x[i] = std::sin(ph) * envAD(t, 0.002f, 0.02f);
    }
    normalize(x, 0.26f);
    return x;
}

// ─── Силы бога ───────────────────────────────────────────────────────────────
Pcm pop(int rate) {  // существо появилось: восходящий "пуп" и искорка
    Pcm x = blank(0.22f, rate);
    float ph = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float t = static_cast<float>(i) / rate;
        ph += kTau * (260.0f * std::exp(t * 10.5f)) / rate;
        float v = std::sin(ph) * envAD(t, 0.003f, 0.05f);
        if (t > 0.02f) v += 0.3f * std::sin(kTau * 2600.0f * (t - 0.02f)) * envAD(t - 0.02f, 0.002f, 0.04f);
        x[i] = v;
    }
    normalize(x, 0.5f);
    return x;
}

Pcm whoosh(int rate) {  // стереть: шум с проходящей полосой
    Pcm x = blank(0.5f, rate);
    Noise n(23);
    Svf f;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float t = static_cast<float>(i) / rate, u = t / 0.5f;
        const float fc = 3500.0f * std::pow(500.0f / 3500.0f, u);
        const auto o = f.process(n.next(), svfCoef(fc, rate), 0.6f);
        x[i] = o.bp * std::pow(window(t, 0.5f), 0.75f);
    }
    normalize(x, 0.5f);
    return x;
}

Pcm thud(int rate) {  // удар валуна: низкий "бум" с падающей частотой и хлопок шума
    Pcm x = blank(0.7f, rate);
    Noise n(31);
    OnePole lp;
    float ph = 0, ph2 = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float t = static_cast<float>(i) / rate;
        ph  += kTau * (38.0f + 62.0f * std::exp(-t * 9.0f)) / rate;
        ph2 += kTau * (60.0f + 110.0f * std::exp(-t * 14.0f)) / rate;
        const float body  = std::sin(ph) * envAD(t, 0.003f, 0.17f) + 0.5f * std::sin(ph2) * envAD(t, 0.002f, 0.09f);
        const float crack = lp.process(n.next(), 0.10f) * std::exp(-t / 0.045f) * 1.6f;
        x[i] = body + crack;
    }
    softClip(x);
    normalize(x, 0.9f);
    return x;
}

Pcm knock(int rate) {  // дом: два глухих стука по дереву
    Pcm x = blank(0.5f, rate);
    Noise n(41);
    OnePole lp;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float t = static_cast<float>(i) / rate;
        float v = 0;
        for (float t0 : {0.0f, 0.17f}) {
            const float tt = t - t0;
            if (tt < 0) continue;
            v += std::sin(kTau * (210.0f - 60.0f * tt) * tt) * envAD(tt, 0.001f, 0.05f);
            v += lp.process(n.next(), 0.3f) * std::exp(-tt / 0.012f) * 0.5f;
        }
        x[i] = v;
    }
    normalize(x, 0.6f);
    return x;
}

Pcm zap(int rate) {  // разряд: падающий тон с кольцевой модуляцией и потрескиванием
    Pcm x = blank(0.5f, rate);
    Noise n(53);
    float ph = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float t = static_cast<float>(i) / rate;
        ph += kTau * (2400.0f * std::exp(-t * 6.5f) + 180.0f) / rate;
        const float saw = 2.0f * (ph / kTau - std::floor(ph / kTau + 0.5f));
        const float ring = std::sin(kTau * 95.0f * t);
        float v = (0.55f * saw * (0.6f + 0.4f * ring) + 0.45f * n.next() * (n.next() > 0.5f ? 1.0f : 0.15f)) * envAD(t, 0.002f, 0.11f);
        x[i] = v;
    }
    softClip(x);
    normalize(x, 0.65f);
    return x;
}

Pcm chime(int rate) {  // колокольчик
    Pcm x = blank(1.3f, rate);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float t = static_cast<float>(i) / rate;
        x[i] = 0.6f * std::sin(kTau * 880.0f * t) * std::exp(-t / 0.55f) + 0.3f * std::sin(kTau * 1760.0f * t) * std::exp(-t / 0.35f) +
               0.2f * std::sin(kTau * 2430.0f * t) * std::exp(-t / 0.2f);
        x[i] *= std::min(1.0f, t / 0.003f);
    }
    normalize(x, 0.5f);
    return x;
}

// ─── Голоса существ ──────────────────────────────────────────────────────────
Pcm alarm(int rate, int v) {  // тревожный писк: восходяще-нисходящий свист с дрожью
    const float f0 = (v == 0 ? 1000.0f : v == 1 ? 1250.0f : 880.0f);
    const float vib = (v == 0 ? 28.0f : v == 1 ? 34.0f : 22.0f);
    const float dur = 0.3f;
    Pcm x = blank(dur, rate);
    Noise n(61 + static_cast<std::uint32_t>(v));
    float ph = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float t = static_cast<float>(i) / rate;
        const float f = f0 * (1.0f + 0.38f * std::sin(kPi * t / dur)) * (1.0f + 0.03f * std::sin(kTau * vib * t));
        ph += kTau * f / rate;
        x[i] = (std::sin(ph) + 0.4f * std::sin(2.0f * ph) + 0.12f * std::sin(3.0f * ph) + 0.04f * n.next()) * window(t, dur) * 0.9f;
    }
    normalize(x, 0.55f);
    return x;
}

Pcm coo(int rate, int v) {  // воркование: две мягкие ноты вверх
    const float f1 = (v == 0 ? 420.0f : v == 1 ? 370.0f : 470.0f);
    const float dur = 0.55f;
    Pcm x = blank(dur, rate);
    float ph1 = 0, ph2 = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float t = static_cast<float>(i) / rate;
        const float vib = 1.0f + 0.02f * std::sin(kTau * 6.5f * t);
        ph1 += kTau * f1 * vib / rate;
        ph2 += kTau * f1 * 1.26f * vib / rate;
        const float a = window(t, 0.28f), b = window(t - 0.23f, 0.32f);
        x[i] = (std::sin(ph1) + 0.25f * std::sin(2.0f * ph1) + 0.07f * std::sin(3.0f * ph1)) * a +
               (std::sin(ph2) + 0.25f * std::sin(2.0f * ph2) + 0.07f * std::sin(3.0f * ph2)) * b;
    }
    normalize(x, 0.5f);
    return x;
}

Pcm growl(int rate, int v) {  // рычание: низкая пила, модулированная и приглушённая, плюс шорох
    const float f0 = (v == 0 ? 95.0f : v == 1 ? 80.0f : 112.0f);
    const float dur = 0.5f;
    Pcm x = blank(dur, rate);
    Noise n(71 + static_cast<std::uint32_t>(v));
    OnePole lp, lpn;
    float ph = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float t = static_cast<float>(i) / rate;
        ph += kTau * f0 * (1.0f + 0.12f * std::sin(kTau * 3.0f * t)) / rate;
        const float saw = 2.0f * (ph / kTau - std::floor(ph / kTau + 0.5f));
        const float tone = lp.process(saw, 0.10f) * (0.6f + 0.4f * std::sin(kTau * 33.0f * t));
        const float hiss = lpn.process(n.next(), 0.22f) * 0.35f;
        x[i] = (tone * 2.0f + hiss) * window(t, dur);
    }
    softClip(x);
    normalize(x, 0.7f);
    return x;
}

Pcm chirp(int rate, int v) {  // птица: 2-3 быстрых восходящих "тью" с FM
    const float f0   = (v == 0 ? 2800.0f : v == 1 ? 3300.0f : v == 2 ? 2400.0f : 3600.0f);
    const int   reps = 2 + (v % 2);
    const float dur  = 0.09f * static_cast<float>(reps) + 0.05f;
    Pcm x = blank(dur, rate);
    float ph = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float t = static_cast<float>(i) / rate;
        const float k = std::fmod(t, 0.09f), idx = std::floor(t / 0.09f);
        if (idx >= static_cast<float>(reps)) break;
        const float f = f0 * (1.0f + 0.5f * k / 0.07f) * (1.0f + 0.01f * idx);
        ph += kTau * f / rate;
        x[i] = std::sin(ph + 1.8f * std::sin(0.5f * ph)) * window(k, 0.075f);
    }
    normalize(x, 0.4f);
    return x;
}

// ─── Петли ───────────────────────────────────────────────────────────────────
Pcm loopWind(int rate) {
    const float T = 10.0f, F = 1.5f;
    Pcm x = blank(T + F, rate);
    Noise n(101);
    OnePole brown;
    Svf f;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float t = static_cast<float>(i) / rate;
        const float gust = 0.5f + 0.5f * std::sin(kTau * 0.1f * t + 1.0f) * 0.7f + 0.3f * std::sin(kTau * 0.23f * t);
        const float fc = 300.0f + 380.0f * (0.5f + 0.5f * std::sin(kTau * 0.17f * t)) * gust;
        const float b  = brown.process(n.next(), 0.2f) * 3.0f;
        const auto  o  = f.process(b + 0.35f * n.next(), svfCoef(fc, rate), 0.42f);
        x[i] = o.bp * (0.45f + 0.55f * gust);
    }
    Pcm out = makeSeamless(x, static_cast<std::size_t>(F * static_cast<float>(rate)));
    normalize(out, 0.7f);
    return out;
}

Pcm loopWater(int rate) {
    const float T = 8.0f, F = 1.2f;
    Pcm x = blank(T + F, rate);
    Noise n(103), bub(107);
    Svf f1;
    OnePole sm;
    float blipT = -1.0f, blipF = 1000.0f;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float t = static_cast<float>(i) / rate;
        const float swell = 0.55f + 0.45f * std::sin(kTau * 0.2f * t) * std::sin(kTau * 0.07f * t + 0.5f);
        const float fc = 1100.0f + 600.0f * std::sin(kTau * 0.11f * t);
        const auto  o  = f1.process(n.next(), svfCoef(fc, rate), 0.5f);
        float v = o.bp * swell;
        // Редкие "пузырьки": короткие всплески тона.
        if (blipT < 0.0f && (bub.next() * 0.5f + 0.5f) < 6.0f / static_cast<float>(rate)) {
            blipT = 0.0f;
            blipF = 700.0f + 1400.0f * (bub.next() * 0.5f + 0.5f);
        }
        if (blipT >= 0.0f) {
            v += 0.5f * std::sin(kTau * blipF * (1.0f + 3.0f * blipT) * blipT) * std::exp(-blipT / 0.012f);
            blipT += 1.0f / static_cast<float>(rate);
            if (blipT > 0.08f) blipT = -1.0f;
        }
        x[i] = sm.process(v, 0.9f);
    }
    Pcm out = makeSeamless(x, static_cast<std::size_t>(F * static_cast<float>(rate)));
    normalize(out, 0.7f);
    return out;
}

// Музыка: медленный мягкий пэд, 24 с по кругу. Бесшовность по построению: все частоты кратны 1/24 Гц
// (в петле целое число периодов), огибающие аккордов -- cos^2 с круговым расстоянием по времени
// (в сумме дают 1), так что отсчёт N равен отсчёту 0 без всякой склейки.
Pcm loopMusic(int rate) {
    constexpr float T = 24.0f;
    const auto snap = [](float f) { return std::round(f * T) / T; };
    struct Chord { float center; float notes[5]; };
    const Chord chords[4] = {
        {3.0f,  {110.00f, 164.81f, 220.00f, 277.18f, 415.30f}},  // Amaj7
        {9.0f,  {92.50f, 138.59f, 185.00f, 220.00f, 329.63f}},   // F#m7
        {15.0f, {146.83f, 220.00f, 293.66f, 329.63f, 369.99f}},  // Dmaj7(9)
        {21.0f, {82.41f, 123.47f, 164.81f, 246.94f, 369.99f}},   // E(sus)
    };
    Pcm x = blank(T, rate);
    // Фазы копим в double: за 24 с аргумент синуса в float терял бы точность, и стык "плыл" бы.
    double phase[4][5][3] = {};
    const double step = 6.283185307179586 / static_cast<double>(rate);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float t = static_cast<float>(i) / rate;
        float v = 0;
        for (int ci = 0; ci < 4; ++ci) {
            const Chord& c = chords[ci];
            float d = std::fabs(t - c.center);
            d = std::min(d, T - d);
            for (int k = 0; k < 5; ++k) {
                const double f = static_cast<double>(snap(c.notes[k]));
                phase[ci][k][0] += step * f;
                phase[ci][k][1] += step * (f + 0.5);
                phase[ci][k][2] += step * 2.0 * f;
            }
            if (d >= 6.0f) continue;
            const float c2 = std::cos(kPi * d / 12.0f);
            const float w  = c2 * c2;
            for (int k = 0; k < 5; ++k) {
                // Каждой ноте -- своя тихая "дышащая" модуляция (частота кратна 1/24 Гц).
                const float breath = 0.78f + 0.22f * std::sin(kTau * (2.0f + static_cast<float>(k)) / T * 2.0f * t + static_cast<float>(k));
                const float amp = 0.20f / (1.0f + 0.35f * static_cast<float>(k));
                v += w * breath * amp *
                     (static_cast<float>(std::sin(phase[ci][k][0])) + 0.5f * static_cast<float>(std::sin(phase[ci][k][1])) +
                      0.28f * static_cast<float>(std::sin(phase[ci][k][2])));
            }
        }
        x[i] = v;
    }
    softClip(x);
    normalize(x, 0.6f);
    return x;
}

Pcm loopRumble(int rate) {
    const float T = 4.0f, F = 0.8f;
    Pcm x = blank(T + F, rate);
    Noise n(109);
    OnePole brown, lp2;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float t = static_cast<float>(i) / rate;
        const float b = lp2.process(brown.process(n.next(), 0.04f), 0.5f) * 8.0f;
        x[i] = b * (0.7f + 0.3f * std::sin(kTau * 0.5f * t));
    }
    Pcm out = makeSeamless(x, static_cast<std::size_t>(F * static_cast<float>(rate)));
    // Низкие тоны с целым числом периодов в петле -- добавляем после склейки.
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float t = static_cast<float>(i) / rate;
        out[i] += 0.35f * std::sin(kTau * 41.0f * t) + 0.22f * std::sin(kTau * 55.0f * t + 1.0f);
    }
    softClip(out);
    normalize(out, 0.75f);
    return out;
}

// Ночные сверчки: два "голоса" с трескучим пульсом, все частоты кратны 1/T -- петля замкнута по построению.
Pcm loopCrickets(int rate) {
    const float T = 6.0f;
    Pcm x = blank(T, rate);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const double g1 = std::pow(std::max(0.0, std::sin(6.283185307 * 30.0 * t)), 2.0) * (0.5 + 0.5 * std::sin(6.283185307 * (1.0 / 3.0) * t));
        const double g2 = std::pow(std::max(0.0, std::sin(6.283185307 * 24.0 * t + 1.0)), 2.0) * (0.5 + 0.5 * std::sin(6.283185307 * (1.0 / 2.0) * t + 2.0));
        x[i] = static_cast<float>(0.5 * g1 * std::sin(6.283185307 * 4200.0 * t) + 0.4 * g2 * std::sin(6.283185307 * 3900.0 * t));
    }
    normalize(x, 0.5f);
    return x;
}

// Потрескивание костра: тихий низкий гул и редкие щелчки.
Pcm loopFire(int rate) {
    const float T = 6.0f, F = 1.0f;
    Pcm x = blank(T + F, rate);
    Noise n(131), c(137);
    OnePole brown;
    Svf f;
    float click = 0.0f;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float base = brown.process(n.next(), 0.07f) * 1.6f;
        if ((c.next() * 0.5f + 0.5f) < 16.0f / static_cast<float>(rate)) click = 0.5f + 0.5f * (c.next() * 0.5f + 0.5f);
        click *= std::exp(-1.0f / (0.0035f * static_cast<float>(rate)));
        const auto o = f.process(n.next(), svfCoef(2400.0f, rate), 0.7f);
        x[i] = 0.5f * base + click * o.bp * 1.4f;
    }
    Pcm out = makeSeamless(x, static_cast<std::size_t>(F * static_cast<float>(rate)));
    normalize(out, 0.55f);
    return out;
}

// Дождь: ровный шум полосой 1.5-7 кГц с мелкой рябью капель.
Pcm loopRain(int rate) {
    const float T = 5.0f, F = 1.0f;
    Pcm x = blank(T + F, rate);
    Noise n(151), d(157);
    Svf hi, lo;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const auto a = hi.process(n.next(), svfCoef(1800.0f, rate), 0.6f);
        const auto b = lo.process(a.hp, svfCoef(7000.0f, rate), 0.6f);
        const float drip = 0.75f + 0.25f * d.next();
        x[i] = b.lp * drip;
    }
    Pcm out = makeSeamless(x, static_cast<std::size_t>(F * static_cast<float>(rate)));
    normalize(out, 0.6f);
    return out;
}

}  // namespace

bool isLoop(SoundId id) noexcept {
    return id == SoundId::LoopWind || id == SoundId::LoopWater || id == SoundId::LoopMusic || id == SoundId::LoopRumble || id == SoundId::LoopCrickets || id == SoundId::LoopFire || id == SoundId::LoopRain;
}

bool soundFromName(std::string_view name, SoundId& out) noexcept {
    struct Entry { std::string_view name; SoundId id; };
    static constexpr Entry kNames[] = {
        {"pop", SoundId::Pop}, {"whoosh", SoundId::Whoosh}, {"thud", SoundId::Thud},
        {"knock", SoundId::Knock}, {"zap", SoundId::Zap}, {"chime", SoundId::Chime},
    };
    for (const auto& e : kNames)
        if (e.name == name) { out = e.id; return true; }
    return false;
}

Pcm synthesize(SoundId id, int rate) {
    switch (id) {
        case SoundId::UiHover:  return uiHover(rate);
        case SoundId::UiClick:  return uiClick(rate);
        case SoundId::UiBack:   return uiBack(rate);
        case SoundId::UiToggle: return uiToggle(rate);
        case SoundId::Pop:      return pop(rate);
        case SoundId::Whoosh:   return whoosh(rate);
        case SoundId::Thud:     return thud(rate);
        case SoundId::Knock:    return knock(rate);
        case SoundId::Zap:      return zap(rate);
        case SoundId::Chime:    return chime(rate);
        case SoundId::Alarm0: case SoundId::Alarm1: case SoundId::Alarm2: return alarm(rate, static_cast<int>(id) - static_cast<int>(SoundId::Alarm0));
        case SoundId::Coo0: case SoundId::Coo1: case SoundId::Coo2:       return coo(rate, static_cast<int>(id) - static_cast<int>(SoundId::Coo0));
        case SoundId::Growl0: case SoundId::Growl1: case SoundId::Growl2: return growl(rate, static_cast<int>(id) - static_cast<int>(SoundId::Growl0));
        case SoundId::Chirp0: case SoundId::Chirp1: case SoundId::Chirp2: case SoundId::Chirp3:
            return chirp(rate, static_cast<int>(id) - static_cast<int>(SoundId::Chirp0));
        case SoundId::LoopWind:   return loopWind(rate);
        case SoundId::LoopWater:  return loopWater(rate);
        case SoundId::LoopMusic:  return loopMusic(rate);
        case SoundId::LoopRumble: return loopRumble(rate);
        case SoundId::LoopCrickets: return loopCrickets(rate);
        case SoundId::LoopFire:   return loopFire(rate);
        case SoundId::LoopRain:   return loopRain(rate);
        case SoundId::Count: break;
    }
    return {};
}

std::vector<Pcm> synthesizeAll(int rate) {
    std::vector<Pcm> all(static_cast<std::size_t>(kSoundCount));
    for (int i = 0; i < kSoundCount; ++i) all[static_cast<std::size_t>(i)] = synthesize(static_cast<SoundId>(i), rate);
    return all;
}

bool writeWav16(const std::string& path, const Pcm& pcm, int rate) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const auto put32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    const auto put16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const std::uint32_t bytes = static_cast<std::uint32_t>(pcm.size() * 2);
    std::fwrite("RIFF", 1, 4, f); put32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    put32(16); put16(1); put16(1); put32(static_cast<std::uint32_t>(rate)); put32(static_cast<std::uint32_t>(rate) * 2); put16(2); put16(16);
    std::fwrite("data", 1, 4, f); put32(bytes);
    for (float v : pcm) {
        const auto s = static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
        std::fwrite(&s, 2, 1, f);
    }
    std::fclose(f);
    return true;
}

}  // namespace wsim::audio
