/*
 * Copyright 2011 Joakim Sindholt <opensource@zhasha.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE. */

#include "basetexture9.h"
#include "device9.h"

/* For UploadSelf: */
#include "texture9.h"
#include "cubetexture9.h"
#include "volumetexture9.h"

#ifdef DEBUG
#include "nine_pipe.h"
#include "nine_dump.h"
#endif

#include "util/u_format.h"
#include "util/u_gen_mipmap.h"

#define DBG_CHANNEL DBG_BASETEXTURE

HRESULT
NineBaseTexture9_ctor( struct NineBaseTexture9 *This,
                       struct NineUnknownParams *pParams,
                       D3DRESOURCETYPE Type,
                       D3DPOOL Pool )
{
    BOOL alloc = (Pool == D3DPOOL_DEFAULT) && !This->base.resource &&
        (This->format != D3DFMT_NULL);
    HRESULT hr;
    DWORD usage = This->base.usage;

    user_assert(!(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) ||
                Pool == D3DPOOL_DEFAULT, D3DERR_INVALIDCALL);
    user_assert(!(usage & D3DUSAGE_DYNAMIC) ||
                Pool != D3DPOOL_MANAGED, D3DERR_INVALIDCALL);

    hr = NineResource9_ctor(&This->base, pParams, alloc, Type, Pool);
    if (FAILED(hr))
        return hr;

    This->pipe = pParams->device->pipe;
    This->mipfilter = (This->base.usage & D3DUSAGE_AUTOGENMIPMAP) ?
        D3DTEXF_LINEAR : D3DTEXF_NONE;
    This->lod = 0;
    This->lod_resident = -1;
    This->shadow = This->format != D3DFMT_INTZ && util_format_has_depth(
        util_format_description(This->base.info.format));

    list_inithead(&This->list);

    return D3D_OK;
}

void
NineBaseTexture9_dtor( struct NineBaseTexture9 *This )
{
    DBG("This=%p\n", This);

    pipe_sampler_view_reference(&This->view[0], NULL);
    pipe_sampler_view_reference(&This->view[1], NULL);

    list_del(&This->list),

    NineResource9_dtor(&This->base);
}

DWORD WINAPI
NineBaseTexture9_SetLOD( struct NineBaseTexture9 *This,
                         DWORD LODNew )
{
    DWORD old = This->lod;

    user_assert(This->base.pool == D3DPOOL_MANAGED, 0);

    This->lod = MIN2(LODNew, This->base.info.last_level);

    if (This->lod != old && This->bind_count && LIST_IS_EMPTY(&This->list))
       list_add(&This->list, &This->base.base.device->update_textures);

    return old;
}

DWORD WINAPI
NineBaseTexture9_GetLOD( struct NineBaseTexture9 *This )
{
    return This->lod;
}

DWORD WINAPI
NineBaseTexture9_GetLevelCount( struct NineBaseTexture9 *This )
{
    if (This->base.usage & D3DUSAGE_AUTOGENMIPMAP)
        return 1;
    return This->base.info.last_level + 1;
}

HRESULT WINAPI
NineBaseTexture9_SetAutoGenFilterType( struct NineBaseTexture9 *This,
                                       D3DTEXTUREFILTERTYPE FilterType )
{
    if (!(This->base.usage & D3DUSAGE_AUTOGENMIPMAP))
        return D3D_OK;
    user_assert(FilterType != D3DTEXF_NONE, D3DERR_INVALIDCALL);

    This->mipfilter = FilterType;

    return D3D_OK;
}

D3DTEXTUREFILTERTYPE WINAPI
NineBaseTexture9_GetAutoGenFilterType( struct NineBaseTexture9 *This )
{
    return This->mipfilter;
}

