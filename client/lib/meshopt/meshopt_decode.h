/*
 * meshopt_decode.h: the EXT_meshopt_compression / KHR_meshopt_compression decoders in C11.
 *
 * A C port of the decoding half of meshoptimizer (https://github.com/zeux/meshoptimizer,
 * MIT License, Copyright (c) 2016-2025 Arseny Kapoulkine; client/lib/meshopt/LICENSE.md):
 * vertexcodec.cpp meshopt_decodeVertexBuffer (versions 0 and 1), indexcodec.cpp
 * meshopt_decodeIndexBuffer / meshopt_decodeIndexSequence (versions 0 and 1) and vertexfilter.cpp
 * meshopt_decodeFilterOct / Quat / Exp. The codecs are the scalar decoders, which are lossless
 * (bit-identical to every SIMD path). The filters follow the WebAssembly SIMD decoder (the one the
 * meshoptimizer npm package, and so Krunker's three.js, runs in a browser): the same float
 * operations in the same order, rounding to nearest even with the 3 << 22 snap, sqrt + divide.
 * The scalar C++ filters round halves away from zero and reorder a sum, so on rare inputs they
 * differ from the browser by one unit; this port matches the browser bit for bit.
 *
 * Single-file: define MESHOPT_DECODE_IMPLEMENTATION in exactly one C file before including it.
 */
#ifndef MESHOPT_DECODE_H
#define MESHOPT_DECODE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The bufferView "mode" and "filter" of the extension. */
enum { MESHOPT_MODE_ATTRIBUTES = 0, MESHOPT_MODE_TRIANGLES = 1, MESHOPT_MODE_INDICES = 2 };
enum { MESHOPT_FILTER_NONE = 0, MESHOPT_FILTER_OCTAHEDRAL = 1, MESHOPT_FILTER_QUATERNION = 2, MESHOPT_FILTER_EXPONENTIAL = 3 };

/* Each returns 0 on success, < 0 on malformed input (-1 bad header / version, -2 truncated,
 * -3 trailing bytes, -4 bad arguments), as meshoptimizer does. dst holds count * size bytes. */
int meshopt_decode_vertex_buffer(void *dst, size_t count, size_t size, const unsigned char *src, size_t src_size);
int meshopt_decode_index_buffer(void *dst, size_t count, size_t size, const unsigned char *src, size_t src_size);
int meshopt_decode_index_sequence(void *dst, size_t count, size_t size, const unsigned char *src, size_t src_size);

/* In-place filters over count elements of `stride` bytes (oct: 4 or 8, quat: 8, exp: 4n). */
void meshopt_decode_filter_oct(void *buffer, size_t count, size_t stride);
void meshopt_decode_filter_quat(void *buffer, size_t count, size_t stride);
void meshopt_decode_filter_exp(void *buffer, size_t count, size_t stride);

/* One compressed bufferView: the mode's codec, then the filter (attributes only). Checks the
 * extension's constraints on stride / count first (-4 when they do not hold). */
int meshopt_decode_gltf_buffer(void *dst, size_t count, size_t stride, const unsigned char *src, size_t src_size, int mode, int filter);

#ifdef __cplusplus
}
#endif

#endif /* MESHOPT_DECODE_H */

#ifdef MESHOPT_DECODE_IMPLEMENTATION
#ifndef MESHOPT_DECODE_IMPLEMENTED
#define MESHOPT_DECODE_IMPLEMENTED

#include <math.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- vertex codec */

#define MESHOPT_VERTEX_HEADER 0xa0
#define MESHOPT_VERTEX_VERSION 1
#define MESHOPT_VERTEX_BLOCK_SIZE_BYTES 8192
#define MESHOPT_VERTEX_BLOCK_MAX_SIZE 256
#define MESHOPT_BYTE_GROUP_SIZE 16
#define MESHOPT_BYTE_GROUP_DECODE_LIMIT 24
#define MESHOPT_TAIL_MIN_SIZE_V0 32
#define MESHOPT_TAIL_MIN_SIZE_V1 24

static const int meshopt_bits_v0[4] = {0, 2, 4, 8};
static const int meshopt_bits_v1[5] = {0, 1, 2, 4, 8};

