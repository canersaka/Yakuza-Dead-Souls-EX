#ifndef PS3EMU_YZ_WKL4_CYCLE_INTERVAL_H
#define PS3EMU_YZ_WKL4_CYCLE_INTERVAL_H

#ifdef __cplusplus
extern "C" {
#endif

void yz_wkl4_cycle_begin_interval(void);

#ifdef __cplusplus
}
#endif

#if defined(YZ_WKL4_CYCLE_DIAGNOSTIC)
#define YZ_WKL4_CYCLE_BEGIN_INTERVAL() yz_wkl4_cycle_begin_interval()
#else
#define YZ_WKL4_CYCLE_BEGIN_INTERVAL() ((void)0)
#endif

#endif
