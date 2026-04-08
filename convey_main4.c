/*
 * convey_main.c  –  Implementation of the convey dynamic-string library.
 *
 * Build:
 *   Compile together with utf8proc.c, or link against -lutf8proc.
 *   Requires C11 or later (uses _Static_assert, designated initialisers, etc.).
 *   Compile with -std=c11 -Wall -Wextra.
 */

#include "convey4.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>   /* UINT32_MAX */

/*
 * SSIZE_MAX: the largest positive value that fits in a ssize_t / ptrdiff_t.
 * Used to clamp size_t byte counts before passing them to utf8proc functions
 * whose length parameters are signed.  Defined here as a fallback for platforms
 * (MSVC, some embedded headers) that do not expose it via <limits.h>.
 */
#ifndef SSIZE_MAX
#  define SSIZE_MAX ((size_t)(~(size_t)0) >> 1)
#endif

/*─────────────────────────────────────────────────────────────────────────────────
  INTERNAL HELPERS
  All helpers in this section are declared `static inline` to give them internal
  linkage and allow the compiler to inline them freely.  Without `static` they
  would have external linkage, pollute the global symbol table, and collide with
  identical names in other translation units (e.g. the system's own `bswap16`).
─────────────────────────────────────────────────────────────────────────────────*/

/*
 * null_size_for  –  Return the NUL-terminator byte count for a given encoding.
 * Mirrors C_STRING_NULL_SIZE() but as an inline function for clarity in places
 * where the macro expansion would be verbose.
 */
static inline size_t null_size_for(unsigned char enc)
{
    return ((enc & C_STRING_ENC_MASK) == C_STRING_ENC_UTF16) ? 2u : 1u;
}

/*
 * c_read_u16  –  Read a uint16_t from an unaligned address without UB.
 * Using memcpy lets the compiler emit the optimal load (e.g. `movzwl` on x86)
 * without strict-aliasing or alignment violations.
 */
static inline uint16_t c_read_u16(const void *ptr)
{
    uint16_t v;
    memcpy(&v, ptr, sizeof v);
    return v;
}

/*
 * c_write_u16  –  Write a uint16_t to an unaligned address without UB.
 */
static inline void c_write_u16(void *ptr, uint16_t v)
{
    memcpy(ptr, &v, sizeof v);
}

/*
 * bswap16  –  Swap the two bytes of a 16-bit value.
 * Declared static inline to avoid conflicts with <byteswap.h> on Linux,
 * compiler built-ins, or any other translation unit that defines the same name.
 */
static inline uint16_t bswap16(uint16_t v)
{
    return (uint16_t)(((v & 0x00FFu) << 8) | ((v & 0xFF00u) >> 8));
}

/*
 * host_is_le  –  Return true when the host CPU is little-endian.
 * The probe variable's address aliased via char * is a defined exception to
 * strict aliasing (C11 §6.5 p7).
 */
static inline bool host_is_le(void)
{
    const int probe = 1;
    return *(const char *)&probe == 1;
}

/*
 * needs_swap  –  Decide whether byte-swapping is required for a UTF-16 operation.
 *
 * convey stores UTF-16 in native LE order by default.  When the caller requests
 * a different endianness:
 *   use_big_endian=false → data is/should be UTF-16 LE  (no swap on LE hosts)
 *   use_big_endian=true  → data is/should be UTF-16 BE  (swap on LE hosts)
 *
 * Returns true when the requested endianness differs from the host byte order.
 */
static inline bool needs_swap(bool use_big_endian)
{
    return use_big_endian == host_is_le();
}

/*─────────────────────────────────────────────────────────────────────────────────
  INTERNAL GETTERS & SETTERS
  These read or write the packed header fields that live immediately before the
  data pointer.  The switch on (string[-1] & C_STRING_TYPE_MASK) selects the
  correct header struct width.
─────────────────────────────────────────────────────────────────────────────────*/

size_t c_string_get_header_size(const unsigned char flags)
{
    switch (flags & C_STRING_TYPE_MASK) {
        case C_STRING_TYPE_8:  return sizeof(struct c_string_header_8);
        case C_STRING_TYPE_16: return sizeof(struct c_string_header_16);
        case C_STRING_TYPE_32: return sizeof(struct c_string_header_32);
        case C_STRING_TYPE_64: return sizeof(struct c_string_header_64);
        default:               return 0; /* unreachable; satisfies -Wreturn-type */
    }
}

unsigned char c_string_determine_type(const size_t size)
{
    /* Choose the smallest header class whose field width can store `size`. */
    if (size <  256u)           return C_STRING_TYPE_8;
    if (size <  65536u)         return C_STRING_TYPE_16;
    if (size <= (size_t)UINT32_MAX) return C_STRING_TYPE_32;
    return C_STRING_TYPE_64;
}

unsigned char c_string_get_encoding(const c_const_string_t string)
{
    if (!string) return C_STRING_ENC_ASCII;
    return (unsigned char)(string[-1] & C_STRING_ENC_MASK);
}

void c_string_set_encoding(c_string_t string, const unsigned char enc)
{
    if (!string) return;
    /* Clear the old encoding bits and OR in the new ones; leave all other bits untouched. */
    string[-1] = (unsigned char)((string[-1] & ~C_STRING_ENC_MASK) | (enc & C_STRING_ENC_MASK));
}

size_t c_string_get_used_length(const c_const_string_t string)
{
    if (!string) return 0;
    switch (string[-1] & C_STRING_TYPE_MASK) {
        case C_STRING_TYPE_8:  return GET_CONST_HEADER_8(string)->used_length;
        case C_STRING_TYPE_16: return GET_CONST_HEADER_16(string)->used_length;
        case C_STRING_TYPE_32: return GET_CONST_HEADER_32(string)->used_length;
        case C_STRING_TYPE_64: return GET_CONST_HEADER_64(string)->used_length;
        default:               return 0;
    }
}

size_t c_string_get_available_capacity(const c_const_string_t string)
{
    if (!string) return 0;
    switch (string[-1] & C_STRING_TYPE_MASK) {
        case C_STRING_TYPE_8: {
            const struct c_string_header_8 *h = GET_CONST_HEADER_8(string);
            return h->allocated_capacity > h->used_length
                   ? (size_t)(h->allocated_capacity - h->used_length) : 0;
        }
        case C_STRING_TYPE_16: {
            const struct c_string_header_16 *h = GET_CONST_HEADER_16(string);
            return h->allocated_capacity > h->used_length
                   ? (size_t)(h->allocated_capacity - h->used_length) : 0;
        }
        case C_STRING_TYPE_32: {
            const struct c_string_header_32 *h = GET_CONST_HEADER_32(string);
            return h->allocated_capacity > h->used_length
                   ? (size_t)(h->allocated_capacity - h->used_length) : 0;
        }
        case C_STRING_TYPE_64: {
            const struct c_string_header_64 *h = GET_CONST_HEADER_64(string);
            return h->allocated_capacity > h->used_length
                   ? (size_t)(h->allocated_capacity - h->used_length) : 0;
        }
        default: return 0;
    }
}

