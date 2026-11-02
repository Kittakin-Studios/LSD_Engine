/* lsd_game.h: the C API for game code (Unreal: your project's C++ module).
 *
 * Game code is a Game.dll built from the project's Source/ folder; it includes only this header (or
 * lsd/game.hpp, the C++ layer over it), so it never depends on how the engine itself was compiled.
 * A *behaviour* is code attached to an object through a Native Script component, like a Lua script:
 * on_start, on_update, on_hit, overlaps and named events, with its own state per object.
 *
 * One source file of the module says LSD_GAME_MODULE(register_fn) (C) or LSD_GAME_MODULE() (C++);
 * the engine calls register_fn once when it loads the DLL, and again after each rebuild.
 * Units: metres, degrees, Z up; rotations are Euler X, then Y, then Z.  Docs: docs/CODING.md. */
#ifndef LSD_GAME_H
#define LSD_GAME_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LSD_GAME_API_VERSION 1

typedef uint32_t lsd_entity; /* an object; 0 = none */
typedef struct lsd_vec3 { float x, y, z; } lsd_vec3;
typedef struct lsd_hit { lsd_entity entity; lsd_vec3 point, normal; } lsd_hit;

/* Values a behaviour shows in Details under its Native Script (Unreal: Instance Editable). */
enum lsd_property_type { LSD_FLOAT = 0, LSD_INT = 1, LSD_BOOL = 2, LSD_TEXT = 3 };
typedef struct lsd_property {
    const char* name;
    int type;      /* lsd_property_type: float, int32_t, bool (1 byte), char[size] */
    size_t offset; /* in the behaviour's state */
    size_t size;   /* LSD_TEXT: the char array's size */
} lsd_property;

typedef struct lsd_behaviour {
    const char* name;  /* what the Native Script component picks */
    size_t state_size; /* per object, zeroed, when create is NULL */
    void* (*create)(void);         /* optional: makes the state (C++: new T) */
    void (*destroy)(void* state);  /* optional: frees it (C++: delete) */
    void (*on_start)(lsd_entity self, void* state);
    void (*on_update)(lsd_entity self, void* state, float dt);
    void (*on_stop)(lsd_entity self, void* state);
    void (*on_hit)(lsd_entity self, void* state, lsd_entity other);
    void (*on_overlap_begin)(lsd_entity self, void* state, lsd_entity other);
    void (*on_overlap_end)(lsd_entity self, void* state, lsd_entity other);
    /* Any other event: lsd_call / lsd_broadcast from any language, Call Event nodes, UI actions
     * (other = 0 for those). */
    void (*on_event)(lsd_entity self, void* state, const char* name, lsd_entity other);
    const lsd_property* properties;
    int property_count;
} lsd_behaviour;

/* A C / C++ script is the same thing: code on an object through its Native Script component. */
typedef lsd_behaviour lsd_script;

typedef struct lsd_registry lsd_registry;

/* Visual-script nodes from game code (Unreal: BlueprintCallable / BlueprintPure functions). A node
 * passes its inputs as values; the function fills *result when it has an output. */
enum lsd_value_type { LSD_VALUE_NONE = -1, LSD_VALUE_NUMBER = 0, LSD_VALUE_BOOL = 1, LSD_VALUE_TEXT = 2, LSD_VALUE_VECTOR = 3, LSD_VALUE_OBJECT = 4 };
typedef struct lsd_value {
    double number;     /* LSD_VALUE_NUMBER; LSD_VALUE_BOOL as 0 / 1 */
    lsd_vec3 vector;   /* LSD_VALUE_VECTOR */
    const char* text;  /* LSD_VALUE_TEXT: inputs live for the call; a result must outlive it (static) */
    lsd_entity object; /* LSD_VALUE_OBJECT */
} lsd_value;
typedef struct lsd_pin {
    const char* name;
    int type;                  /* lsd_value_type */
    const char* default_value; /* used when nothing is wired: "10", "true", "self" (objects), NULL = empty */
} lsd_pin;
typedef struct lsd_function {
    const char* name;     /* graphs save it ("Cpp_<name>"): renaming breaks them */
    const char* title;    /* shown; NULL = name */
    const char* category; /* node menu group; NULL = "Game" */
    const char* tooltip;
    const lsd_pin* inputs; /* up to 7 */
    int input_count;
    int output_type; /* lsd_value_type; LSD_VALUE_NONE: no Result pin */
    int pure;        /* no exec pins: called when its Result is needed (needs an output) */
    void (*call)(const lsd_value* args, int arg_count, lsd_value* result);
} lsd_function;

