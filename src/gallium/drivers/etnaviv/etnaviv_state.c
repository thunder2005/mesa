/*
 * Copyright (c) 2012-2015 Etnaviv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Wladimir J. van der Laan <laanwj@gmail.com>
 *    Christian Gmeiner <christian.gmeiner@gmail.com>
 */

#include "etnaviv_state.h"

#include "hw/common.xml.h"

#include "etnaviv_context.h"
#include "etnaviv_surface.h"
#include "etnaviv_translate.h"
#include "etnaviv_util.h"
#include "util/u_math.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

static void etna_set_blend_color(struct pipe_context *pctx, const struct pipe_blend_color *bc)
{
    struct etna_context *ctx = etna_context(pctx);
    struct compiled_blend_color *cs = &ctx->blend_color;

    cs->PE_ALPHA_BLEND_COLOR =
            VIVS_PE_ALPHA_BLEND_COLOR_R(etna_cfloat_to_uint8(bc->color[0])) |
            VIVS_PE_ALPHA_BLEND_COLOR_G(etna_cfloat_to_uint8(bc->color[1])) |
            VIVS_PE_ALPHA_BLEND_COLOR_B(etna_cfloat_to_uint8(bc->color[2])) |
            VIVS_PE_ALPHA_BLEND_COLOR_A(etna_cfloat_to_uint8(bc->color[3]));
    ctx->dirty |= ETNA_DIRTY_BLEND_COLOR;
}

static void etna_set_stencil_ref(struct pipe_context *pctx, const struct pipe_stencil_ref *sr)
{
    struct etna_context *ctx = etna_context(pctx);
    struct compiled_stencil_ref *cs = &ctx->stencil_ref;

    ctx->stencil_ref_s = *sr;

    cs->PE_STENCIL_CONFIG =
            VIVS_PE_STENCIL_CONFIG_REF_FRONT(sr->ref_value[0]);
            /* rest of bits weaved in from depth_stencil_alpha */
    cs->PE_STENCIL_CONFIG_EXT =
            VIVS_PE_STENCIL_CONFIG_EXT_REF_BACK(sr->ref_value[0]);
    ctx->dirty |= ETNA_DIRTY_STENCIL_REF;
}

static void etna_set_clip_state(struct pipe_context *pctx, const struct pipe_clip_state *pcs)
{
    /* NOOP */
}

static void etna_set_sample_mask(struct pipe_context *pctx, unsigned sample_mask)
{
    struct etna_context *ctx = etna_context(pctx);

    ctx->sample_mask = sample_mask;
    ctx->dirty |= ETNA_DIRTY_SAMPLE_MASK;
}

