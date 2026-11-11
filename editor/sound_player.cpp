#include "sound_player.h"

#include "lsd/asset/data_asset.h"
#include "lsd/foundation/log.h"
#include "lsd/script/graph.h"

#include <cstdio>
#include <cstring>

namespace lsd::editor {

namespace {

template <typename T>
bool load_by_guid(const asset::AssetRegistry& registry, const char* guid_text, T& out) {
    asset::Guid guid;
    const asset::AssetRecord* r = guid_text[0] && asset::parse_guid(guid_text, guid) ? registry.find(guid) : nullptr;
    if (!r) return false;
    char path[300];
    registry.source_path(*r, path, sizeof path);
    return asset::load_data_asset(path, out);
}

} // namespace

const asset::AssetRecord* SoundPlayer::find(const char* sound, bool cues) const {
    asset::Guid guid;
    if (std::strlen(sound) == 32 && asset::parse_guid(sound, guid)) {
        const asset::AssetRecord* r = registry_->find(guid);
        return r && (r->type == asset::AssetType::Sound || (cues && r->type == asset::AssetType::Cue)) ? r : nullptr;
    }
    for (const asset::AssetType type : {asset::AssetType::Cue, asset::AssetType::Sound}) {  // a cue wins over a wave of the same name
        if (type == asset::AssetType::Cue && !cues) continue;
        usize hits[16];
        const usize n = registry_->search(sound, type, nullptr, hits, 16);
        for (usize i = 0; i < n; ++i) {
            if (std::strcmp(registry_->get(hits[i]).name, sound) == 0) return &registry_->get(hits[i]);
        }
    }
    return nullptr;
}

u32 SoundPlayer::class_index(const char* guid, int depth) {
    if (!guid[0] || depth > 16) return audio::kMasterClass;  // depth: a class that is its own ancestor
    for (u32 i = 0; i < class_count_; ++i) {
        if (std::strcmp(classes_[i].guid, guid) == 0) return classes_[i].index;
    }
    audio::SoundClass c;
    asset::Guid id;
    const asset::AssetRecord* r = asset::parse_guid(guid, id) ? registry_->find(id) : nullptr;
    if (!r || !load_by_guid(*registry_, guid, c)) return audio::kMasterClass;
    const u32 parent = class_index(c.parent, depth + 1);  // parents first
    audio::Submix submix;
    const bool has_submix = load_by_guid(*registry_, c.submix, submix);
    const u32 index = has_submix ? audio_->add_class(r->name, parent, c.volume, c.pitch, submix.effects.items, submix.effects.count, submix.volume)
                                 : audio_->add_class(r->name, parent, c.volume, c.pitch);
    if (class_count_ < sizeof classes_ / sizeof classes_[0]) {
        std::snprintf(classes_[class_count_].guid, sizeof classes_[0].guid, "%s", guid);
        classes_[class_count_++].index = index;
    }
    return index;
}

u32 SoundPlayer::concurrency_index(const char* guid) {
    if (!guid[0]) return audio::kNoGroup;
    for (u32 i = 0; i < group_count_; ++i) {
        if (std::strcmp(groups_[i].guid, guid) == 0) return groups_[i].index;
    }
    audio::SoundConcurrency c;
    if (!load_by_guid(*registry_, guid, c)) return audio::kNoGroup;
    const u32 index = audio_->add_concurrency(c.max_voices, c.when_full);
    if (group_count_ < sizeof groups_ / sizeof groups_[0]) {
        std::snprintf(groups_[group_count_].guid, sizeof groups_[0].guid, "%s", guid);
        groups_[group_count_++].index = index;
    }
    return index;
}

const asset::AssetRecord* SoundPlayer::find_data(const char* name, const char* data_type) const {
    usize hits[16];
    const usize n = registry_ ? registry_->search(name, asset::AssetType::Data, nullptr, hits, 16) : 0;
    for (usize i = 0; i < n; ++i) {
        const asset::AssetRecord& r = registry_->get(hits[i]);
        if (std::strcmp(r.name, name) == 0 && std::strcmp(r.data_type, data_type) == 0) return &r;
    }
    return nullptr;
}

// A Music segment's track and its stems, `delay` seconds from now (sample-accurate), looping.
void SoundPlayer::start_segment(u32 index, f32 delay, f32 fade_in) {
    MusicPlay& m = music_play_;
    const audio::MusicSegment& segment = m.asset.segments[index];
    auto start = [&](const char* sound, f32 volume) -> u32 {
        const asset::AssetRecord* r = find(sound, false);
        if (!r) {
            LSD_LOG_WARN("audio", "Music: no sound '%s'", sound);
            return 0;
        }
        const u32 voice = play_file(*r, nullptr, false, {}, 1.0f, 1.0f, true, delay, 0);
        audio_->fade_voice(voice, fade_in > 0.0f ? 0.0f : volume, 0.0f);
        if (fade_in > 0.0f) audio_->fade_voice(voice, volume, fade_in);
        return voice;
    };
    m.segment = index;
    m.start = audio_->time_seconds() + delay;
    m.base = segment.sound[0] ? start(segment.sound, 1.0f) : 0;
    for (u32 i = 0; i < m.asset.stems.count; ++i) {
        const audio::MusicStem& stem = m.asset.stems[i];
        const bool here = !stem.segment[0] || std::strcmp(stem.segment, segment.name) == 0;
        m.stems[i] = here && stem.sound[0] ? start(stem.sound, audio::music_stem_gain(m.intensity, stem.from, stem.full)) : 0;
    }
}

void SoundPlayer::set_music_intensity(f32 intensity) {
    MusicPlay& m = music_play_;
    m.intensity = intensity;
    if (!audio_ || !m.active) return;
    for (u32 i = 0; i < m.asset.stems.count; ++i) {
        if (m.stems[i]) audio_->fade_voice(m.stems[i], audio::music_stem_gain(intensity, m.asset.stems[i].from, m.asset.stems[i].full), m.asset.intensity_fade);
    }
}

void SoundPlayer::set_music_segment(const char* name) {
    MusicPlay& m = music_play_;
    if (!audio_ || !m.active || !name) return;
    for (u32 i = 0; i < m.asset.segments.count; ++i) {
        if (std::strcmp(m.asset.segments[i].name, name) != 0) continue;
        if (i == m.segment) return;
        const f32 wait = audio::music_next_boundary(audio_->time_seconds() - m.start, m.asset.tempo, m.asset.beats_per_bar, m.asset.quantize == 1);
        if (m.base) audio_->stop_at(m.base, wait, m.asset.switch_fade);
        for (u32 k = 0; k < m.asset.stems.count; ++k) {
            if (m.stems[k]) audio_->stop_at(m.stems[k], wait, m.asset.switch_fade);
        }
        start_segment(i, wait, m.asset.switch_fade);
        return;
    }
    LSD_LOG_WARN("audio", "Music: no segment '%s'", name);
}

void SoundPlayer::play_music(const char* name, f32 fade) {
    if (!audio_ || !name || std::strcmp(name, music_) == 0) return;
    stop_music(fade);
    if (const asset::AssetRecord* r = find_data(name, "Music")) {  // a Music asset: segments and stems
        char path[300];
        registry_->source_path(*r, path, sizeof path);
        MusicPlay& m = music_play_;
        if (!asset::load_data_asset(path, m.asset) || m.asset.segments.count == 0) {
            LSD_LOG_WARN("audio", "Music '%s' has no segments", name);
            return;
        }
        m.active = true;
        start_segment(0, 0.0f, fade);
        std::snprintf(music_, sizeof music_, "%s", name);
        return;
    }
    music_voice_ = play(name, false, {});
    if (!music_voice_) return;
    std::snprintf(music_, sizeof music_, "%s", name);
    if (fade > 0.0f) {  // fade in: start silent
        audio_->fade_voice(music_voice_, 0.0f, 0.0f);
        audio_->fade_voice(music_voice_, 1.0f, fade);
    }
}

void SoundPlayer::stop_music(f32 fade) {
    if (!audio_) return;
    if (music_voice_) audio_->stop_with_fade(music_voice_, fade);
    for (u32 i = 0; i < layer_count_; ++i) audio_->stop_with_fade(layers_[i].voice, fade);
    music_voice_ = 0;
    music_[0] = '\0';
    layer_count_ = 0;
    MusicPlay& m = music_play_;
    if (m.active && m.base) audio_->stop_with_fade(m.base, fade);
    for (u32 i = 0; m.active && i < m.asset.stems.count; ++i) {
        if (m.stems[i]) audio_->stop_with_fade(m.stems[i], fade);
    }
    m.active = false;
}

void SoundPlayer::set_music_layer(const char* name, f32 volume, f32 fade) {
    if (!audio_ || !name) return;
    Layer* layer = nullptr;
    for (u32 i = 0; i < layer_count_ && !layer; ++i) {
        if (std::strcmp(layers_[i].name, name) == 0) layer = &layers_[i];
    }
    if (!layer) {
        if (layer_count_ == sizeof layers_ / sizeof layers_[0]) return;
        const u32 voice = play(name, false, {});
        if (!voice) return;
        if (music_voice_) audio_->seek_voice(voice, audio_->voice_time(music_voice_));  // in step with the track
        audio_->fade_voice(voice, 0.0f, 0.0f);
        layer = &layers_[layer_count_++];
        std::snprintf(layer->name, sizeof layer->name, "%s", name);
        layer->voice = voice;
    }
    audio_->fade_voice(layer->voice, volume, fade);
}

u32 SoundPlayer::play(const char* sound, bool spatial, Vec3 position, bool loop) {
    if (!audio_ || !registry_ || !sound || !sound[0]) return 0;
    const asset::AssetRecord* r = find(sound);
    if (!r) {
        LSD_LOG_WARN("audio", "No sound asset '%s'", sound);
        return 0;
    }
    if (r->type == asset::AssetType::Cue) {
        char path[300];
        registry_->source_path(*r, path, sizeof path);
        const usize length = std::strlen(path), ext = sizeof kSynthExtension - 1;
        if (length > ext && std::strcmp(path + length - ext, kSynthExtension) == 0) return play_synth(*r, path, spatial, position);
        return play_cue(*r, spatial, position);
    }
    asset::AssetMeta meta;
    registry_->load_meta(*r, meta);
    return play_file(*r, nullptr, spatial, position, 1.0f, 1.0f, meta.looping || loop, 0.0f, 0);
}

// One sound file. Class, attenuation and concurrency come from the cue that plays it (or the wave's
// own .meta when played directly); volume and pitch multiply: cue x node x wave.
u32 SoundPlayer::play_file(const asset::AssetRecord& wave, const asset::AssetMeta* cue_settings, bool spatial, Vec3 position, f32 volume, f32 pitch,
                           bool looping, f32 delay, u32 instance) {
    asset::AssetMeta own;
    registry_->load_meta(wave, own);
    const asset::AssetMeta& settings = cue_settings ? *cue_settings : own;
    char path[300];
    registry_->source_path(wave, path, sizeof path);
    audio::PlayDesc d;
    d.path = path;
    d.name = wave.name;
    d.spatial = spatial;
    d.position = position;
    load_by_guid(*registry_, settings.attenuation, d.attenuation);  // defaults when none is picked
    d.volume = volume * own.volume * (cue_settings ? cue_settings->volume : 1.0f);
    d.pitch = pitch * own.pitch * (cue_settings ? cue_settings->pitch : 1.0f);
    d.looping = looping;
    d.stream = own.stream;
    d.sound_class = class_index(settings.sound_class);
    d.concurrency = concurrency_index(settings.concurrency);
    d.delay = delay;
    d.instance = instance;
    const u32 voice = audio_->play(d);
    const char* line = settings.subtitle[0] ? settings.subtitle : own.subtitle;
    if (voice && line[0]) {
        std::snprintf(subtitle_, sizeof subtitle_, "%s", line);
        subtitle_voice_ = instance ? instance : voice;  // a cue's voices share its instance
    }
    return voice;
}

const char* SoundPlayer::subtitle() {
    if (subtitle_voice_ && (!audio_ || !audio_->playing(subtitle_voice_))) {
        subtitle_voice_ = 0;
        subtitle_[0] = '\0';
    }
    return subtitle_;
}

// A Sound Synth: compiled from its graph at every play (small, and edits are heard at once), its
// parameters starting from the values scripts set last.
u32 SoundPlayer::play_synth(const asset::AssetRecord& record, const char* path, bool spatial, Vec3 position) {
    script::Graph graph(default_allocator());
    auto* program = new audio::SynthProgram();  // a few KB: kept off the stack
    char problem[160];
    const bool loaded = script::load_graph_file(graph, path);
    if (!loaded || !compile_synth(graph, *program, problem, sizeof problem)) {
        LSD_LOG_WARN("audio", "%s: %s", record.name, loaded ? problem : "could not read it");
        delete program;
        return 0;
    }
    for (u32 i = 0; i < program->param_count; ++i) {
        for (u32 k = 0; k < param_count_; ++k) {
            if (std::strcmp(params_[k].name, program->params[i]) == 0) program->param_defaults[i] = params_[k].value;
        }
    }
    asset::AssetMeta settings;
    registry_->load_meta(record, settings);
    audio::PlayDesc d;
    d.name = record.name;
    d.synth = program;
    char waves[audio::kSynthMaxWaves][300] = {};
    for (u32 i = 0; i < program->wave_count; ++i) {
        const asset::AssetRecord* wave = find(program->waves[i], false);
        if (!wave) {
            LSD_LOG_WARN("audio", "%s: Wave Player names no sound asset '%s'", record.name, program->waves[i]);
            continue;
        }
        registry_->source_path(*wave, waves[i], sizeof waves[i]);
        d.synth_waves[i] = waves[i];
    }
    d.spatial = spatial;
    d.position = position;
    load_by_guid(*registry_, settings.attenuation, d.attenuation);
    d.volume = settings.volume;
    d.pitch = settings.pitch;
    d.sound_class = class_index(settings.sound_class);
    d.concurrency = concurrency_index(settings.concurrency);
    const u32 voice = audio_->play(d);
    delete program;
    return voice;
}

u32 SoundPlayer::play_cue(const asset::AssetRecord& cue, bool spatial, Vec3 position) {
    char path[300];
    registry_->source_path(cue, path, sizeof path);
    script::Graph graph(default_allocator());
    if (!script::load_graph_file(graph, path)) return 0;
    cue_env_.user = this;
    cue_env_.length = [](void* user, const char* sound) {
        auto& self = *static_cast<SoundPlayer*>(user);
        const asset::AssetRecord* r = self.find(sound, false);
        char file[300];
        if (!r) return 0.0f;
        self.registry_->source_path(*r, file, sizeof file);
        return self.audio_->length_seconds(file);
    };
    cue_env_.parameter = [](void* user, const char* name) { return static_cast<const SoundPlayer*>(user)->param(name); };
    CueVoice voices[16];
    const u32 n = evaluate_cue(graph, cue_env_, voices, 16);
    asset::AssetMeta settings;  // the cue's class, attenuation, concurrency, volume
    registry_->load_meta(cue, settings);
    u32 instance = 0;
    for (u32 i = 0; i < n; ++i) {
        const asset::AssetRecord* wave = find(voices[i].sound, false);
        if (!wave) {
            LSD_LOG_WARN("audio", "%s: Wave Player names no sound asset '%s'", cue.name, voices[i].sound);
            continue;
        }
        const u32 voice = play_file(*wave, &settings, spatial, position, voices[i].volume, voices[i].pitch, voices[i].looping, voices[i].delay, instance);
        if (!instance) instance = voice;
    }
    return instance;
}

void SoundPlayer::push_mix(const char* name) {
    if (!audio_ || mix_count_ == sizeof mixes_ / sizeof mixes_[0]) return;
    const asset::AssetRecord* r = find_data(name, "SoundMix");
    char path[300];
    audio::SoundMix mix;
    if (r) registry_->source_path(*r, path, sizeof path);
    if (!r || !asset::load_data_asset(path, mix)) {
        LSD_LOG_WARN("audio", "No Sound Mix '%s'", name);
        return;
    }
    audio::MixAdjust adjust[16];
    for (u32 i = 0; i < mix.adjusters.count; ++i) adjust[i] = {class_index(mix.adjusters[i].sound_class), mix.adjusters[i].volume, mix.adjusters[i].lowpass};
    Pushed& p = mixes_[mix_count_++];
    std::snprintf(p.name, sizeof p.name, "%s", name);
    p.id = audio_->push_mix(adjust, mix.adjusters.count, mix.fade_in, mix.fade_out);
}

void SoundPlayer::pop_mix(const char* name) {
    for (u32 i = mix_count_; i-- > 0;) {
        if (std::strcmp(mixes_[i].name, name) != 0) continue;
        if (audio_) audio_->pop_mix(mixes_[i].id);
        mixes_[i] = mixes_[--mix_count_];
        return;
    }
}

void SoundPlayer::set_class_volume(const char* name, f32 volume, bool warn) {
    if (!audio_) return;
    usize hits[16];
    const usize n = registry_->search(name, asset::AssetType::Data, nullptr, hits, 16);
    for (usize i = 0; i < n; ++i) {
        const asset::AssetRecord& r = registry_->get(hits[i]);
        if (std::strcmp(r.name, name) != 0 || std::strcmp(r.data_type, "SoundClass") != 0) continue;
        char guid[33];
        asset::to_string(r.guid, guid);
        audio_->set_class_volume(class_index(guid), volume);
        return;
    }
    if (std::strcmp(name, "Master") == 0) audio_->set_class_volume(audio::kMasterClass, volume);
    else if (warn) LSD_LOG_WARN("audio", "No Sound Class '%s'", name);
}

void SoundPlayer::trigger(const char* name) { set_param(name, param(name) + 1.0f); }  // a Trigger fires on any change

void SoundPlayer::set_param(const char* name, f32 value) {
    if (audio_) audio_->set_synth_param(name, value);
    for (u32 i = 0; i < param_count_; ++i) {
        if (std::strcmp(params_[i].name, name) == 0) {
            params_[i].value = value;
            return;
        }
    }
    if (param_count_ == sizeof params_ / sizeof params_[0]) return;
    std::snprintf(params_[param_count_].name, sizeof params_[0].name, "%s", name);
    params_[param_count_++].value = value;
}

f32 SoundPlayer::param(const char* name) const {
    for (u32 i = 0; i < param_count_; ++i) {
        if (std::strcmp(params_[i].name, name) == 0) return params_[i].value;
    }
    return 0.0f;
}

void SoundPlayer::stop_all_and_forget() {
    if (audio_) audio_->reset_classes();
    class_count_ = group_count_ = mix_count_ = param_count_ = 0;
    music_[0] = '\0';
    music_voice_ = 0;
    layer_count_ = 0;
    music_play_.active = false;
}

} // namespace lsd::editor
