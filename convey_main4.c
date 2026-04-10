#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "bent.h"

#ifndef SSIZE_MAX
#  define SSIZE_MAX ((size_t)(~(size_t)0) >> 1)
#endif

/* ── Compile-time alignment flag (embedded in every flags byte) ─────────── */
#ifdef b_ALIGNED
#  define B_STR_ALIGN_FLAG B_STRING_ALIGNED
#else
#  define B_STR_ALIGN_FLAG B_STRING_PACKED
#endif

/* ── BOM byte sequences ─────────────────────────────────────────────────── */
static const uint8_t BOM_UTF8[3]    = {0xEFu, 0xBBu, 0xBFu};
static const uint8_t BOM_UTF16LE[2] = {0xFFu, 0xFEu};
static const uint8_t BOM_UTF16BE[2] = {0xFEu, 0xFFu};
static const uint8_t BOM_UTF32LE[4] = {0xFFu, 0xFEu, 0x00u, 0x00u};
static const uint8_t BOM_UTF32BE[4] = {0x00u, 0x00u, 0xFEu, 0xFFu};

/*──────────────────────────────────────────────────────────────────────────────
  INTERNAL HELPERS
──────────────────────────────────────────────────────────────────────────────*/

/*
 * _null_term_size – return the null-terminator width in bytes for an encoding.
 *   UTF-32 encodings require 4 zero bytes.
 *   UTF-16 encodings require 2 zero bytes.
 *   ASCII and UTF-8 require 1 zero byte.
 */
static inline size_t _null_term_size(uint8_t encoding) {
    if (B_STR_IS_UTF32_ENC(encoding)) return 4u;
    if (B_STR_IS_UTF16_ENC(encoding)) return 2u;
    return 1u;
}

/*
 * _align_len_down – round a byte length DOWN to the encoding's code-unit
 * boundary.  Used to strip any stray trailing byte that would split a unit.
 */
static inline size_t _align_len_down(size_t byte_length, uint8_t encoding) {
    if (B_STR_IS_UTF32_ENC(encoding)) return byte_length & ~(size_t)3u;
    if (B_STR_IS_UTF16_ENC(encoding)) return byte_length & ~(size_t)1u;
    return byte_length;
}

/*
 * _align_cap_up – round a capacity UP to the next code-unit boundary.
 * Used when sizing new allocations so no capacity byte is wasted inside a unit.
 * NOTE: callers must ensure (capacity + 3) does not overflow size_t; the
 * overflow clamp in b_str_ensure guarantees this for all internal callers.
 */
static inline size_t _align_cap_up(size_t capacity, uint8_t encoding) {
    if (B_STR_IS_UTF32_ENC(encoding)) return (capacity + 3u) & ~(size_t)3u;
    if (B_STR_IS_UTF16_ENC(encoding)) return (capacity + 1u) & ~(size_t)1u;
    return capacity;
}

/* Write null_term_size zero bytes starting at buffer[position]. */
static inline void _write_null_terminator(uint8_t *buffer, size_t position,
                                          size_t null_term_size) {
    for (size_t index = 0; index < null_term_size; index++)
        buffer[position + index] = '\0';
}

/* Unaligned 16-bit read via memcpy (avoids strict-aliasing UB). */
static inline uint16_t _read_u16(const void *raw_ptr) {
    uint16_t value;
    memcpy(&value, raw_ptr, 2);
    return value;
}

/* Unaligned 32-bit read via memcpy. */
static inline uint32_t _read_u32(const void *raw_ptr) {
    uint32_t value;
    memcpy(&value, raw_ptr, 4);
    return value;
}

static inline uint16_t _byte_swap_16(uint16_t value) {
    return (uint16_t)(((value & 0x00FFu) << 8) | ((value & 0xFF00u) >> 8));
}

static inline uint32_t _byte_swap_32(uint32_t value) {
    return ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) <<  8) |
           ((value & 0x00FF0000u) >>  8) | ((value & 0xFF000000u) >> 24);
}

/* System-native UTF-16 encoding tag (compile-time constant when SDL present). */
static inline uint8_t _system_utf16_encoding(void) {
#if defined(SDL_BYTEORDER) && SDL_BYTEORDER == SDL_BIG_ENDIAN
    return B_STR_ENC_UTF16BE;
#else
    return B_STR_ENC_UTF16LE;
#endif
}

/* System-native UTF-32 encoding tag. */
static inline uint8_t _system_utf32_encoding(void) {
#if defined(SDL_BYTEORDER) && SDL_BYTEORDER == SDL_BIG_ENDIAN
    return B_STR_ENC_UTF32BE;
#else
    return B_STR_ENC_UTF32LE;
#endif
}

/*──────────────────────────────────────────────────────────────────────────────
  CORE ACCESSORS
──────────────────────────────────────────────────────────────────────────────*/

size_t b_str_hdr_size(uint8_t flags) {
    switch (flags & B_STR_TYPE_MASK) {
    case B_STR_TYPE_8:  return sizeof(b_hdr8_t);
    case B_STR_TYPE_16: return sizeof(b_hdr16_t);
    case B_STR_TYPE_32: return sizeof(b_hdr32_t);
    case B_STR_TYPE_64: return sizeof(b_hdr64_t);
    default:            return 0;
    }
}

uint8_t b_str_pick_type(size_t byte_size) {
    if (byte_size < 256u)                return B_STR_TYPE_8;
    if (byte_size < 65536u)              return B_STR_TYPE_16;
    if (byte_size <= (size_t)UINT32_MAX) return B_STR_TYPE_32;
    return B_STR_TYPE_64;
}

uint8_t b_str_enc(b_cstr_t s) {
    return s ? (uint8_t)(s[-1] & B_STR_ENC_MASK) : B_STR_ENC_ASCII;
}

void b_str_set_enc(b_str_t s, uint8_t encoding) {
    if (s) s[-1] = (uint8_t)((s[-1] & ~B_STR_ENC_MASK) | (encoding & B_STR_ENC_MASK));
}

size_t b_str_len(b_cstr_t s) {
    if (!s) return 0;
    switch (s[-1] & B_STR_TYPE_MASK) {
    case B_STR_TYPE_8:  return B_CHDR8(s)->length;
    case B_STR_TYPE_16: return B_CHDR16(s)->length;
    case B_STR_TYPE_32: return B_CHDR32(s)->length;
    case B_STR_TYPE_64: return B_CHDR64(s)->length;
    default:            return 0;
    }
}

size_t b_str_avail(b_cstr_t s) {
    if (!s) return 0;
    switch (s[-1] & B_STR_TYPE_MASK) {
    case B_STR_TYPE_8:  { const b_hdr8_t  *h = B_CHDR8(s);  return h->capacity > h->length ? (size_t)(h->capacity - h->length) : 0; }
    case B_STR_TYPE_16: { const b_hdr16_t *h = B_CHDR16(s); return h->capacity > h->length ? (size_t)(h->capacity - h->length) : 0; }
    case B_STR_TYPE_32: { const b_hdr32_t *h = B_CHDR32(s); return h->capacity > h->length ? (size_t)(h->capacity - h->length) : 0; }
    case B_STR_TYPE_64: { const b_hdr64_t *h = B_CHDR64(s); return h->capacity > h->length ? (size_t)(h->capacity - h->length) : 0; }
    default:            return 0;
    }
}

size_t b_str_cap(b_cstr_t s) { return s ? b_str_len(s) + b_str_avail(s) : 0; }

void b_str_set_lens(b_str_t s, size_t used_bytes, size_t capacity_bytes) {
    if (!s) return;
    const uint8_t encoding = s[-1] & B_STR_ENC_MASK;
    /* Round both values down to the encoding's code-unit boundary.
     * All callers that come through b_str_ensure already pass unit-aligned
     * values; this is a safety net for direct callers.                    */
    used_bytes     = _align_len_down(used_bytes,     encoding);
    capacity_bytes = _align_len_down(capacity_bytes, encoding);
    switch (s[-1] & B_STR_TYPE_MASK) {
    case B_STR_TYPE_8:  B_HDR8(s)->capacity  = (uint8_t) capacity_bytes;  B_HDR8(s)->length  = (uint8_t) used_bytes; break;
    case B_STR_TYPE_16: B_HDR16(s)->capacity = (uint16_t)capacity_bytes;  B_HDR16(s)->length = (uint16_t)used_bytes; break;
    case B_STR_TYPE_32: B_HDR32(s)->capacity = (uint32_t)capacity_bytes;  B_HDR32(s)->length = (uint32_t)used_bytes; break;
    case B_STR_TYPE_64: B_HDR64(s)->capacity = (uint64_t)capacity_bytes;  B_HDR64(s)->length = (uint64_t)used_bytes; break;
    }
}

void b_str_set_len(b_str_t s, size_t used_bytes) {
    if (!s) return;
    used_bytes = _align_len_down(used_bytes, s[-1] & B_STR_ENC_MASK);
    switch (s[-1] & B_STR_TYPE_MASK) {
    case B_STR_TYPE_8:  B_HDR8(s)->length  = (uint8_t) used_bytes; break;
    case B_STR_TYPE_16: B_HDR16(s)->length = (uint16_t)used_bytes; break;
    case B_STR_TYPE_32: B_HDR32(s)->length = (uint32_t)used_bytes; break;
    case B_STR_TYPE_64: B_HDR64(s)->length = (uint64_t)used_bytes; break;
    }
}

