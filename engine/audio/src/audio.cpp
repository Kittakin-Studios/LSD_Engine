#include "lsd/audio/audio.h"

#include "lsd/foundation/log.h"

#include <miniaudio.h>

#include <cstdio>
#include <cstring>

namespace lsd::audio {

struct AudioSystem::Impl {
    ma_engine engine;
    bool ready = false;
    bool device = false;
    ma_sound_group groups[kMaxClasses];  // [0] unused: Master is the engine itself
    char class_names[kMaxClasses][32];
    u32 class_count = 0;
    struct Concurrency {
        u32 max_voices, when_full;
    };
    Concurrency concurrency[64];
    u32 concurrency_count = 0;
    struct Voice {
        bool used;
        u32 id;
        ma_sound sound;
        char name[48];
        u32 sound_class;
        u32 group;
        f32 volume;
        u64 started;  // play order, for Stop Oldest
        bool spatial;
        Vec3 position;
    };
    Voice* voices = nullptr;
    u32 next_id = 1;
    u64 play_counter = 0;
    Vec3 listener;

    void free_voice(Voice& v) {
        if (!v.used) return;
        ma_sound_uninit(&v.sound);
        v.used = false;
    }
    f32 loudness(const Voice& v) const {  // rough: own volume over distance, for Stop Quietest
        const f32 d = v.spatial ? length(v.position - listener) : 0.0f;
        return v.volume / (d > 1.0f ? d : 1.0f);
    }
};

AudioSystem::AudioSystem() : impl_(new Impl()) {}
AudioSystem::~AudioSystem() {
    shutdown();
    delete impl_;
}

bool AudioSystem::init(bool use_device) {
    shutdown();
    ma_engine_config config = ma_engine_config_init();
    config.channels = 2;
    config.sampleRate = 48000;
    config.noDevice = use_device ? MA_FALSE : MA_TRUE;
    ma_result result = ma_engine_init(&config, &impl_->engine);
    if (result != MA_SUCCESS && use_device) {  // no sound card: keep going without one
        LSD_LOG_WARN("audio", "No audio device (%s): sounds are silent", ma_result_description(result));
        config.noDevice = MA_TRUE;
        result = ma_engine_init(&config, &impl_->engine);
        use_device = false;
    }
    if (result != MA_SUCCESS) {
        LSD_LOG_ERROR("audio", "miniaudio failed to start: %s", ma_result_description(result));
        return false;
    }
    impl_->ready = true;
    impl_->device = use_device;
    ma_engine_listener_set_world_up(&impl_->engine, 0, 0.0f, 0.0f, 1.0f);  // Z up
    impl_->voices = new Impl::Voice[kMaxVoices]();
    impl_->class_count = 1;
    std::snprintf(impl_->class_names[0], sizeof impl_->class_names[0], "Master");
    impl_->concurrency_count = 0;
    return true;
}

void AudioSystem::shutdown() {
    if (!impl_->ready) return;
    stop_all();
    for (u32 i = 1; i < impl_->class_count; ++i) ma_sound_group_uninit(&impl_->groups[i]);
    impl_->class_count = 0;
    delete[] impl_->voices;
    impl_->voices = nullptr;
    ma_engine_uninit(&impl_->engine);
    impl_->ready = false;
}

bool AudioSystem::has_device() const { return impl_->device; }

u32 AudioSystem::add_class(const char* name, u32 parent, f32 volume, f32 pitch) {
    if (!impl_->ready || impl_->class_count == kMaxClasses) return kMasterClass;
    const u32 index = impl_->class_count;
    ma_sound_group* parent_group = parent > 0 && parent < index ? &impl_->groups[parent] : nullptr;
    if (ma_sound_group_init(&impl_->engine, 0, parent_group, &impl_->groups[index]) != MA_SUCCESS) return kMasterClass;
    ma_sound_group_set_volume(&impl_->groups[index], volume);
    ma_sound_group_set_pitch(&impl_->groups[index], pitch);
    std::snprintf(impl_->class_names[index], sizeof impl_->class_names[index], "%s", name);
    impl_->class_count = index + 1;
    return index;
}

void AudioSystem::set_class_volume(u32 sound_class, f32 volume) {
    if (!impl_->ready) return;
    if (sound_class == kMasterClass) ma_engine_set_volume(&impl_->engine, volume);
    else if (sound_class < impl_->class_count) ma_sound_group_set_volume(&impl_->groups[sound_class], volume);
}

u32 AudioSystem::class_count() const { return impl_->class_count; }
const char* AudioSystem::class_name(u32 sound_class) const { return sound_class < impl_->class_count ? impl_->class_names[sound_class] : ""; }

void AudioSystem::reset_classes() {
    if (!impl_->ready) return;
    stop_all();
    for (u32 i = 1; i < impl_->class_count; ++i) ma_sound_group_uninit(&impl_->groups[i]);
    impl_->class_count = 1;
    impl_->concurrency_count = 0;
    ma_engine_set_volume(&impl_->engine, 1.0f);
}

u32 AudioSystem::add_concurrency(u32 max_voices, u32 when_full) {
    if (impl_->concurrency_count == sizeof impl_->concurrency / sizeof impl_->concurrency[0]) return kNoGroup;
    impl_->concurrency[impl_->concurrency_count] = {max_voices ? max_voices : 1, when_full};
    return impl_->concurrency_count++;
}

u32 AudioSystem::play(const PlayDesc& d) {
    if (!impl_->ready || !d.path) return 0;
    Impl& m = *impl_;
    // Concurrency: a full group stops its oldest or quietest voice, or refuses the new one.
    if (d.concurrency < m.concurrency_count) {
        const Impl::Concurrency& c = m.concurrency[d.concurrency];
        u32 in_group = 0;
        Impl::Voice* victim = nullptr;
        for (u32 i = 0; i < kMaxVoices; ++i) {
            Impl::Voice& v = m.voices[i];
            if (!v.used || v.group != d.concurrency) continue;
            ++in_group;
            if (!victim || (c.when_full == 0 ? v.started < victim->started : m.loudness(v) < m.loudness(*victim))) victim = &v;
        }
        if (in_group >= c.max_voices) {
            if (c.when_full == 2) return 0;
            if (victim) m.free_voice(*victim);
        }
    }
    Impl::Voice* slot = nullptr;
    for (u32 i = 0; i < kMaxVoices && !slot; ++i) {
        if (!m.voices[i].used) slot = &m.voices[i];
    }
    if (!slot) {  // every voice busy: the oldest makes room
        slot = &m.voices[0];
        for (u32 i = 1; i < kMaxVoices; ++i) {
            if (m.voices[i].started < slot->started) slot = &m.voices[i];
        }
        m.free_voice(*slot);
    }
    ma_uint32 flags = d.stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
    const bool spatial = d.spatial && d.attenuation.spatialize;
    if (!spatial) flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
    ma_sound_group* group = d.sound_class > 0 && d.sound_class < m.class_count ? &m.groups[d.sound_class] : nullptr;
    const ma_result result = ma_sound_init_from_file(&m.engine, d.path, flags, group, nullptr, &slot->sound);
    if (result != MA_SUCCESS) {
        LSD_LOG_WARN("audio", "Could not play %s: %s", d.path, ma_result_description(result));
        return 0;
    }
    slot->used = true;
    slot->id = m.next_id++;
    std::snprintf(slot->name, sizeof slot->name, "%s", d.name);
    slot->sound_class = group ? d.sound_class : kMasterClass;
    slot->group = d.concurrency;
    slot->volume = d.volume;
    slot->started = m.play_counter++;
    slot->spatial = spatial;
    slot->position = d.position;
    ma_sound_set_volume(&slot->sound, d.volume);
    ma_sound_set_pitch(&slot->sound, d.pitch);
    ma_sound_set_looping(&slot->sound, d.looping ? MA_TRUE : MA_FALSE);
    if (spatial) {
        const ma_attenuation_model models[3] = {ma_attenuation_model_inverse, ma_attenuation_model_linear, ma_attenuation_model_exponential};
        ma_sound_set_position(&slot->sound, d.position.x, d.position.y, d.position.z);
        ma_sound_set_attenuation_model(&slot->sound, models[d.attenuation.falloff < 3 ? d.attenuation.falloff : 0]);
        ma_sound_set_min_distance(&slot->sound, d.attenuation.min_distance);
        ma_sound_set_max_distance(&slot->sound, d.attenuation.max_distance);
        ma_sound_set_rolloff(&slot->sound, d.attenuation.rolloff);
    }
    ma_sound_start(&slot->sound);
    return slot->id;
}

void AudioSystem::stop(u32 voice) {
    if (!impl_->ready) return;
    for (u32 i = 0; i < kMaxVoices; ++i) {
        if (impl_->voices[i].used && impl_->voices[i].id == voice) impl_->free_voice(impl_->voices[i]);
    }
}

void AudioSystem::stop_all() {
    if (!impl_->ready) return;
    for (u32 i = 0; i < kMaxVoices; ++i) impl_->free_voice(impl_->voices[i]);
}

bool AudioSystem::playing(u32 voice) const {
    if (!impl_->ready) return false;
    for (u32 i = 0; i < kMaxVoices; ++i) {
        const Impl::Voice& v = impl_->voices[i];
        if (v.used && v.id == voice) return !ma_sound_at_end(&v.sound);
    }
    return false;
}

void AudioSystem::set_position(u32 voice, Vec3 p) {
    if (!impl_->ready) return;
    for (u32 i = 0; i < kMaxVoices; ++i) {
        Impl::Voice& v = impl_->voices[i];
        if (!v.used || v.id != voice) continue;
        v.position = p;
        ma_sound_set_position(&v.sound, p.x, p.y, p.z);
    }
}

void AudioSystem::set_listener(Vec3 p, Vec3 forward) {
    if (!impl_->ready) return;
    impl_->listener = p;
    ma_engine_listener_set_position(&impl_->engine, 0, p.x, p.y, p.z);
    ma_engine_listener_set_direction(&impl_->engine, 0, forward.x, forward.y, forward.z);
}

void AudioSystem::update() {
    if (!impl_->ready) return;
    for (u32 i = 0; i < kMaxVoices; ++i) {
        Impl::Voice& v = impl_->voices[i];
        if (v.used && ma_sound_at_end(&v.sound)) impl_->free_voice(v);
    }
}

u32 AudioSystem::voice_count() const {
    if (!impl_->ready) return 0;
    u32 n = 0;
    for (u32 i = 0; i < kMaxVoices; ++i) n += impl_->voices[i].used;
    return n;
}

VoiceInfo AudioSystem::voice(u32 index) const {
    for (u32 i = 0, n = 0; impl_->ready && i < kMaxVoices; ++i) {
        const Impl::Voice& v = impl_->voices[i];
        if (!v.used || n++ != index) continue;
        return {v.id, v.name, v.sound_class, v.volume, v.spatial ? length(v.position - impl_->listener) : 0.0f, v.spatial};
    }
    return {0, "", 0, 0, 0, false};
}

u32 AudioSystem::read_frames(f32* out, u32 frames) {
    if (!impl_->ready || impl_->device) return 0;
    ma_uint64 read = 0;
    ma_engine_read_pcm_frames(&impl_->engine, out, frames, &read);
    return static_cast<u32>(read);
}

} // namespace lsd::audio
