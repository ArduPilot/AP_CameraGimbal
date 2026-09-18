#include "camera_app/rtsp.h"
#include "camera_app/video_metadata.h"
#include "camera_app/support_video.h"
extern "C" {
#include "camera_app/log.h"
}
#include <cstdlib>
#include <limits>

#include "EventLoop.h"
#include "H264Source.h"
#include "H265Source.h"
#include "MediaSession.h"
#include "RtspServer.h"

#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <cstring>
#include <unordered_set>
#include <vector>

static std::mutex satip_clients_mutex;
static std::unordered_set<const void *> satip_clients;

extern "C" void ca_rtsp_mark_satip_client(const void *client)
{
    std::lock_guard<std::mutex> lock(satip_clients_mutex);
    satip_clients.insert(client);
}

extern "C" void ca_rtsp_unmark_satip_client(const void *client)
{
    std::lock_guard<std::mutex> lock(satip_clients_mutex);
    satip_clients.erase(client);
}

extern "C" bool ca_rtsp_is_satip_client(const void *client)
{
    std::lock_guard<std::mutex> lock(satip_clients_mutex);
    return satip_clients.find(client) != satip_clients.end();
}

/* xop's codec sources packetize one NAL and mark its final packet. Feed a
 * complete access unit here so only its final NAL carries the RTP marker. */
class AccessUnitSource : public xop::MediaSource {
public:
    AccessUnitSource(enum ca_video_codec codec, unsigned frame_rate)
        : source_(codec == CA_VIDEO_H265
                      ? static_cast<xop::MediaSource *>(xop::H265Source::CreateNew(frame_rate))
                      : static_cast<xop::MediaSource *>(xop::H264Source::CreateNew(frame_rate)))
    {
        media_type_ = source_->GetMediaType();
        payload_ = source_->GetPayloadType();
        clock_rate_ = source_->GetClockRate();
        source_->SetSendFrameCallback([this](xop::MediaChannelId channel, xop::RtpPacket packet) {
            packet.last = packet.last && last_nal_;
            packet.timestamp = timestamp_; // preserve zero at RTP clock wrap
            return send_frame_callback_ && send_frame_callback_(channel, packet);
        });
    }
    std::string GetMediaDescription(uint16_t port) override
    {
        return source_->GetMediaDescription(port);
    }
    std::string GetAttribute() override { return source_->GetAttribute(); }
    bool HandleFrame(xop::MediaChannelId channel, xop::AVFrame frame) override
    {
        timestamp_ = frame.timestamp;
        size_t offset = 0, nal, length;
        bool sent = false;
        while (ca_annexb_next(frame.buffer.get(), frame.size, &offset, &nal, &length)) {
            if (length == 0) continue;
            xop::AVFrame part;
            part.buffer = std::shared_ptr<uint8_t>(frame.buffer, frame.buffer.get() + nal);
            part.size = static_cast<uint32_t>(length);
            part.type = frame.type;
            part.timestamp = frame.timestamp;
            last_nal_ = offset == frame.size;
            if (!source_->HandleFrame(channel, part)) return false;
            sent = true;
        }
        return sent;
    }
private:
    std::unique_ptr<xop::MediaSource> source_;
    bool last_nal_ = false;
    uint32_t timestamp_ = 0;
};

struct ca_rtsp {
    struct stream {
        ca_support_video *publisher = nullptr;
        xop::MediaSessionId session_id;
        std::vector<xop::MediaSessionId> aliases;
        enum ca_video_codec codec;
        unsigned frame_rate;
        uint32_t next_timestamp;
        uint32_t timestamp_step;
    };
    std::shared_ptr<xop::EventLoop> loop;
    std::shared_ptr<xop::RtspServer> server;
    std::vector<stream> streams;
    std::mutex mutex;
};

static void add_stream(ca_rtsp *rtsp, xop::MediaSessionId session_id,
                       enum ca_video_codec codec, unsigned frame_rate)
{
    ca_rtsp::stream stream;
    stream.session_id = session_id;
    stream.codec = codec;
    stream.frame_rate = frame_rate;
    stream.next_timestamp = codec == CA_VIDEO_H265
                                ? xop::H265Source::GetTimestamp()
                                : xop::H264Source::GetTimestamp();
    stream.timestamp_step = 90000U / frame_rate;
    rtsp->streams.push_back(stream);
}

