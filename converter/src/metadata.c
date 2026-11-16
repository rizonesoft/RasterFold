#include "rfc_verify.h"

#include <string.h>

static void meta_copy_cstr(char *dst, size_t dst_cap, const char *src) {
    if (!dst || dst_cap == 0u) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= dst_cap) {
        len = dst_cap - 1u;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void meta_append_cstr(char *dst, size_t dst_cap, const char *src) {
    if (!dst || dst_cap == 0u || !src) {
        return;
    }
    size_t used = strlen(dst);
    if (used >= dst_cap - 1u) {
        return;
    }
    meta_copy_cstr(dst + used, dst_cap - used, src);
}

const char *rfc_metadata_policy_name(rfc_metadata_policy policy) {
    switch (policy) {
        case RFC_META_POLICY_PRESERVE: return "preserve";
        case RFC_META_POLICY_STRIP: return "strip";
        default: return "unknown-metadata-policy";
    }
}

rfc_status rfc_metadata_policy_parse(const char *str, rfc_metadata_policy *out) {
    if (!str || !out) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(str, "preserve") == 0) {
        *out = RFC_META_POLICY_PRESERVE;
        return RFC_OK;
    }
    if (strcmp(str, "strip") == 0) {
        *out = RFC_META_POLICY_STRIP;
        return RFC_OK;
    }
    return RFC_ERR_SETTING_VALUE;
}

const char *rfc_verify_meta_outcome_name(rfc_verify_meta_outcome outcome) {
    switch (outcome) {
        case RFC_VERIFY_META_PRESERVED: return "preserved";
        case RFC_VERIFY_META_STRIPPED_EXPLICITLY: return "stripped-explicitly";
        case RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED:
            return "unsupported-items-named";
        default: return "unknown-meta-outcome";
    }
}

uint32_t rfc_format_metadata_caps(rfc_format_id id) {
    const rfc_capability *cap = rfc_registry_find(id);
    if (!cap) {
        return RFC_META_ITEM_NONE;
    }
    return cap->metadata_items & RFC_META_ITEM_ALL;
}

void rfc_metadata_items_format(uint32_t items, char *out_buf, size_t out_cap) {
    if (!out_buf || out_cap == 0u) {
        return;
    }
    out_buf[0] = '\0';
    int any = 0;
    if (items & RFC_META_ITEM_ICC) {
        meta_append_cstr(out_buf, out_cap, "icc");
        any = 1;
    }
    if (items & RFC_META_ITEM_ORIENTATION) {
        if (any) {
            meta_append_cstr(out_buf, out_cap, ", ");
        }
        meta_append_cstr(out_buf, out_cap, "orientation");
        any = 1;
    }
    if (items & RFC_META_ITEM_FRAME_TIMING) {
        if (any) {
            meta_append_cstr(out_buf, out_cap, ", ");
        }
        meta_append_cstr(out_buf, out_cap, "frame-timing");
        any = 1;
    }
    if (items & RFC_META_ITEM_EXIF) {
        if (any) {
            meta_append_cstr(out_buf, out_cap, ", ");
        }
        meta_append_cstr(out_buf, out_cap, "exif");
        any = 1;
    }
    if (items & RFC_META_ITEM_XMP) {
        if (any) {
            meta_append_cstr(out_buf, out_cap, ", ");
        }
        meta_append_cstr(out_buf, out_cap, "xmp");
        any = 1;
    }
    if (!any) {
        meta_copy_cstr(out_buf, out_cap, "none");
    }
}