size_t c_string_get_capacity(const c_const_string_t string)
{
    /* allocated_capacity = used_length + available_capacity */
    if (!string) return 0;
    return c_string_get_used_length(string) + c_string_get_available_capacity(string);
}

void c_string_set_lengths(c_string_t string, size_t used, size_t capacity)
{
    if (!string) return;

    /* UTF-16 invariant: both length values must be even (2-byte aligned). */
    if ((string[-1] & C_STRING_ENC_MASK) == C_STRING_ENC_UTF16) {
        used     &= ~(size_t)1u;
        capacity &= ~(size_t)1u;
    }

    switch (string[-1] & C_STRING_TYPE_MASK) {
        case C_STRING_TYPE_8:
            GET_HEADER_8(string)->used_length        = (uint8_t)used;
            GET_HEADER_8(string)->allocated_capacity = (uint8_t)capacity;
            break;
        case C_STRING_TYPE_16:
            GET_HEADER_16(string)->used_length        = (uint16_t)used;
            GET_HEADER_16(string)->allocated_capacity = (uint16_t)capacity;
            break;
        case C_STRING_TYPE_32:
            GET_HEADER_32(string)->used_length        = (uint32_t)used;
            GET_HEADER_32(string)->allocated_capacity = (uint32_t)capacity;
            break;
        case C_STRING_TYPE_64:
            GET_HEADER_64(string)->used_length        = (uint64_t)used;
            GET_HEADER_64(string)->allocated_capacity = (uint64_t)capacity;
            break;
    }
}

void c_string_set_used_length(c_string_t string, size_t used)
{
    if (!string) return;

    /* UTF-16 invariant: used_length must be even. */
    if ((string[-1] & C_STRING_ENC_MASK) == C_STRING_ENC_UTF16)
        used &= ~(size_t)1u;

    switch (string[-1] & C_STRING_TYPE_MASK) {
        case C_STRING_TYPE_8:  GET_HEADER_8(string)->used_length  = (uint8_t)used;  break;
        case C_STRING_TYPE_16: GET_HEADER_16(string)->used_length = (uint16_t)used; break;
        case C_STRING_TYPE_32: GET_HEADER_32(string)->used_length = (uint32_t)used; break;
        case C_STRING_TYPE_64: GET_HEADER_64(string)->used_length = (uint64_t)used; break;
    }
}

/*─────────────────────────────────────────────────────────────────────────────────
  CODEPOINT COUNT
─────────────────────────────────────────────────────────────────────────────────*/

size_t c_string_codepoint_count(const c_const_string_t string)
{
    if (!string) return 0;

    const size_t        total = c_string_get_used_length(string);
    const unsigned char enc   = c_string_get_encoding(string);

    if (total == 0) return 0;

    if (enc == C_STRING_ENC_UTF8) {
        size_t count = 0, pos = 0;
        while (pos < total) {
            utf8proc_int32_t cp;
            /* Clamp the remaining byte count to utf8proc_ssize_t's positive range. */
            const size_t rem = total - pos;
            const utf8proc_ssize_t bound =
                (utf8proc_ssize_t)(rem > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : rem);
            const utf8proc_ssize_t adv = utf8proc_iterate(string + pos, bound, &cp);
            if (adv <= 0) {
                /* Invalid byte sequence: count the byte as one unit and advance past it. */
                pos++;
            } else {
                pos += (size_t)adv;
            }
            count++;
        }
        return count;
    }

    if (enc == C_STRING_ENC_UTF16) {
        /*
         * Walk the code-unit stream, consuming surrogate pairs as a single
         * codepoint.  Lone surrogates are counted as one codepoint each.
         */
        const size_t total_units = total / 2;
        size_t count = 0, unit_pos = 0;

        while (unit_pos < total_units) {
            const uint16_t u1 = c_read_u16(string + unit_pos * 2);
            unit_pos++;

            if (u1 >= 0xD800u && u1 <= 0xDBFFu && unit_pos < total_units) {
                /* High surrogate: check whether a low surrogate follows. */
                const uint16_t u2 = c_read_u16(string + unit_pos * 2);
                if (u2 >= 0xDC00u && u2 <= 0xDFFFu)
                    unit_pos++; /* valid surrogate pair – consume the low unit */
                /* else: lone high surrogate – already counted as one codepoint */
            }
            count++;
        }
        return count;
    }

    /* ASCII / raw: each byte is exactly one codepoint. */
    return total;
}

/*─────────────────────────────────────────────────────────────────────────────────
  SLICE EXTRACTORS
─────────────────────────────────────────────────────────────────────────────────*/

c_slice_t c_slice_of(const c_const_string_t string)
{
    c_slice_t s = { string, c_string_get_used_length(string) };
    return s;
}

c_slice_t c_subslice(const c_const_string_t string,
                     const size_t byte_offset, const size_t byte_length)
{
    c_slice_t s = { NULL, 0 };
    if (!string) return s;

    const size_t total = c_string_get_used_length(string);
    if (byte_offset >= total) return s;

    const size_t remaining = total - byte_offset;
    s.data   = string + byte_offset;
    s.length = byte_length > remaining ? remaining : byte_length;
    return s;
}

c_utf8_slice_t c_utf8_slice_of(const c_const_string_t string)
{
    c_utf8_slice_t s = { string, c_string_get_used_length(string) };
    return s;
}

c_utf8_slice_t c_utf8_subslice(const c_const_string_t string,
                                const size_t byte_offset, const size_t byte_length)
{
    c_utf8_slice_t s = { NULL, 0 };
    if (!string) return s;

    const size_t total = c_string_get_used_length(string);
    if (byte_offset >= total) return s;

    const size_t remaining = total - byte_offset;
    s.data   = string + byte_offset;
    s.length = byte_length > remaining ? remaining : byte_length;
    return s;
}

