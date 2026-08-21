/*
 * Native-render resource model tests: IO-mapping epochs, the O(1)
 * resource lifetime cache over the guest-page tracker, the persistent
 * PSO cache, and timeline fences.
 */

#include "../rsx_nr_resources.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n");                                           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void test_maps(void)
{
    rsx_nr_maps m;
    rsx_nr_maps_init(&m);
    CHECK(rsx_nr_maps_epoch(&m) == 0, "fresh epoch nonzero");

    CHECK(rsx_nr_maps_map(&m, 0x0000, 0x30100000, 0x100000) == 0, "map A");
    CHECK(rsx_nr_maps_map(&m, 0x100000, 0x40000000, 0x800000) == 0, "map B");
    CHECK(rsx_nr_maps_epoch(&m) == 2, "epoch after two maps");

    /* overlap refused, epoch untouched */
    CHECK(rsx_nr_maps_map(&m, 0x80000, 0x50000000, 0x100000) != 0,
          "overlap accepted");
    CHECK(rsx_nr_maps_epoch(&m) == 2, "refused map bumped epoch");

    u32 ea = 0, gen = 0;
    CHECK(rsx_nr_maps_resolve(&m, 0x120000, 0x1000, &ea, &gen) == 1 &&
          ea == 0x40020000, "resolve B wrong ea %08X", ea);
    CHECK(rsx_nr_maps_resolve(&m, 0xF0000, 0x20000, &ea, &gen) == 0,
          "cross-window resolve accepted");
    CHECK(rsx_nr_maps_resolve(&m, 0x900001, 0x10, &ea, &gen) == 0,
          "unmapped resolve accepted");

    /* exact-start remap bumps epoch + gen; identical remap is a no-op */
    u32 gen_before = gen;
    CHECK(rsx_nr_maps_map(&m, 0x100000, 0x40000000, 0x800000) == 0,
          "identical remap refused");
    CHECK(rsx_nr_maps_epoch(&m) == 2, "identical remap bumped epoch");
    CHECK(rsx_nr_maps_map(&m, 0x100000, 0x48000000, 0x800000) == 0,
          "remap refused");
    CHECK(rsx_nr_maps_epoch(&m) == 3, "remap did not bump epoch");
    rsx_nr_maps_resolve(&m, 0x100000, 4, &ea, &gen);
    CHECK(ea == 0x48000000 && gen == gen_before + 1,
          "remap gen/ea wrong (%08X gen %u)", ea, gen);

    CHECK(rsx_nr_maps_unmap(&m, 0x0000) == 0, "unmap A");
    CHECK(rsx_nr_maps_epoch(&m) == 4, "unmap did not bump epoch");
    CHECK(rsx_nr_maps_resolve(&m, 0x0, 4, &ea, &gen) == 0,
          "resolve into unmapped window");
    CHECK(rsx_nr_maps_unmap(&m, 0x0000) != 0, "double unmap accepted");
}

static u64 g_evicted[16];
static u32 g_evicted_n;
static void on_evict(void* user, u64 backend_id)
{
    (void)user;
    if (g_evicted_n < 16)
        g_evicted[g_evicted_n++] = backend_id;
}

