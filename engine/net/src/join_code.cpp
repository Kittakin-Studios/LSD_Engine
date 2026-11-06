#include "lsd/net/join_code.h"

#include "lsd/foundation/log.h"

#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#endif

#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>

namespace lsd::net {

namespace {

constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";  // Crockford base32

int symbol_value(char c) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    for (int i = 0; i < 32; ++i) {
        if (kAlphabet[i] == c) return i;
    }
    return -1;
}

bool parse_ipv4(const char* text, u8 out[4], const char** end) {
    unsigned a, b, c, d;
    int used = 0;
    if (std::sscanf(text, "%u.%u.%u.%u%n", &a, &b, &c, &d, &used) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
    out[0] = static_cast<u8>(a), out[1] = static_cast<u8>(b), out[2] = static_cast<u8>(c), out[3] = static_cast<u8>(d);
    if (end) *end = text + used;
    return true;
}

u64 check_bits(const u8 bytes[6]) {
    u32 sum = 0;
    for (int i = 0; i < 6; ++i) sum += bytes[i];
    return sum & 3u;
}

} // namespace

bool make_join_code(const char* ipv4, u16 port, char* out, usize capacity) {
    u8 b[6];
    if (capacity < 12 || !parse_ipv4(ipv4, b, nullptr)) return false;
    b[4] = static_cast<u8>(port >> 8), b[5] = static_cast<u8>(port & 0xFF);
    u64 v = check_bits(b) << 48;
    for (int i = 0; i < 6; ++i) v |= static_cast<u64>(b[i]) << (40 - 8 * i);
    usize n = 0;
    for (int i = 9; i >= 0; --i) {  // 10 symbols of 5 bits, most significant first
        out[n++] = kAlphabet[(v >> (5 * i)) & 31u];
        if (i == 5) out[n++] = '-';
    }
    out[n] = '\0';
    return true;
}

bool parse_join_code(const char* code, u16 port, char* address, usize capacity) {
    while (*code == ' ') ++code;
    u8 ip[4];
    const char* rest = nullptr;
    if (parse_ipv4(code, ip, &rest)) {  // typed as an address
        unsigned p = port;
        if (*rest == ':' && (std::sscanf(rest + 1, "%u", &p) != 1 || p == 0 || p > 65535)) return false;
        std::snprintf(address, capacity, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], p);
        return true;
    }
    u64 v = 0;
    int symbols = 0;
    for (const char* c = code; *c; ++c) {
        if (*c == '-' || *c == ' ') continue;
        const int s = symbol_value(*c);
        if (s < 0 || ++symbols > 10) return false;
        v = (v << 5) | static_cast<u64>(s);
    }
    if (symbols != 10) return false;
    u8 b[6];
    for (int i = 0; i < 6; ++i) b[i] = static_cast<u8>(v >> (40 - 8 * i));
    if ((v >> 48) != check_bits(b)) return false;  // a typo
    const unsigned p = (static_cast<unsigned>(b[4]) << 8) | b[5];
    if (p == 0) return false;
    std::snprintf(address, capacity, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], p);
    return true;
}

bool is_private_address(const char* ipv4) {
    u8 a[4];
    if (!parse_ipv4(ipv4, a, nullptr)) return true;
    return a[0] == 0 || a[0] == 10 || a[0] == 127 || (a[0] == 172 && (a[1] & 0xF0) == 16) || (a[0] == 192 && a[1] == 168) ||
           (a[0] == 100 && (a[1] & 0xC0) == 64) || (a[0] == 169 && a[1] == 254);
}