static size_t meshopt_vertex_block_size(const size_t vertex_size) {
    /* the whole block fits the 8 KB scratch buffer, a multiple of the byte group size */
    const size_t result = (MESHOPT_VERTEX_BLOCK_SIZE_BYTES / vertex_size) & ~(size_t) (MESHOPT_BYTE_GROUP_SIZE - 1);
    return result < MESHOPT_VERTEX_BLOCK_MAX_SIZE ? result : MESHOPT_VERTEX_BLOCK_MAX_SIZE;
}

/* 16 values of `bits` bits; the all-ones value means "the next byte of the variable part" */
static const unsigned char *meshopt_decode_bytes_group(const unsigned char *data, unsigned char *buffer, const int bits) {
    if (bits == 0) {
        memset(buffer, 0, MESHOPT_BYTE_GROUP_SIZE);
        return data;
    }
    if (bits == 8) {
        memcpy(buffer, data, MESHOPT_BYTE_GROUP_SIZE);
        return data + MESHOPT_BYTE_GROUP_SIZE;
    }
    const unsigned char *data_var = data + MESHOPT_BYTE_GROUP_SIZE * (size_t) bits / 8;
    const unsigned int sentinel = (1u << bits) - 1;
    for (int i = 0; i < MESHOPT_BYTE_GROUP_SIZE; i++) {
        const unsigned int byte = data[i * bits / 8];
        /* 1-bit groups are stored least significant bit first, 2 and 4 bits most significant first */
        const unsigned int enc = bits == 1 ? (byte >> (i & 7)) & 1 : (byte >> (8 - bits - (i * bits) % 8)) & sentinel;
        if (enc == sentinel) buffer[i] = *data_var++;
        else buffer[i] = (unsigned char) enc;
    }
    return data_var;
}

static const unsigned char *meshopt_decode_bytes(const unsigned char *data, const unsigned char *data_end, unsigned char *buffer,
                                                 const size_t buffer_size, const int *bits) {
    /* 2 header bits per group, rounded up to whole bytes */
    const size_t header_size = (buffer_size / MESHOPT_BYTE_GROUP_SIZE + 3) / 4;
    if ((size_t) (data_end - data) < header_size) return NULL;
    const unsigned char *header = data;
    data += header_size;
    for (size_t i = 0; i < buffer_size; i += MESHOPT_BYTE_GROUP_SIZE) {
        if ((size_t) (data_end - data) < MESHOPT_BYTE_GROUP_DECODE_LIMIT) return NULL;
        const size_t group = i / MESHOPT_BYTE_GROUP_SIZE;
        const int bitsk = (header[group / 4] >> ((group % 4) * 2)) & 3;
        data = meshopt_decode_bytes_group(data, buffer + i, bits[bitsk]);
    }
    return data;
}

static unsigned int meshopt_rotate(const unsigned int v, const int r) {
    return (v << r) | (v >> ((32 - r) & 31));
}

/* channel 0: bytes, zigzag deltas */
static void meshopt_deltas8(const unsigned char *buffer, unsigned char *transposed, const size_t vertex_count, const size_t vertex_size,
                            const unsigned char *last_vertex) {
    for (size_t k = 0; k < 4; k++) {
        unsigned char p = last_vertex[k];
        size_t offset = k;
        for (size_t i = 0; i < vertex_count; i++) {
            const unsigned char v = buffer[k * vertex_count + i];
            const unsigned char d = (unsigned char) ((0u - (v & 1u)) ^ (unsigned int) (v >> 1));
            p = (unsigned char) (d + p);
            transposed[offset] = p;
            offset += vertex_size;
        }
    }
}

/* channel 1: 16-bit words, zigzag deltas */
static void meshopt_deltas16(const unsigned char *buffer, unsigned char *transposed, const size_t vertex_count, const size_t vertex_size,
                             const unsigned char *last_vertex) {
    for (size_t k = 0; k < 4; k += 2) {
        uint16_t p = (uint16_t) (last_vertex[k] | last_vertex[k + 1] << 8);
        const unsigned char *lo = buffer + k * vertex_count, *hi = lo + vertex_count;
        size_t offset = k;
        for (size_t i = 0; i < vertex_count; i++) {
            const uint16_t v = (uint16_t) (lo[i] | hi[i] << 8);
            const uint16_t d = (uint16_t) ((0u - (v & 1u)) ^ (unsigned int) (v >> 1));
            p = (uint16_t) (d + p);
            transposed[offset] = (unsigned char) p;
            transposed[offset + 1] = (unsigned char) (p >> 8);
            offset += vertex_size;
        }
    }
}

