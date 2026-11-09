#pragma once
// Ability system (Unreal's Gameplay Ability System, the smallest complete version):
//   Attribute Set      a component of named numbers (Health, MaxHealth, Stamina...), base and current
//   Gameplay Effect    a data asset that changes attributes: at once (Instant), for a while (Has Duration)
//                      or until removed (Infinite); a Period makes it tick (damage over time); it can
//                      grant gameplay tags while it lasts and stack
//   Gameplay Ability   a data asset: a cost, a cooldown, tag rules, and what it does (a script function
//                      and / or a montage)
//   Ability System     a component listing the abilities an object has
// The pure rules are here (tested in tests/world_tests.cpp); engine/script runs them in Play (lsd.apply_effect,
// lsd.try_activate_ability...) and calls on_attribute_changed.
#include "lsd/reflect/reflect.h"
#include "lsd/foundation/handle.h"

namespace lsd::world {

class World;
struct EntityTag;
using Entity = Handle<EntityTag>;

LSD_STRUCT()
struct Attribute {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The attribute's name (Health, MaxHealth, Stamina...): effects and scripts use it") char name[32] = "";
    LSD_PROPERTY(Edit, Tooltip = "Its base value: Instant and periodic effects (and lsd.set_attribute) change it for good") f32 base = 0.0f;
    LSD_PROPERTY(Visible, Tooltip = "Its value in Play: the base with the lasting effects' modifiers on it, clamped") f32 current = 0.0f;
    LSD_PROPERTY(Edit, Tooltip = "It never goes below this (Health stops at 0)") f32 min = 0.0f;
    LSD_PROPERTY(Edit, Tooltip = "It never goes above this (0 = no maximum)") f32 max = 0.0f;
    LSD_PROPERTY(Edit, Tooltip = "Another attribute that is its maximum (Health: MaxHealth); empty = Max only") char max_attribute[32] = "";
};

enum class ModifierOp : u32 { Add, Multiply, Override };

LSD_STRUCT()
struct AttributeModifier {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The attribute it changes (Health, Stamina...)") char attribute[32] = "";
    LSD_PROPERTY(Edit, Widget = "Choice:Add,Multiply,Override", Tooltip = "Add the magnitude (negative: take away), multiply by it, or replace the value with it") u32 op = 0;
    LSD_PROPERTY(Edit, Tooltip = "How much: -10 with Add is 10 damage") f32 magnitude = 0.0f;
};

enum class EffectDuration : u32 { Instant, HasDuration, Infinite };
enum class EffectStacking : u32 { None, Refresh, Stack };

// Gameplay Effect asset (New Data Asset > GameplayEffect).
LSD_STRUCT(Category = "Gameplay", Tooltip = "Changes attributes: at once, for a while or until removed; can tick, grant tags and stack")
struct GameplayEffect {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Choice:Instant,Has Duration,Infinite", Tooltip = "Instant: changes the base values once; Has Duration: lasts Duration seconds; Infinite: lasts until removed (lsd.remove_effect)")
    u32 duration_type = 0;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "How long it lasts (Has Duration)") f32 duration = 1.0f;
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "Periodic (damage over time): every Period its modifiers change the base values, first one Period after it is applied; 0 = not periodic (the modifiers hold while it lasts)")
    f32 period = 0.0f;
    LSD_PROPERTY(Edit, Tooltip = "What it does to attributes") List<AttributeModifier, 8> modifiers;
    LSD_PROPERTY(Edit, Widget = "Tags", Tooltip = "Gameplay tags the target has while it lasts (Status.Stunned, Cooldown.Fireball): lsd.has_tag sees them") char granted_tags[128] = "";
    LSD_PROPERTY(Edit, Widget = "Choice:None,Refresh,Stack", Tooltip = "Applied again while it lasts: None = another copy, Refresh = the timer starts over, Stack = one more stack (modifiers times stacks) up to Max Stacks")
    u32 stacking = 0;
    LSD_PROPERTY(Edit, Min = 1, Tooltip = "Stack: the most stacks it reaches") u32 max_stacks = 3;
    LSD_PROPERTY(Edit, Tooltip = "Gameplay Cue: a sound (asset name) played at the target when it is applied") char cue_sound[64] = "";
    LSD_PROPERTY(Edit, Tooltip = "Gameplay Cue: a particle system or prefab (asset name) spawned at the target when it is applied") char cue_effect[64] = "";
};

// One lasting effect on an Attribute Set (Play only).
LSD_STRUCT()
struct ActiveEffect {
    LSD_GENERATED()
    LSD_PROPERTY(Visible, Tooltip = "The Gameplay Effect asset") char name[64] = "";
    LSD_PROPERTY(Visible, Units = "s", Tooltip = "Seconds left (Infinite: unused)") f32 remaining = 0.0f;
    LSD_PROPERTY(Visible, Tooltip = "Its stacks") u32 stacks = 1;
    LSD_PROPERTY(Save) f32 period_left = 0.0f;
    LSD_PROPERTY(Save) GameplayEffect spec;  // a copy: periodic ticks need no asset load
};

