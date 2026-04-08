/*******************************************************************************************
*
* convey v1.2  –  Variable-width dynamic/static string library with first-class
*                 Unicode encoding support  (C11 required)
*
* DEPENDENCIES
*   utf8proc.h  – UTF-8 processing, normalisation, codepoint iteration
*   <stdlib.h>  – malloc / realloc / free
*   <string.h>  – memcpy / memmove / memcmp / strlen
*   <stdint.h>  – uint8_t, uint16_t, uint32_t, uint64_t, SIZE_MAX
*   <stdbool.h> – bool
*   <wchar.h>   – wchar_t (UTF-16 print helpers on Windows)
*
* ── MEMORY LAYOUT ──────────────────────────────────────────────────────────────────
*
*   Each allocation looks like this in memory:
*
*     [ header fields ... | type_flags ] [ ... character data ... ] [ NUL byte(s) ]
*                                        ^
*                          c_string_t points HERE  (string[0] is the first data byte)
*                          string[-1] is always the type_flags byte
*
*   The header sits immediately before the data pointer and is one of four sizes
*   depending on how large the string is:
*
*     Header class   Size fields   Max storable length
*     ─────────────  ────────────  ───────────────────
*     header_8       uint8_t × 2   255 bytes
*     header_16      uint16_t × 2  65 535 bytes
*     header_32      uint32_t × 2  4 294 967 295 bytes
*     header_64      uint64_t × 2  virtually unlimited
*
*   Both `used_length` and `allocated_capacity` are always in BYTES (never chars or
*   codepoints) and always exclude the null terminator(s).
*
* ── TYPE-FLAGS BYTE (string[-1]) ───────────────────────────────────────────────────
*
*   Bits 1–2  (C_STRING_TYPE_MASK  0x06)  header size class (TYPE_8/16/32/64)
*   Bit  0    (C_STRING_IS_STATIC  0x01)  1 = fixed-capacity static string
*   Bits 3–4  (C_STRING_ENC_MASK   0x18)  encoding tag
*
*   The three fields are non-overlapping so they can be freely OR'd together:
*
*     type_flags = TYPE_xx | IS_STATIC (if applicable) | ENC_xxx
*
* ── ENCODING TAGS ──────────────────────────────────────────────────────────────────
*
*   C_STRING_ENC_ASCII  (0x00)  Raw bytes / ASCII  – 1-byte NUL terminator
*   C_STRING_ENC_UTF8   (0x08)  UTF-8              – 1-byte NUL terminator
*   C_STRING_ENC_UTF16  (0x10)  UTF-16 LE          – 2-byte NUL terminator
*
*   Rules:
*   · c_string_create()           tags the result C_STRING_ENC_ASCII by default.
*   · c_string_create_from_utf8() NFC-normalises and tags C_STRING_ENC_UTF8.
*   · c_string_append_utf8()      auto-upgrades ASCII → UTF-8 on the destination.
*   · UTF-16 strings are ALWAYS double-NUL-terminated (two zero bytes).
*   · Use C_STRING_NULL_SIZE(enc) wherever a null-terminator byte count is needed
*     instead of hardcoding 1 or 2.
*
* ── PRINT FORMAT MACROS ────────────────────────────────────────────────────────────
*
*   Standard (for variables, slices, and C literals – double-evaluates the argument):
*
*     printf("Result: " C_STR_FMT "\n", C_str_arg(my_str));
*     printf("Name: " C_STR_FMT ", age: %d\n", C_str_arg(my_str), 25);
*
*   WARNING: C_str_arg(X) expands X twice.  Never pass a mutating function call.
*   Use C_PRINTF_SAFE when the argument is a function call:
*
*     C_PRINTF_SAFE("Trimmed: " C_STR_FMT "\n", c_string_trim(my_str));
*
*   UTF-16 on Windows (wchar_t == uint16_t):
*
*     wprintf(L"Result: " C_WSTR_FMT L"\n", C_wstr_arg(my_utf16_str));
*     C_WPRINTF_SAFE(L"Result: " C_WSTR_FMT L"\n", c_string_trim(my_utf16_str));
*
*   Cross-platform UTF-16 printing (converts to UTF-8 first):
*
*     c_string_print_utf16(C_UTF16_SLICE_OF(my_str));
*
* ── QUICK EXAMPLE ──────────────────────────────────────────────────────────────────
*
*   c_string_t s = c_string_create("hello");
*   s = c_string_append(s, " world");
*   printf("Message: " C_STR_FMT "\n", C_str_arg(s));  // Message: hello world
*   c_string_free(s);
*
********************************************************************************************
*
*   zlib/libpng license  –  Copyright (c) 2024 convey contributors
*
*******************************************************************************************/

#ifndef CONVEY_H
#define CONVEY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>   /* malloc / realloc / free */
#include <string.h>   /* memcpy / memmove / memcmp / strlen */
#include <wchar.h>    /* wchar_t – needed by the UTF-16 print helpers */
#include "utf8proc.h"

/*==================================================================================
  SECTION 1 – CUSTOM ALLOCATOR HOOKS
  Define before including this header to redirect all internal allocations.
==================================================================================*/
#ifndef C_STRING_MALLOC
#  define C_STRING_MALLOC(sz)        malloc(sz)
#endif
#ifndef C_STRING_REALLOC
#  define C_STRING_REALLOC(p, sz)    realloc((p), (sz))
#endif
#ifndef C_STRING_FREE
#  define C_STRING_FREE(p)           free(p)
#endif

