// NATIVE SHADOW-VOLUME BUILDER (2026-10-01). Hand translation of 822447B0, the stencil shadow
// volume builder of characters/vehicles (~9 % of all CPU at 4 threads with Shadows High; runs in
// the render-list prep jobs, which the render thread joins).
// Input P (r3): +0 object (+0 u16 flags, +8 index info: +0 index buffer, +8 index count),
// +4 -> +8 vertex info (+0 u16 size class, +12 u16 vertex count), +8 face plane table (12 bytes
// per face) or 0, +12 vertex base, +16 stride, +20 light position, +32 -> +8 mask, +36 caps flag,
// +40 u16 strip count, +44 strips (8 bytes: +0 start, +2 count, +6 first plane), +48 u16 output
// indices, +0x10030 output index count.
// 1) per strip triangle: lit (1) / unlit (2) from the plane table or cross product vs the light;
//    flags per triangle and per vertex (or-ed). 2) edges of lit triangles touching both kinds go
//    into an open-addressing edge table (hash v*19), an edge meeting its reverse cancels it.
// 3) remaining edges chained into strips of side quads (v, v + vertex count). 4) caps (+36).
// Same arithmetic as the recompiled code (vector ops via the same SSE sequences, MXCSR flush mode
// switched at the same points), same guest writes (output, counters, the strip descriptor the
// game leaves on this frame when +44 is 0); per-call scratch (edge table) in host memory; the
// flag arrays stay where the original puts them on the guest stack.
// Switches: dist\native_shadow_off = original; dist\native_shadow_verify = on 1 call in 32 per
// thread run the original, then the native version on the same input, compare the output
// region, keep the original's result; log "NATIVE SHADOW VERIFY".
#include <immintrin.h>
#include <rex/ppc/intrinsics.h>
namespace native_shadow {
using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t; using s16 = int16_t; using s32 = int32_t; using s64 = int64_t;
inline u8* G(u8* b, u32 a) { return b + a + REX_PHYS_HOST_OFFSET(a); }
inline u32 L32(u8* b, u32 a) { u32 v; std::memcpy(&v, G(b, a), 4); return __builtin_bswap32(v); }
inline u16 L16(u8* b, u32 a) { u16 v; std::memcpy(&v, G(b, a), 2); return __builtin_bswap16(v); }
inline u64 L64(u8* b, u32 a) { u64 v; std::memcpy(&v, G(b, a), 8); return __builtin_bswap64(v); }
inline void S16(u8* b, u32 a, u32 v) { u16 x = __builtin_bswap16(u16(v)); std::memcpy(G(b, a), &x, 2); }
inline void S32(u8* b, u32 a, u32 v) { v = __builtin_bswap32(v); std::memcpy(G(b, a), &v, 4); }
inline void S64(u8* b, u32 a, u64 v) { v = __builtin_bswap64(v); std::memcpy(G(b, a), &v, 8); }
inline double LFd(u8* b, u32 a) { const u32 v = L32(b, a); float f; std::memcpy(&f, &v, 4); return double(f); }
inline __m128i MaskL(u32 lo) { return _mm_load_si128(reinterpret_cast<const __m128i*>(&rex::ppc::VectorMaskL[lo * 16])); }
inline __m128i MaskR(u32 lo) { return _mm_load_si128(reinterpret_cast<const __m128i*>(&rex::ppc::VectorMaskR[lo * 16])); }
// lvlx / lvrx on guest memory (as the recompiler emits them).
inline __m128 Lvlx(u8* b, u32 a) {
    return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_load_si128(reinterpret_cast<const __m128i*>(G(b, a & ~15u))), MaskL(a & 15)));
}
inline __m128 Lvrx(u8* b, u32 a) {
    if (!(a & 15)) return _mm_setzero_ps();
    return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_load_si128(reinterpret_cast<const __m128i*>(G(b, a & ~15u))), MaskR(a & 15)));
}
#define NS_PERM(v, imm) _mm_shuffle_ps((v), (v), (imm))
struct Edge { u16 a, b; };
thread_local Edge tab[8192];

