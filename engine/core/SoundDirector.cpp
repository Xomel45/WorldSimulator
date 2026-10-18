#include "core/SoundDirector.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace wsim {

using audio::Group;
using audio::SoundId;

namespace {
constexpr float kAlarmThreshold  = 0.65f;  // выходы мозга ~0.45 +- 0.2: порог ловит "всплески"
constexpr float kMatingThreshold = 0.65f;
constexpr float kDomThreshold    = 0.75f;
}  // namespace

SoundDirector::SoundDirector(audio::AudioEngine& audio) : m_audio(audio) {}

void SoundDirector::ui(UiSound sound) {
    switch (sound) {
        case UiSound::Hover:  m_audio.play(SoundId::UiHover, Group::Effects, 0.7f); break;
        case UiSound::Click:  m_audio.play(SoundId::UiClick, Group::Effects, 0.9f); break;
        case UiSound::Back:   m_audio.play(SoundId::UiBack, Group::Effects, 0.9f); break;
        case UiSound::Toggle: m_audio.play(SoundId::UiToggle, Group::Effects, 0.9f); break;
    }
}

void SoundDirector::impact(const ImpactEvent& hit) {
    // Громче и ниже у тяжёлых валунов; слышно издалека (это событие мира).
    const float size   = std::clamp(hit.radius, 0.5f, 6.0f);
    const float pitch  = std::clamp(1.35f - 0.14f * size, 0.55f, 1.3f);
    const float volume = std::clamp(0.45f + 0.12f * size, 0.5f, 1.0f);
    effect(SoundId::Thud, hit.pos, volume, pitch);
}

void SoundDirector::effect(SoundId id, const glm::vec3& pos, float volume, float pitch) {
    const float minD = 6.0f + 0.2f * m_camDistance;
    m_audio.play(id, Group::Effects, volume, pitch, &pos, minD, minD * 8.0f + 120.0f);
}

bool SoundDirector::playNamed(std::string_view name, const glm::vec3& pos, float volume) {
    SoundId id{};
    if (!audio::soundFromName(name, id)) return false;
    effect(id, pos, std::clamp(volume, 0.0f, 1.0f));
    return true;
}

void SoundDirector::update(float dt, bool playing, const glm::vec3& focus, const glm::vec3& forward, float camDistance,
                           const World& world, const Terrain& terrain) {
    m_playing     = playing;
    m_focus       = focus;
    m_camDistance = camDistance;
    m_time += dt;

    // Слушатель на земле под камерой, смотрит туда же, куда камера (по горизонтали): панорама совпадает с экраном.
    glm::vec3 fwd(forward.x, 0.0f, forward.z);
    fwd = glm::length(fwd) > 1.0e-4f ? glm::normalize(fwd) : glm::vec3(0.0f, 0.0f, -1.0f);
    m_audio.setListener(focus + glm::vec3(0.0f, 1.0f, 0.0f), fwd);

    ambient(dt, playing, focus, camDistance, world, terrain);
    if (playing) creatureVoices(dt, world);
    m_audio.update(dt);
}

void SoundDirector::ambient(float dt, bool playing, const glm::vec3& focus, float camDistance, const World& world, const Terrain& terrain) {
    using audio::SoundId;
    // Музыка есть всегда (и в меню); живой мир (ветер, вода, птицы) -- только в игре.
    m_audio.setLoop(SoundId::LoopMusic, 0.55f);
    if (!playing) {
        m_audio.setLoop(SoundId::LoopWind, 0.0f);
        m_audio.setLoop(SoundId::LoopWater, 0.0f);
        m_audio.setLoop(SoundId::LoopRumble, 0.0f);
        m_audio.setLoop(SoundId::LoopCrickets, 0.0f);
        m_audio.setLoop(SoundId::LoopFire, 0.0f);
        m_audio.setLoop(SoundId::LoopRain, 0.0f);
        return;
    }

    // Ночь: сверчки (на суше, не над водой и не высоко), у костров -- потрескивание по близости к ближайшему.
    {
        float fire = 0.0f;
        for (const Structure& s : world.structures())
            if (s.kind == StructureKind::Campfire && s.hp > 0.02f)
                fire = std::max(fire, 1.0f - std::hypot(s.x - focus.x, s.z - focus.z) / (30.0f + 0.4f * camDistance));
        const float land = terrain.isUnderwater(focus.x, focus.z) ? 0.2f : 1.0f;
        m_audio.setLoop(SoundId::LoopCrickets, 0.4f * world.nightness() * land * (1.0f - std::clamp((camDistance - 20.0f) / 150.0f, 0.0f, 0.8f)));
        m_audio.setLoop(SoundId::LoopFire, 0.55f * std::clamp(fire, 0.0f, 1.0f));
        m_audio.setLoop(SoundId::LoopRain, 0.6f * world.rain());
    }

    // Ветер крепчает с высотой камеры и дышит порывами.
    const float gust = 0.5f + 0.5f * std::sin(m_time * 0.21f) * std::sin(m_time * 0.083f + 1.0f);
    m_audio.setLoop(SoundId::LoopWind, 0.12f + 0.45f * std::clamp((camDistance - 10.0f) / 160.0f, 0.0f, 1.0f) + 0.12f * gust);

    // Вода шумит по доле воды вокруг точки обзора (кольцо точек: дёшево, без обхода рельефа).
    int wet = 0, total = 0;
    for (int ring = 0; ring < 2; ++ring) {
        const float r = (ring == 0 ? 0.0f : 14.0f + 0.25f * camDistance);
        const int   n = (ring == 0 ? 1 : 12);
        for (int i = 0; i < n; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
            ++total;
            if (terrain.isUnderwater(focus.x + std::cos(a) * r, focus.z + std::sin(a) * r)) ++wet;
        }
    }
    m_audio.setLoop(SoundId::LoopWater, 0.75f * std::clamp(1.8f * static_cast<float>(wet) / static_cast<float>(total), 0.0f, 1.0f));

    // Гул рельефа, пока его лепят.
    m_audio.setLoop(SoundId::LoopRumble, 0.8f * std::clamp(m_rumbleTarget, 0.0f, 1.0f));

    // Птицы: редкие посвисты неподалёку от точки обзора, на суше.
    m_birdTimer -= dt;
    if (m_birdTimer <= 0.0f) {
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        m_birdTimer = 3.0f + 8.0f * u(m_rng);
        const float a = 6.2831853f * u(m_rng), r = (10.0f + 0.5f * camDistance) * (0.4f + 0.6f * u(m_rng));
        const float x = focus.x + std::cos(a) * r, z = focus.z + std::sin(a) * r;
        // В пустыне и тундре птиц мало, в остальных биомах -- как обычно.
        const Biome biome = terrain.biomeAt(x, z);
        const float birdy = biome == Biome::Desert ? 0.2f : biome == Biome::Tundra ? 0.4f : 1.0f;
        if (!terrain.isUnderwater(x, z) && u(m_rng) < birdy) {
            const glm::vec3 pos{x, terrain.heightAt(x, z) + 4.0f, z};
            const auto      id = static_cast<SoundId>(static_cast<int>(SoundId::Chirp0) + static_cast<int>(u(m_rng) * 3.99f));
            m_audio.play(id, Group::Ambient, 0.45f, 0.9f + 0.3f * u(m_rng), &pos, 12.0f + 0.2f * camDistance, 110.0f + camDistance);
        }
    }
}