HRESULT
NineBaseTexture9_UploadSelf( struct NineBaseTexture9 *This )
{
    HRESULT hr;
    unsigned last_level = This->base.info.last_level;
    unsigned l;

    DBG("This=%p dirty=%i type=%s\n", This, This->dirty,
        nine_D3DRTYPE_to_str(This->base.type));

    assert(This->base.pool == D3DPOOL_MANAGED);

    if (This->base.usage & D3DUSAGE_AUTOGENMIPMAP)
        last_level = 0; /* TODO: What if level 0 is not resident ? */

    if (This->lod_resident != This->lod) {
        struct pipe_resource *res;

        DBG("updating LOD from %u to %u ...\n", This->lod_resident, This->lod);

        pipe_sampler_view_reference(&This->view[0], NULL);
        pipe_sampler_view_reference(&This->view[1], NULL);

        if (This->bind_count) {
            /* mark state dirty */
            struct nine_state *state = &This->base.base.device->state;
            unsigned s;
            for (s = 0; s < NINE_MAX_SAMPLERS; ++s)
                if (state->texture[s] == This)
                    state->changed.texture |= 1 << s;
            if (state->changed.texture)
                state->changed.group |= NINE_STATE_TEXTURE;
        }

        hr = NineBaseTexture9_CreatePipeResource(This, This->lod_resident != -1);
        if (FAILED(hr))
            return hr;
        res = This->base.resource;

        if (This->lod_resident == -1) /* no levels were resident */
            This->lod_resident = This->base.info.last_level + 1;

        if (This->base.type == D3DRTYPE_TEXTURE) {
            struct NineTexture9 *tex = NineTexture9(This);
            struct pipe_box box;

            /* Mark uninitialized levels as dirty. */
            box.x = box.y = box.z = 0;
            box.depth = 1;
            for (l = This->lod; l < This->lod_resident; ++l) {
                box.width = u_minify(This->base.info.width0, l);
                box.height = u_minify(This->base.info.height0, l);
                NineSurface9_AddDirtyRect(tex->surfaces[l], &box);
            }
            for (l = 0; l < This->lod; ++l)
                NineSurface9_SetResource(tex->surfaces[l], NULL, -1);
            for (; l <= This->base.info.last_level; ++l)
                NineSurface9_SetResource(tex->surfaces[l], res, l - This->lod);
        } else
        if (This->base.type == D3DRTYPE_CUBETEXTURE) {
            struct NineCubeTexture9 *tex = NineCubeTexture9(This);
            struct pipe_box box;
            unsigned z;

            /* Mark uninitialized levels as dirty. */
            box.x = box.y = box.z = 0;
            box.depth = 1;
            for (l = This->lod; l < This->lod_resident; ++l) {
                box.width = u_minify(This->base.info.width0, l);
                box.height = u_minify(This->base.info.height0, l);
                for (z = 0; z < 6; ++z)
                    NineSurface9_AddDirtyRect(tex->surfaces[l * 6 + z], &box);
            }
            for (l = 0; l < This->lod; ++l) {
                for (z = 0; z < 6; ++z)
                    NineSurface9_SetResource(tex->surfaces[l * 6 + z],
                                             NULL, -1);
            }
            for (; l <= This->base.info.last_level; ++l) {
                for (z = 0; z < 6; ++z)
                    NineSurface9_SetResource(tex->surfaces[l * 6 + z],
                                             res, l - This->lod);
            }
        } else
        if (This->base.type == D3DRTYPE_VOLUMETEXTURE) {
            struct NineVolumeTexture9 *tex = NineVolumeTexture9(This);
            struct pipe_box box;

            /* Mark uninitialized levels as dirty. */
            box.x = box.y = box.z = 0;
            for (l = This->lod; l < This->lod_resident; ++l) {
                box.width = u_minify(This->base.info.width0, l);
                box.height = u_minify(This->base.info.height0, l);
                box.depth = u_minify(This->base.info.depth0, l);
                NineVolume9_AddDirtyRegion(tex->volumes[l], &box);
            }
            for (l = 0; l < This->lod; ++l)
                NineVolume9_SetResource(tex->volumes[l], NULL, -1);
            for (; l <= This->base.info.last_level; ++l)
                NineVolume9_SetResource(tex->volumes[l], res, l - This->lod);
        } else {
            assert(!"invalid texture type");
        }

        if (This->lod < This->lod_resident)
            This->dirty = TRUE;
        This->lod_resident = This->lod;
    }
    if (!This->dirty)
        return D3D_OK;

    if (This->base.type == D3DRTYPE_TEXTURE) {
        struct NineTexture9 *tex = NineTexture9(This);
        struct pipe_box box;
        box.z = 0;
        box.depth = 1;

        DBG("TEXTURE: dirty rect=(%u,%u) (%ux%u)\n",
            tex->dirty_rect.x, tex->dirty_rect.y,
            tex->dirty_rect.width, tex->dirty_rect.height);

        if (tex->dirty_rect.width) {
            for (l = 0; l <= last_level; ++l) {
                u_box_minify_2d(&box, &tex->dirty_rect, l);
                NineSurface9_AddDirtyRect(tex->surfaces[l], &box);
            }
            memset(&tex->dirty_rect, 0, sizeof(tex->dirty_rect));
            tex->dirty_rect.depth = 1;
        }
        for (l = This->lod; l <= last_level; ++l)
            NineSurface9_UploadSelf(tex->surfaces[l]);
    } else
    if (This->base.type == D3DRTYPE_CUBETEXTURE) {
        struct NineCubeTexture9 *tex = NineCubeTexture9(This);
        unsigned z;
        struct pipe_box box;
        box.z = 0;
        box.depth = 1;

        for (z = 0; z < 6; ++z) {
            DBG("FACE[%u]: dirty rect=(%u,%u) (%ux%u)\n", z,
                tex->dirty_rect[z].x, tex->dirty_rect[z].y,
                tex->dirty_rect[z].width, tex->dirty_rect[z].height);

            if (tex->dirty_rect[z].width) {
                for (l = 0; l <= last_level; ++l) {
                    u_box_minify_2d(&box, &tex->dirty_rect[z], l);
                    NineSurface9_AddDirtyRect(tex->surfaces[l * 6 + z], &box);
                }
                memset(&tex->dirty_rect[z], 0, sizeof(tex->dirty_rect[z]));
                tex->dirty_rect[z].depth = 1;
            }
            for (l = This->lod; l <= last_level; ++l)
                NineSurface9_UploadSelf(tex->surfaces[l * 6 + z]);
        }
    } else
    if (This->base.type == D3DRTYPE_VOLUMETEXTURE) {
        struct NineVolumeTexture9 *tex = NineVolumeTexture9(This);
        struct pipe_box box;

        DBG("VOLUME: dirty_box=(%u,%u,%u) (%ux%ux%u)\n",
            tex->dirty_box.x, tex->dirty_box.y, tex->dirty_box.y,
            tex->dirty_box.width, tex->dirty_box.height, tex->dirty_box.depth);

        if (tex->dirty_box.width) {
            for (l = 0; l <= last_level; ++l) {
                u_box_minify(&box, &tex->dirty_box, l);
                NineVolume9_AddDirtyRegion(tex->volumes[l], &tex->dirty_box);
            }
            memset(&tex->dirty_box, 0, sizeof(tex->dirty_box));
        }
        for (l = This->lod; l <= last_level; ++l)
            NineVolume9_UploadSelf(tex->volumes[l]);
    } else {
        assert(!"invalid texture type");
    }
    This->dirty = FALSE;

    if (This->base.usage & D3DUSAGE_AUTOGENMIPMAP)
        This->dirty_mip = TRUE;
    /* TODO: if dirty only because of lod change, only generate added levels */

    DBG("DONE, generate mip maps = %i\n", This->dirty_mip);
    return D3D_OK;
}