static void etna_set_framebuffer_state(struct pipe_context *pctx, const struct pipe_framebuffer_state *sv)
{
    struct etna_context *ctx = etna_context(pctx);
    struct compiled_framebuffer_state *cs = &ctx->framebuffer;
    int nr_samples_color = -1;
    int nr_samples_depth = -1;

    /* Set up TS as well. Warning: this state is used by both the RS and PE */
    uint32_t ts_mem_config = 0;
    if (sv->nr_cbufs > 0) /* at least one color buffer? */
    {
        struct etna_surface *cbuf = etna_surface(sv->cbufs[0]);
        bool color_supertiled = (cbuf->layout & 2)!=0;
        assert(cbuf->layout & 1); /* Cannot render to linear surfaces */

        pipe_surface_reference(&cs->cbuf, &cbuf->base);
        cs->PE_COLOR_FORMAT =
                VIVS_PE_COLOR_FORMAT_FORMAT(translate_rt_format(cbuf->base.format, false)) |
                (color_supertiled ? VIVS_PE_COLOR_FORMAT_SUPER_TILED : 0);
                /* XXX VIVS_PE_COLOR_FORMAT_OVERWRITE and the rest comes from blend_state / depth_stencil_alpha */
                /* merged with depth_stencil_alpha */
        if ((cbuf->surf.offset & 63) || (((cbuf->surf.stride*4) & 63) && cbuf->surf.height > 4))
        {
            /* XXX Must make temporary surface here.
             * Need the same mechanism on gc2000 when we want to do mipmap generation by
             * rendering to levels > 1 due to multitiled / tiled conversion.
             */
            BUG("Alignment error, trying to render to offset %08x with tile stride %i",
                    cbuf->surf.offset, cbuf->surf.stride*4);
        }

        struct etna_resource *res = etna_resource(cbuf->base.texture);
        struct etna_bo *bo = res->bo;

        if (ctx->specs.pixel_pipes == 1)
        {
            cs->PE_COLOR_ADDR.bo = bo;
            cs->PE_COLOR_ADDR.offset = cbuf->surf.offset;
            cs->PE_COLOR_ADDR.flags = ETNA_RELOC_READ | ETNA_RELOC_WRITE;
        }
        else if (ctx->specs.pixel_pipes == 2)
        {
            cs->PE_PIPE_COLOR_ADDR[0].bo = bo;
            cs->PE_PIPE_COLOR_ADDR[0].offset = res->levels[0].offset;
            cs->PE_PIPE_COLOR_ADDR[0].flags = ETNA_RELOC_READ | ETNA_RELOC_WRITE;

            cs->PE_PIPE_COLOR_ADDR[1].bo = bo;
            cs->PE_PIPE_COLOR_ADDR[1].offset = res->levels[0].offset + (res->levels[0].size / 2);
            cs->PE_PIPE_COLOR_ADDR[1].flags = ETNA_RELOC_READ | ETNA_RELOC_WRITE;
        }
        cs->PE_COLOR_STRIDE = cbuf->surf.stride;

        if (cbuf->surf.ts_size)
        {
            struct etna_bo *ts_bo = etna_resource(cbuf->base.texture)->ts_bo;
            ts_mem_config |= VIVS_TS_MEM_CONFIG_COLOR_FAST_CLEAR;
            cs->TS_COLOR_CLEAR_VALUE = cbuf->level->clear_value;

            cs->TS_COLOR_STATUS_BASE.bo = ts_bo;
            cs->TS_COLOR_STATUS_BASE.offset = cbuf->surf.ts_offset;
            cs->TS_COLOR_STATUS_BASE.flags = ETNA_RELOC_READ | ETNA_RELOC_WRITE;

            cs->TS_COLOR_SURFACE_BASE.bo = bo;
            cs->TS_COLOR_SURFACE_BASE.offset = cbuf->surf.offset;
            cs->TS_COLOR_SURFACE_BASE.flags = ETNA_RELOC_READ | ETNA_RELOC_WRITE;
        }

        /* MSAA */
        if (cbuf->base.texture->nr_samples > 1)
            ts_mem_config |= VIVS_TS_MEM_CONFIG_MSAA | translate_msaa_format(cbuf->base.format, false);

        nr_samples_color = cbuf->base.texture->nr_samples;
    } else {
        pipe_surface_reference(&cs->cbuf, NULL);
        cs->PE_COLOR_FORMAT = 0; /* Is this enough to render without color? */
    }

    if (sv->zsbuf != NULL)
    {
        struct etna_surface *zsbuf = etna_surface(sv->zsbuf);

        pipe_surface_reference(&cs->zsbuf, &zsbuf->base);
        assert(zsbuf->layout & 1); /* Cannot render to linear surfaces */
        uint32_t depth_format = translate_depth_format(zsbuf->base.format, false);
        unsigned depth_bits = depth_format == VIVS_PE_DEPTH_CONFIG_DEPTH_FORMAT_D16 ? 16 : 24;
        bool depth_supertiled = (zsbuf->layout & 2)!=0;

        cs->PE_DEPTH_CONFIG =
                depth_format |
                (depth_supertiled ? VIVS_PE_DEPTH_CONFIG_SUPER_TILED : 0) |
                VIVS_PE_DEPTH_CONFIG_DEPTH_MODE_Z;
                /* VIVS_PE_DEPTH_CONFIG_ONLY_DEPTH */
                /* merged with depth_stencil_alpha */

        struct etna_resource *res = etna_resource(zsbuf->base.texture);
        struct etna_bo *bo = res->bo;

        if (ctx->specs.pixel_pipes == 1)
        {
            cs->PE_DEPTH_ADDR.bo = bo;
            cs->PE_DEPTH_ADDR.offset = zsbuf->surf.offset;
            cs->PE_DEPTH_ADDR.flags = ETNA_RELOC_READ | ETNA_RELOC_WRITE;
        }
        else if (ctx->specs.pixel_pipes == 2)
        {
            cs->PE_PIPE_DEPTH_ADDR[0].bo = bo;
            cs->PE_PIPE_DEPTH_ADDR[0].offset = res->levels[0].offset;
            cs->PE_PIPE_DEPTH_ADDR[0].flags = ETNA_RELOC_READ | ETNA_RELOC_WRITE;

            cs->PE_PIPE_DEPTH_ADDR[1].bo = bo;
            cs->PE_PIPE_DEPTH_ADDR[1].offset = res->levels[0].offset + (res->levels[0].size / 2);
            cs->PE_PIPE_DEPTH_ADDR[1].flags = ETNA_RELOC_READ | ETNA_RELOC_WRITE;
	}

        cs->PE_DEPTH_STRIDE = zsbuf->surf.stride;
        cs->PE_HDEPTH_CONTROL = VIVS_PE_HDEPTH_CONTROL_FORMAT_DISABLED;
        cs->PE_DEPTH_NORMALIZE = etna_f32_to_u32(exp2f(depth_bits) - 1.0f);

        if (zsbuf->surf.ts_size)
        {
            struct etna_bo *ts_bo = etna_resource(zsbuf->base.texture)->ts_bo;
            ts_mem_config |= VIVS_TS_MEM_CONFIG_DEPTH_FAST_CLEAR;
            cs->TS_DEPTH_CLEAR_VALUE = zsbuf->level->clear_value;

            cs->TS_DEPTH_STATUS_BASE.bo = ts_bo;
            cs->TS_DEPTH_STATUS_BASE.offset = zsbuf->surf.ts_offset;
            cs->TS_DEPTH_STATUS_BASE.flags = ETNA_RELOC_READ | ETNA_RELOC_WRITE;

            cs->TS_DEPTH_SURFACE_BASE.bo = bo;
            cs->TS_DEPTH_SURFACE_BASE.offset = zsbuf->surf.offset;
            cs->TS_DEPTH_SURFACE_BASE.flags = ETNA_RELOC_READ | ETNA_RELOC_WRITE;
        }

        ts_mem_config |= (depth_bits == 16 ? VIVS_TS_MEM_CONFIG_DEPTH_16BPP : 0);

        /* MSAA */
        if (zsbuf->base.texture->nr_samples > 1)
            /* XXX VIVS_TS_MEM_CONFIG_DEPTH_COMPRESSION;
             * Disable without MSAA for now, as it causes corruption in glquake. */
            ts_mem_config |= VIVS_TS_MEM_CONFIG_DEPTH_COMPRESSION;

        nr_samples_depth = zsbuf->base.texture->nr_samples;
    } else {
        pipe_surface_reference(&cs->zsbuf, NULL);
        cs->PE_DEPTH_CONFIG = VIVS_PE_DEPTH_CONFIG_DEPTH_MODE_NONE;
    }

    /* MSAA setup */
    if (nr_samples_depth != -1 && nr_samples_color != -1 && nr_samples_depth != nr_samples_color)
    {
        BUG("Number of samples in color and depth texture must match (%i and %i respectively)",
                nr_samples_color, nr_samples_depth);
    }

    switch(MAX2(nr_samples_depth, nr_samples_color))
    {
    case 0:
    case 1: /* Are 0 and 1 samples allowed? */
        cs->GL_MULTI_SAMPLE_CONFIG = VIVS_GL_MULTI_SAMPLE_CONFIG_MSAA_SAMPLES_NONE;
        cs->msaa_mode = false;
        break;
    case 2:
        cs->GL_MULTI_SAMPLE_CONFIG = VIVS_GL_MULTI_SAMPLE_CONFIG_MSAA_SAMPLES_2X;
        cs->msaa_mode = true; /* Add input to PS */
        cs->RA_MULTISAMPLE_UNK00E04 = 0x0;
        cs->RA_MULTISAMPLE_UNK00E10[0] = 0x0000aa22;
        cs->RA_CENTROID_TABLE[0] = 0x66aa2288;
        cs->RA_CENTROID_TABLE[1] = 0x88558800;
        cs->RA_CENTROID_TABLE[2] = 0x88881100;
        cs->RA_CENTROID_TABLE[3] = 0x33888800;
        break;
    case 4:
        cs->GL_MULTI_SAMPLE_CONFIG = VIVS_GL_MULTI_SAMPLE_CONFIG_MSAA_SAMPLES_4X;
        cs->msaa_mode = true; /* Add input to PS */
        cs->RA_MULTISAMPLE_UNK00E04 = 0x0;
        cs->RA_MULTISAMPLE_UNK00E10[0] = 0xeaa26e26;
        cs->RA_MULTISAMPLE_UNK00E10[1] = 0xe6ae622a;
        cs->RA_MULTISAMPLE_UNK00E10[2] = 0xaaa22a22;
        cs->RA_CENTROID_TABLE[0] = 0x4a6e2688;
        cs->RA_CENTROID_TABLE[1] = 0x888888a2;
        cs->RA_CENTROID_TABLE[2] = 0x888888ea;
        cs->RA_CENTROID_TABLE[3] = 0x888888c6;
        cs->RA_CENTROID_TABLE[4] = 0x46622a88;
        cs->RA_CENTROID_TABLE[5] = 0x888888ae;
        cs->RA_CENTROID_TABLE[6] = 0x888888e6;
        cs->RA_CENTROID_TABLE[7] = 0x888888ca;
        cs->RA_CENTROID_TABLE[8] = 0x262a2288;
        cs->RA_CENTROID_TABLE[9] = 0x886688a2;
        cs->RA_CENTROID_TABLE[10] = 0x888866aa;
        cs->RA_CENTROID_TABLE[11] = 0x668888a6;
        break;
    }

    /* Scissor setup */
    cs->SE_SCISSOR_LEFT = 0; /* affected by rasterizer and scissor state as well */
    cs->SE_SCISSOR_TOP = 0;
    cs->SE_SCISSOR_RIGHT = (sv->width << 16)-1;
    cs->SE_SCISSOR_BOTTOM = (sv->height << 16)-1;

    cs->TS_MEM_CONFIG = ts_mem_config;

    ctx->framebuffer_s = *sv; /* keep copy of original structure */
    ctx->dirty |= ETNA_DIRTY_FRAMEBUFFER;
}

