// NATIVE 4x4 MATRIX MULTIPLY (2026-10-01). 8219DFC0(r3 out, r4 A, r5 B): out = A * B, row-major,
// 16 dot products (vmsum4fp128) on B transposed; ~2 % of all CPU (render thread + colour worker).
// Same results as the recompiled code's rex::ppc::simde_mm_vmsum4fp128, a row at a time instead of
// 16 dots. Bit for bit except the payload or sign of a NaN from two or more NaN sources (NaN inputs,
// inf * 0, inf - inf): the compiler may swap operands and x86 keeps the first NaN. Same flush-mode
// switch, same stack copies (r1-128 = B, r1-64 = A) and the same aligned-down stores.
// Needs a 16-byte aligned r1 (else the original runs). dist\native_math_off = original;
// dist\native_math_verify = 1 call in 64: native into a temp, original for real, compare (two NaNs
// count as equal). The first call runs SelfCheck; if it fails, the log has an error and the
// original runs.
#include <rex/ppc/intrinsics.h>
extern "C" void __imp__sub_8219DFC0(PPCContext& ctx, uint8_t* base);
namespace native_math {
using u8 = uint8_t; using u32 = uint32_t;
inline u8* G(u8* b, u32 a) { return b + a + REX_PHYS_HOST_OFFSET(a); }
// A * B on host vectors (rows; host lane l holds guest element 3 - l) into out[4]. Row i of the
// product is the sum over k of A[i][k] * (row k of B), and lane l of row k of B is B[k][3 - l], so
// lane for lane this is what the original's 16 dots (A row i against B column 3 - l) produce. The
// products are exact in double, so the sum order alone decides the bits: (w + y) + (z + x), as
// simde_mm_vmsum4fp128 adds its host lanes (0 + 2) + (1 + 3).
inline void MulRows(const __m128 a[4], const __m128 bm[4], __m128 out[4]) {
    __m128d lo[4], hi[4];  // row k of B in double: host lanes 0-1 (B[k][3], B[k][2]) / 2-3 (B[k][1], B[k][0])
    for (int k = 0; k < 4; ++k) {
        lo[k] = _mm_cvtps_pd(bm[k]);
        hi[k] = _mm_cvtps_pd(_mm_movehl_ps(bm[k], bm[k]));
    }
    for (int i = 0; i < 4; ++i) {
        const __m128d a_wz = _mm_cvtps_pd(a[i]), a_yx = _mm_cvtps_pd(_mm_movehl_ps(a[i], a[i]));
        const __m128d w = _mm_unpacklo_pd(a_wz, a_wz), z = _mm_unpackhi_pd(a_wz, a_wz);  // A[i][3], A[i][2]
        const __m128d y = _mm_unpacklo_pd(a_yx, a_yx), x = _mm_unpackhi_pd(a_yx, a_yx);  // A[i][1], A[i][0]
        const __m128d s_lo = _mm_add_pd(_mm_add_pd(_mm_mul_pd(w, lo[3]), _mm_mul_pd(y, lo[1])),
                                        _mm_add_pd(_mm_mul_pd(z, lo[2]), _mm_mul_pd(x, lo[0])));
        const __m128d s_hi = _mm_add_pd(_mm_add_pd(_mm_mul_pd(w, hi[3]), _mm_mul_pd(y, hi[1])),
                                        _mm_add_pd(_mm_mul_pd(z, hi[2]), _mm_mul_pd(x, hi[0])));
        out[i] = _mm_movelh_ps(_mm_cvtpd_ps(s_lo), _mm_cvtpd_ps(s_hi));
    }
}
// Computes the 4 output rows (already byte-reversed for big-endian storing) into out[4].
inline void Mul(u8* b, u32 ra, u32 rb, __m128i out[4]) {
    const __m128i rev = _mm_load_si128(reinterpret_cast<const __m128i*>(rex::ppc::VectorMaskL));
    auto L = [&](u32 a) {
        return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(G(b, a))), rev));
    };
    __m128 a[4], bm[4], r[4];
    for (int k = 0; k < 4; ++k) a[k] = L(ra + 16 * k), bm[k] = L(rb + 16 * k);
    MulRows(a, bm, r);
    for (int i = 0; i < 4; ++i) out[i] = _mm_shuffle_epi8(_mm_castps_si128(r[i]), rev);  // -> r3 + 16 * i
}
// Drift guard: the arithmetic is written twice (MulRows here, simde_mm_vmsum4fp128 in the SDK).
// SelfCheck compares MulRows, bit for bit, with 16 simde_mm_vmsum4fp128 calls in the original's
// layout (out row i lane l = A row i . B column 3 - l) on 3 fixed matrix products. The results
// change if either side adds the four products in another order, rounds the products to single
// precision or rounds a partial sum to single precision. If they differ, Mode() logs an error and
// the original runs.
bool SelfCheck() {
    constexpr float tiny = 0x1p-60f, big = 0x1p40f;
    constexpr float inexact = 1 + 0x1p-12f;     // inexact^2 = 1 + 2^-11 + 2^-24 is not a float
    constexpr float rounded_sq = 1 + 0x1p-11f;  // inexact^2 rounded to float
    // matrix[0] * matrix[1]: rows 0-2 . columns 1-3 (ones) cancel and absorb tiny (sum order);
    // row 3 . column 0 = inexact^2 - rounded_sq = 2^-24, 0 with rounded products or partial sums.
    static const float matrix[4][16] = {  // row-major
        {tiny, tiny, -1, 1, -1, tiny, tiny, 1, 1, -1, tiny, tiny, inexact, 1, 0, 0},
        {inexact, 1, 1, 1, -rounded_sq, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1},
        {3, -big, 0x1p-20f, 7, big, 1.5f, -big, 0x1p-30f, -2, 0.1f, 5, -0.3f, 0x1p60f, -1, 0x1p-40f, 1},
        {0.5f, 1e-7f, -3, big, 2, -0x1p30f, 0x1p30f, 1, -1e5f, 7, 0x1p-12f, 1, 1, 0x1p-40f, -1, 0x1p20f},
    };
    static const int pairs[3][2] = {{0, 1}, {2, 3}, {3, 2}};  // A, B
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(0x1F80);  // round to nearest, no flush, as when the compiler folds constants
    bool ok = true;
    for (const auto& p : pairs) {
        const float *A = matrix[p[0]], *B = matrix[p[1]];
        __m128 a[4], bm[4], got[4];
        for (int k = 0; k < 4; ++k) {
            a[k] = _mm_set_ps(A[4 * k], A[4 * k + 1], A[4 * k + 2], A[4 * k + 3]);
            bm[k] = _mm_set_ps(B[4 * k], B[4 * k + 1], B[4 * k + 2], B[4 * k + 3]);
        }
        MulRows(a, bm, got);
        for (int i = 0; i < 4; ++i) {
            float d[4];  // d[j] = A row i . B column j
            for (int j = 0; j < 4; ++j) {
                const __m128 col = _mm_set_ps(B[j], B[4 + j], B[8 + j], B[12 + j]);
                d[j] = _mm_cvtss_f32(rex::ppc::simde_mm_vmsum4fp128(a[i], col));
            }
            const __m128i want = _mm_castps_si128(_mm_set_ps(d[0], d[1], d[2], d[3]));
            ok &= _mm_movemask_epi8(_mm_cmpeq_epi32(_mm_castps_si128(got[i]), want)) == 0xFFFF;
        }
    }
    _mm_setcsr(csr);
    return ok;
}
// Verify compare of one output row (byte-reversed) with guest memory: per 32-bit word, equal bits
// or both NaN (the NaN payload can differ, see the top).
inline bool SameRow(const __m128i& o, const u8* g) {
    for (int k = 0; k < 4; ++k) {
        u32 x, y;
        std::memcpy(&x, reinterpret_cast<const u8*>(&o) + 4 * k, 4);
        std::memcpy(&y, g + 4 * k, 4);
        if (x != y && ((__builtin_bswap32(x) & 0x7FFFFFFFu) <= 0x7F800000u ||
                       (__builtin_bswap32(y) & 0x7FFFFFFFu) <= 0x7F800000u))
            return false;
    }
    return true;
}
int Mode() {
    static const int m = [] {
        if (FILE* f = std::fopen("native_math_off", "rb")) { std::fclose(f); return 0; }
        if (!SelfCheck()) {
            REXLOG_ERROR("NATIVE MATH: self-check against simde_mm_vmsum4fp128 failed, the original runs");
            return 0;
        }
        if (FILE* f = std::fopen("native_math_verify", "rb")) { std::fclose(f); return 2; }
        return 1;
    }();
    return m;
}
std::atomic<uint64_t> v_calls{0}, v_bad{0}, unaligned{0};
std::atomic<int64_t> v_last{0};
}  // namespace native_math

