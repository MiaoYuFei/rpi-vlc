/*****************************************************************************
 * dxa9.c : DXVA2 GPU surface conversion module for vlc
 *****************************************************************************
 * Copyright (C) 2015 VLC authors, VideoLAN and VideoLabs
 * $Id: 281465ac34240f3a14a7d7cddbaf78aa20f3497c $
 *
 * Authors: Steve Lhomme <robux4@gmail.com>
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

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_picture.h>
#include <vlc_modules.h>

#include "copy.h"

#include <windows.h>
#include <d3d9.h>
#include "d3d9_fmt.h"

struct filter_sys_t {
    /* GPU to CPU */
    copy_cache_t      cache;

    /* CPU to GPU */
    IDirect3DDevice9  *d3ddev;
    filter_t          *filter;
    picture_t         *staging;

    HINSTANCE         hd3d_dll;
};

static bool GetLock(filter_t *p_filter, LPDIRECT3DSURFACE9 d3d,
                    D3DLOCKED_RECT *p_lock, D3DSURFACE_DESC *p_desc)
{
    if (unlikely(FAILED( IDirect3DSurface9_GetDesc(d3d, p_desc))))
        return false;

    /* */
    if (FAILED(IDirect3DSurface9_LockRect(d3d, p_lock, NULL, D3DLOCK_READONLY))) {
        msg_Err(p_filter, "Failed to lock surface");
        return false;
    }

    return true;
}

static void DXA9_YV12(filter_t *p_filter, picture_t *src, picture_t *dst)
{
    copy_cache_t *p_copy_cache = (copy_cache_t*) p_filter->p_sys;
    picture_sys_t *p_sys = &((struct va_pic_context *)src->context)->picsys;

    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lock;
    if (!GetLock(p_filter, p_sys->surface, &lock, &desc))
        return;

    if (dst->format.i_chroma == VLC_CODEC_I420) {
        uint8_t *tmp = dst->p[1].p_pixels;
        dst->p[1].p_pixels = dst->p[2].p_pixels;
        dst->p[2].p_pixels = tmp;
    }

    if (desc.Format == MAKEFOURCC('Y','V','1','2') ||
        desc.Format == MAKEFOURCC('I','M','C','3')) {
        bool imc3 = desc.Format == MAKEFOURCC('I','M','C','3');
        size_t chroma_pitch = imc3 ? lock.Pitch : (lock.Pitch / 2);

        const size_t pitch[3] = {
            lock.Pitch,
            chroma_pitch,
            chroma_pitch,
        };

        const uint8_t *plane[3] = {
            (uint8_t*)lock.pBits,
            (uint8_t*)lock.pBits + pitch[0] * desc.Height,
            (uint8_t*)lock.pBits + pitch[0] * desc.Height
                                 + pitch[1] * desc.Height / 2,
        };

        if (imc3) {
            const uint8_t *V = plane[1];
            plane[1] = plane[2];
            plane[2] = V;
        }
        Copy420_P_to_P(dst, plane, pitch, src->format.i_height, p_copy_cache);
    } else if (desc.Format == MAKEFOURCC('N','V','1','2')) {
        const uint8_t *plane[2] = {
            lock.pBits,
            (uint8_t*)lock.pBits + lock.Pitch * desc.Height
        };
        const size_t  pitch[2] = {
            lock.Pitch,
            lock.Pitch,
        };
        Copy420_SP_to_P(dst, plane, pitch,
                        src->format.i_visible_height + src->format.i_y_offset, p_copy_cache);
        picture_SwapUV(dst);
    } else {
        msg_Err(p_filter, "Unsupported DXA9 conversion from 0x%08X to YV12", desc.Format);
    }

    if (dst->format.i_chroma == VLC_CODEC_I420) {
        uint8_t *tmp = dst->p[1].p_pixels;
        dst->p[1].p_pixels = dst->p[2].p_pixels;
        dst->p[2].p_pixels = tmp;
    }

    /* */
    IDirect3DSurface9_UnlockRect(p_sys->surface);
}

