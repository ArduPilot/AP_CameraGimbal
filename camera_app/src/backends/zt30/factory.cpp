/* Continuous native development backend. Shares the normal app, transport and
 * qualified RGB/encoder owners; no probe files, client gates or vendor fallback.
 * Current scope: one fixed 720p RGB source, no actuator/thermal/recording support.
 */
#include "owned_media.h"
#include "zoom_feedback.h"
#include "thermal_file_test.h"
#include "thermal_live.h"
#include "mi/native_sdk.h"
#include "pts_clock.h"
#include "camera_app/rtsp.h"
#include "camera_app/live_video_server.h"
#include "camera_app/video_metadata.h"
#include "camera_app/log.h"
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
    int owner_lock=-1;
    unsigned rtsp_port=8554;
    ca_config settings{};
    ~Context() {
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
        uint64_t pts=0;const auto before=monotonic_ns();
        const int rc=sdk.current_pts(0,&pts);const auto after=monotonic_ns();
        const bool valid=!rc && clock.observe(pts,before,after);
        if(!valid)ca_log("ZT30 presentation clock failed: raw=%d bracket_ns=%llu pts=%llu",
            rc,(unsigned long long)(after-before),(unsigned long long)pts);
        return valid;
    }
    static bool isp_frames(uint64_t &frames) {
        FILE *proc=std::fopen("/proc/mi_modules/mi_isp/mi_isp0","r");
        if(!proc)return false;
        char line[512]{};bool found=false;
        // The MI create/start calls return before the ISP hardware exists.
        // Wait for actual frame completions before the IQ API is safe to use.
        for(unsigned i=0;i<2048 && std::fgets(line,sizeof(line),proc);i++) {
            if(!std::strstr(line,"CreChnNum") || !std::strstr(line,"FrameDoneCnt"))continue;
            unsigned dev=0,channels=0,mask=0,enabled=0,mode=0,num=0;
            unsigned long long cmdq=0,vsync=0,done=0,dropped=0;
            if(std::fgets(line,sizeof(line),proc) &&
               std::sscanf(line,"%u %u %u %llx %u %u %u %llu %llu %llu",
                    &dev,&channels,&mask,&cmdq,&enabled,&mode,&num,&vsync,&done,&dropped)==10 &&
               dev==0 && channels==1 && mask==1 && enabled==1) {
                frames=done;found=true;
            }
            break;
        }
        std::fclose(proc);return found;
    }
    static bool gate(void*,bool after_iq) {
        uint64_t first=0;bool sampled=false;
        const uint64_t deadline=monotonic_ns()+4000000000ULL;
        while(monotonic_ns()<deadline) {
            uint64_t frames=0;
            if(isp_frames(frames)) {
                if(sampled && frames>=first && frames-first>=3) {
                    ca_log("ZT30 ISP frames ready %s IQ: %llu -> %llu",
                        after_iq?"after":"before",(unsigned long long)first,(unsigned long long)frames);
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
    ~Backend() override { if(context->thermal_test)context->thermal_test->join();if(context->thermal_live)context->thermal_live->join(); }
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
    int set_inverted(bool inverted) override { if(inverted) { errno=ENOTSUP;return -1; }return 0; }
    int start(EncoderSource source) {
        MediaHooks h{context.get(),nullptr,Context::gate,Context::open_sink,Context::vui,Context::frame,Context::drain};
        const int rc=initialize(h,context->buffer.get(),4U*1024*1024,source,MediaRun::Continuous);
        if(rc)return rc;
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
            context->thermal_live.reset(new(std::nothrow) ThermalLive(context->sdk,context->memory,context->rtsp,context->live));
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