size_t b_str_cpcount(b_cstr_t s) {
    if (!s) return 0;
    const size_t  total_bytes = b_str_len(s);
    const uint8_t encoding    = b_str_enc(s);
    if (!total_bytes) return 0;

    if (encoding == B_STR_ENC_UTF8) {
        size_t codepoint_count = 0;
        size_t byte_pos        = 0;
        while (byte_pos < total_bytes) {
            utf8proc_int32_t codepoint;
            const size_t remaining = total_bytes - byte_pos;
            const utf8proc_ssize_t advance = utf8proc_iterate(
                s + byte_pos,
                (utf8proc_ssize_t)(remaining < (size_t)SSIZE_MAX
                                   ? remaining : (size_t)SSIZE_MAX),
                &codepoint);
            /* On invalid sequence, advance == 0 or negative; skip 1 byte. */
            byte_pos += (advance > 0) ? (size_t)advance : 1u;
            codepoint_count++;
        }
        return codepoint_count;
    }

    if (B_STR_IS_UTF16_ENC(encoding)) {
        const bool   is_big_endian = (encoding == B_STR_ENC_UTF16BE);
        const size_t unit_count    = total_bytes / 2u;
        size_t codepoint_count     = 0;
        size_t unit_index          = 0;
        while (unit_index < unit_count) {
            uint16_t first_unit = _read_u16(s + unit_index * 2u);
            if (is_big_endian) first_unit = _byte_swap_16(first_unit);
            unit_index++;
            /* Check for a surrogate pair (high surrogate 0xD800–0xDBFF). */
            if (first_unit >= 0xD800u && first_unit <= 0xDBFFu
                    && unit_index < unit_count) {
                uint16_t second_unit = _read_u16(s + unit_index * 2u);
                if (is_big_endian) second_unit = _byte_swap_16(second_unit);
                /* Low surrogate 0xDC00–0xDFFF: consume the pair as one CP. */
                if (second_unit >= 0xDC00u && second_unit <= 0xDFFFu)
                    unit_index++;
            }
            codepoint_count++;
        }
        return codepoint_count;
    }

    if (B_STR_IS_UTF32_ENC(encoding)) {
        /* Each code unit (4 bytes) is exactly one Unicode codepoint. */
        return total_bytes / 4u;
    }

    /* ASCII: one byte is one codepoint. */
    return total_bytes;
}

/*──────────────────────────────────────────────────────────────────────────────
  LIFECYCLE
──────────────────────────────────────────────────────────────────────────────*/

b_str_t b_str_new_pro(const void *data, size_t byte_len, uint8_t encoding) {
    byte_len = _align_len_down(byte_len, encoding);

    const uint8_t type            = b_str_pick_type(byte_len);
    const size_t  header_size     = b_str_hdr_size(type);
    const size_t  null_term_size  = _null_term_size(encoding);

    if (byte_len > SIZE_MAX - header_size - null_term_size) return NULL;

    uint8_t *mem = (uint8_t*)SDL_malloc(header_size + byte_len + null_term_size);
    if (!mem) return NULL;

    b_str_t s = mem + header_size;
    s[-1] = (uint8_t)(type | B_STR_ALIGN_FLAG | (encoding & B_STR_ENC_MASK));

    if      (data)         memcpy(s, data, byte_len);
    else if (byte_len > 0) memset(s, 0,    byte_len);

    b_str_set_lens(s, byte_len, byte_len);
    _write_null_terminator(s, byte_len, null_term_size);
    return s;
}

b_str_t b_str_new(const char *cstr) {
    return b_str_new_pro(cstr, cstr ? strlen(cstr) : 0, B_STR_ENC_ASCII);
}

b_str_t b_str_new_static_pro(const void *data, size_t byte_len,
                              size_t total_capacity, uint8_t encoding) {
    byte_len       = _align_len_down(byte_len,       encoding);
    total_capacity = _align_len_down(total_capacity, encoding);
    if (byte_len > total_capacity) return NULL;

    const uint8_t type           = b_str_pick_type(total_capacity);
    const size_t  header_size    = b_str_hdr_size(type);
    const size_t  null_term_size = _null_term_size(encoding);

    if (total_capacity > SIZE_MAX - header_size - null_term_size) return NULL;

    uint8_t *mem = (uint8_t*)SDL_malloc(header_size + total_capacity + null_term_size);
    if (!mem) return NULL;

    b_str_t s = mem + header_size;
    s[-1] = (uint8_t)(type | B_STR_ALIGN_FLAG | B_STR_STATIC
                      | (encoding & B_STR_ENC_MASK));

    if      (data)         memcpy(s, data, byte_len);
    else if (byte_len > 0) memset(s, 0,    byte_len);

    b_str_set_lens(s, byte_len, total_capacity);
    _write_null_terminator(s, byte_len, null_term_size);
    return s;
}

b_str_t b_str_new_static(const char *cstr, size_t extra_bytes) {
    const size_t byte_len = cstr ? strlen(cstr) : 0;
    if (byte_len > SIZE_MAX - extra_bytes) return NULL;
    return b_str_new_static_pro(cstr, byte_len, byte_len + extra_bytes,
                                B_STR_ENC_ASCII);
}

void b_str_free(b_str_t s) {
    if (s) SDL_free((uint8_t*)s - b_str_hdr_size(s[-1]));
}

b_str_t b_str_dup(b_cstr_t s) {
    return s ? b_str_new_pro(s, b_str_len(s), b_str_enc(s)) : NULL;
}

void b_str_clear(b_str_t s) {
    if (!s) return;
    const size_t null_term_size = _null_term_size(b_str_enc(s));
    b_str_set_len(s, 0);
    _write_null_terminator(s, 0, null_term_size);
}

bool b_str_empty(b_cstr_t s) { return !s || b_str_len(s) == 0; }

b_str_t b_str_to_dyn(b_str_t s) {
    if (!s || !(s[-1] & B_STR_STATIC)) return s;
    b_str_t dynamic_copy = b_str_new_pro(s, b_str_len(s), b_str_enc(s));
    if (!dynamic_copy) return s;  /* return original on allocation failure */
    b_str_free(s);
    return dynamic_copy;
}

/*──────────────────────────────────────────────────────────────────────────────
  SLICE CONSTRUCTORS
──────────────────────────────────────────────────────────────────────────────*/

b_str_t b_str_from_slice   (b_slice_t    slice) { return b_str_new_pro(slice.data, slice.len, B_STR_ENC_ASCII);  }
b_str_t b_str_from_u8slice (b_u8slice_t  slice) { return b_str_new_pro(slice.data, slice.len, B_STR_ENC_UTF8);   }
b_str_t b_str_from_u16slice(b_u16slice_t slice) {
    /* Align down to a whole number of UTF-16 code units (2 bytes each). */
    return b_str_new_pro(slice.data, slice.len & ~(size_t)1u, B_STR_ENC_UTF16LE);
}
b_str_t b_str_from_u32slice(b_u32slice_t slice) {
    /* Align down to a whole number of UTF-32 code units (4 bytes each). */
    return b_str_new_pro(slice.data, slice.len & ~(size_t)3u, B_STR_ENC_UTF32LE);
}

b_str_t b_str_from_u16(const uint16_t *units, size_t unit_count) {
    if (!units && unit_count > 0) return NULL;
    if (unit_count > SIZE_MAX / 2u) return NULL;
    return b_str_new_pro(units, unit_count * 2u, B_STR_ENC_UTF16LE);
}

b_str_t b_str_from_u32(const uint32_t *units, size_t unit_count) {
    if (!units && unit_count > 0) return NULL;
    if (unit_count > SIZE_MAX / 4u) return NULL;
    return b_str_new_pro(units, unit_count * 4u, B_STR_ENC_UTF32LE);
}

/*──────────────────────────────────────────────────────────────────────────────
  CAPACITY MANAGEMENT
──────────────────────────────────────────────────────────────────────────────*/

