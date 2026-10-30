#include "sound_player.h"

#include "lsd/asset/data_asset.h"
#include "lsd/foundation/log.h"

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

const asset::AssetRecord* SoundPlayer::find(const char* sound) const {
    asset::Guid guid;
    if (std::strlen(sound) == 32 && asset::parse_guid(sound, guid)) {
        const asset::AssetRecord* r = registry_->find(guid);
        return r && r->type == asset::AssetType::Sound ? r : nullptr;
    }
    usize hits[16];
    const usize n = registry_->search(sound, asset::AssetType::Sound, nullptr, hits, 16);
    for (usize i = 0; i < n; ++i) {
        if (std::strcmp(registry_->get(hits[i]).name, sound) == 0) return &registry_->get(hits[i]);
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
    const u32 index = audio_->add_class(r->name, parent, c.volume, c.pitch);
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
    asset::AssetMeta meta;
    registry_->load_meta(*r, meta);
    char path[300];
    registry_->source_path(*r, path, sizeof path);
    audio::PlayDesc d;
    d.path = path;
    d.name = r->name;
    d.spatial = spatial;
    d.position = position;
    load_by_guid(*registry_, meta.attenuation, d.attenuation);  // defaults when none is picked
    d.volume = meta.volume;
    d.pitch = meta.pitch;
    d.looping = meta.looping;
    d.stream = meta.stream;
    d.sound_class = class_index(meta.sound_class);
    d.concurrency = concurrency_index(meta.concurrency);
    return audio_->play(d);
}

void SoundPlayer::stop_all_and_forget() {
    if (audio_) audio_->reset_classes();
    class_count_ = group_count_ = 0;
}

} // namespace lsd::editor
