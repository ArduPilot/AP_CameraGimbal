/* Exercise the public wrappers with a backend that tears overlays down before
 * a failed pipeline switch, and an overlay restore that changes errno. */
#include "../src/media/media.c"
#include <assert.h>
static bool overlay_live;
static int control_result, overlay_result;
void ca_log(const char *format, ...) { (void)format; }
int ca_media_impl_apply_overlay(struct ca_media_impl *m, const struct ca_config *c)
{ (void)m; (void)c; overlay_live=overlay_result==0; errno=ENOSPC; return overlay_result; }
static int control(void) { overlay_live=false; errno=EIO; return control_result; }
int ca_media_impl_set_zoom(struct ca_media_impl *m, float v)
{ (void)m; (void)v; return control(); }
int ca_media_impl_set_lens(struct ca_media_impl *m, enum ca_media_lens v)
{ (void)m; (void)v; return control(); }
int ca_media_impl_set_thermal_main(struct ca_media_impl *m, bool v)
{ (void)m; (void)v; return control(); }
int main(void)
{
    struct ca_media media={0};
    media.impl=(struct ca_media_impl *)&media;
    for (control_result=-1;control_result<=0;control_result++) {
        for (overlay_result=-1;overlay_result<=0;overlay_result++) {
            for (unsigned op=0;op<3;op++) {
                int result=op==0 ? ca_media_set_zoom(&media,2) :
                    op==1 ? ca_media_set_lens(&media,CA_MEDIA_LENS_WIDE) :
                            ca_media_set_thermal_main(&media,true);
                assert(result==control_result);
                assert(overlay_live==(overlay_result==0));
                if (control_result<0) assert(errno==EIO);
            }
        }
    }
    puts("PASS media controls restore overlays after success/failure and preserve control errors");
}