uint32_t rfc_metadata_source_items(const rfc_metadata *container_meta,
                                   const rfc_frame *frames,
                                   size_t frame_count) {
    uint32_t items = RFC_META_ITEM_NONE;
    if (container_meta) {
        if ((container_meta->icc_len > 0u && container_meta->icc != NULL) ||
            rfc_colorimetry_has_any(&container_meta->color)) {
            items |= RFC_META_ITEM_ICC;
        }
        if (container_meta->orientation > RFC_ORIENTATION_NORMAL) {
            items |= RFC_META_ITEM_ORIENTATION;
        }
        if (container_meta->duration_ms > 0u) {
            items |= RFC_META_ITEM_FRAME_TIMING;
        }
        if (container_meta->exif_len > 0u && container_meta->exif != NULL) {
            items |= RFC_META_ITEM_EXIF;
        }
        if (container_meta->xmp_len > 0u && container_meta->xmp != NULL) {
            items |= RFC_META_ITEM_XMP;
        }
    }
    if (frames && frame_count > 0u) {
        if (frame_count > 1u) {
            items |= RFC_META_ITEM_FRAME_TIMING;
        }
        for (size_t i = 0u; i < frame_count; i++) {
            if ((frames[i].metadata.icc_len > 0u &&
                 frames[i].metadata.icc != NULL) ||
                rfc_colorimetry_has_any(&frames[i].metadata.color)) {
                items |= RFC_META_ITEM_ICC;
            }
            if (frames[i].metadata.orientation > RFC_ORIENTATION_NORMAL) {
                items |= RFC_META_ITEM_ORIENTATION;
            }
            if (frames[i].metadata.duration_ms > 0u) {
                items |= RFC_META_ITEM_FRAME_TIMING;
            }
            if (frames[i].metadata.exif_len > 0u &&
                frames[i].metadata.exif != NULL) {
                items |= RFC_META_ITEM_EXIF;
            }
            if (frames[i].metadata.xmp_len > 0u &&
                frames[i].metadata.xmp != NULL) {
                items |= RFC_META_ITEM_XMP;
            }
        }
    }
    return items;
}

static int colorimetry_equal(const rfc_colorimetry *a,
                             const rfc_colorimetry *b) {
    int a_any = rfc_colorimetry_has_any(a);
    int b_any = rfc_colorimetry_has_any(b);
    if (!a_any && !b_any) {
        return 1;
    }
    if (!a_any || !b_any) {
        return 0;
    }
    if ((a->has_srgb != 0u) != (b->has_srgb != 0u) ||
        (a->has_srgb && a->srgb_intent != b->srgb_intent)) {
        return 0;
    }
    if ((a->has_gama != 0u) != (b->has_gama != 0u) ||
        (a->has_gama && a->gama_scaled != b->gama_scaled)) {
        return 0;
    }
    if ((a->has_chrm != 0u) != (b->has_chrm != 0u) ||
        (a->has_chrm && memcmp(a->chrm_xy, b->chrm_xy, sizeof(a->chrm_xy)) != 0)) {
        return 0;
    }
    if ((a->has_cicp != 0u) != (b->has_cicp != 0u) ||
        (a->has_cicp && memcmp(a->cicp, b->cicp, sizeof(a->cicp)) != 0)) {
        return 0;
    }
    if ((a->has_mdcv != 0u) != (b->has_mdcv != 0u) ||
        (a->has_mdcv &&
         (memcmp(a->mdcv_xy, b->mdcv_xy, sizeof(a->mdcv_xy)) != 0 ||
          a->mdcv_max_dl != b->mdcv_max_dl ||
          a->mdcv_min_dl != b->mdcv_min_dl))) {
        return 0;
    }
    if ((a->has_clli != 0u) != (b->has_clli != 0u) ||
        (a->has_clli &&
         (a->clli_max_cll != b->clli_max_cll ||
          a->clli_max_fall != b->clli_max_fall))) {
        return 0;
    }
    return 1;
}

static const rfc_colorimetry *effective_colorimetry_at(
    const rfc_metadata *container_meta,
    const rfc_frame *frames,
    size_t frame_count,
    size_t idx) {
    if (frames && idx < frame_count &&
        rfc_colorimetry_has_any(&frames[idx].metadata.color)) {
        return &frames[idx].metadata.color;
    }
    if (container_meta && rfc_colorimetry_has_any(&container_meta->color)) {
        return &container_meta->color;
    }
    return NULL;
}

static rfc_orientation normalize_orientation(rfc_orientation ori) {
    if (ori == RFC_ORIENTATION_UNSPECIFIED) {
        return RFC_ORIENTATION_NORMAL;
    }
    return ori;
}

static const uint8_t *effective_icc_at(const rfc_metadata *container_meta,
                                       const rfc_frame *frames,
                                       size_t frame_count,
                                       size_t idx,
                                       size_t *out_len) {
    if (frames && idx < frame_count && frames[idx].metadata.icc_len > 0u &&
        frames[idx].metadata.icc != NULL) {
        *out_len = frames[idx].metadata.icc_len;
        return frames[idx].metadata.icc;
    }
    if (container_meta && container_meta->icc_len > 0u &&
        container_meta->icc != NULL) {
        *out_len = container_meta->icc_len;
        return container_meta->icc;
    }
    *out_len = 0u;
    return NULL;
}