u32 local_addresses(char (*out)[16], u32 capacity) {
    u32 n = 0;
    auto add = [&](const in_addr& addr) {
        char text[16];
        if (n >= capacity || !inet_ntop(AF_INET, &addr, text, sizeof text) || std::strncmp(text, "127.", 4) == 0 || std::strncmp(text, "169.254.", 8) == 0) return;
        std::snprintf(out[n++], 16, "%s", text);
    };
#ifdef _WIN32
    ULONG size = 16 * 1024;
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto* list = static_cast<IP_ADAPTER_ADDRESSES*>(std::malloc(size));
        if (!list) return n;
        const ULONG r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size);
        if (r == NO_ERROR) {
            for (const IP_ADAPTER_ADDRESSES* a = list; a; a = a->Next) {
                if (a->OperStatus != IfOperStatusUp) continue;
                for (const IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
                    if (u->Address.lpSockaddr->sa_family == AF_INET) add(reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr)->sin_addr);
                }
            }
        }
        std::free(list);
        if (r != ERROR_BUFFER_OVERFLOW) break;  // else `size` now holds what is needed
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return 0;
    for (const ifaddrs* a = list; a; a = a->ifa_next) {
        if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET) add(reinterpret_cast<const sockaddr_in*>(a->ifa_addr)->sin_addr);
    }
    freeifaddrs(list);
#endif
    return n;
}

void PortMapper::start(u16 port) {
    if (state() == State::Working || state() == State::Open) return;
    stop();
    port_ = port;
    cancel_ = false;
    state_.store(State::Working, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
}

void PortMapper::run() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    auto fail = [this](const char* why) {
        std::snprintf(message_, sizeof message_, "%s", why);
        LSD_LOG_WARN("net", "UPnP: %s", why);
        state_.store(State::Failed, std::memory_order_release);
    };
    int error = 0;
    UPNPDev* devices = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
    if (!devices) {
        fail("no router answered (UPnP may be off in its settings)");
    } else {
        UPNPUrls urls{};
        IGDdatas data{};
        char lan[64] = "", wan[64] = "";
        const int igd = UPNP_GetValidIGD(devices, &urls, &data, lan, sizeof lan, wan, sizeof wan);
        freeUPNPDevlist(devices);
        char external[40] = "";
        char port[8];
        std::snprintf(port, sizeof port, "%u", port_);
        if (igd != 1 && igd != 2) {
            fail("no internet router found among the UPnP devices");
        } else if (cancel_) {
            fail("cancelled");
        } else if (UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, external) != UPNPCOMMAND_SUCCESS || !external[0]) {
            fail("the router did not tell its internet address");
        } else if (is_private_address(external)) {
            char why[128];
            std::snprintf(why, sizeof why, "the router's internet address %s is shared (CGNAT or a second router): use a virtual LAN code", external);
            fail(why);
        } else {
            int r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, port, port, lan, "LSD_Engine game", "UDP", nullptr, "7200");
            if (r == 725) r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, port, port, lan, "LSD_Engine game", "UDP", nullptr, "0");  // permanent leases only
            if (r != UPNPCOMMAND_SUCCESS) {
                char why[128];
                std::snprintf(why, sizeof why, "the router refused to forward port %s: %s", port, strupnperror(r));
                fail(why);
            } else {
                std::snprintf(external_, sizeof external_, "%s", external);
                std::snprintf(control_url_, sizeof control_url_, "%s", urls.controlURL);
                std::snprintf(service_, sizeof service_, "%s", data.first.servicetype);
                LSD_LOG_INFO("net", "UPnP: port %s forwarded to %s; internet address %s", port, lan, external);
                state_.store(State::Open, std::memory_order_release);
            }
        }
        if (igd) FreeUPNPUrls(&urls);
    }
#ifdef _WIN32
    WSACleanup();
#endif
}

void PortMapper::stop() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
    if (state() == State::Open) {
        char port[8];
        std::snprintf(port, sizeof port, "%u", port_);
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        UPNP_DeletePortMapping(control_url_, service_, port, "UDP", nullptr);
#ifdef _WIN32
        WSACleanup();
#endif
        LSD_LOG_INFO("net", "UPnP: port %s closed again", port);
    }
    state_.store(State::Off, std::memory_order_release);
}

} // namespace lsd::net