static void etna_set_polygon_stipple(struct pipe_context *pctx, const struct pipe_poly_stipple *stipple)
{
    /* NOP */
}

static void etna_set_scissor_states( struct pipe_context *pctx,
                          unsigned start_slot,
                          unsigned num_scissors,
                          const struct pipe_scissor_state *ss)
{
    struct etna_context *ctx = etna_context(pctx);
    struct compiled_scissor_state *cs = &ctx->scissor;

    ctx->scissor_s = *ss;
    cs->SE_SCISSOR_LEFT = (ss->minx << 16);
    cs->SE_SCISSOR_TOP = (ss->miny << 16);
    cs->SE_SCISSOR_RIGHT = (ss->maxx << 16)-1;
    cs->SE_SCISSOR_BOTTOM = (ss->maxy << 16)-1;
    /* note that this state is only used when rasterizer_state->scissor is on */
    ctx->dirty |= ETNA_DIRTY_SCISSOR;
}

static void etna_set_viewport_states(struct pipe_context *pctx,
                           unsigned start_slot,
                           unsigned num_scissors,
                           const struct pipe_viewport_state *vs)
{
    struct etna_context *ctx = etna_context(pctx);
    struct compiled_viewport_state *cs = &ctx->viewport;

    ctx->viewport_s = *vs;
    /**
     * For Vivante GPU, viewport z transformation is 0..1 to 0..1 instead of -1..1 to 0..1.
     * scaling and translation to 0..1 already happened, so remove that
     *
     * z' = (z * 2 - 1) * scale + translate
     *    = z * (2 * scale) + (translate - scale)
     *
     * scale' = 2 * scale
     * translate' = translate - scale
     */
    cs->PA_VIEWPORT_SCALE_X = etna_f32_to_fixp16(vs->scale[0]); /* must be fixp as v4 state deltas assume it is */
    cs->PA_VIEWPORT_SCALE_Y = etna_f32_to_fixp16(vs->scale[1]);
    cs->PA_VIEWPORT_SCALE_Z = etna_f32_to_u32(vs->scale[2] * 2.0f);
    cs->PA_VIEWPORT_OFFSET_X = etna_f32_to_fixp16(vs->translate[0]);
    cs->PA_VIEWPORT_OFFSET_Y = etna_f32_to_fixp16(vs->translate[1]);
    cs->PA_VIEWPORT_OFFSET_Z = etna_f32_to_u32(vs->translate[2] - vs->scale[2]);

    /* Compute scissor rectangle (fixp) from viewport.
     * Make sure left is always < right and top always < bottom.
     */
    cs->SE_SCISSOR_LEFT = etna_f32_to_fixp16(MAX2(vs->translate[0] - vs->scale[0], 0.0f));
    cs->SE_SCISSOR_TOP = etna_f32_to_fixp16(MAX2(vs->translate[1] - vs->scale[1], 0.0f));
    cs->SE_SCISSOR_RIGHT = etna_f32_to_fixp16(MAX2(vs->translate[0] + vs->scale[0], 0.0f));
    cs->SE_SCISSOR_BOTTOM = etna_f32_to_fixp16(MAX2(vs->translate[1] + vs->scale[1], 0.0f));
    if (cs->SE_SCISSOR_LEFT > cs->SE_SCISSOR_RIGHT)
    {
        uint32_t tmp = cs->SE_SCISSOR_RIGHT;
        cs->SE_SCISSOR_RIGHT = cs->SE_SCISSOR_LEFT;
        cs->SE_SCISSOR_LEFT = tmp;
    }
    if (cs->SE_SCISSOR_TOP > cs->SE_SCISSOR_BOTTOM)
    {
        uint32_t tmp = cs->SE_SCISSOR_BOTTOM;
        cs->SE_SCISSOR_BOTTOM = cs->SE_SCISSOR_TOP;
        cs->SE_SCISSOR_TOP = tmp;
    }

    cs->PE_DEPTH_NEAR = etna_f32_to_u32(0.0); /* not affected if depth mode is Z (as in GL) */
    cs->PE_DEPTH_FAR = etna_f32_to_u32(1.0);
    ctx->dirty |= ETNA_DIRTY_VIEWPORT;
}

