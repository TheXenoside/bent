#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "utf8proc.h"

#ifdef __cplusplus
extern "C" {
#endif

//#define b_ALIGNED

// --- Memory Layout (Bit 0) ---
#define B_STRING_PACKED      0x00u
#define B_STRING_ALIGNED     0x01u
#define B_STRING_PACK_MASK   0x01u

// --- Allocation Strategy (Bit 1) ---
// "static" = fixed-capacity heap buffer; b_str_ensure never reallocs it
#define B_STRING_DYNAMIC     0x00u
#define B_STRING_STATIC      0x02u
#define B_STRING_STATIC_MASK 0x02u
#define B_STR_STATIC  B_STRING_STATIC

// --- Header Size Class (Bits 2-3) ---
#define B_STR_TYPE_8         0x00u
#define B_STR_TYPE_16        0x04u
#define B_STR_TYPE_32        0x08u
#define B_STR_TYPE_64        0x0Cu
#define B_STR_TYPE_MASK      0x0Cu

// --- Encoding Tag (Bits 4-6) ---
#define B_STR_ENC_ASCII      0x00u
#define B_STR_ENC_UTF8       0x10u
#define B_STR_ENC_UTF16BE    0x20u
#define B_STR_ENC_UTF16LE    0x30u
#define B_STR_ENC_UTF32BE    0x40u
#define B_STR_ENC_UTF32LE    0x50u
#define B_STR_ENC_UTF16      B_STR_ENC_UTF16LE
#define B_STR_ENC_UTF32      B_STR_ENC_UTF32LE
#define B_STR_ENC_MASK       0x70u

// UTF-16: bits 5-6 == 01 (0x20..0x3F); UTF-32: bits 5-6 == 10 (0x40..0x5F)
#define B_STR_IS_UTF16_ENC(e) (((e) & 0x60u) == 0x20u)
#define B_STR_IS_UTF32_ENC(e) (((e) & 0x60u) == 0x40u)

#define B_STR_ONE_MEG  1048576u

/*──────────────────────────────────────────────────────────────────────────────
  HEADER STRUCTS  (s[-1] is always the flags byte)
──────────────────────────────────────────────────────────────────────────────*/
#ifdef b_ALIGNED
typedef struct b_string_header_8  { uint8_t  capacity; uint8_t  length; uint8_t flags; uint8_t  data[]; } b_hdr8_t;
typedef struct b_string_header_16 { uint16_t capacity; uint16_t length; uint8_t _pad;  uint8_t  flags; uint8_t data[]; } b_hdr16_t;
typedef struct b_string_header_32 { uint32_t capacity; uint32_t length; uint8_t _pad[3]; uint8_t flags; uint8_t data[]; } b_hdr32_t;
typedef struct b_string_header_64 { uint64_t capacity; uint64_t length; uint8_t _pad[7]; uint8_t flags; uint8_t data[]; } b_hdr64_t;
#else
#  if defined(_MSC_VER)
#    pragma pack(push, 1)
#    define B_STR_PACKED_ATTR
#  elif defined(__GNUC__) || defined(__clang__)
#    define B_STR_PACKED_ATTR __attribute__((__packed__))
#  else
#    define B_STR_PACKED_ATTR
#  endif
typedef struct B_STR_PACKED_ATTR b_string_header_8  { uint8_t  capacity; uint8_t  length; uint8_t flags; uint8_t data[]; } b_hdr8_t;
typedef struct B_STR_PACKED_ATTR b_string_header_16 { uint16_t capacity; uint16_t length; uint8_t flags; uint8_t data[]; } b_hdr16_t;
typedef struct B_STR_PACKED_ATTR b_string_header_32 { uint32_t capacity; uint32_t length; uint8_t flags; uint8_t data[]; } b_hdr32_t;
typedef struct B_STR_PACKED_ATTR b_string_header_64 { uint64_t capacity; uint64_t length; uint8_t flags; uint8_t data[]; } b_hdr64_t;
#  if defined(_MSC_VER)
#    pragma pack(pop)
#  endif
#endif

// Backward-compatible bare-name aliases
typedef b_hdr8_t  b_hdr8;
typedef b_hdr16_t b_hdr16;
typedef b_hdr32_t b_hdr32;
typedef b_hdr64_t b_hdr64;

// Mutable header pointer from data pointer
#define B_HDR8(p)   ((b_hdr8_t *) ((p) - sizeof(b_hdr8_t)))
#define B_HDR16(p)  ((b_hdr16_t*)((p) - sizeof(b_hdr16_t)))
#define B_HDR32(p)  ((b_hdr32_t*)((p) - sizeof(b_hdr32_t)))
#define B_HDR64(p)  ((b_hdr64_t*)((p) - sizeof(b_hdr64_t)))
// Const header pointer
#define B_CHDR8(p)  ((const b_hdr8_t *) ((p) - sizeof(b_hdr8_t)))
#define B_CHDR16(p) ((const b_hdr16_t*)((p) - sizeof(b_hdr16_t)))
#define B_CHDR32(p) ((const b_hdr32_t*)((p) - sizeof(b_hdr32_t)))
#define B_CHDR64(p) ((const b_hdr64_t*)((p) - sizeof(b_hdr64_t)))

/*──────────────────────────────────────────────────────────────────────────────
  CORE TYPES
──────────────────────────────────────────────────────────────────────────────*/
typedef       uint8_t* b_str_t;
typedef const uint8_t* b_cstr_t;

// Slice: (data pointer, byte length) pair, NOT null-terminated
typedef struct b_byte_slice  { const uint8_t *data; size_t len; } b_slice_t;
typedef struct b_utf8_slice  { const uint8_t *data; size_t len; } b_u8slice_t;
typedef struct b_utf16_slice { const uint8_t *data; size_t len; } b_u16slice_t; // len multiple of 2
typedef struct b_utf32_slice { const uint8_t *data; size_t len; } b_u32slice_t; // len multiple of 4

/*──────────────────────────────────────────────────────────────────────────────
  CORE ACCESSORS
──────────────────────────────────────────────────────────────────────────────*/
size_t  b_str_hdr_size (uint8_t flags);
uint8_t b_str_pick_type(size_t  byte_size);
uint8_t b_str_enc      (b_cstr_t s);
void    b_str_set_enc  (b_str_t  s, uint8_t encoding);
size_t  b_str_len      (b_cstr_t s);
size_t  b_str_cap      (b_cstr_t s);
size_t  b_str_avail    (b_cstr_t s);
void    b_str_set_lens (b_str_t  s, size_t used_bytes, size_t cap_bytes);
void    b_str_set_len  (b_str_t  s, size_t used_bytes);
size_t  b_str_cpcount  (b_cstr_t s);

/*──────────────────────────────────────────────────────────────────────────────
  LIFECYCLE
──────────────────────────────────────────────────────────────────────────────*/
b_str_t b_str_new           (const char *cstr);
b_str_t b_str_new_pro       (const void *data, size_t byte_len, uint8_t encoding);
b_str_t b_str_new_static    (const char *cstr, size_t extra_bytes);
b_str_t b_str_new_static_pro(const void *data, size_t byte_len,
                              size_t total_capacity, uint8_t encoding);
void    b_str_free          (b_str_t s);
b_str_t b_str_dup           (b_cstr_t s);
void    b_str_clear         (b_str_t s);
bool    b_str_empty         (b_cstr_t s);
b_str_t b_str_to_dyn        (b_str_t s);  // static→dynamic; returns s on OOM

/*──────────────────────────────────────────────────────────────────────────────
  SLICE CONSTRUCTORS
──────────────────────────────────────────────────────────────────────────────*/
b_str_t b_str_from_slice   (b_slice_t    slice);
b_str_t b_str_from_u8slice (b_u8slice_t  slice);
b_str_t b_str_from_u16slice(b_u16slice_t slice);
b_str_t b_str_from_u16     (const uint16_t *units, size_t unit_count);
b_str_t b_str_from_u32slice(b_u32slice_t slice);
b_str_t b_str_from_u32     (const uint32_t *units, size_t unit_count);

/*──────────────────────────────────────────────────────────────────────────────
  APPENDING & CONCATENATION
  All append variants return s; never NULL when s != NULL.
──────────────────────────────────────────────────────────────────────────────*/
b_str_t b_str_append    (b_str_t s, const char *cstr);
b_str_t b_str_append_pro(b_str_t s, const void *data, size_t byte_len);
b_str_t b_str_append_sl (b_str_t s, b_slice_t    slice);
b_str_t b_str_append_u8 (b_str_t s, b_u8slice_t  slice);
b_str_t b_str_append_u16(b_str_t s, b_u16slice_t slice);
b_str_t b_str_append_u32(b_str_t s, b_u32slice_t slice);
b_str_t b_str_concat    (b_cstr_t a, b_cstr_t b);  // constructor; may return NULL

/*──────────────────────────────────────────────────────────────────────────────
  CAPACITY MANAGEMENT
──────────────────────────────────────────────────────────────────────────────*/
b_str_t b_str_ensure (b_str_t s, size_t extra_bytes); // returns s on OOM/static
b_str_t b_str_reserve(b_str_t s, size_t extra_bytes);
b_str_t b_str_fit    (b_str_t s);
void    b_str_arr_fit(b_str_t *array, size_t count);

/*──────────────────────────────────────────────────────────────────────────────
  SLICE EXTRACTORS
──────────────────────────────────────────────────────────────────────────────*/
b_slice_t    b_slice_of      (b_cstr_t s);
b_slice_t    b_subslice      (b_cstr_t s, size_t byte_off, size_t byte_len);
b_slice_t    b_subslice_cp   (b_cstr_t s, size_t cp_off,   size_t cp_count);

b_u8slice_t  b_u8slice_of    (b_cstr_t s);
b_u8slice_t  b_u8subslice    (b_cstr_t s, size_t byte_off, size_t byte_len);
b_u8slice_t  b_u8subslice_cp (b_cstr_t s, size_t cp_off,   size_t cp_count);

b_u16slice_t b_u16slice_of   (b_cstr_t s);
b_u16slice_t b_u16subslice   (b_cstr_t s, size_t byte_off, size_t byte_len);
b_u16slice_t b_u16subslice_cp(b_cstr_t s, size_t cp_off,   size_t cp_count);
size_t       b_u16slice_units(b_u16slice_t slice);

b_u32slice_t b_u32slice_of   (b_cstr_t s);
b_u32slice_t b_u32subslice   (b_cstr_t s, size_t byte_off, size_t byte_len);
b_u32slice_t b_u32subslice_cp(b_cstr_t s, size_t cp_off,   size_t cp_count);
size_t       b_u32slice_units(b_u32slice_t slice);

/*──────────────────────────────────────────────────────────────────────────────
  ENCODING CONVERTERS
  On failure: return s unchanged (never NULL when s != NULL).
  STATIC input: result written back in-place if it fits; pointer always stable.
  DYNAMIC input: returns new b_str_t on success; caller frees old string.
──────────────────────────────────────────────────────────────────────────────*/
b_str_t b_str_utf8_norm (const char *nul_utf8); // NFC-normalise; returns NULL on fail
b_str_t b_str_to_utf8   (b_str_t s);
b_str_t b_str_to_utf16  (b_str_t s);  // → UTF-16 LE
b_str_t b_str_to_utf16be(b_str_t s);  // → UTF-16 BE
b_str_t b_str_to_utf32le(b_str_t s);  // → UTF-32 LE
b_str_t b_str_to_utf32be(b_str_t s);  // → UTF-32 BE

/*──────────────────────────────────────────────────────────────────────────────
  CASE CONVERSION  (UTF-8/ASCII only; others return s unchanged, fail silently)
  Same static-writeback / never-NULL contract as encoding converters.
──────────────────────────────────────────────────────────────────────────────*/
b_str_t b_str_lower(b_str_t s);
b_str_t b_str_upper(b_str_t s);

/*──────────────────────────────────────────────────────────────────────────────
  COMPARISON & SEARCH  (byte-level; NFC-normalise first for Unicode order)
──────────────────────────────────────────────────────────────────────────────*/
int    b_str_cmp        (b_cstr_t a, b_cstr_t b);
bool   b_str_eq         (b_cstr_t a, b_cstr_t b);
size_t b_str_find       (b_cstr_t haystack, b_cstr_t needle);
bool   b_str_contains   (b_cstr_t s, b_cstr_t needle);
bool   b_str_starts_with(b_cstr_t s, b_cstr_t prefix);
bool   b_str_ends_with  (b_cstr_t s, b_cstr_t suffix);

/*──────────────────────────────────────────────────────────────────────────────
  IN-PLACE MUTATION  (return s; NULL in → NULL out)
──────────────────────────────────────────────────────────────────────────────*/
b_str_t b_str_trim_r(b_str_t s);
b_str_t b_str_trim_l(b_str_t s);
b_str_t b_str_trim  (b_str_t s);
b_str_t b_str_repeat(b_cstr_t s, size_t repeat_count); // constructor; may return NULL

/*──────────────────────────────────────────────────────────────────────────────
  VALIDATION
──────────────────────────────────────────────────────────────────────────────*/
bool b_str_valid_utf8(b_cstr_t s);

/*──────────────────────────────────────────────────────────────────────────────
  BOM
──────────────────────────────────────────────────────────────────────────────*/
uint8_t b_str_detect_bom(const void *data, size_t byte_len, size_t *bom_size_out);
// b_str_add_bom: for dynamic strings caller MUST reassign (s = b_str_add_bom(s))
// For static strings the pointer remains valid; written back in-place if fits.
b_str_t b_str_add_bom(b_str_t s);

/*──────────────────────────────────────────────────────────────────────────────
  FILE I/O
──────────────────────────────────────────────────────────────────────────────*/
b_str_t b_str_load_file(const char *path, uint8_t fallback_encoding);
int     b_str_save_file(const char *path, b_cstr_t s, bool write_bom);
int     b_file_add_bom (const char *path, uint8_t encoding);

/*──────────────────────────────────────────────────────────────────────────────
  FILE CONVERSION WRAPPERS
──────────────────────────────────────────────────────────────────────────────*/
int b_file_conv_ascii_to_utf8_bom            (const char *in, const char *out);
int b_file_conv_ascii_to_utf8_no_bom         (const char *in, const char *out);
int b_file_conv_utf8_to_utf16                (const char *in, const char *out);
int b_file_conv_utf8_to_utf16le_bom          (const char *in, const char *out);
int b_file_conv_utf8_to_utf16le_no_bom       (const char *in, const char *out);
int b_file_conv_utf8_to_utf16be_bom          (const char *in, const char *out);
int b_file_conv_utf8_to_utf16be_no_bom       (const char *in, const char *out);
int b_file_conv_utf8_to_utf32                (const char *in, const char *out);
int b_file_conv_utf8_to_utf32le_bom          (const char *in, const char *out);
int b_file_conv_utf8_to_utf32le_no_bom       (const char *in, const char *out);
int b_file_conv_utf8_to_utf32be_bom          (const char *in, const char *out);
int b_file_conv_utf8_to_utf32be_no_bom       (const char *in, const char *out);
int b_file_conv_utf16_to_utf8_no_bom         (const char *in, const char *out);
int b_file_conv_utf16le_bom_to_utf8_no_bom   (const char *in, const char *out);
int b_file_conv_utf16le_no_bom_to_utf8_bom   (const char *in, const char *out);
int b_file_conv_utf16le_no_bom_to_utf8_no_bom(const char *in, const char *out);
int b_file_conv_utf16be_bom_to_utf8_no_bom   (const char *in, const char *out);
int b_file_conv_utf16be_no_bom_to_utf8_bom   (const char *in, const char *out);
int b_file_conv_utf16be_no_bom_to_utf8_no_bom(const char *in, const char *out);
int b_file_conv_utf32_to_utf8_no_bom         (const char *in, const char *out);
int b_file_conv_utf32le_bom_to_utf8_no_bom   (const char *in, const char *out);
int b_file_conv_utf32le_no_bom_to_utf8_bom   (const char *in, const char *out);
int b_file_conv_utf32le_no_bom_to_utf8_no_bom(const char *in, const char *out);
int b_file_conv_utf32be_bom_to_utf8_no_bom   (const char *in, const char *out);
int b_file_conv_utf32be_no_bom_to_utf8_bom   (const char *in, const char *out);
int b_file_conv_utf32be_no_bom_to_utf8_no_bom(const char *in, const char *out);

/*──────────────────────────────────────────────────────────────────────────────
  UTF-16 STDOUT HELPER
──────────────────────────────────────────────────────────────────────────────*/
void b_str_print_utf16(b_u16slice_t slice);

/*──────────────────────────────────────────────────────────────────────────────
  PROPERTY MACROS
──────────────────────────────────────────────────────────────────────────────*/
#define B_STR_IS_STATIC_S(s)  ((s) && (((s)[-1] & B_STRING_STATIC_MASK) != 0u))
#define B_STR_IS_ALIGNED_S(s) ((s) && (((s)[-1] & B_STRING_PACK_MASK)   != 0u))
#define B_STR_NULL_SIZE(e) \
    (B_STR_IS_UTF32_ENC(e) ? 4u : B_STR_IS_UTF16_ENC(e) ? 2u : 1u)

#define B_STR_ENC_OF(s)      b_str_enc(s)
#define B_STR_IS_ASCII(s)    (b_str_enc(s) == B_STR_ENC_ASCII)
#define B_STR_IS_UTF8(s)     (b_str_enc(s) == B_STR_ENC_UTF8)
#define B_STR_IS_UTF16LE(s)  (b_str_enc(s) == B_STR_ENC_UTF16LE)
#define B_STR_IS_UTF16BE(s)  (b_str_enc(s) == B_STR_ENC_UTF16BE)
#define B_STR_IS_UTF16(s)    (B_STR_IS_UTF16_ENC(b_str_enc(s)))
#define B_STR_IS_UTF32LE(s)  (b_str_enc(s) == B_STR_ENC_UTF32LE)
#define B_STR_IS_UTF32BE(s)  (b_str_enc(s) == B_STR_ENC_UTF32BE)
#define B_STR_IS_UTF32(s)    (B_STR_IS_UTF32_ENC(b_str_enc(s)))
#define B_STR_IS_BYTE_ENC(s) (!B_STR_IS_UTF16(s) && !B_STR_IS_UTF32(s))

// Direct iteration helpers
#define B_STR_AS_U16(s)  ((const uint16_t*)(s))
#define B_STR_AS_U32(s)  ((const uint32_t*)(s))

/*──────────────────────────────────────────────────────────────────────────────
  SLICE HELPERS  (C++ uses aggregate brace-init; C99 uses compound literals)
──────────────────────────────────────────────────────────────────────────────*/
#ifdef __cplusplus
#  define B_SLICE_OF(s)    b_slice_t   {(s), b_str_len(s)}
#  define B_U8SLICE_OF(s)  b_u8slice_t {(s), b_str_len(s)}
#  define B_U16SLICE_OF(s) b_u16slice_t{(s), b_str_len(s)}
#  define B_U32SLICE_OF(s) b_u32slice_t{(s), b_str_len(s)}
#else
#  define B_SLICE_OF(s)    ((b_slice_t)   {(s), b_str_len(s)})
#  define B_U8SLICE_OF(s)  ((b_u8slice_t) {(s), b_str_len(s)})
#  define B_U16SLICE_OF(s) ((b_u16slice_t){(s), b_str_len(s)})
#  define B_U32SLICE_OF(s) ((b_u32slice_t){(s), b_str_len(s)})
#endif

#define B_SUBSLICE(s,o,l)    b_subslice((s),(o),(l))
#define B_SUBSLICE_CP(s,o,c) b_subslice_cp((s),(o),(c))

/*──────────────────────────────────────────────────────────────────────────────
  PRINTF FORMAT HELPERS
  B_STR_FMT / B_FMT_ARG / B_str_len / B_str_ptr / B_str_arg / B_PRINTF_SAFE
  WARNING: B_str_arg(X) evaluates X TWICE – avoid side-effecting expressions.
  Use B_PRINTF_SAFE for any expression that is not a simple variable.
──────────────────────────────────────────────────────────────────────────────*/
#define B_STR_FMT  "%.*s"

typedef struct b_printf_format_argument { int len; const char *ptr; } b_fmt_arg_t;

static inline b_fmt_arg_t _b_fmt_cstr(const char *s) {
    const size_t n = s ? strlen(s) : 0u;
    b_fmt_arg_t r; r.len = (n > (size_t)INT_MAX) ? INT_MAX : (int)n; r.ptr = s ? s : ""; return r;
}
static inline b_fmt_arg_t _b_fmt_str(const uint8_t *s) {
    const size_t n = b_str_len(s);
    b_fmt_arg_t r; r.len = (n > (size_t)INT_MAX) ? INT_MAX : (int)n; r.ptr = s ? (const char*)s : ""; return r;
}
static inline b_fmt_arg_t _b_fmt_sl(b_slice_t sl) {
    b_fmt_arg_t r; r.len = (sl.len > (size_t)INT_MAX) ? INT_MAX : (int)sl.len; r.ptr = sl.data ? (const char*)sl.data : ""; return r;
}
static inline b_fmt_arg_t _b_fmt_u8sl(b_u8slice_t sl) {
    b_fmt_arg_t r; r.len = (sl.len > (size_t)INT_MAX) ? INT_MAX : (int)sl.len; r.ptr = sl.data ? (const char*)sl.data : ""; return r;
}

#ifdef __cplusplus
// C++ overload set instead of _Generic
inline b_fmt_arg_t _b_fmt_arg_d(const char *s)     { return _b_fmt_cstr(s); }
inline b_fmt_arg_t _b_fmt_arg_d(char *s)            { return _b_fmt_cstr(s); }
inline b_fmt_arg_t _b_fmt_arg_d(const uint8_t *s)  { return _b_fmt_str(s); }
inline b_fmt_arg_t _b_fmt_arg_d(uint8_t *s)         { return _b_fmt_str(s); }
inline b_fmt_arg_t _b_fmt_arg_d(b_slice_t sl)       { return _b_fmt_sl(sl); }
inline b_fmt_arg_t _b_fmt_arg_d(b_u8slice_t sl)     { return _b_fmt_u8sl(sl); }
#  define B_FMT_ARG(X) _b_fmt_arg_d(X)
#else
#  define B_FMT_ARG(X) _Generic((X),               \
    char*:          _b_fmt_cstr,                    \
    const char*:    _b_fmt_cstr,                    \
    uint8_t*:       _b_fmt_str,                     \
    const uint8_t*: _b_fmt_str,                     \
    b_slice_t:      _b_fmt_sl,                      \
    b_u8slice_t:    _b_fmt_u8sl)(X)
#endif

#define B_str_len(X)  (B_FMT_ARG(X).len)
#define B_str_ptr(X)  (B_FMT_ARG(X).ptr)
#define B_str_arg(X)  B_str_len(X), B_str_ptr(X)  // WARNING: evaluates X twice

// Safe variant: evaluates expr exactly once
#define B_PRINTF_SAFE(fmt, expr)                              \
    do {                                                      \
        const b_fmt_arg_t _bsa_ = B_FMT_ARG(expr);           \
        printf((fmt), _bsa_.len, _bsa_.ptr);                  \
    } while (0)

#ifdef __cplusplus
}
#endif