// Probe from table[hash(p)] for an entry {a == p, b == q} (signed 16-bit compares, as lhz+extsh vs
// the zero-extended index). Returns the slot or -1.
inline s32 Find(u32 mask, s32 p, s32 q) {
    const u32 i0 = u32(p * 19) & mask;
    u32 i = i0;
    if (s16(tab[i].a) == -1) return -1;
    for (;;) {
        if (s32(s16(tab[i].a)) == p && s32(s16(tab[i].b)) == q) return s32(i);
        i = (i + 1) & mask;
        if (i == i0) return -1;
        if (s16(tab[i].a) == -1) return -1;
    }
}
// First free (0xFFFF) or deleted (0xFFFE) slot from hash(k), or -1 when the table is full.
inline s32 FreeSlot(u32 mask, s32 k) {
    const u32 i0 = u32(k * 19) & mask;
    u32 i = i0;
    if (tab[i].a == 0xFFFF) return s32(i);
    for (;;) {
        if (tab[i].a == 0xFFFE) return s32(i);
        i = (i + 1) & mask;
        if (i == i0) return -1;
        if (tab[i].a == 0xFFFF) return s32(i);
    }
}
// Edge toggle: entry {p, q} present -> deleted; else {q, p} stored at hash(q).
inline void Toggle(u32 mask, s32 p, s32 q) {
    const s32 f = Find(mask, p, q);
    if (f >= 0) { tab[f].a = 0xFFFE; return; }
    const s32 s = FreeSlot(mask, q);
    if (s >= 0) { tab[s].a = u16(q); tab[s].b = u16(p); }
}
// Chain step: entry with a == k (signed compare); returns its b sign-extended and deletes it, or -1.
inline s32 Next(u32 mask, s32 k) {
    const u32 i0 = u32(k * 19) & mask;
    u32 i = i0;
    if (tab[i].a == 0xFFFF) return -1;
    for (;;) {
        if (s32(s16(tab[i].a)) == k) { const s32 r = s16(tab[i].b); tab[i].a = 0xFFFE; return r; }
        i = (i + 1) & mask;
        if (i == i0) return -1;
        if (tab[i].a == 0xFFFF) return -1;
    }
}