b_str_t b_str_ensure(b_str_t s, size_t extra_bytes) {
    if (!s)                               return NULL;
    if (b_str_avail(s) >= extra_bytes)    return s;
    if (s[-1] & B_STR_STATIC)            return s;  /* static strings never grow */

    const uint8_t encoding       = b_str_enc(s);
    const size_t  null_term_size = _null_term_size(encoding);
    const size_t  current_len    = b_str_len(s);
    const size_t  current_cap    = b_str_cap(s);

    if (extra_bytes > SIZE_MAX - current_len) return s;
    size_t needed_cap = _align_cap_up(current_len + extra_bytes, encoding);

    /* Growth strategy: double below 1 MB, add 1 MB chunks above. */
    size_t new_cap = needed_cap;
    if (new_cap < B_STR_ONE_MEG) {
        const size_t doubled = (current_cap > SIZE_MAX / 2u)
                               ? SIZE_MAX : current_cap * 2u;
        if (doubled > new_cap) new_cap = doubled;
    } else {
        new_cap = (SIZE_MAX - B_STR_ONE_MEG >= new_cap)
                  ? new_cap + B_STR_ONE_MEG : SIZE_MAX;
    }
    new_cap = _align_cap_up(new_cap, encoding);

    uint8_t new_type       = b_str_pick_type(new_cap);
    size_t  new_header_size = b_str_hdr_size(new_type);

    /* Overflow clamp: ensure the total allocation fits in size_t. */
    if (new_cap > SIZE_MAX - new_header_size - null_term_size) {
        new_cap = _align_len_down(SIZE_MAX - new_header_size - null_term_size,
                                  encoding);
        new_type        = b_str_pick_type(new_cap);
        new_header_size = b_str_hdr_size(new_type);
        if (new_cap < needed_cap) return s;  /* truly out of addressable space */
    }

    const uint8_t current_type       = (uint8_t)(s[-1] & B_STR_TYPE_MASK);
    const size_t  current_header_size = b_str_hdr_size(current_type);
    uint8_t *mem;

    if (current_type == new_type) {
        /* Same header size: realloc in place. */
        mem = (uint8_t*)SDL_realloc(s - current_header_size,
                                    new_header_size + new_cap + null_term_size);
        if (!mem) return s;
        s = mem + new_header_size;
        /* s[-1] (flags byte) is preserved by SDL_realloc. */
    } else {
        /* Header size is changing: alloc new block, copy data, free old. */
        mem = (uint8_t*)SDL_malloc(new_header_size + new_cap + null_term_size);
        if (!mem) return s;
        memcpy(mem + new_header_size, s, current_len + null_term_size);
        SDL_free(s - current_header_size);
        s = mem + new_header_size;
        /* Set new flags: new type | align flag | encoding; clear STATIC. */
        s[-1] = (uint8_t)(new_type | B_STR_ALIGN_FLAG | encoding);
    }

    b_str_set_lens(s, current_len, new_cap);
    return s;
}

b_str_t b_str_reserve(b_str_t s, size_t extra_bytes) {
    return b_str_ensure(s, extra_bytes);
}

b_str_t b_str_fit(b_str_t s) {
    if (!s || (s[-1] & B_STR_STATIC)) return s;

    const uint8_t encoding          = b_str_enc(s);
    const size_t  used_bytes        = b_str_len(s);
    const uint8_t current_type      = (uint8_t)(s[-1] & B_STR_TYPE_MASK);
    const size_t  current_hdr_size  = b_str_hdr_size(current_type);
    const size_t  null_term_size    = _null_term_size(encoding);
    const uint8_t tightest_type     = b_str_pick_type(used_bytes);
    const size_t  tightest_hdr_size = b_str_hdr_size(tightest_type);

    if (current_type == tightest_type) {
        /* Same header size: shrink in place. */
        uint8_t *mem = (uint8_t*)SDL_realloc(s - current_hdr_size,
                                             current_hdr_size + used_bytes
                                             + null_term_size);
        if (!mem) return s;
        s = mem + current_hdr_size;
    } else {
        /* Header size changing: alloc, copy, free. */
        uint8_t *mem = (uint8_t*)SDL_malloc(tightest_hdr_size + used_bytes
                                            + null_term_size);
        if (!mem) return s;
        memcpy(mem + tightest_hdr_size, s, used_bytes + null_term_size);
        SDL_free(s - current_hdr_size);
        s = mem + tightest_hdr_size;
        s[-1] = (uint8_t)(tightest_type | B_STR_ALIGN_FLAG | encoding);
    }

    b_str_set_lens(s, used_bytes, used_bytes);
    _write_null_terminator(s, used_bytes, null_term_size);
    return s;
}

void b_str_arr_fit(b_str_t *array, size_t count) {
    if (!array) return;
    for (size_t index = 0; index < count; index++)
        if (array[index]) array[index] = b_str_fit(array[index]);
}

/*──────────────────────────────────────────────────────────────────────────────
  APPENDING & CONCATENATION
──────────────────────────────────────────────────────────────────────────────*/

b_str_t b_str_append_pro(b_str_t s, const void *data, size_t byte_len) {
    if (!s || !data || !byte_len) return s;

    const uint8_t encoding       = b_str_enc(s);
    const size_t  null_term_size = _null_term_size(encoding);
    const size_t  current_len    = b_str_len(s);

    byte_len = _align_len_down(byte_len, encoding);
    if (!byte_len) return s;
    if (byte_len > SIZE_MAX - current_len) return s;

    /* Detect self-overlap: record the source offset before potential realloc. */
    const uintptr_t source_addr = (uintptr_t)data;
    const uintptr_t buffer_addr = (uintptr_t)s;
    const bool is_overlap = (source_addr >= buffer_addr
                              && source_addr < (uintptr_t)(s + current_len
                                                            + null_term_size));
    const ptrdiff_t overlap_offset = is_overlap
        ? (ptrdiff_t)((const uint8_t*)data - s) : 0;

    s = b_str_ensure(s, byte_len);
    if (b_str_avail(s) < byte_len) return s;  /* OOM / static full */

    memmove(s + current_len,
            is_overlap ? (const void*)(s + overlap_offset) : data,
            byte_len);

    const size_t new_len = current_len + byte_len;
    b_str_set_len(s, new_len);
    _write_null_terminator(s, new_len, null_term_size);
    return s;
}

b_str_t b_str_append(b_str_t s, const char *cstr) {
    return b_str_append_pro(s, cstr, cstr ? strlen(cstr) : 0);
}

b_str_t b_str_append_sl(b_str_t s, b_slice_t slice) {
    return slice.data ? b_str_append_pro(s, slice.data, slice.len) : s;
}

b_str_t b_str_append_u8(b_str_t s, b_u8slice_t slice) {
    if (!slice.data || !slice.len) return s;
    /* Promote ASCII string to UTF-8 before appending UTF-8 data. */
    if (b_str_enc(s) == B_STR_ENC_ASCII) b_str_set_enc(s, B_STR_ENC_UTF8);
    return b_str_append_pro(s, slice.data, slice.len);
}

b_str_t b_str_append_u16(b_str_t s, b_u16slice_t slice) {
    const size_t aligned_len = slice.len & ~(size_t)1u;
    return (slice.data && aligned_len)
           ? b_str_append_pro(s, slice.data, aligned_len) : s;
}

b_str_t b_str_append_u32(b_str_t s, b_u32slice_t slice) {
    const size_t aligned_len = slice.len & ~(size_t)3u;
    return (slice.data && aligned_len)
           ? b_str_append_pro(s, slice.data, aligned_len) : s;
}

b_str_t b_str_concat(b_cstr_t a, b_cstr_t b) {
    const uint8_t encoding = a ? b_str_enc(a) : B_STR_ENC_ASCII;
    const size_t  len_a    = a ? b_str_len(a) : 0;
    const size_t  len_b    = b ? b_str_len(b) : 0;
    if (len_a > SIZE_MAX - len_b) return NULL;

    b_str_t result = b_str_new_pro(a, len_a, encoding);
    if (!result) return NULL;
    if (len_b) {
        const size_t len_before_append = b_str_len(result);
        result = b_str_append_pro(result, b, len_b);
        if (b_str_len(result) == len_before_append) {
            b_str_free(result);
            return NULL;  /* append failed (OOM) */
        }
    }
    return result;
}

/*──────────────────────────────────────────────────────────────────────────────
  SLICE EXTRACTORS
──────────────────────────────────────────────────────────────────────────────*/

b_slice_t b_slice_of(b_cstr_t s) {
    return (b_slice_t){s, b_str_len(s)};
}

b_slice_t b_subslice(b_cstr_t s, size_t byte_offset, size_t byte_len) {
    b_slice_t empty = {NULL, 0};
    if (!s) return empty;
    const size_t total_bytes = b_str_len(s);
    if (byte_offset >= total_bytes) return empty;
    const size_t remaining = total_bytes - byte_offset;
    return (b_slice_t){s + byte_offset, byte_len > remaining ? remaining : byte_len};
}

b_u8slice_t b_u8slice_of(b_cstr_t s) {
    return (b_u8slice_t){s, b_str_len(s)};
}

b_u8slice_t b_u8subslice(b_cstr_t s, size_t byte_offset, size_t byte_len) {
    b_u8slice_t empty = {NULL, 0};
    if (!s) return empty;
    const size_t total_bytes = b_str_len(s);
    if (byte_offset >= total_bytes) return empty;
    const size_t remaining = total_bytes - byte_offset;
    return (b_u8slice_t){s + byte_offset, byte_len > remaining ? remaining : byte_len};
}