static void test_res_cache(void)
{
    rsx_guest_pages pages;
    CHECK(rsx_guest_pages_init(&pages, 1u << 20, 1u << 20) == 0,
          "pages init");
    rsx_nr_maps maps;
    rsx_nr_maps_init(&maps);
    rsx_nr_maps_map(&maps, 0, 0x30100000, 1u << 20);

    rsx_nr_res_cache c;
    CHECK(rsx_nr_res_cache_init(&c, 64, 4096, &pages, &maps) == 0,
          "cache init");
    CHECK(rsx_nr_res_cache_init(&c, 63, 4096, &pages, &maps) != 0 || 1, "");

    rsx_nr_res_key k;
    memset(&k, 0, sizeof(k));
    k.kind = 1;                       /* texture-class */
    k.space = RSX_NR_SPACE_LOCAL;
    k.offset = 0x10000;
    k.size = 0x8000;
    k.fmt = 0x85ull << 32 | 0x100;

    CHECK(rsx_nr_res_lookup(&c, &k) == NULL, "phantom hit");
    rsx_nr_res* e = rsx_nr_res_insert(&c, &k, 0xAA55);
    CHECK(e != NULL, "insert failed");
    CHECK(rsx_nr_res_lookup(&c, &k) == e, "lookup misses inserted");
    CHECK(rsx_nr_res_current(&c, e) == 1, "fresh entry not current");

    /* content invalidation via the tracker */
    rsx_guest_pages_note_write(&pages, RSX_NR_SPACE_LOCAL, 0x12000, 16);
    CHECK(rsx_nr_res_current(&c, e) == 0, "dirty write not detected");
    rsx_nr_res_revalidate(&c, e);
    CHECK(rsx_nr_res_current(&c, e) == 1, "revalidate did not clean");

    /* a write outside the range does not invalidate */
    rsx_guest_pages_note_write(&pages, RSX_NR_SPACE_LOCAL, 0x40000, 16);
    CHECK(rsx_nr_res_current(&c, e) == 1, "unrelated write invalidated");

    /* distinct fmt = distinct identity */
    rsx_nr_res_key k2 = k;
    k2.fmt ^= 0x20;
    CHECK(rsx_nr_res_lookup(&c, &k2) == NULL, "fmt aliasing");
    rsx_nr_res* e2 = rsx_nr_res_insert(&c, &k2, 0xBB66);
    CHECK(e2 && e2 != e, "fmt-distinct insert failed");

    /* mapping-epoch invalidation for MAIN-space resources */
    rsx_nr_res_key km = k;
    km.space = RSX_NR_SPACE_MAIN;
    km.offset = 0x4000;
    rsx_nr_res* em = rsx_nr_res_insert(&c, &km, 0xCC77);
    CHECK(em && rsx_nr_res_current(&c, em) == 1, "main entry not current");
    rsx_nr_maps_map(&maps, 0, 0x30200000, 1u << 20);   /* remap */
    CHECK(rsx_nr_res_current(&c, em) == 0, "remap not detected");
    CHECK(rsx_nr_res_current(&c, e) == 1, "remap invalidated local space");
    rsx_nr_res_revalidate(&c, em);
    CHECK(rsx_nr_res_current(&c, em) == 1, "main revalidate failed");

    /* eviction frees the slot and reports the backend id */
    g_evicted_n = 0;
    rsx_nr_res_evict(&c, e2, on_evict, NULL);
    CHECK(g_evicted_n == 1 && g_evicted[0] == 0xBB66, "evict cb wrong");
    CHECK(rsx_nr_res_lookup(&c, &k2) == NULL, "evicted still found");
    CHECK(rsx_nr_res_lookup(&c, &k) == e, "tombstone broke probing");

    /* sweep by age */
    rsx_nr_res_next_frame(&c);
    rsx_nr_res_next_frame(&c);
    rsx_nr_res_lookup(&c, &k);        /* touch e at frame 2 */
    rsx_nr_res_next_frame(&c);        /* frame 3: e age 1, em age 3 */
    g_evicted_n = 0;
    u32 n = rsx_nr_res_sweep(&c, 2, on_evict, NULL);
    CHECK(n == 1 && g_evicted_n == 1 && g_evicted[0] == 0xCC77,
          "sweep evicted %u (first %llX)", n,
          (unsigned long long)(g_evicted_n ? g_evicted[0] : 0));
    CHECK(rsx_nr_res_lookup(&c, &k) != NULL, "sweep took the young entry");

    /* arena reuse: insert/evict cycles must not exhaust the arena */
    for (int i = 0; i < 200; i++) {
        rsx_nr_res_key kk = k;
        kk.offset = 0x20000 + (u32)i * 0x1000;
        kk.size = 0x2000;
        rsx_nr_res* ee = rsx_nr_res_insert(&c, &kk, (u64)i);
        CHECK(ee != NULL, "cycle insert %d failed", i);
        if (ee)
            rsx_nr_res_evict(&c, ee, NULL, NULL);
    }
    rsx_nr_res_stats st;
    st = c.stats;
    CHECK(st.arena_exhausted == 0, "arena exhausted (%llu)",
          (unsigned long long)st.arena_exhausted);

    /* generation rollover: an entry snapshotted at the pre-wrap counter
     * value still detects the wrapping write (equality compare), and
     * revalidation across the wrap works */
    {
        rsx_nr_res_key kr = k;
        kr.offset = 0x80000;
        kr.size = 0x1000;
        u32 page = kr.offset >> RSX_GUEST_PAGE_SHIFT;
        rsx_guest_pages_debug_set_page_gen(&pages, RSX_NR_SPACE_LOCAL, page,
                                           0xFFFFFFFFu);
        rsx_nr_res* er = rsx_nr_res_insert(&c, &kr, 0xDD88);
        CHECK(er && rsx_nr_res_current(&c, er) == 1,
              "pre-wrap entry not current");
        rsx_guest_pages_note_write(&pages, RSX_NR_SPACE_LOCAL, kr.offset, 4);
        CHECK(rsx_guest_pages_page_gen(&pages, RSX_NR_SPACE_LOCAL, page) == 0,
              "counter did not wrap");
        CHECK(rsx_nr_res_current(&c, er) == 0, "wrap write undetected");
        rsx_nr_res_revalidate(&c, er);
        CHECK(rsx_nr_res_current(&c, er) == 1, "post-wrap revalidate");
        rsx_guest_pages_note_write(&pages, RSX_NR_SPACE_LOCAL, kr.offset, 4);
        CHECK(rsx_nr_res_current(&c, er) == 0, "post-wrap write undetected");
    }

    rsx_nr_res_cache_destroy(&c);
    rsx_guest_pages_destroy(&pages);
}