PPC_FUNC(sub_8219DFC0) {
    using namespace native_math;
    const int mode = Mode();
    if (mode == 0 || (ctx.r1.u32 & 15)) {
        if (mode && (ctx.r1.u32 & 15)) unaligned.fetch_add(1, std::memory_order_relaxed);
        __imp__sub_8219DFC0(ctx, base);
        return;
    }
    const u32 r1 = ctx.r1.u32, r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
    if (mode == 2) {
        static thread_local uint32_t tick = 0;
        if ((++tick & 63) == 0) {
            __m128i o[4];
            ctx.fpscr.enableFlushMode();
            Mul(base, r4, r5, o);
            __imp__sub_8219DFC0(ctx, base);
            bool bad = false;
            for (int k = 0; k < 4; ++k)
                bad |= !SameRow(o[k], G(base, (r3 + 16 * k) & ~15u));
            ++v_calls;
            if (bad) ++v_bad;
            const int64_t now = int64_t(GetTickCount64());
            int64_t last = v_last.load();
            if (now - last > 10000 && v_last.compare_exchange_strong(last, now))
                REXLOG_INFO("NATIVE MATH VERIFY: {} matrix products compared, {} differ, unaligned stacks {}",
                            v_calls.load(), v_bad.load(), unaligned.load());
            return;
        }
    }
    // The original's stack copies (B at r1-128, A at r1-64), then the product.
    std::memmove(G(base, r1 - 128), G(base, r5), 64);
    std::memmove(G(base, r1 - 64), G(base, r4), 64);
    ctx.fpscr.enableFlushMode();
    __m128i o[4];
    Mul(base, r1 - 64, r1 - 128, o);
    for (int k = 0; k < 4; ++k) _mm_store_si128(reinterpret_cast<__m128i*>(G(base, (r3 + 16 * k) & ~15u)), o[k]);
}
