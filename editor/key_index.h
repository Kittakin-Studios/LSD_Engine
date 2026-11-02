#pragma once
// A hash index from string keys (GUIDs, mesh names) to slots in a cache's own array: one hash and one
// compare per lookup instead of a compare against every entry. The draw loop looks up a mesh, a
// material and its textures for every object in every pass, so this keeps big levels cheap.
// shortcut: std::unordered_map (editor-time code); a flat open-addressing table if it ever shows in profiles.
#include "lsd/foundation/types.h"

#include <unordered_map>

namespace lsd::editor {

class KeyIndex {
public:
    static u64 hash(const char* key, u32 salt = 0) {
        u64 h = 1469598103934665603ull ^ salt;  // FNV-1a, 64-bit
        for (const char* c = key; *c; ++c) h = (h ^ static_cast<u8>(*c)) * 1099511628211ull;
        return h;
    }
    // The slot stored for `key`, or -1. `matches(slot)` confirms it (two keys sharing a hash is
    // astronomically rare, but then the caller's own search still finds the right one).
    template <typename Matches> i64 find(const char* key, u32 salt, Matches&& matches) const {
        const auto it = map_.find(hash(key, salt));
        return it != map_.end() && matches(it->second) ? static_cast<i64>(it->second) : -1;
    }
    void add(const char* key, u32 salt, u32 slot) { map_.emplace(hash(key, salt), slot); }
    void clear() { map_.clear(); }

private:
    std::unordered_map<u64, u32> map_;
};

} // namespace lsd::editor