std::atomic<uint64_t> slow_tests{0}, unaligned{0}, c_planes{0}, c_caps{0}, c_ovf{0}, c_runs{0};
// Phase stats (verify mode only): ticks per phase, sizes.
std::atomic<uint64_t> ph[6], st_size{0}, st_nv{0}, st_ni{0}, st_out{0}, st_n{0};
thread_local bool timing = false;
// false = not handled (frame not 16-byte aligned; the caller runs the original).
bool Run(PPCContext& ctx, u8* base, bool count) {
    u8* const b = base;
    const u32 P = ctx.r3.u32;
    const u32 r31 = ctx.r1.u32 - 33168u;
    if (r31 & 15) { unaligned.fetch_add(1, std::memory_order_relaxed); return false; }
    // The face-plane-table variant (P+8 != 0) never occurred in testing: left to the original.
    if (L32(b, P + 8) != 0) { c_planes.fetch_add(1, std::memory_order_relaxed); return false; }
    {   // the lit/unlit threshold constant (0): a denormal would need the original's flush-mode switching
        const u32 w = L32(b, 0x820875ECu);
        if ((w & 0x7F800000u) == 0 && (w & 0x007FFFFFu) != 0) return false;
    }
    const u32 obj = L32(b, P + 0);
    const u32 vinfo = L32(b, L32(b, P + 4) + 8);
    if (((L16(b, obj) & 0x0C00u) & L32(b, L32(b, P + 32) + 8)) == 0) S32(b, P + 0x10030, 0);
    // Strip descriptor on this frame (+136) and the 8 bytes at +160 (u16 1, 2 bytes as they are,
    // pointer to +136): copied to P+40 when the object has no strip list.
    S16(b, r31 + 136, 0); S16(b, r31 + 138, 0); S16(b, r31 + 140, 0); S16(b, r31 + 142, 0);
    S16(b, r31 + 160, 1); S32(b, r31 + 164, r31 + 136);
    if (L32(b, P + 44) == 0) {
        const u32 ix = L32(b, obj + 8);
        S64(b, P + 40, L64(b, r31 + 160));
        S16(b, r31 + 138, L16(b, ix + 8));
    }
    // Edge table size by the size class; per-size counters.
    const u32 cls = L16(b, vinfo + 0);
    u32 size; u32 ctr;
    if (cls < 512) { size = 512; ctr = 0x8370DC70; }
    else if (cls < 1024) { size = 1024; ctr = 0x8370DC74; }
    else if (cls < 2048) { size = 2048; ctr = 0x8370DC78; }
    else if (cls < 4096) { size = 4096; ctr = 0x8370DC7C; }
    else { size = 8192; ctr = 0x8370DC80; }
    if (count) S32(b, ctr, L32(b, ctr) + 1);
    const u32 mask = size - 1;
    const uint64_t T0 = timing ? __rdtsc() : 0;
    std::memset(tab, 0xFF, size * sizeof(Edge));
    const u32 ix = L32(b, obj + 8);
    const u32 nv = L16(b, vinfo + 12);  // r16
    const u32 ib = L32(b, ix + 0);      // r15
    const u32 ni = L16(b, ix + 8);
    // The two alloca'd flag arrays, at the original's guest addresses.
    const u32 r1a = r31 + ((0u - nv) & 0xFFFFFFF0u);
    const u32 vfa = r1a + 80;
    const u32 trf = r1a + ((0u - ni) & 0xFFFFFFF0u) + 80;
    u8* const vf = G(b, vfa);
    u8* const tri = G(b, trf);
    std::memset(vf, 0, nv);
    tri[0] = 0; tri[1] = 0;
    auto IDX = [&](s32 i) -> u32 { return L16(b, ib + u32(i) * 2u); };

    const uint64_t T1 = timing ? __rdtsc() : 0;
    // ---- 1) lit / unlit per triangle ----
    {
        const u32 nstrips = L16(b, P + 40);
        if (nstrips) {
            ctx.fpscr.disableFlushMode();
            const double f0 = LFd(b, 0x820875ECu);
            const float f0f = float(f0);
            bool flush_on = false;
            const u32 vbase = L32(b, P + 12);
            const s32 stride = s32(L32(b, P + 16));
            for (u32 s = 0; s < L16(b, P + 40); ++s) {
                const u32 S = L32(b, P + 44) + s * 8;
                u8 wind = 1;
                const s32 start = s16(L16(b, S + 0));
                s32 plane = s16(L16(b, S + 6));
                s32 i = start + 2;
                s32 end = s32(s16(L16(b, S + 2))) + start;
                u32 r7 = IDX(start), r8 = IDX(start + 1);
                while (i < end) {
                    const u32 r3 = IDX(i);
                    if (r7 == r8 || r8 == r3 || r7 == r3) {
                        tri[i] = 0;
                    } else {
                        // Planes == 0 here (the plane-table variant goes to the original).
                        // Lanes as the recompiled lvlx/vrlimi sequence leaves them (16-byte aligned
                        // frame): light-to-vertex vector (x, y, z, f0) in lanes 3..0; vertices
                        // (x, y, z, -) - lane 0 never reaches the result
                        // (rex::ppc::simde_mm_vmsum3fp128 takes guest x, y, z).
                        const u32 v3 = u32(s64(stride) * s64(s32(r3))) + vbase;
                        const u32 a7 = u32(s64(stride) * s64(s32(r7))) + vbase;
                        const u32 a8 = u32(s64(stride) * s64(s32(r8))) + vbase;
                        const u32 lxb = L32(b, P + 20), lyb = L32(b, P + 24), lzb = L32(b, P + 28);
                        const u32 vxb = L32(b, v3 + 0), vyb = L32(b, v3 + 4), vzb = L32(b, v3 + 8);
                        auto F = [](u32 w) { float f; std::memcpy(&f, &w, 4); return double(f); };
                        auto Den = [](u32 w) { return (w & 0x7F800000u) == 0 && (w & 0x007FFFFFu) != 0; };
                        auto Tiny = [](double d) { return d != 0.0 && std::fabs(d) < 1.1754943508222875e-38; };
                        double dx, dy, dz;
                        if (!flush_on) {
                            // First lit/unlit test of this call: the original's exact sequence.
                            ctx.fpscr.disableFlushMode();
                            dx = F(lxb) - F(vxb); dy = F(lyb) - F(vyb); dz = F(lzb) - F(vzb);
                            ctx.fpscr.enableFlushModeUnconditional();
                            flush_on = true;
                        } else {
                            // Flush mode stays on between tests: same results unless a denormal is
                            // involved (then this test runs with flush mode off, as the original).
                            dx = F(lxb) - F(vxb); dy = F(lyb) - F(vyb); dz = F(lzb) - F(vzb);
                            if (Den(lxb) | Den(lyb) | Den(lzb) | Den(vxb) | Den(vyb) | Den(vzb) | Tiny(dx) | Tiny(dy) | Tiny(dz)) {
                                ctx.fpscr.disableFlushModeUnconditional();
                                dx = F(lxb) - F(vxb); dy = F(lyb) - F(vyb); dz = F(lzb) - F(vzb);
                                ctx.fpscr.enableFlushModeUnconditional();
                                ++slow_tests;
                            }
                        }
                        const __m128 v11 = _mm_set_ps(float(dx), float(dy), float(dz), f0f);
                        const __m128 p7 = _mm_set_ps(float(F(L32(b, a7))), float(F(L32(b, a7 + 4))), float(F(L32(b, a7 + 8))), 0.0f);
                        const __m128 p8 = _mm_set_ps(float(F(L32(b, a8))), float(F(L32(b, a8 + 4))), float(F(L32(b, a8 + 8))), 0.0f);
                        const __m128 p3 = _mm_set_ps(float(F(vxb)), float(F(vyb)), float(F(vzb)), 0.0f);
                        const __m128 e1 = _mm_sub_ps(p8, p7);
                        const __m128 e2 = _mm_sub_ps(p3, p8);
                        const __m128 a = _mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(e1), 0x9C));
                        const __m128 c2 = _mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(e2), 0x78));
                        const __m128 d2 = _mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(e2), 0x9C));
                        const __m128 c1 = _mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(e1), 0x78));
                        __m128 nrm = _mm_mul_ps(a, c2);
                        nrm = _mm_xor_ps(_mm_sub_ps(_mm_mul_ps(c1, d2), nrm), _mm_castsi128_ps(_mm_set1_epi32(int(0x80000000))));
                        const double d = double(_mm_cvtss_f32(rex::ppc::simde_mm_vmsum3fp128(v11, nrm)));
                        u32 c;
                        if (wind != 0) c = (d < f0) ? 2 : 1;
                        else c = !(d < f0) ? 2 : 1;
                        ++plane;
                        tri[i] = u8(c);
                        vf[r7] |= u8(c);
                        vf[r8] |= u8(c);
                        vf[r3] |= u8(c);
                    }
                    ++i;
                    wind = wind == 0 ? 1 : 0;
                    r7 = r8; r8 = r3;
                    end = s32(s16(L16(b, S + 0))) + s32(s16(L16(b, S + 2)));
                }
            }
            if (flush_on) ctx.fpscr.disableFlushModeUnconditional();  // as after the original's last test
        }
    }
    const uint64_t T2 = timing ? __rdtsc() : 0;
    // ---- 2) silhouette edges ----
    {
        const u32 nstrips = L16(b, P + 40);
        const u32 strips = L32(b, P + 44);
        for (u32 s = 0; s < nstrips; ++s) {
            const u32 S = strips + s * 8;
            const s32 start = s16(L16(b, S + 0));
            const s32 end = s32(s16(L16(b, S + 2))) + start;
            s32 i = start + 2;
            s32 r3 = s32(IDX(start)), r30 = s32(IDX(start + 1));
            u8 w = 0;
            while (i < end) {
                const s32 r29 = s32(IDX(i));
                if (tri[i] == 1) {
                    const u32 f6 = vf[r30], f5 = vf[r3], f4 = vf[r29];
                    if ((f5 | f6 | f4) == 3) {
                        if (w != 0) {
                            if ((f5 & f6) == 3) Toggle(mask, r3, r30);
                            if ((f6 & f4) == 3) Toggle(mask, r30, r29);
                            if ((f5 & f4) == 3) Toggle(mask, r29, r3);
                        } else {
                            if ((f5 & f6) == 3) Toggle(mask, r30, r3);
                            if ((f6 & f4) == 3) Toggle(mask, r29, r30);
                            if ((f5 & f4) == 3) Toggle(mask, r3, r29);
                        }
                    }
                }
                w = u8(~w);
                ++i;
                r3 = r30; r30 = r29;
            }
        }
    }
    const uint64_t T3 = timing ? __rdtsc() : 0;
    // ---- 3) side quads ----
    const u32 out = P + 48;
    auto O = [&](s32 k) -> u32 { return out + u32(k) * 2u; };
    s32 n = 0;
    for (u32 k = 0; k < size; ++k) {
        const u16 a = tab[k].a;
        if (s16(a) < 0) continue;
        if (n != 0) { S16(b, O(n), L16(b, O(n - 1))); S16(b, O(n + 1), a); n += 2; }
        const u16 bb = tab[k].b;
        tab[k].a = 0xFFFE;
        S16(b, O(n + 2), bb);
        S16(b, O(n), a);
        S16(b, O(n + 1), u32(s32(s16(a)) + s32(nv)));
        S16(b, O(n + 3), u32(s32(s16(bb)) + s32(nv)));
        n += 4;
        s32 v = Next(mask, s16(bb));
        while (v >= 0) {
            S16(b, O(n), u32(v));
            S16(b, O(n + 1), u32(v + s32(nv)));
            n += 2;
            v = Next(mask, v);
        }
    }
    const uint64_t T4 = timing ? __rdtsc() : 0;
    // ---- 4) caps ----
    const s32 sides = n;
    bool overflow = false;
    if (G(b, P + 36)[0] != 0 && L16(b, P + 40) != 0) {
        for (u32 s = 0; s < L16(b, P + 40); ++s) {
            const u32 S = L32(b, P + 44) + s * 8;
            if (n >= 32758) { overflow = true; break; }
            const s32 start = s16(L16(b, S + 0));
            s32 r9; u8 par;
            if (tri[start + 2] != 0) {
                if (n & 1) { S16(b, O(n), L16(b, O(n - 1))); ++n; }
                r9 = start + 2; par = 0;
            } else {
                if (!(n & 1)) { S16(b, O(n), L16(b, O(n - 1))); ++n; }
                r9 = start + 3; par = 1;
            }
            u32 c0 = tri[r9];
            u32 x0, x1;
            if (c0 == 1) { x0 = IDX(start); x1 = IDX(start + 1); }
            else { x0 = IDX(start) + nv; x1 = IDX(start + 1) + nv; }
            if (n >= 32758) { overflow = true; break; }
            if (n == 0) break;
            {
                const u32 xs = u32(s32(s16(u16(x0))));
                S16(b, O(n + 3), x1);
                const u32 prev = L16(b, O(n - 1));
                S16(b, O(n + 1), xs);
                S16(b, O(n + 2), xs);
                S16(b, O(n), prev);
                n += 4;
            }
            s32 last = r9 - 1;  // r29
            s32 i = r9;
            if (i >= s32(s16(L16(b, S + 2))) + s32(s16(L16(b, S + 0)))) continue;
            u32 pc = c0;  // r28
            do {
                const u32 c = tri[i];
                if (c != 0) {
                    const u32 v = c == 1 ? IDX(i) : IDX(i) + nv;
                    bool done = false;
                    if (pc == c) {
                        if (last == i - 1) {
                            if (n >= 32758) { overflow = true; break; }
                            S16(b, O(n), v); n += 1; done = true;
                        } else if (last == i - 2) {
                            if (n >= 32758) { overflow = true; break; }
                            const u32 t = L16(b, O(n - 2));
                            S16(b, O(n + 1), v);
                            S16(b, O(n), t);
                            n += 2; done = true;
                        }
                    }
                    if (!done) {
                        u32 a = IDX(i - 2), a1 = IDX(i - 1);
                        if (c != 1) { a += nv; a1 += nv; }
                        if (n >= 32758) { overflow = true; break; }
                        const u32 as = u32(s32(s16(u16(a))));
                        const u32 prev = L16(b, O(n - 1));
                        if (s32(par) + (n & 1) == 1) {
                            S16(b, O(n + 3), as); S16(b, O(n + 2), as); S16(b, O(n), prev); S16(b, O(n + 1), as);
                            S16(b, O(n + 5), v); S16(b, O(n + 4), a1);
                            n += 6;
                        } else {
                            S16(b, O(n + 2), as); S16(b, O(n), prev); S16(b, O(n + 1), as);
                            S16(b, O(n + 4), v); S16(b, O(n + 3), a1);
                            n += 5;
                        }
                    }
                    pc = tri[i];
                    last = i;
                }
                par = u8((par - 1) & 1);
                ++i;
            } while (i < s32(s16(L16(b, S + 2))) + s32(s16(L16(b, S + 0))));
            if (overflow) break;
        }
    }
    c_runs.fetch_add(1, std::memory_order_relaxed);
    if (timing) {
        const uint64_t T5 = __rdtsc();
        ph[0] += T1 - T0; ph[1] += T2 - T1; ph[2] += T3 - T2; ph[3] += T4 - T3; ph[4] += T5 - T4; ph[5] += 1;
        st_size += size; st_nv += nv; st_ni += ni; st_out += u32(n); st_n += 1;
    }
    if (G(b, P + 36)[0]) c_caps.fetch_add(1, std::memory_order_relaxed);
    if (overflow) c_ovf.fetch_add(1, std::memory_order_relaxed);
    u32 result;
    if (overflow) result = 0;
    else if (G(b, P + 36)[0] == 0) result = u32(sides);
    else result = u32(n);
    S32(b, P + 0x10030, result);
    return true;
}