static void etna_set_vertex_buffers( struct pipe_context *pctx,
                           unsigned start_slot,
                           unsigned num_buffers,
                           const struct pipe_vertex_buffer *vb)
{
    struct etna_context *ctx = etna_context(pctx);
    assert((start_slot + num_buffers) <= PIPE_MAX_ATTRIBS);
    struct pipe_vertex_buffer zero_vb = {};

    for (unsigned idx = 0; idx < num_buffers; ++idx)
    {
        unsigned slot = start_slot + idx; /* copy from vb[idx] to priv->...[slot] */
        const struct pipe_vertex_buffer *vbi = vb ? &vb[idx] : &zero_vb;
        struct compiled_set_vertex_buffer *cs = &ctx->vertex_buffer[slot];

        assert(!vbi->user_buffer); /* XXX support user_buffer using etna_usermem_map */
        /* copy pipe_vertex_buffer structure and take reference */
        ctx->vertex_buffer_s[slot].stride = vbi->stride;
        ctx->vertex_buffer_s[slot].buffer_offset = vbi->buffer_offset;
        pipe_resource_reference(&ctx->vertex_buffer_s[slot].buffer, vbi->buffer);
        ctx->vertex_buffer_s[slot].user_buffer = vbi->user_buffer;

        if (vbi->buffer) /* GPU buffer */
        {
            cs->FE_VERTEX_STREAM_BASE_ADDR.bo = etna_resource(vbi->buffer)->bo;
            cs->FE_VERTEX_STREAM_BASE_ADDR.offset = vbi->buffer_offset;
            cs->FE_VERTEX_STREAM_BASE_ADDR.flags = ETNA_RELOC_READ;
        }

        cs->FE_VERTEX_STREAM_CONTROL = FE_VERTEX_STREAM_CONTROL_VERTEX_STRIDE(vbi->stride);
    }

    ctx->dirty |= ETNA_DIRTY_VERTEX_BUFFERS;
}