/* Everything the engine offers, filled in by the engine. Use the lsd_* functions below. */
typedef struct lsd_api {
    int version;
    void (*register_behaviour)(lsd_registry* registry, const lsd_behaviour* behaviour);
    void (*log)(int error, const char* text);
    /* objects */
    lsd_entity (*find)(const char* name);
    const char* (*name)(lsd_entity e);
    int (*alive)(lsd_entity e);
    void (*destroy)(lsd_entity e);
    lsd_entity (*spawn)(const char* asset, lsd_vec3 at);
    lsd_entity (*player)(void);
    double (*time)(void);
    /* transform (relative to the parent) */
    lsd_vec3 (*position)(lsd_entity e);
    void (*set_position)(lsd_entity e, lsd_vec3 p);
    lsd_vec3 (*rotation)(lsd_entity e);
    void (*set_rotation)(lsd_entity e, lsd_vec3 degrees);
    int (*has_tag)(lsd_entity e, const char* tag);
    /* components by type and field name, as in Details (bool / int fields read as numbers) */
    int (*has)(lsd_entity e, const char* component);
    int (*add)(lsd_entity e, const char* component);
    void (*remove)(lsd_entity e, const char* component);
    int (*get_number)(lsd_entity e, const char* component, const char* field, double* out);
    int (*set_number)(lsd_entity e, const char* component, const char* field, double value);
    int (*get_vec3)(lsd_entity e, const char* component, const char* field, lsd_vec3* out);
    int (*set_vec3)(lsd_entity e, const char* component, const char* field, lsd_vec3 value);
    int (*get_text)(lsd_entity e, const char* component, const char* field, char* out, size_t capacity);
    int (*set_text)(lsd_entity e, const char* component, const char* field, const char* value);
    /* input */
    lsd_vec3 (*input)(const char* action); /* x, y of an Input Action */
    int (*pressed)(const char* action);
    /* physics queries: 1 and *hit filled when something is hit */
    int (*raycast)(lsd_vec3 from, lsd_vec3 direction, float distance, lsd_hit* hit);
    int (*sphere_trace)(lsd_vec3 from, lsd_vec3 to, float radius, lsd_hit* hit);
    /* audio, animation, AI, camera, levels */
    uint32_t (*play_sound)(const char* name, int at_position, lsd_vec3 at);
    void (*stop_sound)(uint32_t id);
    void (*set_anim_param)(lsd_entity e, const char* name, float value);
    int (*move_to)(lsd_entity e, lsd_vec3 target);
    void (*camera_shake)(float strength, float seconds, float frequency);
    int (*open_level)(const char* name);
    /* UI documents */
    int (*ui_open)(const char* name);
    void (*ui_close)(int doc);
    void (*ui_set_text)(int doc, const char* id, const char* text);
    /* events between objects, reaching Lua, visual scripts and behaviours alike */
    int (*call)(lsd_entity target, const char* event, lsd_entity other);
    int (*broadcast)(const char* event);
    /* debug drawing (seconds 0 = this frame) */
    void (*draw_line)(lsd_vec3 a, lsd_vec3 b, lsd_vec3 color, float seconds);
    /* Added later; fields are only ever added at the end, so older modules keep working. */
    void (*register_function)(lsd_registry* registry, const lsd_function* function);
} lsd_api;