void WINAPI
NineBaseTexture9_GenerateMipSubLevels( struct NineBaseTexture9 *This )
{
    struct pipe_resource *resource = This->base.resource;

    unsigned base_level = 0;
    unsigned last_level = This->base.info.last_level - This->lod;
    unsigned first_layer = 0;
    unsigned last_layer;
    unsigned filter = This->mipfilter == D3DTEXF_POINT ? PIPE_TEX_FILTER_NEAREST
                                                       : PIPE_TEX_FILTER_LINEAR;
    DBG("This=%p\n", This);

    if (This->base.pool == D3DPOOL_MANAGED)
        NineBaseTexture9_UploadSelf(This);
    if (!This->dirty_mip)
        return;
    if (This->lod) {
        ERR("AUTOGENMIPMAP if level 0 is not resident not supported yet !\n");
        return;
    }

    if (!This->view[0])
        NineBaseTexture9_UpdateSamplerView(This, 0);

    last_layer = util_max_layer(This->view[0]->texture, base_level);

    util_gen_mipmap(This->pipe, resource,
                    resource->format, base_level, last_level,
                    first_layer, last_layer, filter);

    This->dirty_mip = FALSE;

    NineDevice9_RestoreNonCSOState(This->base.base.device, ~0x3);
}

HRESULT
NineBaseTexture9_CreatePipeResource( struct NineBaseTexture9 *This,
                                     BOOL CopyData )
{
    struct pipe_context *pipe = This->pipe;
    struct pipe_screen *screen = This->base.info.screen;
    struct pipe_resource templ;
    unsigned l, m;
    struct pipe_resource *res;
    struct pipe_resource *old = This->base.resource;

    DBG("This=%p lod=%u last_level=%u\n", This,
        This->lod, This->base.info.last_level);

    assert(This->base.pool == D3DPOOL_MANAGED);

    templ = This->base.info;

    if (This->lod) {
        templ.width0 = u_minify(templ.width0, This->lod);
        templ.height0 = u_minify(templ.height0, This->lod);
        templ.depth0 = u_minify(templ.depth0, This->lod);
    }
    templ.last_level = This->base.info.last_level - This->lod;

    if (old) {
        /* LOD might have changed. */
        if (old->width0 == templ.width0 &&
            old->height0 == templ.height0 &&
            old->depth0 == templ.depth0)
            return D3D_OK;
    }

    res = screen->resource_create(screen, &templ);
    if (!res)
        return D3DERR_OUTOFVIDEOMEMORY;
    This->base.resource = res;

    if (old && CopyData) { /* Don't return without releasing old ! */
        struct pipe_box box;
        box.x = 0;
        box.y = 0;
        box.z = 0;

        l = (This->lod < This->lod_resident) ? This->lod_resident - This->lod : 0;
        m = (This->lod < This->lod_resident) ? 0 : This->lod - This->lod_resident;

        box.width = u_minify(templ.width0, l);
        box.height = u_minify(templ.height0, l);
        box.depth = u_minify(templ.depth0, l);

        for (; l <= templ.last_level; ++l, ++m) {
            pipe->resource_copy_region(pipe,
                                       res, l, 0, 0, 0,
                                       old, m, &box);
            box.width = u_minify(box.width, 1);
            box.height = u_minify(box.height, 1);
            box.depth = u_minify(box.depth, 1);
        }
    }
    pipe_resource_reference(&old, NULL);

    return D3D_OK;
}

