#pragma once
#include "rgb_owner.h"
#include "camera_app/config.h"
#include "thermal_encoder.h"
#include "mi/native_sdk.h"
#include "camera_app/still.h"
#include "camera_app/raw_thermal.h"
#include "thermal_pixels.h"
#include <memory>
namespace ca_zt30 {
// Called synchronously by the app main loop for SIYI, MAVLink, UniGCS and web
// requests. Availability reads use that same thread; the live thermal worker
// touches only its own mailbox/encoder. An async capture API needs new locking.
// RGB device owners outlive this rig. JPEG channels are created once, receive
// one picture per request, and never destroy/recreate SDK cached channels.
class PhotoOwner {
public:
    PhotoOwner(NativeSdk &sdk,PacketMemory &memory):s(sdk),other(sdk.api),
        zoom(sdk.api,sdk.encoder,memory),wide(sdk.api,sdk.encoder,memory),
        thermal(sdk.api,sdk.encoder,memory,sdk.input,8,6) {}
    ~PhotoOwner() { close(); }
    int open(EncoderSource primary,bool (*gate)(void*,bool,unsigned),void *opaque);
    int capture(const char *root,ca_photo_scope scope,const uint8_t *thermal_frame,size_t frame_bytes,
                const timespec &thermal_at,ca_raw_thermal *raw);
    int close();
    bool available() const { return opened && reusable; }
    // Allocated before the thermal worker starts, reused for every request.
    uint8_t *snapshot_buffer() { return frame.get(); }
private:
    bool jpeg(EncoderOwner &owner,unsigned width,unsigned height);
    bool thermal_jpeg(const uint8_t *nv12);
    NativeSdk &s;RgbOwner other;EncoderOwner zoom,wide;ThermalEncoder thermal;
    bool ports[2]{},opened=false,reusable=true;
    std::unique_ptr<uint8_t[]> buffer,nv12,frame;
    std::unique_ptr<uint16_t[]> values;
    EncodedFrame copied{};
};
}