extern "C" int ca_rtsp_open(struct ca_rtsp **result, unsigned port,
                            const char *path, enum ca_video_codec codec,
                            unsigned frame_rate)
{
    if (result == nullptr || path == nullptr || *path == '\0' ||
        frame_rate == 0) return -1;
    std::unique_ptr<ca_rtsp> rtsp(new (std::nothrow) ca_rtsp);
    if (!rtsp) return -1;
    rtsp->loop = std::make_shared<xop::EventLoop>();
    rtsp->server = xop::RtspServer::Create(rtsp->loop.get());
    if (!rtsp->server || !rtsp->server->Start("0.0.0.0", port)) return -1;
    xop::MediaSession *session = xop::MediaSession::CreateNew(path);
    if (session == nullptr) return -1;
    session->AddSource(xop::channel_0, new AccessUnitSource(codec, frame_rate));
    xop::MediaSessionId session_id = rtsp->server->AddSession(session);
    if (session_id == 0) return -1;
    add_stream(rtsp.get(), session_id, codec, frame_rate);
    *result = rtsp.release();
    return 0;
}

extern "C" int ca_rtsp_add_video(struct ca_rtsp *rtsp, const char *path,
                                  enum ca_video_codec codec,
                                  unsigned frame_rate, unsigned *stream_id)
{
    if (rtsp == nullptr || path == nullptr || *path == '\0' ||
        frame_rate == 0 || stream_id == nullptr) return -1;
    std::lock_guard<std::mutex> lock(rtsp->mutex);
    xop::MediaSession *session = xop::MediaSession::CreateNew(path);
    if (session == nullptr) return -1;
    session->AddSource(xop::channel_0, new AccessUnitSource(codec, frame_rate));
    xop::MediaSessionId session_id = rtsp->server->AddSession(session);
    if (session_id == 0) return -1;
    *stream_id = static_cast<unsigned>(rtsp->streams.size());
    add_stream(rtsp, session_id, codec, frame_rate);
    return 0;
}

extern "C" int ca_rtsp_add_alias(struct ca_rtsp *rtsp, unsigned stream_id,
                                  const char *alias)
{
    if (rtsp == nullptr || alias == nullptr) return -1;
    if (*alias == '\0') return 0;
    std::lock_guard<std::mutex> lock(rtsp->mutex);
    if (stream_id >= rtsp->streams.size()) return -1;
    ca_rtsp::stream &stream = rtsp->streams[stream_id];
    xop::MediaSession *session = xop::MediaSession::CreateNew(alias);
    if (session == nullptr) return -1;
    session->AddSource(xop::channel_0, new AccessUnitSource(stream.codec, stream.frame_rate));
    xop::MediaSessionId session_id = rtsp->server->AddSession(session);
    if (session_id == 0) return -1;
    stream.aliases.push_back(session_id);
    return 0;
}

extern "C" void ca_rtsp_add_config_aliases(struct ca_rtsp *rtsp,
                                           const struct ca_config *settings)
{
    if (rtsp == nullptr || settings == nullptr) return;
    const char *aliases[2] = {settings->main_alias, settings->sub_alias};
    for (unsigned i = 0; i < 2; i++) {
        if (ca_rtsp_add_alias(rtsp, i, aliases[i]) < 0)
            ca_log("cannot serve /video%u as /%s", i + 1U, aliases[i]);
    }
}

