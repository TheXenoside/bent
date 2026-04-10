#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "bent.h"

#ifndef SSIZE_MAX
#  define SSIZE_MAX ((size_t)(~(size_t)0) >> 1)
#endif

#ifdef b_ALIGNED
#  define B_STR_ALIGN_FLAG B_STRING_ALIGNED
#else
#  define B_STR_ALIGN_FLAG B_STRING_PACKED
#endif

// BOM byte sequences
static const uint8_t BOM_UTF8[3]    = {0xEFu, 0xBBu, 0xBFu};
static const uint8_t BOM_UTF16LE[2] = {0xFFu, 0xFEu};
static const uint8_t BOM_UTF16BE[2] = {0xFEu, 0xFFu};
static const uint8_t BOM_UTF32LE[4] = {0xFFu, 0xFEu, 0x00u, 0x00u};
static const uint8_t BOM_UTF32BE[4] = {0x00u, 0x00u, 0xFEu, 0xFFu};

/*──────────────────────────────────────────────────────────────────────────────
  INTERNAL HELPERS
──────────────────────────────────────────────────────────────────────────────*/

// null terminator width: 4 for UTF-32, 2 for UTF-16, 1 for ASCII/UTF-8
static inline size_t _null_term_size(const uint8_t encoding) {
    if (B_STR_IS_UTF32_ENC(encoding)) return 4u;
    if (B_STR_IS_UTF16_ENC(encoding)) return 2u;
    return 1u;
}

// round byte_length DOWN to code-unit boundary
static inline size_t _align_len_down(const size_t byte_length, const uint8_t encoding) {
    if (B_STR_IS_UTF32_ENC(encoding)) return byte_length & ~(size_t)3u;
    if (B_STR_IS_UTF16_ENC(encoding)) return byte_length & ~(size_t)1u;
    return byte_length;
}

// round capacity UP to next code-unit boundary (caller guards overflow)
static inline size_t _align_cap_up(const size_t capacity, const uint8_t encoding) {
    if (B_STR_IS_UTF32_ENC(encoding)) return (capacity + 3u) & ~(size_t)3u;
    if (B_STR_IS_UTF16_ENC(encoding)) return (capacity + 1u) & ~(size_t)1u;
    return capacity;
}

// write null_term_size zero bytes at buffer[position]
static inline void _write_null_terminator(uint8_t * const buffer,
                                          const size_t position,
                                          const size_t null_term_size) {
    for (size_t i = 0; i < null_term_size; i++) buffer[position + i] = '\0';
}

// unaligned 16/32-bit reads via memcpy (avoids strict-aliasing UB)
static inline uint16_t _read_u16(const void * const raw) {
    uint16_t v; memcpy(&v, raw, 2); return v;
}
static inline uint32_t _read_u32(const void * const raw) {
    uint32_t v; memcpy(&v, raw, 4); return v;
}

static inline uint16_t _byte_swap_16(const uint16_t v) {
    return (uint16_t)(((v & 0x00FFu) << 8) | ((v & 0xFF00u) >> 8));
}
static inline uint32_t _byte_swap_32(const uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) <<  8) |
           ((v & 0x00FF0000u) >>  8) | ((v & 0xFF000000u) >> 24);
}

static inline uint8_t _system_utf16_encoding(void) {
#if defined(SDL_BYTEORDER) && SDL_BYTEORDER == SDL_BIG_ENDIAN
    return B_STR_ENC_UTF16BE;
#else
    return B_STR_ENC_UTF16LE;
#endif
}
static inline uint8_t _system_utf32_encoding(void) {
#if defined(SDL_BYTEORDER) && SDL_BYTEORDER == SDL_BIG_ENDIAN
    return B_STR_ENC_UTF32BE;
#else
    return B_STR_ENC_UTF32LE;
#endif
}

/*──────────────────────────────────────────────────────────────────────────────
  STATIC WRITE-BACK HELPER

  _b_apply_or_keep(original, converted, new_enc):
    - If converted == NULL (conversion failed): return original unchanged.
    - If original == converted (same-enc no-op): return original.
    - If original is STATIC: attempt to copy converted data back into original's
      buffer. If it fits the pointer is stable; if not, fail silently. Either
      way free(converted) and return original.
    - If original is DYNAMIC: return converted (caller owns both; must free old).
──────────────────────────────────────────────────────────────────────────────*/
static b_str_t _b_apply_or_keep(b_str_t original, b_str_t converted,
                                 const uint8_t new_enc) {
    if (!converted || converted == original) return original; // failed or no-op

    if (!(original[-1] & B_STR_STATIC)) return converted; // dynamic: new pointer

    // Static: try to write result back so the original pointer stays valid
    const size_t result_len    = b_str_len(converted);
    const size_t new_null_sz   = _null_term_size(new_enc);
    const size_t old_null_sz   = _null_term_size((uint8_t)(original[-1] & B_STR_ENC_MASK));
    const size_t total_buf     = b_str_cap(original) + old_null_sz; // total data bytes allocated

    if (result_len + new_null_sz <= total_buf) {
        // Fits: copy data + null terminator, update header fields
        memcpy(original, converted, result_len + new_null_sz);
        b_str_set_enc(original, new_enc);
        // Update capacity to be aligned to new encoding; keep available spare bytes
        const size_t new_cap = _align_len_down(total_buf - new_null_sz, new_enc);
        b_str_set_lens(original, result_len, new_cap);
    }
    // If it doesn't fit: fail silently, original is unchanged
    b_str_free(converted);
    return original;
}

/*──────────────────────────────────────────────────────────────────────────────
  CORE ACCESSORS
──────────────────────────────────────────────────────────────────────────────*/

size_t b_str_hdr_size(const uint8_t flags) {
    switch (flags & B_STR_TYPE_MASK) {
    case B_STR_TYPE_8:  return sizeof(b_hdr8_t);
    case B_STR_TYPE_16: return sizeof(b_hdr16_t);
    case B_STR_TYPE_32: return sizeof(b_hdr32_t);
    case B_STR_TYPE_64: return sizeof(b_hdr64_t);
    default:            return 0;
    }
}

uint8_t b_str_pick_type(const size_t byte_size) {
    if (byte_size < 256u)                return B_STR_TYPE_8;
    if (byte_size < 65536u)              return B_STR_TYPE_16;
    if (byte_size <= (size_t)UINT32_MAX) return B_STR_TYPE_32;
    return B_STR_TYPE_64;
}

uint8_t b_str_enc(const b_cstr_t s) {
    return s ? (uint8_t)(s[-1] & B_STR_ENC_MASK) : B_STR_ENC_ASCII;
}

void b_str_set_enc(b_str_t s, const uint8_t encoding) {
    if (s) s[-1] = (uint8_t)((s[-1] & ~B_STR_ENC_MASK) | (encoding & B_STR_ENC_MASK));
}

size_t b_str_len(const b_cstr_t s) {
    if (!s) return 0;
    switch (s[-1] & B_STR_TYPE_MASK) {
    case B_STR_TYPE_8:  return B_CHDR8(s)->length;
    case B_STR_TYPE_16: return B_CHDR16(s)->length;
    case B_STR_TYPE_32: return B_CHDR32(s)->length;
    case B_STR_TYPE_64: return B_CHDR64(s)->length;
    default:            return 0;
    }
}

size_t b_str_avail(const b_cstr_t s) {
    if (!s) return 0;
    switch (s[-1] & B_STR_TYPE_MASK) {
    case B_STR_TYPE_8:  { const b_hdr8_t  *h = B_CHDR8(s);  return h->capacity > h->length ? (size_t)(h->capacity - h->length) : 0u; }
    case B_STR_TYPE_16: { const b_hdr16_t *h = B_CHDR16(s); return h->capacity > h->length ? (size_t)(h->capacity - h->length) : 0u; }
    case B_STR_TYPE_32: { const b_hdr32_t *h = B_CHDR32(s); return h->capacity > h->length ? (size_t)(h->capacity - h->length) : 0u; }
    case B_STR_TYPE_64: { const b_hdr64_t *h = B_CHDR64(s); return h->capacity > h->length ? (size_t)(h->capacity - h->length) : 0u; }
    default:            return 0;
    }
}

size_t b_str_cap(const b_cstr_t s) { return s ? b_str_len(s) + b_str_avail(s) : 0u; }