c_utf8_slice_t c_utf8_subslice_codepoints(const c_const_string_t string,
                                           const size_t cp_offset,
                                           const size_t cp_count)
{
    c_utf8_slice_t s = { NULL, 0 };
    if (!string) return s;

    const size_t total = c_string_get_used_length(string);
    size_t pos = 0;

    /* Skip cp_offset codepoints. */
    for (size_t i = 0; i < cp_offset && pos < total; i++) {
        utf8proc_int32_t cp;
        const size_t rem = total - pos;
        const utf8proc_ssize_t bound =
            (utf8proc_ssize_t)(rem > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : rem);
        const utf8proc_ssize_t adv = utf8proc_iterate(string + pos, bound, &cp);
        pos += (adv > 0) ? (size_t)adv : 1u; /* advance past invalid byte if needed */
    }

    /* If cp_offset was non-zero but we've already exhausted the string, return empty. */
    if (pos >= total && cp_offset > 0) return s;
    const size_t start = pos;

    /* Collect cp_count codepoints starting from `pos`. */
    for (size_t i = 0; i < cp_count && pos < total; i++) {
        utf8proc_int32_t cp;
        const size_t rem = total - pos;
        const utf8proc_ssize_t bound =
            (utf8proc_ssize_t)(rem > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : rem);
        const utf8proc_ssize_t adv = utf8proc_iterate(string + pos, bound, &cp);
        pos += (adv > 0) ? (size_t)adv : 1u;
    }

    s.data   = string + start;
    s.length = pos - start;
    return s;
}

c_utf16_slice_t c_utf16_slice_of(const c_const_string_t string)
{
    c_utf16_slice_t s = { string, c_string_get_used_length(string) };
    return s;
}

c_utf16_slice_t c_utf16_subslice(const c_const_string_t string,
                                  size_t byte_offset, size_t byte_length)
{
    c_utf16_slice_t s = { NULL, 0 };
    if (!string) return s;

    /* Round both values down to 2-byte boundaries (UTF-16 code-unit alignment). */
    byte_offset &= ~(size_t)1u;
    byte_length &= ~(size_t)1u;

    const size_t total = c_string_get_used_length(string) & ~(size_t)1u;
    if (byte_offset >= total) return s;

    const size_t remaining = total - byte_offset;
    s.data   = string + byte_offset;
    s.length = byte_length > remaining ? remaining : byte_length;
    return s;
}

c_utf16_slice_t c_utf16_subslice_codepoints(const c_const_string_t string,
                                             const size_t cp_offset,
                                             const size_t cp_count)
{
    c_utf16_slice_t s = { NULL, 0 };
    if (!string) return s;

    const size_t total_units = c_string_get_used_length(string) / 2;
    size_t unit_pos = 0;

    /* Skip cp_offset codepoints (surrogate-pair-aware). */
    for (size_t i = 0; i < cp_offset && unit_pos < total_units; i++) {
        const uint16_t u1 = c_read_u16(string + unit_pos * 2);
        unit_pos++;
        if (u1 >= 0xD800u && u1 <= 0xDBFFu && unit_pos < total_units) {
            const uint16_t u2 = c_read_u16(string + unit_pos * 2);
            if (u2 >= 0xDC00u && u2 <= 0xDFFFu) unit_pos++;
        }
    }

    if (unit_pos >= total_units && cp_offset > 0) return s;
    const size_t start_unit = unit_pos;

    /* Collect cp_count codepoints. */
    for (size_t i = 0; i < cp_count && unit_pos < total_units; i++) {
        const uint16_t u1 = c_read_u16(string + unit_pos * 2);
        unit_pos++;
        if (u1 >= 0xD800u && u1 <= 0xDBFFu && unit_pos < total_units) {
            const uint16_t u2 = c_read_u16(string + unit_pos * 2);
            if (u2 >= 0xDC00u && u2 <= 0xDFFFu) unit_pos++;
        }
    }

    s.data   = string + start_unit * 2;
    s.length = (unit_pos - start_unit) * 2;
    return s;
}

/*
 * c_subslice_codepoints  –  Universal encoding dispatcher.
 * Delegates to the encoding-specific function selected by the string's tag, then
 * repackages the result as a plain c_slice_t for callers that don't care about
 * the encoding distinction.
 */
c_slice_t c_subslice_codepoints(const c_const_string_t string,
                                 const size_t cp_offset, const size_t cp_count)
{
    c_slice_t result = { NULL, 0 };
    if (!string) return result;

    const unsigned char enc = c_string_get_encoding(string);

    if (enc == C_STRING_ENC_UTF16) {
        const c_utf16_slice_t u16 =
            c_utf16_subslice_codepoints(string, cp_offset, cp_count);
        result.data   = u16.data;
        result.length = u16.length;
        return result;
    }

    if (enc == C_STRING_ENC_UTF8) {
        const c_utf8_slice_t u8 =
            c_utf8_subslice_codepoints(string, cp_offset, cp_count);
        result.data   = u8.data;
        result.length = u8.length;
        return result;
    }

    /* ASCII / raw: each byte is one codepoint – direct pointer arithmetic. */
    const size_t total = c_string_get_used_length(string);
    if (cp_offset >= total) return result;

    const size_t remaining = total - cp_offset;
    result.data   = string + cp_offset;
    result.length = cp_count > remaining ? remaining : cp_count;
    return result;
}

size_t c_utf16_slice_unit_count(const c_utf16_slice_t slice)
{
    return slice.length / 2;
}

/*─────────────────────────────────────────────────────────────────────────────────
  STRING LIFECYCLE & CONSTRUCTORS
─────────────────────────────────────────────────────────────────────────────────*/

c_string_t c_string_create_pro(const void *const initial_data,
                                size_t initial_length,
                                const unsigned char encoding)
{
    /* UTF-16 lengths must be even (each code unit is 2 bytes). */
    if ((encoding & C_STRING_ENC_MASK) == C_STRING_ENC_UTF16)
        initial_length &= ~(size_t)1u;

    const unsigned char type        = c_string_determine_type(initial_length);
    const size_t        header_size = c_string_get_header_size(type);
    const size_t        null_bytes  = null_size_for(encoding);

    /* Guard against arithmetic overflow in the total allocation size. */
    if (initial_length > SIZE_MAX - header_size - null_bytes)
        return NULL;

    uint8_t *const memory = (uint8_t *)C_STRING_MALLOC(
        header_size + initial_length + null_bytes);
    if (!memory) return NULL;

    c_string_t string = memory + header_size;

    /*
     * Pack type, static flag (0 = dynamic), and encoding into type_flags.
     * The IS_STATIC bit is 0 here; c_string_create_static_pro sets it.
     */
    string[-1] = (unsigned char)(type | (encoding & C_STRING_ENC_MASK));

    if (initial_data) {
        memcpy(string, initial_data, initial_length);
    } else if (initial_length > 0) {
        memset(string, 0, initial_length);
    }

    c_string_set_lengths(string, initial_length, initial_length);

    /* Write NUL terminator(s) immediately after the data. */
    string[initial_length] = '\0';
    if (null_bytes == 2) string[initial_length + 1] = '\0';

    return string;
}

