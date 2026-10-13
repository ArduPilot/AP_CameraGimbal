#pragma once
#include "thermal_encoder.h"
#include "thermal_start_budget.h"
#include "mi/native_sdk.h"
#include "usb_iso.h"
#include "uvc_frame.h"
#include "pts_clock.h"
#include "camera_app/live_video_server.h"
#include "camera_app/rtsp.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <pthread.h>
namespace ca_zt30 {
// USB only assembles/copies into a one-frame mailbox;
// the separate encoder worker converts/submits/publishes and drops stale input.
// RGB owns SYS/device0 for the entire lifetime. join() precedes RGB teardown.
class ThermalLive {
public:
    ThermalLive(NativeSdk&,PacketMemory&,ca_rtsp*,ca_live_video_server*);
    ~ThermalLive() { join(); }
    // nullptr selects ordinary startup; a name enables one-shot diagnostic capture.
    bool start(const char *run_name=nullptr);
    bool ready() const;
    void join();
private:
    static uint64_t now(void* =nullptr);
    static void pause(void*,unsigned);
    static int ioctl_call(void*,int,unsigned long,void*);
    static int close_call(void*,int);
    static bool frame(void*,const uint8_t*,size_t,const UvcFrameInfo&);
    static bool packet(void*,const IsoPacket&);
    static int open_device();
    bool initialize(const char *name);
    void capture();
    void encode();
    NativeSdk &sdk;ThermalEncoder encoder;
    ca_rtsp *rtsp;ca_live_video_server *live;unsigned stream_id=0;
    std::unique_ptr<UsbIso> usb;
    UvcFrame assembler;
    std::unique_ptr<uint8_t[]> latest,owned,nv12,encoded;
    std::mutex lock;uint64_t receipt=0,generation=0;
    pthread_t start_thread{},usb_thread{},encode_thread{};
    bool start_started=false;
    char run_name[49]{};
    bool usb_started=false,encode_started=false;
    std::atomic<bool> stop{false},usb_failed{false},degraded{false};
    PtsClock clock;
    ThermalStartBudget budget;
    bool normal=false;
    std::atomic<uint64_t> published_ns{0};
    char raw_sample[128]{};
};
}