// ---- switches / verify ----
int Mode() {  // 0 original, 1 native, 2 verify
    static const int m = [] {
        if (FILE* f = std::fopen("native_shadow_off", "rb")) { std::fclose(f); return 0; }
        if (FILE* f = std::fopen("native_shadow_verify", "rb")) { std::fclose(f); return 2; }
        return 1;
    }();
    return m;
}
std::mutex vm;
uint64_t v_calls = 0, v_bad = 0, v_csr_bad = 0, v_last_log = 0;
thread_local uint64_t v_bytes = 0;
uint64_t v_bytes_total = 0, v_t_orig = 0, v_t_nat = 0;
u32 v_first_off = 0xFFFFFFFF;
constexpr u32 kLo = 40, kHi = 0x10034;
thread_local std::vector<u8> v_pre, v_orig;
thread_local u32 v_tick = 0;
void Verify(PPCContext& ctx, u8* base) {
    const u32 P = ctx.r3.u32;
    v_pre.resize(kHi - kLo); v_orig.resize(kHi - kLo);
    std::memcpy(v_pre.data(), G(base, P + kLo), kHi - kLo);
    // The stack below the caller as the original found it (the flag arrays and this frame's
    // scratch): the original reads a few never-written bytes there (e.g. +162 copied to P+42).
    const u32 r31 = ctx.r1.u32 - 33168u;
    const u32 vinfo = L32(base, L32(base, P + 4) + 8);
    const u32 nv = L16(base, vinfo + 12), ni = L16(base, L32(base, L32(base, P + 0) + 8) + 8);
    const u32 lo = r31 + ((0u - nv) & 0xFFFFFFF0u) + ((0u - ni) & 0xFFFFFFF0u);
    const u32 hi = r31 + 320;
    thread_local std::vector<u8> v_stack;
    v_stack.assign(G(base, lo), G(base, lo) + (hi - lo));
    PPCContext c0 = ctx;
    const uint64_t t0 = __rdtsc();
    __imp__sub_822447B0(ctx, base);
    const uint64_t t1 = __rdtsc();
    std::memcpy(v_orig.data(), G(base, P + kLo), kHi - kLo);
    std::memcpy(G(base, P + kLo), v_pre.data(), kHi - kLo);
    std::memcpy(G(base, lo), v_stack.data(), hi - lo);
    const uint64_t t2 = __rdtsc();
    timing = true;
    const bool ran = Run(c0, base, false);
    timing = false;
    const uint64_t t3 = __rdtsc();
    if (!ran) {
        std::memcpy(G(base, P + kLo), v_orig.data(), kHi - kLo);
        ctx.fpscr.setcsr(ctx.fpscr.csr);
        return;
    }
    u32 diff = 0xFFFFFFFF;
    if (std::memcmp(G(base, P + kLo), v_orig.data(), kHi - kLo) != 0) {
        const u8* g = G(base, P + kLo);
        for (u32 k = 0; k < kHi - kLo; ++k) if (g[k] != v_orig[k]) { if (diff == 0xFFFFFFFF) diff = k + kLo; ++v_bytes; }
    }
    const bool csr_bad = (c0.fpscr.csr & 0xFFC0) != (ctx.fpscr.csr & 0xFFC0);
    std::memcpy(G(base, P + kLo), v_orig.data(), kHi - kLo);  // the original's result stands
    ctx.fpscr.setcsr(ctx.fpscr.csr);
    std::lock_guard lock(vm);
    ++v_calls;
    if (diff != 0xFFFFFFFF) { ++v_bad; if (v_first_off == 0xFFFFFFFF) v_first_off = diff; }
    if (csr_bad) ++v_csr_bad;
    v_bytes_total += v_bytes; v_bytes = 0;
    v_t_orig += t1 - t0; v_t_nat += t3 - t2;
    const uint64_t now = GetTickCount64();
    if (now - v_last_log > 10000) {
        v_last_log = now;
        REXLOG_INFO("NATIVE SHADOW VERIFY: {} calls compared, {} differ (first at P+0x{:X}, {} bytes in all), MXCSR "
                    "differs {}, frames not 16-byte aligned {} | time original/native {:.2f}x | native runs {}: plane "
                    "table {}, caps {}, overflow {}", v_calls, v_bad, v_first_off, v_bytes_total, v_csr_bad,
                    unaligned.load(), v_t_nat ? double(v_t_orig) / double(v_t_nat) : 0.0, c_runs.load(), c_planes.load(),
                    c_caps.load(), c_ovf.load());
        const double nn = double(std::max<uint64_t>(1, ph[5].load()));
        REXLOG_INFO("NATIVE SHADOW phases (slow tests {}) (ticks/call): clear {:.0f}, lit {:.0f}, edges {:.0f}, sides {:.0f}, caps {:.0f} | "
                    "avg table {:.0f}, vertices {:.0f}, indices {:.0f}, output {:.0f}", slow_tests.load(), ph[0] / nn, ph[1] / nn, ph[2] / nn,
                    ph[3] / nn, ph[4] / nn, st_size / nn, st_nv / nn, st_ni / nn, st_out / nn);
    }
}
}  // namespace native_shadow