/* Set by LSD_GAME_MODULE before register_fn runs. */
extern const lsd_api* lsd_api_ptr;

#ifdef _WIN32
#define LSD_EXPORT __declspec(dllexport)
#else
#define LSD_EXPORT __attribute__((visibility("default")))
#endif

/* The module's entry point; put it in exactly one source file. */
#define LSD_GAME_MODULE_C(register_fn)                                                           \
    const lsd_api* lsd_api_ptr = 0;                                                              \
    LSD_EXPORT int lsd_game_register(const lsd_api* api, lsd_registry* registry) {               \
        if (!api || api->version != LSD_GAME_API_VERSION) return 0;                              \
        lsd_api_ptr = api;                                                                       \
        register_fn(registry);                                                                   \
        return 1;                                                                                \
    }
#ifndef __cplusplus
#define LSD_GAME_MODULE(register_fn) LSD_GAME_MODULE_C(register_fn)
#endif

static inline void lsd_register_behaviour(lsd_registry* r, const lsd_behaviour* b) { lsd_api_ptr->register_behaviour(r, b); }
static inline void lsd_register_script(lsd_registry* r, const lsd_script* s) { lsd_api_ptr->register_behaviour(r, s); }
/* A visual-script node (in the node menu once the module is compiled); call it from a register
 * function like lsd_register_script. */
static inline void lsd_register_function(lsd_registry* r, const lsd_function* f) { lsd_api_ptr->register_function(r, f); }

/* C scripts register themselves when the module loads: after a function that calls
 * lsd_register_script, put LSD_C_SCRIPT(that_function) in the same .c file. The module's
 * LSD_GAME_MODULE() (GameModule.cpp) collects them. */