void b_str_set_lens(b_str_t s, size_t used_bytes, size_t cap_bytes) {
    if (!s) return;
    const uint8_t enc = (uint8_t)(s[-1] & B_STR_ENC_MASK);
    used_bytes = _align_len_down(used_bytes, enc);
    cap_bytes  = _align_len_down(cap_bytes,  enc);
    switch (s[-1] & B_STR_TYPE_MASK) {
    case B_STR_TYPE_8:  B_HDR8(s)->capacity  = (uint8_t) cap_bytes;  B_HDR8(s)->length  = (uint8_t) used_bytes; break;
    case B_STR_TYPE_16: B_HDR16(s)->capacity = (uint16_t)cap_bytes;  B_HDR16(s)->length = (uint16_t)used_bytes; break;
    case B_STR_TYPE_32: B_HDR32(s)->capacity = (uint32_t)cap_bytes;  B_HDR32(s)->length = (uint32_t)used_bytes; break;
    case B_STR_TYPE_64: B_HDR64(s)->capacity = (uint64_t)cap_bytes;  B_HDR64(s)->length = (uint64_t)used_bytes; break;
    }
}

void b_str_set_len(b_str_t s, size_t used_bytes) {
    if (!s) return;
    used_bytes = _align_len_down(used_bytes, (uint8_t)(s[-1] & B_STR_ENC_MASK));
    switch (s[-1] & B_STR_TYPE_MASK) {
    case B_STR_TYPE_8:  B_HDR8(s)->length  = (uint8_t) used_bytes; break;
    case B_STR_TYPE_16: B_HDR16(s)->length = (uint16_t)used_bytes; break;
    case B_STR_TYPE_32: B_HDR32(s)->length = (uint32_t)used_bytes; break;
    case B_STR_TYPE_64: B_HDR64(s)->length = (uint64_t)used_bytes; break;
    }
}

size_t b_str_cpcount(const b_cstr_t s) {
    if (!s) return 0;
    const size_t  total = b_str_len(s);
    const uint8_t enc   = b_str_enc(s);
    if (!total) return 0;

    if (enc == B_STR_ENC_UTF8) {
        size_t count = 0, pos = 0;
        while (pos < total) {
            utf8proc_int32_t cp;
            const size_t rem = total - pos;
            const utf8proc_ssize_t adv = utf8proc_iterate(
                s + pos,
                (utf8proc_ssize_t)(rem < (size_t)SSIZE_MAX ? rem : (size_t)SSIZE_MAX),
                &cp);
            pos += (adv > 0) ? (size_t)adv : 1u;
            count++;
        }
        return count;
    }
    if (B_STR_IS_UTF16_ENC(enc)) {
        const bool   be    = (enc == B_STR_ENC_UTF16BE);
        const size_t units = total / 2u;
        size_t count = 0, ui = 0;
        while (ui < units) {
            uint16_t u1 = _read_u16(s + ui * 2u);
            if (be) u1 = _byte_swap_16(u1);
            ui++;
            // high surrogate → consume the low surrogate too
            if (u1 >= 0xD800u && u1 <= 0xDBFFu && ui < units) {
                uint16_t u2 = _read_u16(s + ui * 2u);
                if (be) u2 = _byte_swap_16(u2);
                if (u2 >= 0xDC00u && u2 <= 0xDFFFu) ui++;
            }
            count++;
        }
        return count;
    }
    if (B_STR_IS_UTF32_ENC(enc)) return total / 4u; // one unit = one codepoint
    return total; // ASCII: one byte = one codepoint
}

/*──────────────────────────────────────────────────────────────────────────────
  LIFECYCLE
──────────────────────────────────────────────────────────────────────────────*/

b_str_t b_str_new_pro(const void * const data, size_t byte_len,
                      const uint8_t encoding) {
    byte_len = _align_len_down(byte_len, encoding);

    const uint8_t type          = b_str_pick_type(byte_len);
    const size_t  hdr_sz        = b_str_hdr_size(type);
    const size_t  null_sz       = _null_term_size(encoding);

    if (byte_len > SIZE_MAX - hdr_sz - null_sz) return NULL;

    uint8_t * const mem = (uint8_t*)SDL_malloc(hdr_sz + byte_len + null_sz);
    if (!mem) return NULL;

    b_str_t s = mem + hdr_sz;
    s[-1] = (uint8_t)(type | B_STR_ALIGN_FLAG | (encoding & B_STR_ENC_MASK));

    if      (data)         memcpy(s, data, byte_len);
    else if (byte_len > 0) memset(s, 0,    byte_len);

    b_str_set_lens(s, byte_len, byte_len);
    _write_null_terminator(s, byte_len, null_sz);
    return s;
}

b_str_t b_str_new(const char * const cstr) {
    return b_str_new_pro(cstr, cstr ? strlen(cstr) : 0, B_STR_ENC_ASCII);
}

b_str_t b_str_new_static_pro(const void * const data, size_t byte_len,
                              size_t total_cap, const uint8_t encoding) {
    byte_len  = _align_len_down(byte_len,  encoding);
    total_cap = _align_len_down(total_cap, encoding);
    if (byte_len > total_cap) return NULL;

    const uint8_t type    = b_str_pick_type(total_cap);
    const size_t  hdr_sz  = b_str_hdr_size(type);
    const size_t  null_sz = _null_term_size(encoding);

    if (total_cap > SIZE_MAX - hdr_sz - null_sz) return NULL;

    uint8_t * const mem = (uint8_t*)SDL_malloc(hdr_sz + total_cap + null_sz);
    if (!mem) return NULL;

    b_str_t s = mem + hdr_sz;
    s[-1] = (uint8_t)(type | B_STR_ALIGN_FLAG | B_STR_STATIC | (encoding & B_STR_ENC_MASK));

    if      (data)         memcpy(s, data, byte_len);
    else if (byte_len > 0) memset(s, 0,    byte_len);

    b_str_set_lens(s, byte_len, total_cap);
    _write_null_terminator(s, byte_len, null_sz);
    return s;
}

b_str_t b_str_new_static(const char * const cstr, const size_t extra_bytes) {
    const size_t byte_len = cstr ? strlen(cstr) : 0;
    if (byte_len > SIZE_MAX - extra_bytes) return NULL;
    return b_str_new_static_pro(cstr, byte_len, byte_len + extra_bytes, B_STR_ENC_ASCII);
}

void b_str_free(b_str_t s) {
    if (s) SDL_free((uint8_t*)s - b_str_hdr_size(s[-1]));
}

b_str_t b_str_dup(const b_cstr_t s) {
    return s ? b_str_new_pro(s, b_str_len(s), b_str_enc(s)) : NULL;
}

void b_str_clear(b_str_t s) {
    if (!s) return;
    const size_t null_sz = _null_term_size(b_str_enc(s));
    b_str_set_len(s, 0);
    _write_null_terminator(s, 0, null_sz);
}

bool b_str_empty(const b_cstr_t s) { return !s || b_str_len(s) == 0; }

b_str_t b_str_to_dyn(b_str_t s) {
    if (!s || !(s[-1] & B_STR_STATIC)) return s;
    b_str_t dyn = b_str_new_pro(s, b_str_len(s), b_str_enc(s));
    if (!dyn) return s; // OOM: return original, pointer still valid
    b_str_free(s);
    return dyn;
}

/*──────────────────────────────────────────────────────────────────────────────
  SLICE CONSTRUCTORS
──────────────────────────────────────────────────────────────────────────────*/

b_str_t b_str_from_slice   (const b_slice_t sl)    { return b_str_new_pro(sl.data, sl.len, B_STR_ENC_ASCII); }
b_str_t b_str_from_u8slice (const b_u8slice_t sl)  { return b_str_new_pro(sl.data, sl.len, B_STR_ENC_UTF8); }
b_str_t b_str_from_u16slice(const b_u16slice_t sl) { return b_str_new_pro(sl.data, sl.len & ~(size_t)1u, B_STR_ENC_UTF16LE); }
b_str_t b_str_from_u32slice(const b_u32slice_t sl) { return b_str_new_pro(sl.data, sl.len & ~(size_t)3u, B_STR_ENC_UTF32LE); }