/* channel 2: 32-bit words, rotated xor deltas */
static void meshopt_deltas_xor(const unsigned char *buffer, unsigned char *transposed, const size_t vertex_count, const size_t vertex_size,
                               const unsigned char *last_vertex, const int rot) {
    uint32_t p = (uint32_t) last_vertex[0] | (uint32_t) last_vertex[1] << 8 | (uint32_t) last_vertex[2] << 16 | (uint32_t) last_vertex[3] << 24;
    size_t offset = 0;
    for (size_t i = 0; i < vertex_count; i++) {
        uint32_t v = (uint32_t) buffer[i] | (uint32_t) buffer[i + vertex_count] << 8 | (uint32_t) buffer[i + vertex_count * 2] << 16 |
                     (uint32_t) buffer[i + vertex_count * 3] << 24;
        v = meshopt_rotate(v, rot) ^ p;
        transposed[offset + 0] = (unsigned char) v;
        transposed[offset + 1] = (unsigned char) (v >> 8);
        transposed[offset + 2] = (unsigned char) (v >> 16);
        transposed[offset + 3] = (unsigned char) (v >> 24);
        p = v;
        offset += vertex_size;
    }
}

static const unsigned char *meshopt_decode_vertex_block(const unsigned char *data, const unsigned char *data_end, unsigned char *vertex_data,
                                                        const size_t vertex_count, const size_t vertex_size, unsigned char last_vertex[256],
                                                        const unsigned char *channels, const int version) {
    unsigned char buffer[MESHOPT_VERTEX_BLOCK_MAX_SIZE * 4];
    unsigned char transposed[MESHOPT_VERTEX_BLOCK_SIZE_BYTES];
    const size_t vertex_count_aligned = (vertex_count + MESHOPT_BYTE_GROUP_SIZE - 1) & ~(size_t) (MESHOPT_BYTE_GROUP_SIZE - 1);

    const size_t control_size = version == 0 ? 0 : vertex_size / 4;
    if ((size_t) (data_end - data) < control_size) return NULL;
    const unsigned char *control = data;
    data += control_size;

    for (size_t k = 0; k < vertex_size; k += 4) {
        const unsigned char ctrl_byte = version == 0 ? 0 : control[k / 4];
        for (size_t j = 0; j < 4; j++) {
            const int ctrl = (ctrl_byte >> (j * 2)) & 3;
            if (ctrl == 3) { /* literal */
                if ((size_t) (data_end - data) < vertex_count) return NULL;
                memcpy(buffer + j * vertex_count, data, vertex_count);
                data += vertex_count;
            } else if (ctrl == 2) { /* zero */
                memset(buffer + j * vertex_count, 0, vertex_count);
            } else {
                data = meshopt_decode_bytes(data, data_end, buffer + j * vertex_count, vertex_count_aligned,
                                            version == 0 ? meshopt_bits_v0 : meshopt_bits_v1 + ctrl);
                if (!data) return NULL;
            }
        }
        const int channel = version == 0 ? 0 : channels[k / 4];
        switch (channel & 3) {
        case 0: meshopt_deltas8(buffer, transposed + k, vertex_count, vertex_size, last_vertex + k); break;
        case 1: meshopt_deltas16(buffer, transposed + k, vertex_count, vertex_size, last_vertex + k); break;
        case 2: meshopt_deltas_xor(buffer, transposed + k, vertex_count, vertex_size, last_vertex + k, (32 - (channel >> 4)) & 31); break;
        default: return NULL; /* invalid channel type */
        }
    }
    memcpy(vertex_data, transposed, vertex_count * vertex_size);
    memcpy(last_vertex, &transposed[vertex_size * (vertex_count - 1)], vertex_size);
    return data;
}

