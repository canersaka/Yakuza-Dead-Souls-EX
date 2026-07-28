#include "ps3emu/yz_runtime_config.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

int env_enabled(const char* name)
{
    return std::getenv(name) != nullptr ? 1 : 0;
}

int env_truthy(const char* name)
{
    const char* value = std::getenv(name);
    return value && value[0] && value[0] != '0' ? 1 : 0;
}

int env_integer(const char* name, int fallback)
{
    const char* value = std::getenv(name);
    return value && value[0]
        ? static_cast<int>(std::strtol(value, nullptr, 10))
        : fallback;
}

unsigned long long env_u64(
    const char* name, unsigned long long fallback)
{
    const char* value = std::getenv(name);
    return value && value[0]
        ? std::strtoull(value, nullptr, 10)
        : fallback;
}

yz_runtime_config read_runtime_config()
{
    unsigned long long lockstep_quantum =
        env_u64("YZ_LOCKSTEP_QUANTUM", 65536ull);
    if (lockstep_quantum < 1ull)
        lockstep_quantum = 1ull;
    return {
        env_enabled("YZ_MOVIE_HLE"),
        env_enabled("YZ_AUTO_START"),
        env_enabled("YZ_AUTO_NEW_GAME"),
        env_enabled("YZ_AUTO_NEW_GAME_CIRCLE"),

        env_enabled("YZ_SKIP_VOICE"),
        env_enabled("YZ_A010_RENDER_ENABLE"),
        env_enabled("YZ_A010_MODEL_RETRY"),
        env_enabled("YZ_A010_PALETTE_REPAIR"),
        env_enabled("YZ_A010_CAMERA_HANDOFF"),
        env_enabled("YZ_A010_STAGE_CLASSIFY_REPAIR"),
        env_enabled("YZ_A010_FORCE_STAGE_MESHES"),
        env_enabled("YZ_A010_FORCE_STAGE_MAIN"),
        env_enabled("YZ_A010_STAGE_GROUP_WAIT"),
        env_enabled("YZ_A010_START_REFERENCE"),
        env_enabled("YZ_A010_REFERENCE_874"),

        env_enabled("YZ_XF_IEEE"),
        env_enabled("YZ_NO_TASKRELOAD"),
        env_enabled("YZ_CTXSHADOW"),
        env_enabled("YZ_NO_CTXSHADOW"),
        env_enabled("YZ_NO_CTXSAVE"),
        env_enabled("YZ_NO_RESVSW"),
        env_integer("YZ_RESUME_HOLD", 0),
        env_enabled("YZ_NO_CTXRESTORE"),
        env_enabled("YZ_CH_NONBLOCK"),
        env_enabled("YZ_CH_STRICT"),
        env_enabled("YZ_NO_THROW_RETRY"),
        env_enabled("YZ_NOLAUNCH"),
        env_enabled("YZ_NO_MGMT"),
        env_enabled("YZ_FIXEXIT"),
        env_enabled("YZ_STARTTASK_HOOK"),
        env_enabled("YZ_JOB_WILDCARD_OK"),
        env_enabled("YZ_KERN_WILDCARD_OK"),
        env_truthy("YZ_SENTINEL_GUARD"),
        env_enabled("YZ_NO_IMGSTACK"),
        env_enabled("YZ_NO_MODRELOAD"),
        env_enabled("YZ_NO_LAUNCH_UNWIND"),
        env_enabled("YZ_NORESUME"),
        env_enabled("YZ_CORET_GEN"),
        env_enabled("YZ_CORET_LEGACY"),
        env_enabled("YZ_POLLFORCE"),
        env_enabled("YZ_NOHELPRET"),
        env_enabled("YZ_NO_RECWAIT"),
        env_enabled("YZ_NO_DMAGUARD"),
        env_enabled("YZ_NULLPAGE_FAIL"),
        env_enabled("YZ_NO_LLFAST"),
        env_enabled("YZ_NO_SPUBACKOFF"),
        env_enabled("YZ_NO_LRWAKE"),
        env_enabled("YZ_FRC"),
        env_enabled("YZ_FRC3"),
        env_enabled("YZ_FORCE_WID0"),
        env_enabled("YZ_FORCE_WID1"),
        env_enabled("YZ_CLEARRUN"),
        env_enabled("YZ_CLEARRUN3"),
        env_enabled("YZ_TAGREAD_REPAIR"),
        env_truthy("YZ_SPU_LOCKSTEP"),
        lockstep_quantum,
    };
}

