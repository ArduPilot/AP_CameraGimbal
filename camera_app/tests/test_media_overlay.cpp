/* Fault injection through the public media API: overlay errors must not mask
 * control errors, and a failed pipeline replacement must restore its owner. */
#include "../src/media/media.cpp"
#include "test_media_backend.h"
#include <cassert>
static bool overlay_live, recording_active, fail_open;
static unsigned closed, opened, log_count;
static int control_result, overlay_result;
void ca_log(const char *, ...) { log_count++; }
bool ca_binlog_active() { return false; }
uint64_t ca_binlog_time_us() { return 0; }
void ca_binlog_emit(uint8_t, const void *, size_t) {}
static int control() { overlay_live=false; errno=EIO; return control_result; }
bool ca_config_image_equal(const ca_config *a, const ca_config *b)
{ return a->brightness == b->brightness; }
class OverlayBackend final : public TestMediaBackend {
public:
    ~OverlayBackend() override { closed++; }
    bool recording() const override { return recording_active; }
    int set_recording(bool active) override { recording_active=active; return 0; }
    int apply_overlay(const ca_config *) override
    { overlay_live=overlay_result==0; errno=ENOSPC; return overlay_result; }
    int set_zoom(float) override { return control(); }
    int set_lens(enum ca_media_lens) override { return control(); }
    int set_thermal_main(bool) override { return control(); }
};
std::unique_ptr<APC_Media_Backend> APC_Media_Backend::create(const ca_media_config &)
{
    opened++;
    if (fail_open) { fail_open=false; errno=ENOMEM; return nullptr; }
    return std::unique_ptr<APC_Media_Backend>(new OverlayBackend);
}
int main()
{
    ca_media_config config {};
    ca_media *media=nullptr;
    overlay_result=-1;
    assert(ca_media_open(&media,&config)==0);
    assert(media && !overlay_live && log_count==1 && closed==0);
    ca_media_close(media);
    assert(closed==1);
    overlay_result=0;
    assert(ca_media_open(&media,&config)==0);
    for (control_result=-1;control_result<=0;control_result++) {
        for (overlay_result=-1;overlay_result<=0;overlay_result++) {
            for (unsigned op=0;op<3;op++) {
                int result=op==0 ? ca_media_set_zoom(media,2) :
                    op==1 ? ca_media_set_lens(media,CA_MEDIA_LENS_WIDE) :
                            ca_media_set_thermal_main(media,true);
                assert(result==control_result);
                assert(overlay_live==(overlay_result==0));
                if (control_result<0) assert(errno==EIO);
            }
        }
    }
    control_result=overlay_result=0;
    ca_config next=config.settings;
    next.main_resolution=CA_VIDEO_1080P;
    assert(next.main_resolution!=config.settings.main_resolution);
    const unsigned initial_opens=opened;
    assert(ca_media_set_recording(media,true)==0);
    assert(ca_media_configure(media,&next)<0 && errno==EBUSY);
    assert(opened==initial_opens);
    assert(ca_media_set_recording(media,false)==0);
    fail_open=true;
    assert(ca_media_configure(media,&next)<0 && errno==ENOMEM);
    assert(opened==initial_opens+2 && ca_media_ready(media));
    assert(ca_media_settings(media)->main_resolution==config.settings.main_resolution);
    assert(ca_media_configure(media,&next)==0);
    assert(ca_media_settings(media)->main_resolution==next.main_resolution);
    ca_media_close(media);
    assert(closed==opened-1); // The failed open never acquired a backend.
    puts("PASS media controls, overlay errors, recording guard, rollback and lifetime");
}