/*==================================================================================
  SECTION 2 – TYPE-FLAGS BIT CONSTANTS
  These are packed into the single `type_flags` byte that lives at string[-1].
  Bits 1-2 = header size class.  Bit 0 = static flag.  Bits 3-4 = encoding.
==================================================================================*/

/* ── Header size class (bits 1-2) ─────────────────────────────────────────────── */
#define C_STRING_TYPE_8             0x00u   /* 0000 0000 – used/cap fields are uint8_t  */
#define C_STRING_TYPE_16            0x02u   /* 0000 0010 – used/cap fields are uint16_t */
#define C_STRING_TYPE_32            0x04u   /* 0000 0100 – used/cap fields are uint32_t */
#define C_STRING_TYPE_64            0x06u   /* 0000 0110 – used/cap fields are uint64_t */
#define C_STRING_TYPE_MASK          0x06u   /* mask to isolate bits 1-2                 */

/* ── Static flag (bit 0) ──────────────────────────────────────────────────────── */
#define C_STRING_IS_STATIC          0x01u   /* 1 = fixed-capacity, never reallocated    */

/* ── Encoding tag (bits 3-4) ──────────────────────────────────────────────────── */
#define C_STRING_ENC_ASCII          0x00u   /* 0000 0000 – raw bytes / ASCII            */
#define C_STRING_ENC_UTF8           0x08u   /* 0000 1000 – UTF-8                        */
#define C_STRING_ENC_UTF16          0x10u   /* 0001 0000 – UTF-16 LE (default for UTF-16) */
#define C_STRING_ENC_MASK           0x18u   /* mask to isolate bits 3-4                 */

/* ── Growth policy sentinel ───────────────────────────────────────────────────── */
/** Allocation growth switches from doubling to linear +1 MB chunks above this threshold. */
#define C_STRING_ONE_MEGABYTE       1048576u

/*==================================================================================
  SECTION 3 – TYPE DEFINITIONS  (must precede any macro or inline function that
               uses these types)
==================================================================================*/

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Mutable pointer to a convey string's character data.
 * The header lives immediately before this pointer: string[-1] is type_flags,
 * and the preceding bytes are the used_length and allocated_capacity fields.
 */
typedef uint8_t       *c_string_t;

/** Immutable variant of c_string_t; used for read-only parameters. */
typedef const uint8_t *c_const_string_t;

/** Generic byte-view slice (ASCII / raw bytes).  Zero-copy; does not own its data. */
typedef struct { const uint8_t *data; size_t length; } c_slice_t;

/** UTF-8 byte-view slice.  `length` is in bytes (not codepoints).  Zero-copy. */
typedef struct { const uint8_t *data; size_t length; } c_utf8_slice_t;

/**
 * UTF-16 byte-view slice.  `length` is ALWAYS in bytes (not code units) and is
 * always even.  Zero-copy; the underlying buffer is UTF-16 LE unless a
 * conversion function was told otherwise.
 */
typedef struct { const uint8_t *data; size_t length; } c_utf16_slice_t;

/*==================================================================================
  SECTION 4 – PACKED VARIABLE-WIDTH HEADERS
  Layout (packed, little-endian):
    [ used_length | allocated_capacity | type_flags ] [ ... data ... ] [ NUL(s) ]
  The type_flags byte is always the LAST byte of the header (string[-1]).
  used_length and allocated_capacity are in BYTES and exclude the NUL terminator(s).
==================================================================================*/

#if defined(_MSC_VER)
#  pragma pack(push, 1)
#  define C_STRING_PACKED
#elif defined(__GNUC__) || defined(__clang__)
#  define C_STRING_PACKED __attribute__((__packed__))
#else
#  define C_STRING_PACKED
#endif

struct C_STRING_PACKED c_string_header_8  {
    uint8_t  used_length;
    uint8_t  allocated_capacity;
    unsigned char type_flags;
    uint8_t  character_buffer[];
};
struct C_STRING_PACKED c_string_header_16 {
    uint16_t used_length;
    uint16_t allocated_capacity;
    unsigned char type_flags;
    uint8_t  character_buffer[];
};
struct C_STRING_PACKED c_string_header_32 {
    uint32_t used_length;
    uint32_t allocated_capacity;
    unsigned char type_flags;
    uint8_t  character_buffer[];
};
struct C_STRING_PACKED c_string_header_64 {
    uint64_t used_length;
    uint64_t allocated_capacity;
    unsigned char type_flags;
    uint8_t  character_buffer[];
};

#if defined(_MSC_VER)
#  pragma pack(pop)
#endif

/*==================================================================================
  SECTION 5 – PUBLIC API  (all function declarations)
  Ordered by logical group; implementations live in convey_main.c.
==================================================================================*/

/* ── Lifecycle & Constructors ─────────────────────────────────────────────────── */

/**
 * Create a new dynamic string from a NUL-terminated C string.
 * Tags the result C_STRING_ENC_ASCII.  Use c_string_create_from_utf8() when the
 * content contains multi-byte UTF-8 sequences.
 * Returns NULL on allocation failure or if string_parameter is NULL.
 */
c_string_t c_string_create(const char *string_parameter);