b_str_t b_str_from_u16(const uint16_t * const units, const size_t unit_count) {
    if (!units && unit_count > 0) return NULL;
    if (unit_count > SIZE_MAX / 2u) return NULL;
    return b_str_new_pro(units, unit_count * 2u, B_STR_ENC_UTF16LE);
}
b_str_t b_str_from_u32(const uint32_t * const units, const size_t unit_count) {
    if (!units && unit_count > 0) return NULL;
    if (unit_count > SIZE_MAX / 4u) return NULL;
    return b_str_new_pro(units, unit_count * 4u, B_STR_ENC_UTF32LE);
}

/*──────────────────────────────────────────────────────────────────────────────
  CAPACITY MANAGEMENT
──────────────────────────────────────────────────────────────────────────────*/

b_str_t b_str_ensure(b_str_t s, const size_t extra_bytes) {
    if (!s)                            return NULL;
    if (b_str_avail(s) >= extra_bytes) return s;
    if (s[-1] & B_STR_STATIC)         return s; // static: never reallocate

    const uint8_t enc       = b_str_enc(s);
    const size_t  null_sz   = _null_term_size(enc);
    const size_t  cur_len   = b_str_len(s);
    const size_t  cur_cap   = b_str_cap(s);

    if (extra_bytes > SIZE_MAX - cur_len) return s;
    size_t needed_cap = _align_cap_up(cur_len + extra_bytes, enc);

    // Growth strategy: double below 1 MB, add 1 MB chunks above
    size_t new_cap = needed_cap;
    if (new_cap < B_STR_ONE_MEG) {
        const size_t doubled = (cur_cap > SIZE_MAX / 2u) ? SIZE_MAX : cur_cap * 2u;
        if (doubled > new_cap) new_cap = doubled;
    } else {
        new_cap = (SIZE_MAX - B_STR_ONE_MEG >= new_cap) ? new_cap + B_STR_ONE_MEG : SIZE_MAX;
    }
    new_cap = _align_cap_up(new_cap, enc);

    uint8_t new_type    = b_str_pick_type(new_cap);
    size_t  new_hdr_sz  = b_str_hdr_size(new_type);

    // Clamp to avoid size_t overflow
    if (new_cap > SIZE_MAX - new_hdr_sz - null_sz) {
        new_cap     = _align_len_down(SIZE_MAX - new_hdr_sz - null_sz, enc);
        new_type    = b_str_pick_type(new_cap);
        new_hdr_sz  = b_str_hdr_size(new_type);
        if (new_cap < needed_cap) return s;
    }

    const uint8_t cur_type    = (uint8_t)(s[-1] & B_STR_TYPE_MASK);
    const size_t  cur_hdr_sz  = b_str_hdr_size(cur_type);
    uint8_t *mem;

    if (cur_type == new_type) {
        // Same header size: realloc in place
        mem = (uint8_t*)SDL_realloc(s - cur_hdr_sz,
                                    new_hdr_sz + new_cap + null_sz);
        if (!mem) return s;
        s = mem + new_hdr_sz;
    } else {
        // Header size changing: alloc new, copy, free old
        mem = (uint8_t*)SDL_malloc(new_hdr_sz + new_cap + null_sz);
        if (!mem) return s;
        memcpy(mem + new_hdr_sz, s, cur_len + null_sz);
        SDL_free(s - cur_hdr_sz);
        s = mem + new_hdr_sz;
        s[-1] = (uint8_t)(new_type | B_STR_ALIGN_FLAG | enc);
    }

    b_str_set_lens(s, cur_len, new_cap);
    return s;
}

b_str_t b_str_reserve(b_str_t s, const size_t extra_bytes) {
    return b_str_ensure(s, extra_bytes);
}

b_str_t b_str_fit(b_str_t s) {
    if (!s || (s[-1] & B_STR_STATIC)) return s;

    const uint8_t enc         = b_str_enc(s);
    const size_t  used        = b_str_len(s);
    const uint8_t cur_type    = (uint8_t)(s[-1] & B_STR_TYPE_MASK);
    const size_t  cur_hdr_sz  = b_str_hdr_size(cur_type);
    const size_t  null_sz     = _null_term_size(enc);
    const uint8_t tight_type  = b_str_pick_type(used);
    const size_t  tight_hdr_sz = b_str_hdr_size(tight_type);

    if (cur_type == tight_type) {
        uint8_t * const mem = (uint8_t*)SDL_realloc(s - cur_hdr_sz,
                                                     cur_hdr_sz + used + null_sz);
        if (!mem) return s;
        s = mem + cur_hdr_sz;
    } else {
        uint8_t * const mem = (uint8_t*)SDL_malloc(tight_hdr_sz + used + null_sz);
        if (!mem) return s;
        memcpy(mem + tight_hdr_sz, s, used + null_sz);
        SDL_free(s - cur_hdr_sz);
        s = mem + tight_hdr_sz;
        s[-1] = (uint8_t)(tight_type | B_STR_ALIGN_FLAG | enc);
    }

    b_str_set_lens(s, used, used);
    _write_null_terminator(s, used, null_sz);
    return s;
}

void b_str_arr_fit(b_str_t * const array, const size_t count) {
    if (!array) return;
    for (size_t i = 0; i < count; i++)
        if (array[i]) array[i] = b_str_fit(array[i]);
}

/*──────────────────────────────────────────────────────────────────────────────
  APPENDING & CONCATENATION  (all return s; never NULL when s != NULL)
──────────────────────────────────────────────────────────────────────────────*/

b_str_t b_str_append_pro(b_str_t s, const void * const data, size_t byte_len) {
    if (!s || !data || !byte_len) return s;

    const uint8_t enc       = b_str_enc(s);
    const size_t  null_sz   = _null_term_size(enc);
    const size_t  cur_len   = b_str_len(s);

    byte_len = _align_len_down(byte_len, enc);
    if (!byte_len) return s;
    if (byte_len > SIZE_MAX - cur_len) return s;

    // Detect self-overlap before potential realloc
    const uintptr_t src_addr = (uintptr_t)data;
    const uintptr_t buf_addr = (uintptr_t)s;
    const bool is_overlap = (src_addr >= buf_addr
                              && src_addr < (uintptr_t)(s + cur_len + null_sz));
    const ptrdiff_t ov_off = is_overlap
        ? (ptrdiff_t)((const uint8_t*)data - s) : 0;

    s = b_str_ensure(s, byte_len);
    if (b_str_avail(s) < byte_len) return s; // OOM or static full

    memmove(s + cur_len, is_overlap ? (const void*)(s + ov_off) : data, byte_len);

    const size_t new_len = cur_len + byte_len;
    b_str_set_len(s, new_len);
    _write_null_terminator(s, new_len, null_sz);
    return s;
}

b_str_t b_str_append(b_str_t s, const char * const cstr) {
    return b_str_append_pro(s, cstr, cstr ? strlen(cstr) : 0);
}
b_str_t b_str_append_sl(b_str_t s, const b_slice_t slice) {
    return slice.data ? b_str_append_pro(s, slice.data, slice.len) : s;
}
b_str_t b_str_append_u8(b_str_t s, const b_u8slice_t slice) {
    if (!slice.data || !slice.len) return s;
    if (b_str_enc(s) == B_STR_ENC_ASCII) b_str_set_enc(s, B_STR_ENC_UTF8);
    return b_str_append_pro(s, slice.data, slice.len);
}
b_str_t b_str_append_u16(b_str_t s, const b_u16slice_t slice) {
    const size_t al = slice.len & ~(size_t)1u;
    return (slice.data && al) ? b_str_append_pro(s, slice.data, al) : s;
}
b_str_t b_str_append_u32(b_str_t s, const b_u32slice_t slice) {
    const size_t al = slice.len & ~(size_t)3u;
    return (slice.data && al) ? b_str_append_pro(s, slice.data, al) : s;
}

b_str_t b_str_concat(const b_cstr_t a, const b_cstr_t b) {
    const uint8_t enc   = a ? b_str_enc(a) : B_STR_ENC_ASCII;
    const size_t  len_a = a ? b_str_len(a) : 0u;
    const size_t  len_b = b ? b_str_len(b) : 0u;
    if (len_a > SIZE_MAX - len_b) return NULL;

    b_str_t res = b_str_new_pro(a, len_a, enc);
    if (!res) return NULL;
    if (len_b) {
        const size_t prev = b_str_len(res);
        res = b_str_append_pro(res, b, len_b);
        if (b_str_len(res) == prev) { b_str_free(res); return NULL; }
    }
    return res;
}

