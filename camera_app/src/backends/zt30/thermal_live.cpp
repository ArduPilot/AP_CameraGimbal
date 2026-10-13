#include "thermal_live.h"
#include "thermal_pixels.h"
#include "camera_app/log.h"
#include "camera_app/video_metadata.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <new>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <ctime>
#include <unistd.h>
namespace ca_zt30 {
[[noreturn]] static void fatal(const char *call,int status=0) {
    ca_log("ZT30 live thermal fatal call=%s raw=%d errno=%d",call,status,errno);_exit(26);
}
uint64_t ThermalLive::now(void*) { timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec; }
void ThermalLive::pause(void*,unsigned ms) { timespec t{time_t(ms/1000),long(ms%1000)*1000000};nanosleep(&t,nullptr); }
int ThermalLive::ioctl_call(void*,int fd,unsigned long cmd,void *arg) { return ioctl(fd,cmd,arg); }
int ThermalLive::close_call(void*,int fd) { return ::close(fd); }
ThermalLive::ThermalLive(NativeSdk &s,PacketMemory &p,ca_rtsp *r,ca_live_video_server *l):
    sdk(s),encoder(s.api,s.encoder,p,s.input),rtsp(r),live(l),assembler(ThermalFrameBytes,frame,this),
    latest(new(std::nothrow) uint8_t[ThermalFrameBytes]),owned(new(std::nothrow) uint8_t[ThermalFrameBytes]),
    nv12(new(std::nothrow) uint8_t[ThermalNv12Bytes]),encoded(new(std::nothrow) uint8_t[4*1024*1024]) {}
static bool sys_number(const char *directory,const char *name,unsigned &number,unsigned base) {
    char path[320],text[40]{};std::snprintf(path,sizeof(path),"%s/%s",directory,name);
    FILE *f=std::fopen(path,"r");if(!f)return false;
    const bool ok=std::fgets(text,sizeof(text),f)!=nullptr;std::fclose(f);
    if(!ok)return false;
    char *end=nullptr;const unsigned long value=std::strtoul(text,&end,base);
    if(end==text || (*end!='\n' && *end) || value>65535)return false;
    number=unsigned(value);return true;
}
int ThermalLive::open_device() {
    DIR *dir=opendir("/sys/bus/usb/devices");if(!dir)return -1;
    int fd=-1;unsigned matches=0;
    while(auto *e=readdir(dir)) {
        if(e->d_name[0]=='.' || std::strchr(e->d_name,':'))continue;
        char path[320];std::snprintf(path,sizeof(path),"/sys/bus/usb/devices/%s",e->d_name);
        unsigned vendor=0,product=0,bus=0,device=0;
        if(!sys_number(path,"idVendor",vendor,16) || !sys_number(path,"idProduct",product,16) || vendor!=0x0bda || product!=0x5830)continue;
        ++matches;
        if(!sys_number(path,"busnum",bus,10) || !sys_number(path,"devnum",device,10) || !bus || bus>999 || !device || device>127)continue;
        char usb_path[80];std::snprintf(usb_path,sizeof(usb_path),"/dev/bus/usb/%03u/%03u",bus,device);
        int candidate=::open(usb_path,O_RDWR|O_NOFOLLOW|O_CLOEXEC);struct stat st{};
        if(candidate<0)continue;
        if(fstat(candidate,&st) || !S_ISCHR(st.st_mode) || major(st.st_rdev)!=189 || minor(st.st_rdev)!=(bus-1)*128+device-1) { ::close(candidate);continue; }
        // Read exact device identity and ISO endpoint shape from the opened
        // node, to catch device-number reuse between sysfs discovery and open.
        uint8_t desc[4096]{};const ssize_t size=::read(candidate,desc,sizeof(desc));
        if(size<=0 || !thermal_usb_descriptors(desc,size_t(size))) { ::close(candidate);continue; }
        if(fd>=0)::close(fd);
        fd=candidate;
    }
    closedir(dir);if(matches!=1) { if(fd>=0)::close(fd);return -1; }return fd;
}
bool ThermalLive::start(const char *name) {
    if(start_started || (name && (!*name || std::strlen(name)>48)))return false;
    normal=name==nullptr;
    if(name)std::strcpy(run_name,name);
    const int rc=pthread_create(&start_thread,nullptr,[](void *p)->void* {
        auto &self=*static_cast<ThermalLive*>(p);
        if(!self.initialize(self.normal?nullptr:self.run_name)) {
            self.degraded.store(true);ca_log("ZT30 thermal unavailable; RGB continues");
        }
        return nullptr;
    },this);
    start_started=rc==0;
    return start_started;
}
bool ThermalLive::initialize(const char *name) {
    if(!assembler.ready() || !latest || !owned || !nv12 || !encoded)return false;
    normal=name==nullptr;
    if(!normal) {
        if(!*name || std::strlen(name)>48)return false;
        for(const char *p=name;*p;++p)if(!((*p>='a' && *p<='z') || (*p>='0' && *p<='9') || *p=='-'))return false;
    }
    if(mkdir("/tmp/apcg-zt30-thermal",0700) && errno!=EEXIST)return false;
    struct stat directory{};
    if(lstat("/tmp/apcg-zt30-thermal",&directory) || !S_ISDIR(directory.st_mode) || directory.st_uid || (directory.st_mode&0777)!=0700)return false;
    if(!normal) {
        char marker[128];std::snprintf(marker,sizeof(marker),"/tmp/apcg-zt30-thermal/%s.live-started",name);
        const int once=::open(marker,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(once<0)return false;
        ::close(once);
        std::snprintf(raw_sample,sizeof(raw_sample),"/tmp/apcg-zt30-thermal/%s.raw",name);
    }
    int fd=-1;
    const uint64_t deadline=now()+15000000000ULL;
    do {
        if(stop.load())return false;
        fd=open_device();
        if(fd>=0 || !normal)break;
        pause(nullptr,100);
    } while(now()<deadline);
    if(fd<0) { ca_log("ZT30 thermal USB discovery failed");return false; }
    if(stop.load()) { ::close(fd);return false; }
    if(normal && !budget.begin("/tmp/apcg-zt30-thermal/start-budget")) {
        ::close(fd);ca_log("ZT30 thermal startup budget exhausted or invalid; RGB continues until reboot");return false;
    }
    usb.reset(new(std::nothrow) UsbIso(fd,{nullptr,ioctl_call,close_call,now,pause}));
    if(!usb) { ::close(fd);return false; }
    if(!usb->start()) { if(!usb->finish())fatal("USB finish");ca_log("ZT30 thermal USB startup failed errno=%d",errno);return false; }
    if(encoder.open())fatal(encoder.failure()?encoder.failure():"thermal open",encoder.status());
    if(ca_rtsp_add_video(rtsp,"video2",CA_VIDEO_H264,15,&stream_id) || stream_id!=1 || ca_live_video_server_configure(live,1,640,512,15,true))fatal("stream setup");
    int rc=pthread_create(&usb_thread,nullptr,[](void *p)->void* { static_cast<ThermalLive*>(p)->capture();return nullptr; },this);
    if(rc)fatal("worker create",rc);
    usb_started=true;
    rc=pthread_create(&encode_thread,nullptr,[](void *p)->void* { static_cast<ThermalLive*>(p)->encode();return nullptr; },this);
    if(rc)fatal("worker create",rc);
    encode_started=true;
    ca_log("ZT30 live thermal started 640x512 H264/15fps video2; mode=%s",normal?"normal":"diagnostic");return true;
}
bool ThermalLive::frame(void *opaque,const uint8_t *bytes,size_t length,const UvcFrameInfo &info) {
    auto &self=*static_cast<ThermalLive*>(opaque);
    if(length!=ThermalFrameBytes || !info.last_ns)return false;
    std::lock_guard<std::mutex> guard(self.lock);
    std::memcpy(self.latest.get(),bytes,length);self.receipt=info.last_ns;++self.generation;return true;
}
bool ThermalLive::packet(void *opaque,const IsoPacket &p) {
    auto &self=*static_cast<ThermalLive*>(opaque);
    if(p.status)self.assembler.lost();
    else if(p.bytes && !self.assembler.packet(p.data,p.bytes,p.received_ns))return false;
    return true;
}
void ThermalLive::capture() {
    uint64_t last=now(),seen=0;
    while(!stop.load()) {
        const int rc=usb->step(packet,this);
        if(rc<0) { usb_failed.store(true);break; }
        if(assembler.frames()!=seen) { seen=assembler.frames();last=now(); }
        if(now()-last>(seen?2000000000ULL:8000000000ULL)) { usb_failed.store(true);break; }
        if(!rc)pause(nullptr,1);
    }
    if(!usb->finish())fatal("USB finish");
    ca_log("ZT30 thermal USB stopped frames=%llu dropped=%llu malformed=%llu packet_errors=%llu failure=%u",(unsigned long long)assembler.frames(),(unsigned long long)assembler.dropped(),(unsigned long long)assembler.malformed(),(unsigned long long)usb->packet_errors(),unsigned(usb_failed.load()));
}
void ThermalLive::encode() {
    uint64_t seen=0,frames=0,last=now(),last_receipt=0,healthy_since=0;
    bool budget_reset=false;
    EncodedFrame f{};f.data=encoded.get();f.capacity=4*1024*1024;
    while(!stop.load() && !usb_failed.load()) {
        uint64_t received=0;
        {
            std::lock_guard<std::mutex> guard(lock);
            if(generation!=seen) { seen=generation;received=receipt;std::memcpy(owned.get(),latest.get(),ThermalFrameBytes); }
        }
        if(received) {
            last_receipt=received;
            if(now()-received>150000000ULL)continue; // bounded age, discard stale input
            if(thermal_uyvy_nv12(owned.get(),ThermalFrameBytes,nv12.get(),640*512,640,nv12.get()+640*512,640*256,640)!=ThermalPixelResult::Ok) { ca_log("ZT30 thermal degraded: pixel conversion");degraded.store(true);break; }
            uint64_t pts=0;const auto before=now();const int rc=sdk.current_pts(0,&pts);const auto after=now();
            if(rc) { ca_log("ZT30 thermal degraded: GetCurPts raw=%d",rc);degraded.store(true);break; }
            if(after-before>2000000ULL) {
                ca_log("ZT30 thermal dropping slow PTS sample bracket_ns=%llu",(unsigned long long)(after-before));
            } else {
                if(!clock.observe(pts,before,after)) { ca_log("ZT30 thermal degraded: SYS clock ordering/drift");degraded.store(true);break; }
            // Presentation uses SYS's sampled clock; USB receipt is not claimed
            // to be exposure time or a calibrated conversion of UVC timestamps.
            const auto result=encoder.submit(nv12.get(),ThermalNv12Bytes,pts);
                if(result!=InputBufferResult::Submitted) {
                    ca_log("ZT30 thermal submit result=%u get=%d put=%d",unsigned(result),encoder.get_status(),encoder.put_status());
                    fatal("input submit");
                }
                if(!frames && raw_sample[0]) {
                    FILE *sample=std::fopen(raw_sample,"wbx");
                    if(sample) { const size_t bytes=std::fwrite(owned.get(),1,ThermalFrameBytes,sample);std::fclose(sample);ca_log("ZT30 thermal raw sample bytes=%zu generation=%llu pts=%llu",bytes,(unsigned long long)seen,(unsigned long long)pts); }
                    raw_sample[0]=0;
                }
            }
        }
        const int got=encoder.copy_frame(f);if(got<0)fatal(encoder.failure()?encoder.failure():"copy frame",encoder.status());
        if(got) {
            uint64_t mapped=0;bool valid=f.packets && clock.frame(f.pts[0],now()/1000,mapped);
            for(unsigned i=1;i<f.packets;++i)if(f.pts[i]!=f.pts[0])valid=false;
            if(!valid) { ca_log("ZT30 thermal degraded: frame/packet PTS");degraded.store(true);break; }
            size_t offset=0,nal=0,length=0;bool key=false;
            while(ca_annexb_next(f.data,f.bytes,&offset,&nal,&length))if(length && (f.data[nal]&31)==5)key=true;
            (void)ca_rtsp_push_video_timed(rtsp,stream_id,f.data,f.bytes,key,0,mapped);
            if(ca_live_video_server_publish(live,1,f.data,f.bytes,mapped,key,0)) { ca_log("ZT30 thermal degraded: web publish");degraded.store(true);break; }
            ++frames;const auto previous=last;last=now();published_ns.store(last);
            if(!healthy_since || last-previous>=1000000000ULL)healthy_since=last;
            if(normal && !budget_reset && frames>=9000 && last-healthy_since>=600000000000ULL) {
                budget_reset=budget.reset();
                if(!budget_reset)ca_log("ZT30 thermal startup budget reset failed");
            }
            if(frames==1 || !(frames%150))ca_log("ZT30 thermal live frames=%llu latest=%llu age_ms=%llu",(unsigned long long)frames,(unsigned long long)seen,(unsigned long long)((now()-last_receipt)/1000000));
        }
        if(now()-last>(frames?4000000000ULL:10000000000ULL)) { ca_log("ZT30 thermal degraded: encoder starvation");degraded.store(true);break; }
        pause(nullptr,2);
    }
    stop.store(true);
    if(encoder.close())fatal("encoder close",encoder.status());
    ca_log("ZT30 thermal encoder stopped frames=%llu",(unsigned long long)frames);
}
bool ThermalLive::ready() const {
    const auto last=published_ns.load();
    return last && !stop.load() && !usb_failed.load() && !degraded.load() && now()-last<1000000000ULL;
}
void ThermalLive::join() {
    stop.store(true);
    if(start_started) { if(pthread_join(start_thread,nullptr))fatal("startup join");start_started=false; }
    if(usb_started) { if(pthread_join(usb_thread,nullptr))fatal("USB join");usb_started=false; }
    if(encode_started) { if(pthread_join(encode_thread,nullptr))fatal("encoder join");encode_started=false; }
    if(encoder.close())fatal("encoder close",encoder.status());
    if(normal && !usb_failed.load() && !degraded.load() && published_ns.load() && !budget.reset())
        ca_log("ZT30 thermal clean-shutdown budget reset failed");
}
}