c_string_t c_string_create_static_pro(const void *const initial_data,
                                       size_t initial_length,
                                       size_t total_capacity,
                                       const unsigned char encoding)
{
    if ((encoding & C_STRING_ENC_MASK) == C_STRING_ENC_UTF16) {
        initial_length &= ~(size_t)1u;
        total_capacity &= ~(size_t)1u;
    }

    if (initial_length > total_capacity) return NULL;

    const unsigned char type        = c_string_determine_type(total_capacity);
    const size_t        header_size = c_string_get_header_size(type);
    const size_t        null_bytes  = null_size_for(encoding);

    if (total_capacity > SIZE_MAX - header_size - null_bytes)
        return NULL;

    uint8_t *const memory = (uint8_t *)C_STRING_MALLOC(
        header_size + total_capacity + null_bytes);
    if (!memory) return NULL;

    c_string_t string = memory + header_size;

    /* OR in the IS_STATIC flag alongside the type and encoding bits. */
    string[-1] = (unsigned char)(type | C_STRING_IS_STATIC | (encoding & C_STRING_ENC_MASK));

    if (initial_data) {
        memcpy(string, initial_data, initial_length);
    } else if (initial_length > 0) {
        memset(string, 0, initial_length);
    }

    c_string_set_lengths(string, initial_length, total_capacity);

    string[initial_length] = '\0';
    if (null_bytes == 2) string[initial_length + 1] = '\0';

    return string;
}

c_string_t c_string_create(const char *const string_parameter)
{
    return c_string_create_pro(
        string_parameter,
        string_parameter ? strlen(string_parameter) : 0,
        C_STRING_ENC_ASCII);
}

c_string_t c_string_create_static(const char *const string_parameter,
                                   const size_t extra_capacity)
{
    const size_t len = string_parameter ? strlen(string_parameter) : 0;
    if (len > SIZE_MAX - extra_capacity) return NULL;
    return c_string_create_static_pro(
        string_parameter, len, len + extra_capacity, C_STRING_ENC_ASCII);
}

void c_string_free(c_string_t string)
{
    if (!string) return;
    /* Walk back past the header to the original malloc'd base pointer. */
    C_STRING_FREE((uint8_t *)string - c_string_get_header_size(string[-1]));
}

c_string_t c_string_duplicate(const c_const_string_t string)
{
    if (!string) return NULL;
    /*
     * The duplicate is always dynamic.  Static strings are intentionally
     * not preserved here: the copy can grow freely, which is the safer default.
     */
    return c_string_create_pro(
        string,
        c_string_get_used_length(string),
        c_string_get_encoding(string));
}

void c_string_clear(c_string_t string)
{
    if (!string) return;
    const unsigned char enc       = c_string_get_encoding(string);
    const size_t        null_bytes = null_size_for(enc);
    c_string_set_used_length(string, 0);
    string[0] = '\0';
    if (null_bytes == 2) string[1] = '\0';
}

bool c_string_is_empty(const c_const_string_t string)
{
    return !string || c_string_get_used_length(string) == 0;
}

/*─────────────────────────────────────────────────────────────────────────────────
  SLICE CONSTRUCTORS
─────────────────────────────────────────────────────────────────────────────────*/

c_string_t c_string_from_slice(const c_slice_t slice)
{
    return c_string_create_pro(slice.data, slice.length, C_STRING_ENC_ASCII);
}

c_string_t c_string_from_utf8_slice(const c_utf8_slice_t slice)
{
    return c_string_create_pro(slice.data, slice.length, C_STRING_ENC_UTF8);
}

c_string_t c_string_from_utf16_slice(const c_utf16_slice_t slice)
{
    /* Round down to even to ensure valid UTF-16 byte alignment. */
    const size_t byte_len = slice.length & ~(size_t)1u;
    return c_string_create_pro(slice.data, byte_len, C_STRING_ENC_UTF16);
}

c_string_t c_string_create_from_utf16(const uint16_t *const units,
                                       const size_t unit_count)
{
    if (!units && unit_count > 0) return NULL;
    if (unit_count > SIZE_MAX / 2) return NULL; /* overflow guard */
    return c_string_create_pro(units, unit_count * 2u, C_STRING_ENC_UTF16);
}

/*─────────────────────────────────────────────────────────────────────────────────
  CAPACITY MANAGEMENT
─────────────────────────────────────────────────────────────────────────────────*/

c_string_t c_string_ensure_capacity(c_string_t string, const size_t additional_bytes)
{
    if (!string) return NULL;
    if (c_string_get_available_capacity(string) >= additional_bytes) return string;

    /* Static strings have fixed capacity; cannot reallocate. */
    if (string[-1] & C_STRING_IS_STATIC) return string;

    const size_t        current_length   = c_string_get_used_length(string);
    const unsigned char encoding         = c_string_get_encoding(string);
    const size_t        null_bytes       = null_size_for(encoding);
    const size_t        current_capacity =
        current_length + c_string_get_available_capacity(string);

    /* Check that the needed size does not overflow size_t. */
    if (additional_bytes > SIZE_MAX - current_length) return string;

    size_t needed = current_length + additional_bytes;

    /* Preserve the UTF-16 even-byte invariant in the computed growth target. */
    if (encoding == C_STRING_ENC_UTF16)
        needed = (needed + 1u) & ~(size_t)1u;

    /*
     * Growth policy:
     *   · Below 1 MB: double the current capacity (amortised O(1) appends).
     *   · At or above 1 MB: add 1 MB per step (avoids huge over-allocations).
     */
    size_t new_capacity = needed;
    if (new_capacity < C_STRING_ONE_MEGABYTE) {
        const size_t doubled = (current_capacity > SIZE_MAX / 2)
                               ? SIZE_MAX
                               : current_capacity * 2u;
        if (doubled > new_capacity) new_capacity = doubled;
    } else {
        if (SIZE_MAX - C_STRING_ONE_MEGABYTE >= new_capacity)
            new_capacity += C_STRING_ONE_MEGABYTE;
        else
            new_capacity = SIZE_MAX;
    }

    unsigned char new_type        = c_string_determine_type(new_capacity);
    size_t        new_header_size = c_string_get_header_size(new_type);

    /* Final overflow clamp: ensure header + data + NUL fits in size_t. */
    if (new_capacity > SIZE_MAX - new_header_size - null_bytes) {
        new_capacity = SIZE_MAX - new_header_size - null_bytes;

        if (encoding == C_STRING_ENC_UTF16)
            new_capacity &= ~(size_t)1u;

        /* Recompute type and header size after the clamp, then verify. */
        new_type        = c_string_determine_type(new_capacity);
        new_header_size = c_string_get_header_size(new_type);
        if (new_capacity > SIZE_MAX - new_header_size - null_bytes ||
            new_capacity < needed)
        {
            return string; /* completely unserviceable – leave string intact */
        }
    }

    const unsigned char current_type        = (unsigned char)(string[-1] & C_STRING_TYPE_MASK);
    const size_t        current_header_size = c_string_get_header_size(current_type);
    uint8_t *new_memory;

    if (current_type == new_type) {
        /*
         * Header class is unchanged – realloc in-place.
         * The header content (type_flags, used_length, etc.) is preserved by
         * realloc; c_string_set_lengths below updates allocated_capacity.
         */
        new_memory = (uint8_t *)C_STRING_REALLOC(
            string - current_header_size,
            new_header_size + new_capacity + null_bytes);
        if (!new_memory) return string;
        string = new_memory + new_header_size;
    } else {
        /*
         * Header class grew to accommodate the new capacity.
         * malloc a new block, copy the data, then free the old block.
         * The IS_STATIC bit is intentionally NOT set here (we've already
         * confirmed above that static strings cannot reach this path).
         */
        new_memory = (uint8_t *)C_STRING_MALLOC(
            new_header_size + new_capacity + null_bytes);
        if (!new_memory) return string;
        memcpy(new_memory + new_header_size, string, current_length + null_bytes);
        C_STRING_FREE(string - current_header_size);
        string     = new_memory + new_header_size;
        string[-1] = (unsigned char)(new_type | encoding); /* IS_STATIC bit = 0 */
    }

    c_string_set_lengths(string, current_length, new_capacity);
    return string;
}

