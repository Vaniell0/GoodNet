#include "machine_id.hpp"
#include "logger.hpp"
#include <sodium/crypto_sign.h>
#include <sodium/crypto_generichash.h>
#include <sodium/randombytes.h>
#include <sodium/utils.h>

#include <fstream>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cctype>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <iphlpapi.h>
#elif defined(__APPLE__)
    #include <IOKit/IOKitLib.h>
    #include <net/if.h>
    #include <ifaddrs.h>
    #include <sys/sysctl.h>
    #include <net/if_dl.h>
#else
    #include <net/if.h>
    #include <sys/ioctl.h>
    #include <unistd.h>
    #if defined(__x86_64__) || defined(__i386__)
        #include <cpuid.h>
    #endif
#endif

namespace gn {

namespace {
    std::string to_hex(const uint8_t* data, size_t len) {
        std::string out;
        out.reserve(len * 2);
        for (size_t i = 0; i < len; ++i) {
            char b[3];
            std::snprintf(b, sizeof(b), "%02x", data[i]);
            out += b;
        }
        return out;
    }

    std::string read_file(const fs::path& path) {
        std::ifstream f(path);
        if (!f) return {};
        std::string line;
        std::getline(f, line);
        line.erase(std::remove_if(line.begin(), line.end(), 
            [](char c){ return std::isspace(static_cast<unsigned char>(c)) || c == '-'; }), 
            line.end());
        return line;
    }

#if defined(_WIN32)
    void collect_platform_ids(std::vector<std::string>& out) {
        HKEY key;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
            char buf[256] = {};
            DWORD sz = sizeof(buf);
            if (RegQueryValueExA(key, "MachineGuid", nullptr, nullptr, reinterpret_cast<LPBYTE>(buf), &sz) == ERROR_SUCCESS)
                out.emplace_back(buf);
            RegCloseKey(key);
        }
        DWORD needed = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
        if (needed > 0) {
            std::vector<uint8_t> buf(needed);
            if (GetSystemFirmwareTable('RSMB', 0, buf.data(), needed) > 0) {
                const uint8_t* p = buf.data() + 8;
                const uint8_t* end = buf.data() + needed;
                while (p + 4 < end) {
                    if (p[0] == 1 && p[1] >= 25) { out.push_back(to_hex(p + 8, 16)); break; }
                    if (p[0] == 127) break;
                    p += p[1];
                    while (p + 1 < end && !(p[0] == 0 && p[1] == 0)) ++p;
                    p += 2;
                }
            }
        }
    }
#elif defined(__APPLE__)
    void collect_platform_ids(std::vector<std::string>& out) {
        io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPlatformExpertDevice"));
        if (svc) {
            auto get_prop = [&](const char* key) {
                CFStringRef cf = (CFStringRef)IORegistryEntryCreateCFProperty(svc, CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8), kCFAllocatorDefault, 0);
                if (cf) {
                    char buf[128] = {};
                    CFStringGetCString(cf, buf, sizeof(buf), kCFStringEncodingUTF8);
                    out.emplace_back(buf);
                    CFRelease(cf);
                }
            };
            get_prop("IOPlatformUUID");
            get_prop("IOPlatformSerialNumber");
            IOObjectRelease(svc);
        }
    }
#else
    void collect_platform_ids(std::vector<std::string>& out) {
        for (const char* p : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
            if (auto id = read_file(p); !id.empty()) {
                LOG_TRACE("machine_id: platform_id={}...", id.substr(0, 12));
                out.push_back(id); break;
            }
        }
        if (auto dmi = read_file("/sys/class/dmi/id/product_uuid"); !dmi.empty()) {
            LOG_TRACE("machine_id: dmi_uuid={}...", dmi.substr(0, 12));
            out.push_back(dmi);
        }
#if defined(__x86_64__) || defined(__i386__)
        unsigned int eax, ebx, ecx, edx;
        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) && (edx & (1u << 18))) {
            __cpuid(3, eax, ebx, ecx, edx);
            uint8_t ser[12]; std::memcpy(ser, &eax, 4); std::memcpy(ser+4, &ebx, 4); std::memcpy(ser+8, &ecx, 4);
            out.push_back(to_hex(ser, 12));
        }
