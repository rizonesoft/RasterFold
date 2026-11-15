#include "rfc_verify.h"

#include <stdlib.h>
#include <string.h>

static void *pv_default_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size ? size : 1u);
}

static void pv_default_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

/* Display-only sRGB preview transform. Purposely quantized to 8-bit sRGB with
 * alpha composited over a matte; outputs carry not_for_verification = 1 and are
 * never linked or queried by verification code. */

static uint16_t pv_read_u16_le(const uint8_t *p) {
    uint16_t v = 0u;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t pv_read_u32_le(const uint8_t *p) {
    uint32_t v = 0u;
    memcpy(&v, p, sizeof(v));
    return v;
}

static float pv_half_bits_to_float(uint16_t h) {
    uint32_t sign = ((uint32_t)(h & 0x8000u)) << 16;
    uint32_t exp = (uint32_t)((h >> 10) & 0x1fu);
    uint32_t mant = (uint32_t)(h & 0x03ffu);
    uint32_t bits = 0u;
    if (exp == 0u) {
        if (mant == 0u) {
            bits = sign;
        } else {
            int e = -14;
            while ((mant & 0x0400u) == 0u) {
                mant <<= 1;
                e--;
            }
            mant &= 0x03ffu;
            bits = sign | ((uint32_t)(e + 127) << 23) | (mant << 13);
        }
    } else if (exp == 31u) {
        bits = sign | 0x7f800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 112u) << 23) | (mant << 13);
    }
    float out = 0.0f;
    memcpy(&out, &bits, sizeof(out));
    return out;
}

static float pv_bits_to_float(uint32_t u) {
    float out = 0.0f;
    memcpy(&out, &u, sizeof(out));
    return out;
}

