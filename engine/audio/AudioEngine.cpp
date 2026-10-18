#include "audio/AudioEngine.hpp"

#include "core/Logger.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <future>

namespace wsim::audio {

namespace {
constexpr std::size_t kMaxVoices = 32;

struct Voice {
    ma_audio_buffer_ref ref{};
    ma_sound            sound{};
    bool                inUse = false;
};

struct Loop {
    ma_audio_buffer_ref ref{};
    ma_sound            sound{};
    bool                started = false;
    float               current = 0.0f;
    float               target  = 0.0f;
};
}  // namespace

struct AudioEngine::Impl {
    bool        ok = false;
    std::string status;
    ma_engine   engine{};
    ma_sound_group groups[3]{};
    bool        groupsInit = false;
    int         rate = 48000;

    std::future<std::vector<Pcm>> pending;  // синтез в фоне
    std::vector<Pcm>              bank;
    bool                          ready = false;
    long                          played = 0, dropped = 0;  // статистика для лога при выходе

    std::vector<std::unique_ptr<Voice>> voices;
    std::array<Loop, 6>                 loops{};  // LoopWind, LoopWater, LoopMusic, LoopRumble, LoopCrickets, LoopFire

    static int loopIndex(SoundId id) { return static_cast<int>(id) - static_cast<int>(SoundId::LoopWind); }
    static Group loopGroup(SoundId id) { return id == SoundId::LoopRumble ? Group::Effects : Group::Ambient; }

