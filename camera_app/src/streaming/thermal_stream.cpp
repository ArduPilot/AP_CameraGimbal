#include "camera_app/thermal_stream.h"
#include <stddef.h>

void ca_thermal_test_pattern(uint16_t *pixels, uint64_t sequence)
{
    // All 65536 codes occur five times, including low bits and extrema. This
    // is an explicit sensor test pattern, not simulated terrain temperature.
    for (unsigned i = 0; i < 640U*512U; i++) pixels[i] = uint16_t(i + sequence*257U);
}

#ifdef CA_THERMAL_STREAM_FFV1
#include "thermal_codec.h"
#include "camera_app/support_video.h"
#include "thermal_matroska.h"
#include "camera_app/log.h"
#include "camera_app/metadata.h"
#include "camera_app/video_metadata.h"
#include "apcam/target.h"
#include <arpa/inet.h>
#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <memory>
#include <mutex>
#include <new>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/socket.h>
#include <sys/file.h>
#include <unistd.h>

using thermal_mkv::Bytes;
static std::atomic<unsigned> public_port{0}, public_fps{5};
static std::atomic<bool> enabled{true}, available{false};
static uint64_t mono_us() {
    timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return uint64_t(t.tv_sec)*1000000 + t.tv_nsec/1000;
}
struct ca_thermal_stream {
    int listener = -1;
    ca_support_video *proxy = nullptr;
    pthread_t thread{};
    bool started = false, simulated = false;
    std::atomic<bool> stop{false};
    std::mutex lock;
    std::vector<uint16_t> pixels;
    std::string json;
    uint64_t sequence = 0, capture_us = 0;
    ThermalCodec codec;
    Bytes header;
    std::mutex record_lock;
    int record_fd = -1;
    unsigned record_fps = 5;
    bool recording = false;
    std::string video_path, record_path;
    uint64_t record_generation = 0, record_start = 0, record_epoch = 0;
    uint64_t record_previous = 0, next_record = 0;

};
struct ThermalClient {
    int fd;
    std::string request;
    bool streaming = false;
    uint64_t progress_us;
    std::shared_ptr<const Bytes> pending;
    size_t offset = 0;
};

void ca_thermal_stream_publish(ca_thermal_stream *s, const uint16_t *pixels,
                               const timespec *captured_at, uint8_t gain, bool rotated)
{
    if (!s || !pixels || !captured_at) return;
    ca_metadata metadata;
    ca_metadata_snapshot(&metadata); // freeze telemetry with pixels, never at client-read time
    const uint64_t now = mono_us();
    char telemetry[CA_VIDEO_METADATA_JSON_MAX];
    if (!ca_video_metadata_json(&metadata,captured_at,now*9/100,telemetry,sizeof(telemetry))) return;
    unsigned minimum = UINT16_MAX, maximum = 0;
    for (unsigned i=0;i<640U*512U;i++) {
        if (pixels[i]<minimum) minimum=pixels[i];
        if (pixels[i]>maximum) maximum=pixels[i];
    }
    std::lock_guard<std::mutex> guard(s->lock);
    ++s->sequence;
    char json[CA_VIDEO_METADATA_JSON_MAX+2048];
    int n = snprintf(json,sizeof(json),
        "{\"schema\":\"apcg.thermal.v1\",\"frame_id\":%llu,\"capture_monotonic_us\":%llu,"
        "\"timestamp_source\":\"%s\",\"simulated\":%s,\"width\":640,\"height\":512,"
        "\"pixel_format\":\"gray16le\",\"bits_per_sample\":16,"
        "\"temperature_scale_k\":0.015625,\"temperature_offset_k\":0,\"gain\":%d,"
        "\"minimum_raw\":%u,\"maximum_raw\":%u,\"minimum_c\":%.6f,\"maximum_c\":%.6f,"
        "\"rotation_deg\":%u,\"hfov_deg\":%.6f,\"calibration\":\"nominal_pinhole\","
        "\"altitude_datum\":\"AMSL\",\"gimbal_frame\":\"roll_pitch_level_yaw_vehicle\","
        "\"telemetry\":%s}",
        (unsigned long long)s->sequence,(unsigned long long)now,
        s->simulated?"simulation":"usb_receive",s->simulated?"true":"false",gain==255?-1:int(gain),
        minimum,maximum,minimum/64.0-273.15,maximum/64.0-273.15,
        rotated?180:0,double(APCAM_LENS3_FOV_H),telemetry);
    if (n<0 || size_t(n)>=sizeof(json)) return;
    memcpy(s->pixels.data(),pixels,640U*512U*sizeof(uint16_t));
    s->json.assign(json,size_t(n)); s->capture_us=now;
}

