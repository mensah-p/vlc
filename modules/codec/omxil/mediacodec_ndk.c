/*****************************************************************************
 * mediacodec_ndk.c: mc_api implementation using NDK
 *****************************************************************************
 * Copyright © 2015 VLC authors and VideoLAN, VideoLabs
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <jni.h>
#include <dlfcn.h>
#include <stdint.h>
#include <assert.h>

#include <vlc_common.h>

#include <OMX_Core.h>
#include <OMX_Component.h>
#include "omxil_utils.h"

#include "mediacodec.h"

#define THREAD_NAME "mediacodec_ndk"

/* Not in NdkMedia API but we need it since we send config data via input
 * buffers and not via "csd-*" buffers from AMediaFormat */
#define AMEDIACODEC_FLAG_CODEC_CONFIG 2

/*****************************************************************************
 * NdkMediaError.h
 *****************************************************************************/

typedef enum {
    AMEDIA_OK = 0,

    AMEDIA_ERROR_BASE                  = -10000,
    AMEDIA_ERROR_UNKNOWN               = AMEDIA_ERROR_BASE,
    AMEDIA_ERROR_MALFORMED             = AMEDIA_ERROR_BASE - 1,
    AMEDIA_ERROR_UNSUPPORTED           = AMEDIA_ERROR_BASE - 2,
    AMEDIA_ERROR_INVALID_OBJECT        = AMEDIA_ERROR_BASE - 3,
    AMEDIA_ERROR_INVALID_PARAMETER     = AMEDIA_ERROR_BASE - 4,

    AMEDIA_DRM_ERROR_BASE              = -20000,
    AMEDIA_DRM_NOT_PROVISIONED         = AMEDIA_DRM_ERROR_BASE - 1,
    AMEDIA_DRM_RESOURCE_BUSY           = AMEDIA_DRM_ERROR_BASE - 2,
    AMEDIA_DRM_DEVICE_REVOKED          = AMEDIA_DRM_ERROR_BASE - 3,
    AMEDIA_DRM_SHORT_BUFFER            = AMEDIA_DRM_ERROR_BASE - 4,
    AMEDIA_DRM_SESSION_NOT_OPENED      = AMEDIA_DRM_ERROR_BASE - 5,
    AMEDIA_DRM_TAMPER_DETECTED         = AMEDIA_DRM_ERROR_BASE - 6,
    AMEDIA_DRM_VERIFY_FAILED           = AMEDIA_DRM_ERROR_BASE - 7,
    AMEDIA_DRM_NEED_KEY                = AMEDIA_DRM_ERROR_BASE - 8,
    AMEDIA_DRM_LICENSE_EXPIRED         = AMEDIA_DRM_ERROR_BASE - 9,

} media_status_t;

/*****************************************************************************
 * NdkMediaCodec.h
 *****************************************************************************/

struct AMediaCodec;
typedef struct AMediaCodec AMediaCodec;

struct AMediaCodecBufferInfo {
    int32_t offset;
    int32_t size;
    int64_t presentationTimeUs;
    uint32_t flags;
};
typedef struct AMediaCodecBufferInfo AMediaCodecBufferInfo;

enum {
    AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM = 4,
    AMEDIACODEC_CONFIGURE_FLAG_ENCODE = 1,
    AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED = -3,
    AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED = -2,
    AMEDIACODEC_INFO_TRY_AGAIN_LATER = -1
};

struct AMediaFormat;
typedef struct AMediaFormat AMediaFormat;

struct AMediaCrypto;
typedef struct AMediaCrypto AMediaCrypto;

/*****************************************************************************
 * Ndk symbols
 *****************************************************************************/

typedef AMediaCodec* (*pf_AMediaCodec_createCodecByName)(const char *name);

typedef media_status_t (*pf_AMediaCodec_configure)(AMediaCodec*,
        const AMediaFormat* format,
        ANativeWindow* surface,
        AMediaCrypto *crypto,
        uint32_t flags);

typedef media_status_t (*pf_AMediaCodec_start)(AMediaCodec*);

typedef media_status_t (*pf_AMediaCodec_stop)(AMediaCodec*);

typedef media_status_t (*pf_AMediaCodec_flush)(AMediaCodec*);

typedef media_status_t (*pf_AMediaCodec_delete)(AMediaCodec*);

typedef AMediaFormat* (*pf_AMediaCodec_getOutputFormat)(AMediaCodec*);

typedef ssize_t (*pf_AMediaCodec_dequeueInputBuffer)(AMediaCodec*,
        int64_t timeoutUs);

typedef uint8_t* (*pf_AMediaCodec_getInputBuffer)(AMediaCodec*,
        size_t idx, size_t *out_size);

