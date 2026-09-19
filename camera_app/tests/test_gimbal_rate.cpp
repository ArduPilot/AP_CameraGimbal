#include "apcam/target.h"
#include "camera_app/gimbal_rate.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    const float pitch[] = APCAM_VENDOR_PITCH_RATE_CURVE;
    const float yaw[] = APCAM_VENDOR_YAW_RATE_CURVE;
#define COMMAND(rate, curve) ca_gimbal_rate_command(rate, curve, sizeof(curve)/sizeof(curve[0]))
    assert(COMMAND(0, pitch) == 0);
    assert(COMMAND(2, pitch) == 0);
    assert(COMMAND(-2, pitch) == 0);
    assert(COMMAND(3, pitch) == 6);
    assert(COMMAND(-3, pitch) == -6);
    assert(COMMAND(10, pitch) == 14);
    assert(COMMAND(-10, pitch) == -14);
    assert(COMMAND(10, yaw) == 12);
    assert(COMMAND(-10, yaw) == -14);
    assert(COMMAND(200, yaw) == 100);
    assert(COMMAND(-200, yaw) == -100);
    puts("A8 signed rate conversion and dead zone passed");
    return 0;
}