HRESULT
NineBaseTexture9_UpdateSamplerView( struct NineBaseTexture9 *This,
                                    const int sRGB )
{
    const struct util_format_description *desc;
    struct pipe_context *pipe = This->pipe;
    struct pipe_resource *resource = This->base.resource;
    struct pipe_sampler_view templ;
    uint8_t swizzle[4];

    if (unlikely(!resource)) {
	if (unlikely(This->format == D3DFMT_NULL))
            return D3D_OK;
        NineBaseTexture9_Dump(This);
    }
    assert(resource);

    pipe_sampler_view_reference(&This->view[sRGB], NULL);

    swizzle[0] = PIPE_SWIZZLE_RED;
    swizzle[1] = PIPE_SWIZZLE_GREEN;
    swizzle[2] = PIPE_SWIZZLE_BLUE;
    swizzle[3] = PIPE_SWIZZLE_ALPHA;
    desc = util_format_description(resource->format);
    if (desc->colorspace == UTIL_FORMAT_COLORSPACE_ZS) {
        /* ZZZ1 -> 0Z01 (see end of docs/source/tgsi.rst)
         * XXX: but it's wrong
        swizzle[0] = PIPE_SWIZZLE_ZERO;
        swizzle[2] = PIPE_SWIZZLE_ZERO; */
    } else
    if (desc->swizzle[0] == UTIL_FORMAT_SWIZZLE_X &&
        desc->swizzle[3] == UTIL_FORMAT_SWIZZLE_1) {
        /* R001/RG01 -> R111/RG11 */
        if (desc->swizzle[1] == UTIL_FORMAT_SWIZZLE_0)
            swizzle[1] = PIPE_SWIZZLE_ONE;
        if (desc->swizzle[2] == UTIL_FORMAT_SWIZZLE_0)
            swizzle[2] = PIPE_SWIZZLE_ONE;
    }
    /* but 000A remains unchanged */

    templ.format = sRGB ? util_format_srgb(resource->format) : resource->format;
    templ.u.tex.first_layer = 0;
    templ.u.tex.last_layer = (resource->target == PIPE_TEXTURE_CUBE) ?
        5 : (This->base.info.depth0 - 1);
    templ.u.tex.first_level = 0;
    templ.u.tex.last_level = resource->last_level;
    templ.swizzle_r = swizzle[0];
    templ.swizzle_g = swizzle[1];
    templ.swizzle_b = swizzle[2];
    templ.swizzle_a = swizzle[3];

    This->view[sRGB] = pipe->create_sampler_view(pipe, resource, &templ);

    DBG("sampler view = %p(resource = %p)\n", This->view[sRGB], resource);

    return This->view ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

void WINAPI
NineBaseTexture9_PreLoad( struct NineBaseTexture9 *This )
{
    if (This->dirty && This->base.pool == D3DPOOL_MANAGED)
        NineBaseTexture9_UploadSelf(This);
}

#ifdef DEBUG
void
NineBaseTexture9_Dump( struct NineBaseTexture9 *This )
{
    DBG("\nNineBaseTexture9(%p->%p/%p): Pool=%s Type=%s Usage=%s\n"
        "Format=%s Dims=%ux%ux%u/%u LastLevel=%u Lod=%u(%u)\n", This,
        This->base.resource, This->base.data,
        nine_D3DPOOL_to_str(This->base.pool),
        nine_D3DRTYPE_to_str(This->base.type),
        nine_D3DUSAGE_to_str(This->base.usage),
        d3dformat_to_string(This->format),
        This->base.info.width0, This->base.info.height0, This->base.info.depth0,
        This->base.info.array_size, This->base.info.last_level,
        This->lod, This->lod_resident);
}
#endif /* DEBUG */
