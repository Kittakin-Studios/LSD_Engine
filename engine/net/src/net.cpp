#include "lsd/net/net.h"

#include "lsd/foundation/log.h"
#include "lsd/world/components.h"

#include <enet/enet.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace lsd::net {

namespace {

// Message types (first byte of every packet).
enum Msg : u8 { kWelcome = 1, kSpawn, kDestroy, kSnapshot, kOwnerState, kEvent, kPlayerJoined, kPlayerLeft, kChat, kGroup, kVoice, kVoiceOut, kOwner };
constexpr u32 kMaxVoiceBytes = 1000;
constexpr u8 kReliable = 0, kUnreliable = 1;  // ENet channels
constexpr u32 kLevelIdBit = 0x80000000u;      // ids from level objects' names; spawned ones count up below it
constexpr u32 kMaxEntriesPerPacket = 40;      // 28 bytes each: packets stay under a typical MTU
constexpr usize kMaxText = 16 * 1024;         // event JSON / chat text cap

int g_enet_users = 0;

u32 name_id(const char* name) {  // FNV-1a of the object's name: the same on every machine loading the level
    u32 h = 2166136261u;
    for (const char* c = name; *c; ++c) h = (h ^ static_cast<u8>(*c)) * 16777619u;
    return h | kLevelIdBit;
}

struct Writer {
    std::vector<u8> bytes;
    void put(const void* p, usize n) { bytes.insert(bytes.end(), static_cast<const u8*>(p), static_cast<const u8*>(p) + n); }
    void u8_(u8 v) { put(&v, 1); }
    void u16_(u16 v) { put(&v, 2); }
    void u32_(u32 v) { put(&v, 4); }
    void i32_(i32 v) { put(&v, 4); }
    void f64_(f64 v) { put(&v, 8); }
    void vec(Vec3 v) { put(&v.x, 4), put(&v.y, 4), put(&v.z, 4); }
    void str(const char* s, usize cap) {
        const usize n = std::min(std::strlen(s), cap);
        u32_(static_cast<u32>(n));
        put(s, n);
    }
};

// Reads with bounds checks: a malformed or hostile packet stops reading (ok = false), never crashes.
struct Reader {
    const u8* p;
    usize left;
    bool ok = true;
    bool get(void* out, usize n) {
        if (!ok || n > left) return ok = false;
        std::memcpy(out, p, n);
        p += n;
        left -= n;
        return true;
    }
    u8 u8_() { u8 v = 0; get(&v, 1); return v; }
    u16 u16_() { u16 v = 0; get(&v, 2); return v; }
    u32 u32_() { u32 v = 0; get(&v, 4); return v; }
    i32 i32_() { i32 v = 0; get(&v, 4); return v; }
    f64 f64_() { f64 v = 0; get(&v, 8); return v; }
    Vec3 vec() {
        Vec3 v{};
        get(&v.x, 4), get(&v.y, 4), get(&v.z, 4);
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) ok = false;
        return v;
    }
    std::string str(usize cap) {
        const u32 n = u32_();
        if (!ok || n > cap || n > left) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        left -= n;
        return s;
    }
};

f32 wrap_degrees(f32 a) {  // into -180..180, for the shortest turn between two rotations
    a = std::fmod(a + 180.0f, 360.0f);
    return (a < 0.0f ? a + 360.0f : a) - 180.0f;
}

} // namespace

const char* chat_channel_name(ChatChannel channel) {
    switch (channel) {
    case ChatChannel::Team: return "team";
    case ChatChannel::Squad: return "squad";
    case ChatChannel::Proximity: return "proximity";
    default: return "all";
    }
}

bool parse_chat_channel(const char* name, ChatChannel& out) {
    for (ChatChannel c : {ChatChannel::All, ChatChannel::Team, ChatChannel::Squad, ChatChannel::Proximity}) {
        if (std::strcmp(name, chat_channel_name(c)) == 0) {
            out = c;
            return true;
        }
    }
    return false;
}

struct NetSession::Impl {
    struct State {
        f64 t;
        Vec3 p, r;
    };
    struct Track {  // recent states of an object moved by someone else, played back with a delay
        State s[4];
        u32 count = 0;
    };
    struct Peer {
        ENetPeer* peer;
        u32 id;
    };
    struct Group {
        u32 team = 0, squad = 0;
    };
    struct Pending {
        NetEvent event;
        std::string text;
    };

