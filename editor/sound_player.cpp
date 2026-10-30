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

u32 SoundPlayer::play(const char* sound, bool spatial, Vec3 position) {
    if (!audio_ || !registry_ || !sound || !sound[0]) return 0;
    const asset::AssetRecord* r = find(sound);
    if (!r) {
        LSD_LOG_WARN("audio", "No sound asset '%s'", sound);
        return 0;
    }
    if (r->type == asset::AssetType::Cue) return play_cue(*r, spatial, position);
    asset::AssetMeta meta;
    registry_->load_meta(*r, meta);
    return play_file(*r, nullptr, spatial, position, 1.0f, 1.0f, meta.looping, 0.0f, 0);
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
    return audio_->play(d);
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
    usize hits[16];
    const usize n = registry_->search(name, asset::AssetType::Data, nullptr, hits, 16);
    const asset::AssetRecord* r = nullptr;
    for (usize i = 0; i < n && !r; ++i) {
        const asset::AssetRecord& candidate = registry_->get(hits[i]);
        if (std::strcmp(candidate.name, name) == 0 && std::strcmp(candidate.data_type, "SoundMix") == 0) r = &candidate;
    }
    char path[300];
    audio::SoundMix mix;
    if (r) registry_->source_path(*r, path, sizeof path);
    if (!r || !asset::load_data_asset(path, mix)) {
        LSD_LOG_WARN("audio", "No Sound Mix '%s'", name);
        return;
    }
    audio::MixAdjust adjust[16];
    for (u32 i = 0; i < mix.adjusters.count; ++i) adjust[i] = {class_index(mix.adjusters[i].sound_class), mix.adjusters[i].volume};
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

void SoundPlayer::set_class_volume(const char* name, f32 volume) {
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
    else LSD_LOG_WARN("audio", "No Sound Class '%s'", name);
}

void SoundPlayer::set_param(const char* name, f32 value) {
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
}

} // namespace lsd::editor