/**
 * Low-level constructor.  Copies `initial_length` bytes from `initial_data` into
 * a fresh allocation tagged with `encoding`.  Pass NULL for `initial_data` to get a
 * zero-filled buffer of `initial_length` bytes instead.
 * For UTF-16, `initial_length` must be in bytes; odd counts are rounded down.
 * Returns NULL on allocation failure or arithmetic overflow.
 */
c_string_t c_string_create_pro(const void *initial_data, size_t initial_length,
                                unsigned char encoding);

/**
 * Create a fixed-capacity (static) string.
 * Total allocated capacity = strlen(string_parameter) + extra_capacity.
 * Static strings never reallocate; append functions silently do nothing when the
 * capacity would be exceeded.
 * Returns NULL on arithmetic overflow or allocation failure.
 */
c_string_t c_string_create_static(const char *string_parameter, size_t extra_capacity);

/**
 * Low-level constructor for a static (fixed-capacity) string.
 * `total_capacity` is the absolute byte capacity to reserve (excluding header and NUL).
 * `initial_length` bytes are copied from `initial_data` (or zero-filled if NULL).
 * Fails if initial_length > total_capacity.
 */
c_string_t c_string_create_static_pro(const void *initial_data, size_t initial_length,
                                       size_t total_capacity, unsigned char encoding);

/** Free a dynamic or static string.  Safe to call with NULL (no-op). */
void c_string_free(c_string_t string);

/**
 * Deep-copy a string, preserving its encoding, content, and used-length.
 * The copy is always dynamic, even if the source is static.
 * Returns NULL on allocation failure.
 */
c_string_t c_string_duplicate(c_const_string_t string);

/** Reset the used-length to 0 and write the appropriate NUL terminator(s) in-place. */
void c_string_clear(c_string_t string);

/** Return true when `string` is NULL or has zero used bytes. */
bool c_string_is_empty(c_const_string_t string);

/* ── Slice Constructors ───────────────────────────────────────────────────────── */

/** Create a new ASCII-tagged dynamic string from a raw byte slice. */
c_string_t c_string_from_slice(c_slice_t slice);

/** Create a new UTF-8-tagged dynamic string from a UTF-8 byte slice. */
c_string_t c_string_from_utf8_slice(c_utf8_slice_t slice);

/**
 * Create a new UTF-16-tagged dynamic string from a UTF-16 byte slice.
 * `slice.length` must be in bytes; odd values are rounded down to even.
 * The result is double-NUL-terminated.
 */
c_string_t c_string_from_utf16_slice(c_utf16_slice_t slice);

/**
 * Create a new UTF-16-tagged dynamic string from an array of `unit_count`
 * uint16_t code units.  The result is double-NUL-terminated.
 * Returns NULL if units is NULL and unit_count > 0, or on overflow.
 */
c_string_t c_string_create_from_utf16(const uint16_t *units, size_t unit_count);

/* ── Appending & Concatenation ───────────────────────────────────────────────── */

/**
 * Append a NUL-terminated C string to `string`.
 *
 * The caller MUST reassign from the return value, as the pointer may change on
 * reallocation:
 *   str = c_string_append(str, " world");
 *
 * On allocation failure or when `string` is static with insufficient remaining
 * capacity, returns the original pointer unmodified (no data is lost).
 */
c_string_t c_string_append(c_string_t string, const char *string_parameter);

/**
 * Append `length` raw bytes from `data` to `string`.
 * Self-overlap is handled safely: appending a subslice of the same string is
 * correct even after a reallocation moves the buffer.
 * The caller MUST reassign from the return value.
 * Returns the original pointer unmodified on failure.
 */
c_string_t c_string_append_pro(c_string_t string, const void *data, size_t length);

/** Append a raw byte slice to `string`. */
c_string_t c_string_append_slice(c_string_t string, c_slice_t slice);

/**
 * Append a UTF-8 slice to `string`.
 * If the destination is ASCII-tagged it is automatically re-tagged UTF-8.
 */
c_string_t c_string_append_utf8(c_string_t string, c_utf8_slice_t slice);

/**
 * Append a UTF-16 slice (raw bytes) to `string`.
 * Odd byte lengths are rounded down to the nearest even value before appending.
 */
c_string_t c_string_append_utf16(c_string_t string, c_utf16_slice_t slice);

/**
 * Return a new dynamic string that is `a` concatenated with `b`.
 * The result inherits `a`'s encoding.
 * Returns NULL on allocation failure or arithmetic overflow.
 */
c_string_t c_string_concat(c_const_string_t a, c_const_string_t b);

/* ── Capacity Management ──────────────────────────────────────────────────────── */

/**
 * Ensure at least `additional_bytes` of free space beyond the current used-length.
 *
 * Growth policy:
 *   · Below 1 MB: double the current capacity.
 *   · At or above 1 MB: grow by 1 MB chunks.
 *
 * The caller MUST reassign from the return value.
 * Returns the original pointer unmodified on failure or for static strings.
 */
c_string_t c_string_ensure_capacity(c_string_t string, size_t additional_bytes);

/**
 * Pre-allocate `additional_bytes` of free space.
 * Alias of c_string_ensure_capacity(); provided for semantic clarity at call sites
 * where the intent is to reserve ahead of a known series of appends.
 */
c_string_t c_string_reserve(c_string_t string, size_t additional_bytes);

