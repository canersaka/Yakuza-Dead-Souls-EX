#ifndef YZ_IMAGE4_MLAA_REUSE_H
#define YZ_IMAGE4_MLAA_REUSE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Returns nonzero when the Image4 MLAA phase can reuse a previous result.
 * Called only from the four original phase entry points; it does not consume
 * signals, modify guest memory, publish completion or bypass task barriers. */
int yz_image4_mlaa_reuse(const uint8_t* ls,uint32_t parameter_lsa,
                       uint32_t parameter_ea,unsigned phase);
#ifdef __cplusplus
}
#endif
#endif