int meshopt_decode_vertex_buffer(void *dst, const size_t count, const size_t size, const unsigned char *src, const size_t src_size) {
    if (!dst || !src || size == 0 || size > 256 || size % 4 != 0) return -4;
    unsigned char *vertex_data = (unsigned char *) dst;
    const unsigned char *data = src, *data_end = src + src_size;
    if (src_size < 1) return -2;
    const unsigned char header = *data++;
    if ((header & 0xf0) != MESHOPT_VERTEX_HEADER) return -1;
    const int version = header & 0x0f;
    if (version > MESHOPT_VERTEX_VERSION) return -1;

    const size_t tail_size = size + (version == 0 ? 0 : size / 4);
    const size_t tail_min = version == 0 ? MESHOPT_TAIL_MIN_SIZE_V0 : MESHOPT_TAIL_MIN_SIZE_V1;
    const size_t tail_size_pad = tail_size < tail_min ? tail_min : tail_size;
    if ((size_t) (data_end - data) < tail_size_pad) return -2;

    const unsigned char *tail = data_end - tail_size;
    unsigned char last_vertex[256];
    memcpy(last_vertex, tail, size);
    const unsigned char *channels = version == 0 ? NULL : tail + size;

    const size_t block_size = meshopt_vertex_block_size(size);
    for (size_t offset = 0; offset < count;) {
        const size_t n = offset + block_size < count ? block_size : count - offset;
        data = meshopt_decode_vertex_block(data, data_end, vertex_data + offset * size, n, size, last_vertex, channels, version);
        if (!data) return -2;
        offset += n;
    }
    if ((size_t) (data_end - data) != tail_size_pad) return -3;
    return 0;
}

/* ---------------------------------------------------------------- index codec */

#define MESHOPT_INDEX_HEADER 0xe0
#define MESHOPT_SEQUENCE_HEADER 0xd0
#define MESHOPT_INDEX_VERSION 1

static unsigned int meshopt_decode_vbyte(const unsigned char **data) {
    const unsigned char *p = *data;
    const unsigned char lead = *p++;
    if (lead < 128) {
        *data = p;
        return lead;
    }
    /* up to 4 more 7-bit groups; always terminates on malformed data */
    unsigned int result = lead & 127, shift = 7;
    for (int i = 0; i < 4; i++) {
        const unsigned char group = *p++;
        result |= (unsigned int) (group & 127) << shift;
        shift += 7;
        if (group < 128) break;
    }
    *data = p;
    return result;
}

static unsigned int meshopt_decode_index(const unsigned char **data, const unsigned int last) {
    const unsigned int v = meshopt_decode_vbyte(data);
    const unsigned int d = (v >> 1) ^ (0u - (v & 1));
    return last + d;
}

static void meshopt_write_triangle(void *dst, const size_t offset, const size_t index_size, const unsigned int a, const unsigned int b,
                                   const unsigned int c) {
    if (index_size == 2) {
        uint16_t *out = (uint16_t *) dst + offset;
        out[0] = (uint16_t) a, out[1] = (uint16_t) b, out[2] = (uint16_t) c;
    } else {
        uint32_t *out = (uint32_t *) dst + offset;
        out[0] = a, out[1] = b, out[2] = c;
    }
}

#define MESHOPT_PUSH_EDGE(a_, b_) (edgefifo[edgefifooffset][0] = (a_), edgefifo[edgefifooffset][1] = (b_), edgefifooffset = (edgefifooffset + 1) & 15)
#define MESHOPT_PUSH_VERTEX(v_, cond_) (vertexfifo[vertexfifooffset] = (v_), vertexfifooffset = (vertexfifooffset + (cond_)) & 15)

