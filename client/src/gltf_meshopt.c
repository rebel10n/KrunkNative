// EXT_meshopt_compression for cgltf, see client/include/gltf_meshopt.h.
#include <gltf_meshopt.h>
#include <stdio.h>
#include <stdlib.h>
#include "../lib/cgltf/cgltf.h"
#define MESHOPT_DECODE_IMPLEMENTATION
#include "../lib/meshopt/meshopt_decode.h"

int gltf_meshopt_decode(cgltf_data *data, char *err, const size_t errn) {
    if (err && errn) err[0] = 0;
    for (size_t i = 0; data && i < data->buffer_views_count; i++) {
        cgltf_buffer_view *view = &data->buffer_views[i];
        if (!view->has_meshopt_compression || view->data) continue;
        const cgltf_meshopt_compression *mc = &view->meshopt_compression;
        const unsigned char *source = mc->buffer ? (const unsigned char *) mc->buffer->data : NULL;
        if (!source || mc->offset + mc->size > mc->buffer->size) {
            if (err) snprintf(err, errn, "bufferView %zu: compressed data missing", i);
            return 0;
        }
        int mode, filter;
        switch (mc->mode) {
        case cgltf_meshopt_compression_mode_attributes: mode = MESHOPT_MODE_ATTRIBUTES; break;
        case cgltf_meshopt_compression_mode_triangles: mode = MESHOPT_MODE_TRIANGLES; break;
        case cgltf_meshopt_compression_mode_indices: mode = MESHOPT_MODE_INDICES; break;
        default: mode = -1; break;
        }
        switch (mc->filter) {
        case cgltf_meshopt_compression_filter_none: filter = MESHOPT_FILTER_NONE; break;
        case cgltf_meshopt_compression_filter_octahedral: filter = MESHOPT_FILTER_OCTAHEDRAL; break;
        case cgltf_meshopt_compression_filter_quaternion: filter = MESHOPT_FILTER_QUATERNION; break;
        case cgltf_meshopt_compression_filter_exponential: filter = MESHOPT_FILTER_EXPONENTIAL; break;
        default: filter = -1; break;
        }
        // the decoded view is count * byteStride bytes; accessors may not read past byteLength
        const size_t bytes = mc->count * mc->stride;
        if (!mc->count || !mc->stride || bytes / mc->stride != mc->count || bytes < view->size) {
            if (err) snprintf(err, errn, "bufferView %zu: bad count / byteStride", i);
            return 0;
        }
        void *out = malloc(bytes);
        if (!out) {
            if (err) snprintf(err, errn, "bufferView %zu: out of memory", i);
            return 0;
        }
        const int result = meshopt_decode_gltf_buffer(out, mc->count, mc->stride, source + mc->offset, mc->size, mode, filter);
        if (result != 0) {
            free(out);
            if (err) snprintf(err, errn, "bufferView %zu: meshopt decode error %d", i, result);
            return 0;
        }
        view->data = out; // cgltf_free releases it (data->memory.free_func: free)
    }
    return 1;
}
