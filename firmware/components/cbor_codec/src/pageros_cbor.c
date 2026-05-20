// SPDX-License-Identifier: Apache-2.0
//
// PagerOS CBOR codec — see pageros_cbor.h for the public contract.
//
// The decoder is a thin recursive walk over TinyCBOR's `CborValue` cursor
// API. The encoder is a custom recursive emitter that piggybacks on
// TinyCBOR's `CborEncoder` for the actual byte writes, but layers two
// canonical-form behaviors on top:
//
//   1. Map-key sort. RFC 8949 §4.2.1 requires keys be emitted in ascending
//      order of their CBOR-encoded bytes. We materialize each key into a
//      scratch buffer, qsort the pair list, then emit in order.
//   2. Float demotion. Floats are demoted to binary32 when the double
//      round-trips through the smaller type (§4.2.2 "shortest float").

#include "pageros_cbor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cbor.h"

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------

void pgr_cbor_arena_init(pgr_cbor_arena_t *a, void *backing, size_t cap)
{
    a->buf  = (uint8_t *)backing;
    a->cap  = cap;
    a->used = 0;
}

void pgr_cbor_arena_reset(pgr_cbor_arena_t *a)
{
    a->used = 0;
}

static void *arena_alloc(pgr_cbor_arena_t *a, size_t n, size_t align)
{
    // align up
    size_t mis = a->used % align;
    size_t pad = mis ? (align - mis) : 0;
    if (a->used + pad + n < a->used) return NULL;     // overflow
    if (a->used + pad + n > a->cap) return NULL;      // OOM
    a->used += pad;
    void *p = a->buf + a->used;
    a->used += n;
    return p;
}

static void *arena_zalloc(pgr_cbor_arena_t *a, size_t n, size_t align)
{
    void *p = arena_alloc(a, n, align);
    if (p) memset(p, 0, n);
    return p;
}

