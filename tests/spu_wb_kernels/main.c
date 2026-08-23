/* WB kernel differential test (AVX2 TU).
 *
 * Compares every WB SIMD kernel in runtime/spu/spu_wb_simd.h against its
 * spu_helpers.h twin compiled in the baseline-ISA reference TU
 * (reference.c). Bit-exact assertion over structured edge cases plus
 * randomized operands; immediate forms sweep their full field ranges
 * (including out-of-range shift counts, which the helpers mask/clamp).
 *
 * Exit 0 = all bit-exact. Exit 1 = mismatch (first few printed).
 * If the host lacks AVX2 the test prints SKIP and exits 0 (the WB lane
 * itself never registers on such hosts -- spu_wb_runtime_ok()).
 */
#define _CRT_SECURE_NO_WARNINGS
#include "spu_wb_simd.h"
#include "reference.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng_state = UINT64_C(0x9e3779b97f4a7c15);
static uint64_t next_random(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * UINT64_C(0x2545f4914f6cdd1d);
}
static u128 rand_u128(void)
{
    u128 r;
    r._u64[0] = next_random();
    r._u64[1] = next_random();
    return r;
}

/* ---- edge pool ---------------------------------------------------------- */
static const uint32_t k_edge_words[] = {
    0x00000000u, 0x00000001u, 0x00000002u, 0x00000003u,
    0x0000007Fu, 0x00000080u, 0x000000FFu, 0x00000100u,
    0x00007FFFu, 0x00008000u, 0x0000FFFFu, 0x00010000u,
    0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x80000001u,
    0x00800000u, 0x007FFFFFu, 0x7F800000u, 0xFF800000u,
    0x7FC00000u, 0x3F800000u, 0xBF800000u, 0x00000010u,
    0x0F0F0F0Fu, 0xF0F0F0F0u, 0x01234567u, 0x89ABCDEFu,
    0xC0C0C0C0u, 0xE0E0E0E0u, 0x80808080u, 0x1F1F1F1Fu,
};
#define N_EDGE_WORDS (sizeof(k_edge_words) / sizeof(k_edge_words[0]))
#define N_POOL 96
static u128 g_pool[N_POOL];

static void build_pool(void)
{
    size_t i;
    for (i = 0; i < N_EDGE_WORDS; i++) {
        u128 v;
        v._u32[0] = k_edge_words[i];
        v._u32[1] = k_edge_words[(i + 7) % N_EDGE_WORDS];
        v._u32[2] = k_edge_words[(i + 13) % N_EDGE_WORDS];
        v._u32[3] = k_edge_words[(i + 21) % N_EDGE_WORDS];
        g_pool[i] = v;
    }
    for (i = N_EDGE_WORDS; i < N_EDGE_WORDS * 2; i++) {
        u128 v;
        v._u32[0] = v._u32[1] = v._u32[2] = v._u32[3] =
            k_edge_words[i - N_EDGE_WORDS];
        g_pool[i] = v;
    }
    for (i = N_EDGE_WORDS * 2; i < N_POOL; i++)
        g_pool[i] = rand_u128();
}

static const int32_t k_edge_imms[] = {
    0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 63, -1, -2, -7, -8, -15, -16,
    -31, -32, -63, -64, 64, 127, -128, 255, 256, 511, -512, -511,
    1023, -1024, 12345, -12345, 32767, -32768,
};
#define N_EDGE_IMMS (sizeof(k_edge_imms) / sizeof(k_edge_imms[0]))

/* ---- reporting ---------------------------------------------------------- */
static uint64_t g_cases = 0;
static int g_failures = 0;

static void dump(const char* tag, const u128* v)
{
    fprintf(stderr, "  %s = %08X %08X %08X %08X\n", tag,
            v->_u32[0], v->_u32[1], v->_u32[2], v->_u32[3]);
}
static int report(const char* op, const u128* a, const u128* b, const u128* c,
                  int32_t imm, int has_imm, const u128* got, const u128* want)
{
    if (memcmp(got, want, sizeof(*got)) == 0) {
        g_cases++;
        return 0;
    }
    g_failures++;
    if (g_failures <= 16) {
        fprintf(stderr, "MISMATCH %s%s", op, has_imm ? " imm=" : "\n");
        if (has_imm) fprintf(stderr, "%d\n", imm);
        if (a) dump("a", a);
        if (b) dump("b", b);
        if (c) dump("c", c);
        dump("got ", got);
        dump("want", want);
    }
    return 1;
}

