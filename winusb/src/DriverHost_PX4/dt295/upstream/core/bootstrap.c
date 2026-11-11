/* SPDX-License-Identifier: MIT */
#include "mb86H/bootstrap.h"
#include "mb86H/protocol/constants.h"
#include "mb86H/bytes.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

#define SBC2_MAGIC 0x53424332u
#define SBC2_VERSION 1u
#define SBC2_HEADER_BYTES 64u
#define SBC2_PROFILE_BYTES 64u
#define SBC2_REFERENCE_BYTES 16u
#define SBC2_RELOCATION_BYTES 12u
#define SBC2_XZ_TARGET 1u
#define SBC2_TORNE_TARGET 2u
#define SBC2_DT295_TARGET 3u
#define SBC2_XZ_BODY_CAPACITY 2236u
#define SBC2_TORNE_BODY_CAPACITY 2048u
#define SBC2_RELOC_20 20u
#define SBC2_RELOC_32 32u
#define SBC2_RELOC_48 48u


static void put_mb86H_be32_get(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static bool in_bounds(size_t total, uint32_t offset, size_t length)
{
    return offset <= total && length <= total - offset;
}

static bool aligned4(uint32_t value)
{
    return !(value & 3u);
}

static bool range_end(uint32_t address, size_t length, uint32_t *end)
{
    if (length > 0xffffffffu || address > 0xffffffffu - (uint32_t)length)
        return false;
    *end = address + (uint32_t)length;
    return true;
}

static bool ranges_overlap(uint32_t a, uint32_t a_end, uint32_t b,
                           uint32_t b_end)
{
    return a < b_end && b < a_end;
}

struct span { uint32_t offset; size_t length; };

static int add_span(struct span *spans, uint32_t *count, uint32_t offset,
                    size_t length, size_t total)
{
    uint32_t index;
    uint32_t end;

    if (!in_bounds(total, offset, length) || !range_end(offset, length, &end))
        return -EINVAL;
    for (index = 0; index < *count; ++index) {
        uint32_t prior_end;
        if (!range_end(spans[index].offset, spans[index].length, &prior_end) ||
            ranges_overlap(offset, end, spans[index].offset, prior_end))
            return -EINVAL;
    }
    spans[*count].offset = offset;
    spans[*count].length = length;
    ++*count;
    return 0;
}

static const uint8_t *record_at(const uint8_t *records, uint32_t index,
                                uint32_t bytes)
{
    return records + (size_t)index * bytes;
}

static const uint8_t *find_reference(const struct mb86H_bootstrap *bootstrap,
                                     uint32_t address, size_t length)
{
    uint32_t end;
    uint32_t index;

    if (!bootstrap || !range_end(address, length, &end)) return 0;
    for (index = 0; index < bootstrap->reference_count; ++index) {
        const uint8_t *record = record_at(bootstrap->references, index,
                                          SBC2_REFERENCE_BYTES);
        uint32_t base = mb86H_be32_get(record);
        uint32_t bytes = mb86H_be32_get(record + 4);
        uint32_t reference_end;
        if (!range_end(base, bytes, &reference_end)) return 0;
        if (address >= base && end <= reference_end)
            return bootstrap->bundle + mb86H_be32_get(record + 8) + (address - base);
    }
    return 0;
}

static int validate_records(const uint8_t *bundle, size_t size,
                            uint32_t records_offset, uint32_t count,
                            uint32_t forbidden_address, uint32_t forbidden_size,
                            bool is_relocation)
{
    uint32_t index;
    size_t record_bytes = is_relocation ? SBC2_RELOCATION_BYTES :
                                         SBC2_REFERENCE_BYTES;

    if (!aligned4(records_offset) || (size_t)count > size / record_bytes ||
        !in_bounds(size, records_offset, (size_t)count * record_bytes))
        return -EINVAL;
    for (index = 0; index < count; ++index) {
        const uint8_t *record = bundle + records_offset + (size_t)index * record_bytes;
        uint32_t address = mb86H_be32_get(record);
        uint32_t bytes = is_relocation ? 4u : mb86H_be32_get(record + 4);
        uint32_t data_offset = is_relocation ? 0u : mb86H_be32_get(record + 8);
        uint32_t end;
        uint32_t other;

        if (!bytes || mb86H_be32_get(record + record_bytes - 4)) return -EINVAL;
        if (!range_end(address, bytes, &end) ||
            (forbidden_size && ranges_overlap(address, end, forbidden_address,
                                               forbidden_address + forbidden_size)))
            return -EINVAL;
        if (!is_relocation && !in_bounds(size, data_offset, bytes)) return -EINVAL;
        for (other = 0; other < index; ++other) {
            const uint8_t *prior = bundle + records_offset + (size_t)other * record_bytes;
            uint32_t prior_address = mb86H_be32_get(prior);
            uint32_t prior_bytes = is_relocation ? 4u : mb86H_be32_get(prior + 4);
            uint32_t prior_end;
            if (!range_end(prior_address, prior_bytes, &prior_end) ||
                ranges_overlap(address, end, prior_address, prior_end))
                return -EINVAL;
        }
    }
    return 0;
}

static bool record_covers(const uint8_t *records, uint32_t count,
                          uint32_t address)
{
    uint32_t index;
    for (index = 0; index < count; ++index) {
        const uint8_t *record = record_at(records, index, SBC2_REFERENCE_BYTES);
        uint32_t base = mb86H_be32_get(record), end;
        if (range_end(base, mb86H_be32_get(record + 4), &end) &&
            address >= base && address < end) return true;
    }
    return false;
}

static int require_coverage(const uint8_t *records, uint32_t count,
                            uint32_t address, uint32_t length)
{
    uint32_t offset;
    for (offset = 0; offset < length; ++offset)
        if (!record_covers(records, count, address + offset)) return -EINVAL;
    return 0;
}

static int require_single_reference(const uint8_t *records, uint32_t count,
                                    uint32_t address, uint32_t length)
{
    uint32_t end, index;
    if (!range_end(address, length, &end)) return -EINVAL;
    for (index = 0; index < count; ++index) {
        const uint8_t *record = record_at(records, index, SBC2_REFERENCE_BYTES);
        uint32_t base = mb86H_be32_get(record), reference_end;
        if (!range_end(base, mb86H_be32_get(record + 4), &reference_end)) return -EINVAL;
        if (address >= base && end <= reference_end) return 0;
    }
    return -EINVAL;
}

static int validate_patch_windows(const uint8_t *patches, uint32_t count,
                                  unsigned target)
{
    bool is_torne = target == MB86H_BOOTSTRAP_TORNE;
    static const uint32_t xz_address[] = { MB86H_BOOT0_HOOK_ADDRESS, 0xe9b54u, 0xe9c08u };
    static const uint32_t xz_length[] = { 88u, 8u, 8u };
    static const uint32_t torne_address[] = { MB86H_TORNE_BOOT0_HOOK_ADDRESS,
                                               MB86H_TORNE_BOOT0_OUT_PATCH_ADDRESS };
    static const uint32_t torne_length[] = { 8u, 4u };
    static const uint32_t dt_address[] = {MB86H_DT295_BOOT0_HOOK_ADDRESS};
    static const uint32_t dt_length[] = {8u};
    const uint32_t *address = target == MB86H_BOOTSTRAP_DT295 ? dt_address : is_torne ? torne_address : xz_address;
    const uint32_t *length = target == MB86H_BOOTSTRAP_DT295 ? dt_length : is_torne ? torne_length : xz_length;
    uint32_t windows = target == MB86H_BOOTSTRAP_DT295 ? 1u : is_torne ? 2u : 3u;
    uint32_t index, window;

    for (index = 0; index < count; ++index) {
        const uint8_t *record = record_at(patches, index, SBC2_REFERENCE_BYTES);
        uint32_t base = mb86H_be32_get(record), end;
        bool valid = false;
        if (!range_end(base, mb86H_be32_get(record + 4), &end)) return -EINVAL;
        for (window = 0; window < windows; ++window) {
            uint32_t window_end = address[window] + length[window];
            if (base >= address[window] && end <= window_end) valid = true;
        }
        if (!valid) return -EINVAL;
    }
    for (window = 0; window < windows; ++window) {
        int rc = require_coverage(patches, count, address[window], length[window]);
        if (rc) return rc;
    }
    return 0;
}

static int validate_record_data(const uint8_t *bundle, size_t size,
                                const uint8_t *records, uint32_t count,
                                const struct span *metadata,
                                uint32_t metadata_count)
{
    uint32_t index;
    for (index = 0; index < count; ++index) {
        const uint8_t *record = record_at(records, index, SBC2_REFERENCE_BYTES);
        uint32_t offset = mb86H_be32_get(record + 8), bytes = mb86H_be32_get(record + 4), end;
        uint32_t other, meta;
        if (!in_bounds(size, offset, bytes) || !range_end(offset, bytes, &end))
            return -EINVAL;
        for (meta = 0; meta < metadata_count; ++meta) {
            uint32_t metadata_end;
            if (!range_end(metadata[meta].offset, metadata[meta].length, &metadata_end) ||
                ranges_overlap(offset, end, metadata[meta].offset, metadata_end))
                return -EINVAL;
        }
        for (other = 0; other < index; ++other) {
            const uint8_t *prior = record_at(records, other, SBC2_REFERENCE_BYTES);
            uint32_t prior_offset = mb86H_be32_get(prior + 8), prior_end;
            if (!range_end(prior_offset, mb86H_be32_get(prior + 4), &prior_end) ||
                ranges_overlap(offset, end, prior_offset, prior_end)) return -EINVAL;
        }
    }
    (void)bundle;
    return 0;
}

int mb86H_bootstrap_parse(struct mb86H_bootstrap *bootstrap,
                          const uint8_t *bundle, size_t size, unsigned target,
                          const uint8_t **resident)
{
    const uint8_t *profile = 0;
    uint32_t profiles_offset, profile_count, body_offset, body_size;
    uint32_t relocations_offset, relocation_count, resident_offset, resident_size;
    bool is_torne = target == MB86H_BOOTSTRAP_TORNE;
    bool is_dt295 = target == MB86H_BOOTSTRAP_DT295;
    uint32_t expected_target = target + 1u;
    uint32_t expected_address = is_dt295 ? MB86H_DT295_BOOT0_BODY_ADDRESS : is_torne ? MB86H_TORNE_BOOT0_BODY_ADDRESS :
                                           MB86H_BOOT0_BODY_ADDRESS;
    uint32_t expected_capacity = is_dt295 ? MB86H_DT295_BOOT0_BODY_BYTES : is_torne ? SBC2_TORNE_BODY_CAPACITY :
                                            SBC2_XZ_BODY_CAPACITY;
    uint32_t index;
    struct span metadata[11];
    uint32_t metadata_count = 0;
    int rc;

    if (!bootstrap || !bundle || !resident || size < SBC2_HEADER_BYTES ||
        mb86H_be32_get(bundle) != SBC2_MAGIC || mb86H_be32_get(bundle + 4) != SBC2_VERSION ||
        mb86H_be32_get(bundle + 8) != SBC2_HEADER_BYTES || mb86H_be32_get(bundle + 12) != size ||
        target > MB86H_BOOTSTRAP_DT295 || mb86H_be32_get(bundle + 44) != 262144u)
        return -EINVAL;
    for (index = 12; index < 16; ++index)
        if (mb86H_be32_get(bundle + index * 4)) return -EINVAL;

    profiles_offset = mb86H_be32_get(bundle + 16);
    profile_count = mb86H_be32_get(bundle + 20);
    body_offset = mb86H_be32_get(bundle + 24);
    body_size = mb86H_be32_get(bundle + 28);
    relocations_offset = mb86H_be32_get(bundle + 32);
    relocation_count = mb86H_be32_get(bundle + 36);
    resident_offset = mb86H_be32_get(bundle + 40);
    resident_size = mb86H_be32_get(bundle + 44);
    if (!aligned4(profiles_offset) || !profile_count || profile_count > 3u ||
        relocation_count > size / SBC2_RELOCATION_BYTES ||
        !in_bounds(size, profiles_offset, (size_t)profile_count * SBC2_PROFILE_BYTES) ||
        (body_offset & 1u) || (body_size & 1u) || !body_size ||
        !in_bounds(size, body_offset, body_size) ||
        !aligned4(relocations_offset) ||
        !in_bounds(size, resident_offset, resident_size))
        return -EINVAL;

    rc = add_span(metadata, &metadata_count, 0, SBC2_HEADER_BYTES, size);
    if (!rc) rc = add_span(metadata, &metadata_count, profiles_offset,
                           (size_t)profile_count * SBC2_PROFILE_BYTES, size);
    if (!rc) rc = add_span(metadata, &metadata_count, body_offset, body_size, size);
    if (!rc) rc = add_span(metadata, &metadata_count, relocations_offset,
                           (size_t)relocation_count * SBC2_RELOCATION_BYTES, size);
    if (!rc) rc = add_span(metadata, &metadata_count, resident_offset, resident_size, size);
    if (rc) return rc;

    for (index = 0; index < profile_count; ++index) {
        const uint8_t *candidate = bundle + profiles_offset +
                                   (size_t)index * SBC2_PROFILE_BYTES;
        uint32_t target = mb86H_be32_get(candidate);
        uint32_t field;
        uint32_t other;
        if (target != SBC2_XZ_TARGET && target != SBC2_TORNE_TARGET && target != SBC2_DT295_TARGET)
            return -EINVAL;
        for (field = 8; field < 16; ++field)
            if (mb86H_be32_get(candidate + field * 4)) return -EINVAL;
        for (other = 0; other < index; ++other)
            if (mb86H_be32_get(bundle + profiles_offset + (size_t)other * SBC2_PROFILE_BYTES) == target)
                return -EINVAL;
        if (!aligned4(mb86H_be32_get(candidate + 12)) || !aligned4(mb86H_be32_get(candidate + 20)) ||
            mb86H_be32_get(candidate + 16) > size / SBC2_REFERENCE_BYTES ||
            mb86H_be32_get(candidate + 24) > size / SBC2_REFERENCE_BYTES ||
            !in_bounds(size, mb86H_be32_get(candidate + 12),
                       (size_t)mb86H_be32_get(candidate + 16) * SBC2_REFERENCE_BYTES) ||
            !in_bounds(size, mb86H_be32_get(candidate + 20),
                       (size_t)mb86H_be32_get(candidate + 24) * SBC2_REFERENCE_BYTES))
            return -EINVAL;
        rc = add_span(metadata, &metadata_count, mb86H_be32_get(candidate + 12),
                      (size_t)mb86H_be32_get(candidate + 16) * SBC2_REFERENCE_BYTES, size);
        if (!rc) rc = add_span(metadata, &metadata_count, mb86H_be32_get(candidate + 20),
                               (size_t)mb86H_be32_get(candidate + 24) * SBC2_REFERENCE_BYTES, size);
        if (rc) return rc;
        if (target == expected_target) profile = candidate;
    }
    if (!profile || mb86H_be32_get(profile + 4) != expected_address ||
        mb86H_be32_get(profile + 8) != expected_capacity || body_size > expected_capacity ||
        mb86H_be32_get(profile + 28) != (is_torne || is_dt295 ? 0u : 1u))
        return -EINVAL;

    for (index = 0; index < profile_count; ++index) {
        const uint8_t *candidate = bundle + profiles_offset +
                                   (size_t)index * SBC2_PROFILE_BYTES;
        rc = validate_record_data(bundle, size, bundle + mb86H_be32_get(candidate + 12),
                                  mb86H_be32_get(candidate + 16), metadata, metadata_count);
        if (!rc) rc = validate_record_data(bundle, size, bundle + mb86H_be32_get(candidate + 20),
                                           mb86H_be32_get(candidate + 24), metadata, metadata_count);
        if (rc) return rc;
    }

    rc = validate_records(bundle, size, mb86H_be32_get(profile + 12), mb86H_be32_get(profile + 16),
                          0, 0, false);
    if (rc) return rc;
    rc = validate_records(bundle, size, mb86H_be32_get(profile + 20), mb86H_be32_get(profile + 24),
                          expected_address, expected_capacity, false);
    if (rc) return rc;
    rc = validate_patch_windows(bundle + mb86H_be32_get(profile + 20), mb86H_be32_get(profile + 24), target);
    if (rc) return rc;
    if (is_torne || is_dt295) {
        rc = require_single_reference(bundle + mb86H_be32_get(profile + 12), mb86H_be32_get(profile + 16),
                                      is_dt295 ? MB86H_DT295_BOOT0_CODE_ADDRESS : MB86H_TORNE_BOOT0_CODE_ADDRESS,
                                      is_dt295 ? MB86H_DT295_BOOT0_CODE_BYTES : MB86H_TORNE_BOOT0_CODE_BYTES);
    } else {
        rc = require_single_reference(bundle + mb86H_be32_get(profile + 12), mb86H_be32_get(profile + 16),
                                      MB86H_BOOT0_HOOK_ADDRESS, 88u);
        if (!rc) rc = require_single_reference(bundle + mb86H_be32_get(profile + 12),
                                               mb86H_be32_get(profile + 16),
                                               MB86H_BOOT0_BODY_ADDRESS, SBC2_XZ_BODY_CAPACITY);
        if (!rc) rc = require_single_reference(bundle + mb86H_be32_get(profile + 12),
                                               mb86H_be32_get(profile + 16), 0xe9b54u, 8u);
        if (!rc) rc = require_single_reference(bundle + mb86H_be32_get(profile + 12),
                                               mb86H_be32_get(profile + 16), 0xe9c08u, 8u);
    }
    if (rc) return rc;
    if (!in_bounds(size, relocations_offset,
                   (size_t)relocation_count * SBC2_RELOCATION_BYTES))
        return -EINVAL;
    for (index = 0; index < relocation_count; ++index) {
        const uint8_t *record = bundle + relocations_offset +
                                (size_t)index * SBC2_RELOCATION_BYTES;
        uint32_t offset = mb86H_be32_get(record);
        uint32_t type = mb86H_be32_get(record + 4);
        uint32_t target = mb86H_be32_get(record + 8);
        uint32_t bytes = type == SBC2_RELOC_48 ? 6u : 4u;
        uint32_t other;
        if ((type != SBC2_RELOC_20 && type != SBC2_RELOC_32 &&
             type != SBC2_RELOC_48) || target >= body_size ||
            offset > body_size || bytes > body_size - offset)
            return -EINVAL;
        if (type == SBC2_RELOC_20) {
            const uint8_t *operand = bundle + body_offset + offset;
            uint32_t immediate = ((uint32_t)(operand[1] >> 4) << 16) |
                                 ((uint32_t)operand[2] << 8) | operand[3];
            if (operand[0] != 0x9bu || immediate != target || target >= body_size)
                return -EINVAL;
        } else if (type == SBC2_RELOC_32) {
            if (mb86H_be32_get(bundle + body_offset + offset) != target) return -EINVAL;
        } else {
            const uint8_t *operand = bundle + body_offset + offset;
            if (operand[0] != 0x9fu || (operand[1] & 0xf0u) != 0x80u ||
                mb86H_be32_get(operand + 2) != target) return -EINVAL;
        }
        for (other = 0; other < index; ++other) {
            const uint8_t *prior = bundle + relocations_offset +
                                   (size_t)other * SBC2_RELOCATION_BYTES;
            uint32_t prior_offset = mb86H_be32_get(prior);
            uint32_t prior_type = mb86H_be32_get(prior + 4);
            uint32_t prior_bytes = prior_type == SBC2_RELOC_48 ? 6u : 4u;
            if (ranges_overlap(offset, offset + bytes, prior_offset,
                               prior_offset + prior_bytes)) return -EINVAL;
        }
    }

    memset(bootstrap, 0, sizeof(*bootstrap));
    bootstrap->bundle = bundle;
    bootstrap->size = size;
    bootstrap->profile = profile;
    bootstrap->body = bundle + body_offset;
    bootstrap->relocations = bundle + relocations_offset;
    bootstrap->references = bundle + mb86H_be32_get(profile + 12);
    bootstrap->patches = bundle + mb86H_be32_get(profile + 20);
    bootstrap->body_size = body_size;
    bootstrap->body_address = expected_address;
    bootstrap->body_capacity = expected_capacity;
    bootstrap->relocation_count = relocation_count;
    bootstrap->reference_count = mb86H_be32_get(profile + 16);
    bootstrap->patch_count = mb86H_be32_get(profile + 24);
    bootstrap->original_tail = mb86H_be32_get(profile + 28) == 1u;
    *resident = bundle + resident_offset;
    return 0;
}

const uint8_t *mb86H_bootstrap_reference(const struct mb86H_bootstrap *bootstrap,
                                         uint32_t address, size_t length)
{
    return find_reference(bootstrap, address, length);
}

static int apply_relocation(const struct mb86H_bootstrap *bootstrap,
                            uint32_t address, uint8_t *out, size_t length,
                            const uint8_t *record)
{
    uint32_t offset = mb86H_be32_get(record);
    uint32_t type = mb86H_be32_get(record + 4);
    uint32_t target = mb86H_be32_get(record + 8);
    uint32_t relocated = bootstrap->body_address + target;
    uint32_t relocation_address = bootstrap->body_address + offset;
    uint32_t end;
    const uint8_t *template;
    uint8_t bytes[6];
    uint32_t first, last;
    size_t start;
    size_t width = type == SBC2_RELOC_48 ? 6u : 4u;

    if (!range_end(relocation_address, width, &end) || end <= address ||
        relocation_address >= address + length)
        return 0;
    template = bootstrap->body + offset;
    memcpy(bytes, template, width);
    if (type == SBC2_RELOC_20) {
        uint32_t immediate = ((uint32_t)(bytes[1] >> 4) << 16) |
                             ((uint32_t)bytes[2] << 8) | bytes[3];
        if (immediate != target || relocated > 0xfffffu) return -EINVAL;
        bytes[1] = (uint8_t)((bytes[1] & 15u) | ((relocated >> 16) << 4));
        bytes[2] = (uint8_t)(relocated >> 8);
        bytes[3] = (uint8_t)relocated;
    } else if (type == SBC2_RELOC_32) {
        if (mb86H_be32_get(bytes) != target) return -EINVAL;
        put_mb86H_be32_get(bytes, relocated);
    } else {
        if (bytes[0] != 0x9fu || (bytes[1] & 0xf0u) != 0x80u ||
            mb86H_be32_get(bytes + 2) != target) return -EINVAL;
        put_mb86H_be32_get(bytes + 2, relocated);
    }
    first = relocation_address > address ? relocation_address : address;
    last = end < address + length ? end : address + (uint32_t)length;
    start = first - relocation_address;
    memcpy(out + first - address, bytes + start, last - first);
    return 0;
}

int mb86H_bootstrap_read(const struct mb86H_bootstrap *bootstrap,
                         uint32_t address, void *data, size_t length)
{
    uint8_t *out = data;
    uint32_t end;
    uint32_t body_end;
    size_t index;

    if (!bootstrap || !data || !length || !range_end(address, length, &end) ||
        !range_end(bootstrap->body_address, bootstrap->body_capacity, &body_end))
        return -EINVAL;
    for (index = 0; index < length; ++index) {
        uint32_t here = address + (uint32_t)index;
        const uint8_t *source = find_reference(bootstrap, here, 1);
        if (source) out[index] = *source;
        else if (here >= bootstrap->body_address && here < body_end) out[index] = 0;
        else return -EINVAL;
    }
    for (index = 0; index < bootstrap->body_size; ++index) {
        uint32_t here = bootstrap->body_address + (uint32_t)index;
        if (here >= address && here < end) out[here - address] = bootstrap->body[index];
    }
    for (index = 0; index < bootstrap->relocation_count; ++index) {
        int rc = apply_relocation(bootstrap, address, out, length,
            record_at(bootstrap->relocations, (uint32_t)index, SBC2_RELOCATION_BYTES));
        if (rc) return rc;
    }
    for (index = 0; index < bootstrap->patch_count; ++index) {
        const uint8_t *record = record_at(bootstrap->patches, (uint32_t)index,
                                          SBC2_REFERENCE_BYTES);
        uint32_t base = mb86H_be32_get(record);
        uint32_t bytes = mb86H_be32_get(record + 4);
        uint32_t patch_end;
        uint32_t first, last;
        if (!range_end(base, bytes, &patch_end)) return -EINVAL;
        first = base > address ? base : address;
        last = patch_end < end ? patch_end : end;
        if (first < last)
            memcpy(out + first - address, bootstrap->bundle + mb86H_be32_get(record + 8) + first - base,
                   last - first);
    }
    return 0;
}