#endif
    }
#endif

    void collect_mac_address(std::vector<std::string>& out) {
#if defined(_WIN32)
        ULONG sz = 0; GetAdaptersInfo(nullptr, &sz);
        std::vector<uint8_t> buf(sz);
        if (GetAdaptersInfo(reinterpret_cast<IP_ADAPTER_INFO*>(buf.data()), &sz) == NO_ERROR) {
            for (auto* i = reinterpret_cast<IP_ADAPTER_INFO*>(buf.data()); i; i = i->Next)
                if (i->AddressLength == 6 && !(i->Address[0] & 0x02)) out.push_back(to_hex(i->Address, 6));
        }
#elif defined(__APPLE__)
        struct ifaddrs* ifap;
        if (getifaddrs(&ifap) == 0) {
            for (auto* p = ifap; p; p = p->ifa_next) {
                if (p->ifa_addr && p->ifa_addr->sa_family == AF_LINK && !std::string_view(p->ifa_name).starts_with("lo")) {
                    auto* sdl = reinterpret_cast<struct sockaddr_dl*>(p->ifa_addr);
                    if (sdl->sdl_alen == 6 && !(reinterpret_cast<uint8_t*>(LLADDR(sdl))[0] & 0x02))
                        out.push_back(to_hex(reinterpret_cast<uint8_t*>(LLADDR(sdl)), 6));
                }
            }
            freeifaddrs(ifap);
        }
#else
        if (fs::exists("/sys/class/net")) {
            for (const auto& e : fs::directory_iterator("/sys/class/net")) {
                if (e.path().filename() == "lo") continue;
                auto mac = read_file(e.path() / "address");
                if (!mac.empty() && mac != "000000000000" && fs::exists(e.path() / "device")) {
                    LOG_TRACE("machine_id: MAC={}", mac);
                    out.push_back(mac);
                }
            }
        }
#endif
    }
}

std::string MachineId::read() {
    std::vector<std::string> sources;
    collect_platform_ids(sources);
    collect_mac_address(sources);
    LOG_TRACE("machine_id: {} sources collected", sources.size());
    if (sources.empty()) return {};

    uint8_t out[crypto_generichash_BYTES];
    crypto_generichash_state state;
    crypto_generichash_init(&state, nullptr, 0, sizeof(out));
    for (const auto& s : sources) crypto_generichash_update(&state, reinterpret_cast<const uint8_t*>(s.data()), s.size());
    crypto_generichash_final(&state, out, sizeof(out));
    return to_hex(out, sizeof(out));
}

std::string MachineId::get_or_create(const fs::path& dir) {
    auto id = read();
    if (!id.empty()) return id;
    const fs::path p = dir / "machine_id";
    if (fs::exists(p)) { if (auto c = read_file(p); !c.empty()) return c; }
    uint8_t rb[32]; randombytes_buf(rb, 32);
    auto h = to_hex(rb, 32);
    fs::create_directories(dir);
    std::ofstream f(p); if (f) f << h;
    return h;
}

void MachineId::derive_device_keypair(std::string_view mid, const uint8_t upk[crypto_sign_PUBLICKEYBYTES], uint8_t op[crypto_sign_PUBLICKEYBYTES], uint8_t os[crypto_sign_SECRETKEYBYTES]) {
    uint8_t seed[crypto_sign_SEEDBYTES];
    crypto_generichash_state state;
    crypto_generichash_init(&state, nullptr, 0, sizeof(seed));
    crypto_generichash_update(&state, reinterpret_cast<const uint8_t*>(mid.data()), mid.size());
    crypto_generichash_update(&state, upk, crypto_sign_PUBLICKEYBYTES);
    crypto_generichash_final(&state, seed, sizeof(seed));
    crypto_sign_seed_keypair(op, os, seed);
    sodium_memzero(seed, sizeof(seed));
}

} // namespace gn