static rfc_orientation effective_orientation_at(
    const rfc_metadata *container_meta,
    const rfc_frame *frames,
    size_t frame_count,
    size_t idx) {
    if (frames && idx < frame_count &&
        frames[idx].metadata.orientation != RFC_ORIENTATION_UNSPECIFIED) {
        return normalize_orientation(frames[idx].metadata.orientation);
    }
    if (container_meta &&
        container_meta->orientation != RFC_ORIENTATION_UNSPECIFIED) {
        return normalize_orientation(container_meta->orientation);
    }
    return RFC_ORIENTATION_NORMAL;
}

static const uint8_t *effective_exif_at(const rfc_metadata *container_meta,
                                        const rfc_frame *frames,
                                        size_t frame_count,
                                        size_t idx,
                                        size_t *out_len) {
    if (frames && idx < frame_count && frames[idx].metadata.exif_len > 0u &&
        frames[idx].metadata.exif != NULL) {
        *out_len = frames[idx].metadata.exif_len;
        return frames[idx].metadata.exif;
    }
    if (container_meta && container_meta->exif_len > 0u &&
        container_meta->exif != NULL) {
        *out_len = container_meta->exif_len;
        return container_meta->exif;
    }
    *out_len = 0u;
    return NULL;
}

static const uint8_t *effective_xmp_at(const rfc_metadata *container_meta,
                                       const rfc_frame *frames,
                                       size_t frame_count,
                                       size_t idx,
                                       size_t *out_len) {
    if (frames && idx < frame_count && frames[idx].metadata.xmp_len > 0u &&
        frames[idx].metadata.xmp != NULL) {
        *out_len = frames[idx].metadata.xmp_len;
        return frames[idx].metadata.xmp;
    }
    if (container_meta && container_meta->xmp_len > 0u &&
        container_meta->xmp != NULL) {
        *out_len = container_meta->xmp_len;
        return container_meta->xmp;
    }
    *out_len = 0u;
    return NULL;
}

static int has_any_exif(const rfc_metadata *container_meta,
                        const rfc_frame *frames,
                        size_t frame_count) {
    if (container_meta && container_meta->exif_len > 0u &&
        container_meta->exif != NULL) {
        return 1;
    }
    if (frames) {
        for (size_t i = 0u; i < frame_count; i++) {
            if (frames[i].metadata.exif_len > 0u &&
                frames[i].metadata.exif != NULL) {
                return 1;
            }
        }
    }
    return 0;
}

static int has_any_xmp(const rfc_metadata *container_meta,
                       const rfc_frame *frames,
                       size_t frame_count) {
    if (container_meta && container_meta->xmp_len > 0u &&
        container_meta->xmp != NULL) {
        return 1;
    }
    if (frames) {
        for (size_t i = 0u; i < frame_count; i++) {
            if (frames[i].metadata.xmp_len > 0u &&
                frames[i].metadata.xmp != NULL) {
                return 1;
            }
        }
    }
    return 0;
}

static int has_any_raw_icc(const rfc_metadata *container_meta,
                           const rfc_frame *frames,
                           size_t frame_count) {
    if (container_meta && container_meta->icc_len > 0u &&
        container_meta->icc != NULL) {
        return 1;
    }
    if (frames) {
        for (size_t i = 0u; i < frame_count; i++) {
            if (frames[i].metadata.icc_len > 0u &&
                frames[i].metadata.icc != NULL) {
                return 1;
            }
        }
    }
    return 0;
}

static int has_any_raw_colorimetry(const rfc_metadata *container_meta,
                                   const rfc_frame *frames,
                                   size_t frame_count) {
    if (container_meta && rfc_colorimetry_has_any(&container_meta->color)) {
        return 1;
    }
    if (frames) {
        for (size_t i = 0u; i < frame_count; i++) {
            if (rfc_colorimetry_has_any(&frames[i].metadata.color)) {
                return 1;
            }
        }
    }
    return 0;
}

static int format_supports_raw_colorimetry(rfc_format_id id) {
    return id == RFC_FORMAT_PNG || id == RFC_FORMAT_APNG;
}

