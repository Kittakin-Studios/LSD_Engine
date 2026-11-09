#include "lsd/world/abilities.h"
#include "lsd/world/world.h"

#include <cstdio>
#include <cstring>

namespace lsd::world {

namespace {
constexpr f32 kEpsilon = 1e-4f;  // timers that land within float noise of zero count as done

f32 apply_op(f32 value, ModifierOp op, f32 magnitude) {
    switch (op) {
        case ModifierOp::Add: return value + magnitude;
        case ModifierOp::Multiply: return value * magnitude;
        case ModifierOp::Override: return magnitude;
    }
    return value;
}

f32 clamp_attribute(const AttributeSet& set, const Attribute& a, f32 value) {
    f32 high = a.max > 0.0f ? a.max : 3.4e38f;
    if (const Attribute* m = a.max_attribute[0] ? find_attribute(set, a.max_attribute) : nullptr) high = high < m->current ? high : m->current;
    value = value < high ? value : high;
    return value > a.min ? value : a.min;
}

// Instant execution: each modifier changes its attribute's base value for good.
void execute(AttributeSet& set, const GameplayEffect& effect, u32 stacks) {
    for (const AttributeModifier& m : effect.modifiers) {
        Attribute* a = find_attribute(set, m.attribute);
        if (!a) continue;
        for (u32 s = 0; s < stacks; ++s) a->base = apply_op(a->base, static_cast<ModifierOp>(m.op), m.magnitude);
    }
    recompute(set);
    for (Attribute& a : set.attributes) a.base = clamp_attribute(set, a, a.base);
    recompute(set);
}

void append_tags(char* out, usize capacity, const char* tags) {
    if (!tags[0]) return;
    const usize used = std::strlen(out);
    std::snprintf(out + used, capacity - used, "%s%s", used ? ", " : "", tags);
}
} // namespace

Attribute* find_attribute(AttributeSet& set, const char* name) {
    for (Attribute& a : set.attributes) {
        if (std::strcmp(a.name, name) == 0) return &a;
    }
    return nullptr;
}

const Attribute* find_attribute(const AttributeSet& set, const char* name) { return find_attribute(const_cast<AttributeSet&>(set), name); }

void recompute(AttributeSet& set) {
    for (Attribute& a : set.attributes) {
        f32 add = 0.0f, multiply = 1.0f, value = 0.0f;
        bool overridden = false;
        for (const ActiveEffect& e : set.effects) {
            if (e.spec.period > 0.0f) continue;  // periodic effects change the base when they tick
            for (const AttributeModifier& m : e.spec.modifiers) {
                if (std::strcmp(m.attribute, a.name) != 0) continue;
                for (u32 s = 0; s < e.stacks; ++s) {
                    if (m.op == static_cast<u32>(ModifierOp::Add)) add += m.magnitude;
                    else if (m.op == static_cast<u32>(ModifierOp::Multiply)) multiply *= m.magnitude;
                }
                if (m.op == static_cast<u32>(ModifierOp::Override)) overridden = true, value = m.magnitude;
            }
        }
        a.current = overridden ? value : (a.base + add) * multiply;
    }
    // Clamped after every value is known, so Health sees MaxHealth's buffed value.
    for (Attribute& a : set.attributes) a.current = clamp_attribute(set, a, a.current);
}

bool apply_effect(AttributeSet& set, const GameplayEffect& effect, const char* name) {
    if (effect.duration_type == static_cast<u32>(EffectDuration::Instant)) {
        execute(set, effect, 1);
        return true;
    }
    if (effect.stacking != static_cast<u32>(EffectStacking::None)) {
        for (ActiveEffect& e : set.effects) {
            if (std::strcmp(e.name, name) != 0) continue;
            e.remaining = effect.duration;
            if (effect.stacking == static_cast<u32>(EffectStacking::Stack) && e.stacks < (effect.max_stacks ? effect.max_stacks : 1u)) ++e.stacks;
            recompute(set);
            return true;
        }
    }
    ActiveEffect e;
    std::snprintf(e.name, sizeof e.name, "%s", name);
    e.remaining = effect.duration;
    e.period_left = effect.period;
    e.spec = effect;
    if (!set.effects.push_back(e)) return false;
    recompute(set);
    return true;
}

u32 remove_effect(AttributeSet& set, const char* name) {
    u32 removed = 0;
    for (u32 i = set.effects.count; i-- > 0;) {
        if (std::strcmp(set.effects[i].name, name) == 0) set.effects.remove(i), ++removed;
    }
    if (removed) recompute(set);
    return removed;
}

bool tick_effects(AttributeSet& set, f32 dt) {
    bool changed = false;
    for (u32 i = 0; i < set.effects.count;) {
        ActiveEffect& e = set.effects[i];
        const bool timed = e.spec.duration_type == static_cast<u32>(EffectDuration::HasDuration);
        const f32 lived = timed && e.remaining + kEpsilon < dt ? e.remaining + kEpsilon : dt;  // ticks only inside its lifetime
        if (timed) e.remaining -= dt;
        if (e.spec.period > 0.0f) {
            e.period_left -= lived;
            while (e.period_left <= kEpsilon) {
                e.period_left += e.spec.period;
                execute(set, e.spec, e.stacks);
                changed = true;
            }
        }
        if (timed && e.remaining <= kEpsilon) {
            set.effects.remove(i);
            changed = true;
            continue;
        }
        ++i;
    }
    if (changed) recompute(set);
    return changed;
}

bool can_afford(const AttributeSet* set, const GameplayEffect& cost) {
    for (const AttributeModifier& m : cost.modifiers) {
        const Attribute* a = set ? find_attribute(*set, m.attribute) : nullptr;
        if (!a) return false;  // a cost on something the owner lacks cannot be paid
        if (apply_op(a->base, static_cast<ModifierOp>(m.op), m.magnitude) < a->min - kEpsilon) return false;
    }
    return true;
}

AbilityBlock can_activate(const GameplayAbility& ability, const GrantedAbility& state, const char* owner_tags, const char* cooldown_tags,
                          const AttributeSet* set, const GameplayEffect* cost) {
    if (state.active) return AbilityBlock::Active;
    if (state.cooldown_left > kEpsilon || (cooldown_tags[0] && tags_query(owner_tags, cooldown_tags, TagQuery::Any))) return AbilityBlock::Cooldown;
    if (ability.required_tags[0] && !tags_query(owner_tags, ability.required_tags, TagQuery::All)) return AbilityBlock::MissingTags;
    if (ability.blocked_tags[0] && tags_query(owner_tags, ability.blocked_tags, TagQuery::Any)) return AbilityBlock::BlockedTags;
    if (cost && !can_afford(set, *cost)) return AbilityBlock::Cost;
    return AbilityBlock::None;
}

const char* to_string(AbilityBlock block) {
    switch (block) {
        case AbilityBlock::None: return "";
        case AbilityBlock::Active: return "active";
        case AbilityBlock::Cooldown: return "cooldown";
        case AbilityBlock::MissingTags: return "missing tags";
        case AbilityBlock::BlockedTags: return "blocked";
        case AbilityBlock::Cost: return "cost";
    }
    return "";
}

u32 tick_abilities(AbilitySystem& system, f32 dt, u32* ended, u32 capacity) {
    u32 count = 0;
    for (u32 i = 0; i < system.abilities.count; ++i) {
        GrantedAbility& g = system.abilities[i];
        if (g.cooldown_left > 0.0f) g.cooldown_left = g.cooldown_left > dt ? g.cooldown_left - dt : 0.0f;
        if (!g.active) continue;
        g.active_left -= dt;
        if (g.active_left > kEpsilon) continue;
        g.active = false;
        g.active_tags[0] = '\0';
        if (count < capacity) ended[count++] = i;
    }
    return count;
}

void owned_tags(const World& world, Entity entity, char* out, usize capacity) {
    out[0] = '\0';
    if (const Tags* tags = world.get<Tags>(entity)) append_tags(out, capacity, tags->tags);
    if (const AttributeSet* set = world.get<AttributeSet>(entity)) {
        for (const ActiveEffect& e : set->effects) append_tags(out, capacity, e.spec.granted_tags);
    }
    if (const AbilitySystem* abilities = world.get<AbilitySystem>(entity)) {
        for (const GrantedAbility& g : abilities->abilities) {
            if (g.active) append_tags(out, capacity, g.active_tags);
        }
    }
}

} // namespace lsd::world