/*──────────────────────────────────────────────────────────────────────────────
  SLICE EXTRACTORS
──────────────────────────────────────────────────────────────────────────────*/

b_slice_t b_slice_of(const b_cstr_t s) {
    b_slice_t r; r.data = s; r.len = b_str_len(s); return r;
}

b_slice_t b_subslice(const b_cstr_t s, const size_t byte_off,
                     const size_t byte_len) {
    b_slice_t empty; empty.data = NULL; empty.len = 0;
    if (!s) return empty;
    const size_t total = b_str_len(s);
    if (byte_off >= total) return empty;
    const size_t rem = total - byte_off;
    b_slice_t r; r.data = s + byte_off; r.len = byte_len > rem ? rem : byte_len; return r;
}

b_u8slice_t b_u8slice_of(const b_cstr_t s) {
    b_u8slice_t r; r.data = s; r.len = b_str_len(s); return r;
}

b_u8slice_t b_u8subslice(const b_cstr_t s, const size_t byte_off,
                          const size_t byte_len) {
    b_u8slice_t empty; empty.data = NULL; empty.len = 0;
    if (!s) return empty;
    const size_t total = b_str_len(s);
    if (byte_off >= total) return empty;
    const size_t rem = total - byte_off;
    b_u8slice_t r; r.data = s + byte_off; r.len = byte_len > rem ? rem : byte_len; return r;
}

b_u8slice_t b_u8subslice_cp(const b_cstr_t s, const size_t cp_off,
                              const size_t cp_count) {
    b_u8slice_t empty; empty.data = NULL; empty.len = 0;
    if (!s) return empty;
    const size_t total = b_str_len(s);
    size_t pos = 0;

    for (size_t i = 0; i < cp_off && pos < total; i++) {
        utf8proc_int32_t cp;
        const size_t rem = total - pos;
        const utf8proc_ssize_t adv = utf8proc_iterate(
            s + pos,
            (utf8proc_ssize_t)(rem < (size_t)SSIZE_MAX ? rem : (size_t)SSIZE_MAX), &cp);
        pos += (adv > 0) ? (size_t)adv : 1u;
    }
    if (pos >= total && cp_off > 0) return empty;
    const size_t start = pos;

    for (size_t i = 0; i < cp_count && pos < total; i++) {
        utf8proc_int32_t cp;
        const size_t rem = total - pos;
        const utf8proc_ssize_t adv = utf8proc_iterate(
            s + pos,
            (utf8proc_ssize_t)(rem < (size_t)SSIZE_MAX ? rem : (size_t)SSIZE_MAX), &cp);
        pos += (adv > 0) ? (size_t)adv : 1u;
    }
    b_u8slice_t r; r.data = s + start; r.len = pos - start; return r;
}

b_u16slice_t b_u16slice_of(const b_cstr_t s) {
    b_u16slice_t r; r.data = s; r.len = b_str_len(s); return r;
}

b_u16slice_t b_u16subslice(const b_cstr_t s, size_t byte_off,
                             size_t byte_len) {
    b_u16slice_t empty; empty.data = NULL; empty.len = 0;
    if (!s) return empty;
    byte_off &= ~(size_t)1u; byte_len &= ~(size_t)1u;
    const size_t total = b_str_len(s) & ~(size_t)1u;
    if (byte_off >= total) return empty;
    const size_t rem = total - byte_off;
    b_u16slice_t r; r.data = s + byte_off; r.len = byte_len > rem ? rem : byte_len; return r;
}

b_u16slice_t b_u16subslice_cp(const b_cstr_t s, const size_t cp_off,
                                const size_t cp_count) {
    b_u16slice_t empty; empty.data = NULL; empty.len = 0;
    if (!s) return empty;
    const bool   be    = (b_str_enc(s) == B_STR_ENC_UTF16BE);
    const size_t units = b_str_len(s) / 2u;
    size_t ui = 0;

    for (size_t i = 0; i < cp_off && ui < units; i++) {
        uint16_t u1 = _read_u16(s + ui * 2u);
        if (be) u1 = _byte_swap_16(u1);
        ui++;
        if (u1 >= 0xD800u && u1 <= 0xDBFFu && ui < units) {
            uint16_t u2 = _read_u16(s + ui * 2u);
            if (be) u2 = _byte_swap_16(u2);
            if (u2 >= 0xDC00u && u2 <= 0xDFFFu) ui++;
        }
    }
    if (ui >= units && cp_off > 0) return empty;
    const size_t start = ui;

    for (size_t i = 0; i < cp_count && ui < units; i++) {
        uint16_t u1 = _read_u16(s + ui * 2u);
        if (be) u1 = _byte_swap_16(u1);
        ui++;
        if (u1 >= 0xD800u && u1 <= 0xDBFFu && ui < units) {
            uint16_t u2 = _read_u16(s + ui * 2u);
            if (be) u2 = _byte_swap_16(u2);
            if (u2 >= 0xDC00u && u2 <= 0xDFFFu) ui++;
        }
    }
    b_u16slice_t r; r.data = s + start * 2u; r.len = (ui - start) * 2u; return r;
}

size_t b_u16slice_units(const b_u16slice_t slice) { return slice.len / 2u; }

b_u32slice_t b_u32slice_of(const b_cstr_t s) {
    b_u32slice_t r; r.data = s; r.len = b_str_len(s); return r;
}

b_u32slice_t b_u32subslice(const b_cstr_t s, size_t byte_off,
                             size_t byte_len) {
    b_u32slice_t empty; empty.data = NULL; empty.len = 0;
    if (!s) return empty;
    byte_off &= ~(size_t)3u; byte_len &= ~(size_t)3u;
    const size_t total = b_str_len(s) & ~(size_t)3u;
    if (byte_off >= total) return empty;
    const size_t rem = total - byte_off;
    b_u32slice_t r; r.data = s + byte_off; r.len = byte_len > rem ? rem : byte_len; return r;
}

b_u32slice_t b_u32subslice_cp(const b_cstr_t s, const size_t cp_off,
                                const size_t cp_count) {
    b_u32slice_t empty; empty.data = NULL; empty.len = 0;
    if (!s) return empty;
    const size_t total  = b_str_len(s) & ~(size_t)3u;
    const size_t units  = total / 4u;
    if (cp_off >= units) return empty;
    const size_t avail  = units - cp_off;
    const size_t actual = cp_count > avail ? avail : cp_count;
    b_u32slice_t r; r.data = s + cp_off * 4u; r.len = actual * 4u; return r;
}

size_t b_u32slice_units(const b_u32slice_t slice) { return slice.len / 4u; }

b_slice_t b_subslice_cp(const b_cstr_t s, const size_t cp_off,
                         const size_t cp_count) {
    b_slice_t empty; empty.data = NULL; empty.len = 0;
    if (!s) return empty;
    const uint8_t enc = b_str_enc(s);

    if (B_STR_IS_UTF32_ENC(enc)) {
        const b_u32slice_t u32 = b_u32subslice_cp(s, cp_off, cp_count);
        b_slice_t r; r.data = u32.data; r.len = u32.len; return r;
    }
    if (B_STR_IS_UTF16_ENC(enc)) {
        const b_u16slice_t u16 = b_u16subslice_cp(s, cp_off, cp_count);
        b_slice_t r; r.data = u16.data; r.len = u16.len; return r;
    }
    if (enc == B_STR_ENC_UTF8) {
        const b_u8slice_t u8 = b_u8subslice_cp(s, cp_off, cp_count);
        b_slice_t r; r.data = u8.data; r.len = u8.len; return r;
    }
    // ASCII: one byte == one codepoint
    const size_t total = b_str_len(s);
    if (cp_off >= total) return empty;
    const size_t rem = total - cp_off;
    b_slice_t r; r.data = s + cp_off; r.len = cp_count > rem ? rem : cp_count; return r;
}

/*──────────────────────────────────────────────────────────────────────────────
  ENCODING CONVERTERS – SDL_iconv
──────────────────────────────────────────────────────────────────────────────*/

