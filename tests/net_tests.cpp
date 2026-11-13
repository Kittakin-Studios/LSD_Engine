// Multiplayer tests: a server and a client in one process over loopback, each with its own world.
#include "lsd/audio/voice_io.h"
#include "lsd/net/join_code.h"
#include "lsd/net/net.h"
#include "lsd/net/voice.h"
#include "lsd/world/components.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

static int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); ++g_failures; } } while (0)

using namespace lsd;
using namespace lsd::world;

static World* g_client_world = nullptr;
static Entity client_spawn(void*, const char*, const char* name, Vec3 position, Vec3 rotation) {
    const Entity e = g_client_world->create(name);
    g_client_world->get<Transform>(e)->position = position;
    g_client_world->get<Transform>(e)->rotation = rotation;
    return e;
}

static Entity find(World& w, const char* name) {
    for (u32 slot = 0; slot < w.slot_count(); ++slot) {
        const Entity e = w.entity_at_slot(slot);
        if (e.valid() && std::strcmp(w.name(e), name) == 0) return e;
    }
    return {};
}

// Join codes: round trip, typos caught, addresses typed as they are, private ranges.
static void test_join_codes() {
    char code[16], address[32];
    CHECK(net::make_join_code("203.0.113.45", 7777, code, sizeof code) && std::strlen(code) == 11 && code[5] == '-');
    CHECK(net::parse_join_code(code, 7777, address, sizeof address) && std::strcmp(address, "203.0.113.45:7777") == 0);
    char lower[16];
    for (int i = 0; i < 12; ++i) lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(code[i])));
    CHECK(net::parse_join_code(lower, 1, address, sizeof address) && std::strcmp(address, "203.0.113.45:7777") == 0);  // any case
    char typo[16];
    std::snprintf(typo, sizeof typo, "%s", code);
    typo[3] = typo[3] == '7' ? '8' : '7';
    CHECK(!net::parse_join_code(typo, 7777, address, sizeof address));  // the check bits catch it
    CHECK(!net::parse_join_code("ABC", 7777, address, sizeof address) && !net::parse_join_code("", 7777, address, sizeof address));
    CHECK(net::parse_join_code("100.101.7.9", 7777, address, sizeof address) && std::strcmp(address, "100.101.7.9:7777") == 0);
    CHECK(net::parse_join_code("10.0.0.2:9000", 7777, address, sizeof address) && std::strcmp(address, "10.0.0.2:9000") == 0);
    CHECK(!net::parse_join_code("10.0.0.2:99999", 7777, address, sizeof address));
    CHECK(net::is_private_address("192.168.1.4") && net::is_private_address("100.64.0.1") && net::is_private_address("172.20.1.1"));
    CHECK(!net::is_private_address("203.0.113.45") && !net::is_private_address("100.128.0.1") && !net::is_private_address("172.32.0.1"));
    char locals[8][16];
    const u32 n = net::local_addresses(locals, 8);
    for (u32 i = 0; i < n; ++i) CHECK(std::strncmp(locals[i], "127.", 4) != 0);
}

