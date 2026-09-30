#ifndef PS3EMU_YZ_THREAD_PLACEMENT_H
#define PS3EMU_YZ_THREAD_PLACEMENT_H

#include <stdint.h>
#include <stddef.h>

typedef struct yz_cpu_core {
    unsigned logical, core, efficiency, unavailable;
} yz_cpu_core;

/* Select two different physical cores in the fastest eligible class. Never
 * interpret an SMT sibling as another core, or exceed the process's mask. */
static inline int yz_select_critical_cores(const yz_cpu_core* cpus, size_t count,
                                           uint64_t allowed, unsigned out[2])
{
    unsigned best = 0, found = 0, first = 64, second = 64, first_core = 0;
    for (size_t i = 0; i < count; ++i) {
        const yz_cpu_core* c = &cpus[i];
        if (c->unavailable || c->logical >= 64 || !(allowed & (UINT64_C(1) << c->logical))) continue;
        if (!found || c->efficiency > best) { best = c->efficiency; found = 1; }
    }
    if (!found) return 0;
    for (size_t i = 0; i < count; ++i) {
        const yz_cpu_core* c = &cpus[i];
        if (c->unavailable || c->logical >= 64 || c->efficiency != best ||
            !(allowed & (UINT64_C(1) << c->logical))) continue;
        if (c->logical < first) { first = c->logical; first_core = c->core; }
    }
    for (size_t i = 0; i < count; ++i) {
        const yz_cpu_core* c = &cpus[i];
        if (c->unavailable || c->logical >= 64 || c->efficiency != best || c->core == first_core ||
            !(allowed & (UINT64_C(1) << c->logical))) continue;
        if (c->logical < second) second = c->logical;
    }
    if (first == 64 || second == 64) return 0;
    out[0] = first; out[1] = second;
    return 1;
}

#if defined(_WIN32)
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Enabled only by YZ_SEPARATE_CRITICAL_CORES=1. Roles 0/1 are the RSX
 * consumer and native GS worker. Only the calling thread changes; no process,
 * guest or OS power policy does. Restricted to one processor group. */
static inline void yz_place_critical_thread(unsigned role)
{
    const char* flag = getenv("YZ_SEPARATE_CRITICAL_CORES");
    if (!flag || strcmp(flag, "1") || role > 1) return;
    DWORD_PTR allowed = 0, system = 0;
    ULONG bytes = 0;
    unsigned selected[2];
    yz_cpu_core cpus[256];
    size_t count = 0;
    if (GetActiveProcessorGroupCount() != 1 ||
        !GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system)) goto unavailable;
    GetSystemCpuSetInformation(NULL, 0, &bytes, GetCurrentProcess(), 0);
    if (!bytes || bytes > 65536) goto unavailable;
    {
        unsigned char* buffer = (unsigned char*)malloc(bytes);
        if (!buffer) goto unavailable;
        if (!GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buffer,
                bytes, &bytes, GetCurrentProcess(), 0)) { free(buffer); goto unavailable; }
        ULONG offset = 0;
        while (offset < bytes) {
            const SYSTEM_CPU_SET_INFORMATION* info = (const SYSTEM_CPU_SET_INFORMATION*)(buffer + offset);
            if (bytes - offset < 8 || info->Size < 8 || info->Size > bytes - offset) {
                free(buffer); goto unavailable;
            }
            if (info->Type == CpuSetInformation) {
                if (info->Size < sizeof(*info) || count == 256 || info->CpuSet.Group != 0) {
                    free(buffer); goto unavailable;
                }
                cpus[count].logical = info->CpuSet.LogicalProcessorIndex;
                cpus[count].core = info->CpuSet.CoreIndex;
                cpus[count].efficiency = info->CpuSet.EfficiencyClass;
                cpus[count].unavailable = info->CpuSet.Parked ||
                    (info->CpuSet.Allocated && !info->CpuSet.AllocatedToTargetProcess);
                ++count;
            }
            offset += info->Size;
        }
        free(buffer);
    }
    if (!yz_select_critical_cores(cpus, count, (uint64_t)allowed, selected)) goto unavailable;
    {
        const DWORD_PTR mask = (DWORD_PTR)1 << selected[role];
        const DWORD_PTR previous = SetThreadAffinityMask(GetCurrentThread(), mask);
        fprintf(stderr, "[critical-cores] role=%u tid=%lu cpu=%u mask=0x%llX previous=0x%llX applied=%u\n",
            role, GetCurrentThreadId(), selected[role], (unsigned long long)mask,
            (unsigned long long)previous, previous != 0);
    }
    return;
unavailable:
    fprintf(stderr, "[critical-cores] role=%u tid=%lu applied=0 topology-unavailable\n", role, GetCurrentThreadId());
}
#else
static inline void yz_place_critical_thread(unsigned role) { (void)role; }
#endif
#endif