    void initLoops() {
        for (int i = 0; i < 6; ++i) {
            const SoundId id = static_cast<SoundId>(static_cast<int>(SoundId::LoopWind) + i);
            const Pcm&    pcm = bank[static_cast<std::size_t>(id)];
            Loop&         l = loops[static_cast<std::size_t>(i)];
            if (ma_audio_buffer_ref_init(ma_format_f32, 1, pcm.data(), pcm.size(), &l.ref) != MA_SUCCESS) continue;
            l.ref.sampleRate = static_cast<ma_uint32>(rate);
            if (ma_sound_init_from_data_source(&engine, &l.ref, MA_SOUND_FLAG_NO_SPATIALIZATION, &groups[static_cast<int>(loopGroup(id))], &l.sound) != MA_SUCCESS) {
                ma_audio_buffer_ref_uninit(&l.ref);
                continue;
            }
            ma_sound_set_looping(&l.sound, MA_TRUE);
            ma_sound_set_volume(&l.sound, 0.0f);
            ma_sound_start(&l.sound);
            l.started = true;
        }
    }
};

AudioEngine::AudioEngine() : m_impl(std::make_unique<Impl>()) {
    Impl& im = *m_impl;
    if (const char* off = std::getenv("WSIM_NOAUDIO"); off && *off) {
        im.status = "отключён (WSIM_NOAUDIO)";
        return;
    }
    ma_engine_config cfg = ma_engine_config_init();
    if (const ma_result r = ma_engine_init(&cfg, &im.engine); r != MA_SUCCESS) {
        im.status = std::string("нет звукового устройства: ") + ma_result_description(r);
        Logger::warn("Звук отключён: {}", im.status);
        return;
    }
    im.ok   = true;
    im.rate = static_cast<int>(ma_engine_get_sample_rate(&im.engine));

    char name[256] = {};
    if (ma_device* dev = ma_engine_get_device(&im.engine))
        ma_device_get_name(dev, ma_device_type_playback, name, sizeof name, nullptr);
    im.status = name[0] ? name : "звуковое устройство по умолчанию";

    for (auto& g : im.groups) ma_sound_group_init(&im.engine, 0, nullptr, &g);
    im.groupsInit = true;
    im.voices.reserve(kMaxVoices);
    for (std::size_t i = 0; i < kMaxVoices; ++i) im.voices.push_back(std::make_unique<Voice>());

    // Синтез всех звуков -- в фоне, на частоте устройства.
    im.pending = std::async(std::launch::async, [rate = im.rate] { return synthesizeAll(rate); });
    Logger::info("Звук: {} ({} Гц)", im.status, im.rate);
}

AudioEngine::~AudioEngine() {
    Impl& im = *m_impl;
    if (im.pending.valid()) im.pending.wait();
    if (!im.ok) return;
    Logger::info("Звук: сыграно одиночных {}, не хватило голосов {}", im.played, im.dropped);
    for (auto& l : im.loops)
        if (l.started) { ma_sound_uninit(&l.sound); ma_audio_buffer_ref_uninit(&l.ref); }
    for (auto& v : im.voices)
        if (v->inUse) { ma_sound_uninit(&v->sound); ma_audio_buffer_ref_uninit(&v->ref); }
    if (im.groupsInit)
        for (auto& g : im.groups) ma_sound_group_uninit(&g);
    ma_engine_uninit(&im.engine);
}

bool AudioEngine::ok() const noexcept { return m_impl->ok; }
bool AudioEngine::ready() const noexcept { return m_impl->ready; }
const std::string& AudioEngine::status() const noexcept { return m_impl->status; }

void AudioEngine::setListener(const glm::vec3& pos, const glm::vec3& forward) {
    Impl& im = *m_impl;
    if (!im.ok) return;
    ma_engine_listener_set_position(&im.engine, 0, pos.x, pos.y, pos.z);
    ma_engine_listener_set_direction(&im.engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&im.engine, 0, 0.0f, 1.0f, 0.0f);
}

void AudioEngine::setVolumes(float master, float ambient, float creatures, float effects) {
    Impl& im = *m_impl;
    if (!im.ok) return;
    ma_engine_set_volume(&im.engine, std::clamp(master, 0.0f, 1.0f));
    ma_sound_group_set_volume(&im.groups[0], std::clamp(ambient, 0.0f, 1.0f));
    ma_sound_group_set_volume(&im.groups[1], std::clamp(creatures, 0.0f, 1.0f));
    ma_sound_group_set_volume(&im.groups[2], std::clamp(effects, 0.0f, 1.0f));
}

void AudioEngine::play(SoundId id, Group group, float volume, float pitch, const glm::vec3* pos, float minDist, float maxDist) {
    Impl& im = *m_impl;
    if (!im.ok || !im.ready || isLoop(id) || id == SoundId::Count) return;
    const Pcm& pcm = im.bank[static_cast<std::size_t>(id)];
    if (pcm.empty()) return;

    Voice* free = nullptr;
    for (auto& v : im.voices) {
        if (v->inUse && ma_sound_at_end(&v->sound)) {
            ma_sound_uninit(&v->sound);
            ma_audio_buffer_ref_uninit(&v->ref);
            v->inUse = false;
        }
        if (!v->inUse && !free) free = v.get();
    }
    if (!free) { ++im.dropped; return; }  // все голоса заняты -- новый просто не звучит

    if (ma_audio_buffer_ref_init(ma_format_f32, 1, pcm.data(), pcm.size(), &free->ref) != MA_SUCCESS) return;
    free->ref.sampleRate = static_cast<ma_uint32>(im.rate);
    const ma_uint32 flags = pos ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (ma_sound_init_from_data_source(&im.engine, &free->ref, flags, &im.groups[static_cast<int>(group)], &free->sound) != MA_SUCCESS) {
        ma_audio_buffer_ref_uninit(&free->ref);
        return;
    }
    free->inUse = true;
    ma_sound_set_volume(&free->sound, std::max(volume, 0.0f));
    ma_sound_set_pitch(&free->sound, std::clamp(pitch, 0.25f, 4.0f));
    if (pos) {
        ma_sound_set_position(&free->sound, pos->x, pos->y, pos->z);
        ma_sound_set_attenuation_model(&free->sound, ma_attenuation_model_linear);
        ma_sound_set_min_distance(&free->sound, minDist);
        ma_sound_set_max_distance(&free->sound, std::max(maxDist, minDist + 1.0f));
        ma_sound_set_rolloff(&free->sound, 1.0f);
        ma_sound_set_min_gain(&free->sound, 0.0f);
        ma_sound_set_doppler_factor(&free->sound, 0.0f);
    }
    ma_sound_start(&free->sound);
    ++im.played;
}

void AudioEngine::setLoop(SoundId loop, float volume) {
    if (!isLoop(loop)) return;
    m_impl->loops[static_cast<std::size_t>(Impl::loopIndex(loop))].target = std::clamp(volume, 0.0f, 1.0f);
}

void AudioEngine::update(float dt) {
    Impl& im = *m_impl;
    if (!im.ok) return;

    if (!im.ready && im.pending.valid() && im.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        im.bank  = im.pending.get();
        im.ready = true;
        im.initLoops();
        Logger::info("Звуки синтезированы ({} штук)", im.bank.size());
    }
    if (!im.ready) return;

    for (auto& v : im.voices)
        if (v->inUse && ma_sound_at_end(&v->sound)) {
            ma_sound_uninit(&v->sound);
            ma_audio_buffer_ref_uninit(&v->ref);
            v->inUse = false;
        }
    const float k = 1.0f - std::exp(-std::min(dt, 0.25f) * 2.5f);  // плавное изменение громкости петель
    for (auto& l : im.loops) {
        if (!l.started) continue;
        l.current += (l.target - l.current) * k;
        ma_sound_set_volume(&l.sound, l.current);
    }
}

void AudioEngine::dumpBank(const std::string& dir) const {
    const Impl& im = *m_impl;
    const int   rate = im.ok ? im.rate : 48000;
    static const char* names[] = {"ui_hover", "ui_click", "ui_back", "ui_toggle", "pop", "whoosh", "thud", "knock", "zap", "chime",
                                  "alarm0", "alarm1", "alarm2", "coo0", "coo1", "coo2", "growl0", "growl1", "growl2",
                                  "chirp0", "chirp1", "chirp2", "chirp3", "loop_wind", "loop_water", "loop_music", "loop_rumble", "loop_crickets", "loop_fire"};
    static_assert(sizeof(names) / sizeof(names[0]) == static_cast<std::size_t>(kSoundCount));
    for (int i = 0; i < kSoundCount; ++i)
        writeWav16(dir + "/" + names[i] + ".wav", synthesize(static_cast<SoundId>(i), rate), rate);
    Logger::info("Звуки записаны в {}", dir);
}

}  // namespace wsim::audio