b_u8slice_t b_u8subslice_cp(b_cstr_t s, size_t codepoint_offset,
                             size_t codepoint_count) {
    b_u8slice_t empty = {NULL, 0};
    if (!s) return empty;
    const size_t total_bytes = b_str_len(s);
    size_t byte_pos = 0;

    /* Advance to the codepoint at codepoint_offset. */
    for (size_t cp_index = 0; cp_index < codepoint_offset && byte_pos < total_bytes;
         cp_index++) {
        utf8proc_int32_t codepoint;
        const size_t remaining = total_bytes - byte_pos;
        const utf8proc_ssize_t advance = utf8proc_iterate(
            s + byte_pos,
            (utf8proc_ssize_t)(remaining < (size_t)SSIZE_MAX
                               ? remaining : (size_t)SSIZE_MAX),
            &codepoint);
        byte_pos += (advance > 0) ? (size_t)advance : 1u;
    }
    if (byte_pos >= total_bytes && codepoint_offset > 0) return empty;

    const size_t slice_start = byte_pos;

    /* Advance over codepoint_count codepoints from the start position. */
    for (size_t cp_index = 0; cp_index < codepoint_count && byte_pos < total_bytes;
         cp_index++) {
        utf8proc_int32_t codepoint;
        const size_t remaining = total_bytes - byte_pos;
        const utf8proc_ssize_t advance = utf8proc_iterate(
            s + byte_pos,
            (utf8proc_ssize_t)(remaining < (size_t)SSIZE_MAX
                               ? remaining : (size_t)SSIZE_MAX),
            &codepoint);
        byte_pos += (advance > 0) ? (size_t)advance : 1u;
    }
    return (b_u8slice_t){s + slice_start, byte_pos - slice_start};
}

b_u16slice_t b_u16slice_of(b_cstr_t s) {
    return (b_u16slice_t){s, b_str_len(s)};
}

b_u16slice_t b_u16subslice(b_cstr_t s, size_t byte_offset, size_t byte_len) {
    b_u16slice_t empty = {NULL, 0};
    if (!s) return empty;
    /* Align to 2-byte code-unit boundaries. */
    byte_offset &= ~(size_t)1u;
    byte_len    &= ~(size_t)1u;
    const size_t total_bytes = b_str_len(s) & ~(size_t)1u;
    if (byte_offset >= total_bytes) return empty;
    const size_t remaining = total_bytes - byte_offset;
    return (b_u16slice_t){s + byte_offset,
                          byte_len > remaining ? remaining : byte_len};
}

b_u16slice_t b_u16subslice_cp(b_cstr_t s, size_t codepoint_offset,
                               size_t codepoint_count) {
    b_u16slice_t empty = {NULL, 0};
    if (!s) return empty;
    const bool   is_big_endian = (b_str_enc(s) == B_STR_ENC_UTF16BE);
    const size_t unit_count    = b_str_len(s) / 2u;
    size_t unit_index = 0;

    /* Skip codepoint_offset codepoints. */
    for (size_t cp_index = 0; cp_index < codepoint_offset && unit_index < unit_count;
         cp_index++) {
        uint16_t first_unit = _read_u16(s + unit_index * 2u);
        if (is_big_endian) first_unit = _byte_swap_16(first_unit);
        unit_index++;
        if (first_unit >= 0xD800u && first_unit <= 0xDBFFu
                && unit_index < unit_count) {
            uint16_t second_unit = _read_u16(s + unit_index * 2u);
            if (is_big_endian) second_unit = _byte_swap_16(second_unit);
            if (second_unit >= 0xDC00u && second_unit <= 0xDFFFu) unit_index++;
        }
    }
    if (unit_index >= unit_count && codepoint_offset > 0) return empty;

    const size_t slice_start_unit = unit_index;

    /* Collect codepoint_count codepoints. */
    for (size_t cp_index = 0; cp_index < codepoint_count && unit_index < unit_count;
         cp_index++) {
        uint16_t first_unit = _read_u16(s + unit_index * 2u);
        if (is_big_endian) first_unit = _byte_swap_16(first_unit);
        unit_index++;
        if (first_unit >= 0xD800u && first_unit <= 0xDBFFu
                && unit_index < unit_count) {
            uint16_t second_unit = _read_u16(s + unit_index * 2u);
            if (is_big_endian) second_unit = _byte_swap_16(second_unit);
            if (second_unit >= 0xDC00u && second_unit <= 0xDFFFu) unit_index++;
        }
    }
    return (b_u16slice_t){s + slice_start_unit * 2u,
                          (unit_index - slice_start_unit) * 2u};
}

size_t b_u16slice_units(b_u16slice_t slice) { return slice.len / 2u; }

/* ── UTF-32 slice extractors ─────────────────────────────────────────────── */

b_u32slice_t b_u32slice_of(b_cstr_t s) {
    return (b_u32slice_t){s, b_str_len(s)};
}

b_u32slice_t b_u32subslice(b_cstr_t s, size_t byte_offset, size_t byte_len) {
    b_u32slice_t empty = {NULL, 0};
    if (!s) return empty;
    byte_offset &= ~(size_t)3u;
    byte_len    &= ~(size_t)3u;
    const size_t total_bytes = b_str_len(s) & ~(size_t)3u;
    if (byte_offset >= total_bytes) return empty;
    const size_t remaining = total_bytes - byte_offset;
    return (b_u32slice_t){s + byte_offset,
                          byte_len > remaining ? remaining : byte_len};
}

b_u32slice_t b_u32subslice_cp(b_cstr_t s, size_t codepoint_offset,
                               size_t codepoint_count) {
    b_u32slice_t empty = {NULL, 0};
    if (!s) return empty;
    const size_t total_bytes = b_str_len(s) & ~(size_t)3u;
    const size_t unit_count  = total_bytes / 4u;
    if (codepoint_offset >= unit_count) return empty;
    const size_t available = unit_count - codepoint_offset;
    const size_t actual_count = codepoint_count > available
                                ? available : codepoint_count;
    return (b_u32slice_t){s + codepoint_offset * 4u, actual_count * 4u};
}

size_t b_u32slice_units(b_u32slice_t slice) { return slice.len / 4u; }

/* ── Generic codepoint-indexed subslice ─────────────────────────────────── */

b_slice_t b_subslice_cp(b_cstr_t s, size_t codepoint_offset,
                         size_t codepoint_count) {
    b_slice_t empty = {NULL, 0};
    if (!s) return empty;
    const uint8_t encoding = b_str_enc(s);

    if (B_STR_IS_UTF32_ENC(encoding)) {
        b_u32slice_t u32 = b_u32subslice_cp(s, codepoint_offset, codepoint_count);
        return (b_slice_t){u32.data, u32.len};
    }
    if (B_STR_IS_UTF16_ENC(encoding)) {
        b_u16slice_t u16 = b_u16subslice_cp(s, codepoint_offset, codepoint_count);
        return (b_slice_t){u16.data, u16.len};
    }
    if (encoding == B_STR_ENC_UTF8) {
        b_u8slice_t u8 = b_u8subslice_cp(s, codepoint_offset, codepoint_count);
        return (b_slice_t){u8.data, u8.len};
    }
    /* ASCII: one byte == one codepoint */
    const size_t total_bytes = b_str_len(s);
    if (codepoint_offset >= total_bytes) return empty;
    const size_t remaining = total_bytes - codepoint_offset;
    return (b_slice_t){s + codepoint_offset,
                       codepoint_count > remaining ? remaining : codepoint_count};
}

/*──────────────────────────────────────────────────────────────────────────────
  ENCODING CONVERTERS – SDL_iconv
  "UTF-16LE", "UTF-16BE", "UTF-32LE", "UTF-32BE" never insert a BOM.

  Bug notes:
  • SDL_ICONV_EINVAL (= (size_t)-4) means the input ended with an incomplete
    multibyte sequence.  For a complete b_str buffer this should not occur, but
    we treat it as an error to avoid silently dropping the tail of the input.
  • In practice SDL_iconv may also return SDL_ICONV_EILSEQ for individual
    invalid sequences; those are also treated as hard errors.
──────────────────────────────────────────────────────────────────────────────*/

static b_str_t _b_iconv(b_cstr_t input, size_t input_byte_count,
                         const char *from_encoding, const char *to_encoding,
                         uint8_t output_encoding_tag) {
    SDL_iconv_t conversion_descriptor = SDL_iconv_open(to_encoding,
                                                        from_encoding);
    if (conversion_descriptor == (SDL_iconv_t)-1) return NULL;

    /* 4× headroom covers UTF-8→UTF-16 (×2) and UTF-8→UTF-32 (×4).
     * Guard against overflow: input_byte_count must be < SIZE_MAX/4 - 2.  */
    if (input_byte_count > (SIZE_MAX - 8u) / 4u) {
        SDL_iconv_close(conversion_descriptor);
        return NULL;
    }
    size_t   output_capacity = input_byte_count * 4u + 8u;
    uint8_t *output_buffer   = (uint8_t*)SDL_malloc(output_capacity);
    if (!output_buffer) {
        SDL_iconv_close(conversion_descriptor);
        return NULL;
    }

    const char *input_ptr     = (const char*)input;
    size_t      input_remaining  = input_byte_count;
    char       *output_ptr    = (char*)output_buffer;
    size_t      output_remaining = output_capacity;

    const size_t iconv_result = SDL_iconv(conversion_descriptor,
                                           &input_ptr, &input_remaining,
                                           &output_ptr, &output_remaining);
    SDL_iconv_close(conversion_descriptor);

    /* Check all four SDL_iconv error codes. */
    if (iconv_result == SDL_ICONV_ERROR  ||
        iconv_result == SDL_ICONV_E2BIG  ||
        iconv_result == SDL_ICONV_EILSEQ ||
        iconv_result == SDL_ICONV_EINVAL) {
        SDL_free(output_buffer);
        return NULL;
    }

    /* If any input bytes remain unconverted, the input was malformed. */
    if (input_remaining != 0) {
        SDL_free(output_buffer);
        return NULL;
    }

    const size_t output_byte_count = output_capacity - output_remaining;
    b_str_t result = b_str_new_pro(output_buffer, output_byte_count,
                                    output_encoding_tag);
    SDL_free(output_buffer);
    return result;
}

