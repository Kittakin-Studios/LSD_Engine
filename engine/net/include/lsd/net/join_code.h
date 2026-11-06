#pragma once
// Getting playtesters into a game without typing addresses or forwarding ports.
//
// A join code is a server's IPv4 address and port in 10 letters / digits ("K7QF2-MXA9P", Crockford
// base32: no I / L / O / U to misread, any case, dashes optional) with a 2-bit check. No server of
// ours is involved: the code is the address.
//
// PortMapper asks the router (UPnP) to forward the game's UDP port to this PC and tells its public
// address: that address's code works from the internet. Without UPnP, or behind an internet
// provider's shared address (CGNAT), use a local address's code over a virtual LAN (Tailscale /
// ZeroTier) instead.
#include "lsd/foundation/types.h"

#include <atomic>
#include <thread>

namespace lsd::net {

bool make_join_code(const char* ipv4, u16 port, char* out, usize capacity);
// A code, or an address typed as it is ("1.2.3.4" or "1.2.3.4:7777"; `port` when none), into
// "a.b.c.d:port" for NetSession::join. False when it is neither.
bool parse_join_code(const char* code, u16 port, char* address, usize capacity);

// This PC's IPv4 addresses (not 127.x), Tailscale / ZeroTier adapters included. Returns how many.
u32 local_addresses(char (*out)[16], u32 capacity);
// Private, CGNAT or link-local: not reachable from the internet.
bool is_private_address(const char* ipv4);

class PortMapper {
public:
    enum class State : u8 { Off, Working, Open, Failed };

    ~PortMapper() { stop(); }
    void start(u16 port);  // discovery takes a couple of seconds: on its own thread
    void stop();           // removes the forward (call when the server stops)
    State state() const { return state_.load(std::memory_order_acquire); }
    const char* external_address() const { return state() == State::Open ? external_ : ""; }
    const char* message() const { return state() == State::Working ? "asking the router" : message_; }  // why it failed

private:
    void run();
    std::thread thread_;
    std::atomic<State> state_{State::Off};
    std::atomic<bool> cancel_{false};
    u16 port_ = 0;
    char external_[16] = "";
    char message_[128] = "";
    char control_url_[256] = "", service_[128] = "";  // to remove the forward
};

} // namespace lsd::net