typedef media_status_t (*pf_AMediaCodec_queueInputBuffer)(AMediaCodec*,
        size_t idx, off_t offset, size_t size, uint64_t time, uint32_t flags);

typedef ssize_t (*pf_AMediaCodec_dequeueOutputBuffer)(AMediaCodec*,
        AMediaCodecBufferInfo *info, int64_t timeoutUs);

typedef uint8_t* (*pf_AMediaCodec_getOutputBuffer)(AMediaCodec*,
        size_t idx, size_t *out_size);

typedef media_status_t (*pf_AMediaCodec_releaseOutputBuffer)(AMediaCodec*,
        size_t idx, bool render);

typedef AMediaFormat *(*pf_AMediaFormat_new)();
typedef media_status_t (*pf_AMediaFormat_delete)(AMediaFormat*);

typedef void (*pf_AMediaFormat_setString)(AMediaFormat*,
        const char* name, const char* value);

typedef void (*pf_AMediaFormat_setInt32)(AMediaFormat*,
        const char* name, int32_t value);

typedef bool (*pf_AMediaFormat_getInt32)(AMediaFormat*,
        const char *name, int32_t *out);

struct syms
{
    struct {
        pf_AMediaCodec_createCodecByName createCodecByName;
        pf_AMediaCodec_configure configure;
        pf_AMediaCodec_start start;
        pf_AMediaCodec_stop stop;
        pf_AMediaCodec_flush flush;
        pf_AMediaCodec_delete delete;
        pf_AMediaCodec_getOutputFormat getOutputFormat;
        pf_AMediaCodec_dequeueInputBuffer dequeueInputBuffer;
        pf_AMediaCodec_getInputBuffer getInputBuffer;
        pf_AMediaCodec_queueInputBuffer queueInputBuffer;
        pf_AMediaCodec_dequeueOutputBuffer dequeueOutputBuffer;
        pf_AMediaCodec_getOutputBuffer getOutputBuffer;
        pf_AMediaCodec_releaseOutputBuffer releaseOutputBuffer;
    } AMediaCodec;
    struct {
        pf_AMediaFormat_new new;
        pf_AMediaFormat_delete delete;
        pf_AMediaFormat_setString setString;
        pf_AMediaFormat_setInt32 setInt32;
        pf_AMediaFormat_getInt32 getInt32;
    } AMediaFormat;
};
static struct syms syms;

struct members
{
    const char *name;
    int offset;
    bool critical;
};
static struct members members[] =
{
#define OFF(x) offsetof(struct syms, AMediaCodec.x)
    { "AMediaCodec_createCodecByName", OFF(createCodecByName), true },
    { "AMediaCodec_configure", OFF(configure), true },
    { "AMediaCodec_start", OFF(start), true },
    { "AMediaCodec_stop", OFF(stop), true },
    { "AMediaCodec_flush", OFF(flush), true },
    { "AMediaCodec_delete", OFF(delete), true },
    { "AMediaCodec_getOutputFormat", OFF(getOutputFormat), true },
    { "AMediaCodec_dequeueInputBuffer", OFF(dequeueInputBuffer), true },
    { "AMediaCodec_getInputBuffer", OFF(getInputBuffer), true },
    { "AMediaCodec_queueInputBuffer", OFF(queueInputBuffer), true },
    { "AMediaCodec_dequeueOutputBuffer", OFF(dequeueOutputBuffer), true },
    { "AMediaCodec_getOutputBuffer", OFF(getOutputBuffer), true },
    { "AMediaCodec_releaseOutputBuffer", OFF(releaseOutputBuffer), true },
#undef OFF
#define OFF(x) offsetof(struct syms, AMediaFormat.x)
    { "AMediaFormat_new", OFF(new), true },
    { "AMediaFormat_delete", OFF(delete), true },
    { "AMediaFormat_setString", OFF(setString), true },
    { "AMediaFormat_setInt32", OFF(setInt32), true },
    { "AMediaFormat_getInt32", OFF(getInt32), true },
#undef OFF
    { NULL, 0, false }
};
#undef OFF

/* Initialize all symbols.
 * Done only one time during the first initialisation */
static bool
InitSymbols(mc_api *api)
{
    static vlc_mutex_t lock = VLC_STATIC_MUTEX;
    static int i_init_state = -1;
    bool ret;

    vlc_mutex_lock(&lock);

    if (i_init_state != -1)
        goto end;

    i_init_state = 0;

    void *ndk_handle = dlopen("libmediandk.so", RTLD_NOW);
    if (!ndk_handle)
        goto end;

    for (int i = 0; members[i].name; i++)
    {
        void *sym = dlsym(ndk_handle, members[i].name);
        if (!sym && members[i].critical)
        {
            dlclose(ndk_handle);
            goto end;
        }
        *(void **)((uint8_t*)&syms + members[i].offset) = sym;
    }

    i_init_state = 1;
end:
    ret = i_init_state == 1;
    if (!ret)
        msg_Err(api->p_obj, "MediaCodec NDK init failed");

    vlc_mutex_unlock(&lock);
    return ret;
}