static void DXA9_NV12(filter_t *p_filter, picture_t *src, picture_t *dst)
{
    copy_cache_t *p_copy_cache = (copy_cache_t*) p_filter->p_sys;
    picture_sys_t *p_sys = &((struct va_pic_context *)src->context)->picsys;

    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lock;
    if (!GetLock(p_filter, p_sys->surface, &lock, &desc))
        return;

    if (desc.Format == MAKEFOURCC('N','V','1','2')) {
        const uint8_t *plane[2] = {
            lock.pBits,
            (uint8_t*)lock.pBits + lock.Pitch * desc.Height
        };
        size_t  pitch[2] = {
            lock.Pitch,
            lock.Pitch,
        };
        Copy420_SP_to_SP(dst, plane, pitch,
                         src->format.i_visible_height + src->format.i_y_offset, p_copy_cache);
    } else {
        msg_Err(p_filter, "Unsupported DXA9 conversion from 0x%08X to NV12", desc.Format);
    }

    /* */
    IDirect3DSurface9_UnlockRect(p_sys->surface);
}

static void DestroyPicture(picture_t *picture)
{
    picture_sys_t *p_sys = picture->p_sys;
    ReleasePictureSys( p_sys );
    free(p_sys);
    free(picture);
}

static void DeleteFilter( filter_t * p_filter )
{
    if( p_filter->p_module )
        module_unneed( p_filter, p_filter->p_module );

    es_format_Clean( &p_filter->fmt_in );
    es_format_Clean( &p_filter->fmt_out );

    vlc_object_release( p_filter );
}

static picture_t *NewBuffer(filter_t *p_filter)
{
    filter_t *p_parent = p_filter->owner.sys;
    return p_parent->p_sys->staging;
}

static filter_t *CreateFilter( vlc_object_t *p_this, const es_format_t *p_fmt_in,
                               vlc_fourcc_t dst_chroma )
{
    filter_t *p_filter;

    p_filter = vlc_object_create( p_this, sizeof(filter_t) );
    if (unlikely(p_filter == NULL))
        return NULL;

    p_filter->b_allow_fmt_out_change = false;
    p_filter->owner.video.buffer_new = NewBuffer;
    p_filter->owner.sys = p_this;

    es_format_InitFromVideo( &p_filter->fmt_in,  &p_fmt_in->video );
    es_format_InitFromVideo( &p_filter->fmt_out, &p_fmt_in->video );
    p_filter->fmt_out.i_codec = p_filter->fmt_out.video.i_chroma = dst_chroma;
    p_filter->p_module = module_need( p_filter, "video converter", NULL, false );

    if( !p_filter->p_module )
    {
        msg_Dbg( p_filter, "no video converter found" );
        DeleteFilter( p_filter );
        return NULL;
    }

    return p_filter;
}

struct d3d_pic_context
{
    picture_context_t s;
};

static void d3d9_pic_context_destroy(struct picture_context_t *ctx)
{
    struct va_pic_context *pic_ctx = (struct va_pic_context*)ctx;
    ReleasePictureSys(&pic_ctx->picsys);
    free(pic_ctx);
}

static struct picture_context_t *d3d9_pic_context_copy(struct picture_context_t *ctx)
{
    struct va_pic_context *src_ctx = (struct va_pic_context*)ctx;
    struct va_pic_context *pic_ctx = calloc(1, sizeof(*pic_ctx));
    if (unlikely(pic_ctx==NULL))
        return NULL;
    pic_ctx->s.destroy = d3d9_pic_context_destroy;
    pic_ctx->s.copy    = d3d9_pic_context_copy;
    pic_ctx->picsys = src_ctx->picsys;
    AcquirePictureSys(&pic_ctx->picsys);
    return &pic_ctx->s;
}

static void YV12_D3D9(filter_t *p_filter, picture_t *src, picture_t *dst)
{
    filter_sys_t *sys = (filter_sys_t*) p_filter->p_sys;
    picture_sys_t *p_sys = dst->p_sys;

    D3DSURFACE_DESC texDesc;
    IDirect3DSurface9_GetDesc( p_sys->surface, &texDesc);

    D3DLOCKED_RECT d3drect;
    HRESULT hr = IDirect3DSurface9_LockRect(sys->staging->p_sys->surface, &d3drect, NULL, 0);
    if (FAILED(hr))
        return;

    picture_UpdatePlanes(sys->staging, d3drect.pBits, d3drect.Pitch);

    picture_Hold( src );
    sys->filter->pf_video_filter(sys->filter, src);

    IDirect3DSurface9_UnlockRect(sys->staging->p_sys->surface);

    RECT visibleSource = {
        .right = dst->format.i_width, .bottom = dst->format.i_height,
    };
    IDirect3DDevice9_StretchRect( sys->d3ddev,
                                  sys->staging->p_sys->surface, &visibleSource,
                                  dst->p_sys->surface, &visibleSource,
                                  D3DTEXF_NONE );

    if (dst->context == NULL)
    {
        struct va_pic_context *pic_ctx = calloc(1, sizeof(*pic_ctx));
        if (likely(pic_ctx))
        {
            pic_ctx->s.destroy = d3d9_pic_context_destroy;
            pic_ctx->s.copy    = d3d9_pic_context_copy;
            pic_ctx->picsys = *dst->p_sys;
            AcquirePictureSys(&pic_ctx->picsys);
            dst->context = &pic_ctx->s;
        }
    }
}

