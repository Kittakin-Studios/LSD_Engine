// lsd/game.hpp: the C++ layer over lsd_game.h (header-only). A behaviour is a plain struct with the
// events it wants as members; LSD_BEHAVIOUR(Type) registers it. One source file says LSD_GAME_MODULE().
//
//   struct Door {
//       float open_angle = 90.0f;                                   // default for every object
//       static void properties(lsd::game::Properties<Door>& p) {   // optional: shown in Details
//           p.add("open_angle", &Door::open_angle);
//       }
//       void on_start(lsd::game::Entity self) {}
//       void on_update(lsd::game::Entity self, float dt) {}
//   };
//   LSD_BEHAVIOUR(Door)
//
// Events (all optional): on_start(self), on_update(self, dt), on_stop(self), on_hit(self, other),
// on_overlap_begin(self, other), on_overlap_end(self, other), on_event(self, name, other).
#pragma once
#include "lsd/lsd_game.h"

#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace lsd::game {

using Entity = lsd_entity;  // 0 = none
using Self = lsd_entity;

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3(lsd_vec3 v) : x(v.x), y(v.y), z(v.z) {}
    operator lsd_vec3() const { return {x, y, z}; }
    Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
};

struct Hit {
    Entity entity;
    Vec3 point, normal;
};

// ---- Commands (same names as Lua's lsd.*; docs/CODING.md) ----

template <typename... Args> void log(const char* format, Args... args) { lsd_log(format, args...); }
template <typename... Args> void log_error(const char* format, Args... args) { lsd_log_error(format, args...); }
inline Entity find(const char* name) { return lsd_find(name); }
inline const char* name(Entity e) { return lsd_name(e); }
inline bool alive(Entity e) { return lsd_alive(e) != 0; }
inline void destroy(Entity e) { lsd_destroy(e); }
inline Entity spawn(const char* asset, Vec3 at) { return lsd_spawn(asset, at); }
inline Entity player() { return lsd_player(); }
inline double time() { return lsd_time(); }
inline Vec3 position(Entity e) { return lsd_position(e); }
inline void set_position(Entity e, Vec3 p) { lsd_set_position(e, p); }
inline Vec3 rotation(Entity e) { return lsd_rotation(e); }
inline void set_rotation(Entity e, Vec3 degrees) { lsd_set_rotation(e, degrees); }
inline bool has_tag(Entity e, const char* tag) { return lsd_has_tag(e, tag) != 0; }
inline bool has(Entity e, const char* component) { return lsd_has(e, component) != 0; }
inline bool add(Entity e, const char* component) { return lsd_add(e, component) != 0; }
inline void remove(Entity e, const char* component) { lsd_remove(e, component); }
inline std::optional<double> get_number(Entity e, const char* component, const char* field) {
    double v = 0;
    if (!lsd_get_number(e, component, field, &v)) return std::nullopt;
    return v;
}
inline bool set_number(Entity e, const char* component, const char* field, double v) { return lsd_set_number(e, component, field, v) != 0; }
inline std::optional<Vec3> get_vec3(Entity e, const char* component, const char* field) {
    lsd_vec3 v{};
    if (!lsd_get_vec3(e, component, field, &v)) return std::nullopt;
    return Vec3(v);
}
inline bool set_vec3(Entity e, const char* component, const char* field, Vec3 v) { return lsd_set_vec3(e, component, field, v) != 0; }
inline std::optional<std::string> get_text(Entity e, const char* component, const char* field) {
    char text[512];
    if (!lsd_get_text(e, component, field, text, sizeof text)) return std::nullopt;
    return std::string(text);
}
inline bool set_text(Entity e, const char* component, const char* field, const char* v) { return lsd_set_text(e, component, field, v) != 0; }
inline Vec3 input(const char* action) { return lsd_input(action); }
inline bool pressed(const char* action) { return lsd_pressed(action) != 0; }
inline std::optional<Hit> raycast(Vec3 from, Vec3 direction, float distance) {
    lsd_hit h{};
    if (!lsd_raycast(from, direction, distance, &h)) return std::nullopt;
    return Hit{h.entity, h.point, h.normal};
}
inline std::optional<Hit> sphere_trace(Vec3 from, Vec3 to, float radius) {
    lsd_hit h{};
    if (!lsd_sphere_trace(from, to, radius, &h)) return std::nullopt;
    return Hit{h.entity, h.point, h.normal};
}
inline uint32_t play_sound(const char* name) { return lsd_play_sound(name); }
inline uint32_t play_sound(const char* name, Vec3 at) { return lsd_play_sound_at(name, at); }
inline void stop_sound(uint32_t id) { lsd_stop_sound(id); }
inline void set_anim_param(Entity e, const char* name, float v) { lsd_set_anim_param(e, name, v); }
inline bool move_to(Entity e, Vec3 target) { return lsd_move_to(e, target) != 0; }
inline void camera_shake(float strength, float seconds = 0.5f, float frequency = 12.0f) { lsd_camera_shake(strength, seconds, frequency); }
inline bool open_level(const char* name) { return lsd_open_level(name) != 0; }
inline int ui_open(const char* name) { return lsd_ui_open(name); }
inline void ui_close(int doc) { lsd_ui_close(doc); }
inline void ui_set_text(int doc, const char* id, const char* text) { lsd_ui_set_text(doc, id, text); }
inline int call(Entity target, const char* event, Entity other = 0) { return lsd_call(target, event, other); }
inline int broadcast(const char* event) { return lsd_broadcast(event); }
inline void draw_line(Vec3 a, Vec3 b, Vec3 color = {1, 0, 0}, float seconds = 0) { lsd_draw_line(a, b, color, seconds); }

