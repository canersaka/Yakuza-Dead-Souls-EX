#include "rsx_nr_span_router.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { \
    fprintf(stderr, "FAIL:%d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); failures++; \
} } while (0)

static rsx_nr_span make_span(rsx_nr_span_router* router, u32 ea, u32 words,
                             u32 kind, u32 a, u32 b)
{
    rsx_nr_span span;
    memset(&span, 0, sizeof(span));
    span.ea = ea;
    span.word_count = words;
    span.generation = rsx_nr_span_router_generation(router);
    span.payload.op_count = 1;
    span.payload.ops[0].kind = kind;
    if (kind == RSX_NIR_OP_SET_REFERENCE)
        span.payload.ops[0].u.reference.value = a;
    else if (kind == RSX_NIR_OP_USER_COMMAND)
        span.payload.ops[0].u.user_command.cause = a;
    else if (kind == RSX_NIR_OP_SEMAPHORE_ACQUIRE) {
        span.payload.ops[0].u.semaphore.dma_context = 0x66616661;
        span.payload.ops[0].u.semaphore.offset = a;
        span.payload.ops[0].u.semaphore.value = b;
    }
    span.fingerprint = rsx_nr_span_fingerprint(&span);
    return span;
}

int main(void)
{
    rsx_nr_span_router router;
    CHECK(rsx_nr_span_router_init(&router, 8) == 0, "init");

    rsx_nr_span out;
    memset(&out, 0, sizeof(out));
    CHECK(rsx_nr_span_router_take(&router, 0x40400000, &out) ==
              RSX_NR_SPAN_TAKE_FAST_MISS,
          "unwatched address did not reject at the page count");

    rsx_nr_span ref = make_span(&router, 0x40401000, 2,
                                RSX_NIR_OP_SET_REFERENCE, 0xBEEF, 0);
    const u32 epoch_before =
        rsx_nr_span_router_publication_epoch(&router);
    CHECK(rsx_nr_span_router_publish(&router, &ref) == RSX_NR_SPAN_PUBLISHED,
          "reference publish");
    CHECK(rsx_nr_span_router_publication_epoch(&router) != epoch_before,
          "publication did not invalidate a cached miss");
    CHECK(rsx_nr_span_router_publish(&router, &ref) ==
              RSX_NR_SPAN_PUBLISH_DUPLICATE,
          "duplicate not refused");
    CHECK(rsx_nr_span_router_take(&router, 0x40401004, &out) ==
              RSX_NR_SPAN_TAKE_MISS,
          "same watched page false-positive claimed");
    CHECK(rsx_nr_span_router_take(&router, ref.ea, &out) ==
              RSX_NR_SPAN_TAKE_CLAIMED,
          "reference take");
    CHECK(out.word_count == 2 && out.payload.op_count == 1 &&
              out.payload.ops[0].kind == RSX_NIR_OP_SET_REFERENCE &&
              out.payload.ops[0].u.reference.value == 0xBEEF,
          "reference payload changed");
    CHECK(rsx_nr_span_router_take(&router, ref.ea, &out) ==
              RSX_NR_SPAN_TAKE_FAST_MISS,
          "last claim did not clear the exact page count");

    /* A retained claim remains exact-owned and keeps its page watched while
     * a consumer-side dependency is blocked. It cannot be duplicated,
     * claimed twice, retired twice, or confused with a stale token. */
    rsx_nr_span blocked = make_span(&router, 0x40401800, 10,
                                    RSX_NIR_OP_SEMAPHORE_ACQUIRE,
                                    0x230, 0xAABBCCDD);
    CHECK(rsx_nr_span_router_publish(&router, &blocked) ==
              RSX_NR_SPAN_PUBLISHED,
          "blocked publish");
    rsx_nr_span_claim retained = {0};
    CHECK(rsx_nr_span_router_claim(&router, blocked.ea, &out, &retained) ==
              RSX_NR_SPAN_TAKE_CLAIMED,
          "blocked retained claim");
    CHECK(retained.slot && retained.ea == blocked.ea &&
              retained.generation == blocked.generation &&
              retained.fingerprint == out.fingerprint,
          "retained token identity");
    rsx_nr_span_claim duplicate_claim = {0};
    CHECK(rsx_nr_span_router_claim(&router, blocked.ea, &out,
                                   &duplicate_claim) ==
              RSX_NR_SPAN_TAKE_MISS,
          "claimed span was claimable twice");
    CHECK(rsx_nr_span_router_publish(&router, &blocked) ==
              RSX_NR_SPAN_PUBLISH_DUPLICATE,
          "claimed exact address was silently replaced");
    rsx_nr_span_claim stale = retained;
    stale.fingerprint ^= 1u;
    CHECK(rsx_nr_span_router_retire(&router, &stale) != 0,
          "mismatched claim token retired live span");
    CHECK(rsx_nr_span_router_retire(&router, &retained) == 0,
          "retained retire");
    CHECK(rsx_nr_span_router_retire(&router, &retained) != 0,
          "retained double retire");
    CHECK(rsx_nr_span_router_take(&router, blocked.ea, &out) ==
              RSX_NR_SPAN_TAKE_FAST_MISS,
          "retained retire did not clear watched page");

    /* The page count remains watched until every exact span on that page is
     * retired, then returns to the one-load fast-miss path. */
    rsx_nr_span same_page_a = make_span(&router, 0x40402000, 2,
                                        RSX_NIR_OP_USER_COMMAND, 1, 0);
    rsx_nr_span same_page_b = make_span(&router, 0x40402008, 2,
                                        RSX_NIR_OP_USER_COMMAND, 2, 0);
    CHECK(rsx_nr_span_router_publish(&router, &same_page_a) ==
              RSX_NR_SPAN_PUBLISHED &&
              rsx_nr_span_router_publish(&router, &same_page_b) ==
              RSX_NR_SPAN_PUBLISHED,
          "same-page publishes");
    CHECK(rsx_nr_span_router_take(&router, same_page_a.ea, &out) ==
              RSX_NR_SPAN_TAKE_CLAIMED &&
              rsx_nr_span_router_take(&router, same_page_a.ea, &out) ==
              RSX_NR_SPAN_TAKE_MISS,
          "page count cleared before final exact span");
    CHECK(rsx_nr_span_router_take(&router, same_page_b.ea, &out) ==
              RSX_NR_SPAN_TAKE_CLAIMED &&
              rsx_nr_span_router_take(&router, same_page_b.ea, &out) ==
              RSX_NR_SPAN_TAKE_FAST_MISS,
          "page count remained sticky after final exact span");

    /* Tombstone reuse plus mixed typed actions. */
    rsx_nr_span acquire = make_span(&router, ref.ea, 4,
                                    RSX_NIR_OP_SEMAPHORE_ACQUIRE,
                                    0xFE0, 0x1234);
    CHECK(rsx_nr_span_router_publish(&router, &acquire) ==
              RSX_NR_SPAN_PUBLISHED,
          "tombstone reuse");
    CHECK(rsx_nr_span_router_take(&router, acquire.ea, &out) ==
              RSX_NR_SPAN_TAKE_CLAIMED &&
              out.payload.ops[0].u.semaphore.offset == 0xFE0 &&
              out.payload.ops[0].u.semaphore.value == 0x1234,
          "acquire payload");

    /* Old-generation records cannot survive a quiescent reset. */
    rsx_nr_span user = make_span(&router, 0x40500000, 2,
                                 RSX_NIR_OP_USER_COMMAND, 7, 0);
    CHECK(rsx_nr_span_router_publish(&router, &user) == RSX_NR_SPAN_PUBLISHED,
          "user publish");
    const u32 old_generation = user.generation;
    const u32 new_generation = rsx_nr_span_router_reset(&router);
    CHECK(new_generation && new_generation != old_generation,
          "generation did not advance");
    CHECK(rsx_nr_span_router_take(&router, user.ea, &out) ==
              RSX_NR_SPAN_TAKE_FAST_MISS,
          "reset left watched page/span live");
    CHECK(rsx_nr_span_router_publish(&router, &user) ==
              RSX_NR_SPAN_PUBLISH_INVALID,
          "stale generation accepted");

    /* Fill every slot without consuming; the ninth unique span must fail
     * closed instead of evicting a live command. */
    for (u32 i = 0; i < 8; ++i) {
        rsx_nr_span s = make_span(&router, 0x41000000 + i * 0x1000, 2,
                                  RSX_NIR_OP_USER_COMMAND, i, 0);
        CHECK(rsx_nr_span_router_publish(&router, &s) ==
                  RSX_NR_SPAN_PUBLISHED,
              "fill publish %u", i);
    }
    rsx_nr_span full = make_span(&router, 0x42000000, 2,
                                 RSX_NIR_OP_USER_COMMAND, 99, 0);
    CHECK(rsx_nr_span_router_publish(&router, &full) ==
              RSX_NR_SPAN_PUBLISH_FULL,
          "saturation did not fail closed");

    /* Payload fingerprints include address, generation, op data, and used
     * side words, but ignore unused fixed storage. */
    rsx_nr_span a = make_span(&router, 0x43000000, 2,
                              RSX_NIR_OP_USER_COMMAND, 1, 0);
    rsx_nr_span b = a;
    b.payload.side[31] = 0xDEADBEEF; /* unused */
    CHECK(rsx_nr_span_fingerprint(&a) == rsx_nr_span_fingerprint(&b),
          "unused storage changed fingerprint");
    b.payload.ops[0].u.user_command.cause = 2;
    CHECK(rsx_nr_span_fingerprint(&a) != rsx_nr_span_fingerprint(&b),
          "used payload missing from fingerprint");
    b = a;
    b.flags = RSX_NR_SPAN_RETAINED_FALLBACK;
    CHECK(rsx_nr_span_fingerprint(&a) != rsx_nr_span_fingerprint(&b),
          "fallback ownership guarantee missing from fingerprint");
    CHECK(rsx_nr_span_router_publish(&router, &b) ==
              RSX_NR_SPAN_PUBLISH_FULL,
          "valid retained-fallback flag rejected before capacity gate");
    b.flags = 0x80000000u;
    CHECK(rsx_nr_span_router_publish(&router, &b) ==
              RSX_NR_SPAN_PUBLISH_INVALID,
          "unknown ownership flag accepted");

    rsx_nr_span_router_stats stats;
    rsx_nr_span_router_get_stats(&router, &stats);
    CHECK(stats.published == 14 && stats.claimed == 5 &&
              stats.duplicates == 2 && stats.full == 2,
          "stats p=%llu c=%llu dup=%llu full=%llu",
          stats.published, stats.claimed, stats.duplicates, stats.full);

    rsx_nr_span_router_destroy(&router);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("rsx_nr_span_router: ok");
    return 0;
}