// Push-to-talk's path without hardware (miniaudio's null devices, their callbacks on their own threads):
// the microphone's frames read while the game thread plays other speakers, silence / noise / out-of-range
// samples through Opus, broken packets, more speakers than slots, start and stop again. The owner's
// "V crashed the game" report: none of it may crash, overrun a buffer or touch anything but audio.
static void test_voice_io() {
    audio::VoiceIO idle;  // never started: everything is a no-op
    f32 frame[net::kVoiceFrame] = {};
    CHECK(!idle.capturing() && !idle.read_frame(frame) && idle.level(1) == 0.0f);
    idle.play(1, frame, 1.0f);
    static_assert(audio::VoiceIO::kFrame == net::kVoiceFrame && audio::VoiceIO::kRate == net::kVoiceRate);

    net::VoiceEncoder encoder;
    net::VoiceDecoder decoder;
    u8 packet[net::kMaxVoicePacket];
    u32 seed = 1;
    auto noise = [&] { seed = seed * 1664525u + 1013904223u; return static_cast<f32>(seed >> 8) / 8388608.0f - 1.0f; };
    for (int kind = 0; kind < 3; ++kind) {  // silence, noise, clipping far past -1..1
        for (int n = 0; n < 10; ++n) {
            for (f32& s : frame) s = kind == 0 ? 0.0f : kind == 1 ? noise() : noise() * 40.0f;
            const u32 bytes = encoder.encode(frame, packet, sizeof packet);
            CHECK(bytes > 0 && bytes <= sizeof packet);
            CHECK(decoder.decode(packet, bytes, frame));
        }
    }
    CHECK(encoder.encode(frame, packet, 1) <= 1);  // a tiny buffer: at most a 1-byte packet, never an overrun
    for (u8& b : packet) {
        noise();
        b = static_cast<u8>(seed >> 24);
    }
    for (u32 bytes : {1u, 2u, 7u, 100u, net::kMaxVoicePacket}) decoder.decode(packet, bytes, frame);  // garbage: may fail, must not crash

    for (int pass = 0; pass < 3; ++pass) {  // started and stopped again, as Play does
        audio::VoiceIO io;
        if (!io.start(true, true)) {
            std::printf("voice: miniaudio null devices unavailable, skipped\n");
            return;
        }
        CHECK(io.capturing());
        u32 frames = 0, sent = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        while (std::chrono::steady_clock::now() < until) {
            while (io.read_frame(frame)) {  // talk(): every frame read, encoded while the key is down
                ++frames;
                if (frames % 2 && encoder.encode(frame, packet, sizeof packet)) ++sent;
            }
            for (f32& s : frame) s = noise();
            for (u32 speaker = 0; speaker < 20; ++speaker) io.play(speaker, frame, 0.5f);  // more than the 16 slots: the rest dropped
            CHECK(io.level(3) >= 0.0f && io.level(3) <= 1.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(frames >= 5 && sent > 0);  // ~20 frames in 400 ms
        io.stop();
        CHECK(!io.capturing() && !io.read_frame(frame));
    }
    audio::VoiceIO listen_only;
    CHECK(listen_only.start(false, true) && !listen_only.capturing() && !listen_only.read_frame(frame));
}

int main() {
    test_join_codes();
    test_voice_io();
    HeapAllocator heap("net");
    {
        World server_world(heap), client_world(heap);
        g_client_world = &client_world;
        for (World* w : {&server_world, &client_world}) w->add<NetReplicated>(w->create("Box"));  // the same level on both
        net::NetSettings settings;
        settings.port = 27651;
        settings.tick_rate = 60.0f;
        settings.interpolation_delay = 0.05f;
        net::NetSession server, client;
        client.set_spawner(client_spawn, nullptr);
        CHECK(server.host(server_world, settings));
        CHECK(client.join(client_world, "127.0.0.1:27651", settings));

        std::string server_calls, client_calls, server_chat, client_chat;
        u32 server_voice = 0;  // voice bytes the server's player heard
        u32 joined = 0;
        auto pump = [&](f32 seconds, auto&& each_frame) {
            for (f32 t = 0; t < seconds; t += 0.016f) {
                server.receive(server_world, 0.016f);
                client.receive(client_world, 0.016f);
                each_frame();
                server.send(server_world, 0.016f);
                client.send(client_world, 0.016f);
                net::NetEvent ev;
                while (server.take_event(ev)) {
                    if (ev.kind == net::NetEvent::Kind::PlayerJoined) joined = ev.from;
                    if (ev.kind == net::NetEvent::Kind::Call) server_calls += std::string(ev.name) + ev.json + std::to_string(ev.from) + ";";
                    if (ev.kind == net::NetEvent::Kind::Chat) server_chat += std::string(ev.name) + ":" + ev.json + ";";
                    if (ev.kind == net::NetEvent::Kind::Voice) server_voice += ev.size;
                }
                while (client.take_event(ev)) {
                    if (ev.kind == net::NetEvent::Kind::Call) client_calls += std::string(ev.name) + ev.json + ";";
                    if (ev.kind == net::NetEvent::Kind::Chat) client_chat += std::string(ev.name) + ":" + ev.json + ";";
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        };
        pump(1.0f, [] {});
        CHECK(client.connected() && client.player_id() == 1 && joined == 1);

        // Movement: the server moves the Box; the client's follows, smoothly, then settles on it.
        const Entity box = find(server_world, "Box");
        f32 x = 0;
        pump(1.0f, [&] { server_world.get<Transform>(box)->position.x = (x += 0.05f); });
        pump(0.5f, [] {});
        const Vec3 seen = client_world.get<Transform>(find(client_world, "Box"))->position;
        CHECK(std::fabs(seen.x - x) < 1e-3f);

        // Spawn and destroy: made on the server, appears on the client, goes when destroyed.
        const Entity rock = server_world.create("Rock");
        std::snprintf(server_world.add<NetReplicated>(rock)->spawned_from, 64, "Rock");
        server_world.get<Transform>(rock)->position = {3, 4, 5};
        pump(0.5f, [] {});
        const Entity client_rock = find(client_world, "Rock");
        CHECK(client_rock.valid() && client_world.get<Transform>(client_rock)->position.y == 4.0f);
        server_world.destroy(rock);
        pump(0.5f, [] {});
        CHECK(!client_world.alive(client_rock));

        // An object the client owns (its avatar): the client moves it, the server follows.
        const Entity avatar = server_world.create("Avatar1");
        server.set_owner(server_world, avatar, 1);
        pump(0.5f, [] {});
        const Entity client_avatar = find(client_world, "Avatar1");
        CHECK(client_avatar.valid() && client_world.get<NetReplicated>(client_avatar)->owner == 1);
        pump(0.6f, [&] { client_world.get<Transform>(client_avatar)->position = {7, 0, 1}; });
        CHECK(std::fabs(server_world.get<Transform>(avatar)->position.x - 7.0f) < 1e-3f);

        // Ownership handed over after the spawn (a car to its driver's machine) and back: the Box,
        // which the server moved, is now moved by the client; then by the server again.
        server.set_owner(server_world, box, 1);
        pump(0.3f, [] {});
        const Entity client_box = find(client_world, "Box");
        CHECK(client_world.get<NetReplicated>(client_box)->owner == 1);
        pump(0.6f, [&] { client_world.get<Transform>(client_box)->position = {-4, 2, 0}; });
        CHECK(std::fabs(server_world.get<Transform>(box)->position.x + 4.0f) < 1e-3f);
        server.set_owner(server_world, box, net::kServerPlayer);
        pump(0.3f, [&] { server_world.get<Transform>(box)->position = {9, 0, 0}; });
        pump(0.4f, [&] { server_world.get<Transform>(box)->position = {9, 0, 0}; });
        CHECK(client_world.get<NetReplicated>(client_box)->owner == net::kServerPlayer);
        CHECK(server_world.get<Transform>(box)->position.x == 9.0f);  // the client's old states no longer move it
        CHECK(std::fabs(client_world.get<Transform>(client_box)->position.x - 9.0f) < 1e-3f);

        // Replicated Fields: values the server changes reach the client's copy.
        server_world.add<StaticMesh>(box);
        client_world.add<StaticMesh>(client_box);
        std::snprintf(server_world.get<NetReplicated>(box)->replicated_fields, 256, " StaticMesh.color , StaticMesh.nothing, Missing.field");
        server_world.get<StaticMesh>(box)->color = {0.25f, 0.5f, 1.0f, 1.0f};
        pump(0.3f, [] {});
        CHECK(client_world.get<StaticMesh>(client_box)->color.g == 0.5f && client_world.get<StaticMesh>(client_box)->color.b == 1.0f);
        server_world.get<StaticMesh>(box)->color.r = 0.75f;  // sent again only when it changes
        pump(0.3f, [] {});
        CHECK(client_world.get<StaticMesh>(client_box)->color.r == 0.75f);

        // Lag compensation: the server rewinds a moving Box to where the client saw it.
        f32 moving = 9.0f;
        pump(1.0f, [&] { server_world.get<Transform>(box)->position.x = (moving += 0.16f); });  // 10 m/s
        Vec3 rewound;
        CHECK(server.seen_position(server_world, box, 1, rewound));
        std::printf("  lag compensation: now x %.2f, player 1 saw x %.2f\n", static_cast<double>(moving), static_cast<double>(rewound.x));
        CHECK(rewound.x < moving - 0.3f && rewound.x > moving - 3.0f);
        CHECK(server.seen_position(server_world, box, net::kServerPlayer, rewound) && std::fabs(rewound.x - moving) < 1e-3f);
        CHECK(!client.seen_position(client_world, client_box, 1, rewound));  // the server keeps the history

        // Relevancy: the Box farther than 5 m from player 1's avatar (at x 7) stops reaching it.
        server_world.get<NetReplicated>(box)->relevancy_distance = 5.0f;
        pump(0.4f, [&] { server_world.get<Transform>(box)->position = {8, 0, 0}; });
        CHECK(std::fabs(client_world.get<Transform>(client_box)->position.x - 8.0f) < 1e-3f);
        pump(0.6f, [&] { server_world.get<Transform>(box)->position = {60, 0, 0}; });
        CHECK(client_world.get<Transform>(client_box)->position.x < 20.0f);  // culled for this player
        pump(0.4f, [&] { server_world.get<Transform>(box)->position = {9, 1, 0}; });
        CHECK(std::fabs(client_world.get<Transform>(client_box)->position.y - 1.0f) < 1e-3f);  // relevant again
        server_world.get<NetReplicated>(box)->relevancy_distance = 0.0f;

        // Server-side move check: the client's avatar may move 5 m/s; a 90 m jump is refused and put back.
        server_world.get<NetReplicated>(avatar)->max_speed = 5.0f;
        pump(0.3f, [&] { client_world.get<Transform>(client_avatar)->position = {7.5f, 0, 1}; });  // allowed
        CHECK(std::fabs(server_world.get<Transform>(avatar)->position.x - 7.5f) < 1e-3f);
        client_calls.clear();
        client_world.get<Transform>(client_avatar)->position = {97.5f, 0, 1};
        pump(0.4f, [] {});
        CHECK(server_world.get<Transform>(avatar)->position.x < 10.0f);
        CHECK(client_world.get<Transform>(client_avatar)->position.x < 10.0f);  // corrected
        CHECK(client_calls.find("__corrected[") != std::string::npos);
        client_calls.clear();

        // The LAN browser finds this server (port + 1), with both players.
        net::LanGame games[4];
        u32 found = 0;
        std::thread browser([&] { found = net::NetSession::find_lan_games(settings.port, 0.5f, games, 4); });  // another machine's browser
        pump(0.7f, [] {});  // the server keeps running (and answering) meanwhile
        browser.join();
        std::printf("  LAN: %u game(s)%s%s\n", found, found ? ", " : "", found ? games[0].address : "");
        CHECK(found >= 1 && games[0].players == 2 && std::strcmp(games[0].name, "LSD_Engine game") == 0);

        // Events both ways.
        client.send_event(net::kToServer, "hello", "[1,\"x\"]");
        server.send_event(net::kToEveryone, "boom", "[2]");
        pump(0.3f, [] {});
        CHECK(server_calls == "hello[1,\"x\"]1;");
        CHECK(client_calls == "boom[2];");

        // Chat: team mates hear team chat (the sender sees its own), squad chat is off unless the game turns it on.
        server.set_player_group(0, 1, 0);
        server.set_player_group(1, 1, 0);
        pump(0.2f, [] {});
        CHECK(client.player_team(1) == 1);
        CHECK(client.send_chat(net::ChatChannel::Team, "go left"));
        CHECK(!client.send_chat(net::ChatChannel::Squad, "nope"));
        CHECK(server.send_chat(net::ChatChannel::All, "gg"));
        pump(0.3f, [] {});
        // Arrival order depends on timing (the server's own line is routed at once): check membership.
        auto has = [](const std::string& log, const char* line) { return log.find(line) != std::string::npos; };
        CHECK(has(server_chat, "team:go left;") && has(server_chat, "all:gg;") && server_chat.size() == 20);
        CHECK(has(client_chat, "team:go left;") && has(client_chat, "all:gg;") && client_chat.size() == 20);
        server.set_player_group(0, 2, 0);  // different teams now: team chat stays with its team
        pump(0.2f, [] {});
        client.send_chat(net::ChatChannel::Team, "secret");
        pump(0.3f, [] {});
        CHECK(server_chat.size() == 20 && !has(server_chat, "secret"));  // the server's player is on another team

        // Voice: an Opus round trip keeps the sound; team voice reaches team mates only.
        net::VoiceEncoder encoder;
        net::VoiceDecoder decoder;
        CHECK(encoder.ok() && decoder.ok());
        f32 tone[net::kVoiceFrame], heard[net::kVoiceFrame];
        u8 packet[net::kMaxVoicePacket];
        u32 bytes = 0;
        f64 energy_in = 0, energy_out = 0;
        for (int round = 0; round < 5; ++round) {  // a few frames: the codec settles in
            for (u32 i = 0; i < net::kVoiceFrame; ++i) tone[i] = 0.5f * std::sin(6.2831853f * 440.0f * static_cast<f32>(round * net::kVoiceFrame + i) / 48000.0f);
            bytes = encoder.encode(tone, packet, sizeof packet);
            CHECK(bytes > 0 && bytes < 200);  // ~24 kbit/s: about 60 bytes per 20 ms
            CHECK(decoder.decode(packet, bytes, heard));
        }
        for (u32 i = 0; i < net::kVoiceFrame; ++i) energy_in += tone[i] * tone[i], energy_out += heard[i] * heard[i];
        CHECK(energy_out > energy_in * 0.5 && energy_out < energy_in * 2.0);
        CHECK(decoder.decode(nullptr, 0, heard));  // a lost packet is concealed, not an error
        CHECK(client.send_voice(net::ChatChannel::Team, packet, bytes));
        CHECK(!client.send_voice(net::ChatChannel::All, packet, bytes));  // voice to everyone is off unless the game turns it on
        pump(0.3f, [] {});
        CHECK(server_voice == 0);  // the server's player is on team 2
        server.set_player_group(0, 1, 0);
        pump(0.2f, [] {});
        client.send_voice(net::ChatChannel::Team, packet, bytes);
        pump(0.3f, [] {});
        CHECK(server_voice == bytes);
        server_voice = 0;
        CHECK(server.send_voice(net::ChatChannel::Team, packet, bytes));  // the host talks: its client hears, the host never hears itself
        pump(0.3f, [] {});
        CHECK(server_voice == 0);

        client.shutdown();
        pump(0.3f, [] {});
        server.shutdown();
    }
    CHECK(heap.allocated_bytes() == 0);
    if (g_failures == 0) std::printf("All net tests passed.\n");
    return g_failures ? 1 : 0;
}