c_string_t c_string_reserve(c_string_t string, const size_t additional_bytes)
{
    return c_string_ensure_capacity(string, additional_bytes);
}

c_string_t c_string_shrink_to_fit(c_string_t string)
{
    if (!string) return NULL;
    if (string[-1] & C_STRING_IS_STATIC) return string; /* no-op for static strings */

    const unsigned char type        = (unsigned char)(string[-1] & C_STRING_TYPE_MASK);
    const unsigned char enc         = c_string_get_encoding(string);
    const size_t        used        = c_string_get_used_length(string);
    const size_t        header_size = c_string_get_header_size(type);
    const size_t        null_bytes  = null_size_for(enc);

    /* Determine the smallest header class that can still represent `used`. */
    const unsigned char new_type        = c_string_determine_type(used);
    const size_t        new_header_size = c_string_get_header_size(new_type);

    if (new_type == type) {
        /* Same header class: realloc to trim the tail. */
        uint8_t *const new_memory = (uint8_t *)C_STRING_REALLOC(
            string - header_size,
            header_size + used + null_bytes);
        if (!new_memory) return string;
        string = new_memory + header_size;
    } else {
        /* Header class shrank: malloc a smaller block, copy, free the old one. */
        uint8_t *const new_memory = (uint8_t *)C_STRING_MALLOC(
            new_header_size + used + null_bytes);
        if (!new_memory) return string;
        memcpy(new_memory + new_header_size, string, used + null_bytes);
        C_STRING_FREE(string - header_size);
        string     = new_memory + new_header_size;
        string[-1] = (unsigned char)(new_type | enc); /* IS_STATIC bit = 0 */
    }

    c_string_set_lengths(string, used, used);

    /* Rewrite the NUL terminator(s) in case the block moved or was trimmed. */
    string[used] = '\0';
    if (null_bytes == 2) string[used + 1] = '\0';

    return string;
}

void c_string_array_shrink_to_fit(c_string_t *const array, const size_t count)
{
    if (!array || count == 0) return;
    for (size_t i = 0; i < count; i++) {
        if (array[i]) array[i] = c_string_shrink_to_fit(array[i]);
    }
}

/*─────────────────────────────────────────────────────────────────────────────────
  APPENDING & CONCATENATION
─────────────────────────────────────────────────────────────────────────────────*/

c_string_t c_string_append_pro(c_string_t string,
                                const void *const data,
                                size_t length)
{
    if (!string || !data || length == 0) return string;

    const size_t        current_length = c_string_get_used_length(string);
    const unsigned char encoding       = c_string_get_encoding(string);
    const size_t        null_bytes     = null_size_for(encoding);

    /* UTF-16: discard the last byte if an odd count was passed. */
    if (encoding == C_STRING_ENC_UTF16) {
        length &= ~(size_t)1u;
        if (length == 0) return string;
    }

    if (length > SIZE_MAX - current_length) return string; /* overflow guard */

    /*
     * Self-overlap detection.
     *
     * If `data` points into the same character buffer we're about to grow,
     * a reallocation that moves the buffer would leave `data` dangling.
     * We detect this BEFORE the potential realloc by recording `data`'s byte
     * offset relative to string[0].  After the realloc the offset is still
     * valid and we can reconstruct the new absolute pointer.
     *
     * buf_start is string[0] (not the header base): we only need to detect
     * overlaps into the live character region.  Appending from within the
     * header bytes is a programming error and not supported.
     */
    const uintptr_t src_start  = (uintptr_t)data;
    const uintptr_t buf_start  = (uintptr_t)string;
    const uintptr_t buf_end    = (uintptr_t)(string + current_length + null_bytes);
    const bool      is_overlap = (src_start >= buf_start && src_start < buf_end);
    const ptrdiff_t src_offset = is_overlap
                                 ? (ptrdiff_t)((const uint8_t *)data - string)
                                 : 0;

    /*
     * Grow the buffer.  On failure, c_string_ensure_capacity returns the original
     * pointer unmodified (invariant: available_capacity is then still < length).
     */
    string = c_string_ensure_capacity(string, length);
    if (c_string_get_available_capacity(string) < length) {
        /*
         * Could not grow (OOM or static string with full capacity).
         * Return the original pointer – no data was lost or modified.
         * Note: ensure_capacity returns the original on failure, so `string`
         * here is still valid even if the header class could not change.
         */
        return string;
    }

    /* If the buffer moved, rebase the source pointer using the saved offset. */
    const void *const src = is_overlap ? (const void *)(string + src_offset) : data;
    memmove(string + current_length, src, length);

    const size_t new_length = current_length + length;
    c_string_set_used_length(string, new_length);
    string[new_length] = '\0';
    if (null_bytes == 2) string[new_length + 1] = '\0';

    return string;
}

c_string_t c_string_append(c_string_t string, const char *const string_parameter)
{
    return c_string_append_pro(
        string,
        string_parameter,
        string_parameter ? strlen(string_parameter) : 0);
}

c_string_t c_string_append_slice(c_string_t string, const c_slice_t slice)
{
    if (!slice.data || slice.length == 0) return string;
    return c_string_append_pro(string, slice.data, slice.length);
}

c_string_t c_string_append_utf8(c_string_t string, const c_utf8_slice_t slice)
{
    if (!slice.data || slice.length == 0) return string;
    /* Auto-upgrade: if the destination is ASCII-only, retag it as UTF-8. */
    if (c_string_get_encoding(string) == C_STRING_ENC_ASCII)
        c_string_set_encoding(string, C_STRING_ENC_UTF8);
    return c_string_append_pro(string, slice.data, slice.length);
}

