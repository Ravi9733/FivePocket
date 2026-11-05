#pragma once
// bot-vision (2026-10-03): READ-ONLY scan of the game's live entity pools for the play bot (RENDER_DIAG_NEAR line, switch
// files/private-trace/diag-bot-vision, read once a second together with diag-telemetry). Every address and offset below was
// derived from our own recompiled code (validated-ppc asm comments), never from game source:
//   script handle -> entity: sub_82362DB8 (called by get_entity_coords 1647f1cb -> sub_82342D88): P = *(0x83C5D1F0) is a
//     fwBasePool {+0 data, +4 flag bytes, +8 size, +12 item size}; handle = index << 8 | generation byte; the slot is live when
//     flags[index] == generation; entity = *(data + index * itemSize + 4).
//   vehicles: get_closest_vehicle (d7e26b2c -> sub_82E0E510) walks P = **(0x83A5DCEC): {+0 items, +4 count, +12 stride,
//     +24 used-bit words}; slot i is used when bit (i & 31) of word i >> 5 is set; vehicle = *(items + i * stride).
//   peds: the per-frame entity update sub_823B6D60 (and get_closest_ped's spatial query, sub_82DCA708) use P = *(0x8394B8A8),
//     a fwBasePool of CPed (a slot is free when its flag byte has bit 7 set); its items carry the ped intelligence at +2908
//     (the same field get_ped_nearby_peds 4d3325f4 reads).
//   entity type byte +20: 3 = vehicle (is_entity_a_vehicle sub_82D71E50), 4 = ped (is_entity_a_ped sub_82D71D40).
//   position (sub_82461CA8, get_entity_coords): transform T = *(entity + 48); kind = u16 T+12 & ~2: 0 or 8 -> position at
//     T+16, 1 -> T+48. forward (sub_82353268, get_entity_forward_vector): kind 1 -> T+16; kind 0 -> (-T[0], T[4]).
//   ped in a vehicle (sub_82461CA8): (u32 ped+3444 & 0x40000000) and vehicle = *(ped + 3620) non-null.
//   driver (get_ped_in_vehicle_seat 388fde9a -> sub_82490098 -> sub_823D48A8): seat s at *(vehicle + 1664 + (s + 1) * 4); the
//     driver (s = -1) at vehicle + 1668.
// Only aligned 1/2/4-byte relaxed loads of guest memory; nothing is written and no guest code is called (no guest-visible
// effect). A torn read (entities move while the script thread scans) only gives a bad entry of one sample.
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace bot_vision {
constexpr std::uint32_t GuidPoolGlobal = 0x83C5D1F0u, VehiclePoolGlobal = 0x83A5DCECu, PedPoolGlobal = 0x8394B8A8u;
constexpr std::uint32_t EntType = 20, EntTransform = 48, PedFlags = 3444, PedInVehicleBit = 0x40000000u, PedVehicle = 3620,
    VehDriver = 1668;
constexpr std::uint8_t TypeVehicle = 3, TypePed = 4;
constexpr unsigned MaxSlots = 2048, MaxVeh = 16, MaxPed = 12;

inline bool addr(std::uint32_t a, std::uint32_t align) { return a >= 0x10000u && a <= 0xFFFF0000u && !(a & (align - 1)); }
inline bool ptr(std::uint32_t a) { return addr(a, 4); }

struct Mem {
    const std::uint8_t* base;
    std::uint32_t u32(std::uint32_t a) const {
        if (!addr(a, 4)) return 0;
        return __builtin_bswap32(__atomic_load_n(reinterpret_cast<const std::uint32_t*>(base + a), __ATOMIC_RELAXED));
    }
    std::uint16_t u16(std::uint32_t a) const {
        if (!addr(a, 2)) return 0;
        return __builtin_bswap16(__atomic_load_n(reinterpret_cast<const std::uint16_t*>(base + a), __ATOMIC_RELAXED));
    }
    std::uint8_t u8(std::uint32_t a) const { return addr(a, 1) ? __atomic_load_n(base + a, __ATOMIC_RELAXED) : 0; }
    float f32(std::uint32_t a) const { const auto w = u32(a); float v; std::memcpy(&v, &w, 4); return v; }
};

struct Pose { float x = 0, y = 0, z = 0, fx = 0, fy = 0; };

inline bool sane(float v) { return std::isfinite(v) && std::fabs(v) < 100000.f; }

inline bool pose(const Mem& m, std::uint32_t ent, Pose& p) {
    const auto t = m.u32(ent + EntTransform);
    if (!ptr(t)) return false;
#if defined(BOT_VISION_MUTANT_KIND) // test mutant: the kind's bit 1 is not masked
    const unsigned kind = m.u16(t + 12);
#else
    const unsigned kind = m.u16(t + 12) & ~2u;
#endif
    std::uint32_t at;
    p.fx = p.fy = 0;
    if (kind == 0 || kind == 8) {
        at = t + 16;
        if (kind == 0) { p.fx = -m.f32(t); p.fy = m.f32(t + 4); }
    } else if (kind == 1) {
        at = t + 48; p.fx = m.f32(t + 16); p.fy = m.f32(t + 20);
    } else {
        return false;
    }
    p.x = m.f32(at); p.y = m.f32(at + 4); p.z = m.f32(at + 8);
    if (!sane(p.fx) || !sane(p.fy) || std::fabs(p.fx) > 1.5f || std::fabs(p.fy) > 1.5f) p.fx = p.fy = 0;
    return sane(p.x) && sane(p.y) && sane(p.z);
}

inline std::uint32_t entityFromHandle(const Mem& m, std::uint32_t handle) {
    if (handle == 0xFFFFFFFFu || !handle) return 0;
    const auto pool = m.u32(GuidPoolGlobal);
    if (!ptr(pool)) return 0;
    const auto index = static_cast<std::uint32_t>(static_cast<std::int32_t>(handle) >> 8);
    if (index >= m.u32(pool + 8) || index >= 0x100000u) return 0;
    const auto flags = m.u32(pool + 4);
    if (!addr(flags, 1) || m.u8(flags + index) != (handle & 0xFFu)) return 0;
    const auto item = m.u32(pool) + m.u32(pool + 12) * index;
    return ptr(item) ? m.u32(item + 4) : 0;
}

struct Entry { std::uint32_t id = 0; float d2 = 0, dx = 0, dy = 0, dz = 0, fx = 0, fy = 0; std::uint8_t driver = 0; };

template <unsigned N> struct Nearest {
    Entry e[N]; unsigned n = 0;
    void add(const Entry& c) {
        if (n == N && c.d2 >= e[N - 1].d2) return;
        unsigned k = n < N ? n++ : N - 1;
        while (k > 0 && e[k - 1].d2 > c.d2) { e[k] = e[k - 1]; --k; }
        e[k] = c;
    }
};

struct Snapshot {
    bool player = false; char ref = '-';          // 'V' = the player's vehicle, 'P' = the player ped
    Pose me; std::uint32_t myPed = 0, myVeh = 0;
    unsigned vehUsed = 0, pedUsed = 0; bool vehPool = false, pedPool = false;
    Nearest<MaxVeh> veh; Nearest<MaxPed> ped;
};

// vehRadius / pedRadius: horizontal metres around the player (or its vehicle); |dz| > 10 m (overpasses, tunnels) is left out.
inline void scan(const std::uint8_t* base, std::uint32_t playerHandle, float vehRadius, float pedRadius, Snapshot& s) {
    const Mem m{base};
    s = Snapshot{};
    const auto me = entityFromHandle(m, playerHandle);
    if (!ptr(me) || m.u8(me + EntType) != TypePed) return;
    s.myPed = me;
    const auto v = m.u32(me + PedVehicle);
    if ((m.u32(me + PedFlags) & PedInVehicleBit) && ptr(v)) s.myVeh = v;
    if (!pose(m, s.myVeh ? s.myVeh : me, s.me)) { s.myVeh = 0; if (!pose(m, me, s.me)) return; }
    s.player = true; s.ref = s.myVeh ? 'V' : 'P';
    const float vr2 = vehRadius * vehRadius, pr2 = pedRadius * pedRadius;
    const auto consider = [&](std::uint32_t ent, Entry& c, float r2) {
        Pose p;
        if (!pose(m, ent, p)) return false;
        c.id = ent; c.dx = p.x - s.me.x; c.dy = p.y - s.me.y; c.dz = p.z - s.me.z; c.d2 = c.dx * c.dx + c.dy * c.dy;
        c.fx = p.fx; c.fy = p.fy;
        return c.d2 <= r2 && std::fabs(c.dz) <= 10.f;
    };
    // vehicles
    const auto vg = m.u32(VehiclePoolGlobal);
    const auto vp = ptr(vg) ? m.u32(vg) : 0u;
    if (ptr(vp)) {
        const auto items = m.u32(vp), count = m.u32(vp + 4), stride = m.u32(vp + 12), bits = m.u32(vp + 24);
        if (ptr(items) && ptr(bits) && count <= MaxSlots && stride >= 4 && stride <= 4096 && !(stride & 3)) {
            s.vehPool = true;
            std::uint32_t word = 0;
            for (std::uint32_t i = 0; i < count; ++i) {
                if (!(i & 31)) word = m.u32(bits + (i >> 5) * 4);
#if defined(BOT_VISION_MUTANT_BITS) // test mutant: IBM bit numbering (bit 0 = the most significant)
                if (!((word >> (31 - (i & 31))) & 1u)) continue;
#else
                if (!((word >> (i & 31)) & 1u)) continue;
#endif
                const auto ent = m.u32(items + stride * i);
                if (!ptr(ent) || m.u8(ent + EntType) != TypeVehicle) continue;
                ++s.vehUsed;
                if (ent == s.myVeh) continue;
                Entry c;
                if (!consider(ent, c, vr2)) continue;
                c.driver = ptr(m.u32(ent + VehDriver)) ? 1 : 0;
                s.veh.add(c);
            }
        }
    }
    // peds (on foot only: a ped in a vehicle is that vehicle's entry)
    const auto pp = m.u32(PedPoolGlobal);
    if (ptr(pp)) {
        const auto data = m.u32(pp), flags = m.u32(pp + 4), size = m.u32(pp + 8), item = m.u32(pp + 12);
        if (ptr(data) && addr(flags, 1) && size <= MaxSlots && item >= 64 && item <= 0x10000u && !(item & 3)) {
            s.pedPool = true;
            for (std::uint32_t i = 0; i < size; ++i) {
                if (m.u8(flags + i) & 0x80u) continue;
                const auto ent = data + item * i;
                if (!ptr(ent) || m.u8(ent + EntType) != TypePed) continue;
                ++s.pedUsed;
                if (ent == s.myPed) continue;
#if !defined(BOT_VISION_MUTANT_INVEHICLE) // test mutant: peds inside vehicles are listed as pedestrians
                if ((m.u32(ent + PedFlags) & PedInVehicleBit) && ptr(m.u32(ent + PedVehicle))) continue;
#endif
                Entry c;
                if (consider(ent, c, pr2)) s.ped.add(c);
            }
        }
    }
#if defined(BOT_VISION_MUTANT_WRITE) // test mutant: a guest-visible write (must be caught by the read-only check)
    const_cast<std::uint8_t*>(base)[0x82000000u] ^= 1;
#endif
}

inline int clampi(float v, int lim) { const long x = std::lround(v); return x < -lim ? -lim : x > lim ? lim : static_cast<int>(x); }

// RENDER_DIAG_NEAR ms=<ms> us=<scan us> me=<x>,<y>,<z>:<fx*1000>,<fy*1000>:<V|P> pools=<used vehicles>,<used peds>[!v][!p]
//   veh=<id>:<dx>,<dy>,<dz>:<fx*100>,<fy*100>:<driver 0|1>;...  ped=<id>:<dx>,<dy>,<dz>;...
// (dx/dy/dz relative to me, metres; nearest first; '!v' / '!p' = that pool did not validate; me=- = no player entity)
struct Text {
    char* out; std::size_t cap, n = 0;
    __attribute__((format(printf, 2, 3))) void put(const char* fmt, ...) {
        if (n + 1 >= cap) return;
        va_list ap; va_start(ap, fmt);
        const int k = std::vsnprintf(out + n, cap - n, fmt, ap);
        va_end(ap);
        if (k > 0) n = (n + static_cast<std::size_t>(k) < cap) ? n + static_cast<std::size_t>(k) : cap - 1;
    }
};

inline int format(const Snapshot& s, long long ms, unsigned us, char* out, std::size_t cap) {
    if (!cap) return 0;
    out[0] = 0;
    Text t{out, cap};
    t.put("RENDER_DIAG_NEAR ms=%lld us=%u ", ms, us);
    if (!s.player) { t.put("me=- pools=%u,%u veh= ped=", s.vehUsed, s.pedUsed); return static_cast<int>(t.n); }
    t.put("me=%.2f,%.2f,%.2f:%d,%d:%c pools=%u,%u%s%s veh=", s.me.x, s.me.y, s.me.z, clampi(s.me.fx * 1000.f, 1500),
          clampi(s.me.fy * 1000.f, 1500), s.ref, s.vehUsed, s.pedUsed, s.vehPool ? "" : "!v", s.pedPool ? "" : "!p");
    for (unsigned k = 0; k < s.veh.n; ++k) {
        const auto& e = s.veh.e[k];
        t.put("%08x:%.1f,%.1f,%.1f:%d,%d:%u;", e.id, e.dx, e.dy, e.dz, clampi(e.fx * 100.f, 150), clampi(e.fy * 100.f, 150),
              unsigned{e.driver});
    }
    t.put(" ped=");
    for (unsigned k = 0; k < s.ped.n; ++k) {
        const auto& e = s.ped.e[k];
        t.put("%08x:%.1f,%.1f,%.1f;", e.id, e.dx, e.dy, e.dz);
    }
    return static_cast<int>(t.n);
}
} // namespace bot_vision