static int write_all(int fd, const Bytes &data)
{
    // Files downloads snapshot an immutable prefix between complete clusters.
    if (flock(fd, LOCK_EX) < 0) return -1;
    off_t start = lseek(fd, 0, SEEK_CUR);
    size_t offset = 0;
    int result = start < 0 ? -1 : 0;
    while (result == 0 && offset < data.size()) {
        ssize_t n = write(fd, data.data()+offset, data.size()-offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (n == 0) errno = EIO; result = -1; break; }
        offset += size_t(n);
    }
    int saved = errno;
    // Keep earlier frames readable if storage fills part way through a frame.
    if (result < 0 && start >= 0 && ftruncate(fd, start) < 0)
        ca_log("raw thermal partial-frame truncation failed: %s", strerror(errno));
    (void)flock(fd, LOCK_UN);
    errno = saved;
    return result;
}

// Called with record_lock held; stop waits for the current complete frame.
static int close_recording(ca_thermal_stream *s)
{
    if (s->record_fd < 0) return 0;
    int result = fdatasync(s->record_fd);
    int saved = errno;
    if (close(s->record_fd) < 0 && result == 0) { result = -1; saved = errno; }
    s->record_fd = -1;
    ++s->record_generation;
    ca_log("raw thermal recording stopped: %s%s", s->record_path.c_str(),
           result < 0 ? " (flush failed)" : "");
    errno = saved;
    return result;
}

static int open_recording(ca_thermal_stream *s)
{
    std::string stem = s->video_path;
    if (stem.size() >= 4 && stem.compare(stem.size()-4, 4, ".mp4") == 0)
        stem.resize(stem.size()-4);
    for (unsigned part=0; part<10000; part++) {
        s->record_path = stem + ".raw" + (part ? "-" + std::to_string(part) : "") + ".mkv";
        s->record_fd = open(s->record_path.c_str(), O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC, 0644);
        if (s->record_fd >= 0) break;
        if (errno != EEXIST) return -1;
    }
    if (s->record_fd < 0) return -1;
    if (write_all(s->record_fd, s->header) < 0) {
        int saved = errno;
        close(s->record_fd); s->record_fd = -1;
        unlink(s->record_path.c_str()); errno = saved;
        return -1;
    }
    ++s->record_generation;
    s->record_start = mono_us();
    s->record_epoch = s->record_previous = s->next_record = 0;
    ca_log("raw thermal recording started: %s (%u fps)", s->record_path.c_str(), s->record_fps);
    return 0;
}

int ca_thermal_stream_recording(ca_thermal_stream *s, bool active, const char *video_path)
{
    if (!s) return 0;
    std::lock_guard<std::mutex> guard(s->record_lock);
    if (!active) {
        s->recording = false;
        return close_recording(s);
    }
    if (s->recording) return 0;
    if (!video_path || !*video_path) { errno = EINVAL; return -1; }
    s->video_path = video_path;
    if (s->record_fps && open_recording(s) < 0) return -1;
    s->recording = true;
    return 0;
}

int ca_thermal_stream_configure(ca_thermal_stream *s, const ca_config *settings)
{
    if (!s) return 0;
    if (!settings || settings->raw_stream_fps < 1 || settings->raw_stream_fps > 25 ||
        settings->raw_record_fps > 25) { errno = EINVAL; return -1; }
    std::lock_guard<std::mutex> guard(s->record_lock);
    unsigned previous = s->record_fps;
    s->record_fps = settings->raw_record_fps;
    if (!s->record_fps) {
        if (close_recording(s) < 0) { s->record_fps = previous; return -1; }
    } else if (s->recording && s->record_fd < 0 && open_recording(s) < 0) {
        s->record_fps = previous;
        return -1;
    }
    if (previous != s->record_fps) s->next_record = 0;
    public_fps = settings->raw_stream_fps;
    return 0;
}

static uint64_t next_deadline(uint64_t previous, uint64_t now, unsigned fps)
{
    uint64_t interval = 1000000 / fps;
    // Preserve phase when selecting from the 25 Hz sensor (e.g. 10 Hz needs
    // alternating two/three-frame gaps), but never queue catch-up frames.
    return previous ? previous + ((now-previous)/interval+1)*interval : now+interval;
}

