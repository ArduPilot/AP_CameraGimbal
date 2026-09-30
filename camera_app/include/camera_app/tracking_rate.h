#pragma once
#include <cmath>

// A rate below the motor's minimum cannot be held continuously. Carry the
// unsent angle between control ticks and alternate zero/minimum-rate commands
// instead of building a controller integral against a stationary motor.
class ca_tracking_rate_pulses {
public:
    void reset() { _angle=0; _direction=0; }
    float apply(float rate, float minimum, float dt) {
        if(!std::isfinite(rate) || !std::isfinite(dt) || dt<=0 || dt>.1f) {
            reset(); return 0;
        }
        if(rate==0 || !(minimum>0) || std::fabs(rate)>=minimum) {
            reset(); return rate;
        }
        const int direction=rate>0 ? 1 : -1;
        if(direction!=_direction) reset();
        _direction=direction;
        _angle+=std::fabs(rate)*dt;
        const float quantum=minimum*dt;
        if(_angle>=quantum*.5f) {
            _angle-=quantum;
            return direction*minimum;
        }
        return 0;
    }
private:
    float _angle=0;
    int _direction=0;
};