void hash_byte(std::uint64_t& hash, unsigned char value)
{
    hash ^= value;
    hash *= UINT64_C(1099511628211);
}

void hash_text(std::uint64_t& hash, const char* text)
{
    while (*text)
        hash_byte(hash, static_cast<unsigned char>(*text++));
}

void hash_option(
    std::uint64_t& hash, const char* name,
    unsigned long long value)
{
    hash_text(hash, name);
    hash_byte(hash, '=');
    for (unsigned shift = 0; shift < 64; shift += 8)
        hash_byte(
            hash,
            static_cast<unsigned char>((value >> shift) & 0xFFu));
    hash_byte(hash, '\n');
}

} // namespace

extern "C" const yz_runtime_config g_yz_runtime_config =
    read_runtime_config();

extern "C" void yz_runtime_config_print_fingerprint(void)
{
    struct option_value {
        const char* name;
        unsigned long long value;
        unsigned long long default_value;
    };
    const option_value options[] = {
#define YZ_OPTION(field, name) \
        {name, (unsigned long long)g_yz_runtime_config.field, 0ull}
        YZ_OPTION(movie_hle, "YZ_MOVIE_HLE"),
        YZ_OPTION(auto_start, "YZ_AUTO_START"),
        YZ_OPTION(auto_new_game, "YZ_AUTO_NEW_GAME"),
        YZ_OPTION(auto_new_game_circle, "YZ_AUTO_NEW_GAME_CIRCLE"),
        YZ_OPTION(skip_voice, "YZ_SKIP_VOICE"),
        YZ_OPTION(a010_render_enable, "YZ_A010_RENDER_ENABLE"),
        YZ_OPTION(a010_model_retry, "YZ_A010_MODEL_RETRY"),
        YZ_OPTION(a010_palette_repair, "YZ_A010_PALETTE_REPAIR"),
        YZ_OPTION(a010_camera_handoff, "YZ_A010_CAMERA_HANDOFF"),
        YZ_OPTION(a010_stage_classify_repair,
                  "YZ_A010_STAGE_CLASSIFY_REPAIR"),
        YZ_OPTION(a010_force_stage_meshes,
                  "YZ_A010_FORCE_STAGE_MESHES"),
        YZ_OPTION(a010_force_stage_main, "YZ_A010_FORCE_STAGE_MAIN"),
        YZ_OPTION(a010_stage_group_wait, "YZ_A010_STAGE_GROUP_WAIT"),
        YZ_OPTION(a010_start_reference, "YZ_A010_START_REFERENCE"),
        YZ_OPTION(a010_reference_874, "YZ_A010_REFERENCE_874"),
        YZ_OPTION(xf_ieee, "YZ_XF_IEEE"),
        YZ_OPTION(no_taskreload, "YZ_NO_TASKRELOAD"),
        YZ_OPTION(ctxshadow, "YZ_CTXSHADOW"),
        YZ_OPTION(no_ctxshadow, "YZ_NO_CTXSHADOW"),
        YZ_OPTION(no_ctxsave, "YZ_NO_CTXSAVE"),
        YZ_OPTION(no_resvsw, "YZ_NO_RESVSW"),
        YZ_OPTION(resume_hold_ms, "YZ_RESUME_HOLD"),
        YZ_OPTION(no_ctxrestore, "YZ_NO_CTXRESTORE"),
        YZ_OPTION(ch_nonblock, "YZ_CH_NONBLOCK"),
        YZ_OPTION(ch_strict, "YZ_CH_STRICT"),
        YZ_OPTION(no_throw_retry, "YZ_NO_THROW_RETRY"),
        YZ_OPTION(nolaunch, "YZ_NOLAUNCH"),
        YZ_OPTION(no_mgmt, "YZ_NO_MGMT"),
        YZ_OPTION(fixexit, "YZ_FIXEXIT"),
        YZ_OPTION(starttask_hook, "YZ_STARTTASK_HOOK"),
        YZ_OPTION(job_wildcard_ok, "YZ_JOB_WILDCARD_OK"),
        YZ_OPTION(kern_wildcard_ok, "YZ_KERN_WILDCARD_OK"),
        YZ_OPTION(sentinel_guard, "YZ_SENTINEL_GUARD"),
        YZ_OPTION(no_imgstack, "YZ_NO_IMGSTACK"),
        YZ_OPTION(no_modreload, "YZ_NO_MODRELOAD"),
        YZ_OPTION(no_launch_unwind, "YZ_NO_LAUNCH_UNWIND"),
        YZ_OPTION(noresume, "YZ_NORESUME"),
        YZ_OPTION(coret_gen, "YZ_CORET_GEN"),
        YZ_OPTION(coret_legacy, "YZ_CORET_LEGACY"),
        YZ_OPTION(pollforce, "YZ_POLLFORCE"),
        YZ_OPTION(nohelpret, "YZ_NOHELPRET"),
        YZ_OPTION(no_recwait, "YZ_NO_RECWAIT"),
        YZ_OPTION(no_dmaguard, "YZ_NO_DMAGUARD"),
        YZ_OPTION(nullpage_fail, "YZ_NULLPAGE_FAIL"),
        YZ_OPTION(no_llfast, "YZ_NO_LLFAST"),
        YZ_OPTION(no_spubackoff, "YZ_NO_SPUBACKOFF"),
        YZ_OPTION(no_lrwake, "YZ_NO_LRWAKE"),
        YZ_OPTION(frc, "YZ_FRC"),
        YZ_OPTION(frc3, "YZ_FRC3"),
        YZ_OPTION(force_wid0, "YZ_FORCE_WID0"),
        YZ_OPTION(force_wid1, "YZ_FORCE_WID1"),
        YZ_OPTION(clearrun, "YZ_CLEARRUN"),
        YZ_OPTION(clearrun3, "YZ_CLEARRUN3"),
        YZ_OPTION(tagread_repair, "YZ_TAGREAD_REPAIR"),
        YZ_OPTION(spu_lockstep, "YZ_SPU_LOCKSTEP"),
        {"YZ_LOCKSTEP_QUANTUM",
         g_yz_runtime_config.lockstep_quantum, 65536ull},
#undef YZ_OPTION
    };

    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const option_value& option : options)
        hash_option(hash, option.name, option.value);

#if defined(YZ_PERF_CLEAN)
    const char* lane = "clean";
#else
    const char* lane = "diagnostic";
#endif
    std::fprintf(
        stderr,
        "[config] fingerprint=%016llX lane=%s "
        "required{YZ_MOVIE_HLE=%d,YZ_AUTO_START=%d,"
        "YZ_AUTO_NEW_GAME=%d} nondefault{",
        static_cast<unsigned long long>(hash), lane,
        g_yz_runtime_config.movie_hle,
        g_yz_runtime_config.auto_start,
        g_yz_runtime_config.auto_new_game);
    bool first = true;
    for (unsigned i = 3;
         i < sizeof(options) / sizeof(options[0]); ++i) {
        if (options[i].value == options[i].default_value)
            continue;
        std::fprintf(
            stderr, "%s%s=%llu", first ? "" : ",",
            options[i].name, options[i].value);
        first = false;
    }
    if (first)
        std::fputs("<none>", stderr);
    std::fputs("}\n", stderr);
    std::fflush(stderr);
}