static int has_any_color_metadata(const rfc_metadata *container_meta,
                                  const rfc_frame *frames,
                                  size_t frame_count) {
    if (container_meta &&
        ((container_meta->icc_len > 0u && container_meta->icc != NULL) ||
         rfc_colorimetry_has_any(&container_meta->color))) {
        return 1;
    }
    if (frames) {
        for (size_t i = 0u; i < frame_count; i++) {
            if ((frames[i].metadata.icc_len > 0u &&
                 frames[i].metadata.icc != NULL) ||
                rfc_colorimetry_has_any(&frames[i].metadata.color)) {
                return 1;
            }
        }
    }
    return 0;
}

static int has_non_normal_orientation(const rfc_metadata *container_meta,
                                      const rfc_frame *frames,
                                      size_t frame_count) {
    if (container_meta &&
        container_meta->orientation > RFC_ORIENTATION_NORMAL) {
        return 1;
    }
    if (frames) {
        for (size_t i = 0u; i < frame_count; i++) {
            if (frames[i].metadata.orientation > RFC_ORIENTATION_NORMAL) {
                return 1;
            }
        }
    }
    return 0;
}

rfc_status rfc_metadata_check_preflight(rfc_format_id target_format,
                                        rfc_metadata_policy policy,
                                        rfc_exactness_class exactness,
                                        uint32_t present_source_items,
                                        uint32_t acknowledged_unsupported,
                                        rfc_metadata_report *out_report) {
    if (out_report) {
        memset(out_report, 0, sizeof(*out_report));
    }
    const rfc_capability *cap = rfc_registry_find(target_format);
    if (!cap) {
        return RFC_ERR_UNKNOWN_FORMAT;
    }
    if ((present_source_items & ~RFC_META_ITEM_ALL) != 0u ||
        (acknowledged_unsupported & ~RFC_META_ITEM_ALL) != 0u) {
        return RFC_ERR_INVALID_ARGUMENT;
    }
    if (policy != RFC_META_POLICY_PRESERVE && policy != RFC_META_POLICY_STRIP) {
        return RFC_ERR_SETTING_VALUE;
    }
    if (policy == RFC_META_POLICY_STRIP &&
        (exactness == RFC_EXACT_BYTE_IDENTICAL ||
         exactness == RFC_EXACT_ORIGINAL_BYTE_RECONSTRUCTION)) {
        if (out_report) {
            meta_copy_cstr(
                out_report->detail, sizeof(out_report->detail),
                "metadata=strip changes bytes and cannot claim byte-oriented "
                "exactness");
        }
        return RFC_ERR_UNSUPPORTED_COMBINATION;
    }

    uint32_t caps = cap->metadata_items & RFC_META_ITEM_ALL;
    uint32_t unsupported = present_source_items & (~caps);
    uint32_t supported = present_source_items & caps;

    if (out_report) {
        out_report->present_source_items = present_source_items;
        out_report->target_supported_items = caps;
    }

    if (policy == RFC_META_POLICY_STRIP) {
        uint32_t removable = present_source_items &
                             (RFC_META_ITEM_ICC | RFC_META_ITEM_ORIENTATION |
                              RFC_META_ITEM_EXIF | RFC_META_ITEM_XMP);
        if (out_report) {
            out_report->outcome = RFC_VERIFY_META_STRIPPED_EXPLICITLY;
            out_report->stripped_items = removable;
            meta_copy_cstr(out_report->detail, sizeof(out_report->detail),
                           "metadata=strip requested explicitly");
        }
        return RFC_OK;
    }

    uint32_t unack = unsupported & (~acknowledged_unsupported);
    if (unack != 0u) {
        if (out_report) {
            char names[96];
            rfc_metadata_items_format(unack, names, sizeof(names));
            out_report->outcome = RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED;
            out_report->unsupported_items = unsupported;
            meta_copy_cstr(out_report->detail, sizeof(out_report->detail),
                           "target format ");
            meta_append_cstr(out_report->detail, sizeof(out_report->detail),
                             cap->name);
            meta_append_cstr(out_report->detail, sizeof(out_report->detail),
                             " does not support metadata items: ");
            meta_append_cstr(out_report->detail, sizeof(out_report->detail),
                             names);
        }
        return RFC_ERR_METADATA_POLICY;
    }

    if (out_report) {
        out_report->unsupported_items = unsupported;
        out_report->preserved_items = supported;
        if (unsupported != 0u) {
            char names[96];
            rfc_metadata_items_format(unsupported, names, sizeof(names));
            out_report->outcome = RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED;
            meta_copy_cstr(out_report->detail, sizeof(out_report->detail),
                           "acknowledged unsupported metadata items for ");
            meta_append_cstr(out_report->detail, sizeof(out_report->detail),
                             cap->name);
            meta_append_cstr(out_report->detail, sizeof(out_report->detail),
                             ": ");
            meta_append_cstr(out_report->detail, sizeof(out_report->detail),
                             names);
        } else {
            out_report->outcome = RFC_VERIFY_META_PRESERVED;
            meta_copy_cstr(out_report->detail, sizeof(out_report->detail),
                           "all present metadata items supported");
        }
    }
    return RFC_OK;
}

