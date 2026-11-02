// Multiplayer tests: a server and a client in one process over loopback, each with its own world.
#include "lsd/net/net.h"
#include "lsd/world/components.h"

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

int main() {
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

        client.shutdown();
        pump(0.3f, [] {});
        server.shutdown();
    }
    CHECK(heap.allocated_bytes() == 0);
    if (g_failures == 0) std::printf("All net tests passed.\n");
    return g_failures ? 1 : 0;
}