/**
 * Shrink the allocation to exactly fit the current used-length plus NUL terminator(s).
 * The caller MUST reassign from the return value.
 * No-op for static strings (returned unmodified).
 */
c_string_t c_string_shrink_to_fit(c_string_t string);

/** Call c_string_shrink_to_fit() on every non-NULL entry of `array` in-place. */
void c_string_array_shrink_to_fit(c_string_t *array, size_t count);

/* ── Slice Extractors ─────────────────────────────────────────────────────────── */

/** Return a zero-copy view of the entire string as a raw byte slice. */
c_slice_t c_slice_of(c_const_string_t string);

/**
 * Return a zero-copy subslice by byte offset and byte length.
 * Both values are clamped to the string's used-length; the result is never
 * out-of-bounds.
 */
c_slice_t c_subslice(c_const_string_t string, size_t byte_offset, size_t byte_length);

/**
 * Return a zero-copy subslice by logical codepoint offset and count.
 * Dispatches on the string's encoding tag:
 *   ASCII  – each byte is one codepoint (O(n) but trivially fast).
 *   UTF-8  – walks multi-byte sequences with utf8proc_iterate.
 *   UTF-16 – accounts for surrogate pairs.
 * The result is 2-byte-aligned for UTF-16 strings.
 */
c_slice_t c_subslice_codepoints(c_const_string_t string, size_t cp_offset, size_t cp_count);

/** Zero-copy view of the entire string as a UTF-8 byte slice. */
c_utf8_slice_t c_utf8_slice_of(c_const_string_t string);

/** UTF-8 subslice by byte offset and byte length (clamped to bounds). */
c_utf8_slice_t c_utf8_subslice(c_const_string_t string, size_t byte_offset,
                                size_t byte_length);

/** UTF-8 subslice by codepoint offset and count (uses utf8proc_iterate). */
c_utf8_slice_t c_utf8_subslice_codepoints(c_const_string_t string, size_t cp_offset,
                                           size_t cp_count);

/**
 * Zero-copy view of the entire string as a UTF-16 byte slice.
 * `length` is always in bytes (always even).
 */
c_utf16_slice_t c_utf16_slice_of(c_const_string_t string);

/**
 * UTF-16 subslice by byte offset and byte length.
 * Both values are rounded down to even (2-byte alignment) before clamping.
 */
c_utf16_slice_t c_utf16_subslice(c_const_string_t string, size_t byte_offset,
                                  size_t byte_length);

/** UTF-16 subslice by codepoint offset and count (surrogate-pair-aware). */
c_utf16_slice_t c_utf16_subslice_codepoints(c_const_string_t string, size_t cp_offset,
                                             size_t cp_count);

/**
 * Return the number of UTF-16 code units (uint16_t values) in a slice.
 * Equal to slice.length / 2.
 */
size_t c_utf16_slice_unit_count(c_utf16_slice_t slice);

/* ── Encoding Converters ──────────────────────────────────────────────────────── */

/**
 * Validate and NFC-normalise a NUL-terminated UTF-8 string.
 * Returns a new convey string tagged C_STRING_ENC_UTF8.
 * Returns NULL if the input is NULL, contains invalid UTF-8, or allocation fails.
 */
c_string_t c_string_create_from_utf8(const char *text);

/**
 * Like c_string_create_from_utf8(), but exposes the full set of utf8proc option
 * flags.  UTF8PROC_NULLTERM is always added internally; the input must be
 * NUL-terminated.
 */
c_string_t c_string_create_from_utf8_pro(const char *text, utf8proc_option_t options);

/**
 * Convert a convey UTF-8 (or ASCII) string to a new convey UTF-16 LE string.
 * The result is tagged C_STRING_ENC_UTF16 and is double-NUL-terminated.
 * Returns NULL on invalid UTF-8, lone surrogates in the input, or allocation failure.
 */
c_string_t c_string_convert_utf8_utf16(c_const_string_t utf8_input);

/**
 * Like c_string_convert_utf8_utf16(), but allows selecting the output byte order.
 * Pass use_big_endian=true for UTF-16 BE (used by Java and some network protocols).
 */
c_string_t c_string_convert_utf8_utf16_pro(c_const_string_t utf8_input,
                                            bool use_big_endian);

/**
 * Convert a convey UTF-16 LE string to a new convey UTF-8 string.
 * Lone surrogates that cannot be paired are replaced with U+FFFD (replacement char).
 * Result is tagged C_STRING_ENC_UTF8.
 */
c_string_t c_string_convert_utf16_utf8(c_const_string_t utf16_input);

/**
 * Like c_string_convert_utf16_utf8(), but allows selecting the source byte order.
 * Pass use_big_endian=true when the UTF-16 data is stored as big-endian.
 */
c_string_t c_string_convert_utf16_utf8_pro(c_const_string_t utf16_input,
                                            bool use_big_endian);

/**
 * Convert a static string to a dynamic string.
 * If the string is already dynamic, returns it unchanged.
 * On allocation failure, returns the original static string as a fallback.
 * On success, the old static string is freed and a new dynamic string is returned.
 */
c_string_t c_string_to_dynamic(c_string_t string);

/**
 * Print a UTF-16 slice to stdout by first converting to UTF-8.
 * Works correctly on all platforms (Linux, macOS, Windows terminal).
 *
 *   c_string_print_utf16(C_UTF16_SLICE_OF(my_str));
 *   c_string_print_utf16(C_UTF16_SUBSLICE_CP(my_str, 0, 10));
 */