static b_str_t _b_iconv(const b_cstr_t input, const size_t in_bytes,
                         const char * const from_enc, const char * const to_enc,
                         const uint8_t out_enc_tag) {
    SDL_iconv_t cd = SDL_iconv_open(to_enc, from_enc);
    if (cd == (SDL_iconv_t)-1) return NULL;

    // 4× headroom: worst case UTF-8 → UTF-32
    if (in_bytes > (SIZE_MAX - 8u) / 4u) { SDL_iconv_close(cd); return NULL; }
    const size_t out_cap = in_bytes * 4u + 8u;
    uint8_t * const out_buf = (uint8_t*)SDL_malloc(out_cap);
    if (!out_buf) { SDL_iconv_close(cd); return NULL; }

    const char *in_ptr  = (const char*)input;
    size_t in_rem       = in_bytes;
    char  *out_ptr      = (char*)out_buf;
    size_t out_rem      = out_cap;

    const size_t res = SDL_iconv(cd, &in_ptr, &in_rem, &out_ptr, &out_rem);
    SDL_iconv_close(cd);

    if (res == SDL_ICONV_ERROR || res == SDL_ICONV_E2BIG ||
        res == SDL_ICONV_EILSEQ || res == SDL_ICONV_EINVAL ||
        in_rem != 0) {
        SDL_free(out_buf);
        return NULL;
    }

    const size_t out_bytes = out_cap - out_rem;
    b_str_t result = b_str_new_pro(out_buf, out_bytes, out_enc_tag);
    SDL_free(out_buf);
    return result;
}

// Helper: route UTF-8 → target via iconv, or for same-byte-layout encodings
// just dup and retag
static b_str_t _b_from_utf8(const b_cstr_t utf8_src, const size_t bytes,
                              const char * const to_enc_str,
                              const uint8_t out_enc_tag) {
    return _b_iconv(utf8_src, bytes, "UTF-8", to_enc_str, out_enc_tag);
}

b_str_t b_str_to_utf8(b_str_t s) {
    if (!s) return NULL;
    const uint8_t enc = b_str_enc(s);
    if (!B_STR_IS_UTF16_ENC(enc) && !B_STR_IS_UTF32_ENC(enc)) {
        // ASCII/UTF-8: same bytes, just need to retag
        if (enc == B_STR_ENC_UTF8) return s; // already UTF-8, no-op
        // ASCII → UTF-8: for static just update the tag in-place
        if (s[-1] & B_STR_STATIC) { b_str_set_enc(s, B_STR_ENC_UTF8); return s; }
        // Dynamic: dup and retag
        b_str_t r = b_str_dup(s);
        if (!r) return s; // OOM: return s to avoid null-loss
        b_str_set_enc(r, B_STR_ENC_UTF8);
        return r;
    }
    const char *from;
    if      (enc == B_STR_ENC_UTF16BE) from = "UTF-16BE";
    else if (enc == B_STR_ENC_UTF16LE) from = "UTF-16LE";
    else if (enc == B_STR_ENC_UTF32BE) from = "UTF-32BE";
    else                               from = "UTF-32LE";
    b_str_t r = _b_iconv(s, b_str_len(s), from, "UTF-8", B_STR_ENC_UTF8);
    return _b_apply_or_keep(s, r, B_STR_ENC_UTF8);
}

b_str_t b_str_to_utf16(b_str_t s) {
    if (!s) return NULL;
    const uint8_t enc = b_str_enc(s);
    if (enc == B_STR_ENC_UTF16LE) return s; // already UTF-16LE
    if (enc == B_STR_ENC_UTF16BE || B_STR_IS_UTF32_ENC(enc)) {
        // Route through UTF-8
        b_str_t mid = b_str_to_utf8(s);
        if (!mid || mid == s) {
            // to_utf8 failed or was a no-op (shouldn't happen for these encs)
            b_str_t r = _b_iconv(s, b_str_len(s), "UTF-8", "UTF-16LE", B_STR_ENC_UTF16LE);
            return _b_apply_or_keep(s, r, B_STR_ENC_UTF16LE);
        }
        // mid is now UTF-8 (may be same pointer if static)
        b_str_t r = _b_iconv(mid == s ? s : mid,
                              b_str_len(mid == s ? s : mid),
                              "UTF-8", "UTF-16LE", B_STR_ENC_UTF16LE);
        if (mid != s) b_str_free(mid); // free the intermediate if dynamic
        return _b_apply_or_keep(s, r, B_STR_ENC_UTF16LE);
    }
    // ASCII/UTF-8 → UTF-16LE direct
    b_str_t r = _b_iconv(s, b_str_len(s), "UTF-8", "UTF-16LE", B_STR_ENC_UTF16LE);
    return _b_apply_or_keep(s, r, B_STR_ENC_UTF16LE);
}

b_str_t b_str_to_utf16be(b_str_t s) {
    if (!s) return NULL;
    const uint8_t enc = b_str_enc(s);
    if (enc == B_STR_ENC_UTF16BE) return s;
    if (B_STR_IS_UTF16_ENC(enc) || B_STR_IS_UTF32_ENC(enc)) {
        b_str_t mid = b_str_to_utf8(s);
        const b_cstr_t src = (mid && mid != s) ? mid : s;
        b_str_t r = _b_iconv(src, b_str_len(src), "UTF-8", "UTF-16BE", B_STR_ENC_UTF16BE);
        if (mid && mid != s) b_str_free(mid);
        return _b_apply_or_keep(s, r, B_STR_ENC_UTF16BE);
    }
    b_str_t r = _b_iconv(s, b_str_len(s), "UTF-8", "UTF-16BE", B_STR_ENC_UTF16BE);
    return _b_apply_or_keep(s, r, B_STR_ENC_UTF16BE);
}

b_str_t b_str_to_utf32le(b_str_t s) {
    if (!s) return NULL;
    const uint8_t enc = b_str_enc(s);
    if (enc == B_STR_ENC_UTF32LE) return s;
    if (B_STR_IS_UTF16_ENC(enc) || enc == B_STR_ENC_UTF32BE) {
        b_str_t mid = b_str_to_utf8(s);
        const b_cstr_t src = (mid && mid != s) ? mid : s;
        b_str_t r = _b_iconv(src, b_str_len(src), "UTF-8", "UTF-32LE", B_STR_ENC_UTF32LE);
        if (mid && mid != s) b_str_free(mid);
        return _b_apply_or_keep(s, r, B_STR_ENC_UTF32LE);
    }
    b_str_t r = _b_iconv(s, b_str_len(s), "UTF-8", "UTF-32LE", B_STR_ENC_UTF32LE);
    return _b_apply_or_keep(s, r, B_STR_ENC_UTF32LE);
}

b_str_t b_str_to_utf32be(b_str_t s) {
    if (!s) return NULL;
    const uint8_t enc = b_str_enc(s);
    if (enc == B_STR_ENC_UTF32BE) return s;
    if (B_STR_IS_UTF16_ENC(enc) || enc == B_STR_ENC_UTF32LE) {
        b_str_t mid = b_str_to_utf8(s);
        const b_cstr_t src = (mid && mid != s) ? mid : s;
        b_str_t r = _b_iconv(src, b_str_len(src), "UTF-8", "UTF-32BE", B_STR_ENC_UTF32BE);
        if (mid && mid != s) b_str_free(mid);
        return _b_apply_or_keep(s, r, B_STR_ENC_UTF32BE);
    }
    b_str_t r = _b_iconv(s, b_str_len(s), "UTF-8", "UTF-32BE", B_STR_ENC_UTF32BE);
    return _b_apply_or_keep(s, r, B_STR_ENC_UTF32BE);
}

/*──────────────────────────────────────────────────────────────────────────────
  UTF-8 NFC NORMALISATION
──────────────────────────────────────────────────────────────────────────────*/

// utf8proc_map uses system malloc; we bridge its output into SDL allocation
b_str_t b_str_utf8_norm(const char * const nul_utf8) {
    if (!nul_utf8) return NULL;
    utf8proc_uint8_t *tmp = NULL;
    const utf8proc_ssize_t n = utf8proc_map(
        (const utf8proc_uint8_t*)nul_utf8, 0, &tmp,
        (utf8proc_option_t)(UTF8PROC_NULLTERM | UTF8PROC_STABLE | UTF8PROC_COMPOSE));
    if (n < 0) { free(tmp); return NULL; }
    b_str_t r = b_str_new_pro(tmp, (size_t)n, B_STR_ENC_UTF8);
    free(tmp);
    return r;
}