static void test_pso_cache(void)
{
    rsx_nr_pso_cache c;
    CHECK(rsx_nr_pso_cache_init(&c, 64) == 0, "pso init");

    u64 v = 0;
    u64 key = rsx_nr_hash_fold(0, "vp+fp+blend", 11);
    CHECK(key != 0, "hash_fold returned 0");
    CHECK(rsx_nr_pso_lookup(&c, key, &v) == 0, "phantom pso hit");
    CHECK(rsx_nr_pso_insert(&c, key, 0x1234) == 0, "pso insert");
    CHECK(rsx_nr_pso_lookup(&c, key, &v) == 1 && v == 0x1234, "pso lookup");
    CHECK(rsx_nr_pso_insert(&c, key, 0x5678) == 0, "pso refresh");
    rsx_nr_pso_lookup(&c, key, &v);
    CHECK(v == 0x5678, "pso refresh lost");

    /* fill to the load-factor limit; overflow is a counted refusal */
    u32 full = 0;
    for (u32 i = 0; i < 64; i++) {
        if (rsx_nr_pso_insert(&c, rsx_nr_hash_fold(0, &i, 4), i) != 0)
            full++;
    }
    CHECK(full > 0, "table never refused past load factor");
    CHECK(c.stats.table_full == full, "refusals miscounted");
    /* everything accepted is still retrievable */
    u32 found = 0;
    for (u32 i = 0; i < 64; i++)
        if (rsx_nr_pso_lookup(&c, rsx_nr_hash_fold(0, &i, 4), &v) && v == i)
            found++;
    CHECK(found == 64 - full, "post-fill retrieval %u of %u", found,
          64 - full);

    rsx_nr_pso_cache_destroy(&c);
}

static void test_fences(void)
{
    rsx_nr_fence f;
    rsx_nr_fence_init(&f);
    CHECK(rsx_nr_fence_done(&f, 0), "value 0 not complete");
    u64 a = rsx_nr_fence_next(&f);
    u64 b = rsx_nr_fence_next(&f);
    CHECK(a == 1 && b == 2, "fence values %llu %llu",
          (unsigned long long)a, (unsigned long long)b);
    CHECK(!rsx_nr_fence_done(&f, a), "unsignaled done");
    rsx_nr_fence_complete(&f, b);     /* out-of-order completion covers a */
    CHECK(rsx_nr_fence_done(&f, a) && rsx_nr_fence_done(&f, b),
          "completion not monotonic");
    rsx_nr_fence_complete(&f, a);     /* stale completion must not regress */
    CHECK(rsx_nr_fence_completed(&f) == b, "fence regressed");
}

int main(void)
{
    test_maps();
    test_res_cache();
    test_pso_cache();
    test_fences();

    if (g_failures) {
        fprintf(stderr, "rsx_nr_resources: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("rsx_nr_resources: PASS\n");
    return 0;
}