void c_string_print_utf16(c_utf16_slice_t slice);

/* ── Case Conversion  (UTF-8 / ASCII only) ────────────────────────────────────── */

/**
 * Return a new NFC-normalised, case-folded (Unicode full lowercase) copy.
 * Uses UTF8PROC_CASEFOLD | UTF8PROC_STABLE | UTF8PROC_COMPOSE via utf8proc_map.
 * Input must be UTF-8 or ASCII tagged; returns NULL for UTF-16 or on failure.
 * Result is tagged C_STRING_ENC_UTF8.
 */
c_string_t c_string_to_lower(c_const_string_t string);

/**
 * Return a new NFC-normalised, uppercased copy.
 * Uses utf8proc_toupper() per codepoint via utf8proc_map_custom().
 * Input must be UTF-8 or ASCII tagged; returns NULL for UTF-16 or on failure.
 * Result is tagged C_STRING_ENC_UTF8.
 */
c_string_t c_string_to_upper(c_const_string_t string);

/* ── Searching & Comparison ───────────────────────────────────────────────────── */

/**
 * Bytewise lexicographic comparison.
 * Returns < 0 when a < b, 0 when equal, > 0 when a > b.
 * NULL sorts before any non-NULL string.
 * For NFC-normalised UTF-8 strings this matches Unicode codepoint order.
 */
int c_string_compare(c_const_string_t a, c_const_string_t b);

/**
 * Return true when `a` and `b` have identical byte content and length.
 * Encoding tags are NOT compared; use C_STRING_ENC_OF() if you need that check.
 */
bool c_string_equal(c_const_string_t a, c_const_string_t b);

/**
 * Find the first occurrence of `needle` inside `haystack` (bytewise search).
 * Returns the byte offset on success.
 * Returns SIZE_MAX when not found or when either argument is NULL.
 * An empty needle (zero used_length) always returns 0.
 */
size_t c_string_find(c_const_string_t haystack, c_const_string_t needle);

/**
 * Return true when `needle` occurs anywhere inside `str`.
 * Equivalent to (c_string_find(str, needle) != SIZE_MAX).
 */
bool c_string_contains(c_const_string_t str, c_const_string_t needle);

/**
 * Return true when `str` begins with the bytes of `prefix`.
 * An empty prefix always returns true.  NULL arguments return false.
 */
bool c_string_starts_with(c_const_string_t str, c_const_string_t prefix);

/**
 * Return true when `str` ends with the bytes of `suffix`.
 * An empty suffix always returns true.  NULL arguments return false.
 */
bool c_string_ends_with(c_const_string_t str, c_const_string_t suffix);

/* ── In-Place Mutation ────────────────────────────────────────────────────────── */

/**
 * Strip ASCII whitespace (' ' '\\t' '\\n' '\\r') from the right end in-place.
 * For UTF-16 strings the corresponding UTF-16 LE code units are stripped.
 * Never reallocates; always returns the same pointer.
 * Returns NULL for a NULL input.
 */
c_string_t c_string_trim_right(c_string_t string);

/**
 * Strip ASCII whitespace from the left end in-place using memmove.
 * For UTF-16 the corresponding UTF-16 LE code units are stripped.
 * Never reallocates; always returns the same pointer.
 * Returns NULL for a NULL input.
 */
c_string_t c_string_trim_left(c_string_t string);

/**
 * Strip ASCII whitespace from both ends in-place.
 * Equivalent to c_string_trim_right(c_string_trim_left(string)).
 */
c_string_t c_string_trim(c_string_t string);

/**
 * Return a new dynamic string containing `string` repeated `count` times.
 * Encoding is inherited from the source.
 * Returns an empty string for count == 0.
 * Returns NULL on arithmetic overflow or allocation failure.
 */
c_string_t c_string_repeat(c_const_string_t string, size_t count);

/* ── Validation ───────────────────────────────────────────────────────────────── */

/**
 * Return true when every byte sequence in `string` is valid UTF-8.
 * Uses utf8proc_iterate() per codepoint.
 * Always returns true for NULL.
 */
bool c_string_validate_utf8(c_const_string_t string);

/* ── Internal Getters & Setters ───────────────────────────────────────────────── */

/**
 * Return the byte size of the header struct for the given raw type_flags byte.
 * Internally masks with C_STRING_TYPE_MASK, so the full flags byte may be passed.
 */
size_t c_string_get_header_size(unsigned char flags);

/**
 * Return the smallest header class constant (C_STRING_TYPE_8/16/32/64) capable
 * of storing a length or capacity value of `size` bytes.
 */
unsigned char c_string_determine_type(size_t size);

/**
 * Return the encoding tag (C_STRING_ENC_ASCII / UTF8 / UTF16) from the header.
 * Returns C_STRING_ENC_ASCII for a NULL string.
 */
unsigned char c_string_get_encoding(c_const_string_t string);

/**
 * Re-tag the string's encoding byte in-place WITHOUT converting any data.
 * Use with care: only call this when you have verified that the byte content
 * already matches the new encoding (e.g. after confirming a UTF-8 string
 * contains only ASCII codepoints).
 */
void c_string_set_encoding(c_string_t string, unsigned char enc);

/**
 * Return the current used byte count (excludes NUL terminator(s)).
 * Returns 0 for NULL.
 */