c_string_t c_string_append_utf16(c_string_t string, const c_utf16_slice_t slice)
{
    if (!slice.data || slice.length == 0) return string;
    /* Round down to even before delegating. */
    const size_t byte_len = slice.length & ~(size_t)1u;
    if (byte_len == 0) return string;
    return c_string_append_pro(string, slice.data, byte_len);
}

c_string_t c_string_concat(const c_const_string_t a, const c_const_string_t b)
{
    const unsigned char enc_a = a ? c_string_get_encoding(a) : C_STRING_ENC_ASCII;
    const size_t la = a ? c_string_get_used_length(a) : 0;
    const size_t lb = b ? c_string_get_used_length(b) : 0;

    if (la > SIZE_MAX - lb) return NULL; /* overflow guard */

    /* Start with a copy of `a` (or an empty string if a is NULL). */
    c_string_t result = c_string_create_pro(a, la, enc_a);
    if (!result) return NULL;

    if (lb > 0) {
        const size_t before = c_string_get_used_length(result);
        result = c_string_append_pro(result, b, lb);
        if (c_string_get_used_length(result) == before) {
            /* Append failed (OOM); free and propagate the failure. */
            c_string_free(result);
            return NULL;
        }
    }
    return result;
}

/*─────────────────────────────────────────────────────────────────────────────────
  ENCODING CONVERTERS – UTF-8 NORMALISATION  (via utf8proc)
─────────────────────────────────────────────────────────────────────────────────*/

c_string_t c_string_create_from_utf8_pro(const char *const text,
                                          const utf8proc_option_t options)
{
    if (!text) return NULL;

    utf8proc_uint8_t *destination = NULL;

    /*
     * utf8proc_map with UTF8PROC_NULLTERM: the `strlen` argument (0 here) is
     * ignored when NULLTERM is set – utf8proc scans for NUL internally.
     * We always OR in UTF8PROC_NULLTERM regardless of what the caller passed,
     * because `text` is required to be NUL-terminated.
     */
    const utf8proc_ssize_t length = utf8proc_map(
        (const utf8proc_uint8_t *)text,
        0,
        &destination,
        options | UTF8PROC_NULLTERM);

    if (length < 0) {
        /* utf8proc frees nothing on error; `destination` may be NULL, but
         * free(NULL) is always safe, so no special-casing is needed. */
        free(destination);
        return NULL;
    }

    c_string_t result = c_string_create_pro(destination, (size_t)length,
                                             C_STRING_ENC_UTF8);
    free(destination);
    return result;
}

c_string_t c_string_create_from_utf8(const char *const text)
{
    return c_string_create_from_utf8_pro(
        text,
        (utf8proc_option_t)(UTF8PROC_NULLTERM | UTF8PROC_STABLE | UTF8PROC_COMPOSE));
}

/*─────────────────────────────────────────────────────────────────────────────────
  ENCODING CONVERTERS – UTF-8 → UTF-16
─────────────────────────────────────────────────────────────────────────────────*/

c_string_t c_string_convert_utf8_utf16_pro(const c_const_string_t utf8_input,
                                            const bool use_big_endian)
{
    if (!utf8_input) return NULL;

    const size_t input_length = c_string_get_used_length(utf8_input);

    /* Start with an empty UTF-16 output string. */
    c_string_t output = c_string_create_pro(NULL, 0, C_STRING_ENC_UTF16);
    if (!output) return NULL;

    if (input_length > 0) {
        /*
         * Pre-allocate for the worst case: every UTF-8 byte produces one
         * UTF-16 code unit, i.e. a 2× byte expansion.  (Supplementary plane
         * characters produce two UTF-16 units from four UTF-8 bytes, so they
         * are also bounded by 2× at most.)
         */
        if (input_length > SIZE_MAX / 2) { c_string_free(output); return NULL; }

        output = c_string_ensure_capacity(output, input_length * 2u);
        if (c_string_get_available_capacity(output) < input_length * 2u) {
            c_string_free(output);
            return NULL;
        }
    }

    const bool swap = needs_swap(use_big_endian);
    size_t offset = 0;

    while (offset < input_length) {
        utf8proc_int32_t cp;
        const size_t rem = input_length - offset;
        const utf8proc_ssize_t bound =
            (utf8proc_ssize_t)(rem > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : rem);
        const utf8proc_ssize_t read_bytes =
            utf8proc_iterate(utf8_input + offset, bound, &cp);

        if (read_bytes <= 0) {
            /* Invalid UTF-8 sequence (or unexpected 0-advance): abort. */
            c_string_free(output);
            return NULL;
        }
        offset += (size_t)read_bytes;

        /* Lone surrogates (U+D800–U+DFFF) are not valid Unicode scalar values. */
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            c_string_free(output);
            return NULL;
        }

        uint16_t units[2];
        int      unit_count = 0;

        if (cp < 0x10000) {
            units[unit_count++] = (uint16_t)cp;
        } else {
            /* Encode as a surrogate pair (cp – 0x10000 split into 10-bit halves). */
            const int32_t adj = cp - 0x10000;
            units[unit_count++] = (uint16_t)(0xD800u + (uint32_t)(adj >> 10));
            units[unit_count++] = (uint16_t)(0xDC00u + (uint32_t)(adj & 0x3FFu));
        }

        if (swap) {
            for (int i = 0; i < unit_count; i++) units[i] = bswap16(units[i]);
        }

        const size_t before = c_string_get_used_length(output);
        output = c_string_append_pro(output, units, (size_t)unit_count * 2u);
        if (c_string_get_used_length(output) == before) {
            c_string_free(output);
            return NULL;
        }
    }

    return c_string_shrink_to_fit(output);
}

c_string_t c_string_convert_utf8_utf16(const c_const_string_t utf8_input)
{
    return c_string_convert_utf8_utf16_pro(utf8_input, false);
}

/*─────────────────────────────────────────────────────────────────────────────────
  ENCODING CONVERTERS – UTF-16 → UTF-8
─────────────────────────────────────────────────────────────────────────────────*/