/*──────────────────────────────────────────────────────────────────────────────
  CASE CONVERSION  (UTF-8/ASCII only)
  Returns s unchanged (never NULL) when encoding is unsupported or OOM.
  For static strings: result written back in-place when it fits.
──────────────────────────────────────────────────────────────────────────────*/

b_str_t b_str_lower(b_str_t s) {
    if (!s) return NULL;
    const uint8_t enc = b_str_enc(s);
    if (B_STR_IS_UTF16_ENC(enc) || B_STR_IS_UTF32_ENC(enc)) return s; // fail silently

    const size_t in_len = b_str_len(s);
    size_t out_cap = in_len * 4u + 16u;
    uint8_t *out = (uint8_t*)SDL_malloc(out_cap);
    if (!out) return s; // OOM: return s, no data lost

    size_t in_pos = 0, out_pos = 0;
    while (in_pos < in_len) {
        utf8proc_int32_t cp;
        const size_t rem = in_len - in_pos;
        const utf8proc_ssize_t adv = utf8proc_iterate(
            s + in_pos,
            (utf8proc_ssize_t)(rem < (size_t)SSIZE_MAX ? rem : (size_t)SSIZE_MAX), &cp);

        if (adv <= 0) { // invalid byte: pass through unchanged
            if (out_pos + 1u > out_cap) {
                out_cap *= 2u;
                uint8_t * const r = (uint8_t*)SDL_realloc(out, out_cap);
                if (!r) { SDL_free(out); return s; }
                out = r;
            }
            out[out_pos++] = s[in_pos++];
            continue;
        }
        in_pos += (size_t)adv;

        utf8proc_int32_t folded[4];
        int lcc = 0;
        const utf8proc_ssize_t fcount = utf8proc_decompose_char(
            cp, folded, (utf8proc_ssize_t)(sizeof folded / sizeof *folded),
            (utf8proc_option_t)(UTF8PROC_CASEFOLD | UTF8PROC_DECOMPOSE | UTF8PROC_COMPOSE),
            &lcc);
        const int total_cp = (fcount > 0) ? (int)fcount : 1;
        if (fcount <= 0) folded[0] = utf8proc_tolower(cp);

        for (int fi = 0; fi < total_cp; fi++) {
            if (out_pos + 4u > out_cap) {
                out_cap = out_cap * 2u + 16u;
                uint8_t * const r = (uint8_t*)SDL_realloc(out, out_cap);
                if (!r) { SDL_free(out); return s; }
                out = r;
            }
            const utf8proc_ssize_t bw = utf8proc_encode_char(folded[fi], out + out_pos);
            if (bw > 0) out_pos += (size_t)bw;
        }
    }

    b_str_t result = b_str_new_pro(out, out_pos, B_STR_ENC_UTF8);
    SDL_free(out);
    if (!result) return s; // OOM building final string
    return _b_apply_or_keep(s, result, B_STR_ENC_UTF8);
}

b_str_t b_str_upper(b_str_t s) {
    if (!s) return NULL;
    const uint8_t enc = b_str_enc(s);
    if (B_STR_IS_UTF16_ENC(enc) || B_STR_IS_UTF32_ENC(enc)) return s;

    const size_t in_len = b_str_len(s);
    size_t out_cap = in_len * 4u + 16u;
    uint8_t *out = (uint8_t*)SDL_malloc(out_cap);
    if (!out) return s;

    size_t in_pos = 0, out_pos = 0;
    while (in_pos < in_len) {
        utf8proc_int32_t cp;
        const size_t rem = in_len - in_pos;
        const utf8proc_ssize_t adv = utf8proc_iterate(
            s + in_pos,
            (utf8proc_ssize_t)(rem < (size_t)SSIZE_MAX ? rem : (size_t)SSIZE_MAX), &cp);

        if (adv <= 0) {
            if (out_pos + 1u > out_cap) {
                out_cap *= 2u;
                uint8_t * const r = (uint8_t*)SDL_realloc(out, out_cap);
                if (!r) { SDL_free(out); return s; }
                out = r;
            }
            out[out_pos++] = s[in_pos++];
            continue;
        }
        in_pos += (size_t)adv;

        if (out_pos + 4u > out_cap) {
            out_cap = out_cap * 2u + 16u;
            uint8_t * const r = (uint8_t*)SDL_realloc(out, out_cap);
            if (!r) { SDL_free(out); return s; }
            out = r;
        }
        const utf8proc_ssize_t bw = utf8proc_encode_char(utf8proc_toupper(cp), out + out_pos);
        if (bw > 0) out_pos += (size_t)bw;
    }

    b_str_t result = b_str_new_pro(out, out_pos, B_STR_ENC_UTF8);
    SDL_free(out);
    if (!result) return s;
    return _b_apply_or_keep(s, result, B_STR_ENC_UTF8);
}

/*──────────────────────────────────────────────────────────────────────────────
  COMPARISON & SEARCH
──────────────────────────────────────────────────────────────────────────────*/

int b_str_cmp(const b_cstr_t a, const b_cstr_t b) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return  1;
    const size_t la = b_str_len(a), lb = b_str_len(b);
    const int cmp = memcmp(a, b, la < lb ? la : lb);
    if (cmp) return cmp;
    return (la < lb) ? -1 : (la > lb) ? 1 : 0;
}

bool b_str_eq(const b_cstr_t a, const b_cstr_t b) {
    if (a == b) return true;
    if (!a || !b) return false;
    const size_t la = b_str_len(a);
    return la == b_str_len(b) && memcmp(a, b, la) == 0;
}

size_t b_str_find(const b_cstr_t haystack, const b_cstr_t needle) {
    if (!haystack || !needle) return SIZE_MAX;
    const size_t hl = b_str_len(haystack), nl = b_str_len(needle);
    if (!nl) return 0;
    if (nl > hl) return SIZE_MAX;
    const size_t lim = hl - nl;
    for (size_t i = 0; i <= lim; i++)
        if (memcmp(haystack + i, needle, nl) == 0) return i;
    return SIZE_MAX;
}

bool b_str_contains(const b_cstr_t s, const b_cstr_t needle) {
    return b_str_find(s, needle) != SIZE_MAX;
}

bool b_str_starts_with(const b_cstr_t s, const b_cstr_t prefix) {
    if (!s || !prefix) return false;
    const size_t pl = b_str_len(prefix);
    return !pl || (pl <= b_str_len(s) && memcmp(s, prefix, pl) == 0);
}

bool b_str_ends_with(const b_cstr_t s, const b_cstr_t suffix) {
    if (!s || !suffix) return false;
    const size_t sl = b_str_len(s), sfl = b_str_len(suffix);
    return !sfl || (sfl <= sl && memcmp(s + sl - sfl, suffix, sfl) == 0);
}

/*──────────────────────────────────────────────────────────────────────────────
  IN-PLACE MUTATION
──────────────────────────────────────────────────────────────────────────────*/

static inline bool _is_utf16_ws(const uint8_t * const buf, const size_t off,
                                 const bool be) {
    uint16_t u = _read_u16(buf + off);
    if (be) u = _byte_swap_16(u);
    return u == 0x0020u || u == 0x0009u || u == 0x000Au || u == 0x000Du;
}

static inline bool _is_utf32_ws(const uint8_t * const buf, const size_t off,
                                 const bool be) {
    uint32_t u = _read_u32(buf + off);
    if (be) u = _byte_swap_32(u);
    return u == 0x20u || u == 0x09u || u == 0x0Au || u == 0x0Du;
}

b_str_t b_str_trim_r(b_str_t s) {
    if (!s) return NULL;
    const uint8_t enc     = b_str_enc(s);
    const size_t  null_sz = _null_term_size(enc);
    size_t        len     = b_str_len(s);

    if (B_STR_IS_UTF32_ENC(enc)) {
        const bool be = (enc == B_STR_ENC_UTF32BE);
        while (len >= 4u && _is_utf32_ws(s, len - 4u, be)) len -= 4u;
    } else if (B_STR_IS_UTF16_ENC(enc)) {
        const bool be = (enc == B_STR_ENC_UTF16BE);
        while (len >= 2u && _is_utf16_ws(s, len - 2u, be)) len -= 2u;
    } else {
        while (len > 0u) {
            const uint8_t b = s[len - 1u];
            if (b == ' ' || b == '\t' || b == '\n' || b == '\r') len--;
            else break;
        }
    }
    b_str_set_len(s, len);
    _write_null_terminator(s, len, null_sz);
    return s;
}