void lsd_game_add_c_script(void (*register_fn)(lsd_registry* registry));
#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
/* A pointer in the C runtime's initializer table: runs when the DLL loads, before any C++ static. */
#define LSD_C_SCRIPT(register_fn)                                                                       \
    static void lsd_auto_##register_fn(void) { lsd_game_add_c_script(register_fn); }                  \
    __declspec(allocate(".CRT$XCU")) void (*lsd_auto_ptr_##register_fn)(void) = lsd_auto_##register_fn; \
    __pragma(comment(linker, "/include:lsd_auto_ptr_" #register_fn))
#else
#define LSD_C_SCRIPT(register_fn)                                                                       \
    __attribute__((constructor)) static void lsd_auto_##register_fn(void) { lsd_game_add_c_script(register_fn); }
#endif

static inline void lsd_vlog(int error, const char* format, va_list args) {
    char text[1024];
    vsnprintf(text, sizeof text, format, args);
    lsd_api_ptr->log(error, text);
}
static inline void lsd_log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    lsd_vlog(0, format, args);
    va_end(args);
}
static inline void lsd_log_error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    lsd_vlog(1, format, args);
    va_end(args);
}

static inline lsd_entity lsd_find(const char* name) { return lsd_api_ptr->find(name); }
static inline const char* lsd_name(lsd_entity e) { return lsd_api_ptr->name(e); }
static inline int lsd_alive(lsd_entity e) { return lsd_api_ptr->alive(e); }
static inline void lsd_destroy(lsd_entity e) { lsd_api_ptr->destroy(e); }
static inline lsd_entity lsd_spawn(const char* asset, lsd_vec3 at) { return lsd_api_ptr->spawn(asset, at); }
static inline lsd_entity lsd_player(void) { return lsd_api_ptr->player(); }
static inline double lsd_time(void) { return lsd_api_ptr->time(); }
static inline lsd_vec3 lsd_position(lsd_entity e) { return lsd_api_ptr->position(e); }
static inline void lsd_set_position(lsd_entity e, lsd_vec3 p) { lsd_api_ptr->set_position(e, p); }
static inline lsd_vec3 lsd_rotation(lsd_entity e) { return lsd_api_ptr->rotation(e); }
static inline void lsd_set_rotation(lsd_entity e, lsd_vec3 r) { lsd_api_ptr->set_rotation(e, r); }
static inline int lsd_has_tag(lsd_entity e, const char* tag) { return lsd_api_ptr->has_tag(e, tag); }
static inline int lsd_has(lsd_entity e, const char* c) { return lsd_api_ptr->has(e, c); }
static inline int lsd_add(lsd_entity e, const char* c) { return lsd_api_ptr->add(e, c); }
static inline void lsd_remove(lsd_entity e, const char* c) { lsd_api_ptr->remove(e, c); }
static inline int lsd_get_number(lsd_entity e, const char* c, const char* f, double* out) { return lsd_api_ptr->get_number(e, c, f, out); }
static inline int lsd_set_number(lsd_entity e, const char* c, const char* f, double v) { return lsd_api_ptr->set_number(e, c, f, v); }
static inline int lsd_get_vec3(lsd_entity e, const char* c, const char* f, lsd_vec3* out) { return lsd_api_ptr->get_vec3(e, c, f, out); }
static inline int lsd_set_vec3(lsd_entity e, const char* c, const char* f, lsd_vec3 v) { return lsd_api_ptr->set_vec3(e, c, f, v); }
static inline int lsd_get_text(lsd_entity e, const char* c, const char* f, char* out, size_t n) { return lsd_api_ptr->get_text(e, c, f, out, n); }
static inline int lsd_set_text(lsd_entity e, const char* c, const char* f, const char* v) { return lsd_api_ptr->set_text(e, c, f, v); }
static inline lsd_vec3 lsd_input(const char* action) { return lsd_api_ptr->input(action); }
static inline int lsd_pressed(const char* action) { return lsd_api_ptr->pressed(action); }
static inline int lsd_raycast(lsd_vec3 from, lsd_vec3 dir, float distance, lsd_hit* hit) { return lsd_api_ptr->raycast(from, dir, distance, hit); }
static inline int lsd_sphere_trace(lsd_vec3 from, lsd_vec3 to, float radius, lsd_hit* hit) { return lsd_api_ptr->sphere_trace(from, to, radius, hit); }
static inline uint32_t lsd_play_sound(const char* name) { lsd_vec3 none = {0, 0, 0}; return lsd_api_ptr->play_sound(name, 0, none); }
static inline uint32_t lsd_play_sound_at(const char* name, lsd_vec3 at) { return lsd_api_ptr->play_sound(name, 1, at); }
static inline void lsd_stop_sound(uint32_t id) { lsd_api_ptr->stop_sound(id); }
static inline void lsd_set_anim_param(lsd_entity e, const char* name, float v) { lsd_api_ptr->set_anim_param(e, name, v); }
static inline int lsd_move_to(lsd_entity e, lsd_vec3 target) { return lsd_api_ptr->move_to(e, target); }
static inline void lsd_camera_shake(float strength, float seconds, float frequency) { lsd_api_ptr->camera_shake(strength, seconds, frequency); }
static inline int lsd_open_level(const char* name) { return lsd_api_ptr->open_level(name); }
static inline int lsd_ui_open(const char* name) { return lsd_api_ptr->ui_open(name); }
static inline void lsd_ui_close(int doc) { lsd_api_ptr->ui_close(doc); }
static inline void lsd_ui_set_text(int doc, const char* id, const char* text) { lsd_api_ptr->ui_set_text(doc, id, text); }
static inline int lsd_call(lsd_entity target, const char* event, lsd_entity other) { return lsd_api_ptr->call(target, event, other); }
static inline int lsd_broadcast(const char* event) { return lsd_api_ptr->broadcast(event); }
static inline void lsd_draw_line(lsd_vec3 a, lsd_vec3 b, lsd_vec3 color, float seconds) { lsd_api_ptr->draw_line(a, b, color, seconds); }

#ifdef __cplusplus
}
#endif

#endif /* LSD_GAME_H */