    NetSession& self;
    world::World* world = nullptr;
    NetSettings settings;
    ENetHost* host = nullptr;
    ENetPeer* server = nullptr;  // client: the connection to the server
    bool welcomed = false;
    f64 clock = 0.0;     // local seconds since the session started
    f64 offset = 0.0;    // client: server clock minus ours
    bool offset_valid = false;
    f32 send_timer = 0.0f, keyframe_timer = 0.0f, stats_timer = 0.0f;
    u32 next_id = 1, next_player = 1;
    u32 last_sent_bytes = 0, last_received_bytes = 0;
    std::unordered_map<u32, world::Entity> by_id;
    std::unordered_map<u32, Track> tracks;
    std::unordered_map<u32, State> last_sent;   // server: what the clients last heard, per object
    std::vector<u32> destroyed_level_ids;       // server: told to players who join later
    std::vector<Peer> peers;
    std::unordered_map<u32, Group> groups;
    std::deque<Pending> events;
    std::string current;  // the taken event's text

    explicit Impl(NetSession& s) : self(s) {}

    void queue(NetEvent::Kind kind, u32 from, const char* name, std::string text = {}) {
        if (kind == NetEvent::Kind::Voice && events.size() > 256) return;  // nobody is taking them: drop audio, never grow
        Pending p;
        p.event.kind = kind;
        p.event.from = from;
        std::snprintf(p.event.name, sizeof p.event.name, "%s", name);
        p.text = std::move(text);
        events.push_back(std::move(p));
    }

    void send_to(ENetPeer* peer, const Writer& w, u8 channel) {
        if (!peer) return;
        ENetPacket* packet = enet_packet_create(w.bytes.data(), w.bytes.size(), channel == kReliable ? ENET_PACKET_FLAG_RELIABLE : 0);
        if (enet_peer_send(peer, channel, packet) != 0) enet_packet_destroy(packet);
    }
    void broadcast(const Writer& w, u8 channel, ENetPeer* except = nullptr) {
        for (const Peer& p : peers) {
            if (p.peer != except) send_to(p.peer, w, channel);
        }
    }
    u32 peer_id(ENetPeer* peer) const {
        for (const Peer& p : peers) {
            if (p.peer == peer) return p.id;
        }
        return 0xFFFFFFFFu;
    }
    ENetPeer* peer_of(u32 id) const {
        for (const Peer& p : peers) {
            if (p.id == id) return p.peer;
        }
        return nullptr;
    }

    // Level objects with NetReplicated get ids from their names, the same everywhere.
    void index_level() {
        const reflect::TypeInfo& type = reflect::type_of<world::NetReplicated>();
        for (u32 i = 0; i < world->count(type); ++i) {
            const world::Entity e = world->owner(type, i);
            auto* r = static_cast<world::NetReplicated*>(world->at(type, i));
            r->net_id = name_id(world->name(e));
            if (by_id.count(r->net_id)) LSD_LOG_WARN("net", "%s: another replicated object has a name with the same id; rename one", world->name(e));
            by_id[r->net_id] = e;
        }
    }

    world::NetReplicated* replicated(world::Entity e) const { return world->alive(e) ? world->get<world::NetReplicated>(e) : nullptr; }

    void write_spawn(Writer& w, u32 id, world::Entity e) {
        const auto* r = world->get<world::NetReplicated>(e);
        const auto* t = world->get<world::Transform>(e);
        w.u8_(kSpawn);
        w.u32_(id);
        w.u32_(r->owner);
        w.str(r->spawned_from, 63);
        w.str(world->name(e), 63);
        w.vec(t->position);
        w.vec(t->rotation);
    }

    void write_group(Writer& w, u32 player) {
        const Group g = groups[player];
        w.u8_(kGroup);
        w.u32_(player);
        w.u32_(g.team);
        w.u32_(g.squad);
    }

    // ---- Server ----