static void etna_set_index_buffer( struct pipe_context *pctx,
                         const struct pipe_index_buffer *ib)
{
    struct etna_context *ctx = etna_context(pctx);

    if (ib) {
        pipe_resource_reference(&ctx->index_buffer.buffer, ib->buffer);
        ctx->index_buffer.index_size = ib->index_size;
        ctx->index_buffer.offset = ib->offset;
        ctx->index_buffer.user_buffer = ib->user_buffer;
    } else {
        pipe_resource_reference(&ctx->index_buffer.buffer, NULL);
	ctx->index_buffer.index_size = 0;
    }

    ctx->dirty |= ETNA_DIRTY_INDEX_BUFFER;
}

static void etna_blend_state_bind(struct pipe_context *pctx, void *bs)
{
    struct etna_context *ctx = etna_context(pctx);

    ctx->blend = bs;
    ctx->dirty |= ETNA_DIRTY_BLEND;
}

static void etna_blend_state_delete(struct pipe_context *pctx, void *bs)
{
    FREE(bs);
}

static void etna_rasterizer_state_bind(struct pipe_context *pctx, void *rs)
{
    struct etna_context *ctx = etna_context(pctx);

    ctx->rasterizer = rs;
    ctx->dirty |= ETNA_DIRTY_RASTERIZER;
}

