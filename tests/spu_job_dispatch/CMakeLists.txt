add_executable(spu_job_dispatch_test main.c)

if(MSVC)
    target_compile_options(spu_job_dispatch_test PRIVATE /W4)
else()
    target_compile_options(spu_job_dispatch_test PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME spu_job_dispatch COMMAND spu_job_dispatch_test)
set_tests_properties(
    spu_job_dispatch
    PROPERTIES
        LABELS "unit;spu;renderer"
        TIMEOUT 30
)
