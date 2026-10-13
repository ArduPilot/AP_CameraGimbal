/* Initial simulated control link. Native ZT30 routing/ABI must be qualified
 * independently; never deploy the A8 transport policy as a ZT30 driver. */
#if !defined(CAMERA_APP_SITL) && !defined(CAMERA_APP_HOST)
#error "ZT30 native control is not qualified; build zt30_sitl instead"
#endif
#include "../siyi/control.cpp"