/* ---- drivers ------------------------------------------------------------ */
#define RUN1(name) do {                                                       \
    size_t pi; uint64_t r;                                                    \
    for (pi = 0; pi < N_POOL; pi++) {                                         \
        u128 a = g_pool[pi];                                                  \
        u128 got = wb_to_u128(wbk_##name(wb_from_u128(a)));                   \
        u128 want = ref_##name(a);                                            \
        report(#name, &a, NULL, NULL, 0, 0, &got, &want);                     \
    }                                                                         \
    for (r = 0; r < n_random; r++) {                                          \
        u128 a = rand_u128();                                                 \
        u128 got = wb_to_u128(wbk_##name(wb_from_u128(a)));                   \
        u128 want = ref_##name(a);                                            \
        report(#name, &a, NULL, NULL, 0, 0, &got, &want);                     \
    }                                                                         \
} while (0)

#define RUN2(name) do {                                                       \
    size_t pi, pj; uint64_t r;                                                \
    for (pi = 0; pi < N_EDGE_WORDS * 2; pi++)                                 \
        for (pj = 0; pj < N_EDGE_WORDS * 2; pj++) {                           \
            u128 a = g_pool[pi], b = g_pool[pj];                              \
            u128 got = wb_to_u128(wbk_##name(wb_from_u128(a), wb_from_u128(b))); \
            u128 want = ref_##name(a, b);                                     \
            report(#name, &a, &b, NULL, 0, 0, &got, &want);                   \
        }                                                                     \
    for (r = 0; r < n_random; r++) {                                          \
        u128 a = rand_u128(), b = rand_u128();                                \
        u128 got = wb_to_u128(wbk_##name(wb_from_u128(a), wb_from_u128(b)));  \
        u128 want = ref_##name(a, b);                                         \
        report(#name, &a, &b, NULL, 0, 0, &got, &want);                       \
    }                                                                         \
} while (0)

#define RUN3(name) do {                                                       \
    size_t pi; uint64_t r;                                                    \
    for (pi = 0; pi < N_POOL; pi++) {                                         \
        u128 a = g_pool[pi];                                                  \
        u128 b = g_pool[(pi * 7 + 3) % N_POOL];                               \
        u128 c = g_pool[(pi * 13 + 5) % N_POOL];                              \
        u128 got = wb_to_u128(wbk_##name(wb_from_u128(a), wb_from_u128(b),    \
                                         wb_from_u128(c)));                   \
        u128 want = ref_##name(a, b, c);                                      \
        report(#name, &a, &b, &c, 0, 0, &got, &want);                         \
    }                                                                         \
    for (r = 0; r < n_random; r++) {                                          \
        u128 a = rand_u128(), b = rand_u128(), c = rand_u128();               \
        u128 got = wb_to_u128(wbk_##name(wb_from_u128(a), wb_from_u128(b),    \
                                         wb_from_u128(c)));                   \
        u128 want = ref_##name(a, b, c);                                      \
        report(#name, &a, &b, &c, 0, 0, &got, &want);                         \
    }                                                                         \
} while (0)

/* immediate forms: full [-64, 127] shift/i7 window + edge imms + randoms,
 * each against pool + random operands */
#define RUNI(name) do {                                                       \
    int32_t imm; size_t ii, pi; uint64_t r;                                   \
    for (imm = -64; imm <= 127; imm++) {                                      \
        for (pi = 0; pi < N_EDGE_WORDS; pi++) {                               \
            u128 a = g_pool[pi];                                              \
            u128 got = wb_to_u128(wbk_##name(wb_from_u128(a), imm));          \
            u128 want = ref_##name(a, imm);                                   \
            report(#name, &a, NULL, NULL, imm, 1, &got, &want);               \
        }                                                                     \
        for (r = 0; r < n_random / 64 + 1; r++) {                             \
            u128 a = rand_u128();                                             \
            u128 got = wb_to_u128(wbk_##name(wb_from_u128(a), imm));          \
            u128 want = ref_##name(a, imm);                                   \
            report(#name, &a, NULL, NULL, imm, 1, &got, &want);               \
        }                                                                     \
    }                                                                         \
    for (ii = 0; ii < N_EDGE_IMMS; ii++) {                                    \
        for (pi = 0; pi < N_EDGE_WORDS; pi++) {                               \
            u128 a = g_pool[pi];                                              \
            u128 got = wb_to_u128(wbk_##name(wb_from_u128(a), k_edge_imms[ii])); \
            u128 want = ref_##name(a, k_edge_imms[ii]);                       \
            report(#name, &a, NULL, NULL, k_edge_imms[ii], 1, &got, &want);   \
        }                                                                     \
    }                                                                         \
} while (0)

int main(int argc, char** argv)
{
    uint64_t n_random = 200000;
    if (argc > 1)
        n_random = (uint64_t)strtoull(argv[1], NULL, 0);
    if (!spu_wb_runtime_ok()) {
        printf("SKIP: host lacks AVX2/OS support; WB lane never registers here\n");
        return 0;
    }
    build_pool();

    /* unary */
    RUN1(clz); RUN1(cntb); RUN1(gb); RUN1(gbh); RUN1(gbb); RUN1(orx);
    RUN1(xsbh); RUN1(xshw); RUN1(xswd); RUN1(fsm); RUN1(fsmh); RUN1(fsmb);
    RUN1(frest); RUN1(frsqest); RUN1(fesd); RUN1(frds); RUN1(mfspr);

    /* binary */
    RUN2(a); RUN2(sf); RUN2(ah); RUN2(sfh);
    RUN2(and); RUN2(or); RUN2(xor); RUN2(nand); RUN2(nor);
    RUN2(andc); RUN2(orc); RUN2(eqv);
    RUN2(ceq); RUN2(ceqh); RUN2(ceqb); RUN2(cgt); RUN2(cgth); RUN2(cgtb);
    RUN2(clgt); RUN2(clgth); RUN2(clgtb);
    RUN2(mpy); RUN2(mpyu); RUN2(mpyh); RUN2(mpyhh); RUN2(mpyhhu); RUN2(mpys);
    RUN2(fi); RUN2(fa); RUN2(fs); RUN2(fm);
    RUN2(fceq); RUN2(fcgt); RUN2(fcmeq); RUN2(fcmgt);
    RUN2(dfa); RUN2(dfs); RUN2(dfm);
    RUN2(dfceq); RUN2(dfcmeq); RUN2(dfcgt); RUN2(dfcmgt);
    RUN2(absdb); RUN2(avgb); RUN2(cg); RUN2(bg); RUN2(sumb);
    RUN2(shl); RUN2(rot); RUN2(rotm); RUN2(rotma);
    RUN2(shlh); RUN2(roth); RUN2(rothm); RUN2(rothma);
    RUN2(shlqbi); RUN2(rotqbi); RUN2(shlqby); RUN2(rotqby);
    RUN2(shlqbybi); RUN2(rotqbybi);
    RUN2(rotqmbi); RUN2(rotqmby); RUN2(rotqmbybi);

    /* ternary */
    RUN3(selb); RUN3(shufb); RUN3(mpya); RUN3(fma); RUN3(fms); RUN3(fnms);
    RUN3(addx); RUN3(sfx); RUN3(cgx); RUN3(bgx);
    RUN3(dfma); RUN3(dfms); RUN3(dfnms); RUN3(dfnma);
    RUN3(mpyhha); RUN3(mpyhhau);

    /* immediates */
    RUNI(ai); RUNI(ahi); RUNI(sfi); RUNI(sfhi);
    RUNI(andi); RUNI(ori); RUNI(xori);
    RUNI(andhi); RUNI(andbi); RUNI(orhi); RUNI(orbi); RUNI(xorhi); RUNI(xorbi);
    RUNI(iohl);
    RUNI(ceqi); RUNI(cgti); RUNI(clgti); RUNI(ceqbi); RUNI(ceqhi);
    RUNI(clgtbi); RUNI(clgthi); RUNI(cgthi); RUNI(cgtbi);
    RUNI(mpyi); RUNI(mpyui);
    RUNI(shli); RUNI(shlhi); RUNI(roti); RUNI(rothi);
    RUNI(rotmi); RUNI(rotmai); RUNI(rotmhi); RUNI(rothmi); RUNI(rotmahi);
    RUNI(shlqbyi); RUNI(rotqbyi); RUNI(shlqbii); RUNI(rotqbii);
    RUNI(rotqmbii); RUNI(rotqmbyi);
    RUNI(cflts); RUNI(cfltu); RUNI(csflt); RUNI(cuflt); RUNI(dftsv);

    /* generate-controls: scalar-position kernels vs the register-form
     * helpers, over positions covering wrap and both alignments */
    {
        uint32_t base;
        int32_t i7;
        for (base = 0; base < 64; base++) {
            for (i7 = -64; i7 <= 63; i7++) {
                u128 a; memset(&a, 0, sizeof a);
                a._u32[0] = base;
                {
                    u128 got = wb_to_u128(wbk_cbd_pos(base + (uint32_t)i7));
                    u128 want = ref_cbd(a, i7);
                    report("cbd", &a, NULL, NULL, i7, 1, &got, &want);
                }
                {
                    u128 got = wb_to_u128(wbk_chd_pos(base + (uint32_t)i7));
                    u128 want = ref_chd(a, i7);
                    report("chd", &a, NULL, NULL, i7, 1, &got, &want);
                }
                {
                    u128 got = wb_to_u128(wbk_cwd_pos(base + (uint32_t)i7));
                    u128 want = ref_cwd(a, i7);
                    report("cwd", &a, NULL, NULL, i7, 1, &got, &want);
                }
                {
                    u128 got = wb_to_u128(wbk_cdd_pos(base + (uint32_t)i7));
                    u128 want = ref_cdd(a, i7);
                    report("cdd", &a, NULL, NULL, i7, 1, &got, &want);
                }
            }
        }
        /* x-forms: pos = a0 + b0, random pairs */
        for (base = 0; base < 4096; base++) {
            u128 a = rand_u128(), b = rand_u128();
            {
                u128 got = wb_to_u128(wbk_cbd_pos(a._u32[0] + b._u32[0]));
                u128 want = ref_cbx(a, b);
                report("cbx", &a, &b, NULL, 0, 0, &got, &want);
            }
            {
                u128 got = wb_to_u128(wbk_chd_pos(a._u32[0] + b._u32[0]));
                u128 want = ref_chx(a, b);
                report("chx", &a, &b, NULL, 0, 0, &got, &want);
            }
            {
                u128 got = wb_to_u128(wbk_cwd_pos(a._u32[0] + b._u32[0]));
                u128 want = ref_cwx(a, b);
                report("cwx", &a, &b, NULL, 0, 0, &got, &want);
            }
            {
                u128 got = wb_to_u128(wbk_cdd_pos(a._u32[0] + b._u32[0]));
                u128 want = ref_cdx(a, b);
                report("cdx", &a, &b, NULL, 0, 0, &got, &want);
            }
        }
    }

    /* fsmb exhaustive over all 65536 preferred-halfword patterns */
    {
        uint32_t p;
        for (p = 0; p < 0x10000; p++) {
            u128 a = rand_u128();
            a._u32[0] = (a._u32[0] & 0xFFFF0000u) | p;
            {
                u128 got = wb_to_u128(wbk_fsmb(wb_from_u128(a)));
                u128 want = ref_fsmb(a);
                report("fsmb-exh", &a, NULL, NULL, 0, 0, &got, &want);
            }
        }
    }

    /* variable shift counts 0..255 in the preferred slot, every quad form */
    {
        uint32_t cnt;
        for (cnt = 0; cnt < 256; cnt++) {
            u128 a = rand_u128();
            u128 b; memset(&b, 0, sizeof b);
            b._u32[0] = cnt | (cnt << 16);   /* also exercise sub-lane counts */
#define SHIFT_CASE(nm) do {                                                   \
            u128 got = wb_to_u128(wbk_##nm(wb_from_u128(a), wb_from_u128(b))); \
            u128 want = ref_##nm(a, b);                                       \
            report(#nm "-cnt", &a, &b, NULL, (int32_t)cnt, 1, &got, &want);   \
} while (0)
            SHIFT_CASE(shl); SHIFT_CASE(rot); SHIFT_CASE(rotm); SHIFT_CASE(rotma);
            SHIFT_CASE(shlh); SHIFT_CASE(roth); SHIFT_CASE(rothm); SHIFT_CASE(rothma);
            SHIFT_CASE(shlqbi); SHIFT_CASE(rotqbi); SHIFT_CASE(rotqmbi);
            SHIFT_CASE(shlqby); SHIFT_CASE(rotqby); SHIFT_CASE(rotqmby);
            SHIFT_CASE(shlqbybi); SHIFT_CASE(rotqbybi); SHIFT_CASE(rotqmbybi);
#undef SHIFT_CASE
            /* negative / huge counts */
            b._u32[0] = 0u - cnt;
            {
                u128 got = wb_to_u128(wbk_rotqmby(wb_from_u128(a), wb_from_u128(b)));
                u128 want = ref_rotqmby(a, b);
                report("rotqmby-neg", &a, &b, NULL, (int32_t)cnt, 1, &got, &want);
            }
            {
                u128 got = wb_to_u128(wbk_rotqmbi(wb_from_u128(a), wb_from_u128(b)));
                u128 want = ref_rotqmbi(a, b);
                report("rotqmbi-neg", &a, &b, NULL, (int32_t)cnt, 1, &got, &want);
            }
            {
                u128 got = wb_to_u128(wbk_rotqmbybi(wb_from_u128(a), wb_from_u128(b)));
                u128 want = ref_rotqmbybi(a, b);
                report("rotqmbybi-neg", &a, &b, NULL, (int32_t)cnt, 1, &got, &want);
            }
        }
    }

    if (g_failures) {
        fprintf(stderr, "spu_wb_kernels: %d MISMATCH(ES) in %" PRIu64 " cases\n",
                g_failures, g_cases);
        return 1;
    }
    printf("spu_wb_kernels: OK, %" PRIu64 " cases bit-exact (n_random=%" PRIu64 ")\n",
           g_cases, n_random);
    return 0;
}