size_t c_string_get_used_length(c_const_string_t string);

/**
 * Return the number of free bytes currently available (allocated_capacity –
 * used_length, excluding NUL bytes).
 * Returns 0 for NULL or a full string.
 */
size_t c_string_get_available_capacity(c_const_string_t string);

/**
 * Return the total allocated data capacity in bytes (used + spare, excluding NUL).
 * Equal to used_length + available_capacity.
 * Returns 0 for NULL.
 */
size_t c_string_get_capacity(c_const_string_t string);

/**
 * Count logical codepoints, respecting the string's encoding tag:
 *   ASCII  – returns used_length.
 *   UTF-8  – counts by walking multi-byte sequences; invalid bytes count as one each.
 *   UTF-16 – counts code-unit pairs as one codepoint, lone units as one each.
 * Returns 0 for NULL.
 */
size_t c_string_codepoint_count(c_const_string_t string);

/**
 * Force both the used-length and allocated-capacity fields in the header.
 * Does NOT write NUL terminators – call this only when you know the
 * terminator bytes are already correct (e.g. immediately after bulk memcpy).
 * For UTF-16 strings, both values are rounded down to even automatically.
 */
void c_string_set_lengths(c_string_t string, size_t used, size_t capacity);

/**
 * Force only the used-length field in the header.
 * For UTF-16 strings, the value is rounded down to even automatically.
 */
void c_string_set_used_length(c_string_t string, size_t used);

#ifdef __cplusplus
}
#endif

/*==================================================================================
  SECTION 6 – ENCODING PROPERTY MACROS
  These use c_string_get_encoding() which is declared in Section 5 above.
==================================================================================*/

/** True when `str` is non-NULL and carries the static/fixed-capacity flag. */
#define C_STRING_IS_STATIC_STR(str)  (((str) != NULL) && (((str)[-1] & C_STRING_IS_STATIC) != 0))

/**
 * Null-terminator byte count for a given encoding constant.
 * Returns 2 for UTF-16, 1 for everything else.
 * Use this instead of hardcoding 1 or 2 whenever NUL size matters.
 */
#define C_STRING_NULL_SIZE(enc)      (((enc) & C_STRING_ENC_MASK) == C_STRING_ENC_UTF16 ? 2u : 1u)

/** Extract the encoding tag (C_STRING_ENC_*) from a live string. */
#define C_STRING_ENC_OF(str)         c_string_get_encoding(str)

/** True when `str` is tagged C_STRING_ENC_ASCII. */
#define C_STRING_IS_ASCII(str)       (c_string_get_encoding(str) == C_STRING_ENC_ASCII)

/** True when `str` is tagged C_STRING_ENC_UTF8. */
#define C_STRING_IS_UTF8(str)        (c_string_get_encoding(str) == C_STRING_ENC_UTF8)

/** True when `str` is tagged C_STRING_ENC_UTF16. */
#define C_STRING_IS_UTF16(str)       (c_string_get_encoding(str) == C_STRING_ENC_UTF16)

/** True when `str` uses single-byte units (ASCII or UTF-8, i.e. not UTF-16). */
#define C_STRING_IS_BYTE_ENC(str)    (!C_STRING_IS_UTF16(str))

/** Reinterpret a convey UTF-16 string buffer as a const uint16_t pointer. */
#define C_STRING_AS_U16(str)         ((const uint16_t *)(str))

/*==================================================================================
  SECTION 7 – INTERNAL HEADER-ACCESS MACROS
  These cast the data pointer backwards into the packed header struct.
  They are intended for use inside convey_main.c only – prefer the public API
  (c_string_get_used_length, c_string_get_capacity, etc.) in application code.
==================================================================================*/

/* Mutable header access */
#define GET_HEADER_8(ptr)        ((struct c_string_header_8  *)((ptr) - sizeof(struct c_string_header_8)))
#define GET_HEADER_16(ptr)       ((struct c_string_header_16 *)((ptr) - sizeof(struct c_string_header_16)))
#define GET_HEADER_32(ptr)       ((struct c_string_header_32 *)((ptr) - sizeof(struct c_string_header_32)))
#define GET_HEADER_64(ptr)       ((struct c_string_header_64 *)((ptr) - sizeof(struct c_string_header_64)))

/* Read-only header access */
#define GET_CONST_HEADER_8(ptr)  ((const struct c_string_header_8  *)((ptr) - sizeof(struct c_string_header_8)))
#define GET_CONST_HEADER_16(ptr) ((const struct c_string_header_16 *)((ptr) - sizeof(struct c_string_header_16)))
#define GET_CONST_HEADER_32(ptr) ((const struct c_string_header_32 *)((ptr) - sizeof(struct c_string_header_32)))
#define GET_CONST_HEADER_64(ptr) ((const struct c_string_header_64 *)((ptr) - sizeof(struct c_string_header_64)))

/*==================================================================================
  SECTION 8 – printf / wprintf FORMAT HELPERS  (C11 _Generic)
==================================================================================*/

/*
 * Format specifier for use with printf and C_str_arg():
 *   printf("Name: " C_STR_FMT "\n", C_str_arg(my_str));
 */
#define C_STR_FMT   "%.*s"
#define C_WSTR_FMT  L"%.*ls"