static int push_video(struct ca_rtsp *rtsp, unsigned stream_id,
                      const uint8_t *data, size_t length, bool key_frame,
                      float hfov_deg, bool timed, uint32_t timestamp)
{
    if (rtsp == nullptr || data == nullptr || length == 0) return -1;
    std::lock_guard<std::mutex> lock(rtsp->mutex);
    if (stream_id >= rtsp->streams.size()) return -1;
    ca_rtsp::stream &stream = rtsp->streams[stream_id];
    if (timed) stream.next_timestamp = timestamp;
    uint8_t *annotated = nullptr;
    size_t annotated_length = 0;
    int inserted = ca_video_metadata_insert(stream.codec, data, length,
                                             stream.next_timestamp, hfov_deg,
                                             &annotated, &annotated_length);
    if (inserted < 0) return -1;
    if (inserted == 0) {
        annotated = static_cast<uint8_t *>(std::malloc(length));
        if (annotated == nullptr) return -1;
        std::memcpy(annotated, data, length);
        annotated_length = length;
    }
    xop::AVFrame frame;
    frame.buffer = std::shared_ptr<uint8_t>(annotated, std::free);
    if (annotated_length > std::numeric_limits<uint32_t>::max()) return -1;
    frame.size = static_cast<uint32_t>(annotated_length);
    frame.type = key_frame ? xop::VIDEO_FRAME_I : xop::VIDEO_FRAME_P;
    frame.timestamp = stream.next_timestamp;
    ca_support_video_push(stream.publisher, annotated, annotated_length,
                           frame.timestamp, key_frame);
    bool sent = rtsp->server->PushFrame(stream.session_id, xop::channel_0, frame);
    for (xop::MediaSessionId alias : stream.aliases)
        sent = rtsp->server->PushFrame(alias, xop::channel_0, frame) || sent;
    // The stream clock must advance even without a local RTSP viewer; an
    // independent SupportProxy publisher still consumes these access units.
    if (inserted != 0) stream.next_timestamp += stream.timestamp_step;
    return sent ? 0 : -1;
}

extern "C" int ca_rtsp_push_h264_stream(struct ca_rtsp *rtsp, unsigned stream_id,
                                          const uint8_t *data, size_t length,
                                          bool key_frame, float hfov_deg)
{
    return push_video(rtsp, stream_id, data, length, key_frame, hfov_deg, false, 0);
}

extern "C" int ca_rtsp_push_video_timed(struct ca_rtsp *rtsp, unsigned stream_id,
                                         const uint8_t *data, size_t length,
                                         bool key_frame, float hfov_deg, uint64_t pts_us)
{
    uint32_t timestamp = static_cast<uint32_t>((pts_us / 1000000U) * 90000U +
                                              (pts_us % 1000000U) * 9U / 100U);
    return push_video(rtsp, stream_id, data, length, key_frame, hfov_deg, true, timestamp);
}

extern "C" int ca_rtsp_push_video(struct ca_rtsp *rtsp, unsigned stream_id,
                                    const uint8_t *data, size_t length,
                                    bool key_frame, float hfov_deg)
{
    return ca_rtsp_push_h264_stream(rtsp, stream_id, data, length, key_frame, hfov_deg);
}

extern "C" int ca_rtsp_push_h264(struct ca_rtsp *rtsp,
                                  const uint8_t *data, size_t length,
                                  bool key_frame, float hfov_deg)
{
    return ca_rtsp_push_h264_stream(rtsp, 0, data, length, key_frame, hfov_deg);
}

extern "C" int ca_rtsp_set_frame_rate(struct ca_rtsp *rtsp,
                                        unsigned stream_id,
                                        unsigned frame_rate)
{
    if (rtsp == nullptr || frame_rate == 0) return -1;
    std::lock_guard<std::mutex> lock(rtsp->mutex);
    if (stream_id >= rtsp->streams.size()) return -1;
    rtsp->streams[stream_id].timestamp_step = 90000U / frame_rate;
    return 0;
}

extern "C" int ca_rtsp_support_proxy(struct ca_rtsp *rtsp,
                                        const struct ca_support_config *config)
{
    if (rtsp == nullptr || config == nullptr) return -1;
    std::lock_guard<std::mutex> lock(rtsp->mutex);
    for (size_t i = 0; i < rtsp->streams.size(); i++) {
        auto &stream = rtsp->streams[i];
        if (ca_support_video_open(&stream.publisher, config, i, stream.codec) < 0) return -1;
    }
    return 0;
}

extern "C" void ca_rtsp_close(struct ca_rtsp *rtsp)
{
    if (rtsp == nullptr) return;
    for (const auto &stream : rtsp->streams) ca_support_video_close(stream.publisher);
    {
        std::lock_guard<std::mutex> lock(rtsp->mutex);
        if (rtsp->server) {
            for (const ca_rtsp::stream &stream : rtsp->streams) {
                rtsp->server->RemoveSession(stream.session_id);
                for (xop::MediaSessionId alias : stream.aliases)
                    rtsp->server->RemoveSession(alias);
            }
        }
        rtsp->server.reset();
        rtsp->loop.reset();
    }
    delete rtsp;
}
