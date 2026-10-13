#pragma once
#include "encoder_owner.h"
#include "input_buffer.h"
#include "input_mapping.h"
namespace ca_zt30 {
// One synchronous worker. RGB must own SYS/device0 before open(), and remain
// alive until close(). No SDK pointer or input lease crosses a submit call.
class ThermalEncoder {
public:
    ThermalEncoder(Api &sys,EncoderApi &encoder,PacketMemory &packets,InputBufferApi buffers);
    int open() { return output.open_thermal(); }
    InputBufferResult submit(const uint8_t*,size_t,uint64_t pts);
    int copy_frame(EncodedFrame &frame) { return output.copy_frame(frame); }
    int close() { return output.close(); }
    const char *failure() const { return output.failed_call; }
    int status() const { return output.raw_status; }
    int get_status() const { return input.get_status; }
    int put_status() const { return input.put_status; }
private:
    static thread_local ThermalEncoder *active;
    static int get(InputPort*,InputBufferConfig*,InputBufferInfo*,uint32_t*,int32_t);
    static int put(uint32_t,InputBufferInfo*,uint8_t);
    static bool map(uint32_t,size_t,uint8_t**);
    static bool copy(uint8_t*,size_t,const uint8_t*,size_t,size_t);
    static bool maps(std::vector<InputMapping>&);
    EncoderOwner output;
    InputBufferApi sdk;
    InputBufferOwner input;
    InputBufferInfo leased{};
    bool writable=false;
};
}