rfc_status rfc_metadata_check_source_preflight(rfc_format_id target_format,
                                               rfc_metadata_policy policy,
                                               rfc_exactness_class exactness,
                                               const rfc_metadata *src_meta,
                                               const rfc_frame *src_frames,
                                               size_t src_frame_count,
                                               uint32_t acknowledged_unsupported,
                                               rfc_metadata_report *out_report) {
    rfc_metadata_report local_rep;
    rfc_metadata_report *rep = out_report ? out_report : &local_rep;
    uint32_t present =
        rfc_metadata_source_items(src_meta, src_frames, src_frame_count);
    rfc_status st = rfc_metadata_check_preflight(
        target_format, policy, exactness, present, acknowledged_unsupported,
        rep);
    if (st != RFC_OK) {
        return st;
    }
    if (policy == RFC_META_POLICY_PRESERVE &&
        has_any_raw_colorimetry(src_meta, src_frames, src_frame_count) &&
        !format_supports_raw_colorimetry(target_format)) {
        const rfc_capability *cap = rfc_registry_find(target_format);
        uint32_t unsupported = rep->unsupported_items | RFC_META_ITEM_ICC;
        rep->unsupported_items = unsupported;
        if (!has_any_raw_icc(src_meta, src_frames, src_frame_count)) {
            rep->preserved_items &= ~RFC_META_ITEM_ICC;
        }
        if ((acknowledged_unsupported & RFC_META_ITEM_ICC) == 0u) {
            char names[96];
            rfc_metadata_items_format(unsupported & (~acknowledged_unsupported),
                                      names, sizeof(names));
            rep->outcome = RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED;
            meta_copy_cstr(rep->detail, sizeof(rep->detail), "target format ");
            meta_append_cstr(rep->detail, sizeof(rep->detail),
                             cap ? cap->name : "unknown");
            meta_append_cstr(rep->detail, sizeof(rep->detail),
                             " does not support metadata items: ");
            meta_append_cstr(rep->detail, sizeof(rep->detail), names);
            return RFC_ERR_METADATA_POLICY;
        }
        char names[96];
        rfc_metadata_items_format(unsupported, names, sizeof(names));
        rep->outcome = RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED;
        meta_copy_cstr(rep->detail, sizeof(rep->detail),
                       "acknowledged unsupported metadata items for ");
        meta_append_cstr(rep->detail, sizeof(rep->detail),
                         cap ? cap->name : "unknown");
        meta_append_cstr(rep->detail, sizeof(rep->detail), ": ");
        meta_append_cstr(rep->detail, sizeof(rep->detail), names);
    }
    return RFC_OK;
}