VIDEO_FILTER_WRAPPER (DXA9_YV12)
VIDEO_FILTER_WRAPPER (DXA9_NV12)
VIDEO_FILTER_WRAPPER (YV12_D3D9)

static int OpenConverter( vlc_object_t *obj )
{
    filter_t *p_filter = (filter_t *)obj;
    HINSTANCE hd3d_dll = NULL;
    int err = VLC_EGENERIC;

    if ( p_filter->fmt_in.video.i_chroma != VLC_CODEC_D3D9_OPAQUE )
        return VLC_EGENERIC;

    if ( p_filter->fmt_in.video.i_height != p_filter->fmt_out.video.i_height
         || p_filter->fmt_in.video.i_width != p_filter->fmt_out.video.i_width )
        return VLC_EGENERIC;

    switch( p_filter->fmt_out.video.i_chroma ) {
    case VLC_CODEC_I420:
    case VLC_CODEC_YV12:
        p_filter->pf_video_filter = DXA9_YV12_Filter;
        break;
    case VLC_CODEC_NV12:
        p_filter->pf_video_filter = DXA9_NV12_Filter;
        break;
    default:
        return VLC_EGENERIC;
    }

    hd3d_dll = LoadLibrary(TEXT("D3D9.DLL"));
    if (unlikely(!hd3d_dll)) {
        msg_Warn(p_filter, "cannot load d3d9.dll, aborting");
        goto done;
    }

    filter_sys_t *p_sys = calloc(1, sizeof(filter_sys_t));
    if (!p_sys) {
         err = VLC_ENOMEM;
         goto done;
    }
    CopyInitCache(&p_sys->cache, p_filter->fmt_in.video.i_width );
    p_filter->p_sys = p_sys;
    err = VLC_SUCCESS;

done:
    if (err != VLC_SUCCESS)
    {
        if (hd3d_dll)
            FreeLibrary(hd3d_dll);
    }
    return err;
}