void SoundDirector::creatureVoices(float dt, const World& world) {
    m_voiceBudget = std::min(m_voiceBudget + dt * 3.0f, 3.0f);  // не больше ~3 новых голосов в секунду
    const float hearing = 40.0f + 2.5f * m_camDistance;         // дальше -- не озвучиваем вовсе
    std::uniform_real_distribution<float> u(0.0f, 1.0f);

    for (const Inhabitant& in : world.population()) {
        const Creature& c = in.creature;
        VoiceState& st = m_voices[c.id];
        st.cooldown = std::max(0.0f, st.cooldown - dt);

        // Подаём голос на "фронт": выход мозга перешёл порог снизу вверх (а не пока он держится выше).
        const bool alarm  = in.lastOutputs.alarmCall > kAlarmThreshold;
        const bool mating = in.lastOutputs.matingCall > kMatingThreshold;
        const bool dom    = in.lastOutputs.dominanceDisplay > kDomThreshold;
        const bool alarmEdge = alarm && !st.alarmHigh, matingEdge = mating && !st.matingHigh, domEdge = dom && !st.domHigh;
        st.alarmHigh = alarm; st.matingHigh = mating; st.domHigh = dom;

        if (!(alarmEdge || matingEdge || domEdge) || st.cooldown > 0.0f || m_voiceBudget < 1.0f) continue;
        const glm::vec3 pos = c.position + glm::vec3(0.0f, 0.5f, 0.0f);
        if (glm::length(glm::vec2(pos.x - m_focus.x, pos.z - m_focus.z)) > hearing) continue;

        // Тембр по id, высота по размеру: мелкие пищат, крупные басят.
        const int   variant = static_cast<int>(c.id % 3);
        const float size01  = std::clamp((c.size() - 0.7f) / 0.7f, 0.0f, 1.0f);
        const float pitch   = std::clamp(1.35f - 0.65f * size01 + 0.08f * (u(m_rng) - 0.5f), 0.6f, 1.45f);
        SoundId     id      = SoundId::Coo0;
        if (alarmEdge)       id = static_cast<SoundId>(static_cast<int>(SoundId::Alarm0) + variant);
        else if (matingEdge) id = static_cast<SoundId>(static_cast<int>(SoundId::Coo0) + variant);
        else                 id = static_cast<SoundId>(static_cast<int>(SoundId::Growl0) + variant);
        m_audio.play(id, Group::Creatures, alarmEdge ? 0.9f : 0.75f, pitch, &pos, 6.0f + 0.2f * m_camDistance, 60.0f + 2.0f * m_camDistance);
        st.cooldown = 4.0f + 5.0f * u(m_rng);
        m_voiceBudget -= 1.0f;
    }

    // Раз в несколько секунд забываем умерших.
    m_pruneTimer += dt;
    if (m_pruneTimer > 6.0f) {
        m_pruneTimer = 0.0f;
        std::unordered_set<std::uint64_t> alive;
        for (const auto& in : world.population()) alive.insert(in.creature.id);
        std::erase_if(m_voices, [&](const auto& kv) { return !alive.contains(kv.first); });
    }
}

}  // namespace wsim