static void *thermal_worker(void *opaque)
{
    auto *s = static_cast<ca_thermal_stream *>(opaque);
    std::vector<ThermalClient> clients;
    std::vector<uint16_t> pixels(640U*512U);
    uint64_t previous=0, next_encode=0, epoch=0, encoded_sequence=0;
    unsigned stream_fps = public_fps.load();
    Bytes encoded;
    while (!s->stop.load()) {
        pollfd waitfd{s->listener,POLLIN,0};
        poll(&waitfd,1,10);
        while (s->listener >= 0) {
            int fd=accept4(s->listener,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);
            if (fd<0) break;
            if (clients.size()>=4) { close(fd); continue; }
            clients.push_back(ThermalClient{fd,{},false,mono_us(),{},0});
        }
        const uint64_t now=mono_us();
        for (auto &c:clients) {
            if (!enabled.load()) { close(c.fd); c.fd=-1; continue; }
            if (!c.streaming) {
                char buffer[1024];
                ssize_t n=recv(c.fd,buffer,sizeof(buffer),0);
                if (n>0) c.request.append(buffer,size_t(n));
                else if (n==0 || (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)) { close(c.fd); c.fd=-1; continue; }
                if (c.request.size()>4096) { close(c.fd); c.fd=-1; continue; }
                if (c.request.find("\r\n\r\n")!=std::string::npos) {
                    if (c.request.rfind("GET /thermal.mkv HTTP/1.1\r\n",0) != 0 &&
                        c.request.rfind("GET /thermal.mkv HTTP/1.0\r\n",0) != 0) {
                        close(c.fd); c.fd=-1; continue;
                    }
                    const char response[]="HTTP/1.1 200 OK\r\nContent-Type: video/x-matroska\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
                    auto out=std::make_shared<Bytes>(response,response+sizeof(response)-1);
                    thermal_mkv::append(*out,s->header); c.pending=out;
                    c.streaming=true; c.progress_us=now;
                }
            }
            if (c.pending) {
                const auto &b=*c.pending;
                ssize_t n=send(c.fd,b.data()+c.offset,b.size()-c.offset,MSG_NOSIGNAL);
                if (n>0) {
                    c.offset+=n; c.progress_us=now;
                    if (c.offset==b.size()) { c.pending.reset(); c.offset=0; }
                } else if (n==0 || (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)) {
                    close(c.fd); c.fd=-1; continue;
                }
            } else if (c.streaming) {
                char b; ssize_t n=recv(c.fd,&b,1,MSG_PEEK);
                if (n==0) { close(c.fd); c.fd=-1; continue; }
            }
            if ((!c.streaming || c.pending) && now-c.progress_us>2000000) { close(c.fd); c.fd=-1; }
        }
        for (size_t i=0;i<clients.size();) {
            if (clients[i].fd<0) clients.erase(clients.begin()+i); else i++;
        }
        if (stream_fps != public_fps.load()) {
            stream_fps = public_fps.load();
            next_encode = 0;
        }
        bool stream_due = s->proxy && enabled.load();
        for (auto &c:clients) stream_due |= c.streaming && !c.pending;
        stream_due = stream_due && now >= next_encode;
        uint64_t generation;
        bool record_due;
        {
            std::lock_guard<std::mutex> guard(s->record_lock);
            record_due = s->record_fd >= 0 && now >= s->next_record;
            generation = s->record_generation;
        }
        if (!stream_due && !record_due) continue;
        std::string json;
        uint64_t sequence,capture;
        {
            std::lock_guard<std::mutex> guard(s->lock);
            sequence=s->sequence; capture=s->capture_us;
            if (s->json.empty()) continue;
            pixels=s->pixels; json=s->json;
        }
        stream_due = stream_due && sequence != previous;
        {
            std::lock_guard<std::mutex> guard(s->record_lock);
            record_due = record_due && generation == s->record_generation &&
                capture >= s->record_start && sequence != s->record_previous;
        }
        if (!stream_due && !record_due) continue;
        if (!epoch) epoch=capture;
        // Reuse the last intra frame if the other output selects it later.
        if (encoded_sequence != sequence && !s->codec.encode(pixels.data(),(capture-epoch)/1000,encoded)) {
            ca_log("thermal FFV1 encoding failed"); enabled=false;
            std::lock_guard<std::mutex> guard(s->record_lock);
            (void)close_recording(s);
            break;
        }
        encoded_sequence = sequence;
        if (stream_due) {
            auto data=std::make_shared<Bytes>(thermal_mkv::frame(encoded,json.c_str(),(capture-epoch)/1000));
            for (auto &c:clients) if (c.streaming && !c.pending) { c.pending=data; c.progress_us=now; }
            if (enabled.load()) ca_support_video_push(s->proxy, data->data(), data->size(), 0, true);
            previous=sequence; next_encode=next_deadline(next_encode,now,stream_fps);
        }
        if (record_due) {
            std::lock_guard<std::mutex> guard(s->record_lock);
            // A stop/restart or rate change can occur while encoding.
            if (s->record_fd >= 0 && generation == s->record_generation && now >= s->next_record) {
                if (!s->record_epoch) s->record_epoch=capture;
                Bytes data=thermal_mkv::frame(encoded,json.c_str(),(capture-s->record_epoch)/1000);
                if (write_all(s->record_fd,data) < 0) {
                    ca_log("raw thermal recording write failed: %s",strerror(errno));
                    (void)close_recording(s);
                } else {
                    s->record_previous=sequence;
                    s->next_record=next_deadline(s->next_record,now,s->record_fps);
                }
            }
        }
    }
    for (auto &c:clients) close(c.fd);
    return nullptr;
}