static void etna_rasterizer_state_delete(struct pipe_context *pctx, void *rs)
{
    FREE(rs);
}

static void etna_zsa_state_bind(struct pipe_context *pctx, void *zs)
{
    struct etna_context *ctx = etna_context(pctx);

    ctx->zsa = zs;
    ctx->dirty |= ETNA_DIRTY_ZSA;
}

static void etna_zsa_state_delete(struct pipe_context *pctx, void *zs)
{
    FREE(zs);
}

/** Create vertex element states, which define a layout for fetching
 * vertices for rendering.
 */
static void *etna_vertex_elements_state_create(struct pipe_context *pctx,
                                      unsigned num_elements,
                                      const struct pipe_vertex_element *elements)
{
    struct etna_context *ctx = etna_context(pctx);
    struct compiled_vertex_elements_state *cs = CALLOC_STRUCT(compiled_vertex_elements_state);

    if (!cs)
        return NULL;

    /* XXX could minimize number of consecutive stretches here by sorting, and
     * permuting the inputs in shader or does Mesa do this already? */

    /* Check that vertex element binding is compatible with hardware; thus
     * elements[idx].vertex_buffer_index are < stream_count. If not, the binding
     * uses more streams than is supported, and u_vbuf should have done some reorganization
     * for compatibility.
     */

    /* TODO: does mesa this for us? */
    bool incompatible = false;
    for (unsigned idx = 0; idx < num_elements; ++idx)
    {
        if (elements[idx].vertex_buffer_index >= ctx->specs.stream_count ||
           elements[idx].instance_divisor > 0)
            incompatible = true;
    }

    cs->num_elements = num_elements;
    if (incompatible || num_elements == 0)
    {
        DBG("Error: zero vertex elements, or more vertex buffers used than supported");
        FREE(cs);
        return NULL;
    }

    unsigned start_offset = 0; /* start of current consecutive stretch */
    bool nonconsecutive = true; /* previous value of nonconsecutive */
    for (unsigned idx = 0; idx < num_elements; ++idx)
    {
        unsigned element_size = util_format_get_blocksize(elements[idx].src_format);
        unsigned end_offset = elements[idx].src_offset + element_size;

        if (nonconsecutive)
            start_offset = elements[idx].src_offset;

        assert(element_size != 0 && end_offset <= 256); /* maximum vertex size is 256 bytes */
        /* check whether next element is consecutive to this one */
        nonconsecutive = (idx == (num_elements-1)) ||
                    elements[idx+1].vertex_buffer_index != elements[idx].vertex_buffer_index ||
                    end_offset != elements[idx+1].src_offset;

        cs->FE_VERTEX_ELEMENT_CONFIG[idx] =
                (nonconsecutive ? VIVS_FE_VERTEX_ELEMENT_CONFIG_NONCONSECUTIVE : 0) |
                translate_vertex_format_type(elements[idx].src_format, false) |
                VIVS_FE_VERTEX_ELEMENT_CONFIG_NUM(util_format_get_nr_components(elements[idx].src_format)) |
                translate_vertex_format_normalize(elements[idx].src_format) |
                VIVS_FE_VERTEX_ELEMENT_CONFIG_ENDIAN(ENDIAN_MODE_NO_SWAP) |
                VIVS_FE_VERTEX_ELEMENT_CONFIG_STREAM(elements[idx].vertex_buffer_index) |
                VIVS_FE_VERTEX_ELEMENT_CONFIG_START(elements[idx].src_offset) |
                VIVS_FE_VERTEX_ELEMENT_CONFIG_END(end_offset - start_offset);
    }

    return cs;
}

