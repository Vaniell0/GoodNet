#pragma once

/// @file core/data/machine_id.hpp
/// @brief Hardware-bound device identity derivation.
///
/// Hardware sources (priority order):
///   Linux:   /etc/machine-id → DMI product_uuid → CPU serial (cpuid) → MAC address
///   macOS:   IOPlatformUUID (IOKit) → MAC address
///   Windows: MachineGuid (registry) → SMBIOS UUID → MAC address
///   Fallback: generated random, persisted to <dir>/machine_id

#include <sodium.h>
#include <filesystem>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cstdio>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <iphlpapi.h>
    #pragma comment(lib, "iphlpapi.lib")
#elif defined(__APPLE__)
    #include <IOKit/IOKitLib.h>
    #include <net/if.h>
    #include <net/if_dl.h>
    #include <ifaddrs.h>
    #include <sys/sysctl.h>
#else
    #include <net/if.h>
    #include <sys/ioctl.h>
    #include <netinet/in.h>
    #include <unistd.h>
    #if defined(__x86_64__) || defined(__i386__)
        #include <cpuid.h>
    #endif
#endif

namespace gn {

namespace fs = std::filesystem;

/// @brief Hardware-bound machine identity provider.
class MachineId {
public:
    /// @brief Collect all available hardware identifiers and combine them.
    /// @return Hex string of BLAKE2b-256 over all found hardware IDs, or empty.
    static std::string read() {
        std::vector<std::string> sources;

#if defined(_WIN32)
        collect_windows(sources);
#elif defined(__APPLE__)
        collect_macos(sources);
#else
        collect_linux(sources);
#endif
        collect_mac_address(sources);

        if (sources.empty()) return {};
        return hash_sources(sources);
    }

    /// @brief Get or create persistent machine ID in <dir>/machine_id.
    static std::string get_or_create(const fs::path& dir) {
        auto id = read();
        if (!id.empty()) return id;

        const fs::path path = dir / "machine_id";
        if (fs::exists(path)) {
            if (auto cached = read_file(path); !cached.empty()) return cached;
        }

        uint8_t rand_bytes[32];
        randombytes_buf(rand_bytes, sizeof(rand_bytes));
        auto hex = to_hex(rand_bytes, sizeof(rand_bytes));

        fs::create_directories(dir);
        std::ofstream f(path);
        if (f) f << hex;
        return hex;
    }

    /// @brief Derive Ed25519 device keypair deterministically from machine_id + user_pubkey.
    /// @details seed = BLAKE2b-256(machine_id || user_pubkey)
    static void derive_device_keypair(
        std::string_view            machine_id,
        const uint8_t               user_pubkey[crypto_sign_PUBLICKEYBYTES],
        uint8_t                     out_pub[crypto_sign_PUBLICKEYBYTES],
        uint8_t                     out_sec[crypto_sign_SECRETKEYBYTES])
    {
        uint8_t seed[crypto_sign_SEEDBYTES];
        crypto_generichash_state state;
        crypto_generichash_init(&state, nullptr, 0, sizeof(seed));
        crypto_generichash_update(&state,
            reinterpret_cast<const uint8_t*>(machine_id.data()), machine_id.size());
        crypto_generichash_update(&state, user_pubkey, crypto_sign_PUBLICKEYBYTES);
        crypto_generichash_final(&state, seed, sizeof(seed));

        crypto_sign_seed_keypair(out_pub, out_sec, seed);
        sodium_memzero(seed, sizeof(seed));
    }

private:
    static std::string to_hex(const uint8_t* data, size_t len) {
        std::string out;
        out.reserve(len * 2);
        for (size_t i = 0; i < len; ++i) {
            char b[3];
            std::snprintf(b, sizeof(b), "%02x", data[i]);
            out += b;
        }
        return out;
    }

    static std::string read_file(const fs::path& path) {
        std::ifstream f(path);
        if (!f) return {};
        std::string line;
        std::getline(f, line);
        line.erase(std::remove_if(line.begin(), line.end(),
            [](char c){ return c == '-' || c == '\n' || c == '\r' || c == ' '; }),
            line.end());
        return line;
    }

    /// @brief Hash all collected sources into a single stable ID.
    static std::string hash_sources(const std::vector<std::string>& sources) {
        uint8_t out[32];
        crypto_generichash_state state;
        crypto_generichash_init(&state, nullptr, 0, sizeof(out));
        for (const auto& s : sources)
            crypto_generichash_update(&state,
                reinterpret_cast<const uint8_t*>(s.data()), s.size());
        crypto_generichash_final(&state, out, sizeof(out));
        return to_hex(out, sizeof(out));
    }

#if defined(_WIN32)
    static void collect_windows(std::vector<std::string>& out) {
        // MachineGuid from registry
        HKEY key;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
            char buf[256] = {};
            DWORD sz = sizeof(buf);
            if (RegQueryValueExA(key, "MachineGuid", nullptr, nullptr,
                    reinterpret_cast<LPBYTE>(buf), &sz) == ERROR_SUCCESS)
                out.emplace_back(buf);
            RegCloseKey(key);
        }