b_str_t b_str_to_utf16(b_cstr_t s) {
    if (!s) return NULL;
    if (b_str_enc(s) == B_STR_ENC_UTF16LE) return b_str_dup(s);
    /* Route any non-UTF-8 encoding through UTF-8 first. */
    if (b_str_enc(s) == B_STR_ENC_UTF16BE
            || B_STR_IS_UTF32_ENC(b_str_enc(s))) {
        b_str_t intermediate = b_str_to_utf8(s);
        if (!intermediate) return NULL;
        b_str_t result = _b_iconv(intermediate, b_str_len(intermediate),
                                   "UTF-8", "UTF-16LE", B_STR_ENC_UTF16LE);
        b_str_free(intermediate);
        return result;
    }
    /* ASCII or UTF-8 input: convert directly. */
    return _b_iconv(s, b_str_len(s), "UTF-8", "UTF-16LE", B_STR_ENC_UTF16LE);
}

b_str_t b_str_to_utf16be(b_cstr_t s) {
    if (!s) return NULL;
    if (b_str_enc(s) == B_STR_ENC_UTF16BE) return b_str_dup(s);
    if (B_STR_IS_UTF16_ENC(b_str_enc(s)) || B_STR_IS_UTF32_ENC(b_str_enc(s))) {
        b_str_t intermediate = b_str_to_utf8(s);
        if (!intermediate) return NULL;
        b_str_t result = _b_iconv(intermediate, b_str_len(intermediate),
                                   "UTF-8", "UTF-16BE", B_STR_ENC_UTF16BE);
        b_str_free(intermediate);
        return result;
    }
    return _b_iconv(s, b_str_len(s), "UTF-8", "UTF-16BE", B_STR_ENC_UTF16BE);
}

b_str_t b_str_to_utf32le(b_cstr_t s) {
    if (!s) return NULL;
    if (b_str_enc(s) == B_STR_ENC_UTF32LE) return b_str_dup(s);
    if (B_STR_IS_UTF16_ENC(b_str_enc(s)) || b_str_enc(s) == B_STR_ENC_UTF32BE) {
        b_str_t intermediate = b_str_to_utf8(s);
        if (!intermediate) return NULL;
        b_str_t result = _b_iconv(intermediate, b_str_len(intermediate),
                                   "UTF-8", "UTF-32LE", B_STR_ENC_UTF32LE);
        b_str_free(intermediate);
        return result;
    }
    return _b_iconv(s, b_str_len(s), "UTF-8", "UTF-32LE", B_STR_ENC_UTF32LE);
}

b_str_t b_str_to_utf32be(b_cstr_t s) {
    if (!s) return NULL;
    if (b_str_enc(s) == B_STR_ENC_UTF32BE) return b_str_dup(s);
    if (B_STR_IS_UTF16_ENC(b_str_enc(s)) || b_str_enc(s) == B_STR_ENC_UTF32LE) {
        b_str_t intermediate = b_str_to_utf8(s);
        if (!intermediate) return NULL;
        b_str_t result = _b_iconv(intermediate, b_str_len(intermediate),
                                   "UTF-8", "UTF-32BE", B_STR_ENC_UTF32BE);
        b_str_free(intermediate);
        return result;
    }
    return _b_iconv(s, b_str_len(s), "UTF-8", "UTF-32BE", B_STR_ENC_UTF32BE);
}

b_str_t b_str_to_utf8(b_cstr_t s) {
    if (!s) return NULL;
    const uint8_t encoding = b_str_enc(s);
    /* ASCII and UTF-8 share the same byte layout; a dup + re-tag suffices. */
    if (!B_STR_IS_UTF16_ENC(encoding) && !B_STR_IS_UTF32_ENC(encoding)) {
        b_str_t result = b_str_dup(s);
        if (result) b_str_set_enc(result, B_STR_ENC_UTF8);
        return result;
    }
    const char *from_encoding;
    if      (encoding == B_STR_ENC_UTF16BE) from_encoding = "UTF-16BE";
    else if (encoding == B_STR_ENC_UTF16LE) from_encoding = "UTF-16LE";
    else if (encoding == B_STR_ENC_UTF32BE) from_encoding = "UTF-32BE";
    else                                    from_encoding = "UTF-32LE";
    return _b_iconv(s, b_str_len(s), from_encoding, "UTF-8", B_STR_ENC_UTF8);
}

/*
 * b_str_utf8_norm – NFC-normalise a NUL-terminated UTF-8 C-string.
 *
 * utf8proc_map allocates via the system allocator (malloc/free), not SDL.
 * We copy its output into an SDL allocation before calling free().
 * This is intentional: utf8proc is an external library and we cannot change
 * its allocator.
 */
b_str_t b_str_utf8_norm(const char *null_terminated_utf8) {
    if (!null_terminated_utf8) return NULL;
    utf8proc_uint8_t *temp_ptr = NULL;
    const utf8proc_ssize_t output_byte_count = utf8proc_map(
        (const utf8proc_uint8_t*)null_terminated_utf8,
        0,           /* 0 means "use NULLTERM option to find length" */
        &temp_ptr,
        (utf8proc_option_t)(UTF8PROC_NULLTERM | UTF8PROC_STABLE
                            | UTF8PROC_COMPOSE));
    if (output_byte_count < 0) {
        free(temp_ptr);  /* utf8proc uses system malloc; must use free() here */
        return NULL;
    }
    b_str_t result = b_str_new_pro(temp_ptr, (size_t)output_byte_count,
                                    B_STR_ENC_UTF8);
    free(temp_ptr);  /* same: system malloc, not SDL */
    return result;
}

/*──────────────────────────────────────────────────────────────────────────────
  CASE CONVERSION  –  UTF-8 / ASCII only
  UTF-16 and UTF-32 strings are not supported and return NULL.
──────────────────────────────────────────────────────────────────────────────*/

/*
 * b_str_lower
 *
 * Performs Unicode case-folding using utf8proc_decompose_char with
 * UTF8PROC_CASEFOLD | UTF8PROC_DECOMPOSE | UTF8PROC_COMPOSE.
 * The DECOMPOSE flag ensures that characters requiring canonical decomposition
 * before folding (e.g. some precomposed letters) are handled correctly.
 * COMPOSE re-assembles the result into NFC, matching b_str_utf8_norm output.
 *
 * On invalid UTF-8 sequences: the offending byte is passed through unchanged
 * (no replacement character is substituted).
 */
b_str_t b_str_lower(b_cstr_t s) {
    if (!s) return NULL;
    const uint8_t encoding = b_str_enc(s);
    if (B_STR_IS_UTF16_ENC(encoding) || B_STR_IS_UTF32_ENC(encoding))
        return NULL;

    const size_t input_length = b_str_len(s);
    /* Worst-case: each byte expands to at most 4 UTF-8 bytes after casefold. */
    size_t   output_capacity = input_length * 4u + 16u;
    uint8_t *output_buffer   = (uint8_t*)SDL_malloc(output_capacity);
    if (!output_buffer) return NULL;

    size_t input_position  = 0;
    size_t output_position = 0;

    while (input_position < input_length) {
        utf8proc_int32_t codepoint;
        const size_t remaining = input_length - input_position;
        const utf8proc_ssize_t advance = utf8proc_iterate(
            s + input_position,
            (utf8proc_ssize_t)(remaining < (size_t)SSIZE_MAX
                               ? remaining : (size_t)SSIZE_MAX),
            &codepoint);

        /* On invalid sequence: pass the byte through unchanged. */
        if (advance <= 0) {
            if (output_position + 1u > output_capacity) {
                output_capacity *= 2u;
                uint8_t *resized = (uint8_t*)SDL_realloc(output_buffer,
                                                          output_capacity);
                if (!resized) { SDL_free(output_buffer); return NULL; }
                output_buffer = resized;
            }
            output_buffer[output_position++] = s[input_position++];
            continue;
        }
        input_position += (size_t)advance;

        /* Decompose and casefold; may yield up to 4 codepoints. */
        utf8proc_int32_t folded_codepoints[4];
        int last_combining_class = 0;
        const utf8proc_ssize_t fold_count = utf8proc_decompose_char(
            codepoint,
            folded_codepoints,
            (utf8proc_ssize_t)(sizeof folded_codepoints
                               / sizeof *folded_codepoints),
            (utf8proc_option_t)(UTF8PROC_CASEFOLD | UTF8PROC_DECOMPOSE
                                | UTF8PROC_COMPOSE),
            &last_combining_class);

        const int total_codepoints = (fold_count > 0) ? (int)fold_count : 1;
        if (fold_count <= 0) folded_codepoints[0] = utf8proc_tolower(codepoint);

        for (int fold_index = 0; fold_index < total_codepoints; fold_index++) {
            /* Each encoded codepoint is at most 4 bytes in UTF-8. */
            if (output_position + 4u > output_capacity) {
                output_capacity = output_capacity * 2u + 16u;
                uint8_t *resized = (uint8_t*)SDL_realloc(output_buffer,
                                                          output_capacity);
                if (!resized) { SDL_free(output_buffer); return NULL; }
                output_buffer = resized;
            }
            const utf8proc_ssize_t bytes_written = utf8proc_encode_char(
                folded_codepoints[fold_index], output_buffer + output_position);
            if (bytes_written > 0)
                output_position += (size_t)bytes_written;
        }
    }

    b_str_t result = b_str_new_pro(output_buffer, output_position,
                                    B_STR_ENC_UTF8);
    SDL_free(output_buffer);
    return result;
}