/* ──────────────────────────────────────────────────────────────────────────────────
 * STANDARD (double-evaluation) MACROS
 *
 * WARNING: C_str_arg(X) evaluates X twice – once for the length and once for the
 * pointer.  Never pass a function call with side-effects here.  Use the
 * C_PRINTF_SAFE() macro for that.
 * ──────────────────────────────────────────────────────────────────────────────── */

/**
 * Resolve to the integer character count of X, where X may be:
 *   - char * / const char *          (NUL-terminated C string)
 *   - uint8_t * / const uint8_t *    (convey string)
 *   - c_slice_t / c_utf8_slice_t     (byte-view slices)
 *
 * c_utf16_slice_t is intentionally omitted: use C_wstr_arg on Windows or
 * c_string_print_utf16() for cross-platform output of UTF-16 text.
 */
#define C_str_len(X) _Generic((X),                                         \
    char *:           (int)strlen((const char *)(X)),                      \
    const char *:     (int)strlen((const char *)(X)),                      \
    uint8_t *:        (int)c_string_get_used_length((const uint8_t *)(X)), \
    const uint8_t *:  (int)c_string_get_used_length((const uint8_t *)(X)), \
    c_slice_t:        (int)(X).length,                                     \
    c_utf8_slice_t:   (int)(X).length                                      \
)

/**
 * Resolve to a `const char *` suitable for use with `%.*s`.
 * Returns "" for NULL pointers and NULL slice data (never returns NULL itself).
 */
#define C_str_ptr(X) _Generic((X),                                              \
    char *:           ((X) ? (const char *)(X) : ""),                           \
    const char *:     ((X) ? (const char *)(X) : ""),                           \
    uint8_t *:        ((X) ? (const char *)(X) : ""),                           \
    const uint8_t *:  ((X) ? (const char *)(X) : ""),                           \
    c_slice_t:        ((X).data ? (const char *)(X).data : ""),                 \
    c_utf8_slice_t:   ((X).data ? (const char *)(X).data : "")                  \
)

/**
 * Expand to the two printf arguments required by C_STR_FMT ("%.*s"):
 * the integer length followed by the char pointer.
 *
 * Usage:
 *   printf("Hello " C_STR_FMT "!\n", C_str_arg(my_str));
 *   printf("A=" C_STR_FMT " B=" C_STR_FMT "\n", C_str_arg(a), C_str_arg(b));
 *
 * Accepts: c_string_t, c_const_string_t, c_slice_t, c_utf8_slice_t, char *.
 *
 * WARNING: evaluates X twice.  Assign function results to a variable first.
 */
#define C_str_arg(X)  C_str_len(X), C_str_ptr(X)

/* ──────────────────────────────────────────────────────────────────────────────────
 * SAFE (single-evaluation) STATEMENT MACROS
 *
 * These evaluate the expression exactly once, making them safe for function calls
 * that mutate or allocate.  The trade-off is that they cannot be mixed with other
 * printf arguments in the same call – they are standalone print statements.
 * ──────────────────────────────────────────────────────────────────────────────── */

/* Internal helper struct – do not use directly. */
typedef struct { int len; const char *ptr; } c_str_fmt_arg_t;

/* Internal helper functions – not part of the public API. */
static inline c_str_fmt_arg_t _c_fmt_cstr(const char *s) {
    return (c_str_fmt_arg_t){ s ? (int)strlen(s) : 0, s ? s : "" };
}
static inline c_str_fmt_arg_t _c_fmt_str(const uint8_t *s) {
    return (c_str_fmt_arg_t){ (int)c_string_get_used_length(s), s ? (const char *)s : "" };
}
static inline c_str_fmt_arg_t _c_fmt_slice(c_slice_t s) {
    return (c_str_fmt_arg_t){ (int)s.length, s.data ? (const char *)s.data : "" };
}
static inline c_str_fmt_arg_t _c_fmt_utf8_slice(c_utf8_slice_t s) {
    return (c_str_fmt_arg_t){ (int)s.length, s.data ? (const char *)s.data : "" };
}

/* Internal dispatcher – do not use directly. */
#define C_FMT_ARG(X) _Generic((X),                      \
    char *:           _c_fmt_cstr,                       \
    const char *:     _c_fmt_cstr,                       \
    uint8_t *:        _c_fmt_str,                        \
    const uint8_t *:  _c_fmt_str,                        \
    c_slice_t:        _c_fmt_slice,                      \
    c_utf8_slice_t:   _c_fmt_utf8_slice                  \
)(X)

/**
 * Single-evaluation printf statement.  Safe to use with mutating function calls.
 *
 * Usage:
 *   C_PRINTF_SAFE("Trimmed: " C_STR_FMT "\n", c_string_trim(my_str));
 *
 * Limitation: cannot be mixed with additional format arguments.
 * For mixed output, store the result in a variable and use C_str_arg().
 */
#define C_PRINTF_SAFE(fmt, expr)                     \
    do {                                             \
        c_str_fmt_arg_t _csa_tmp = C_FMT_ARG(expr); \
        printf((fmt), _csa_tmp.len, _csa_tmp.ptr);   \
    } while (0)

/* ──────────────────────────────────────────────────────────────────────────────────
 * UTF-16 PRINTF EQUIVALENTS  (Windows / wchar_t targets only)
 * Active when wchar_t is 16 bits (Windows) or _WIN32 is defined.
 * ──────────────────────────────────────────────────────────────────────────────── */