static uint8_t pv_linear_to_srgb8(float lin) {
    if (!(lin > 0.0f)) {
        return 0u;
    }
    if (lin >= 1.0f) {
        return 255u;
    }
    float s = 0.0f;
    if (lin <= 0.0031308f) {
        s = lin * 12.92f;
    } else {
        /* Compute y = lin^(1/2.4) = lin^(5/12) without libm via monotonic
         * bisection on y^12 <= lin^5 over y in [0, 1]. */
        float x2 = lin * lin;
        float x5 = x2 * x2 * lin;
        float lo = 0.0f;
        float hi = 1.0f;
        for (int i = 0; i < 16; i++) {
            float mid = 0.5f * (lo + hi);
            float m2 = mid * mid;
            float m4 = m2 * m2;
            float m12 = m4 * m4 * m4;
            if (m12 <= x5) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        s = 1.055f * (0.5f * (lo + hi)) - 0.055f;
    }
    if (s <= 0.0f) {
        return 0u;
    }
    if (s >= 1.0f) {
        return 255u;
    }
    int v = (int)(s * 255.0f + 0.5f);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

static float pv_srgb8_to_linear(uint8_t u) {
    float s = (float)u / 255.0f;
    if (s <= 0.04045f) {
        return s / 12.92f;
    }
    /* Compute lin = ((s + 0.055) / 1.055)^2.4 = x^(12/5) without libm via
     * bisection on y^5 <= x^12 over y in [0, 1]. */
    float x = (s + 0.055f) / 1.055f;
    float x2 = x * x;
    float x4 = x2 * x2;
    float x12 = x4 * x4 * x4;
    float lo = 0.0f;
    float hi = 1.0f;
    for (int i = 0; i < 20; i++) {
        float mid = 0.5f * (lo + hi);
        float m2 = mid * mid;
        float m5 = m2 * m2 * mid;
        if (m5 <= x12) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return 0.5f * (lo + hi);
}

static float pv_sanitize_linear(float v) {
    return (v == v && v > 0.0f) ? v : 0.0f;
}

static float pv_clamp_unit(float v) {
    if (!(v > 0.0f)) {
        return 0.0f;
    }
    if (v >= 1.0f) {
        return 1.0f;
    }
    return v;
}

static uint8_t pv_quantize_u16_to_u8(uint16_t v) {
    return (uint8_t)((v >> 8) & 0xffu);
}

static uint8_t pv_composite_channel(uint8_t fg,
                                    uint8_t alpha,
                                    uint8_t matte,
                                    int premult) {
    if (alpha == 255u) {
        return fg;
    }
    uint32_t inv_a = 255u - (uint32_t)alpha;
    uint32_t bg = (uint32_t)matte * inv_a;
    uint32_t fgp = premult ? ((uint32_t)fg * 255u) : ((uint32_t)fg * alpha);
    uint32_t sum = (fgp + bg + 127u) / 255u;
    if (sum > 255u) {
        sum = 255u;
    }
    return (uint8_t)sum;
}

static uint8_t pv_composite_linear_channel(float fg_lin,
                                           float alpha_lin,
                                           float matte_lin,
                                           int premult) {
    float fg_pre = premult ? fg_lin : ((alpha_lin > 0.0f) ? (fg_lin * alpha_lin) : 0.0f);
    float bg = (alpha_lin < 1.0f) ? (matte_lin * (1.0f - alpha_lin)) : 0.0f;
    return pv_linear_to_srgb8(fg_pre + bg);
}

static void pv_render_pixel(const rfc_frame *frame,
                            uint32_t x,
                            uint32_t y,
                            uint8_t matte_r,
                            uint8_t matte_g,
                            uint8_t matte_b,
                            float matte_r_lin,
                            float matte_g_lin,
                            float matte_b_lin,
                            uint8_t *out_rgba) {
    const rfc_layout *lay = &frame->layout;
    size_t bpp = rfc_sample_bytes(lay->format);
    const uint8_t *p =
        frame->data + (size_t)y * lay->stride_bytes + (size_t)x * bpp;
    int premult = (lay->alpha == RFC_ALPHA_PREMULTIPLIED) ? 1 : 0;

    switch (lay->format) {
        case RFC_SAMPLE_GRAY8:
        case RFC_SAMPLE_GRAY16:
        case RFC_SAMPLE_RGB8:
        case RFC_SAMPLE_RGBA8:
        case RFC_SAMPLE_RGB16:
        case RFC_SAMPLE_RGBA16: {
            uint8_t r = 0u, g = 0u, b = 0u, a = 255u;
            if (lay->format == RFC_SAMPLE_GRAY8) {
                r = g = b = p[0];
            } else if (lay->format == RFC_SAMPLE_GRAY16) {
                r = g = b = pv_quantize_u16_to_u8(pv_read_u16_le(p));
            } else if (lay->format == RFC_SAMPLE_RGB8) {
                r = p[0];
                g = p[1];
                b = p[2];
            } else if (lay->format == RFC_SAMPLE_RGBA8) {
                r = p[0];
                g = p[1];
                b = p[2];
                a = (lay->alpha == RFC_ALPHA_NONE) ? 255u : p[3];
            } else if (lay->format == RFC_SAMPLE_RGB16) {
                r = pv_quantize_u16_to_u8(pv_read_u16_le(p));
                g = pv_quantize_u16_to_u8(pv_read_u16_le(p + 2));
                b = pv_quantize_u16_to_u8(pv_read_u16_le(p + 4));
            } else {
                r = pv_quantize_u16_to_u8(pv_read_u16_le(p));
                g = pv_quantize_u16_to_u8(pv_read_u16_le(p + 2));
                b = pv_quantize_u16_to_u8(pv_read_u16_le(p + 4));
                a = (lay->alpha == RFC_ALPHA_NONE)
                        ? 255u
                        : pv_quantize_u16_to_u8(pv_read_u16_le(p + 6));
            }
            out_rgba[0] = pv_composite_channel(r, a, matte_r, premult);
            out_rgba[1] = pv_composite_channel(g, a, matte_g, premult);
            out_rgba[2] = pv_composite_channel(b, a, matte_b, premult);
            out_rgba[3] = 255u;
            return;
        }
        case RFC_SAMPLE_RGB_HALF:
        case RFC_SAMPLE_RGBA_HALF: {
            float lr = pv_sanitize_linear(pv_half_bits_to_float(pv_read_u16_le(p)));
            float lg = pv_sanitize_linear(pv_half_bits_to_float(pv_read_u16_le(p + 2)));
            float lb = pv_sanitize_linear(pv_half_bits_to_float(pv_read_u16_le(p + 4)));
            float la = (lay->format == RFC_SAMPLE_RGBA_HALF &&
                        lay->alpha != RFC_ALPHA_NONE)
                           ? pv_clamp_unit(pv_half_bits_to_float(pv_read_u16_le(p + 6)))
                           : 1.0f;
            out_rgba[0] = pv_composite_linear_channel(lr, la, matte_r_lin, premult);
            out_rgba[1] = pv_composite_linear_channel(lg, la, matte_g_lin, premult);
            out_rgba[2] = pv_composite_linear_channel(lb, la, matte_b_lin, premult);
            out_rgba[3] = 255u;
            return;
        }
        case RFC_SAMPLE_RGB_FLOAT:
        case RFC_SAMPLE_RGBA_FLOAT: {
            float lr = pv_sanitize_linear(pv_bits_to_float(pv_read_u32_le(p)));
            float lg = pv_sanitize_linear(pv_bits_to_float(pv_read_u32_le(p + 4)));
            float lb = pv_sanitize_linear(pv_bits_to_float(pv_read_u32_le(p + 8)));
            float la = (lay->format == RFC_SAMPLE_RGBA_FLOAT &&
                        lay->alpha != RFC_ALPHA_NONE)
                           ? pv_clamp_unit(pv_bits_to_float(pv_read_u32_le(p + 12)))
                           : 1.0f;
            out_rgba[0] = pv_composite_linear_channel(lr, la, matte_r_lin, premult);
            out_rgba[1] = pv_composite_linear_channel(lg, la, matte_g_lin, premult);
            out_rgba[2] = pv_composite_linear_channel(lb, la, matte_b_lin, premult);
            out_rgba[3] = 255u;
            return;
        }
        default:
            out_rgba[0] = out_rgba[1] = out_rgba[2] = 0u;
            out_rgba[3] = 255u;
            return;
    }
}

static void pv_map_coords(rfc_orientation ori,
                          uint32_t src_w,
                          uint32_t src_h,
                          uint32_t dx,
                          uint32_t dy,
                          uint32_t *sx,
                          uint32_t *sy) {
    switch (ori) {
        case RFC_ORIENTATION_FLIP_HORIZONTAL:
            *sx = src_w - 1u - dx;
            *sy = dy;
            return;
        case RFC_ORIENTATION_ROTATE_180:
            *sx = src_w - 1u - dx;
            *sy = src_h - 1u - dy;
            return;
        case RFC_ORIENTATION_FLIP_VERTICAL:
            *sx = dx;
            *sy = src_h - 1u - dy;
            return;
        case RFC_ORIENTATION_TRANSPOSE:
            *sx = dy;
            *sy = dx;
            return;
        case RFC_ORIENTATION_ROTATE_90:
            *sx = dy;
            *sy = src_h - 1u - dx;
            return;
        case RFC_ORIENTATION_TRANSVERSE:
            *sx = src_w - 1u - dy;
            *sy = src_h - 1u - dx;
            return;
        case RFC_ORIENTATION_ROTATE_270:
            *sx = src_w - 1u - dy;
            *sy = dx;
            return;
        default:
            *sx = dx;
            *sy = dy;
            return;
    }
}

rfc_status rfc_preview_render_srgb8(const rfc_frame *frame,
                                    uint8_t matte_r,
                                    uint8_t matte_g,
                                    uint8_t matte_b,
                                    int apply_orientation,
                                    const rfc_allocator *alloc,
                                    rfc_preview_frame *out_preview) {
    if (!frame || !frame->data || !out_preview) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    memset(out_preview, 0, sizeof(*out_preview));

    size_t min_len = 0u;
    rfc_status st = rfc_layout_validate(&frame->layout, &min_len);
    if (st != RFC_OK) {
        return st;
    }
    if (frame->data_len < min_len) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_allocator eff_alloc = {pv_default_alloc, pv_default_free, NULL};
    if (alloc) {
        eff_alloc = *alloc;
    }
    if (!eff_alloc.alloc || !eff_alloc.free) {
        return RFC_ERR_INVALID_ARGUMENT;
    }

    rfc_orientation ori = apply_orientation ? frame->metadata.orientation
                                            : RFC_ORIENTATION_NORMAL;
    int swap_wh = (ori >= RFC_ORIENTATION_TRANSPOSE &&
                   ori <= RFC_ORIENTATION_ROTATE_270);
    uint32_t dst_w = swap_wh ? frame->layout.height : frame->layout.width;
    uint32_t dst_h = swap_wh ? frame->layout.width : frame->layout.height;
    if (dst_w == 0u || dst_h == 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    uint64_t stride64 = (uint64_t)dst_w * 4ull;
    if (stride64 > (uint64_t)SIZE_MAX ||
        (uint64_t)dst_h > (uint64_t)(SIZE_MAX / (size_t)stride64)) {
        return RFC_ERR_OVERFLOW;
    }
    size_t stride = (size_t)stride64;
    size_t total = (size_t)dst_h * stride;
    uint8_t *buf = (uint8_t *)eff_alloc.alloc(eff_alloc.ctx, total);
    if (!buf) {
        return RFC_ERR_OUT_OF_MEMORY;
    }

    float matte_r_lin = pv_srgb8_to_linear(matte_r);
    float matte_g_lin = pv_srgb8_to_linear(matte_g);
    float matte_b_lin = pv_srgb8_to_linear(matte_b);

    for (uint32_t dy = 0u; dy < dst_h; dy++) {
        uint8_t *dst_row = buf + (size_t)dy * stride;
        for (uint32_t dx = 0u; dx < dst_w; dx++) {
            uint32_t sx = 0u, sy = 0u;
            pv_map_coords(ori, frame->layout.width, frame->layout.height, dx,
                          dy, &sx, &sy);
            pv_render_pixel(frame, sx, sy, matte_r, matte_g, matte_b,
                            matte_r_lin, matte_g_lin, matte_b_lin,
                            dst_row + (size_t)dx * 4u);
        }
    }

    out_preview->width = dst_w;
    out_preview->height = dst_h;
    out_preview->stride_bytes = stride;
    out_preview->rgba8_srgb = buf;
    out_preview->data_len = total;
    out_preview->not_for_verification = 1;
    out_preview->allocator = eff_alloc;
    return RFC_OK;
}

void rfc_preview_free(rfc_preview_frame *preview) {
    if (!preview) {
        return;
    }
    if (preview->rgba8_srgb && preview->allocator.free) {
        preview->allocator.free(preview->allocator.ctx, preview->rgba8_srgb);
    }
    memset(preview, 0, sizeof(*preview));
}