c_string_t c_string_convert_utf16_utf8_pro(const c_const_string_t utf16_input,
                                            const bool use_big_endian)
{
    if (!utf16_input) return NULL;

    const size_t input_units = c_string_get_used_length(utf16_input) / 2;

    c_string_t output = c_string_create_pro(NULL, 0, C_STRING_ENC_UTF8);
    if (!output) return NULL;

    const bool swap = needs_swap(use_big_endian);

    for (size_t i = 0; i < input_units; ) {
        uint16_t u1 = c_read_u16(utf16_input + i * 2);
        i++;
        if (swap) u1 = bswap16(u1);

        utf8proc_int32_t cp;

        if (u1 >= 0xD800u && u1 <= 0xDBFFu) {
            /* High surrogate: expect a low surrogate to follow. */
            if (i < input_units) {
                uint16_t u2 = c_read_u16(utf16_input + i * 2);
                if (swap) u2 = bswap16(u2);

                if (u2 >= 0xDC00u && u2 <= 0xDFFFu) {
                    /* Valid surrogate pair: decode to the supplementary codepoint. */
                    cp = (utf8proc_int32_t)(0x10000u +
                         (((uint32_t)(u1 - 0xD800u) << 10) | (uint32_t)(u2 - 0xDC00u)));
                    i++;
                } else {
                    cp = 0xFFFD; /* lone high surrogate */
                }
            } else {
                cp = 0xFFFD; /* high surrogate at end of stream */
            }
        } else if (u1 >= 0xDC00u && u1 <= 0xDFFFu) {
            cp = 0xFFFD; /* unexpected low surrogate without a preceding high one */
        } else {
            cp = (utf8proc_int32_t)u1;
        }

        uint8_t utf8_buf[4];
        const utf8proc_ssize_t written = utf8proc_encode_char(cp, utf8_buf);
        if (written > 0) {
            const size_t before = c_string_get_used_length(output);
            output = c_string_append_pro(output, utf8_buf, (size_t)written);
            if (c_string_get_used_length(output) == before) {
                c_string_free(output);
                return NULL;
            }
        }
    }

    return c_string_shrink_to_fit(output);
}

c_string_t c_string_convert_utf16_utf8(const c_const_string_t utf16_input)
{
    return c_string_convert_utf16_utf8_pro(utf16_input, false);
}

/*─────────────────────────────────────────────────────────────────────────────────
  STATIC → DYNAMIC CONVERTER
─────────────────────────────────────────────────────────────────────────────────*/

c_string_t c_string_to_dynamic(c_string_t string)
{
    if (!string) return NULL;
    if (!(string[-1] & C_STRING_IS_STATIC)) return string; /* already dynamic */

    const size_t        len = c_string_get_used_length(string);
    const unsigned char enc = c_string_get_encoding(string);

    c_string_t dyn = c_string_create_pro(string, len, enc);
    if (!dyn) return string; /* fallback: return original on allocation failure */

    c_string_free(string);
    return dyn;
}

/*─────────────────────────────────────────────────────────────────────────────────
  UTF-16 STDOUT PRINTING
─────────────────────────────────────────────────────────────────────────────────*/

void c_string_print_utf16(const c_utf16_slice_t slice)
{
    if (!slice.data || slice.length == 0) return;

    /* Convert the UTF-16 slice to a temporary UTF-8 string, then write it. */
    c_string_t tmp_utf16 = c_string_from_utf16_slice(slice);
    if (!tmp_utf16) return;

    c_string_t tmp_utf8 = c_string_convert_utf16_utf8(tmp_utf16);
    c_string_free(tmp_utf16);

    if (tmp_utf8) {
        fwrite(tmp_utf8, 1, c_string_get_used_length(tmp_utf8), stdout);
        c_string_free(tmp_utf8);
    }
}

/*─────────────────────────────────────────────────────────────────────────────────
  CASE CONVERSION  (UTF-8 / ASCII only)
─────────────────────────────────────────────────────────────────────────────────*/

c_string_t c_string_to_lower(const c_const_string_t string)
{
    if (!string) return NULL;

    const size_t        input_len = c_string_get_used_length(string);
    const unsigned char enc       = c_string_get_encoding(string);

    /* Case conversion is only defined for byte-encoded (non-UTF-16) strings. */
    if (enc == C_STRING_ENC_UTF16) return NULL;

    /*
     * Guard the cast: utf8proc_map takes a signed length parameter (utf8proc_ssize_t).
     * If input_len exceeds SSIZE_MAX the cast would produce a negative value, which
     * utf8proc would misinterpret as a "read until NUL" sentinel.
     */
    if (input_len > (size_t)SSIZE_MAX) return NULL;

    utf8proc_uint8_t *dest = NULL;

    /*
     * UTF8PROC_CASEFOLD maps each codepoint to its Unicode case-fold form
     * (locale-independent, full lowercase), then STABLE | COMPOSE normalise
     * the result to NFC.  We pass the explicit byte count (no NULLTERM needed).
     */
    const utf8proc_ssize_t len = utf8proc_map(
        (const utf8proc_uint8_t *)string,
        (utf8proc_ssize_t)input_len,
        &dest,
        (utf8proc_option_t)(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD));

    if (len < 0) { free(dest); return NULL; }
    c_string_t result = c_string_create_pro(dest, (size_t)len, C_STRING_ENC_UTF8);
    free(dest);
    return result;
}

/* Per-codepoint toupper callback for utf8proc_map_custom. */
static utf8proc_int32_t _toupper_cb(utf8proc_int32_t cp, void *data)
{
    (void)data;
    return utf8proc_toupper(cp);
}

c_string_t c_string_to_upper(const c_const_string_t string)
{
    if (!string) return NULL;

    const size_t        input_len = c_string_get_used_length(string);
    const unsigned char enc       = c_string_get_encoding(string);

    if (enc == C_STRING_ENC_UTF16) return NULL;

    /* Same signed-length guard as c_string_to_lower(). */
    if (input_len > (size_t)SSIZE_MAX) return NULL;

    utf8proc_uint8_t *dest = NULL;

    /*
     * utf8proc_map_custom applies _toupper_cb to each codepoint before
     * normalisation.  STABLE | COMPOSE produce NFC output.
     */
    const utf8proc_ssize_t len = utf8proc_map_custom(
        (const utf8proc_uint8_t *)string,
        (utf8proc_ssize_t)input_len,
        &dest,
        (utf8proc_option_t)(UTF8PROC_STABLE | UTF8PROC_COMPOSE),
        _toupper_cb, NULL);

    if (len < 0) { free(dest); return NULL; }
    c_string_t result = c_string_create_pro(dest, (size_t)len, C_STRING_ENC_UTF8);
    free(dest);
    return result;
}

/*─────────────────────────────────────────────────────────────────────────────────
  SEARCHING & COMPARISON
─────────────────────────────────────────────────────────────────────────────────*/

int c_string_compare(const c_const_string_t a, const c_const_string_t b)
{
    if (a == b) return 0;  /* identity fast-path (includes NULL == NULL) */
    if (!a)     return -1; /* NULL sorts before any non-NULL string */
    if (!b)     return  1;

    const size_t la  = c_string_get_used_length(a);
    const size_t lb  = c_string_get_used_length(b);
    const size_t min = la < lb ? la : lb;

    const int cmp = memcmp(a, b, min);
    if (cmp != 0) return cmp;
    /* Common prefix is identical: shorter string sorts first. */
    if (la < lb) return -1;
    if (la > lb) return  1;
    return 0;
}

bool c_string_equal(const c_const_string_t a, const c_const_string_t b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    const size_t la = c_string_get_used_length(a);
    if (la != c_string_get_used_length(b)) return false;
    return memcmp(a, b, la) == 0;
}