// ---- Behaviours ----

template <typename T> struct Properties {
    std::vector<lsd_property> list;
    void add(const char* name, float T::*member) { push(name, LSD_FLOAT, member, 0); }
    void add(const char* name, int32_t T::*member) { push(name, LSD_INT, member, 0); }
    void add(const char* name, bool T::*member) { push(name, LSD_BOOL, member, 0); }
    template <size_t N> void add(const char* name, char (T::*member)[N]) { push(name, LSD_TEXT, member, N); }

private:
    template <typename M> void push(const char* name, int type, M T::*member, size_t size) {
        alignas(T) static unsigned char probe[sizeof(T)];  // never constructed: only member addresses are taken
        const T* object = reinterpret_cast<const T*>(probe);
        const size_t offset = static_cast<size_t>(reinterpret_cast<const unsigned char*>(&(object->*member)) - probe);
        list.push_back({name, type, offset, size});
    }
};

namespace detail {

inline std::vector<void (*)(lsd_registry*)>& registrars() {
    static std::vector<void (*)(lsd_registry*)> list;
    return list;
}

template <typename T> struct Behaviour {
    static T& self_state(void* s) { return *static_cast<T*>(s); }
    static void register_it(lsd_registry* registry) {
        static Properties<T> props;
        props.list.clear();
        if constexpr (requires(Properties<T>& p) { T::properties(p); }) T::properties(props);
        static lsd_behaviour b{};
        b.name = name;
        b.state_size = sizeof(T);
        b.create = [] { return static_cast<void*>(new T()); };
        b.destroy = [](void* s) { delete static_cast<T*>(s); };
        if constexpr (requires(T t) { t.on_start(Entity{}); }) b.on_start = [](lsd_entity e, void* s) { self_state(s).on_start(e); };
        if constexpr (requires(T t) { t.on_update(Entity{}, 0.0f); }) b.on_update = [](lsd_entity e, void* s, float dt) { self_state(s).on_update(e, dt); };
        if constexpr (requires(T t) { t.on_stop(Entity{}); }) b.on_stop = [](lsd_entity e, void* s) { self_state(s).on_stop(e); };
        if constexpr (requires(T t) { t.on_hit(Entity{}, Entity{}); }) b.on_hit = [](lsd_entity e, void* s, lsd_entity o) { self_state(s).on_hit(e, o); };
        if constexpr (requires(T t) { t.on_overlap_begin(Entity{}, Entity{}); })
            b.on_overlap_begin = [](lsd_entity e, void* s, lsd_entity o) { self_state(s).on_overlap_begin(e, o); };
        if constexpr (requires(T t) { t.on_overlap_end(Entity{}, Entity{}); })
            b.on_overlap_end = [](lsd_entity e, void* s, lsd_entity o) { self_state(s).on_overlap_end(e, o); };
        if constexpr (requires(T t) { t.on_event(Entity{}, "", Entity{}); })
            b.on_event = [](lsd_entity e, void* s, const char* n, lsd_entity o) { self_state(s).on_event(e, n, o); };
        b.properties = props.list.data();
        b.property_count = static_cast<int>(props.list.size());
        lsd_register_behaviour(registry, &b);
    }
    static inline const char* name = "";
};

template <typename T> struct Registrar {
    explicit Registrar(const char* behaviour_name) {
        Behaviour<T>::name = behaviour_name;
        registrars().push_back(&Behaviour<T>::register_it);
    }
};

inline void register_all(lsd_registry* registry) {
    for (auto* fn : registrars()) fn(registry);
}

} // namespace detail
} // namespace lsd::game

#define LSD_BEHAVIOUR(Type) static ::lsd::game::detail::Registrar<Type> lsd_registrar_##Type(#Type);

// The module's entry point: in exactly one .cpp file of Source/.
#define LSD_GAME_MODULE()                                                                   \
    static void lsd_register_cpp(lsd_registry* registry) { ::lsd::game::detail::register_all(registry); } \
    extern "C" {                                                                            \
    LSD_GAME_MODULE_C(lsd_register_cpp)                                                     \
    }
