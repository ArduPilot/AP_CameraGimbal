#include "thermal_encoder.h"
#include <cstring>
namespace ca_zt30 {
thread_local ThermalEncoder *ThermalEncoder::active=nullptr;
ThermalEncoder::ThermalEncoder(Api &sys,EncoderApi &encoder,PacketMemory &packets,InputBufferApi buffers):
    output(sys,encoder,packets),sdk(buffers),input({get,put},{map,copy},{{2,0,2,0}}) {}
bool ThermalEncoder::maps(std::vector<InputMapping> &out) {
    FILE *file=std::fopen("/proc/self/maps","r");if(!file)return false;
    bool ok=read_input_mappings(file,out);std::fclose(file);return ok;
}
int ThermalEncoder::get(InputPort *port,InputBufferConfig *config,InputBufferInfo *info,uint32_t *handle,int32_t timeout) {
    auto &self=*active;self.writable=false;
    std::vector<InputMapping> before,leased;
    if(!self.sdk.get || !self.sdk.put || !maps(before))return -1;
    const int rc=self.sdk.get(port,config,info,handle,timeout);
    if(rc)return rc; // failed outputs must never be inspected/released
    self.leased=*info;
    self.writable=maps(leased) && new_input_mapping(before,leased,*info);
    return rc; // validation failure is discarded exactly once by InputBufferOwner
}
int ThermalEncoder::put(uint32_t handle,InputBufferInfo *info,uint8_t flag) {
    auto &self=*active;self.writable=false;
    return self.sdk.put(handle,info,flag);
}
bool ThermalEncoder::map(uint32_t address,size_t bytes,uint8_t **pointer) {
    auto &self=*active;uint32_t allocation=0;
    std::memcpy(&allocation,reinterpret_cast<const uint8_t*>(&self.leased)+108,4);
    if(sizeof(uintptr_t)!=4 || !self.writable || address!=self.leased.plane_address[0] || bytes!=allocation)return false;
    *pointer=reinterpret_cast<uint8_t*>(uintptr_t(address));return true;
}
bool ThermalEncoder::copy(uint8_t *destination,size_t stride,const uint8_t *source,size_t bytes,size_t rows) {
    for(size_t row=0;row<rows;++row)std::memcpy(destination+row*stride,source+row*bytes,bytes);
    return true;
}
InputBufferResult ThermalEncoder::submit(const uint8_t *frame,size_t bytes,uint64_t pts) {
    if(active || !output.ready())return InputBufferResult::Unhealthy;
    active=this;
    const auto result=input.submit(frame,bytes,pts,100);
    active=nullptr;return result;
}
}