static int OpenFromCPU( vlc_object_t *obj )
{
    filter_t *p_filter = (filter_t *)obj;
    int err = VLC_EGENERIC;
    LPDIRECT3DSURFACE9 texture = NULL;
    IDirect3DDevice9  *d3ddev = NULL;
    filter_t *p_cpu_filter = NULL;
    picture_t *p_dst = NULL;
    HINSTANCE hd3d_dll = NULL;
    video_format_t fmt_staging;

    if ( p_filter->fmt_out.video.i_chroma != VLC_CODEC_D3D9_OPAQUE )
        return VLC_EGENERIC;

    if ( p_filter->fmt_in.video.i_height != p_filter->fmt_out.video.i_height
         || p_filter->fmt_in.video.i_width != p_filter->fmt_out.video.i_width )
        return VLC_EGENERIC;

    switch( p_filter->fmt_in.video.i_chroma ) {
    case VLC_CODEC_I420:
    case VLC_CODEC_YV12:
        p_filter->pf_video_filter = YV12_D3D9_Filter;
        break;
    default:
        return VLC_EGENERIC;
    }

    picture_t *peek = filter_NewPicture(p_filter);
    if (peek == NULL)
        return VLC_EGENERIC;
    if (!peek->p_sys)
    {
        msg_Dbg(p_filter, "D3D9 opaque without a texture");
        return VLC_EGENERIC;
    }

    video_format_Init(&fmt_staging, 0);
    D3DSURFACE_DESC texDesc;
    IDirect3DSurface9_GetDesc( peek->p_sys->surface, &texDesc);
    vlc_fourcc_t d3d_fourcc = texDesc.Format;
    if (d3d_fourcc == 0)
        goto done;

    if ( p_filter->fmt_in.video.i_chroma != d3d_fourcc )
    {
        picture_resource_t res;
        res.pf_destroy = DestroyPicture;
        res.p_sys = calloc(1, sizeof(picture_sys_t));
        if (res.p_sys == NULL) {
            err = VLC_ENOMEM;
            goto done;
        }

        video_format_Copy(&fmt_staging, &p_filter->fmt_out.video);
        fmt_staging.i_chroma = d3d_fourcc;
        fmt_staging.i_height = texDesc.Height;
        fmt_staging.i_width  = texDesc.Width;

        p_dst = picture_NewFromResource(&fmt_staging, &res);
        if (p_dst == NULL) {
            msg_Err(p_filter, "Failed to map create the temporary picture.");
            goto done;
        }
        picture_Setup(p_dst, &p_dst->format);

        IDirect3DSurface9_GetDevice(peek->p_sys->surface, &d3ddev);
        HRESULT hr = IDirect3DDevice9_CreateOffscreenPlainSurface(d3ddev,
                                                          p_dst->format.i_width,
                                                          p_dst->format.i_height,
                                                          texDesc.Format,
                                                          D3DPOOL_DEFAULT,
                                                          &texture,
                                                          NULL);
        if (FAILED(hr)) {
            msg_Err(p_filter, "Failed to create a %4.4s staging texture to extract surface pixels (hr=0x%0lx)", (char *)texDesc.Format, hr );
            goto done;
        }
        res.p_sys->surface = texture;
        IDirect3DSurface9_AddRef(texture);

        p_cpu_filter = CreateFilter(VLC_OBJECT(p_filter), &p_filter->fmt_in, p_dst->format.i_chroma);
        if (!p_cpu_filter)
            goto done;
    }

    hd3d_dll = LoadLibrary(TEXT("D3D9.DLL"));
    if (unlikely(!hd3d_dll)) {
        msg_Warn(p_filter, "cannot load d3d9.dll, aborting");
        goto done;
    }

    filter_sys_t *p_sys = calloc(1, sizeof(filter_sys_t));
    if (!p_sys) {
         err = VLC_ENOMEM;
         goto done;
    }
    p_sys->d3ddev    = d3ddev;
    p_sys->filter    = p_cpu_filter;
    p_sys->staging   = p_dst;
    p_sys->hd3d_dll = hd3d_dll;
    p_filter->p_sys = p_sys;
    err = VLC_SUCCESS;

done:
    video_format_Clean(&fmt_staging);
    picture_Release(peek);
    if (err != VLC_SUCCESS)
    {
        if (d3ddev)
            IDirect3DDevice9_Release(d3ddev);
        if (p_cpu_filter)
            DeleteFilter( p_cpu_filter );
        if (texture)
            IDirect3DSurface9_Release(texture);
        if (hd3d_dll)
            FreeLibrary(hd3d_dll);
    }
    return err;
}

static void CloseConverter( vlc_object_t *obj )
{
    filter_t *p_filter = (filter_t *)obj;
    copy_cache_t *p_copy_cache = (copy_cache_t*) p_filter->p_sys;
    CopyCleanCache(p_copy_cache);
    free( p_copy_cache );
    p_filter->p_sys = NULL;
}

static void CloseFromCPU( vlc_object_t *obj )
{
    filter_t *p_filter = (filter_t *)obj;
    filter_sys_t *p_sys = (filter_sys_t*) p_filter->p_sys;
    DeleteFilter(p_sys->filter);
    picture_Release(p_sys->staging);
    IDirect3DDevice9_Release(p_sys->d3ddev);
    FreeLibrary(p_sys->hd3d_dll);
    free( p_sys );
    p_filter->p_sys = NULL;
}

/*****************************************************************************
 * Module descriptor.
 *****************************************************************************/
vlc_module_begin ()
    set_description( N_("Conversions from DxVA2 to YUV") )
    set_capability( "video converter", 10 )
    set_callbacks( OpenConverter, CloseConverter )
    add_submodule()
        set_callbacks( OpenFromCPU, CloseFromCPU )
        set_capability( "video converter", 10 )
vlc_module_end ()