b_str_t b_str_trim_l(b_str_t s) {
    if (!s) return NULL;
    const uint8_t enc     = b_str_enc(s);
    const size_t  null_sz = _null_term_size(enc);
    const size_t  len     = b_str_len(s);
    size_t        start   = 0;

    if (B_STR_IS_UTF32_ENC(enc)) {
        const bool be = (enc == B_STR_ENC_UTF32BE);
        while (start + 4u <= len && _is_utf32_ws(s, start, be)) start += 4u;
    } else if (B_STR_IS_UTF16_ENC(enc)) {
        const bool be = (enc == B_STR_ENC_UTF16BE);
        while (start + 2u <= len && _is_utf16_ws(s, start, be)) start += 2u;
    } else {
        while (start < len) {
            const uint8_t b = s[start];
            if (b == ' ' || b == '\t' || b == '\n' || b == '\r') start++;
            else break;
        }
    }

    if (start > 0u) {
        const size_t new_len = len - start;
        memmove(s, s + start, new_len);
        b_str_set_len(s, new_len);
        _write_null_terminator(s, new_len, null_sz);
    }
    return s;
}

b_str_t b_str_trim(b_str_t s) { return b_str_trim_r(b_str_trim_l(s)); }

b_str_t b_str_repeat(const b_cstr_t s, const size_t count) {
    const uint8_t enc = s ? b_str_enc(s) : B_STR_ENC_ASCII;
    if (!s || !count) return b_str_new_pro(NULL, 0, enc);
    const size_t unit = b_str_len(s);
    if (!unit) return b_str_new_pro(NULL, 0, enc);
    if (unit > SIZE_MAX / count) return NULL;

    const size_t total = unit * count;
    b_str_t r = b_str_new_pro(NULL, total, enc);
    if (!r) return NULL;
    for (size_t i = 0; i < count; i++) memcpy(r + i * unit, s, unit);
    return r;
}

/*──────────────────────────────────────────────────────────────────────────────
  VALIDATION
──────────────────────────────────────────────────────────────────────────────*/

bool b_str_valid_utf8(const b_cstr_t s) {
    if (!s) return true;
    const size_t total = b_str_len(s);
    size_t pos = 0;
    while (pos < total) {
        utf8proc_int32_t cp;
        const size_t rem = total - pos;
        const utf8proc_ssize_t adv = utf8proc_iterate(
            s + pos,
            (utf8proc_ssize_t)(rem < (size_t)SSIZE_MAX ? rem : (size_t)SSIZE_MAX), &cp);
        if (adv <= 0) return false;
        pos += (size_t)adv;
    }
    return true;
}

/*──────────────────────────────────────────────────────────────────────────────
  BOM
──────────────────────────────────────────────────────────────────────────────*/

uint8_t b_str_detect_bom(const void * const data, const size_t byte_len,
                          size_t * const bom_size_out) {
    if (bom_size_out) *bom_size_out = 0;
    if (!data || byte_len < 2u) return B_STR_ENC_ASCII;
    const uint8_t * const raw = (const uint8_t*)data;

    // UTF-32 BE: 00 00 FE FF  (must check before UTF-16 BE)
    if (byte_len >= 4u && raw[0]==0x00u && raw[1]==0x00u && raw[2]==0xFEu && raw[3]==0xFFu)
        { if (bom_size_out) *bom_size_out = 4u; return B_STR_ENC_UTF32BE; }
    // UTF-32 LE: FF FE 00 00  (must check before UTF-16 LE)
    if (byte_len >= 4u && raw[0]==0xFFu && raw[1]==0xFEu && raw[2]==0x00u && raw[3]==0x00u)
        { if (bom_size_out) *bom_size_out = 4u; return B_STR_ENC_UTF32LE; }
    // UTF-8: EF BB BF
    if (byte_len >= 3u && raw[0]==0xEFu && raw[1]==0xBBu && raw[2]==0xBFu)
        { if (bom_size_out) *bom_size_out = 3u; return B_STR_ENC_UTF8; }
    // UTF-16 LE: FF FE
    if (raw[0]==0xFFu && raw[1]==0xFEu)
        { if (bom_size_out) *bom_size_out = 2u; return B_STR_ENC_UTF16LE; }
    // UTF-16 BE: FE FF
    if (raw[0]==0xFEu && raw[1]==0xFFu)
        { if (bom_size_out) *bom_size_out = 2u; return B_STR_ENC_UTF16BE; }

    return B_STR_ENC_ASCII;
}

static const uint8_t *_bom_for_enc(const uint8_t encoding,
                                    size_t * const out) {
    switch (encoding & B_STR_ENC_MASK) {
    case B_STR_ENC_UTF8:    *out = 3u; return BOM_UTF8;
    case B_STR_ENC_UTF16LE: *out = 2u; return BOM_UTF16LE;
    case B_STR_ENC_UTF16BE: *out = 2u; return BOM_UTF16BE;
    case B_STR_ENC_UTF32LE: *out = 4u; return BOM_UTF32LE;
    case B_STR_ENC_UTF32BE: *out = 4u; return BOM_UTF32BE;
    default:                *out = 0u; return NULL;
    }
}

b_str_t b_str_add_bom(b_str_t s) {
    if (!s) return NULL;
    size_t bom_sz;
    const uint8_t * const bom = _bom_for_enc(b_str_enc(s), &bom_sz);
    if (!bom || !bom_sz) return s; // ASCII: no BOM

    const size_t  cur_len = b_str_len(s);
    const uint8_t enc     = b_str_enc(s);
    const size_t  null_sz = _null_term_size(enc);

    s = b_str_ensure(s, bom_sz);
    if (b_str_avail(s) < bom_sz) return s; // OOM or static full: fail silently

    memmove(s + bom_sz, s, cur_len + null_sz); // shift content right
    memcpy(s, bom, bom_sz);
    b_str_set_len(s, cur_len + bom_sz);
    return s;
}

/*──────────────────────────────────────────────────────────────────────────────
  FILE I/O
──────────────────────────────────────────────────────────────────────────────*/

static uint8_t *_read_entire_file(const char * const path,
                                   size_t * const file_size_out) {
    FILE * const fh = fopen(path, "rb");
    if (!fh) return NULL;
    if (fseek(fh, 0, SEEK_END) != 0) { fclose(fh); return NULL; }
    const long sz_signed = ftell(fh);
    if (sz_signed < 0) { fclose(fh); return NULL; }
    rewind(fh);
    const size_t sz = (size_t)sz_signed;
    // +4 trailing zero bytes for widest null terminator (UTF-32)
    uint8_t * const buf = (uint8_t*)SDL_malloc(sz + 4u);
    if (!buf) { fclose(fh); return NULL; }
    memset(buf + sz, 0, 4u);
    if (fread(buf, 1u, sz, fh) != sz) { fclose(fh); SDL_free(buf); return NULL; }
    fclose(fh);
    *file_size_out = sz;
    return buf;
}

b_str_t b_str_load_file(const char * const path, uint8_t fallback_enc) {
    if (!path) return NULL;
    if (!fallback_enc) fallback_enc = B_STR_ENC_UTF8;
    size_t sz;
    uint8_t * const raw = _read_entire_file(path, &sz);
    if (!raw) return NULL;
    size_t bom_sz = 0;
    const uint8_t detected = b_str_detect_bom(raw, sz, &bom_sz);
    const uint8_t eff_enc  = bom_sz ? detected : fallback_enc;
    b_str_t r = b_str_new_pro(raw + bom_sz, sz - bom_sz, eff_enc);
    SDL_free(raw);
    return r;
}