rfc_status rfc_metadata_verify(rfc_format_id target_format,
                               rfc_metadata_policy policy,
                               rfc_exactness_class exactness,
                               const rfc_metadata *src_meta,
                               const rfc_frame *src_frames,
                               size_t src_frame_count,
                               const rfc_metadata *dst_meta,
                               const rfc_frame *dst_frames,
                               size_t dst_frame_count,
                               uint32_t acknowledged_unsupported,
                               rfc_metadata_report *out_report,
                               rfc_sample_mismatch *out_mismatch) {
    rfc_metadata_report local_rep;
    rfc_metadata_report *rep = out_report ? out_report : &local_rep;
    memset(rep, 0, sizeof(*rep));

    uint32_t present =
        rfc_metadata_source_items(src_meta, src_frames, src_frame_count);
    rfc_status st = rfc_metadata_check_source_preflight(
        target_format, policy, exactness, src_meta, src_frames, src_frame_count,
        acknowledged_unsupported, rep);
    if (st != RFC_OK) {
        if (st == RFC_ERR_METADATA_POLICY && out_mismatch) {
            memset(out_mismatch, 0, sizeof(*out_mismatch));
            out_mismatch->kind = RFC_MISMATCH_METADATA_UNSUPPORTED;
            out_mismatch->expected_bits = present;
            out_mismatch->actual_bits = rep->unsupported_items;
            meta_copy_cstr(out_mismatch->detail, sizeof(out_mismatch->detail),
                           rep->detail);
        }
        return st;
    }

    if (policy == RFC_META_POLICY_STRIP) {
        uint32_t retained = RFC_META_ITEM_NONE;
        if (has_any_color_metadata(dst_meta, dst_frames, dst_frame_count)) {
            retained |= RFC_META_ITEM_ICC;
        }
        if (has_non_normal_orientation(dst_meta, dst_frames, dst_frame_count)) {
            retained |= RFC_META_ITEM_ORIENTATION;
        }
        if (has_any_exif(dst_meta, dst_frames, dst_frame_count)) {
            retained |= RFC_META_ITEM_EXIF;
        }
        if (has_any_xmp(dst_meta, dst_frames, dst_frame_count)) {
            retained |= RFC_META_ITEM_XMP;
        }
        if (retained != 0u) {
            char names[96];
            rfc_metadata_items_format(retained, names, sizeof(names));
            rep->retained_removable_items = retained;
            meta_copy_cstr(rep->detail, sizeof(rep->detail),
                           "metadata=strip retained removable items: ");
            meta_append_cstr(rep->detail, sizeof(rep->detail), names);
            if (out_mismatch) {
                memset(out_mismatch, 0, sizeof(*out_mismatch));
                out_mismatch->kind = RFC_MISMATCH_METADATA_RETAINED;
                out_mismatch->actual_bits = retained;
                meta_copy_cstr(out_mismatch->detail,
                               sizeof(out_mismatch->detail), rep->detail);
            }
            return RFC_ERR_VERIFY_MISMATCH;
        }
        rep->outcome = RFC_VERIFY_META_STRIPPED_EXPLICITLY;
        meta_copy_cstr(rep->detail, sizeof(rep->detail),
                       "removable metadata stripped explicitly");
        return RFC_OK;
    }

    uint32_t caps = rep->target_supported_items;
    uint32_t supported = present & caps;
    if (!format_supports_raw_colorimetry(target_format) &&
        has_any_raw_colorimetry(src_meta, src_frames, src_frame_count) &&
        !has_any_raw_icc(src_meta, src_frames, src_frame_count)) {
        supported &= ~RFC_META_ITEM_ICC;
    }
    uint32_t lost = RFC_META_ITEM_NONE;
    size_t check_count =
        (src_frame_count > dst_frame_count) ? src_frame_count : dst_frame_count;
    if (check_count == 0u) {
        check_count = 1u;
    }

    if (caps & RFC_META_ITEM_ICC) {
        if (src_frame_count > 0u && dst_frame_count > 0u &&
            src_frame_count != dst_frame_count) {
            lost |= RFC_META_ITEM_ICC;
        } else {
            int raw_ok = format_supports_raw_colorimetry(target_format);
            for (size_t i = 0u; i < check_count; i++) {
                size_t slen = 0u, dlen = 0u;
                const uint8_t *s_icc = effective_icc_at(
                    src_meta, src_frames, src_frame_count, i, &slen);
                const uint8_t *d_icc = effective_icc_at(
                    dst_meta, dst_frames, dst_frame_count, i, &dlen);
                const rfc_colorimetry *s_col = effective_colorimetry_at(
                    src_meta, src_frames, src_frame_count, i);
                const rfc_colorimetry *d_col = effective_colorimetry_at(
                    dst_meta, dst_frames, dst_frame_count, i);
                if (slen != dlen ||
                    (slen > 0u &&
                     (!s_icc || !d_icc || memcmp(s_icc, d_icc, slen) != 0)) ||
                    (raw_ok ? !colorimetry_equal(s_col, d_col)
                            : (d_col != NULL))) {
                    lost |= RFC_META_ITEM_ICC;
                    break;
                }
            }
        }
    }

    if (caps & RFC_META_ITEM_ORIENTATION) {
        if (src_frame_count > 0u && dst_frame_count > 0u &&
            src_frame_count != dst_frame_count) {
            lost |= RFC_META_ITEM_ORIENTATION;
        } else {
            for (size_t i = 0u; i < check_count; i++) {
                rfc_orientation s_ori = effective_orientation_at(
                    src_meta, src_frames, src_frame_count, i);
                rfc_orientation d_ori = effective_orientation_at(
                    dst_meta, dst_frames, dst_frame_count, i);
                if (s_ori != d_ori) {
                    lost |= RFC_META_ITEM_ORIENTATION;
                    break;
                }
            }
        }
    }

    if (caps & RFC_META_ITEM_FRAME_TIMING) {
        uint32_t sdur = src_meta ? src_meta->duration_ms : 0u;
        uint32_t ddur = dst_meta ? dst_meta->duration_ms : 0u;
        if (src_frame_count != dst_frame_count || sdur != ddur) {
            lost |= RFC_META_ITEM_FRAME_TIMING;
        } else if (src_frames && dst_frames) {
            for (size_t i = 0u; i < src_frame_count; i++) {
                if (src_frames[i].metadata.duration_ms !=
                    dst_frames[i].metadata.duration_ms) {
                    lost |= RFC_META_ITEM_FRAME_TIMING;
                    break;
                }
            }
        }
    }

    if (caps & RFC_META_ITEM_EXIF) {
        if (src_frame_count > 0u && dst_frame_count > 0u &&
            src_frame_count != dst_frame_count) {
            lost |= RFC_META_ITEM_EXIF;
        } else {
            for (size_t i = 0u; i < check_count; i++) {
                size_t slen = 0u, dlen = 0u;
                const uint8_t *s_ex = effective_exif_at(
                    src_meta, src_frames, src_frame_count, i, &slen);
                const uint8_t *d_ex = effective_exif_at(
                    dst_meta, dst_frames, dst_frame_count, i, &dlen);
                rfc_orientation s_ori = effective_orientation_at(
                    src_meta, src_frames, src_frame_count, i);
                if (slen > 0u) {
                    if (slen != dlen || !s_ex || !d_ex ||
                        memcmp(s_ex, d_ex, slen) != 0) {
                        lost |= RFC_META_ITEM_EXIF;
                        break;
                    }
                } else if (s_ori <= RFC_ORIENTATION_NORMAL) {
                    if (dlen != 0u) {
                        lost |= RFC_META_ITEM_EXIF;
                        break;
                    }
                }
            }
        }
    }

    if (caps & RFC_META_ITEM_XMP) {
        if (src_frame_count > 0u && dst_frame_count > 0u &&
            src_frame_count != dst_frame_count) {
            lost |= RFC_META_ITEM_XMP;
        } else {
            for (size_t i = 0u; i < check_count; i++) {
                size_t slen = 0u, dlen = 0u;
                const uint8_t *s_xmp = effective_xmp_at(
                    src_meta, src_frames, src_frame_count, i, &slen);
                const uint8_t *d_xmp = effective_xmp_at(
                    dst_meta, dst_frames, dst_frame_count, i, &dlen);
                if (slen != dlen ||
                    (slen > 0u &&
                     (!s_xmp || !d_xmp || memcmp(s_xmp, d_xmp, slen) != 0))) {
                    lost |= RFC_META_ITEM_XMP;
                    break;
                }
            }
        }
    }

    if (lost != 0u) {
        char names[96];
        rfc_metadata_items_format(lost, names, sizeof(names));
        rep->lost_supported_items = lost;
        rep->preserved_items = supported & (~lost);
        meta_copy_cstr(rep->detail, sizeof(rep->detail),
                       "supported metadata items lost or altered: ");
        meta_append_cstr(rep->detail, sizeof(rep->detail), names);
        if (out_mismatch) {
            memset(out_mismatch, 0, sizeof(*out_mismatch));
            out_mismatch->kind = RFC_MISMATCH_METADATA_LOST;
            out_mismatch->expected_bits = supported;
            out_mismatch->actual_bits = rep->preserved_items;
            meta_copy_cstr(out_mismatch->detail, sizeof(out_mismatch->detail),
                           rep->detail);
        }
        return RFC_ERR_VERIFY_MISMATCH;
    }

    rep->preserved_items = supported;
    if (rep->unsupported_items != 0u) {
        char names[96];
        rfc_metadata_items_format(rep->unsupported_items, names, sizeof(names));
        rep->outcome = RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED;
        meta_copy_cstr(rep->detail, sizeof(rep->detail),
                       "supported items preserved; named unsupported items: ");
        meta_append_cstr(rep->detail, sizeof(rep->detail), names);
    } else {
        rep->outcome = RFC_VERIFY_META_PRESERVED;
        meta_copy_cstr(rep->detail, sizeof(rep->detail),
                       "all supported metadata preserved");
    }
    return RFC_OK;
}