#if WCHAR_MAX == 0xFFFFu || defined(_WIN32)

/**
 * Resolve to the wchar_t character count of a UTF-16 string or slice.
 * The length is derived from the byte length divided by 2 (each code unit is 2 bytes).
 */
#define C_wstr_len(X) _Generic((X),                                                          \
    uint8_t *:        (int)(c_string_get_used_length((const uint8_t *)(X)) / 2u),            \
    const uint8_t *:  (int)(c_string_get_used_length((const uint8_t *)(X)) / 2u),            \
    c_utf16_slice_t:  (int)((X).length / 2u)                                                 \
)

/**
 * Resolve to a `const wchar_t *` suitable for use with `%.*ls`.
 * Returns L"" for NULL pointers (never returns NULL itself).
 */
#define C_wstr_ptr(X) _Generic((X),                                              \
    uint8_t *:        ((X) ? (const wchar_t *)(X) : L""),                        \
    const uint8_t *:  ((X) ? (const wchar_t *)(X) : L""),                        \
    c_utf16_slice_t:  ((X).data ? (const wchar_t *)(X).data : L"")               \
)

/**
 * Expand to the two wprintf arguments required by C_WSTR_FMT (L"%.*ls"):
 * the integer code-unit count followed by the wchar_t pointer.
 *
 * Usage:
 *   wprintf(L"Name: " C_WSTR_FMT L"\n", C_wstr_arg(my_utf16_str));
 *
 * WARNING: evaluates X twice.  Assign function results first.
 */
#define C_wstr_arg(X)  C_wstr_len(X), C_wstr_ptr(X)

/* Internal helper struct for safe UTF-16 print – do not use directly. */
typedef struct { int len; const wchar_t *ptr; } c_wstr_fmt_arg_t;

static inline c_wstr_fmt_arg_t _c_wfmt_str(const uint8_t *s) {
    return (c_wstr_fmt_arg_t){
        (int)(c_string_get_used_length(s) / 2u),
        s ? (const wchar_t *)s : L""
    };
}
static inline c_wstr_fmt_arg_t _c_wfmt_utf16_slice(c_utf16_slice_t s) {
    return (c_wstr_fmt_arg_t){
        (int)(s.length / 2u),
        s.data ? (const wchar_t *)s.data : L""
    };
}

/* Internal dispatcher – do not use directly. */
#define C_WFMT_ARG(X) _Generic((X),                \
    uint8_t *:        _c_wfmt_str,                  \
    const uint8_t *:  _c_wfmt_str,                  \
    c_utf16_slice_t:  _c_wfmt_utf16_slice            \
)(X)

/**
 * Single-evaluation wprintf statement for UTF-16 strings.
 *
 * Usage:
 *   C_WPRINTF_SAFE(L"Name: " C_WSTR_FMT L"\n", c_string_trim(my_utf16_str));
 */
#define C_WPRINTF_SAFE(fmt, expr)                      \
    do {                                               \
        c_wstr_fmt_arg_t _cwsa_tmp = C_WFMT_ARG(expr);\
        wprintf((fmt), _cwsa_tmp.len, _cwsa_tmp.ptr);  \
    } while (0)

#endif /* WCHAR_MAX == 0xFFFF || _WIN32 */

/*==================================================================================
  SECTION 9 – SLICE COMPOUND-LITERAL HELPERS
  All of these use c_string_get_used_length(), which is declared in Section 5.
==================================================================================*/

/** Zero-copy view of the entire string as a c_slice_t. */
#define C_SLICE_OF(str)                   ((c_slice_t){ (str), c_string_get_used_length(str) })

/** Zero-copy view of the entire string as a c_utf8_slice_t. */
#define C_UTF8_SLICE_OF(str)              ((c_utf8_slice_t){ (str), c_string_get_used_length(str) })

/**
 * Zero-copy view of the entire string as a c_utf16_slice_t.
 * Length is in bytes (always even for valid UTF-16 strings).
 */
#define C_UTF16_SLICE_OF(str)             ((c_utf16_slice_t){ (str), c_string_get_used_length(str) })

/** Extract a byte-offset / byte-length subslice (generic). */
#define C_SUBSLICE(str, off, len)         c_subslice((str), (off), (len))

/**
 * Codepoint-indexed subslice.  Dispatches on the string's encoding tag and
 * returns a zero-copy view.  Works for ASCII, UTF-8, and UTF-16.
 */
#define C_SUBSLICE_CP(str, off, cnt)      c_subslice_codepoints((str), (off), (cnt))

/** UTF-8 subslice by byte offset + byte length. */
#define C_UTF8_SUBSLICE(str, o, l)        c_utf8_subslice((str), (o), (l))

/** UTF-8 subslice by codepoint offset + count. */
#define C_UTF8_SUBSLICE_CP(str, o, cnt)   c_utf8_subslice_codepoints((str), (o), (cnt))

/** UTF-16 subslice by byte offset + byte length (both rounded to even). */
#define C_UTF16_SUBSLICE(str, o, l)       c_utf16_subslice((str), (o), (l))

/** UTF-16 subslice by codepoint offset + count (surrogate-pair-aware). */
#define C_UTF16_SUBSLICE_CP(str, o, cnt)  c_utf16_subslice_codepoints((str), (o), (cnt))

#endif /* CONVEY_H */