static void etna_vertex_elements_state_delete(struct pipe_context *pctx, void *ve)
{
    FREE(ve);
}

static void etna_vertex_elements_state_bind(struct pipe_context *pctx, void *ve)
{
    struct etna_context *ctx = etna_context(pctx);

    ctx->vertex_elements = ve;
    ctx->dirty |= ETNA_DIRTY_VERTEX_ELEMENTS;
}

void etna_state_init(struct pipe_context *pctx)
{
    pctx->set_blend_color = etna_set_blend_color;
    pctx->set_stencil_ref = etna_set_stencil_ref;
    pctx->set_clip_state = etna_set_clip_state;
    pctx->set_sample_mask = etna_set_sample_mask;
    pctx->set_framebuffer_state = etna_set_framebuffer_state;
    pctx->set_polygon_stipple = etna_set_polygon_stipple;
    pctx->set_scissor_states = etna_set_scissor_states;
    pctx->set_viewport_states = etna_set_viewport_states;

    pctx->set_vertex_buffers = etna_set_vertex_buffers;
    pctx->set_index_buffer = etna_set_index_buffer;

    pctx->bind_blend_state = etna_blend_state_bind;
    pctx->delete_blend_state = etna_blend_state_delete;

    pctx->bind_rasterizer_state = etna_rasterizer_state_bind;
    pctx->delete_rasterizer_state = etna_rasterizer_state_delete;

    pctx->bind_depth_stencil_alpha_state = etna_zsa_state_bind;
    pctx->delete_depth_stencil_alpha_state = etna_zsa_state_delete;

    pctx->create_vertex_elements_state = etna_vertex_elements_state_create;
    pctx->delete_vertex_elements_state = etna_vertex_elements_state_delete;
    pctx->bind_vertex_elements_state = etna_vertex_elements_state_bind;
}