/****************************************************************************
 * Local prototypes
 ****************************************************************************/

struct mc_api_sys
{
    AMediaCodec* p_codec;
    AMediaFormat* p_format;
};

/*****************************************************************************
 * Stop
 *****************************************************************************/
static int Stop(mc_api *api)
{
    mc_api_sys *p_sys = api->p_sys;

    api->b_direct_rendering = false;

    if (p_sys->p_codec)
    {
        if (api->b_started)
        {
            syms.AMediaCodec.stop(p_sys->p_codec);
            api->b_started = false;
        }
        syms.AMediaCodec.delete(p_sys->p_codec);
        p_sys->p_codec = NULL;
    }
    if (p_sys->p_format)
    {
        syms.AMediaFormat.delete(p_sys->p_format);
        p_sys->p_format = NULL;
    }

    msg_Dbg(api->p_obj, "MediaCodec via NDK closed");
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Start
 *****************************************************************************/
static int Start(mc_api *api, AWindowHandler *p_awh, const char *psz_name,
                 const char *psz_mime, int i_width, int i_height, int i_angle)
{
    mc_api_sys *p_sys = api->p_sys;
    int i_ret = VLC_EGENERIC;
    ANativeWindow *p_anw = NULL;

    p_sys->p_codec = syms.AMediaCodec.createCodecByName(psz_name);
    if (!p_sys->p_codec)
    {
        msg_Err(api->p_obj, "AMediaCodec.createCodecByName for %s failed", psz_name);
        goto error;
    }

    p_sys->p_format = syms.AMediaFormat.new();
    if (!p_sys->p_format)
    {
        msg_Err(api->p_obj, "AMediaFormat.new failed");
        goto error;
    }

    syms.AMediaFormat.setString(p_sys->p_format, "mime", psz_mime);
    syms.AMediaFormat.setInt32(p_sys->p_format, "width", i_width);
    syms.AMediaFormat.setInt32(p_sys->p_format, "height", i_height);
    syms.AMediaFormat.setInt32(p_sys->p_format, "rotation-degrees", i_angle);
    syms.AMediaFormat.setInt32(p_sys->p_format, "encoder", 0);

    if (p_awh)
        p_anw = AWindowHandler_getANativeWindow(p_awh, AWindow_Video);

    if (syms.AMediaCodec.configure(p_sys->p_codec, p_sys->p_format,
                                   p_anw, NULL, 0) != AMEDIA_OK)
    {
        msg_Err(api->p_obj, "AMediaCodec.configure failed");
        goto error;
    }
    if (syms.AMediaCodec.start(p_sys->p_codec) != AMEDIA_OK)
    {
        msg_Err(api->p_obj, "AMediaCodec.start failed");
        goto error;
    }

    api->b_started = true;
    api->b_direct_rendering = !!p_anw;
    i_ret = VLC_SUCCESS;

    msg_Dbg(api->p_obj, "MediaCodec via NDK opened");
error:
    if (i_ret != VLC_SUCCESS)
        Stop(api);
    return i_ret;
}

/*****************************************************************************
 * Flush
 *****************************************************************************/
static int Flush(mc_api *api)
{
    mc_api_sys *p_sys = api->p_sys;

    if (syms.AMediaCodec.flush(p_sys->p_codec) == AMEDIA_OK)
        return VLC_SUCCESS;
    else
        return VLC_EGENERIC;
}

/*****************************************************************************
 * PutInput
 *****************************************************************************/
static int PutInput(mc_api *api, const void *p_buf, size_t i_size,
                    mtime_t i_ts, bool b_config, mtime_t i_timeout)
{
    mc_api_sys *p_sys = api->p_sys;
    ssize_t i_index;
    uint8_t *p_mc_buf;
    size_t i_mc_size;
    int i_flags = b_config ? AMEDIACODEC_FLAG_CODEC_CONFIG : 0;

    i_index = syms.AMediaCodec.dequeueInputBuffer(p_sys->p_codec, i_timeout);
    if (i_index < 0)
    {
        if (i_index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
            return 0;
        else
        {
            msg_Err(api->p_obj, "AMediaCodec.dequeueInputBuffer failed");
            return VLC_EGENERIC;
        }
    }

    p_mc_buf = syms.AMediaCodec.getInputBuffer(p_sys->p_codec,
                                               i_index, &i_mc_size);
    if (!p_mc_buf)
        return VLC_EGENERIC;

    if (i_mc_size > i_size)
        i_mc_size = i_size;
    memcpy(p_mc_buf, p_buf, i_mc_size);

    if (syms.AMediaCodec.queueInputBuffer(p_sys->p_codec, i_index, 0, i_mc_size,
                                          i_ts, i_flags) == AMEDIA_OK)
        return 1;
    else
    {
        msg_Err(api->p_obj, "AMediaCodec.queueInputBuffer failed");
        return VLC_EGENERIC;
    }
}

static int32_t GetFormatInteger(AMediaFormat *p_format, const char *psz_name)
{
    int32_t i_out = 0;
    syms.AMediaFormat.getInt32(p_format, psz_name, &i_out);
    return i_out;
}

/*****************************************************************************
 * GetOutput
 *****************************************************************************/
static int GetOutput(mc_api *api, mc_api_out *p_out, mtime_t i_timeout)
{
    mc_api_sys *p_sys = api->p_sys;
    AMediaCodecBufferInfo info;
    ssize_t i_index;

    i_index = syms.AMediaCodec.dequeueOutputBuffer(p_sys->p_codec, &info,
                                                   i_timeout);
    if (i_index >= 0)
    {
        p_out->type = MC_OUT_TYPE_BUF;
        p_out->u.buf.i_index = i_index;

        p_out->u.buf.i_ts = info.presentationTimeUs;

        if (api->b_direct_rendering)
        {
            p_out->u.buf.p_ptr = NULL;
            p_out->u.buf.i_size = 0;
        }
        else
        {
            size_t i_mc_size;
            uint8_t *p_mc_buf = syms.AMediaCodec.getOutputBuffer(p_sys->p_codec,
                                                                 i_index,
                                                                 &i_mc_size);
            if (!p_mc_buf)
            {
                msg_Err(api->p_obj, "AMediaCodec.getOutputBuffer failed");
                return VLC_EGENERIC;
            }
            p_out->u.buf.p_ptr = p_mc_buf + info.offset;
            p_out->u.buf.i_size = i_mc_size;
        }
        return 1;
    }
    else if (i_index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
    {
        AMediaFormat *format = syms.AMediaCodec.getOutputFormat(p_sys->p_codec);

        p_out->type = MC_OUT_TYPE_CONF;
        p_out->u.conf.width = GetFormatInteger(format, "width");
        p_out->u.conf.height = GetFormatInteger(format, "height");
        p_out->u.conf.stride = GetFormatInteger(format, "stride");
        p_out->u.conf.slice_height = GetFormatInteger(format, "slice-height");
        p_out->u.conf.pixel_format = GetFormatInteger(format, "color-format");
        p_out->u.conf.crop_left = GetFormatInteger(format, "crop-left");
        p_out->u.conf.crop_top = GetFormatInteger(format, "crop-top");
        p_out->u.conf.crop_right = GetFormatInteger(format, "crop-right");
        p_out->u.conf.crop_bottom = GetFormatInteger(format, "crop-bottom");
        return 1;
    }
    else if (i_index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED
          || i_index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
    {
        return 0;
    }
    else
    {
        msg_Err(api->p_obj, "AMediaCodec.dequeueOutputBuffer failed");
        return VLC_EGENERIC;
    }
}

/*****************************************************************************
 * ReleaseOutput
 *****************************************************************************/
static int ReleaseOutput(mc_api *api, int i_index, bool b_render)
{
    mc_api_sys *p_sys = api->p_sys;

    if (syms.AMediaCodec.releaseOutputBuffer(p_sys->p_codec, i_index, b_render)
                                             == AMEDIA_OK)
        return VLC_SUCCESS;
    else
        return VLC_EGENERIC;
}


/*****************************************************************************
 * Clean
 *****************************************************************************/
static void Clean(mc_api *api)
{
    free(api->p_sys);
}

/*****************************************************************************
 * MediaCodecNdk_Init
 *****************************************************************************/
int MediaCodecNdk_Init(mc_api *api)
{
    if (!InitSymbols(api))
        return VLC_EGENERIC;

    api->p_sys = calloc(1, sizeof(mc_api_sys));
    if (!api->p_sys)
        return VLC_EGENERIC;

    api->clean = Clean;
    api->start = Start;
    api->stop = Stop;
    api->flush = Flush;
    api->put_in = PutInput;
    api->get_out = GetOutput;
    api->release_out = ReleaseOutput;

    api->b_support_interlaced = true;
    return VLC_SUCCESS;
}