int b_str_save_file(const char * const path, const b_cstr_t s,
                    const bool write_bom) {
    if (!path || !s) return -1;
    FILE * const fh = fopen(path, "wb");
    if (!fh) return -1;
    if (write_bom) {
        size_t bom_sz;
        const uint8_t * const bom = _bom_for_enc(b_str_enc(s), &bom_sz);
        if (bom && bom_sz && fwrite(bom, 1u, bom_sz, fh) != bom_sz)
            { fclose(fh); return -1; }
    }
    const size_t cl = b_str_len(s);
    if (cl && fwrite(s, 1u, cl, fh) != cl) { fclose(fh); return -1; }
    fclose(fh);
    return 0;
}

int b_file_add_bom(const char * const path, const uint8_t encoding) {
    size_t bom_sz;
    const uint8_t * const bom = _bom_for_enc(encoding, &bom_sz);
    if (!bom || !bom_sz) return 0; // ASCII: nothing to do

    size_t file_sz;
    uint8_t * const raw = _read_entire_file(path, &file_sz);
    if (!raw) return -1;
    // Already has this exact BOM: leave untouched
    if (file_sz >= bom_sz && memcmp(raw, bom, bom_sz) == 0)
        { SDL_free(raw); return 0; }

    FILE * const fh = fopen(path, "wb");
    if (!fh) { SDL_free(raw); return -1; }
    int res = 0;
    if (fwrite(bom, 1u, bom_sz, fh) != bom_sz) res = -1;
    if (res == 0 && file_sz && fwrite(raw, 1u, file_sz, fh) != file_sz) res = -1;
    fclose(fh);
    SDL_free(raw);
    return res;
}

/*──────────────────────────────────────────────────────────────────────────────
  INTERNAL GENERIC FILE CONVERTER
──────────────────────────────────────────────────────────────────────────────*/

static int _b_file_convert(const char * const in_path, const char * const out_path,
                            const uint8_t fallback_enc,
                            const uint8_t out_enc, const bool write_bom) {
    const uint8_t eff_fb = fallback_enc ? fallback_enc : B_STR_ENC_UTF8;
    b_str_t src = b_str_load_file(in_path, eff_fb);
    if (!src) return -1;

    const uint8_t src_enc = b_str_enc(src);
    b_str_t dst = NULL;

    if (src_enc == out_enc) {
        dst = b_str_dup(src);
    } else if (!B_STR_IS_UTF16_ENC(src_enc) && !B_STR_IS_UTF32_ENC(src_enc)
               && !B_STR_IS_UTF16_ENC(out_enc) && !B_STR_IS_UTF32_ENC(out_enc)) {
        dst = b_str_dup(src);
        if (dst) b_str_set_enc(dst, out_enc);
    } else {
        // Route through UTF-8
        b_str_t mid = NULL;
        if (B_STR_IS_UTF16_ENC(src_enc) || B_STR_IS_UTF32_ENC(src_enc)) {
            mid = b_str_to_utf8(src); // src is dynamic; returns new ptr or src on fail
            if (mid == src) { b_str_free(src); return -1; } // conversion actually failed
        } else {
            mid = b_str_dup(src);
            if (mid) b_str_set_enc(mid, B_STR_ENC_UTF8);
        }
        if (!mid) { b_str_free(src); return -1; }

        if (!B_STR_IS_UTF16_ENC(out_enc) && !B_STR_IS_UTF32_ENC(out_enc)) {
            dst = mid; mid = NULL;
            b_str_set_enc(dst, out_enc);
        } else if (out_enc == B_STR_ENC_UTF16LE) {
            dst = b_str_to_utf16(mid);
            if (dst == mid) dst = NULL; // conversion failed if returned same ptr
        } else if (out_enc == B_STR_ENC_UTF16BE) {
            dst = b_str_to_utf16be(mid);
            if (dst == mid) dst = NULL;
        } else if (out_enc == B_STR_ENC_UTF32LE) {
            dst = b_str_to_utf32le(mid);
            if (dst == mid) dst = NULL;
        } else if (out_enc == B_STR_ENC_UTF32BE) {
            dst = b_str_to_utf32be(mid);
            if (dst == mid) dst = NULL;
        }
        b_str_free(mid);
    }

    b_str_free(src);
    if (!dst) return -1;

    b_str_set_enc(dst, out_enc);
    const int result = b_str_save_file(out_path, dst, write_bom);
    b_str_free(dst);
    return result;
}

/*──────────────────────────────────────────────────────────────────────────────
  FILE CONVERSION WRAPPERS
──────────────────────────────────────────────────────────────────────────────*/

int b_file_conv_ascii_to_utf8_bom   (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_ASCII,   B_STR_ENC_UTF8,    true); }
int b_file_conv_ascii_to_utf8_no_bom(const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_ASCII,   B_STR_ENC_UTF8,   false); }

int b_file_conv_utf8_to_utf16               (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF8, _system_utf16_encoding(), true); }
int b_file_conv_utf8_to_utf16le_bom         (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF8, B_STR_ENC_UTF16LE,  true); }
int b_file_conv_utf8_to_utf16le_no_bom      (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF8, B_STR_ENC_UTF16LE, false); }
int b_file_conv_utf8_to_utf16be_bom         (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF8, B_STR_ENC_UTF16BE,  true); }
int b_file_conv_utf8_to_utf16be_no_bom      (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF8, B_STR_ENC_UTF16BE, false); }

int b_file_conv_utf8_to_utf32               (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF8, _system_utf32_encoding(), true); }
int b_file_conv_utf8_to_utf32le_bom         (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF8, B_STR_ENC_UTF32LE,  true); }
int b_file_conv_utf8_to_utf32le_no_bom      (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF8, B_STR_ENC_UTF32LE, false); }
int b_file_conv_utf8_to_utf32be_bom         (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF8, B_STR_ENC_UTF32BE,  true); }
int b_file_conv_utf8_to_utf32be_no_bom      (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF8, B_STR_ENC_UTF32BE, false); }

int b_file_conv_utf16_to_utf8_no_bom          (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF16LE, B_STR_ENC_UTF8, false); }
int b_file_conv_utf16le_bom_to_utf8_no_bom    (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF16LE, B_STR_ENC_UTF8, false); }
int b_file_conv_utf16le_no_bom_to_utf8_bom    (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF16LE, B_STR_ENC_UTF8,  true); }
int b_file_conv_utf16le_no_bom_to_utf8_no_bom (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF16LE, B_STR_ENC_UTF8, false); }
int b_file_conv_utf16be_bom_to_utf8_no_bom    (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF16BE, B_STR_ENC_UTF8, false); }
int b_file_conv_utf16be_no_bom_to_utf8_bom    (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF16BE, B_STR_ENC_UTF8,  true); }
int b_file_conv_utf16be_no_bom_to_utf8_no_bom (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF16BE, B_STR_ENC_UTF8, false); }

int b_file_conv_utf32_to_utf8_no_bom          (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF32LE, B_STR_ENC_UTF8, false); }
int b_file_conv_utf32le_bom_to_utf8_no_bom    (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF32LE, B_STR_ENC_UTF8, false); }
int b_file_conv_utf32le_no_bom_to_utf8_bom    (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF32LE, B_STR_ENC_UTF8,  true); }
int b_file_conv_utf32le_no_bom_to_utf8_no_bom (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF32LE, B_STR_ENC_UTF8, false); }
int b_file_conv_utf32be_bom_to_utf8_no_bom    (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF32BE, B_STR_ENC_UTF8, false); }
int b_file_conv_utf32be_no_bom_to_utf8_bom    (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF32BE, B_STR_ENC_UTF8,  true); }
int b_file_conv_utf32be_no_bom_to_utf8_no_bom (const char *i, const char *o) { return _b_file_convert(i, o, B_STR_ENC_UTF32BE, B_STR_ENC_UTF8, false); }

/*──────────────────────────────────────────────────────────────────────────────
  UTF-16 STDOUT HELPER
──────────────────────────────────────────────────────────────────────────────*/

void b_str_print_utf16(const b_u16slice_t slice) {
    if (!slice.data || !slice.len) return;
    b_str_t tmp16 = b_str_from_u16slice(slice);
    if (!tmp16) return;
    b_str_t tmp8 = b_str_to_utf8(tmp16);
    // if to_utf8 returns tmp16 (same ptr, failed), nothing to print as UTF-8
    if (tmp8 && tmp8 != tmp16)
        fwrite(tmp8, 1u, b_str_len(tmp8), stdout);
    if (tmp8 && tmp8 != tmp16) b_str_free(tmp8);
    b_str_free(tmp16);
}