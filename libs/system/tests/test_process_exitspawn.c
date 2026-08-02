#include "sysPrxForUser.h"

#include <stdio.h>

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr, "check failed: %s (%s:%d)\n",                  \
                    #expr, __FILE__, __LINE__);                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main(void)
{
    CHECK(sys_process_atexitspawn_callback() == 0);
    CHECK(sys_process_at_Exitspawn_callback() == 0);

    CHECK(_sys_process_atexitspawn(0x00123450u) == CELL_OK);
    CHECK(_sys_process_at_Exitspawn(0x006789a0u) == CELL_OK);
    CHECK(sys_process_atexitspawn_callback() == 0x00123450u);
    CHECK(sys_process_at_Exitspawn_callback() == 0x006789a0u);

    CHECK(_sys_process_atexitspawn(0x00abcdefu) == CELL_OK);
    CHECK(sys_process_atexitspawn_callback() == 0x00abcdefu);
    CHECK(sys_process_at_Exitspawn_callback() == 0x006789a0u);

    puts("process_exitspawn_test: PASS");
    return 0;
}