        // SMBIOS UUID via WMI (fallback: firmware table)
        DWORD needed = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
        if (needed > 0) {
            std::vector<uint8_t> buf(needed);
            if (GetSystemFirmwareTable('RSMB', 0, buf.data(), needed) > 0) {
                // SMBIOS type 1 (System Information) contains UUID at offset 8
                // Walk SMBIOS structures to find type 1
                const uint8_t* p = buf.data() + 8; // skip RawSMBIOSData header
                const uint8_t* end = buf.data() + needed;
                while (p + 4 < end) {
                    uint8_t type = p[0];
                    uint8_t len  = p[1];
                    if (type == 1 && len >= 25 && p + 25 <= end) {
                        out.push_back(to_hex(p + 8, 16)); // UUID at offset 8
                        break;
                    }
                    if (type == 127) break; // end-of-table
                    p += len;
                    // skip string section
                    while (p + 1 < end && !(p[0] == 0 && p[1] == 0)) ++p;
                    p += 2;
                }
            }
        }
    }

#elif defined(__APPLE__)
    static void collect_macos(std::vector<std::string>& out) {
        // IOPlatformUUID — unique per-hardware, survives OS reinstall
        io_service_t service = IOServiceGetMatchingService(
            kIOMainPortDefault, IOServiceMatching("IOPlatformExpertDevice"));
        if (service) {
            CFStringRef uuid = (CFStringRef)IORegistryEntryCreateCFProperty(
                service, CFSTR("IOPlatformUUID"), kCFAllocatorDefault, 0);
            if (uuid) {
                char buf[64] = {};
                CFStringGetCString(uuid, buf, sizeof(buf), kCFStringEncodingUTF8);
                out.emplace_back(buf);
                CFRelease(uuid);
            }
            IOObjectRelease(service);
        }

        // Hardware serial number
        io_service_t platform = IOServiceGetMatchingService(
            kIOMainPortDefault, IOServiceMatching("IOPlatformExpertDevice"));
        if (platform) {
            CFStringRef serial = (CFStringRef)IORegistryEntryCreateCFProperty(
                platform, CFSTR("IOPlatformSerialNumber"), kCFAllocatorDefault, 0);
            if (serial) {
                char buf[64] = {};
                CFStringGetCString(serial, buf, sizeof(buf), kCFStringEncodingUTF8);
                out.emplace_back(buf);
                CFRelease(serial);
            }
            IOObjectRelease(platform);
        }
    }

#else // Linux
    static void collect_linux(std::vector<std::string>& out) {
        // systemd / dbus machine-id
        for (const char* p : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
            auto id = read_file(p);
            if (!id.empty()) { out.push_back(id); break; }
        }

        // DMI product UUID (survives OS reinstall, tied to motherboard)
        auto dmi = read_file("/sys/class/dmi/id/product_uuid");
        if (!dmi.empty()) out.push_back(dmi);

        // DMI board serial
        auto board = read_file("/sys/class/dmi/id/board_serial");
        if (!board.empty() && board != "None" && board != "To Be Filled By O.E.M.")
            out.push_back(board);

        // CPU serial via CPUID (leaf 3, available on some Intel CPUs)
#if defined(__x86_64__) || defined(__i386__)
        collect_cpu_serial(out);
#endif
    }

#if defined(__x86_64__) || defined(__i386__)
    static void collect_cpu_serial(std::vector<std::string>& out) {
        unsigned int eax, ebx, ecx, edx;

        // Check if CPUID leaf 3 (PSN) is supported
        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
            // PSN bit: edx bit 18
            if (edx & (1u << 18)) {
                unsigned int hi, lo_hi, lo_lo;
                // Leaf 3: processor serial
                __cpuid(3, lo_lo, lo_hi, ecx, edx);
                hi = eax; // from leaf 1
                uint8_t serial[12];
                std::memcpy(serial,     &hi,     4);
                std::memcpy(serial + 4, &lo_hi,  4);
                std::memcpy(serial + 8, &lo_lo,  4);
                out.push_back(to_hex(serial, sizeof(serial)));
            }
        }
    }
#endif
#endif // Linux

    static void collect_mac_address(std::vector<std::string>& out) {
#if defined(_WIN32)
        ULONG sz = 0;
        GetAdaptersInfo(nullptr, &sz);
        std::vector<uint8_t> buf(sz);
        auto* info = reinterpret_cast<IP_ADAPTER_INFO*>(buf.data());
        if (GetAdaptersInfo(info, &sz) == NO_ERROR) {
            while (info) {
                if (info->AddressLength == 6) {
                    // Skip virtual/loopback: first byte odd = multicast
                    if (!(info->Address[0] & 0x02))
                        out.push_back(to_hex(info->Address, 6));
                }
                info = info->Next;
            }
        }
#elif defined(__APPLE__)
        struct ifaddrs* ifap = nullptr;
        if (getifaddrs(&ifap) == 0) {
            for (auto* p = ifap; p; p = p->ifa_next) {
                if (!p->ifa_addr || p->ifa_addr->sa_family != AF_LINK) continue;
                if (std::string_view(p->ifa_name).starts_with("lo")) continue;
                auto* sdl = reinterpret_cast<struct sockaddr_dl*>(p->ifa_addr);
                if (sdl->sdl_alen == 6) {
                    const uint8_t* mac = reinterpret_cast<const uint8_t*>(
                        LLADDR(sdl));
                    if (!(mac[0] & 0x02)) // skip locally-administered
                        out.push_back(to_hex(mac, 6));
                }
            }
            freeifaddrs(ifap);
        }
#else // Linux
        // Read MACs via sysfs — no root required
        const fs::path net_dir = "/sys/class/net";
        if (fs::exists(net_dir)) {
            for (const auto& entry : fs::directory_iterator(net_dir)) {
                const std::string iface = entry.path().filename().string();
                if (iface == "lo") continue;
                auto mac = read_file(entry.path() / "address");
                if (mac.empty() || mac == "000000000000") continue;
                // Skip virtual: check /sys/class/net/<iface>/device (physical only)
                if (fs::exists(entry.path() / "device"))
                    out.push_back(mac);
            }
        }
#endif
    }
};

} // namespace gn
