/* SigmaStar hardware regions; one small encoder/VPE overlay per stream.
 * Region ABI declarations are the same pinned OpenIPC sources as each backend. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/overlay.h"
#include "camera_app/log.h"
#include "apcam/target.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#if APCAM_TARGET == APCAM_TARGET_A8
#include "mi/star/m6_rgn.h"
#define MI_TYPE(name) m6_##name
#define MI_CALL(hw, name, ...) (hw)->mi.name(0, __VA_ARGS__)
#define MI_DEINIT(hw) (hw)->mi.fnDeinit(0)
/* The vendor app attaches its OSD to the scaler output ports (module id 34),
 * not to the encoder; port c feeds VENC channel c from SCL device/channel 0. */
#define MI_DEST(c) (m6_sys_bind){.module=M6_SYS_MOD_SCL,.device=0,.channel=0,.port=(c)}
#else
#include "mi/star/i6_rgn.h"
#define MI_TYPE(name) i6_##name
#define MI_CALL(hw, name, ...) (hw)->mi.name(__VA_ARGS__)
#define MI_DEINIT(hw) (hw)->mi.fnDeinit()
/* RGN's VPE destination enum is 0, distinct from the SYS module enum. */
#define MI_DEST(c) (i6_sys_bind){.module=(i6_sys_mod)0,.device=0,.channel=0,.port=(c)}
#endif
struct ca_overlay_hw {
    MI_TYPE(rgn_impl) mi;
    bool initialized, created[3], attached[3];
    unsigned width[3], height[3];
};
static void clear(struct ca_overlay_hw *hw,unsigned c)
{
    MI_TYPE(sys_bind) dest=MI_DEST(c);
    if (hw->attached[c]) (void)MI_CALL(hw,fnDetachChannel,c,&dest);
    if (hw->created[c]) (void)MI_CALL(hw,fnDestroyRegion,c);
    hw->attached[c]=hw->created[c]=false;
    hw->width[c]=hw->height[c]=0;
}
void ca_overlay_hw_close(struct ca_overlay_hw *hw)
{
    if (!hw) return;
    for (unsigned c=0;c<3;c++) clear(hw,c);
    if (hw->initialized) (void)MI_DEINIT(hw);
    MI_TYPE(rgn_unload)(&hw->mi);
    free(hw);
}
int ca_overlay_hw_set(struct ca_overlay_hw **out,const struct ca_overlay_channel *channels,unsigned count)
{
    if (count>3) { errno=EINVAL; return -1; }
    bool enabled=false;
    for (unsigned c=0;c<count;c++) enabled |= channels[c].cross;
    if (!enabled) { ca_overlay_hw_close(*out); *out=NULL; return 0; }
    int error=0;
    struct ca_overlay_hw *hw;
    if (!*out) {
        struct ca_overlay_hw *hw=(struct ca_overlay_hw *)calloc(1,sizeof(*hw));
        if (!hw) return -1;
        *out=hw;
        if ((error=MI_TYPE(rgn_load)(&hw->mi))) goto fail;
        MI_TYPE(rgn_pal) palette= {};
        if ((error=MI_CALL(hw,fnInit,&palette))) goto fail;
        hw->initialized=true;
    }
    hw=*out;
    for (unsigned c=0;c<count;c++) {
        const struct ca_overlay_channel *s=&channels[c];
        if (!s->cross) { clear(hw,c); continue; }
        if (hw->attached[c] && hw->width[c]==s->width && hw->height[c]==s->height) continue;
        clear(hw,c);
        struct ca_overlay_geometry g;
        struct ca_overlay_bitmap bits[CA_OVERLAY_REGIONS];
        ca_overlay_geometry(&g,s->width,s->height,true,false,0);
        if (ca_overlay_bitmaps(bits,s->width,s->height,&g)<0) { error=-1; goto fail; }
        struct ca_overlay_bitmap *b=&bits[0];
        if (!b->pixels) { ca_overlay_free(bits); error=-ENOMEM; goto fail; }
        MI_TYPE(rgn_cnf) config={.type=(MI_TYPE(rgn_type))0,.pixFmt=(MI_TYPE(rgn_pixfmt))0,.size={b->width,b->height}};
        error=MI_CALL(hw,fnCreateRegion,c,&config);
        if (!error) {
            hw->created[c]=true;
            MI_TYPE(rgn_bmp) bitmap={.pixFmt=(MI_TYPE(rgn_pixfmt))0,.size={b->width,b->height},.data=b->pixels};
            error=MI_CALL(hw,fnSetBitmap,c,&bitmap);
        }
        if (!error) {
            MI_TYPE(sys_bind) dest=MI_DEST(c);
            MI_TYPE(rgn_chn) display={.show=1,.point={b->x,b->y}};
            display.osd.bgFgAlpha[0]=0;
            display.osd.bgFgAlpha[1]=255;
            error=MI_CALL(hw,fnAttachChannel,c,&dest,&display);
            if (!error) { hw->attached[c]=true; hw->width[c]=s->width; hw->height[c]=s->height; }
        }
        ca_overlay_free(bits);
        if (error) goto fail;
    }
    return 0;
fail:
    ca_log("SigmaStar overlay failed: 0x%x",(unsigned)(error ? error : errno));
    ca_overlay_hw_close(*out); *out=NULL; errno=EIO; return -1;
}