    void on_connect(ENetPeer* peer) {
        const u32 id = next_player++;
        peers.push_back({peer, id});
        enet_peer_timeout(peer, 0, 0, static_cast<u32>(settings.timeout_seconds * 1000.0f));
        Writer w;
        w.u8_(kWelcome);
        w.u32_(id);
        w.f64_(clock);
        send_to(peer, w, kReliable);
        // What already exists: spawned objects, level objects destroyed since, teams and squads.
        for (const auto& [net_id, e] : by_id) {
            if ((net_id & kLevelIdBit) || !replicated(e)) continue;
            Writer s;
            write_spawn(s, net_id, e);
            send_to(peer, s, kReliable);
        }
        for (u32 gone : destroyed_level_ids) {
            Writer d;
            d.u8_(kDestroy);
            d.u32_(gone);
            send_to(peer, d, kReliable);
        }
        for (const auto& [player, g] : groups) {
            Writer gw;
            write_group(gw, player);
            send_to(peer, gw, kReliable);
        }
        keyframe_timer = 1e9f;  // a full snapshot next tick
        Writer j;
        j.u8_(kPlayerJoined);
        j.u32_(id);
        broadcast(j, kReliable, peer);
        queue(NetEvent::Kind::PlayerJoined, id, "");
        LSD_LOG_INFO("net", "Player %u joined (%zu connected)", id, peers.size());
    }

