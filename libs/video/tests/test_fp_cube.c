/*
 * Real-capture cube-sampling compile check.
 *
 * Decompiles an actual RSX fragment program (the a010 draw-130 deferred-plane
 * shader, which samples a reflection CUBEMAP on texture unit 6) with the cube
 * dimensionality mask set, and D3DCompiles the generated HLSL to prove the
 * TextureCube path is valid pixel-shader source. Also compiles the baseline
 * (cube_mask=0, all Texture2D) form to prove the default is unaffected.
 *
 * Build (Windows / MSVC):
 *   cl /nologo /std:c17 /I..\..\..\include test_fp_cube.c ../rsx_fp_decompiler.c \
 *      /Fe:test_fp_cube.exe /link d3dcompiler.lib
 * Run:
 *   ./test_fp_cube.exe <path-to.fp> [cube-unit]
 *   (cube-unit defaults to 6)
 */
#include "../rsx_fp_decompiler.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <d3dcompiler.h>

static u8   filebuf[64 * 1024];
static char hlsl[256 * 1024];

static long read_file(const char* path, u8* buf, long cap)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    long n = (long)fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return n;
}

static int compile(const char* label, const char* src)
{
    ID3DBlob* code = NULL; ID3DBlob* err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), label, NULL, NULL,
                            "main", "ps_5_0", 0, 0, &code, &err);
    if (SUCCEEDED(hr)) {
        printf("[PASS] %s: D3DCompile OK (ps_5_0)\n", label);
        if (code) code->lpVtbl->Release(code);
        if (err) err->lpVtbl->Release(err);
        return 1;
    }
    printf("[FAIL] %s: D3DCompile failed: %s\n", label,
           err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
    if (err) err->lpVtbl->Release(err);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: %s <path-to.fp> [cube-unit]\n", argv[0]);
        return 2;
    }

    const char* path = argv[1];
    u32 cube_unit = (argc > 2) ? (u32)atoi(argv[2]) : 6u;

    long n = read_file(path, filebuf, (long)sizeof(filebuf));
    if (n < 16) { printf("cannot read %s\n", path); return 2; }
    u32 size = rsx_fp_program_size(filebuf, (u32)n);
    if (!size) size = (u32)n;

    int ok = 1;

    /* Baseline: all 2D (cube_mask=0). */
    int fi0 = rsx_fp_decompile_ex(filebuf, size, RSX_FP_CTRL_AUTO, 0u, hlsl, sizeof(hlsl));
    printf("-- baseline decompile: %d instr\n", fi0);
    ok &= compile("2d_baseline", hlsl);

    /* Cube: unit `cube_unit` is a TextureCube (draw-130 samples u6 with a
     * reflection direction). */
    u32 mask = (1u << cube_unit);
    int fi1 = rsx_fp_decompile_ex(filebuf, size, RSX_FP_CTRL_AUTO, mask, hlsl, sizeof(hlsl));
    printf("-- cube decompile (mask=0x%X): %d instr\n", mask, fi1);
    if (!strstr(hlsl, "TextureCube")) {
        printf("[FAIL] cube_decl: no TextureCube emitted\n"); ok = 0;
    } else {
        printf("[PASS] cube_decl: TextureCube emitted\n");
    }
    ok &= compile("cube_u6", hlsl);

    printf("\n=== %s ===\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
