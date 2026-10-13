#include "../camera_app/src/backends/zt30/pts_clock.h"
#include <cassert>
using ca_zt30::PtsClock;
int main() {
    const uint64_t seed=UINT64_C(0xe1237890f7e21ce3), ns=UINT64_C(2000000000000);
    uint64_t out=99;
    PtsClock c;
    assert(c.observe(seed,ns,ns+100000));
    assert(c.frame(seed-33333,ns/1000+50,out) && out==ns/1000+50-33333);
    assert(c.observe(seed+33333,ns+33333000,ns+33433000));
    assert(c.frame(seed,ns/1000+33383,out) && out==ns/1000+50);
    assert(c.frame(seed+1,ns/1000+33383,out) && out==ns/1000+51);
    assert(!c.frame(seed+1,ns/1000+33383,out) && out==0 && !c.healthy());
    assert(!c.observe(seed+66666,ns+66666000,ns+66766000));
    PtsClock reset;assert(reset.observe(seed,ns,ns));
    assert(!reset.observe(0,ns+1000000,ns+1000000));
    PtsClock jumped;assert(jumped.observe(seed,ns,ns));
    assert(!jumped.observe(seed+1000000,ns+1000000,ns+1000000));
    PtsClock stale;assert(stale.observe(seed,ns,ns));
    assert(!stale.frame(seed-500001,ns/1000,out));
    PtsClock future;assert(future.observe(seed,ns,ns));
    assert(!future.frame(seed+2001,ns/1000,out));
    PtsClock underflow;assert(underflow.observe(9999999,1000,1000));
    assert(!underflow.frame(0,1,out));
    PtsClock overflow;assert(overflow.observe(0,ns,ns));
    assert(!overflow.frame(UINT64_MAX,ns/1000,out));
    PtsClock wrap;assert(wrap.observe(UINT64_MAX-10,ns,ns));
    assert(!wrap.observe(10,ns+20000,ns+20000));
    PtsClock boundary;assert(boundary.observe(seed,ns,ns+2000000));
    assert(boundary.frame(seed-500000,ns/1000+1000,out)); // exactly500ms
    PtsClock future_edge;assert(future_edge.observe(seed,ns,ns));
    assert(future_edge.frame(seed+2000,ns/1000,out));
    PtsClock tolerance;assert(tolerance.observe(seed,ns,ns));
    assert(tolerance.observe(seed+5001,ns+1000,ns+1000)); // exactly5ms
    PtsClock equal;assert(equal.observe(seed,ns,ns));
    assert(!equal.observe(seed,ns+1000,ns+1000));
    PtsClock slow;assert(!slow.observe(seed,ns,ns+2000001));
    PtsClock reverse;assert(!reverse.observe(seed,ns+1,ns));
    PtsClock uncalibrated;assert(!uncalibrated.frame(seed,ns/1000,out));
    PtsClock drift;assert(drift.observe(seed,ns,ns));
    assert(drift.observe(seed+1001000,ns+1000000000,ns+1000000000));
    assert(!drift.observe(seed+2010000,ns+2000000000,ns+2000000000));
}
