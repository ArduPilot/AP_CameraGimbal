/* Continuous native development backend. Shares the normal app, transport and
 * qualified RGB/encoder owners; no probe files, client gates or vendor fallback.
 * Current scope: one fixed 720p RGB source, no actuator/thermal/recording support.
 */
#include "owned_media.h"
#include "zoom_feedback.h"
#include "thermal_file_test.h"
#include "thermal_live.h"
#include "photo_owner.h"
#include "mi/native_sdk.h"
#include "pts_clock.h"
#include "camera_app/rtsp.h"
#include "camera_app/live_video_server.h"
#include "camera_app/video_metadata.h"
#include "camera_app/log.h"
#include "camera_app/raw_thermal.h"
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <new>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace ca_zt30 {
static_assert(sizeof(uintptr_t)==4,"ZT30 native SDK requires ARM32");
static uint64_t monotonic_ns() {
    timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);
    return uint64_t(t.tv_sec)*1000000000ULL+uint64_t(t.tv_nsec);
}
static bool pointer_slot(const void *p,uint32_t *slot) {
    const auto address=reinterpret_cast<uintptr_t>(p);
    if(!address)return false;
    *slot=uint32_t(address);return true;
}
static bool copy_packet(uint32_t address,uint8_t *out,size_t length) {
    if(!address || !length || uint64_t(address)+length>UINT64_C(0x100000000))return false;
    FILE *maps=std::fopen("/proc/self/maps","r");if(!maps)return false;
    char line[512]{};bool valid=false;
    while(std::fgets(line,sizeof(line),maps)) {
        unsigned long first=0,last=0;char permission[5]{};
        if(std::sscanf(line,"%lx-%lx %4s",&first,&last,permission)==3 && permission[0]=='r' &&
           first<=address && uint64_t(address)+length<=last) { valid=true;break; }
    }
    std::fclose(maps);
    if(valid)std::memcpy(out,reinterpret_cast<const void*>(uintptr_t(address)),length);
    return valid;
}
static bool vendor_stopped() {
    // Development launcher must suspend/stop vendor owners first. Refuse a
    // concurrent vendor app before SDK constructors can open any MI devices.
    DIR *proc=opendir("/proc");if(!proc)return false;
    bool idle=true;
    while(auto *entry=readdir(proc)) {
        if(entry->d_name[0]<'0' || entry->d_name[0]>'9')continue;
        char path[320]{},name[512]{};
        std::snprintf(path,sizeof(path),"/proc/%s/exe",entry->d_name);
        const ssize_t n=readlink(path,name,sizeof(name)-1);
        if(n<0) { if(errno!=ENOENT && errno!=ESRCH) { idle=false;break; }continue; }
        if(n>=ssize_t(sizeof(name)-1)) { idle=false;break; }
        const char *base=std::strrchr(name,'/');base=base?base+1:name;
        if(!std::strncmp(base,"siyi_app",8)) { idle=false;break; }
        std::snprintf(path,sizeof(path),"/proc/%s/cmdline",entry->d_name);
        FILE *cmd=std::fopen(path,"r");
        if(!cmd) { if(errno!=ENOENT && errno!=ESRCH) { idle=false;break; }continue; }
        const size_t bytes=std::fread(name,1,sizeof(name)-1,cmd);
        const bool read_error=std::ferror(cmd);std::fclose(cmd);
        if(read_error) { idle=false;break; }
        for(size_t i=0;i<bytes;i++)if(!name[i])name[i]=' ';
        name[bytes]=0;
        if(std::strstr(name,"/customer/app_restore.sh")) {
            std::snprintf(path,sizeof(path),"/proc/%s/stat",entry->d_name);
            FILE *stat=std::fopen(path,"r");if(!stat) { idle=false;break; }
            const bool read=std::fgets(name,sizeof(name),stat)!=nullptr;std::fclose(stat);
            const char *end=read?std::strrchr(name,')'):nullptr;
            if(!end || end[1]!=' ' || (end[2]!='T' && end[2]!='t')) { idle=false;break; }
        }
    }
    closedir(proc);return idle;
}
struct Context {
    NativeSdk sdk; // context destroyed only after MediaBackend joins/unwinds
    Api &api=sdk.api;EncoderApi &encoder=sdk.encoder;
    PacketMemory memory{pointer_slot,copy_packet};
    std::unique_ptr<uint8_t[]> buffer{new(std::nothrow) uint8_t[4U*1024*1024]};
    ca_rtsp *rtsp=nullptr;
    ca_live_video_server *live=nullptr;
    PtsClock clock;
    std::unique_ptr<ThermalFileTest> thermal_test;
    std::unique_ptr<ThermalLive> thermal_live;
    std::unique_ptr<PhotoOwner> photos;
    EncoderSource primary_source=EncoderSource::Zoom;
    ThermalStartBudget photo_budget;bool photo_opened=false;
    int owner_lock=-1;
    unsigned rtsp_port=8554;
    ca_config settings{};
    std::string capture_root;
    ca_raw_thermal_server *raw_server=nullptr;ca_raw_thermal *raw_photo=nullptr;
    ca_thermal_stream *raw_stream=nullptr;
    ~Context() {
        ca_thermal_stream_close(raw_stream);ca_raw_thermal_server_close(raw_server);ca_raw_thermal_close(raw_photo);
        ca_live_video_server_close(live);ca_rtsp_close(rtsp);
        sdk.close(); // keep cross-process ownership through SDK destruction
        if(owner_lock>=0)close(owner_lock);
    }
    bool lock_owner() {
        owner_lock=::open("/tmp/apcg-zt30-media.lock",O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
        struct stat st{};
        return owner_lock>=0 && !fstat(owner_lock,&st) && S_ISREG(st.st_mode) &&
            !st.st_uid && st.st_nlink==1 && !(st.st_mode&0022) && !flock(owner_lock,LOCK_EX|LOCK_NB);
    }
    bool observe() {
        PtsObservation sample;
        const bool valid=observe_pts_clock(clock,[this](uint64_t *pts) { return sdk.current_pts(0,pts); },monotonic_ns,sample);
        if(!valid)ca_log("ZT30 presentation clock failed: raw=%d bracket_ns=%llu pts=%llu attempts=%u",
            sample.status,(unsigned long long)sample.bracket_ns,(unsigned long long)sample.pts,sample.attempts);
        return valid;
    }
    static bool isp_frames(uint64_t &frames,unsigned wanted_channel) {
        FILE *proc=std::fopen("/proc/mi_modules/mi_isp/mi_isp0","r");if(!proc)return false;
        char line[512]{};bool found=false;
        // Per-channel checkout counter, independently observed in mi_isp0.
        // Device-level FrameDoneCnt cannot qualify a second sensor.
        while(std::fgets(line,sizeof(line),proc)) {
            if(!std::strstr(line,"PreCnt") || !std::strstr(line,"checkout"))continue;
            for(unsigned row=0;row<32 && std::fgets(line,sizeof(line),proc);row++) {
                unsigned dev=0,ch=0;unsigned long long pre=0,enq=0,bar=0,in=0,out=0,deq=0,drop=0,ot=0;
                if(std::sscanf(line,"%u %u %llu %llu %llu %llu %llu %llu %llu %llu",
                    &dev,&ch,&pre,&enq,&bar,&in,&out,&deq,&drop,&ot)!=10)break;
                if(!dev && ch==wanted_channel) { frames=out;found=true;break; }
            }
            break;
        }
        std::fclose(proc);return found;
    }
    static bool gate(void *opaque,bool after_iq) {
        return gate_channel(opaque,after_iq,uint32_t(static_cast<Context*>(opaque)->primary_source));
    }
    static bool gate_channel(void *opaque,bool after_iq,unsigned channel) {
        (void)opaque;
        uint64_t first=0;bool sampled=false;
        const uint64_t deadline=monotonic_ns()+4000000000ULL;
        while(monotonic_ns()<deadline) {
            uint64_t frames=0;
            if(isp_frames(frames,channel)) {
                if(sampled && frames>=first && frames-first>=3) {
                    ca_log("ZT30 ISP channel=%u frames ready %s IQ: %llu -> %llu",
                        channel,after_iq?"after":"before",(unsigned long long)first,(unsigned long long)frames);
                    return true;
                }
                if(!sampled) { first=frames;sampled=true; }
            }
            timespec delay{0,50000000};nanosleep(&delay,nullptr);
        }
        ca_log("ZT30 ISP frame startup timed out %s IQ",after_iq?"after":"before");
        return false;
    }
    static bool vui(void*,const char*,const H264Vui&) { return true; }
    static bool open_sink(void *opaque) {
        auto &c=*static_cast<Context*>(opaque);
        if(ca_live_video_server_open(&c.live,8555)) {
            ca_log("ZT30 web video listener failed: errno=%d",errno);return false;
        }
        if(ca_live_video_server_configure(c.live,0,1280,720,30,true)) {
            ca_log("ZT30 web video configuration failed: errno=%d",errno);return false;
        }
        if(ca_rtsp_open(&c.rtsp,c.rtsp_port,"video1",CA_VIDEO_H264,30)) {
            ca_log("ZT30 RTSP listener failed: port=%u errno=%d",c.rtsp_port,errno);return false;
        }
        ca_rtsp_add_config_aliases(c.rtsp,&c.settings);
        return c.observe();
    }
    static bool frame(void *opaque,const EncodedFrame &f,uint64_t,uint64_t after) {
        auto &c=*static_cast<Context*>(opaque);
        if(!c.observe() || !f.packets)return false;
        uint64_t mapped=0;
        // Use receipt after the clock sample, not the earlier stream-copy call.
        if(!c.clock.frame(f.pts[0],monotonic_ns()/1000,mapped))return false;
        (void)after;
        for(unsigned i=1;i<f.packets;i++)if(f.pts[i]!=f.pts[0])return false;
        size_t offset=0,nal=0,length=0;bool key=false;
        while(ca_annexb_next(f.data,f.bytes,&offset,&nal,&length))
            if(length && (f.data[nal]&31)==5)key=true;
        // No-viewer RTSP publication may return -1; it is not an encoder fault.
        (void)ca_rtsp_push_video_timed(c.rtsp,0,f.data,f.bytes,key,0,mapped);
        return !ca_live_video_server_publish(c.live,0,f.data,f.bytes,mapped,key,0);
    }
    static bool drain(void*) { return true; }
};
class Backend final : public OwnedMediaBackend<Context> {
public:
    explicit Backend(std::unique_ptr<Context> c):OwnedMediaBackend<Context>(std::move(c)) {}
    ~Backend() override { if(shutdown())_exit(25); }
    int shutdown() {
        if(context->thermal_test)context->thermal_test->join();
        if(context->thermal_live)context->thermal_live->join();
        if(context->photos && context->photos->close())return EOWNERDEAD;
        if(context->photo_opened) { (void)context->photo_budget.reset();context->photo_opened=false; }
        return MediaBackend::shutdown();
    }
    bool photo_available() const override {
        // Both scopes include thermal; do not advertise working photos when
        // the required live source has failed or has never produced a frame.
        return context->photos && context->photo_opened && context->photos->available() &&
            context->thermal_live && context->thermal_live->ready();
    }
    int capture_photo(ca_photo_scope scope) override {
        if(!photo_available()) { errno=ENODATA;return -1; }
        auto *frame=context->photos->snapshot_buffer();timespec at{};
        if(!context->thermal_live->snapshot(frame,ThermalPixelFrameBytes,at))return -1;
        return context->photos->capture(context->capture_root.c_str(),scope,frame,ThermalPixelFrameBytes,at,context->raw_photo);
    }
    // SIYI/UniGCS use background module zoom even when wide is selected;
    // MAVLink selected-source geometry uses lens_zoom() and hfov().
    float zoom() const override { return zoom_feedback().get(monotonic_ns()/1000000); }
    float lens_zoom(enum ca_media_lens value) const override {
        return value==CA_MEDIA_LENS_WIDE ? 1 : value==CA_MEDIA_LENS_ZOOM ? zoom() : NAN;
    }
    float hfov(bool thermal) const override {
        return thermal ? 0 : zoom_feedback().hfov(lens()==CA_MEDIA_LENS_WIDE,monotonic_ns()/1000000);
    }
    unsigned frame_rate(bool thermal) const override {
        if(thermal)return context->thermal_live && context->thermal_live->ready()?15:0;
        return MediaBackend::frame_rate(false);
    }
    int configure_raw_thermal(const ca_config *settings) override {
        return ca_thermal_stream_configure(context->raw_stream,settings);
    }
    int set_inverted(bool inverted) override { if(inverted) { errno=ENOTSUP;return -1; }return 0; }
    int start(EncoderSource source) {
        MediaHooks h{context.get(),nullptr,Context::gate,Context::open_sink,Context::vui,Context::frame,Context::drain};
        context->primary_source=source;
        const int rc=initialize(h,context->buffer.get(),4U*1024*1024,source,MediaRun::Continuous);
        if(rc)return rc;
        if(ca_raw_thermal_open(&context->raw_photo,context->capture_root.c_str()))
            ca_log("ZT30 raw photo writer unavailable errno=%d",errno);
        if(ca_raw_thermal_server_open(&context->raw_server,context->capture_root.c_str(),7345,640,512))
            ca_log("ZT30 raw TCP unavailable errno=%d; video continues",errno);
        if(ca_thermal_stream_open(&context->raw_stream,context->rtsp_port,false,&context->settings))
            ca_log("ZT30 lossless stream unavailable errno=%d; video continues",errno);
        ca_raw_thermal_server_source_timeout(context->raw_server,1000);
        ca_thermal_stream_source_timeout(context->raw_stream,1000);
        const char *photos_off=std::getenv("CAMERA_APP_ZT30_PHOTOS_OFF");
        // A root-controlled persistent marker permits raw-only development
        // qualification without changing saved settings or the startup script.
        const bool disabled=(photos_off && !std::strcmp(photos_off,"1")) ||
            !access("/customer/camera-app/photos.disabled",F_OK);
        if(!disabled) {
            const char *directory="/tmp/apcg-zt30-photos";struct stat st{};
            const bool directory_ok=(!mkdir(directory,0700) || errno==EEXIST) &&
                !lstat(directory,&st) && S_ISDIR(st.st_mode) && !st.st_uid && (st.st_mode&0777)==0700;
            if(!context->raw_photo || !directory_ok || !context->photo_budget.begin("/tmp/apcg-zt30-photos/start-budget"))
                ca_log("ZT30 photos unavailable: startup budget/writer; RGB continues");
            else if(!context->sdk.resolve_jpeg() || !context->sdk.resolve_input())
                ca_log("ZT30 JPEG API unavailable; RGB continues");
            else {
                context->photos.reset(new(std::nothrow) PhotoOwner(context->sdk,context->memory));
                if(context->photos && !context->photos->open(source,Context::gate_channel,context.get()))
                    context->photo_opened=true;
            }
        }
        const char *test=std::getenv("CAMERA_APP_ZT30_THERMAL_TEST_INPUT");
        if(test && *test && context->sdk.resolve_input()) {
            context->thermal_test.reset(new(std::nothrow) ThermalFileTest(context->sdk,context->memory));
            if(!context->thermal_test || !context->thermal_test->start(test)) {
                ca_log("ZT30 thermal file test refused; RGB continues");context->thermal_test.reset();
            }
        }
        const char *live=std::getenv("CAMERA_APP_ZT30_THERMAL_RUN");
        const char *off=std::getenv("CAMERA_APP_ZT30_THERMAL_OFF");
        if((!test || !*test) && (!off || std::strcmp(off,"1")) && context->sdk.resolve_input()) {
            context->thermal_live.reset(new(std::nothrow) ThermalLive(context->sdk,context->memory,context->rtsp,context->live,context->raw_server,context->raw_stream));
            if(!context->thermal_live || !context->thermal_live->start(live && *live?live:nullptr)) {
                ca_log("ZT30 live thermal refused; RGB continues");context->thermal_live.reset();
            }
        }
        return 0;
    }
};
}
std::unique_ptr<APC_Media_Backend> APC_Media_Backend::create(const ca_media_config &config) {
    using namespace ca_zt30;
    if(!config.backend || std::strcmp(config.backend,"zt30") ||
       config.settings.main_resolution!=CA_VIDEO_720P || config.settings.main_codec!=CA_VIDEO_H264) {
        ca_log("ZT30 development backend requires main stream 1280x720 H264");errno=ENOTSUP;return nullptr;
    }
    if(!vendor_stopped()) { ca_log("ZT30 vendor application still owns media; refusing startup");errno=EBUSY;return nullptr; }
    const char *selected=std::getenv("CAMERA_APP_ZT30_SOURCE");
    if(selected && std::strcmp(selected,"zoom") && std::strcmp(selected,"wide")) { errno=EINVAL;return nullptr; }
    const EncoderSource source=selected && !std::strcmp(selected,"wide")?EncoderSource::Wide:EncoderSource::Zoom;
    auto c=std::unique_ptr<Context>(new(std::nothrow) Context);
    if(!c || !c->buffer) { errno=ENOMEM;return nullptr; }
    if(!c->lock_owner()) { ca_log("Another AP media owner is active or lock is invalid");errno=EBUSY;return nullptr; }
    if(!config.capture_root || !*config.capture_root) { errno=EINVAL;return nullptr; }
    c->capture_root=config.capture_root;
    c->settings=config.settings;c->rtsp_port=config.rtsp_port;
    const char *sdk=std::getenv("CAMERA_APP_ZT30_SDK");
    if(!sdk || !*sdk)sdk="/customer/camera-app/encoder-sdk.so";
    // Partial dlopen/init can retain SDK callbacks: terminate without dlclose.
    if(!c->sdk.open(sdk) || !c->sdk.resolve_encoder() || !c->sdk.resolve_h264_getter() || !c->sdk.resolve_clock())_exit(4);
    auto backend=std::unique_ptr<Backend>(new(std::nothrow) Backend(std::move(c)));
    if(!backend)_exit(20);
    if(backend->start(source)) {
        ca_log("ZT30 media startup failed: %s raw=%d",backend->failed_call()?backend->failed_call():"unknown",backend->raw_status());
        if(backend->shutdown())_exit(25);
        return nullptr;
    }
    ca_log("ZT30 continuous RGB ready: source=%s 1280x720 H264; native gimbal and optional live thermal",source==EncoderSource::Wide?"wide":"zoom");
    return std::unique_ptr<APC_Media_Backend>(backend.release());
}
