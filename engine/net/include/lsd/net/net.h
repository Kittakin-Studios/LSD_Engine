#pragma once
// Multiplayer (Unreal's replication, our own design) on ENet: reliable UDP plus an unreliable channel.
//
// The server owns the game. Objects with a NetReplicated component replicate: at the tick rate the
// server sends a snapshot of the ones that moved (all of them once a second, so a lost packet only
// costs a moment); clients play them back a little in the past (the interpolation delay), blending
// between snapshots, so latency and lost packets show as smoothness instead of jitter. An object the
// client owns (its own player) is moved by that client and sent to the server, so the local player
// never waits for the network. Objects the server spawns appear on every client, and go when they
// are destroyed. Events are named calls with JSON arguments: to the server, to one client, or to all.
//
// Everything runs on the game thread in a few cheap calls (no blocking waits): receive() at the start
// of a frame, send() at the end. Nothing here runs unless a session was hosted or joined.
#include "lsd/foundation/math.h"
#include "lsd/world/world.h"

namespace lsd::net {

enum class Mode : u8 { Standalone, Server, Client };

struct NetSettings {
    u16 port = 7777;
    u32 max_players = 16;
    f32 tick_rate = 30.0f;            // snapshots (and owned-object updates) a second
    f32 interpolation_delay = 0.1f;   // seconds clients play other objects back in the past
    f32 timeout_seconds = 10.0f;      // a silent peer is dropped after this
    // Text chat: which channels the game offers (the server checks them; off channels are refused).
    bool chat_all = true;
    bool chat_team = true;
    bool chat_squad = false;
    bool chat_proximity = false;
    f32 proximity_range = 30.0f;  // metres between the players' avatars (their owned objects)
    // Voice chat channels (the same groups as text; proximity uses the same range).
    bool voice_all = false;
    bool voice_team = true;
    bool voice_squad = false;
    bool voice_proximity = false;
};

// Player 0 is the server's own (a listen server); clients get 1, 2, 3...
constexpr u32 kServerPlayer = 0;
constexpr i32 kToServer = -1;
constexpr i32 kToEveryone = -2;

// Text chat channels: everyone, the sender's team, its squad (within the team), or players nearby.
enum class ChatChannel : u8 { All, Team, Squad, Proximity };
const char* chat_channel_name(ChatChannel channel);  // "all", "team", "squad", "proximity"
bool parse_chat_channel(const char* name, ChatChannel& out);

struct NetEvent {
    enum class Kind : u8 { Call, PlayerJoined, PlayerLeft, Connected, Disconnected, Chat, Voice };
    Kind kind = Kind::Call;
    u32 from = 0;         // Call / Chat: the sender's player id; Joined / Left: the player
    char name[64] = "";   // Call: the event (a script function's name); Chat: the channel's name
    const char* json = "";  // Call: its arguments as a JSON array; Chat: the text; Voice: an Opus packet (valid until the next take_event)
    u32 size = 0;           // its length in bytes
};

struct NetStats {
    f32 ping_ms = 0.0f;
    f32 bytes_in_per_second = 0.0f;
    f32 bytes_out_per_second = 0.0f;
    u32 players = 0;
    u32 replicated = 0;
};

class NetSession {
public:
    NetSession();
    ~NetSession();
    NetSession(const NetSession&) = delete;
    NetSession& operator=(const NetSession&) = delete;

    // `spawn(user, asset, name, position, rotation)` makes a replicated object on this machine (the
    // server's spawns on clients; avatars of players): return the new entity, or invalid.
    using SpawnFn = world::Entity (*)(void* user, const char* asset, const char* name, Vec3 position, Vec3 rotation);
    void set_spawner(SpawnFn spawn, void* user) {
        spawn_ = spawn;
        spawn_user_ = user;
    }

    bool host(world::World& world, const NetSettings& settings);  // a server (listen or dedicated)
    bool join(world::World& world, const char* address, const NetSettings& settings);
    void shutdown();  // tells the others (a clean disconnect) and closes

    Mode mode() const { return mode_; }
    bool active() const { return mode_ != Mode::Standalone; }
    bool connected() const;  // client: welcomed by the server; server: always
    u32 player_id() const { return player_id_; }
    NetStats stats() const { return stats_; }

    // Start of the frame: network in, snapshots applied (clients move other objects), events queued.
    void receive(world::World& world, f32 dt);
    // End of the frame: server: snapshots, spawns, destroys; client: its owned objects.
    void send(world::World& world, f32 dt);

    // A named call with JSON array arguments: kToServer, kToEveryone (server only), or a player id.
    void send_event(i32 target, const char* name, const char* json);
    // The next queued event (calls, joins, leaves), or false. Its json stays valid until the next take.
    bool take_event(NetEvent& out);

    // A replicated object this machine controls: the server gives it to `player` (an avatar).
    void set_owner(world::World& world, world::Entity e, u32 player);

    // Teams and squads (server; every machine learns them): 0 = none. Chat and scripts use them.
    void set_player_group(u32 player, u32 team, u32 squad);
    u32 player_team(u32 player) const;
    u32 player_squad(u32 player) const;
    // Text chat from this machine's player. False if the channel is off, the text is empty, or there
    // is no session. Delivered as Chat events (to the sender too); the server routes and checks it.
    bool send_chat(ChatChannel channel, const char* text);
    // Voice chat: one Opus packet (20 ms) from this machine's player. Unreliable (late audio is useless);
    // the server routes it like chat. Delivered as Voice events to the listeners (not the speaker).
    bool send_voice(ChatChannel channel, const u8* packet, u32 bytes);
    bool voice_channel_on(ChatChannel channel) const;
    // Where a player's avatar is (proximity voice gets quieter with distance); false if unknown.
    bool player_position(u32 player, Vec3& out) const;

    struct Impl;

private:
    Impl* impl_ = nullptr;
    Mode mode_ = Mode::Standalone;
    u32 player_id_ = 0;
    NetStats stats_;
    SpawnFn spawn_ = nullptr;
    void* spawn_user_ = nullptr;
    friend struct Impl;
};

} // namespace lsd::net
