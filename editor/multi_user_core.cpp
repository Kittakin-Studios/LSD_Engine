#include "multi_user_core.h"

#include "lsd/reflect/json.h"
#include "lsd/world/level.h"

#include <cstdlib>
#include <cstring>

namespace lsd::editor::multi_user {

namespace {

bool in_transient(const world::World& world, world::Entity e) {  // Sub Level contents: not part of the level
    for (; e.valid(); e = world.parent(e)) {
        if (world.transient(e)) return true;
    }
    return false;
}

std::string settings_text(const world::World& world) {
    std::string text(4096, '\0');
    const reflect::TypeInfo& type = reflect::type_of<world::WorldSettings>();
    usize n = reflect::save_text(type, &world.settings(), text.data(), text.size());
    if (n >= text.size()) {
        text.resize(n + 1);
        n = reflect::save_text(type, &world.settings(), text.data(), text.size());
    }
    text.resize(n);
    return text;
}

} // namespace

void capture(const world::World& world, Baseline& base) {
    base.objects.clear();
    Array<char> text(default_allocator());
    for (u32 slot = 0; slot < world.slot_count(); ++slot) {
        const world::Entity e = world.entity_at_slot(slot);
        if (!e.valid() || in_transient(world, e)) continue;
        world::save_entity(world, e, text);
        base.objects[e.value].assign(text.data(), text.size());
    }
    base.settings = settings_text(world);
}

std::string diff(const world::World& world, Baseline& base) {
    Baseline now;
    capture(world, now);
    std::string set, remove;
    for (const auto& [id, text] : now.objects) {
        const auto it = base.objects.find(id);
        if (it != base.objects.end() && it->second == text) continue;
        set += set.empty() ? "" : ",\n";
        set += text;
    }
    for (const auto& [id, text] : base.objects) {
        if (now.objects.count(id)) continue;
        remove += remove.empty() ? "" : ", ";
        remove += std::to_string(id);
    }
    std::string ops;
    if (now.settings != base.settings) ops += "\"settings\": " + now.settings;
    if (!remove.empty()) ops += (ops.empty() ? "" : ",\n") + std::string("\"remove\": [") + remove + "]";
    if (!set.empty()) ops += (ops.empty() ? "" : ",\n") + std::string("\"set\": [") + set + "]";
    base = std::move(now);
    return ops.empty() ? ops : "{" + ops + "}";
}

bool apply(world::World& world, const char* ops, usize length, Baseline& base) {
    reflect::JsonReader r{ops, ops + length};
    r.expect('{');
    while (r.ok && !r.peek('}')) {
        char key[16];
        r.read_string(key, sizeof key);
        r.expect(':');
        if (std::strcmp(key, "settings") == 0) {
            reflect::read_object(r, reflect::type_of<world::WorldSettings>(), &world.settings());
        } else if (std::strcmp(key, "remove") == 0) {  // before "set": a new object may reuse a removed one's slot
            r.expect('[');
            while (r.ok && !r.peek(']')) {
                char token[16];
                r.read_token(token, sizeof token);
                world::Entity e;
                e.value = static_cast<u32>(std::strtoul(token, nullptr, 10));
                world.destroy(e);  // dead or unknown ids: nothing happens
                if (!r.accept(',')) break;
            }
            r.expect(']');
        } else if (std::strcmp(key, "set") == 0) {
            r.skip_ws();
            const char* start = r.p;
            r.skip_value();
            if (r.ok && !world::load_entities(world, start, static_cast<usize>(r.p - start))) r.fail("bad object");
        } else {
            r.skip_value();  // from a newer editor
        }
        if (!r.accept(',')) break;
    }
    r.expect('}');
    // shortcut: re-captures the whole level per received op (as costly as an undo step); patch the
    // baseline per object when levels reach thousands of objects.
    capture(world, base);
    return r.ok;
}

} // namespace lsd::editor::multi_user
