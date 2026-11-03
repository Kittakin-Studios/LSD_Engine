#include "lsd/audio/audio.h"

#include "lsd/foundation/log.h"

#include <miniaudio.h>
#include <extras/nodes/ma_reverb_node/ma_reverb_node.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace lsd::audio {

namespace {

constexpr f32 kOpen = 20000.0f;  // low-pass cutoff that leaves the sound as it is

// A feed-forward compressor (miniaudio has none): follows the louder channel's peak, and above the
// threshold turns the level down by the ratio.
struct CompressorNode {
    ma_node_base base;
    f32 threshold_db, ratio, makeup_db;
    f32 attack, release;  // per-sample smoothing coefficients
    f32 envelope;
};

void compressor_process(ma_node* node, const float** in, ma_uint32* in_frames, float** out, ma_uint32* out_frames) {
    auto& c = *static_cast<CompressorNode*>(node);
    const ma_uint32 frames = *out_frames < *in_frames ? *out_frames : *in_frames;
    for (ma_uint32 i = 0; i < frames; ++i) {
        const f32 l = in[0][i * 2], r = in[0][i * 2 + 1];
        const f32 peak = std::fmax(std::fabs(l), std::fabs(r));
        const f32 k = peak > c.envelope ? c.attack : c.release;
        c.envelope = c.envelope * k + peak * (1.0f - k);
        const f32 level_db = 20.0f * std::log10(c.envelope > 1e-6f ? c.envelope : 1e-6f);
        const f32 over = level_db - c.threshold_db;
        const f32 reduce_db = over > 0.0f ? over - over / c.ratio : 0.0f;
        const f32 gain = std::pow(10.0f, (c.makeup_db - reduce_db) / 20.0f);
        out[0][i * 2] = l * gain;
        out[0][i * 2 + 1] = r * gain;
    }
    *out_frames = frames;
}

ma_node_vtable g_compressor_vtable = {compressor_process, nullptr, 1, 1, 0};

// ma_reverb_node_init ignores its config's levels, and verblib starts with no dry signal: set them here.
// verblib scales dry by 2, so 0.5 passes the sound through unchanged.
void set_reverb(verblib& reverb, f32 amount, f32 room_size) {
    verblib_set_dry(&reverb, 0.5f);
    verblib_set_wet(&reverb, amount);
    verblib_set_room_size(&reverb, room_size);
}

} // namespace

struct AudioSystem::Impl {
    ma_engine engine;
    bool ready = false;
    bool device = false;
    ma_sound_group groups[kMaxClasses];  // [0] unused: Master is `master` below
    // Every sound ends in `master`, then the listener's environment reverb, then the speakers.
    ma_sound_group master;
    ma_reverb_node environment;
    struct Effect {
        u32 type;
        void* node;  // the miniaudio node, heap-allocated by its type
    };
    struct Chain {
        Effect effects[8];
        u32 count;
    };
    Chain chains[kMaxClasses];  // a class's Submix
    char class_names[kMaxClasses][32];
    f32 class_volume[kMaxClasses];  // as set, before mixes
    u32 class_parent[kMaxClasses];  // 0 (Master) for top-level classes
    f32 class_cutoff[kMaxClasses];  // low-pass from the mixes, inherited down the tree (kOpen = none)
    struct Mix {
        u32 id;
        MixAdjust adjust[16];
        u32 count;
        f32 fade_in, fade_out;
        f32 weight;  // 0..1
        bool popping;
    };
    Mix mixes[16];
    u32 mix_count = 0;
    u32 next_mix = 1;
    u32 class_count = 0;
    struct Concurrency {
        u32 max_voices, when_full;
    };
    Concurrency concurrency[64];
    u32 concurrency_count = 0;
    struct Voice {
        bool used;
        u32 id;
        u32 instance;  // the voice id it belongs to (itself when alone)
        ma_sound sound;
        char name[48];
        u32 sound_class;
        u32 group;
        f32 volume;
        u64 started;  // play order, for Stop Oldest
        bool spatial;
        Vec3 position;
        ma_lpf_node lowpass;  // sound -> lowpass -> class group
        f32 cutoff;       // applied: the lower of base_cutoff and its class's mix cutoff
        f32 base_cutoff;  // set_voice_lowpass (walls, occlusion)
        bool occlusion;
        f32 occlusion_muffle;
        bool fading_out;  // stop_with_fade: done once miniaudio has stopped it
    };
    Voice* voices = nullptr;
    u32 next_id = 1;
    u64 play_counter = 0;
    Vec3 listener;