int meshopt_decode_index_buffer(void *dst, const size_t count, const size_t size, const unsigned char *src, const size_t src_size) {
    if (!dst || !src || count % 3 != 0 || (size != 2 && size != 4)) return -4;
    /* the smallest valid encoding: the header, 1 byte per triangle and the 16-byte codeaux table */
    if (src_size < 1 + count / 3 + 16) return -2;
    if ((src[0] & 0xf0) != MESHOPT_INDEX_HEADER) return -1;
    const int version = src[0] & 0x0f;
    if (version > MESHOPT_INDEX_VERSION) return -1;

    unsigned int edgefifo[16][2], vertexfifo[16];
    memset(edgefifo, -1, sizeof(edgefifo));
    memset(vertexfifo, -1, sizeof(vertexfifo));
    size_t edgefifooffset = 0, vertexfifooffset = 0;
    unsigned int next = 0, last = 0;
    const int fecmax = version >= 1 ? 13 : 15;

    const unsigned char *code = src + 1;
    const unsigned char *data = code + count / 3;
    const unsigned char *data_safe_end = src + src_size - 16;
    const unsigned char *codeaux_table = data_safe_end;

    for (size_t i = 0; i < count; i += 3) {
        /* a triangle reads at most 16 bytes of data, and the codeaux table is 16 bytes */
        if (data > data_safe_end) return -2;
        const unsigned char codetri = *code++;
        if (codetri < 0xf0) {
            const int fe = codetri >> 4;
            const unsigned int a = edgefifo[(edgefifooffset - 1 - (size_t) fe) & 15][0];
            const unsigned int b = edgefifo[(edgefifooffset - 1 - (size_t) fe) & 15][1];
            unsigned int c;
            const int fec = codetri & 15;
            if (fec < fecmax) {
                const unsigned int cf = vertexfifo[(vertexfifooffset - 1 - (size_t) fec) & 15];
                c = fec == 0 ? next : cf;
                const int fec0 = fec == 0;
                next += (unsigned int) fec0;
                MESHOPT_PUSH_VERTEX(c, (size_t) fec0);
            } else {
                /* 13, 14: last -1 / +1; 15: a free (delta coded) index */
                last = c = fec != 15 ? last + (unsigned int) (fec - (fec ^ 3)) : meshopt_decode_index(&data, last);
                MESHOPT_PUSH_VERTEX(c, 1);
            }
            MESHOPT_PUSH_EDGE(c, b);
            MESHOPT_PUSH_EDGE(a, c);
            meshopt_write_triangle(dst, i, size, a, b, c);
        } else if (codetri < 0xfe) {
            const unsigned char codeaux = codeaux_table[codetri & 15];
            const int feb = codeaux >> 4, fec = codeaux & 15;
            const unsigned int a = next++;
            const unsigned int bf = vertexfifo[(vertexfifooffset - (size_t) feb) & 15];
            const unsigned int b = feb == 0 ? next : bf;
            const int feb0 = feb == 0;
            next += (unsigned int) feb0;
            const unsigned int cf = vertexfifo[(vertexfifooffset - (size_t) fec) & 15];
            const unsigned int c = fec == 0 ? next : cf;
            const int fec0 = fec == 0;
            next += (unsigned int) fec0;
            meshopt_write_triangle(dst, i, size, a, b, c);
            MESHOPT_PUSH_VERTEX(a, 1);
            MESHOPT_PUSH_VERTEX(b, (size_t) feb0);
            MESHOPT_PUSH_VERTEX(c, (size_t) fec0);
            MESHOPT_PUSH_EDGE(b, a);
            MESHOPT_PUSH_EDGE(c, b);
            MESHOPT_PUSH_EDGE(a, c);
        } else {
            const unsigned char codeaux = *data++;
            const int fea = codetri == 0xfe ? 0 : 15;
            const int feb = codeaux >> 4, fec = codeaux & 15;
            if (codeaux == 0) next = 0; /* reset */
            unsigned int a = fea == 0 ? next++ : 0;
            unsigned int b = feb == 0 ? next++ : vertexfifo[(vertexfifooffset - (size_t) feb) & 15];
            unsigned int c = fec == 0 ? next++ : vertexfifo[(vertexfifooffset - (size_t) fec) & 15];
            if (fea == 15) last = a = meshopt_decode_index(&data, last);
            if (feb == 15) last = b = meshopt_decode_index(&data, last);
            if (fec == 15) last = c = meshopt_decode_index(&data, last);
            meshopt_write_triangle(dst, i, size, a, b, c);
            MESHOPT_PUSH_VERTEX(a, 1);
            MESHOPT_PUSH_VERTEX(b, (size_t) ((feb == 0) | (feb == 15)));
            MESHOPT_PUSH_VERTEX(c, (size_t) ((fec == 0) | (fec == 15)));
            MESHOPT_PUSH_EDGE(b, a);
            MESHOPT_PUSH_EDGE(c, b);
            MESHOPT_PUSH_EDGE(a, c);
        }
    }
    /* all data read, stopped at the codeaux table */
    if (data != data_safe_end) return -3;
    return 0;
}

#undef MESHOPT_PUSH_EDGE
#undef MESHOPT_PUSH_VERTEX