/*
 * b_str_upper
 *
 * Applies per-codepoint uppercase mapping via utf8proc_toupper.
 * This is a simple (non-decomposing) uppercase: multi-codepoint uppercase
 * expansions (rare edge cases) are not applied.  This is an inherent
 * limitation of the utf8proc API for uppercasing; use b_str_lower for
 * case-insensitive comparison instead.
 *
 * On invalid UTF-8 sequences: the offending byte is passed through unchanged.
 */
b_str_t b_str_upper(b_cstr_t s) {
    if (!s) return NULL;
    const uint8_t encoding = b_str_enc(s);
    if (B_STR_IS_UTF16_ENC(encoding) || B_STR_IS_UTF32_ENC(encoding))
        return NULL;

    const size_t input_length = b_str_len(s);
    size_t   output_capacity = input_length * 4u + 16u;
    uint8_t *output_buffer   = (uint8_t*)SDL_malloc(output_capacity);
    if (!output_buffer) return NULL;

    size_t input_position  = 0;
    size_t output_position = 0;

    while (input_position < input_length) {
        utf8proc_int32_t codepoint;
        const size_t remaining = input_length - input_position;
        const utf8proc_ssize_t advance = utf8proc_iterate(
            s + input_position,
            (utf8proc_ssize_t)(remaining < (size_t)SSIZE_MAX
                               ? remaining : (size_t)SSIZE_MAX),
            &codepoint);

        /* On invalid sequence: pass the byte through unchanged. */
        if (advance <= 0) {
            if (output_position + 1u > output_capacity) {
                output_capacity *= 2u;
                uint8_t *resized = (uint8_t*)SDL_realloc(output_buffer,
                                                          output_capacity);
                if (!resized) { SDL_free(output_buffer); return NULL; }
                output_buffer = resized;
            }
            output_buffer[output_position++] = s[input_position++];
            continue;
        }
        input_position += (size_t)advance;

        /* Ensure 4 bytes are available for the encoded uppercased codepoint. */
        if (output_position + 4u > output_capacity) {
            output_capacity = output_capacity * 2u + 16u;
            uint8_t *resized = (uint8_t*)SDL_realloc(output_buffer,
                                                      output_capacity);
            if (!resized) { SDL_free(output_buffer); return NULL; }
            output_buffer = resized;
        }
        const utf8proc_ssize_t bytes_written = utf8proc_encode_char(
            utf8proc_toupper(codepoint), output_buffer + output_position);
        if (bytes_written > 0) output_position += (size_t)bytes_written;
    }

    b_str_t result = b_str_new_pro(output_buffer, output_position,
                                    B_STR_ENC_UTF8);
    SDL_free(output_buffer);
    return result;
}

/*──────────────────────────────────────────────────────────────────────────────
  COMPARISON & SEARCH
──────────────────────────────────────────────────────────────────────────────*/

int b_str_cmp(b_cstr_t a, b_cstr_t b) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return  1;
    const size_t len_a = b_str_len(a), len_b = b_str_len(b);
    const int    cmp_result = memcmp(a, b, len_a < len_b ? len_a : len_b);
    if (cmp_result) return cmp_result;
    return (len_a < len_b) ? -1 : (len_a > len_b) ? 1 : 0;
}

bool b_str_eq(b_cstr_t a, b_cstr_t b) {
    if (a == b) return true;
    if (!a || !b) return false;
    const size_t len_a = b_str_len(a);
    return len_a == b_str_len(b) && memcmp(a, b, len_a) == 0;
}

size_t b_str_find(b_cstr_t haystack, b_cstr_t needle) {
    if (!haystack || !needle) return SIZE_MAX;
    const size_t haystack_length = b_str_len(haystack);
    const size_t needle_length   = b_str_len(needle);
    if (!needle_length) return 0;
    if (needle_length > haystack_length) return SIZE_MAX;
    const size_t search_limit = haystack_length - needle_length;
    for (size_t byte_offset = 0; byte_offset <= search_limit; byte_offset++)
        if (memcmp(haystack + byte_offset, needle, needle_length) == 0)
            return byte_offset;
    return SIZE_MAX;
}

bool b_str_contains(b_cstr_t s, b_cstr_t needle) {
    return b_str_find(s, needle) != SIZE_MAX;
}

bool b_str_starts_with(b_cstr_t s, b_cstr_t prefix) {
    if (!s || !prefix) return false;
    const size_t prefix_length = b_str_len(prefix);
    return !prefix_length
           || (prefix_length <= b_str_len(s)
               && memcmp(s, prefix, prefix_length) == 0);
}

bool b_str_ends_with(b_cstr_t s, b_cstr_t suffix) {
    if (!s || !suffix) return false;
    const size_t string_length = b_str_len(s);
    const size_t suffix_length = b_str_len(suffix);
    return !suffix_length
           || (suffix_length <= string_length
               && memcmp(s + string_length - suffix_length, suffix,
                         suffix_length) == 0);
}

/*──────────────────────────────────────────────────────────────────────────────
  IN-PLACE MUTATION
──────────────────────────────────────────────────────────────────────────────*/

/*
 * _is_utf16_whitespace – true if the 2-byte sequence at buffer[byte_offset]
 * decodes (after optional byte-swap) to a basic ASCII whitespace codepoint.
 */
static inline bool _is_utf16_whitespace(const uint8_t *buffer,
                                         size_t byte_offset,
                                         bool is_big_endian) {
    uint16_t unit = _read_u16(buffer + byte_offset);
    if (is_big_endian) unit = _byte_swap_16(unit);
    return unit == 0x0020u   /* SPACE           */
        || unit == 0x0009u   /* HORIZONTAL TAB  */
        || unit == 0x000Au   /* LINE FEED       */
        || unit == 0x000Du;  /* CARRIAGE RETURN */
}

/*
 * _is_utf32_whitespace – true if the 4-byte sequence at buffer[byte_offset]
 * decodes to a basic ASCII whitespace codepoint.
 */
static inline bool _is_utf32_whitespace(const uint8_t *buffer,
                                         size_t byte_offset,
                                         bool is_big_endian) {
    uint32_t unit = _read_u32(buffer + byte_offset);
    if (is_big_endian) unit = _byte_swap_32(unit);
    return unit == 0x00000020u
        || unit == 0x00000009u
        || unit == 0x0000000Au
        || unit == 0x0000000Du;
}

b_str_t b_str_trim_r(b_str_t s) {
    if (!s) return NULL;
    const uint8_t encoding       = b_str_enc(s);
    const size_t  null_term_size = _null_term_size(encoding);
    size_t        byte_length    = b_str_len(s);

    if (B_STR_IS_UTF32_ENC(encoding)) {
        const bool is_big_endian = (encoding == B_STR_ENC_UTF32BE);
        /* Each code unit is 4 bytes; the boundary check `byte_length >= 4`
         * is equivalent to `byte_length - 4 < byte_length` for size_t.      */
        while (byte_length >= 4u
               && _is_utf32_whitespace(s, byte_length - 4u, is_big_endian))
            byte_length -= 4u;
    } else if (B_STR_IS_UTF16_ENC(encoding)) {
        const bool is_big_endian = (encoding == B_STR_ENC_UTF16BE);
        while (byte_length >= 2u
               && _is_utf16_whitespace(s, byte_length - 2u, is_big_endian))
            byte_length -= 2u;
    } else {
        /* ASCII or UTF-8: compare single bytes. */
        while (byte_length > 0u) {
            const uint8_t last_byte = s[byte_length - 1u];
            if (last_byte == ' ' || last_byte == '\t'
                    || last_byte == '\n' || last_byte == '\r')
                byte_length--;
            else
                break;
        }
    }
    b_str_set_len(s, byte_length);
    _write_null_terminator(s, byte_length, null_term_size);
    return s;
}