    void free_voice(Voice& v) {
        if (!v.used) return;
        ma_sound_uninit(&v.sound);
        ma_lpf_node_uninit(&v.lowpass, nullptr);
        v.used = false;
    }
    // The voice's low-pass: the lower of what its caller set and what the mixes ask of its class.
    void apply_cutoff(Voice& v) {
        const f32 mix = v.sound_class < class_count ? class_cutoff[v.sound_class] : kOpen;
        const f32 want = v.base_cutoff < mix ? v.base_cutoff : mix;
        if (std::fabs(v.cutoff - want) < want * 0.02f) return;  // re-initialising the filter costs: only on real changes
        const ma_lpf_config config = ma_lpf_config_init(ma_format_f32, 2, ma_engine_get_sample_rate(&engine), want, 2);
        ma_lpf_node_reinit(&config, &v.lowpass);
        v.cutoff = want;
    }
    ma_node* class_node(u32 c) { return c > 0 && c < class_count ? static_cast<ma_node*>(&groups[c]) : static_cast<ma_node*>(&master); }

    void* create_effect(const SubmixEffect& e) {
        ma_node_graph* graph = ma_engine_get_node_graph(&engine);
        const ma_uint32 rate = ma_engine_get_sample_rate(&engine);
        switch (e.type) {
            case 0: {  // reverb
                auto* n = new ma_reverb_node();
                ma_reverb_node_config c = ma_reverb_node_config_init(2, rate);
                if (ma_reverb_node_init(graph, &c, nullptr, n) != MA_SUCCESS) {
                    delete n;
                    return nullptr;
                }
                set_reverb(n->reverb, e.amount, e.size);
                return n;
            }
            case 1: {
                auto* n = new ma_lpf_node();
                const ma_lpf_node_config c = ma_lpf_node_config_init(2, rate, e.frequency, 2);
                if (ma_lpf_node_init(graph, &c, nullptr, n) == MA_SUCCESS) return n;
                delete n;
                return nullptr;
            }
            case 2: {
                auto* n = new ma_hpf_node();
                const ma_hpf_node_config c = ma_hpf_node_config_init(2, rate, e.frequency, 2);
                if (ma_hpf_node_init(graph, &c, nullptr, n) == MA_SUCCESS) return n;
                delete n;
                return nullptr;
            }
            case 3: {
                auto* n = new ma_peak_node();
                const ma_peak_node_config c = ma_peak_node_config_init(2, rate, e.gain_db, 1.0, e.frequency);
                if (ma_peak_node_init(graph, &c, nullptr, n) == MA_SUCCESS) return n;
                delete n;
                return nullptr;
            }
            case 4: {
                auto* n = new ma_delay_node();
                const ma_delay_node_config c = ma_delay_node_config_init(2, rate, static_cast<ma_uint32>(e.time * static_cast<f32>(rate)), e.size);
                if (ma_delay_node_init(graph, &c, nullptr, n) != MA_SUCCESS) {
                    delete n;
                    return nullptr;
                }
                ma_delay_node_set_wet(n, e.amount);
                ma_delay_node_set_dry(n, 1.0f);
                return n;
            }
            case 5: {
                auto* n = new CompressorNode();
                ma_node_config c = ma_node_config_init();
                c.vtable = &g_compressor_vtable;
                const ma_uint32 channels[1] = {2};
                c.pInputChannels = channels;
                c.pOutputChannels = channels;
                if (ma_node_init(graph, &c, nullptr, &n->base) != MA_SUCCESS) {
                    delete n;
                    return nullptr;
                }
                n->threshold_db = e.threshold_db;
                n->ratio = e.ratio < 1.0f ? 1.0f : e.ratio;
                n->makeup_db = e.gain_db;
                n->attack = std::exp(-1.0f / (0.005f * static_cast<f32>(rate)));  // 5 ms
                n->release = std::exp(-1.0f / ((e.time > 0.01f ? e.time : 0.01f) * static_cast<f32>(rate)));
                n->envelope = 0.0f;
                return n;
            }
        }
        return nullptr;
    }
    static void destroy_effect(Effect& e) {
        switch (e.type) {
            case 0: ma_reverb_node_uninit(static_cast<ma_reverb_node*>(e.node), nullptr); delete static_cast<ma_reverb_node*>(e.node); break;
            case 1: ma_lpf_node_uninit(static_cast<ma_lpf_node*>(e.node), nullptr); delete static_cast<ma_lpf_node*>(e.node); break;
            case 2: ma_hpf_node_uninit(static_cast<ma_hpf_node*>(e.node), nullptr); delete static_cast<ma_hpf_node*>(e.node); break;
            case 3: ma_peak_node_uninit(static_cast<ma_peak_node*>(e.node), nullptr); delete static_cast<ma_peak_node*>(e.node); break;
            case 4: ma_delay_node_uninit(static_cast<ma_delay_node*>(e.node), nullptr); delete static_cast<ma_delay_node*>(e.node); break;
            case 5: ma_node_uninit(e.node, nullptr); delete static_cast<CompressorNode*>(e.node); break;
            default: break;
        }
        e.node = nullptr;
    }
    // Classes first (they feed the chains), then the chains.
    void release_classes() {
        for (u32 i = 1; i < class_count; ++i) ma_sound_group_uninit(&groups[i]);
        for (u32 i = 1; i < class_count; ++i) {
            for (u32 k = 0; k < chains[i].count; ++k) destroy_effect(chains[i].effects[k]);
            chains[i].count = 0;
        }
        class_count = 1;
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
    // master -> environment reverb (silent until an Audio Volume asks) -> speakers
    ma_sound_group_init(&impl_->engine, 0, nullptr, &impl_->master);
    const ma_reverb_node_config reverb = ma_reverb_node_config_init(2, ma_engine_get_sample_rate(&impl_->engine));
    ma_reverb_node_init(ma_engine_get_node_graph(&impl_->engine), &reverb, nullptr, &impl_->environment);
    set_reverb(impl_->environment.reverb, 0.0f, 0.5f);
    ma_node_attach_output_bus(&impl_->master, 0, &impl_->environment, 0);
    ma_node_attach_output_bus(&impl_->environment, 0, ma_engine_get_endpoint(&impl_->engine), 0);
    impl_->voices = new Impl::Voice[kMaxVoices]();
    impl_->class_count = 1;
    std::snprintf(impl_->class_names[0], sizeof impl_->class_names[0], "Master");
    impl_->class_volume[0] = 1.0f;
    impl_->class_cutoff[0] = kOpen;
    impl_->mix_count = 0;
    impl_->concurrency_count = 0;
    return true;
}

void AudioSystem::shutdown() {
    if (!impl_->ready) return;
    if (impl_->device) ma_engine_stop(&impl_->engine);  // the device thread stops mixing before its sounds and groups go
    stop_all();
    impl_->release_classes();
    impl_->class_count = 0;
    ma_sound_group_uninit(&impl_->master);
    ma_reverb_node_uninit(&impl_->environment, nullptr);
    delete[] impl_->voices;
    impl_->voices = nullptr;
    ma_engine_uninit(&impl_->engine);
    impl_->ready = false;
}

bool AudioSystem::has_device() const { return impl_->device; }

u32 AudioSystem::add_class(const char* name, u32 parent, f32 volume, f32 pitch, const SubmixEffect* effects, u32 effect_count, f32 submix_volume) {
    if (!impl_->ready || impl_->class_count == kMaxClasses) return kMasterClass;
    Impl& m = *impl_;
    const u32 index = m.class_count;
    ma_sound_group* parent_group = parent > 0 && parent < index ? &m.groups[parent] : &m.master;
    if (ma_sound_group_init(&m.engine, 0, parent_group, &m.groups[index]) != MA_SUCCESS) return kMasterClass;
    // Submix: class -> effect -> effect -> ... -> parent.
    Impl::Chain& chain = m.chains[index];
    chain.count = 0;
    ma_node* previous = &m.groups[index];
    for (u32 i = 0; i < effect_count && chain.count < 8; ++i) {
        void* node = m.create_effect(effects[i]);
        if (!node) continue;
        chain.effects[chain.count++] = {effects[i].type, node};
        ma_node_attach_output_bus(previous, 0, static_cast<ma_node*>(node), 0);
        previous = static_cast<ma_node*>(node);
    }
    if (chain.count) {
        ma_node_attach_output_bus(previous, 0, parent_group, 0);
        ma_node_set_output_bus_volume(previous, 0, submix_volume);
    }
    ma_sound_group_set_volume(&impl_->groups[index], volume);
    ma_sound_group_set_pitch(&impl_->groups[index], pitch);
    impl_->class_volume[index] = volume;
    impl_->class_parent[index] = parent < index ? parent : 0;
    impl_->class_cutoff[index] = kOpen;
    std::snprintf(impl_->class_names[index], sizeof impl_->class_names[index], "%s", name);
    impl_->class_count = index + 1;
    return index;
}

void AudioSystem::set_class_volume(u32 sound_class, f32 volume) {
    if (!impl_->ready || sound_class >= impl_->class_count) return;
    impl_->class_volume[sound_class] = volume;
    update(0.0f);  // mixes on top
}

namespace {
void apply_class_volume(ma_engine& engine, ma_sound_group* groups, u32 sound_class, f32 volume) {
    if (sound_class == kMasterClass) ma_engine_set_volume(&engine, volume);
    else ma_sound_group_set_volume(&groups[sound_class], volume);
}
} // namespace

u32 AudioSystem::class_count() const { return impl_->class_count; }
const char* AudioSystem::class_name(u32 sound_class) const { return sound_class < impl_->class_count ? impl_->class_names[sound_class] : ""; }

void AudioSystem::reset_classes() {
    if (!impl_->ready) return;
    stop_all();
    impl_->release_classes();
    impl_->concurrency_count = 0;
    set_environment(0.0f, 0.5f);
    impl_->mix_count = 0;
    impl_->class_volume[0] = 1.0f;
    impl_->class_cutoff[0] = kOpen;
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
    ma_sound_group* group = d.sound_class > 0 && d.sound_class < m.class_count ? &m.groups[d.sound_class] : &m.master;
    const ma_result result = ma_sound_init_from_file(&m.engine, d.path, flags, group, nullptr, &slot->sound);
    if (result != MA_SUCCESS) {
        LSD_LOG_WARN("audio", "Could not play %s: %s", d.path, ma_result_description(result));
        return 0;
    }
    // Its own low-pass (open until an Audio Volume muffles it): sound -> lowpass -> class.
    const ma_lpf_node_config lowpass = ma_lpf_node_config_init(2, ma_engine_get_sample_rate(&m.engine), kOpen, 2);
    if (ma_lpf_node_init(ma_engine_get_node_graph(&m.engine), &lowpass, nullptr, &slot->lowpass) != MA_SUCCESS) {
        ma_sound_uninit(&slot->sound);
        return 0;
    }
    ma_node_attach_output_bus(&slot->sound, 0, &slot->lowpass, 0);
    ma_node_attach_output_bus(&slot->lowpass, 0, group, 0);
    slot->cutoff = kOpen;
    slot->base_cutoff = kOpen;
    slot->used = true;
    slot->fading_out = false;
    slot->id = m.next_id++;
    slot->instance = d.instance ? d.instance : slot->id;
    std::snprintf(slot->name, sizeof slot->name, "%s", d.name);
    slot->sound_class = group != &m.master ? d.sound_class : kMasterClass;
    slot->group = d.concurrency;
    slot->volume = d.volume;
    slot->started = m.play_counter++;
    slot->spatial = spatial;
    slot->position = d.position;
    slot->occlusion = spatial && d.attenuation.occlusion;
    slot->occlusion_muffle = d.attenuation.occlusion_muffle;
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
    if (d.delay > 0.0f) {
        const ma_uint64 now = ma_engine_get_time_in_pcm_frames(&m.engine);
        ma_sound_set_start_time_in_pcm_frames(&slot->sound, now + static_cast<ma_uint64>(d.delay * static_cast<f32>(ma_engine_get_sample_rate(&m.engine))));
    }
    ma_sound_start(&slot->sound);
    return slot->id;
}

void AudioSystem::stop(u32 voice) {
    if (!impl_->ready) return;
    for (u32 i = 0; i < kMaxVoices; ++i) {
        if (impl_->voices[i].used && (impl_->voices[i].id == voice || impl_->voices[i].instance == voice)) impl_->free_voice(impl_->voices[i]);
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
        if (v.used && (v.id == voice || v.instance == voice) && !ma_sound_at_end(&v.sound) && !(v.fading_out && !ma_sound_is_playing(&v.sound))) return true;
    }
    return false;
}

void AudioSystem::set_position(u32 voice, Vec3 p) {
    if (!impl_->ready) return;
    for (u32 i = 0; i < kMaxVoices; ++i) {
        Impl::Voice& v = impl_->voices[i];
        if (!v.used || (v.id != voice && v.instance != voice)) continue;
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

f32 AudioSystem::length_seconds(const char* path) {
    if (!impl_->ready || !path) return 0.0f;
    ma_sound sound;
    if (ma_sound_init_from_file(&impl_->engine, path, MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, nullptr, &sound) != MA_SUCCESS) return 0.0f;
    float seconds = 0.0f;
    ma_sound_get_length_in_seconds(&sound, &seconds);
    ma_sound_uninit(&sound);
    return seconds;
}

u32 AudioSystem::push_mix(const MixAdjust* adjusters, u32 count, f32 fade_in, f32 fade_out) {
    Impl& m = *impl_;
    if (!m.ready || m.mix_count == sizeof m.mixes / sizeof m.mixes[0]) return 0;
    Impl::Mix& mix = m.mixes[m.mix_count++];
    mix.id = m.next_mix++;
    mix.count = count < 16 ? count : 16;
    for (u32 i = 0; i < mix.count; ++i) mix.adjust[i] = adjusters[i];
    mix.fade_in = fade_in;
    mix.fade_out = fade_out;
    mix.weight = fade_in > 0.0f ? 0.0f : 1.0f;
    mix.popping = false;
    update(0.0f);
    return mix.id;
}

void AudioSystem::pop_mix(u32 id) {
    for (u32 i = 0; i < impl_->mix_count; ++i) {
        if (impl_->mixes[i].id == id) impl_->mixes[i].popping = true;
    }
    update(0.0f);
}

u32 AudioSystem::mix_count() const { return impl_->mix_count; }

void AudioSystem::set_environment(f32 reverb_amount, f32 room_size) {
    if (!impl_->ready) return;
    set_reverb(impl_->environment.reverb, reverb_amount, room_size);
}

void AudioSystem::set_voice_lowpass(u32 voice, f32 cutoff) {
    if (!impl_->ready) return;
    cutoff = cutoff < 20.0f ? 20.0f : cutoff > kOpen ? kOpen : cutoff;
    for (u32 i = 0; i < kMaxVoices; ++i) {
        Impl::Voice& v = impl_->voices[i];
        if (!v.used || (v.id != voice && v.instance != voice)) continue;
        v.base_cutoff = cutoff;
        impl_->apply_cutoff(v);
    }
}

void AudioSystem::fade_voice(u32 voice, f32 volume, f32 seconds) {
    if (!impl_->ready) return;
    for (u32 i = 0; i < kMaxVoices; ++i) {
        Impl::Voice& v = impl_->voices[i];
        if (v.used && (v.id == voice || v.instance == voice)) ma_sound_set_fade_in_milliseconds(&v.sound, -1.0f, volume, static_cast<ma_uint64>(seconds * 1000.0f));
    }
}

void AudioSystem::stop_with_fade(u32 voice, f32 seconds) {
    if (!impl_->ready) return;
    if (seconds <= 0.0f) {
        stop(voice);
        return;
    }
    for (u32 i = 0; i < kMaxVoices; ++i) {
        Impl::Voice& v = impl_->voices[i];
        if (!v.used || (v.id != voice && v.instance != voice)) continue;
        ma_sound_stop_with_fade_in_milliseconds(&v.sound, static_cast<ma_uint64>(seconds * 1000.0f));
        v.fading_out = true;
    }
}

f32 AudioSystem::voice_time(u32 voice) {
    if (!impl_->ready) return 0.0f;
    for (u32 i = 0; i < kMaxVoices; ++i) {
        Impl::Voice& v = impl_->voices[i];
        float seconds = 0.0f;
        if (v.used && v.id == voice && ma_sound_get_cursor_in_seconds(&v.sound, &seconds) == MA_SUCCESS) return seconds;
    }
    return 0.0f;
}

void AudioSystem::seek_voice(u32 voice, f32 seconds) {
    if (!impl_->ready) return;
    const ma_uint32 rate = ma_engine_get_sample_rate(&impl_->engine);
    for (u32 i = 0; i < kMaxVoices; ++i) {
        Impl::Voice& v = impl_->voices[i];
        if (v.used && v.id == voice) ma_sound_seek_to_pcm_frame(&v.sound, static_cast<ma_uint64>(static_cast<double>(seconds) * rate));
    }
}

void AudioSystem::update(f32 dt) {
    if (!impl_->ready) return;
    Impl& m = *impl_;
    // Mix fades, then every class volume = its own volume x each mix's adjuster, by the mix's weight.
    for (u32 i = 0; i < m.mix_count;) {
        Impl::Mix& mix = m.mixes[i];
        if (mix.popping) mix.weight = mix.fade_out > 0.0f ? mix.weight - dt / mix.fade_out : 0.0f;
        else mix.weight = mix.fade_in > 0.0f ? mix.weight + dt / mix.fade_in : 1.0f;
        mix.weight = mix.weight < 0.0f ? 0.0f : mix.weight > 1.0f ? 1.0f : mix.weight;
        if (mix.popping && mix.weight <= 0.0f) m.mixes[i] = m.mixes[--m.mix_count];
        else ++i;
    }
    for (u32 c = 0; c < m.class_count; ++c) {
        f32 volume = m.class_volume[c];
        // Low-pass: the lowest any mix asks for (faded in log space, as ears hear pitch), and never
        // brighter than the parent's (classes come after their parents).
        f32 cutoff = c > 0 ? m.class_cutoff[m.class_parent[c]] : kOpen;
        for (u32 i = 0; i < m.mix_count; ++i) {
            for (u32 k = 0; k < m.mixes[i].count; ++k) {
                const MixAdjust& a = m.mixes[i].adjust[k];
                if (a.sound_class != c) continue;
                volume *= 1.0f + (a.volume - 1.0f) * m.mixes[i].weight;
                if (a.lowpass < kOpen) cutoff = std::fmin(cutoff, kOpen * std::pow(std::fmax(a.lowpass, 20.0f) / kOpen, m.mixes[i].weight));
            }
        }
        m.class_cutoff[c] = cutoff;
        apply_class_volume(m.engine, m.groups, c, volume);
    }
    for (u32 i = 0; i < kMaxVoices; ++i) {
        if (impl_->voices[i].used) impl_->apply_cutoff(impl_->voices[i]);
    }
    for (u32 i = 0; i < kMaxVoices; ++i) {
        Impl::Voice& v = impl_->voices[i];
        if (v.used && (ma_sound_at_end(&v.sound) || (v.fading_out && !ma_sound_is_playing(&v.sound)))) impl_->free_voice(v);
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
        return {v.id, v.name, v.sound_class, v.volume, v.spatial ? length(v.position - impl_->listener) : 0.0f, v.spatial, v.position, v.cutoff,
                v.occlusion, v.occlusion_muffle};
    }
    return {0, "", 0, 0, 0, false, {}, kOpen, false, kOpen};
}

u32 AudioSystem::read_frames(f32* out, u32 frames) {
    if (!impl_->ready || impl_->device) return 0;
    ma_uint64 read = 0;
    ma_engine_read_pcm_frames(&impl_->engine, out, frames, &read);
    return static_cast<u32>(read);
}

bool waveform(const char* path, u32 columns, f32* peaks) {
    for (u32 c = 0; c < columns; ++c) peaks[c] = 0.0f;
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder;
    if (columns == 0 || ma_decoder_init_file(path, &config, &decoder) != MA_SUCCESS) return false;
    ma_uint64 frames = 0;
    // Streams without a known length (some MP3s) are read twice: once to count, once to measure.
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) != MA_SUCCESS || frames == 0) {
        float scratch[4096];
        for (ma_uint64 read = 1; read > 0;) {
            read = 0;
            ma_decoder_read_pcm_frames(&decoder, scratch, 4096 / decoder.outputChannels, &read);
            frames += read;
        }
        ma_decoder_seek_to_pcm_frame(&decoder, 0);
    }
    const u32 channels = decoder.outputChannels;
    if (frames == 0 || channels == 0) {
        ma_decoder_uninit(&decoder);
        return false;
    }
    float buffer[4096];
    ma_uint64 at = 0;
    for (;;) {
        ma_uint64 read = 0;
        const ma_result result = ma_decoder_read_pcm_frames(&decoder, buffer, 4096 / channels, &read);  // the last block may come with MA_AT_END
        for (ma_uint64 f = 0; f < read; ++f, ++at) {
            const u32 column = static_cast<u32>(at * columns / frames);
            if (column >= columns) break;
            for (u32 ch = 0; ch < channels; ++ch) peaks[column] = std::fmax(peaks[column], std::fabs(buffer[f * channels + ch]));
        }
        if (result != MA_SUCCESS || read == 0) break;
    }
    ma_decoder_uninit(&decoder);
    for (u32 c = 0; c < columns; ++c) peaks[c] = peaks[c] > 1.0f ? 1.0f : peaks[c];
    return true;
}

} // namespace lsd::audio