int meshopt_decode_index_sequence(void *dst, const size_t count, const size_t size, const unsigned char *src, const size_t src_size) {
    if (!dst || !src || (size != 2 && size != 4)) return -4;
    /* the smallest valid encoding: the header, 1 byte per index and a 4-byte tail */
    if (src_size < 1 + count + 4) return -2;
    if ((src[0] & 0xf0) != MESHOPT_SEQUENCE_HEADER) return -1;
    const int version = src[0] & 0x0f;
    if (version > MESHOPT_INDEX_VERSION) return -1;

    const unsigned char *data = src + 1;
    const unsigned char *data_safe_end = src + src_size - 4;
    unsigned int last[2] = {0, 0};
    for (size_t i = 0; i < count; i++) {
        /* an index reads at most 5 bytes; the 4-byte tail follows data_safe_end */
        if (data >= data_safe_end) return -2;
        unsigned int v = meshopt_decode_vbyte(&data);
        const unsigned int current = v & 1; /* the baseline */
        v >>= 1;
        const unsigned int d = (v >> 1) ^ (0u - (v & 1));
        const unsigned int index = last[current] + d;
        last[current] = index;
        if (size == 2) ((uint16_t *) dst)[i] = (uint16_t) index;
        else ((uint32_t *) dst)[i] = index;
    }
    if (data != data_safe_end) return -3;
    return 0;
}

/* ---------------------------------------------------------------- filters (WebAssembly SIMD order) */

/* Products and sums stay separately rounded floats: no FMA contraction (aarch64 / -ffp-contract)
 * and no excess precision, so every platform computes what f32x4 computes. */
static float meshopt_fmul(const float a, const float b) {
    volatile float r = a * b;
    return r;
}

static float meshopt_fadd(const float a, const float b) {
    volatile float r = a + b;
    return r;
}