LSD_STRUCT(Component, Category = "Gameplay", Tooltip = "Attributes (Health, Stamina...) that Gameplay Effects change; scripts get on_attribute_changed")
struct AttributeSet {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The attributes and their starting values") List<Attribute, 16> attributes;
    LSD_PROPERTY(Visible, Tooltip = "Gameplay Effects on it now (in Play)") List<ActiveEffect, 16> effects;
};

// Gameplay Ability asset (New Data Asset > GameplayAbility).
LSD_STRUCT(Category = "Gameplay", Tooltip = "Something an object can do: cost, cooldown, tag rules, a script function and / or a montage")
struct GameplayAbility {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:GameplayEffect", Tooltip = "Applied to the owner on activation (Instant, e.g. Stamina -20); it cannot activate if that would take an attribute below its Min")
    char cost_effect[33] = "";
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "Seconds before it can activate again") f32 cooldown = 0.0f;
    LSD_PROPERTY(Edit, Widget = "Asset:GameplayEffect", Tooltip = "Applied to the owner on activation (a Has Duration effect granting a Cooldown tag); it cannot activate while the owner has that effect's tags")
    char cooldown_effect[33] = "";
    LSD_PROPERTY(Edit, Widget = "Tags", Tooltip = "The owner must have all of these to activate it") char required_tags[128] = "";
    LSD_PROPERTY(Edit, Widget = "Tags", Tooltip = "It cannot activate while the owner has any of these (Status.Stunned)") char blocked_tags[128] = "";
    LSD_PROPERTY(Edit, Widget = "Tags", Tooltip = "Tags the owner has while it is active (State.Attacking)") char active_tags[128] = "";
    LSD_PROPERTY(Edit, Tooltip = "Called on the owner's scripts on activation: function(self, ability) (empty: none)") char function[48] = "";
    LSD_PROPERTY(Edit, Tooltip = "An animation (asset name) played on the owner as a montage on activation (empty: none)") char montage[64] = "";
    LSD_PROPERTY(Edit, Units = "s", Min = 0, Tooltip = "How long it stays active; 0 = the montage's length, or it ends at once without one (lsd.end_ability ends it early)")
    f32 active_seconds = 0.0f;
};

// One ability an object has, and its state in Play.
LSD_STRUCT()
struct GrantedAbility {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Widget = "Asset:GameplayAbility", Tooltip = "The Gameplay Ability") char ability[33] = "";
    LSD_PROPERTY(Visible, Tooltip = "Its asset name (filled in Play)") char name[64] = "";
    LSD_PROPERTY(Visible, Units = "s", Tooltip = "Cooldown seconds left") f32 cooldown_left = 0.0f;
    LSD_PROPERTY(Visible, Tooltip = "It is running now") bool active = false;
    LSD_PROPERTY(Visible, Units = "s", Tooltip = "Active seconds left") f32 active_left = 0.0f;
    LSD_PROPERTY(Save) char active_tags[128] = "";  // the ability's, while active
    LSD_PROPERTY(Save) bool montage = false;        // it played a montage (stopped on cancel)
};

LSD_STRUCT(Component, Category = "Gameplay", Tooltip = "The Gameplay Abilities this object has (lsd.try_activate_ability)")
struct AbilitySystem {
    LSD_GENERATED()
    LSD_PROPERTY(Edit, Tooltip = "The abilities it has (more with lsd.give_ability)") List<GrantedAbility, 16> abilities;
};

// ---- Rules ----
Attribute* find_attribute(AttributeSet& set, const char* name);
const Attribute* find_attribute(const AttributeSet& set, const char* name);
// Current values from the base values and the lasting (non-periodic) effects, then every value clamped.
void recompute(AttributeSet& set);
// Applies `effect` (asset `name`): Instant changes base values; others are added (or stacked / refreshed).
// False if the effect list is full.
bool apply_effect(AttributeSet& set, const GameplayEffect& effect, const char* name);
// Removes every copy of the effect `name`; how many.
u32 remove_effect(AttributeSet& set, const char* name);
// Periodic ticks and expiry; true if any attribute may have changed.
bool tick_effects(AttributeSet& set, f32 dt);
// Would the cost effect leave every attribute it changes at or above its Min?
bool can_afford(const AttributeSet* set, const GameplayEffect& cost);

enum class AbilityBlock : u32 { None, Active, Cooldown, MissingTags, BlockedTags, Cost };
// Why the ability cannot activate now (None: it can). `owner_tags`: the owner's tags (owned_tags);
// `cooldown_tags`: the cooldown effect's granted tags (or ""); `cost`: the cost effect (or null).
AbilityBlock can_activate(const GameplayAbility& ability, const GrantedAbility& state, const char* owner_tags, const char* cooldown_tags,
                          const AttributeSet* set, const GameplayEffect* cost);
const char* to_string(AbilityBlock block);
// Counts cooldowns and active time down; returns how many abilities ended this tick (ended[] gets their indices).
u32 tick_abilities(AbilitySystem& system, f32 dt, u32* ended, u32 capacity);

// Every gameplay tag the object has: its Tags component, its effects' granted tags and its active abilities' tags.
void owned_tags(const World& world, Entity entity, char* out, usize capacity);

} // namespace lsd::world