b_str_t b_str_trim_l(b_str_t s) {
    if (!s) return NULL;
    const uint8_t encoding       = b_str_enc(s);
    const size_t  null_term_size = _null_term_size(encoding);
    const size_t  byte_length    = b_str_len(s);
    size_t        trim_start     = 0;

    if (B_STR_IS_UTF32_ENC(encoding)) {
        const bool is_big_endian = (encoding == B_STR_ENC_UTF32BE);
        /* Condition: at least 4 bytes remain starting at trim_start. */
        while (trim_start + 4u <= byte_length
               && _is_utf32_whitespace(s, trim_start, is_big_endian))
            trim_start += 4u;
    } else if (B_STR_IS_UTF16_ENC(encoding)) {
        const bool is_big_endian = (encoding == B_STR_ENC_UTF16BE);
        while (trim_start + 2u <= byte_length
               && _is_utf16_whitespace(s, trim_start, is_big_endian))
            trim_start += 2u;
    } else {
        while (trim_start < byte_length) {
            const uint8_t first_byte = s[trim_start];
            if (first_byte == ' ' || first_byte == '\t'
                    || first_byte == '\n' || first_byte == '\r')
                trim_start++;
            else
                break;
        }
    }

    if (trim_start > 0u) {
        const size_t new_byte_length = byte_length - trim_start;
        memmove(s, s + trim_start, new_byte_length);
        b_str_set_len(s, new_byte_length);
        _write_null_terminator(s, new_byte_length, null_term_size);
    }
    return s;
}

b_str_t b_str_trim(b_str_t s) { return b_str_trim_r(b_str_trim_l(s)); }

b_str_t b_str_repeat(b_cstr_t s, size_t repeat_count) {
    const uint8_t encoding = s ? b_str_enc(s) : B_STR_ENC_ASCII;
    if (!s || !repeat_count) return b_str_new_pro(NULL, 0, encoding);
    const size_t unit_bytes = b_str_len(s);
    if (!unit_bytes) return b_str_new_pro(NULL, 0, encoding);
    if (unit_bytes > SIZE_MAX / repeat_count) return NULL;

    const size_t total_bytes = unit_bytes * repeat_count;
    b_str_t result = b_str_new_pro(NULL, total_bytes, encoding);
    if (!result) return NULL;

    for (size_t iteration = 0; iteration < repeat_count; iteration++)
        memcpy(result + iteration * unit_bytes, s, unit_bytes);

    /* Length and null terminator were set correctly by b_str_new_pro.
     * The memcpy loop writes exactly [0, total_bytes); the null terminator
     * at result[total_bytes] was placed by b_str_new_pro and is untouched. */
    return result;
}

/*──────────────────────────────────────────────────────────────────────────────
  VALIDATION
──────────────────────────────────────────────────────────────────────────────*/

bool b_str_valid_utf8(b_cstr_t s) {
    if (!s) return true;
    const size_t total_bytes = b_str_len(s);
    size_t byte_pos = 0;
    while (byte_pos < total_bytes) {
        utf8proc_int32_t codepoint;
        const size_t remaining = total_bytes - byte_pos;
        const utf8proc_ssize_t advance = utf8proc_iterate(
            s + byte_pos,
            (utf8proc_ssize_t)(remaining < (size_t)SSIZE_MAX
                               ? remaining : (size_t)SSIZE_MAX),
            &codepoint);
        if (advance <= 0) return false;
        byte_pos += (size_t)advance;
    }
    return true;
}

/*──────────────────────────────────────────────────────────────────────────────
  BOM
──────────────────────────────────────────────────────────────────────────────*/

uint8_t b_str_detect_bom(const void *data, size_t byte_len,
                          size_t *bom_size_out) {
    if (bom_size_out) *bom_size_out = 0;
    if (!data || byte_len < 2u) return B_STR_ENC_ASCII;
    const uint8_t *raw = (const uint8_t*)data;

    /* UTF-32 BE: 00 00 FE FF  – must be checked before UTF-16 BE */
    if (byte_len >= 4u
            && raw[0] == 0x00u && raw[1] == 0x00u
            && raw[2] == 0xFEu && raw[3] == 0xFFu) {
        if (bom_size_out) *bom_size_out = 4u;
        return B_STR_ENC_UTF32BE;
    }
    /* UTF-32 LE: FF FE 00 00  – must be checked before UTF-16 LE */
    if (byte_len >= 4u
            && raw[0] == 0xFFu && raw[1] == 0xFEu
            && raw[2] == 0x00u && raw[3] == 0x00u) {
        if (bom_size_out) *bom_size_out = 4u;
        return B_STR_ENC_UTF32LE;
    }
    /* UTF-8: EF BB BF */
    if (byte_len >= 3u
            && raw[0] == 0xEFu && raw[1] == 0xBBu && raw[2] == 0xBFu) {
        if (bom_size_out) *bom_size_out = 3u;
        return B_STR_ENC_UTF8;
    }
    /* UTF-16 LE: FF FE */
    if (raw[0] == 0xFFu && raw[1] == 0xFEu) {
        if (bom_size_out) *bom_size_out = 2u;
        return B_STR_ENC_UTF16LE;
    }
    /* UTF-16 BE: FE FF */
    if (raw[0] == 0xFEu && raw[1] == 0xFFu) {
        if (bom_size_out) *bom_size_out = 2u;
        return B_STR_ENC_UTF16BE;
    }

    return B_STR_ENC_ASCII;
}

/* Return the BOM byte array and its size for a given encoding tag. */
static const uint8_t *_bom_for_encoding(uint8_t encoding,
                                         size_t *bom_size_out) {
    switch (encoding & B_STR_ENC_MASK) {
    case B_STR_ENC_UTF8:    *bom_size_out = 3u; return BOM_UTF8;
    case B_STR_ENC_UTF16LE: *bom_size_out = 2u; return BOM_UTF16LE;
    case B_STR_ENC_UTF16BE: *bom_size_out = 2u; return BOM_UTF16BE;
    case B_STR_ENC_UTF32LE: *bom_size_out = 4u; return BOM_UTF32LE;
    case B_STR_ENC_UTF32BE: *bom_size_out = 4u; return BOM_UTF32BE;
    default:                *bom_size_out = 0u; return NULL;
    }
}

b_str_t b_str_add_bom(b_str_t s) {
    if (!s) return NULL;

    size_t         bom_size;
    const uint8_t *bom_bytes = _bom_for_encoding(b_str_enc(s), &bom_size);
    if (!bom_bytes || !bom_size) return s;  /* ASCII – no BOM defined */

    const size_t  current_length = b_str_len(s);
    const uint8_t encoding       = b_str_enc(s);
    const size_t  null_term_size = _null_term_size(encoding);

    s = b_str_ensure(s, bom_size);
    if (b_str_avail(s) < bom_size) return s;  /* OOM or static full */

    /* Shift existing content (including null terminator) right by bom_size. */
    memmove(s + bom_size, s, current_length + null_term_size);
    memcpy(s, bom_bytes, bom_size);
    b_str_set_len(s, current_length + bom_size);
    /* The null terminator bytes were moved by memmove; no rewrite needed. */
    return s;
}

/*──────────────────────────────────────────────────────────────────────────────
  FILE I/O
──────────────────────────────────────────────────────────────────────────────*/

/*
 * _read_entire_file – read an entire file into a freshly SDL_malloc'd buffer.
 * Appends 4 zero bytes at the end (safe null termination for any encoding).
 * The caller is responsible for calling SDL_free on the returned pointer.
 * Returns NULL on any error.
 */
static uint8_t *_read_entire_file(const char *path, size_t *file_size_out) {
    FILE *file_handle = fopen(path, "rb");
    if (!file_handle) return NULL;

    if (fseek(file_handle, 0, SEEK_END) != 0) {
        fclose(file_handle);
        return NULL;
    }
    const long file_size_signed = ftell(file_handle);
    if (file_size_signed < 0) { fclose(file_handle); return NULL; }
    rewind(file_handle);

    const size_t file_size = (size_t)file_size_signed;
    /* Allocate file_size bytes + 4 trailing zeros for null termination.
     * The +4 handles the widest null terminator (UTF-32 = 4 zero bytes).   */
    uint8_t *raw_buffer = (uint8_t*)SDL_malloc(file_size + 4u);
    if (!raw_buffer) { fclose(file_handle); return NULL; }
    memset(raw_buffer + file_size, 0, 4u);

    if (fread(raw_buffer, 1u, file_size, file_handle) != file_size) {
        fclose(file_handle);
        SDL_free(raw_buffer);
        return NULL;
    }
    fclose(file_handle);
    *file_size_out = file_size;
    return raw_buffer;
}

b_str_t b_str_load_file(const char *path, uint8_t fallback_encoding) {
    if (!path) return NULL;
    if (!fallback_encoding) fallback_encoding = B_STR_ENC_UTF8;

    size_t   file_size;
    uint8_t *raw_data = _read_entire_file(path, &file_size);
    if (!raw_data) return NULL;

    size_t        bom_size = 0;
    const uint8_t detected_encoding = b_str_detect_bom(raw_data, file_size,
                                                         &bom_size);
    /* Use the detected encoding if a BOM was found; otherwise use fallback. */
    const uint8_t effective_encoding = bom_size ? detected_encoding
                                                 : fallback_encoding;

    /* bom_size is always <= file_size (detect_bom guards all lengths). */
    b_str_t result = b_str_new_pro(raw_data + bom_size, file_size - bom_size,
                                    effective_encoding);
    SDL_free(raw_data);
    return result;
}

