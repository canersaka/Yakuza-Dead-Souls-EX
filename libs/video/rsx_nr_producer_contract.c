#include "rsx_nr_producer_contract.h"

/* Audited directly against the lifted wrapper bodies at checkpoint 311410a.
 * Every entry copies gpr4 unchanged into its sole packet argument.  Wrappers
 * that pack multiple arguments, floats, reports, transfers, or synchronization
 * are intentionally absent until they receive their own typed contract. */
static const rsx_nr_direct_setter_contract g_direct_setters[] = {
    {0x00EBC488u, 0x1D8Cu},
    {0x00EBC51Cu, 0x1830u},
    {0x00EBC664u, 0x1834u},
    {0x00EBC6F8u, 0x03B8u},
    {0x00EBC790u, 0x03BCu},
    {0x00EBC824u, 0x1DB4u},
    {0x00EBC8B8u, 0x0378u},
    {0x00EBCA00u, 0x1838u},
    {0x00EBCA94u, 0x147Cu},
    {0x00EBCB28u, 0x1828u},
    {0x00EBCBBCu, 0x182Cu},
    {0x00EBCC50u, 0x032Cu},
    {0x00EBCCE4u, 0x034Cu},
    {0x00EBCD78u, 0x183Cu},
    {0x00EBCE0Cu, 0x0380u},
    {0x00EBCEA0u, 0x0A68u},
    {0x00EBCF34u, 0x1DACu},
    {0x00EBCFC8u, 0x1DB0u},
    {0x00EBD05Cu, 0x1FC0u},
    {0x00EBD0F4u, 0x1FF8u},
    {0x00EBD188u, 0x1FF0u},
    {0x00EBD220u, 0x1FECu},
    {0x00EBD3F8u, 0x17CCu},
    {0x00EBD48Cu, 0x17C8u},
    {0x00EBD5CCu, 0x1804u},
};

u32 rsx_nr_direct_setter_count(void)
{
    return (u32)(sizeof(g_direct_setters) / sizeof(g_direct_setters[0]));
}

const rsx_nr_direct_setter_contract*
rsx_nr_direct_setter_by_function(u32 function_ea)
{
    for (u32 i = 0; i < rsx_nr_direct_setter_count(); ++i) {
        if (g_direct_setters[i].function_ea == function_ea)
            return &g_direct_setters[i];
    }
    return 0;
}

const rsx_nr_direct_setter_contract*
rsx_nr_direct_setter_by_method(u32 method)
{
    for (u32 i = 0; i < rsx_nr_direct_setter_count(); ++i) {
        if (g_direct_setters[i].method == method)
            return &g_direct_setters[i];
    }
    return 0;
}

int rsx_nr_direct_setter_packet(u32 function_ea, u32 value, u32 out[2])
{
    const rsx_nr_direct_setter_contract* const contract =
        rsx_nr_direct_setter_by_function(function_ea);
    if (!contract || !out)
        return 0;
    out[0] = (1u << 18) | contract->method;
    out[1] = value;
    return 1;
}
