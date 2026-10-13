#pragma once
// EXT_meshopt_compression for cgltf (client/src/gltf_meshopt.c over client/lib/meshopt): v7's AI
// and map .glb user assets (gltfpack output: KHR_mesh_quantization + EXT_meshopt_compression,
// with a "fallback" buffer that has no data) store their vertex attributes, indices and animation
// keys compressed. cgltf parses the extension; this decodes every compressed bufferView into
// cgltf_buffer_view.data (owned and freed by cgltf_free), so accessors read the decoded bytes.
// GL-free: client/tests/meshopt_test.c runs it against the meshoptimizer npm decoder's goldens.
#include <stddef.h>

struct cgltf_data;

// After cgltf_load_buffers. 1 when every compressed view decoded (or there were none); 0 with a
// message in err otherwise (the model is unusable: its fallback buffer holds no data).
int gltf_meshopt_decode(struct cgltf_data *data, char *err, size_t errn);