static bool env_unsigned(const char *name,unsigned fallback,unsigned maximum,unsigned &value)
{
    const char *s=getenv(name);
    if (!s) { value=fallback; return value<=maximum; }
    char *end; errno=0; unsigned long n=strtoul(s,&end,10);
    if (!*s || *end || errno || n>maximum) return false;
    value=unsigned(n); return true;
}
int ca_thermal_stream_open(ca_thermal_stream **result,unsigned rtsp_port,bool simulated,const ca_config *settings)
{
    *result=nullptr;
    unsigned port;
    if (!settings || settings->raw_stream_fps < 1 || settings->raw_stream_fps > 25 ||
        settings->raw_record_fps > 25 ||
        !env_unsigned("CAMERA_APP_RAW_THERMAL_PORT",rtsp_port+2,65535,port)) { errno=EINVAL; return -1; }
    unsigned fps = settings->raw_stream_fps;
    auto *s=new(std::nothrow) ca_thermal_stream;
    if (!s) { errno=ENOMEM; return -1; }
    s->simulated=simulated; s->pixels.resize(640U*512U);
    if (!s->codec.open()) { delete s; errno=ENOTSUP; return -1; }
    s->header=thermal_mkv::header(s->codec.context->extradata,s->codec.context->extradata_size,0);
    s->record_fps = settings->raw_record_fps;
    if (port) {
        s->listener=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        int one=1; setsockopt(s->listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
        sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(port); address.sin_addr.s_addr=htonl(INADDR_ANY);
        if (s->listener<0 || bind(s->listener,reinterpret_cast<sockaddr *>(&address),sizeof(address))<0 || listen(s->listener,4)<0) {
            int error=errno; if (s->listener>=0) close(s->listener); delete s; errno=error; return -1;
        }
    }
    if (ca_support_video_open_matroska(&s->proxy, &settings->support, s->header.data(), s->header.size()) < 0) {
        if (s->listener >= 0) close(s->listener);
        delete s; return -1;
    }
    enabled=true; public_fps=fps;
    int error=pthread_create(&s->thread,nullptr,thermal_worker,s);
    if (error) { ca_support_video_close(s->proxy); close(s->listener); delete s; errno=error; return -1; }
    s->started=true; public_port=port; available=port || s->proxy; *result=s;
    ca_log("lossless thermal ready: HTTP port=%u FFV1 gray16 640x512 %u fps%s",port,fps,simulated?" (test pattern)":"");
    return 0;
}
void ca_thermal_stream_close(ca_thermal_stream *s)
{
    if (!s) return;
    public_port=0; available=false; s->stop=true;
    if (s->started) pthread_join(s->thread,nullptr);
    ca_support_video_close(s->proxy);
    (void)ca_thermal_stream_recording(s, false, nullptr);
    if (s->listener >= 0) close(s->listener);
    delete s;
}
unsigned ca_thermal_stream_port() { return public_port.load(); }
bool ca_thermal_stream_available() { return available.load(); }
unsigned ca_thermal_stream_fps() { return public_fps.load(); }
bool ca_thermal_stream_enabled() { return enabled.load(); }
void ca_thermal_stream_enable(bool value) { enabled=value; }
#else
int ca_thermal_stream_open(ca_thermal_stream **s,unsigned,bool,const ca_config *) { *s=nullptr; return 0; }
void ca_thermal_stream_close(ca_thermal_stream *) {}
int ca_thermal_stream_configure(ca_thermal_stream *,const ca_config *) { return 0; }
int ca_thermal_stream_recording(ca_thermal_stream *,bool,const char *) { return 0; }
void ca_thermal_stream_publish(ca_thermal_stream *,const uint16_t *,const timespec *,uint8_t,bool) {}
unsigned ca_thermal_stream_port() { return 0; }
bool ca_thermal_stream_available() { return false; }
unsigned ca_thermal_stream_fps() { return 0; }
bool ca_thermal_stream_enabled() { return false; }
void ca_thermal_stream_enable(bool) {}
#endif