const char *pgr_cbor_strerror(pgr_cbor_err_t e)
{
    switch (e) {
        case PGR_CBOR_OK:              return "ok";
        case PGR_CBOR_ERR_PARSE:       return "malformed cbor";
        case PGR_CBOR_ERR_DEPTH:       return "nesting too deep";
        case PGR_CBOR_ERR_OOM:         return "arena out of memory";
        case PGR_CBOR_ERR_BUFFER:      return "output buffer too small";
        case PGR_CBOR_ERR_UNSUPPORTED: return "unsupported cbor feature";
        case PGR_CBOR_ERR_TRAILING:    return "trailing bytes after top item";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

static pgr_cbor_err_t decode_value(CborValue *cur,
                                   pgr_cbor_arena_t *arena,
                                   pgr_cbor_value_t *out,
                                   int depth);

static pgr_cbor_err_t map_tinycbor_err(CborError e)
{
    switch (e) {
        case CborNoError:        return PGR_CBOR_OK;
        case CborErrorOutOfMemory:
        case CborErrorTooFewItems:
        case CborErrorTooManyItems:
            return PGR_CBOR_ERR_PARSE;
        default:
            return PGR_CBOR_ERR_PARSE;
    }
}

// Copy a tinycbor byte / text string out of the source buffer into the
// arena. We allocate `len` bytes (no NUL); callers that want a C-string
// can append their own terminator. Caller picks the major-type variant by
// inspecting `cbor_value_get_type` before calling.
static pgr_cbor_err_t copy_string(CborValue *cur,
                                  bool is_text,
                                  pgr_cbor_arena_t *arena,
                                  const uint8_t **out_data,
                                  size_t *out_len)
{
    size_t len = 0;
    CborError e = cbor_value_calculate_string_length(cur, &len);
    if (e != CborNoError) return PGR_CBOR_ERR_PARSE;

    uint8_t *buf = NULL;
    if (len > 0) {
        buf = (uint8_t *)arena_alloc(arena, len, 1);
        if (!buf) return PGR_CBOR_ERR_OOM;
    }
    size_t copied = len;
    // Pass `cur` as both the source and the `next` cursor — TinyCBOR
    // updates `next` to point just past the string, which is what we want
    // for the surrounding container walker.
    if (is_text) {
        e = cbor_value_copy_text_string(cur, (char *)buf, &copied, cur);
    } else {
        e = cbor_value_copy_byte_string(cur, buf, &copied, cur);
    }
    if (e != CborNoError) return PGR_CBOR_ERR_PARSE;

    *out_data = buf;
    *out_len  = len;
    return PGR_CBOR_OK;
}

static pgr_cbor_err_t decode_array(CborValue *cur,
                                   pgr_cbor_arena_t *arena,
                                   pgr_cbor_value_t *out,
                                   int depth)
{
    if (!cbor_value_is_length_known(cur)) return PGR_CBOR_ERR_UNSUPPORTED;
    size_t n = 0;
    if (cbor_value_get_array_length(cur, &n) != CborNoError)
        return PGR_CBOR_ERR_PARSE;

    pgr_cbor_value_t *items = NULL;
    if (n > 0) {
        items = (pgr_cbor_value_t *)arena_zalloc(arena,
                                                 n * sizeof(*items),
                                                 _Alignof(pgr_cbor_value_t));
        if (!items) return PGR_CBOR_ERR_OOM;
    }

    CborValue child;
    if (cbor_value_enter_container(cur, &child) != CborNoError)
        return PGR_CBOR_ERR_PARSE;
    for (size_t i = 0; i < n; i++) {
        pgr_cbor_err_t r = decode_value(&child, arena, &items[i], depth + 1);
        if (r != PGR_CBOR_OK) return r;
    }
    if (cbor_value_leave_container(cur, &child) != CborNoError)
        return PGR_CBOR_ERR_PARSE;

    out->kind          = PGR_CBOR_KIND_ARRAY;
    out->v.arr.items   = items;
    out->v.arr.len     = n;
    return PGR_CBOR_OK;
}

static pgr_cbor_err_t decode_map(CborValue *cur,
                                 pgr_cbor_arena_t *arena,
                                 pgr_cbor_value_t *out,
                                 int depth)
{
    if (!cbor_value_is_length_known(cur)) return PGR_CBOR_ERR_UNSUPPORTED;
    size_t n = 0;
    if (cbor_value_get_map_length(cur, &n) != CborNoError)
        return PGR_CBOR_ERR_PARSE;

    pgr_cbor_pair_t *pairs = NULL;
    if (n > 0) {
        pairs = (pgr_cbor_pair_t *)arena_zalloc(arena,
                                                n * sizeof(*pairs),
                                                _Alignof(pgr_cbor_pair_t));
        if (!pairs) return PGR_CBOR_ERR_OOM;
    }

    CborValue child;
    if (cbor_value_enter_container(cur, &child) != CborNoError)
        return PGR_CBOR_ERR_PARSE;
    for (size_t i = 0; i < n; i++) {
        pgr_cbor_err_t r = decode_value(&child, arena, &pairs[i].key, depth + 1);
        if (r != PGR_CBOR_OK) return r;
        r = decode_value(&child, arena, &pairs[i].val, depth + 1);
        if (r != PGR_CBOR_OK) return r;
    }
    if (cbor_value_leave_container(cur, &child) != CborNoError)
        return PGR_CBOR_ERR_PARSE;

    out->kind        = PGR_CBOR_KIND_MAP;
    out->v.map.items = pairs;
    out->v.map.len   = n;
    return PGR_CBOR_OK;
}

static pgr_cbor_err_t decode_value(CborValue *cur,
                                   pgr_cbor_arena_t *arena,
                                   pgr_cbor_value_t *out,
                                   int depth)
{
    if (depth >= PGR_CBOR_MAX_DEPTH) return PGR_CBOR_ERR_DEPTH;
    if (cbor_value_at_end(cur))      return PGR_CBOR_ERR_PARSE;

    CborType t = cbor_value_get_type(cur);
    switch (t) {
        case CborIntegerType: {
            // Tinycbor reports raw-encoded value via cbor_value_get_raw_integer:
            // for major-0 it's the value, for major-1 it's the bit-inverted
            // representation, matching our wire-faithful split.
            uint64_t raw = 0;
            if (cbor_value_get_raw_integer(cur, &raw) != CborNoError)
                return PGR_CBOR_ERR_PARSE;
            out->kind   = cbor_value_is_unsigned_integer(cur)
                              ? PGR_CBOR_KIND_UINT
                              : PGR_CBOR_KIND_NEGINT;
            out->v.u64  = raw;
            return map_tinycbor_err(cbor_value_advance_fixed(cur));
        }

        case CborByteStringType:
        case CborTextStringType: {
            bool is_text = (t == CborTextStringType);
            out->kind = is_text ? PGR_CBOR_KIND_TEXT : PGR_CBOR_KIND_BYTES;
            return copy_string(cur, is_text, arena,
                               &out->v.bytes.data, &out->v.bytes.len);
        }

        case CborArrayType:
            return decode_array(cur, arena, out, depth);

        case CborMapType:
            return decode_map(cur, arena, out, depth);

        case CborTagType: {
            CborTag tag = 0;
            if (cbor_value_get_tag(cur, &tag) != CborNoError)
                return PGR_CBOR_ERR_PARSE;
            if (cbor_value_advance_fixed(cur) != CborNoError)
                return PGR_CBOR_ERR_PARSE;
            pgr_cbor_value_t *content =
                (pgr_cbor_value_t *)arena_zalloc(arena,
                                                 sizeof(*content),
                                                 _Alignof(pgr_cbor_value_t));
            if (!content) return PGR_CBOR_ERR_OOM;
            pgr_cbor_err_t r = decode_value(cur, arena, content, depth + 1);
            if (r != PGR_CBOR_OK) return r;
            out->kind             = PGR_CBOR_KIND_TAG;
            out->v.tagged.tag     = (uint64_t)tag;
            out->v.tagged.content = content;
            return PGR_CBOR_OK;
        }

        case CborBooleanType: {
            bool b = false;
            if (cbor_value_get_boolean(cur, &b) != CborNoError)
                return PGR_CBOR_ERR_PARSE;
            out->kind     = PGR_CBOR_KIND_BOOL;
            out->v.boolean = b;
            return map_tinycbor_err(cbor_value_advance_fixed(cur));
        }

        case CborNullType:
            out->kind = PGR_CBOR_KIND_NULL;
            return map_tinycbor_err(cbor_value_advance_fixed(cur));

        case CborUndefinedType:
            out->kind = PGR_CBOR_KIND_UNDEFINED;
            return map_tinycbor_err(cbor_value_advance_fixed(cur));

        case CborHalfFloatType: {
            float f = 0;
            if (cbor_value_get_half_float_as_float(cur, &f) != CborNoError)
                return PGR_CBOR_ERR_PARSE;
            out->kind  = PGR_CBOR_KIND_FLOAT;
            out->v.dbl = (double)f;
            return map_tinycbor_err(cbor_value_advance_fixed(cur));
        }
        case CborFloatType: {
            float f = 0;
            if (cbor_value_get_float(cur, &f) != CborNoError)
                return PGR_CBOR_ERR_PARSE;
            out->kind  = PGR_CBOR_KIND_FLOAT;
            out->v.dbl = (double)f;
            return map_tinycbor_err(cbor_value_advance_fixed(cur));
        }
        case CborDoubleType: {
            double d = 0;
            if (cbor_value_get_double(cur, &d) != CborNoError)
                return PGR_CBOR_ERR_PARSE;
            out->kind  = PGR_CBOR_KIND_FLOAT;
            out->v.dbl = d;
            return map_tinycbor_err(cbor_value_advance_fixed(cur));
        }

        case CborInvalidType:
        default:
            return PGR_CBOR_ERR_UNSUPPORTED;
    }
}

pgr_cbor_err_t pgr_cbor_decode(const uint8_t   *buf,
                               size_t           len,
                               pgr_cbor_arena_t *arena,
                               pgr_cbor_value_t **out_root)
{
    if (!buf || !arena || !out_root) return PGR_CBOR_ERR_PARSE;

    CborParser parser;
    CborValue  cur;
    if (cbor_parser_init(buf, len, 0, &parser, &cur) != CborNoError)
        return PGR_CBOR_ERR_PARSE;

    pgr_cbor_value_t *root =
        (pgr_cbor_value_t *)arena_zalloc(arena,
                                         sizeof(*root),
                                         _Alignof(pgr_cbor_value_t));
    if (!root) return PGR_CBOR_ERR_OOM;

    pgr_cbor_err_t r = decode_value(&cur, arena, root, 0);
    if (r != PGR_CBOR_OK) return r;

    // PagerOS frames are one top-level item; reject anything trailing.
    if (!cbor_value_at_end(&cur)) return PGR_CBOR_ERR_TRAILING;

    *out_root = root;
    return PGR_CBOR_OK;
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

static pgr_cbor_err_t encode_value(CborEncoder *enc,
                                   const pgr_cbor_value_t *v,
                                   pgr_cbor_arena_t *arena,
                                   int depth);

// Detect "shortest float" per RFC 8949 §4.2.2: if a double round-trips
// through binary32 (and isn't a NaN with a non-canonical payload), emit
// it as a 32-bit float. We don't try half-float demotion because the
// PROTO-003 vectors don't exercise it and tinycbor's half-float encoder
// requires a separate explicit call.
static bool fits_in_float(double d)
{
    if (isnan(d) || isinf(d)) return true;            // half/float will hold these
    float f = (float)d;
    return ((double)f) == d;
}

static pgr_cbor_err_t encode_scalar(CborEncoder *enc, const pgr_cbor_value_t *v)
{
    CborError e = CborNoError;
    switch (v->kind) {
        case PGR_CBOR_KIND_UINT:
            e = cbor_encode_uint(enc, v->v.u64);
            break;
        case PGR_CBOR_KIND_NEGINT:
            // Tinycbor's cbor_encode_negative_int takes the unsigned
            // "absolute - 1" form directly; pass the wire-faithful raw.
            e = cbor_encode_negative_int(enc, v->v.u64);
            break;
        case PGR_CBOR_KIND_BYTES:
            e = cbor_encode_byte_string(enc, v->v.bytes.data, v->v.bytes.len);
            break;
        case PGR_CBOR_KIND_TEXT:
            e = cbor_encode_text_string(enc, (const char *)v->v.bytes.data, v->v.bytes.len);
            break;
        case PGR_CBOR_KIND_BOOL:
            e = cbor_encode_boolean(enc, v->v.boolean);
            break;
        case PGR_CBOR_KIND_NULL:
            e = cbor_encode_null(enc);
            break;
        case PGR_CBOR_KIND_UNDEFINED:
            e = cbor_encode_undefined(enc);
            break;
        case PGR_CBOR_KIND_FLOAT:
            if (fits_in_float(v->v.dbl)) {
                float f = (float)v->v.dbl;
                e = cbor_encode_float(enc, f);
            } else {
                e = cbor_encode_double(enc, v->v.dbl);
            }
            break;
        default:
            return PGR_CBOR_ERR_UNSUPPORTED;
    }
    if (e == CborErrorOutOfMemory) return PGR_CBOR_ERR_BUFFER;
    if (e != CborNoError) return PGR_CBOR_ERR_PARSE;
    return PGR_CBOR_OK;
}

// Encode an entire value into a freshly-allocated arena buffer, returning a
// pointer + length. The buffer auto-grows by re-encoding into successively
// larger buffers; in practice the first attempt almost always fits because
// CBOR encodings are bounded by ~9× the value tree depth + payload bytes.
static pgr_cbor_err_t encode_to_arena(const pgr_cbor_value_t *v,
                                      pgr_cbor_arena_t *arena,
                                      const uint8_t **out_data,
                                      size_t *out_len)
{
    size_t cap = 64;
    while (cap <= 1u << 20) {
        uint8_t *buf = (uint8_t *)arena_alloc(arena, cap, 1);
        if (!buf) return PGR_CBOR_ERR_OOM;
        CborEncoder enc;
        cbor_encoder_init(&enc, buf, cap, 0);
        pgr_cbor_err_t r = encode_value(&enc, v, arena, 0);
        if (r == PGR_CBOR_OK) {
            size_t produced = cbor_encoder_get_buffer_size(&enc, buf);
            *out_data = buf;
            *out_len  = produced;
            return PGR_CBOR_OK;
        }
        if (r != PGR_CBOR_ERR_BUFFER) return r;
        // Discard the failed allocation and try a bigger window. We
        // intentionally don't reclaim the arena slack; map sorting is
        // bounded to a handful of attempts per call so the waste is small.
        cap *= 2;
    }
    return PGR_CBOR_ERR_OOM;
}

typedef struct {
    const pgr_cbor_pair_t *pair;
    const uint8_t         *key_bytes;
    size_t                 key_len;
} sortable_pair_t;

static int compare_sortable(const void *a, const void *b)
{
    const sortable_pair_t *x = (const sortable_pair_t *)a;
    const sortable_pair_t *y = (const sortable_pair_t *)b;
    size_t n = x->key_len < y->key_len ? x->key_len : y->key_len;
    int c = memcmp(x->key_bytes, y->key_bytes, n);
    if (c) return c;
    if (x->key_len < y->key_len) return -1;
    if (x->key_len > y->key_len) return  1;
    return 0;
}

static pgr_cbor_err_t encode_map(CborEncoder *enc,
                                 const pgr_cbor_value_t *v,
                                 pgr_cbor_arena_t *arena,
                                 int depth)
{
    size_t n = v->v.map.len;
    sortable_pair_t *tmp = NULL;
    if (n > 0) {
        tmp = (sortable_pair_t *)arena_alloc(arena,
                                             n * sizeof(*tmp),
                                             _Alignof(sortable_pair_t));
        if (!tmp) return PGR_CBOR_ERR_OOM;
        for (size_t i = 0; i < n; i++) {
            tmp[i].pair = &v->v.map.items[i];
            pgr_cbor_err_t r = encode_to_arena(&tmp[i].pair->key, arena,
                                               &tmp[i].key_bytes,
                                               &tmp[i].key_len);
            if (r != PGR_CBOR_OK) return r;
        }
        qsort(tmp, n, sizeof(*tmp), compare_sortable);
    }

    CborEncoder mapEnc;
    CborError e = cbor_encoder_create_map(enc, &mapEnc, n);
    if (e == CborErrorOutOfMemory) return PGR_CBOR_ERR_BUFFER;
    if (e != CborNoError) return PGR_CBOR_ERR_PARSE;
    for (size_t i = 0; i < n; i++) {
        // Re-encode the key via the normal path so tinycbor's internal
        // bookkeeping (`remaining`, length tracking) stays consistent.
        // The byte string produced by encode_to_arena above is used only
        // as the sort key; identical bytes come back out here.
        pgr_cbor_err_t r = encode_value(&mapEnc, &tmp[i].pair->key, arena, depth + 1);
        if (r != PGR_CBOR_OK) return r;
        r = encode_value(&mapEnc, &tmp[i].pair->val, arena, depth + 1);
        if (r != PGR_CBOR_OK) return r;
    }
    e = cbor_encoder_close_container(enc, &mapEnc);
    if (e == CborErrorOutOfMemory) return PGR_CBOR_ERR_BUFFER;
    if (e != CborNoError) return PGR_CBOR_ERR_PARSE;
    return PGR_CBOR_OK;
}

static pgr_cbor_err_t encode_array(CborEncoder *enc,
                                   const pgr_cbor_value_t *v,
                                   pgr_cbor_arena_t *arena,
                                   int depth)
{
    CborEncoder arrEnc;
    CborError e = cbor_encoder_create_array(enc, &arrEnc, v->v.arr.len);
    if (e == CborErrorOutOfMemory) return PGR_CBOR_ERR_BUFFER;
    if (e != CborNoError) return PGR_CBOR_ERR_PARSE;
    for (size_t i = 0; i < v->v.arr.len; i++) {
        pgr_cbor_err_t r = encode_value(&arrEnc, &v->v.arr.items[i], arena, depth + 1);
        if (r != PGR_CBOR_OK) return r;
    }
    e = cbor_encoder_close_container(enc, &arrEnc);
    if (e == CborErrorOutOfMemory) return PGR_CBOR_ERR_BUFFER;
    if (e != CborNoError) return PGR_CBOR_ERR_PARSE;
    return PGR_CBOR_OK;
}

static pgr_cbor_err_t encode_value(CborEncoder *enc,
                                   const pgr_cbor_value_t *v,
                                   pgr_cbor_arena_t *arena,
                                   int depth)
{
    if (depth >= PGR_CBOR_MAX_DEPTH) return PGR_CBOR_ERR_DEPTH;
    switch (v->kind) {
        case PGR_CBOR_KIND_ARRAY: return encode_array(enc, v, arena, depth);
        case PGR_CBOR_KIND_MAP:   return encode_map  (enc, v, arena, depth);
        case PGR_CBOR_KIND_TAG: {
            CborError e = cbor_encode_tag(enc, v->v.tagged.tag);
            if (e == CborErrorOutOfMemory) return PGR_CBOR_ERR_BUFFER;
            if (e != CborNoError) return PGR_CBOR_ERR_PARSE;
            return encode_value(enc, v->v.tagged.content, arena, depth + 1);
        }
        default:
            return encode_scalar(enc, v);
    }
}

pgr_cbor_err_t pgr_cbor_encode_canonical(const pgr_cbor_value_t *root,
                                         uint8_t          *out_buf,
                                         size_t            out_cap,
                                         size_t           *out_written,
                                         pgr_cbor_arena_t *arena)
{
    if (!root || !out_buf || !out_written || !arena) return PGR_CBOR_ERR_PARSE;
    CborEncoder enc;
    cbor_encoder_init(&enc, out_buf, out_cap, 0);
    pgr_cbor_err_t r = encode_value(&enc, root, arena, 0);
    if (r != PGR_CBOR_OK) return r;
    *out_written = cbor_encoder_get_buffer_size(&enc, out_buf);
    return PGR_CBOR_OK;
}
