#ifndef YZ_NATIVE_SCENE_POSE_JOB_H
#define YZ_NATIVE_SCENE_POSE_JOB_H
#include "../libs/spurs/cellSpurs.h"
/* Native handler for the scene pose job (job binary 0x01254500). Prepare
 * snapshots inputs; execute publishes the result and completion synchronously.
 * One outstanding operation per thread. */
int yz_native_scene_pose_prepare(const cellSpursNativeJobRequest*,cellSpursNativeOperation*);
#endif