    void on_disconnect(ENetPeer* peer) {
        const u32 id = peer_id(peer);
        for (usize i = 0; i < peers.size(); ++i) {
            if (peers[i].peer == peer) {
                peers.erase(peers.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        if (id == 0xFFFFFFFFu) return;
        // Their objects (the avatar) go with them, everywhere (the next send tells the clients).
        std::vector<world::Entity> owned;
        for (const auto& [net_id, e] : by_id) {
            if (const auto* r = replicated(e); r && r->owner == id) owned.push_back(e);
        }
        for (world::Entity e : owned) world->destroy(e);
        groups.erase(id);
        Writer w;
        w.u8_(kPlayerLeft);
        w.u32_(id);
        broadcast(w, kReliable);
        queue(NetEvent::Kind::PlayerLeft, id, "");
        LSD_LOG_INFO("net", "Player %u left", id);
    }

    void server_message(ENetPeer* peer, Reader& r) {
        const u32 from = peer_id(peer);
        if (from == 0xFFFFFFFFu) return;
        switch (r.u8_()) {
        case kOwnerState: {  // a client's own objects: accepted only for objects it owns
            const u16 count = r.u16_();
            for (u16 k = 0; k < count && r.ok; ++k) {
                const u32 id = r.u32_();
                const Vec3 p = r.vec(), rot = r.vec();
                if (!r.ok) break;
                const auto it = by_id.find(id);
                const auto* rep = it == by_id.end() ? nullptr : replicated(it->second);
                if (rep && rep->owner == from) push_state(id, {clock, p, rot});
            }
            break;
        }
        case kEvent: {
            r.i32_();  // the target: from a client always the server
            const std::string name = r.str(63), json = r.str(kMaxText);
            if (r.ok) queue(NetEvent::Kind::Call, from, name.c_str(), json);
            break;
        }
        case kChat: {
            const u8 channel = r.u8_();
            const std::string text = r.str(kMaxText);
            if (r.ok && channel <= static_cast<u8>(ChatChannel::Proximity)) route_chat(from, static_cast<ChatChannel>(channel), text);
            break;
        }
        case kVoice: {
            const u8 channel = r.u8_();
            const std::string packet = r.str(kMaxVoiceBytes);
            if (r.ok && channel <= static_cast<u8>(ChatChannel::Proximity)) route_voice(from, static_cast<ChatChannel>(channel), packet);
            break;
        }
        default: break;  // unknown or server-only messages from a client: ignored
        }
    }

    bool channel_on(ChatChannel c) const {
        switch (c) {
        case ChatChannel::Team: return settings.chat_team;
        case ChatChannel::Squad: return settings.chat_squad;
        case ChatChannel::Proximity: return settings.chat_proximity;
        default: return settings.chat_all;
        }
    }

    bool voice_on(ChatChannel c) const {
        switch (c) {
        case ChatChannel::Team: return settings.voice_team;
        case ChatChannel::Squad: return settings.voice_squad;
        case ChatChannel::Proximity: return settings.voice_proximity;
        default: return settings.voice_all;
        }
    }

    // Voice goes to the same listeners chat would, except the speaker (who does not hear itself).
    void route_voice(u32 from, ChatChannel channel, const std::string& packet) {
        if (!voice_on(channel) || packet.empty()) return;
        Writer w;
        w.u8_(kVoiceOut);
        w.u8_(static_cast<u8>(channel));
        w.u32_(from);
        w.u32_(static_cast<u32>(packet.size()));
        w.put(packet.data(), packet.size());
        for (const Peer& p : peers) {
            if (p.id != from && hears(from, p.id, channel)) send_to(p.peer, w, kUnreliable);
        }
        if (from != kServerPlayer && hears(from, kServerPlayer, channel)) queue(NetEvent::Kind::Voice, from, chat_channel_name(channel), packet);
    }

    bool avatar_position(u32 player, Vec3& out) const {  // the first object the player owns
        for (const auto& [id, e] : by_id) {
            if (const auto* r = replicated(e); r && r->owner == player) {
                const Mat4 m = world->world_matrix(e);
                out = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
                return true;
            }
        }
        return false;
    }

    bool hears(u32 sender, u32 listener, ChatChannel c) const {
        if (sender == listener) return true;
        const auto gs = groups.find(sender), gl = groups.find(listener);
        const Group s = gs == groups.end() ? Group{} : gs->second, l = gl == groups.end() ? Group{} : gl->second;
        switch (c) {
        case ChatChannel::Team: return s.team != 0 && s.team == l.team;
        case ChatChannel::Squad: return s.squad != 0 && s.team == l.team && s.squad == l.squad;
        case ChatChannel::Proximity: {
            Vec3 a, b;
            if (!avatar_position(sender, a) || !avatar_position(listener, b)) return false;
            const Vec3 d = a - b;
            return d.x * d.x + d.y * d.y + d.z * d.z <= settings.proximity_range * settings.proximity_range;
        }
        default: return true;
        }
    }

    // The server decides who gets a message (clients never pick their listeners themselves).
    void route_chat(u32 from, ChatChannel channel, const std::string& text) {
        if (!channel_on(channel) || text.empty()) return;
        Writer w;
        w.u8_(kChat);
        w.u8_(static_cast<u8>(channel));
        w.u32_(from);
        w.str(text.c_str(), kMaxText);
        for (const Peer& p : peers) {
            if (hears(from, p.id, channel)) send_to(p.peer, w, kReliable);
        }
        if (hears(from, kServerPlayer, channel)) queue(NetEvent::Kind::Chat, from, chat_channel_name(channel), text);
    }

    void server_send(f32 dt) {
        // Spawns: replicated objects without an id were made during play; destroys: ids whose object went.
        const reflect::TypeInfo& type = reflect::type_of<world::NetReplicated>();
        for (u32 i = 0; i < world->count(type); ++i) {
            auto* r = static_cast<world::NetReplicated*>(world->at(type, i));
            if (r->net_id) continue;
            const world::Entity e = world->owner(type, i);
            r->net_id = next_id++;
            by_id[r->net_id] = e;
            Writer w;
            write_spawn(w, r->net_id, e);
            broadcast(w, kReliable);
        }
        for (auto it = by_id.begin(); it != by_id.end();) {
            if (replicated(it->second)) {
                ++it;
                continue;
            }
            Writer w;
            w.u8_(kDestroy);
            w.u32_(it->first);
            broadcast(w, kReliable);
            if (it->first & kLevelIdBit) destroyed_level_ids.push_back(it->first);
            last_sent.erase(it->first);
            tracks.erase(it->first);
            it = by_id.erase(it);
        }
        send_timer += dt;
        keyframe_timer += dt;
        if (send_timer < 1.0f / settings.tick_rate) return;
        send_timer = 0.0f;
        const bool keyframe = keyframe_timer >= 1.0f;  // everything once a second: lost packets heal
        if (keyframe) keyframe_timer = 0.0f;
        Writer w;
        u16 count = 0;
        auto flush = [&] {
            if (!count) return;
            std::memcpy(w.bytes.data() + 9, &count, 2);
            broadcast(w, kUnreliable);
            count = 0;
        };
        auto begin = [&] {
            w.bytes.clear();
            w.u8_(kSnapshot);
            w.f64_(clock);
            w.u16_(0);  // count, filled by flush
        };
        begin();
        for (const auto& [id, e] : by_id) {
            const auto* r = replicated(e);
            if (!r || !r->replicate_movement) continue;
            const auto* t = world->get<world::Transform>(e);
            const auto sent = last_sent.find(id);
            if (!keyframe && sent != last_sent.end() && length(sent->second.p - t->position) < 1e-4f && length(sent->second.r - t->rotation) < 1e-3f) continue;
            last_sent[id] = {clock, t->position, t->rotation};
            w.u32_(id);
            w.vec(t->position);
            w.vec(t->rotation);
            if (++count == kMaxEntriesPerPacket) {
                flush();
                begin();
            }
        }
        flush();
    }

    // ---- Client ----

    void client_message(Reader& r) {
        const u8 type = r.u8_();
        switch (type) {
        case kWelcome: {
            self.player_id_ = r.u32_();
            const f64 server_time = r.f64_();
            if (!r.ok) return;
            offset = server_time - clock;
            offset_valid = true;
            welcomed = true;
            queue(NetEvent::Kind::Connected, self.player_id_, "");
            LSD_LOG_INFO("net", "Joined as player %u", self.player_id_);
            break;
        }
        case kSpawn: {
            const u32 id = r.u32_(), owner = r.u32_();
            const std::string asset = r.str(63), name = r.str(63);
            const Vec3 p = r.vec(), rot = r.vec();
            if (!r.ok || by_id.count(id)) return;
            const world::Entity e = self.spawn_ ? self.spawn_(self.spawn_user_, asset.c_str(), name.c_str(), p, rot) : world::Entity{};
            if (!e.valid()) {
                LSD_LOG_WARN("net", "The server spawned %s from '%s', which this machine cannot make", name.c_str(), asset.c_str());
                return;
            }
            auto* rep = world->get<world::NetReplicated>(e) ? world->get<world::NetReplicated>(e) : world->add<world::NetReplicated>(e);
            rep->net_id = id;
            rep->owner = owner;
            std::snprintf(rep->spawned_from, sizeof rep->spawned_from, "%s", asset.c_str());
            by_id[id] = e;
            break;
        }
        case kDestroy: {
            const u32 id = r.u32_();
            const auto it = r.ok ? by_id.find(id) : by_id.end();
            if (it == by_id.end()) return;
            if (world->alive(it->second)) world->destroy(it->second);
            tracks.erase(id);
            by_id.erase(it);
            break;
        }
        case kOwner: {  // who moves an object now (a car its driver's machine simulates)
            const u32 id = r.u32_(), owner = r.u32_();
            const auto it = r.ok ? by_id.find(id) : by_id.end();
            if (it == by_id.end()) return;
            if (auto* rep = replicated(it->second)) rep->owner = owner;
            tracks.erase(id);  // states from the old owner do not blend into the new one's
            break;
        }
        case kSnapshot: {
            const f64 server_time = r.f64_();
            const u16 count = r.u16_();
            if (!r.ok) return;
            // The server's clock now: the snapshot's time plus half the round trip.
            const f64 sample = server_time + (server ? server->roundTripTime * 0.0005 : 0.0) - clock;
            if (!offset_valid || std::fabs(sample - offset) > 0.25) offset = sample;
            else offset += (sample - offset) * 0.05;
            offset_valid = true;
            for (u16 k = 0; k < count && r.ok; ++k) {
                const u32 id = r.u32_();
                const Vec3 p = r.vec(), rot = r.vec();
                if (!r.ok) break;
                const auto it = by_id.find(id);
                const auto* rep = it == by_id.end() ? nullptr : replicated(it->second);
                if (rep && rep->owner != self.player_id_) push_state(id, {server_time, p, rot});
            }
            break;
        }
        case kEvent: {
            r.i32_();
            const std::string name = r.str(63), json = r.str(kMaxText);
            if (r.ok) queue(NetEvent::Kind::Call, kServerPlayer, name.c_str(), json);
            break;
        }
        case kPlayerJoined:
        case kPlayerLeft: {
            const u32 id = r.u32_();
            if (!r.ok) return;
            if (type == kPlayerLeft) groups.erase(id);
            queue(type == kPlayerJoined ? NetEvent::Kind::PlayerJoined : NetEvent::Kind::PlayerLeft, id, "");
            break;
        }
        case kChat: {
            const u8 channel = r.u8_();
            const u32 from = r.u32_();
            const std::string text = r.str(kMaxText);
            if (r.ok && channel <= static_cast<u8>(ChatChannel::Proximity)) queue(NetEvent::Kind::Chat, from, chat_channel_name(static_cast<ChatChannel>(channel)), text);
            break;
        }
        case kGroup: {
            const u32 player = r.u32_(), team = r.u32_(), squad = r.u32_();
            if (r.ok) groups[player] = {team, squad};
            break;
        }
        case kVoiceOut: {
            const u8 channel = r.u8_();
            const u32 from = r.u32_();
            const std::string packet = r.str(kMaxVoiceBytes);
            if (r.ok && channel <= static_cast<u8>(ChatChannel::Proximity)) queue(NetEvent::Kind::Voice, from, chat_channel_name(static_cast<ChatChannel>(channel)), packet);
            break;
        }
        default: break;
        }
    }

    void client_send(f32 dt) {
        send_timer += dt;
        if (!welcomed || send_timer < 1.0f / settings.tick_rate) return;
        send_timer = 0.0f;
        Writer w;
        w.u8_(kOwnerState);
        w.u16_(0);
        u16 count = 0;
        for (const auto& [id, e] : by_id) {
            const auto* r = replicated(e);
            if (!r || r->owner != self.player_id_ || count == kMaxEntriesPerPacket) continue;
            const auto* t = world->get<world::Transform>(e);
            w.u32_(id);
            w.vec(t->position);
            w.vec(t->rotation);
            ++count;
        }
        if (!count) return;
        std::memcpy(w.bytes.data() + 1, &count, 2);
        send_to(server, w, kUnreliable);
    }

    // ---- Both: playing back other machines' objects ----

    void push_state(u32 id, const State& s) {
        Track& t = tracks[id];
        if (t.count && s.t <= t.s[t.count - 1].t) return;  // late or repeated: older than what we have
        if (t.count == 4) {
            for (u32 i = 1; i < 4; ++i) t.s[i - 1] = t.s[i];
            t.count = 3;
        }
        t.s[t.count++] = s;
    }

    void apply_tracks() {
        // Clients play the server's states a little in the past; the server smooths clients' own
        // objects over about one of their sends.
        const f64 now = self.mode_ == Mode::Client ? clock + offset - settings.interpolation_delay : clock - 1.5 / settings.tick_rate;
        for (auto& [id, track] : tracks) {
            const auto it = by_id.find(id);
            if (it == by_id.end() || !track.count) continue;
            auto* transform = world->alive(it->second) ? world->get<world::Transform>(it->second) : nullptr;
            const auto* rep = replicated(it->second);
            if (!transform || (rep && rep->owner == self.player_id_)) continue;  // this machine moves its own objects
            State a = track.s[0], b = track.s[0];
            for (u32 i = 0; i < track.count; ++i) {
                if (track.s[i].t <= now) a = track.s[i];
                b = track.s[i];
                if (track.s[i].t >= now) break;
            }
            // Between two states: blend; past the newest: hold it (no guessing ahead, no overshoot).
            const f32 f = b.t > a.t ? static_cast<f32>(std::clamp((now - a.t) / (b.t - a.t), 0.0, 1.0)) : 1.0f;
            transform->position = a.p + (b.p - a.p) * f;
            transform->rotation = {a.r.x + wrap_degrees(b.r.x - a.r.x) * f, a.r.y + wrap_degrees(b.r.y - a.r.y) * f, a.r.z + wrap_degrees(b.r.z - a.r.z) * f};
        }
    }

    void pump() {
        ENetEvent ev;
        while (host && enet_host_service(host, &ev, 0) > 0) {  // never waits: the frame keeps its pace
            switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                if (self.mode_ == Mode::Server) on_connect(ev.peer);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                if (self.mode_ == Mode::Server) {
                    on_disconnect(ev.peer);
                } else {
                    server = nullptr;
                    welcomed = false;
                    queue(NetEvent::Kind::Disconnected, kServerPlayer, "");
                    LSD_LOG_WARN("net", "Disconnected from the server");
                }
                break;
            case ENET_EVENT_TYPE_RECEIVE: {
                Reader r{ev.packet->data, ev.packet->dataLength};
                if (self.mode_ == Mode::Server) server_message(ev.peer, r);
                else client_message(r);
                enet_packet_destroy(ev.packet);
                break;
            }
            default: break;
            }
        }
    }
};

NetSession::NetSession() = default;
NetSession::~NetSession() { shutdown(); }

bool NetSession::host(world::World& world, const NetSettings& settings) {
    shutdown();
    if (g_enet_users++ == 0 && enet_initialize() != 0) {
        --g_enet_users;
        LSD_LOG_ERROR("net", "Could not start networking (ENet)");
        return false;
    }
    impl_ = new Impl(*this);
    impl_->world = &world;
    impl_->settings = settings;
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = settings.port;
    impl_->host = enet_host_create(&address, settings.max_players, 2, 0, 0);
    if (!impl_->host) {
        LSD_LOG_ERROR("net", "Could not host on port %u (in use?)", settings.port);
        shutdown();
        return false;
    }
    mode_ = Mode::Server;
    player_id_ = kServerPlayer;
    impl_->index_level();
    LSD_LOG_INFO("net", "Hosting on port %u (up to %u players)", settings.port, settings.max_players);
    return true;
}

bool NetSession::join(world::World& world, const char* address_text, const NetSettings& settings) {
    shutdown();
    if (g_enet_users++ == 0 && enet_initialize() != 0) {
        --g_enet_users;
        LSD_LOG_ERROR("net", "Could not start networking (ENet)");
        return false;
    }
    impl_ = new Impl(*this);
    impl_->world = &world;
    impl_->settings = settings;
    impl_->host = enet_host_create(nullptr, 1, 2, 0, 0);
    ENetAddress address{};
    char name[128];
    std::snprintf(name, sizeof name, "%s", address_text);
    u16 port = settings.port;
    if (char* colon = std::strrchr(name, ':')) {  // "host:port"
        *colon = '\0';
        port = static_cast<u16>(std::atoi(colon + 1));
    }
    address.port = port;
    if (!impl_->host || enet_address_set_host(&address, name) != 0) {
        LSD_LOG_ERROR("net", "Could not find %s", address_text);
        shutdown();
        return false;
    }
    impl_->server = enet_host_connect(impl_->host, &address, 2, 0);
    if (!impl_->server) {
        shutdown();
        return false;
    }
    enet_peer_timeout(impl_->server, 0, 0, static_cast<u32>(settings.timeout_seconds * 1000.0f));
    mode_ = Mode::Client;
    player_id_ = 0xFFFFFFFFu;  // until welcomed
    impl_->index_level();
    LSD_LOG_INFO("net", "Connecting to %s:%u", name, port);
    return true;
}

void NetSession::shutdown() {
    if (!impl_) return;
    if (impl_->host) {
        if (impl_->server) enet_peer_disconnect_now(impl_->server, 0);
        for (const Impl::Peer& p : impl_->peers) enet_peer_disconnect_now(p.peer, 0);
        enet_host_flush(impl_->host);
        enet_host_destroy(impl_->host);
    }
    delete impl_;
    impl_ = nullptr;
    if (--g_enet_users == 0) enet_deinitialize();
    mode_ = Mode::Standalone;
    player_id_ = 0;
    stats_ = {};
}

bool NetSession::connected() const { return mode_ == Mode::Server || (impl_ && impl_->welcomed); }

void NetSession::receive(world::World& world, f32 dt) {
    if (!impl_) return;
    impl_->world = &world;
    impl_->clock += dt;
    impl_->pump();
    impl_->apply_tracks();
}

void NetSession::send(world::World& world, f32 dt) {
    if (!impl_) return;
    impl_->world = &world;
    if (mode_ == Mode::Server) impl_->server_send(dt);
    else impl_->client_send(dt);
    enet_host_flush(impl_->host);
    impl_->stats_timer += dt;
    if (impl_->stats_timer >= 1.0f) {
        stats_.bytes_out_per_second = static_cast<f32>(impl_->host->totalSentData - impl_->last_sent_bytes) / impl_->stats_timer;
        stats_.bytes_in_per_second = static_cast<f32>(impl_->host->totalReceivedData - impl_->last_received_bytes) / impl_->stats_timer;
        impl_->last_sent_bytes = impl_->host->totalSentData;
        impl_->last_received_bytes = impl_->host->totalReceivedData;
        impl_->stats_timer = 0.0f;
    }
    stats_.ping_ms = impl_->server ? static_cast<f32>(impl_->server->roundTripTime) : 0.0f;
    stats_.players = mode_ == Mode::Server ? static_cast<u32>(impl_->peers.size()) + 1 : 0;
    stats_.replicated = static_cast<u32>(impl_->by_id.size());
}

void NetSession::send_event(i32 target, const char* name, const char* json) {
    if (!impl_) return;
    Writer w;
    w.u8_(kEvent);
    w.i32_(target);
    w.str(name, 63);
    w.str(json && *json ? json : "[]", kMaxText);
    if (mode_ == Mode::Client) impl_->send_to(impl_->server, w, kReliable);
    else if (target == kToEveryone) impl_->broadcast(w, kReliable);
    else if (target >= 0) impl_->send_to(impl_->peer_of(static_cast<u32>(target)), w, kReliable);
}

bool NetSession::take_event(NetEvent& out) {
    if (!impl_ || impl_->events.empty()) return false;
    Impl::Pending p = std::move(impl_->events.front());
    impl_->events.pop_front();
    impl_->current = std::move(p.text);
    out = p.event;
    out.json = impl_->current.c_str();
    out.size = static_cast<u32>(impl_->current.size());
    return true;
}

void NetSession::set_owner(world::World& world, world::Entity e, u32 player) {
    auto* r = world.get<world::NetReplicated>(e) ? world.get<world::NetReplicated>(e) : world.add<world::NetReplicated>(e);
    if (r->owner == player) return;
    r->owner = player;
    if (!impl_ || mode_ != Mode::Server || !r->net_id) return;  // not yet known to clients: their spawn carries the owner
    impl_->tracks.erase(r->net_id);  // the old owner's states stop moving it here
    Writer w;  // a change after the spawn (a car handed to its driver's machine): everyone hears it
    w.u8_(kOwner);
    w.u32_(r->net_id);
    w.u32_(player);
    impl_->broadcast(w, kReliable);
}

void NetSession::set_player_group(u32 player, u32 team, u32 squad) {
    if (!impl_ || mode_ != Mode::Server) return;
    impl_->groups[player] = {team, squad};
    Writer w;
    impl_->write_group(w, player);
    impl_->broadcast(w, kReliable);
}

u32 NetSession::player_team(u32 player) const {
    if (!impl_) return 0;
    const auto it = impl_->groups.find(player);
    return it == impl_->groups.end() ? 0 : it->second.team;
}

u32 NetSession::player_squad(u32 player) const {
    if (!impl_) return 0;
    const auto it = impl_->groups.find(player);
    return it == impl_->groups.end() ? 0 : it->second.squad;
}

bool NetSession::voice_channel_on(ChatChannel channel) const { return impl_ && impl_->voice_on(channel); }

bool NetSession::player_position(u32 player, Vec3& out) const { return impl_ && impl_->avatar_position(player, out); }

bool NetSession::send_voice(ChatChannel channel, const u8* packet, u32 bytes) {
    if (!impl_ || !packet || !bytes || bytes > kMaxVoiceBytes || !impl_->voice_on(channel)) return false;
    if (mode_ == Mode::Server) {
        impl_->route_voice(kServerPlayer, channel, std::string(reinterpret_cast<const char*>(packet), bytes));
        return true;
    }
    if (!impl_->welcomed) return false;
    Writer w;
    w.u8_(kVoice);
    w.u8_(static_cast<u8>(channel));
    w.u32_(bytes);
    w.put(packet, bytes);
    impl_->send_to(impl_->server, w, kUnreliable);
    return true;
}

bool NetSession::send_chat(ChatChannel channel, const char* text) {
    if (!impl_ || !text || !*text || !impl_->channel_on(channel)) return false;
    if (mode_ == Mode::Server) {
        impl_->route_chat(kServerPlayer, channel, text);
        return true;
    }
    if (!impl_->welcomed) return false;
    Writer w;
    w.u8_(kChat);
    w.u8_(static_cast<u8>(channel));
    w.str(text, kMaxText);
    impl_->send_to(impl_->server, w, kReliable);
    return true;
}

} // namespace lsd::net