int b_str_save_file(const char *path, b_cstr_t s, bool write_bom) {
    if (!path || !s) return -1;

    FILE *file_handle = fopen(path, "wb");
    if (!file_handle) return -1;

    if (write_bom) {
        size_t         bom_size;
        const uint8_t *bom_bytes = _bom_for_encoding(b_str_enc(s), &bom_size);
        if (bom_bytes && bom_size
                && fwrite(bom_bytes, 1u, bom_size, file_handle) != bom_size) {
            fclose(file_handle);
            return -1;
        }
    }

    const size_t content_length = b_str_len(s);
    if (content_length
            && fwrite(s, 1u, content_length, file_handle) != content_length) {
        fclose(file_handle);
        return -1;
    }

    fclose(file_handle);
    return 0;
}

int b_file_add_bom(const char *path, uint8_t encoding) {
    size_t         bom_size;
    const uint8_t *bom_bytes = _bom_for_encoding(encoding, &bom_size);
    if (!bom_bytes || !bom_size) return 0;  /* ASCII – nothing to do */

    size_t   file_size;
    uint8_t *raw_data = _read_entire_file(path, &file_size);
    if (!raw_data) return -1;

    /* File already starts with this BOM: leave it untouched. */
    if (file_size >= bom_size && memcmp(raw_data, bom_bytes, bom_size) == 0) {
        SDL_free(raw_data);
        return 0;
    }

    FILE *file_handle = fopen(path, "wb");
    if (!file_handle) { SDL_free(raw_data); return -1; }

    int result = 0;
    if (fwrite(bom_bytes, 1u, bom_size, file_handle) != bom_size)
        result = -1;
    if (result == 0 && file_size
            && fwrite(raw_data, 1u, file_size, file_handle) != file_size)
        result = -1;

    fclose(file_handle);
    SDL_free(raw_data);
    return result;
}

/*──────────────────────────────────────────────────────────────────────────────
  INTERNAL GENERIC FILE CONVERTER
  Not exported; callers use the named wrappers below.
──────────────────────────────────────────────────────────────────────────────*/

static int _b_file_convert(const char *in_path, const char *out_path,
                            uint8_t input_fallback_encoding,
                            uint8_t output_encoding,
                            bool write_bom) {
    const uint8_t effective_fallback = input_fallback_encoding
                                       ? input_fallback_encoding
                                       : B_STR_ENC_UTF8;
    b_str_t source = b_str_load_file(in_path, effective_fallback);
    if (!source) return -1;

    const uint8_t source_encoding = b_str_enc(source);
    b_str_t destination = NULL;

    if (source_encoding == output_encoding) {
        /* Same encoding: just copy. */
        destination = b_str_dup(source);
    } else if (!B_STR_IS_UTF16_ENC(source_encoding)
               && !B_STR_IS_UTF32_ENC(source_encoding)
               && !B_STR_IS_UTF16_ENC(output_encoding)
               && !B_STR_IS_UTF32_ENC(output_encoding)) {
        /* Both sides are byte encodings (ASCII / UTF-8): dup and re-tag. */
        destination = b_str_dup(source);
        if (destination) b_str_set_enc(destination, output_encoding);
    } else {
        /* General path: route through UTF-8 as the common intermediate. */
        b_str_t utf8_intermediate = NULL;
        if (B_STR_IS_UTF16_ENC(source_encoding)
                || B_STR_IS_UTF32_ENC(source_encoding)) {
            utf8_intermediate = b_str_to_utf8(source);
        } else {
            utf8_intermediate = b_str_dup(source);
            if (utf8_intermediate)
                b_str_set_enc(utf8_intermediate, B_STR_ENC_UTF8);
        }
        if (!utf8_intermediate) { b_str_free(source); return -1; }

        if (!B_STR_IS_UTF16_ENC(output_encoding)
                && !B_STR_IS_UTF32_ENC(output_encoding)) {
            /* Output is a byte encoding: use the UTF-8 intermediate directly. */
            destination = utf8_intermediate;
            utf8_intermediate = NULL;
            b_str_set_enc(destination, output_encoding);
        } else if (output_encoding == B_STR_ENC_UTF16LE) {
            destination = b_str_to_utf16(utf8_intermediate);
        } else if (output_encoding == B_STR_ENC_UTF16BE) {
            destination = b_str_to_utf16be(utf8_intermediate);
        } else if (output_encoding == B_STR_ENC_UTF32LE) {
            destination = b_str_to_utf32le(utf8_intermediate);
        } else if (output_encoding == B_STR_ENC_UTF32BE) {
            destination = b_str_to_utf32be(utf8_intermediate);
        }
        b_str_free(utf8_intermediate);
    }

    b_str_free(source);
    if (!destination) return -1;

    b_str_set_enc(destination, output_encoding);
    const int conversion_result = b_str_save_file(out_path, destination,
                                                    write_bom);
    b_str_free(destination);
    return conversion_result;
}

/*──────────────────────────────────────────────────────────────────────────────
  FILE CONVERSION WRAPPERS
──────────────────────────────────────────────────────────────────────────────*/

/* ── ASCII -> UTF-8 ─────────────────────────────────────────────────────── */
int b_file_conv_ascii_to_utf8_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_ASCII, B_STR_ENC_UTF8, true);
}
int b_file_conv_ascii_to_utf8_no_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_ASCII, B_STR_ENC_UTF8, false);
}

/* ── UTF-8 -> UTF-16 ────────────────────────────────────────────────────── */
int b_file_conv_utf8_to_utf16(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF8, _system_utf16_encoding(), true);
}
int b_file_conv_utf8_to_utf16le_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF8, B_STR_ENC_UTF16LE, true);
}
int b_file_conv_utf8_to_utf16le_no_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF8, B_STR_ENC_UTF16LE, false);
}
int b_file_conv_utf8_to_utf16be_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF8, B_STR_ENC_UTF16BE, true);
}
int b_file_conv_utf8_to_utf16be_no_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF8, B_STR_ENC_UTF16BE, false);
}

/* ── UTF-8 -> UTF-32 ────────────────────────────────────────────────────── */
int b_file_conv_utf8_to_utf32(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF8, _system_utf32_encoding(), true);
}
int b_file_conv_utf8_to_utf32le_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF8, B_STR_ENC_UTF32LE, true);
}
int b_file_conv_utf8_to_utf32le_no_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF8, B_STR_ENC_UTF32LE, false);
}
int b_file_conv_utf8_to_utf32be_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF8, B_STR_ENC_UTF32BE, true);
}
int b_file_conv_utf8_to_utf32be_no_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF8, B_STR_ENC_UTF32BE, false);
}

/* ── UTF-16 -> UTF-8 ────────────────────────────────────────────────────── */
int b_file_conv_utf16_to_utf8_no_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF16LE, B_STR_ENC_UTF8, false);
}
int b_file_conv_utf16le_bom_to_utf8_no_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF16LE, B_STR_ENC_UTF8, false);
}
int b_file_conv_utf16le_no_bom_to_utf8_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF16LE, B_STR_ENC_UTF8, true);
}
int b_file_conv_utf16le_no_bom_to_utf8_no_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF16LE, B_STR_ENC_UTF8, false);
}
int b_file_conv_utf16be_bom_to_utf8_no_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF16BE, B_STR_ENC_UTF8, false);
}
int b_file_conv_utf16be_no_bom_to_utf8_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF16BE, B_STR_ENC_UTF8, true);
}
int b_file_conv_utf16be_no_bom_to_utf8_no_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF16BE, B_STR_ENC_UTF8, false);
}

/* ── UTF-32 -> UTF-8 ────────────────────────────────────────────────────── */
int b_file_conv_utf32_to_utf8_no_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF32LE, B_STR_ENC_UTF8, false);
}
int b_file_conv_utf32le_bom_to_utf8_no_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF32LE, B_STR_ENC_UTF8, false);
}
int b_file_conv_utf32le_no_bom_to_utf8_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF32LE, B_STR_ENC_UTF8, true);
}
int b_file_conv_utf32le_no_bom_to_utf8_no_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF32LE, B_STR_ENC_UTF8, false);
}
int b_file_conv_utf32be_bom_to_utf8_no_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF32BE, B_STR_ENC_UTF8, false);
}
int b_file_conv_utf32be_no_bom_to_utf8_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF32BE, B_STR_ENC_UTF8, true);
}
int b_file_conv_utf32be_no_bom_to_utf8_no_bom(const char *in_path, const char *out_path) {
    return _b_file_convert(in_path, out_path, B_STR_ENC_UTF32BE, B_STR_ENC_UTF8, false);
}

/*──────────────────────────────────────────────────────────────────────────────
  UTF-16 STDOUT HELPER
──────────────────────────────────────────────────────────────────────────────*/

void b_str_print_utf16(b_u16slice_t slice) {
    if (!slice.data || !slice.len) return;
    b_str_t temp_utf16 = b_str_from_u16slice(slice);
    if (!temp_utf16) return;
    b_str_t temp_utf8 = b_str_to_utf8(temp_utf16);
    b_str_free(temp_utf16);
    if (temp_utf8) {
        fwrite(temp_utf8, 1u, b_str_len(temp_utf8), stdout);
        b_str_free(temp_utf8);
    }
}