static uint32_t meshopt_fbits(const float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static float meshopt_bitsf(const uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

/* f32x4.add(v, 3 << 22): the low bits of the sum are the value rounded to nearest even */
static uint32_t meshopt_snap(const float v) {
    return meshopt_fbits(meshopt_fadd(v, (float) (3 << 22)));
}

/* x + (t ^ (x & -0.f)): x + t for x >= +0, x - t for negative x */
static float meshopt_oct_fix(const float x, const float t) {
    return meshopt_fadd(x, meshopt_bitsf(meshopt_fbits(t) ^ (meshopt_fbits(x) & 0x80000000u)));
}

/* i32x4.min(z, 0) on float bits: z when its sign bit is set, else +0 */
static float meshopt_min0(const float z) {
    return (int32_t) meshopt_fbits(z) < 0 ? z : 0.0f;
}

void meshopt_decode_filter_oct(void *buffer, const size_t count, const size_t stride) {
    const float max = stride == 4 ? 127.0f : 32767.0f;
    for (size_t i = 0; i < count; i++) {
        float x, y, z;
        if (stride == 4) {
            const signed char *n = (const signed char *) buffer + i * 4;
            x = (float) n[0], y = (float) n[1], z = (float) n[2];
        } else {
            const int16_t *n = (const int16_t *) buffer + i * 4;
            x = (float) n[0], y = (float) n[1], z = (float) (n[2] & 0x7fff);
        }
        z = z - meshopt_fadd(fabsf(x), fabsf(y));
        const float t = meshopt_min0(z);
        x = meshopt_oct_fix(x, t);
        y = meshopt_oct_fix(y, t);
        const float ll = meshopt_fadd(meshopt_fmul(x, x), meshopt_fadd(meshopt_fmul(y, y), meshopt_fmul(z, z)));
        const float s = max / sqrtf(ll);
        const uint32_t xr = meshopt_snap(meshopt_fmul(x, s));
        const uint32_t yr = meshopt_snap(meshopt_fmul(y, s));
        const uint32_t zr = meshopt_snap(meshopt_fmul(z, s));
        if (stride == 4) {
            signed char *n = (signed char *) buffer + i * 4;
            n[0] = (signed char) (int8_t) (uint8_t) xr, n[1] = (signed char) (int8_t) (uint8_t) yr, n[2] = (signed char) (int8_t) (uint8_t) zr;
        } else {
            int16_t *n = (int16_t *) buffer + i * 4;
            n[0] = (int16_t) (uint16_t) xr, n[1] = (int16_t) (uint16_t) yr, n[2] = (int16_t) (uint16_t) zr;
        }
    }
}

void meshopt_decode_filter_quat(void *buffer, const size_t count, const size_t stride) {
    (void) stride;
    const float scale = 32767.0f / sqrtf(2.0f);
    for (size_t i = 0; i < count; i++) {
        int16_t *q = (int16_t *) buffer + i * 4;
        const int cf = q[3];
        /* the scaler: the 4th component with its 2 low bits (the index) set */
        const float s = (float) (cf | 3);
        const float x = (float) q[0], y = (float) q[1], z = (float) q[2];
        const float ws = meshopt_fmul(s, s);
        float ww = meshopt_fadd(ws, ws) - meshopt_fadd(meshopt_fmul(x, x), meshopt_fadd(meshopt_fmul(y, y), meshopt_fmul(z, z)));
        if ((int32_t) meshopt_fbits(ww) < 0) ww = 0.0f; /* i32x4.max(ww, 0) */
        const float w = sqrtf(ww);
        const float ss = scale / s;
        const uint16_t xr = (uint16_t) meshopt_snap(meshopt_fmul(x, ss));
        const uint16_t yr = (uint16_t) meshopt_snap(meshopt_fmul(y, ss));
        const uint16_t zr = (uint16_t) meshopt_snap(meshopt_fmul(z, ss));
        const uint16_t wr = (uint16_t) meshopt_snap(meshopt_fmul(w, ss));
        const int qc = cf & 3;
        q[(qc + 1) & 3] = (int16_t) xr;
        q[(qc + 2) & 3] = (int16_t) yr;
        q[(qc + 3) & 3] = (int16_t) zr;
        q[(qc + 0) & 3] = (int16_t) wr;
    }
}

void meshopt_decode_filter_exp(void *buffer, const size_t count, const size_t stride) {
    unsigned char *bytes = (unsigned char *) buffer;
    const size_t words = count * (stride / 4);
    for (size_t i = 0; i < words; i++) {
        uint32_t v;
        memcpy(&v, bytes + i * 4, 4);
        /* 8-bit signed exponent, 24-bit signed mantissa: ldexp(m, e) */
        const int32_t m = (int32_t) (v << 8) >> 8;
        const int32_t e = (int32_t) v >> 24;
        const float r = meshopt_fmul(meshopt_bitsf((uint32_t) (e + 127) << 23), (float) m);
        memcpy(bytes + i * 4, &r, 4);
    }
}

int meshopt_decode_gltf_buffer(void *dst, const size_t count, const size_t stride, const unsigned char *src, const size_t src_size,
                               const int mode, const int filter) {
    int result;
    switch (mode) {
    case MESHOPT_MODE_ATTRIBUTES:
        if (stride % 4 != 0 || stride == 0 || stride > 256) return -4;
        if (filter == MESHOPT_FILTER_OCTAHEDRAL && stride != 4 && stride != 8) return -4;
        if (filter == MESHOPT_FILTER_QUATERNION && stride != 8) return -4;
        if (filter < MESHOPT_FILTER_NONE || filter > MESHOPT_FILTER_EXPONENTIAL) return -4;
        result = meshopt_decode_vertex_buffer(dst, count, stride, src, src_size);
        if (result) return result;
        if (filter == MESHOPT_FILTER_OCTAHEDRAL) meshopt_decode_filter_oct(dst, count, stride);
        else if (filter == MESHOPT_FILTER_QUATERNION) meshopt_decode_filter_quat(dst, count, stride);
        else if (filter == MESHOPT_FILTER_EXPONENTIAL) meshopt_decode_filter_exp(dst, count, stride);
        return 0;
    case MESHOPT_MODE_TRIANGLES:
        if (filter != MESHOPT_FILTER_NONE || count % 3 != 0) return -4;
        return meshopt_decode_index_buffer(dst, count, stride, src, src_size);
    case MESHOPT_MODE_INDICES:
        if (filter != MESHOPT_FILTER_NONE) return -4;
        return meshopt_decode_index_sequence(dst, count, stride, src, src_size);
    default:
        return -4;
    }
}

#endif /* MESHOPT_DECODE_IMPLEMENTED */
#endif /* MESHOPT_DECODE_IMPLEMENTATION */