size_t c_string_find(const c_const_string_t haystack, const c_const_string_t needle)
{
    if (!haystack || !needle) return SIZE_MAX;

    const size_t hlen = c_string_get_used_length(haystack);
    const size_t nlen = c_string_get_used_length(needle);

    if (nlen == 0)    return 0;          /* empty needle always matches at offset 0 */
    if (nlen > hlen)  return SIZE_MAX;   /* needle is longer than haystack */

    /* Naive O(n·m) search; suitable for most real-world string lengths. */
    const size_t limit = hlen - nlen;
    for (size_t i = 0; i <= limit; i++) {
        if (memcmp(haystack + i, needle, nlen) == 0) return i;
    }
    return SIZE_MAX;
}

bool c_string_contains(const c_const_string_t str, const c_const_string_t needle)
{
    return c_string_find(str, needle) != SIZE_MAX;
}

bool c_string_starts_with(const c_const_string_t str, const c_const_string_t prefix)
{
    if (!str || !prefix) return false;
    const size_t plen = c_string_get_used_length(prefix);
    if (plen == 0) return true;
    const size_t slen = c_string_get_used_length(str);
    if (plen > slen) return false;
    return memcmp(str, prefix, plen) == 0;
}

bool c_string_ends_with(const c_const_string_t str, const c_const_string_t suffix)
{
    if (!str || !suffix) return false;
    const size_t sflen = c_string_get_used_length(suffix);
    if (sflen == 0) return true;
    const size_t slen = c_string_get_used_length(str);
    if (sflen > slen) return false;
    return memcmp(str + slen - sflen, suffix, sflen) == 0;
}

/*─────────────────────────────────────────────────────────────────────────────────
  IN-PLACE MUTATION  (trim, repeat)
─────────────────────────────────────────────────────────────────────────────────*/

/*
 * is_utf16le_whitespace_at  –  True when the UTF-16 LE code unit starting at
 * `byte_off` in `buf` is one of the four ASCII-range whitespace characters.
 * Only BMP whitespace (space, tab, LF, CR) is checked here; Unicode-category
 * space characters like NBSP or ideographic space are left as-is.
 */
static inline bool is_utf16le_whitespace_at(const uint8_t *buf, size_t byte_off)
{
    const uint16_t u = c_read_u16(buf + byte_off);
    return u == 0x0020u   /* SPACE            */
        || u == 0x0009u   /* CHARACTER TABULATION */
        || u == 0x000Au   /* LINE FEED        */
        || u == 0x000Du;  /* CARRIAGE RETURN  */
}

c_string_t c_string_trim_right(c_string_t string)
{
    if (!string) return NULL;

    const unsigned char enc        = c_string_get_encoding(string);
    const size_t        null_bytes = null_size_for(enc);
    size_t len = c_string_get_used_length(string);

    if (enc == C_STRING_ENC_UTF16) {
        /*
         * Walk backwards in 2-byte steps.
         * The condition `len >= 2` guards against wrapping to SIZE_MAX on
         * an empty string (len == 0).
         */
        while (len >= 2 && is_utf16le_whitespace_at(string, len - 2))
            len -= 2;
    } else {
        while (len > 0) {
            const uint8_t c = string[len - 1];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') len--;
            else break;
        }
    }

    c_string_set_used_length(string, len);
    string[len] = '\0';
    if (null_bytes == 2) string[len + 1] = '\0';
    return string;
}

c_string_t c_string_trim_left(c_string_t string)
{
    if (!string) return NULL;

    const unsigned char enc        = c_string_get_encoding(string);
    const size_t        null_bytes = null_size_for(enc);
    const size_t        len        = c_string_get_used_length(string);
    size_t start = 0;

    if (enc == C_STRING_ENC_UTF16) {
        /*
         * `start + 2 <= len` is equivalent to `start + 1 < len` for even
         * values of `start`, which is always the case here since we step by 2.
         * Written as `start + 1 < len` to avoid a potential size_t wrap when
         * `len == 0` (though that is guarded by the outer check as well).
         */
        while (start + 1 < len && is_utf16le_whitespace_at(string, start))
            start += 2;
    } else {
        while (start < len) {
            const uint8_t c = string[start];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') start++;
            else break;
        }
    }

    if (start > 0) {
        const size_t new_len = len - start;
        memmove(string, string + start, new_len);
        c_string_set_used_length(string, new_len);
        string[new_len] = '\0';
        if (null_bytes == 2) string[new_len + 1] = '\0';
    }
    return string;
}

c_string_t c_string_trim(c_string_t string)
{
    return c_string_trim_right(c_string_trim_left(string));
}

c_string_t c_string_repeat(const c_const_string_t string, const size_t count)
{
    const unsigned char enc = string
                              ? c_string_get_encoding(string)
                              : C_STRING_ENC_ASCII;

    /* Edge cases: NULL input or zero repeat count → return an empty string. */
    if (!string || count == 0)
        return c_string_create_pro(NULL, 0, enc);

    const size_t unit_len = c_string_get_used_length(string);
    if (unit_len == 0)
        return c_string_create_pro(NULL, 0, enc);

    /* Overflow check: unit_len × count must not exceed SIZE_MAX. */
    if (unit_len > SIZE_MAX / count) return NULL;

    const size_t total = unit_len * count;

    /*
     * Allocate a zero-filled buffer of `total` bytes with the correct encoding
     * and NUL terminator(s), then overwrite with repeated copies of the source.
     */
    c_string_t result = c_string_create_pro(NULL, total, enc);
    if (!result) return NULL;

    for (size_t i = 0; i < count; i++)
        memcpy(result + i * unit_len, string, unit_len);

    /* NUL terminator(s) were already written by c_string_create_pro. */
    return result;
}

/*─────────────────────────────────────────────────────────────────────────────────
  VALIDATION
─────────────────────────────────────────────────────────────────────────────────*/

bool c_string_validate_utf8(const c_const_string_t string)
{
    if (!string) return true; /* NULL is vacuously valid */

    const size_t total = c_string_get_used_length(string);
    size_t pos = 0;

    while (pos < total) {
        utf8proc_int32_t cp;
        const size_t rem = total - pos;
        const utf8proc_ssize_t bound =
            (utf8proc_ssize_t)(rem > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : rem);

        /*
         * utf8proc_iterate returns a positive advance count on success and a
         * negative error code on failure.  A return of <= 0 (which includes
         * the adv == 0 edge case) is treated as invalid.
         * Note: checking `adv <= 0` is sufficient; the `cp < 0` test would
         * be redundant because utf8proc always sets cp = -1 on error.
         */
        const utf8proc_ssize_t adv =
            utf8proc_iterate((const utf8proc_uint8_t *)(string + pos), bound, &cp);
        if (adv <= 0) return false;

        pos += (size_t)adv;
    }
    return true;
}
