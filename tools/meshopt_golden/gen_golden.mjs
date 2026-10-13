// Goldens for client/tests/meshopt_test.c (the C port in client/lib/meshopt/meshopt_decode.h),
// made with the reference decoder: the meshoptimizer npm package (1.1.1 was used) under node.
//
//   node tools/meshopt_golden/gen_golden.mjs <meshoptimizer package dir> > client/tests/data/meshopt_golden.json
//
// "vectors": small buffers encoded here with the package's encoder (vertex codec versions 0 and 1,
// the index buffer and index sequence codecs, the octahedral / quaternion / exponential filters)
// with the FNV-1a 64 hash of the decoder's output (and the output itself when small). "assets": every EXT_meshopt_compression bufferView of the
// v7 AI / map .glb user assets (assets/user-assets/<id>/model.glb, gitignored content from
// tools/ui-import/fetch_live_assets.py) with the FNV-1a 64 hash of its decoded bytes.
//
// The package's decoder is WebAssembly; node runs its SIMD build, as browsers (and so Krunker's
// three.js) do. Its filters differ from the package's non-SIMD build on QUATERNION views by one
// unit here and there (a different rounding and summation order); the C port follows SIMD.
import fs from 'node:fs';
import path from 'node:path';

const pkg = process.argv[2];
if (!pkg) throw new Error('usage: gen_golden.mjs <meshoptimizer package dir>');
const { MeshoptDecoder } = await import(path.resolve(pkg, 'meshopt_decoder.mjs'));
const { MeshoptEncoder } = await import(path.resolve(pkg, 'meshopt_encoder.js'));
await MeshoptDecoder.ready;
await MeshoptEncoder.ready;

const hex = (u8) => Buffer.from(u8.buffer, u8.byteOffset, u8.byteLength).toString('hex');
function fnv64(bytes) {
  let h = 0xcbf29ce484222325n;
  for (const b of bytes) { h ^= BigInt(b); h = (h * 0x100000001b3n) & 0xffffffffffffffffn; }
  return h.toString(16).padStart(16, '0');
}
let seed = 12345;
const rand = () => ((seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296);

const vectors = [];
function vector(name, encoded, count, stride, mode, filter) {
  const decoded = new Uint8Array(count * stride);
  MeshoptDecoder.decodeGltfBuffer(decoded, count, stride, encoded, mode, filter);
  const v = { name, mode, filter, count, stride, encoded: hex(encoded), fnv64: fnv64(decoded) };
  if (decoded.length <= 256) v.decoded = hex(decoded);
  vectors.push(v);
}

// a 260-vertex mesh (more than one 256-vertex block at stride 16): position xyz float + uv u16
const count = 260;
const verts = new Uint8Array(count * 16);
const dv = new DataView(verts.buffer);
for (let i = 0; i < count; i++) {
  dv.setFloat32(i * 16 + 0, Math.sin(i * 0.1) * 10, true);
  dv.setFloat32(i * 16 + 4, i * 0.05, true);
  dv.setFloat32(i * 16 + 8, Math.cos(i * 0.07) * 3 + rand() * 0.01, true);
  dv.setUint16(i * 16 + 12, (i * 37) & 0xffff, true);
  dv.setUint16(i * 16 + 14, Math.floor(rand() * 65535), true);
}
for (const version of [0, 1])
  for (const level of [0, 2, 3])
    vector(`vertex_v${version}_l${level}`, MeshoptEncoder.encodeVertexBufferLevel(verts, count, 16, level, version), count, 16, 'ATTRIBUTES', 'NONE');
vector('vertex_small', MeshoptEncoder.encodeVertexBufferLevel(verts.subarray(0, 3 * 16), 3, 16, 2, 0), 3, 16, 'ATTRIBUTES', 'NONE');

// a 20 x 20 grid of triangles; and a line strip index sequence
const indices = [];
for (let y = 0; y < 20; y++)
  for (let x = 0; x < 20; x++) {
    const a = y * 21 + x, b = a + 1, c = a + 21, d = c + 1;
    indices.push(a, c, b, b, c, d);
  }
for (const size of [2, 4]) {
  const raw = size == 2 ? new Uint16Array(indices) : new Uint32Array(indices);
  const u8 = new Uint8Array(raw.buffer);
  vector(`triangles_${size}`, MeshoptEncoder.encodeIndexBuffer(u8, indices.length, size), indices.length, size, 'TRIANGLES', 'NONE');
  const seq = size == 2 ? new Uint16Array(500) : new Uint32Array(500);
  for (let i = 0; i < seq.length; i++) seq[i] = (i * 7 + (i % 3 == 0 ? 1000 : 0)) % (size == 2 ? 65536 : 100000);
  vector(`indices_${size}`, MeshoptEncoder.encodeIndexSequence(new Uint8Array(seq.buffer), seq.length, size), seq.length, size, 'INDICES', 'NONE');
}

// filters: unit normals, unit quaternions, floats over a wide range
const n = 129;
const normals = new Float32Array(n * 4), quats = new Float32Array(n * 4), floats = new Float32Array(n * 3);
for (let i = 0; i < n; i++) {
  let x = rand() * 2 - 1, y = rand() * 2 - 1, z = rand() * 2 - 1;
  let l = Math.hypot(x, y, z) || 1;
  normals.set([x / l, y / l, z / l, i % 2 ? 1 : -1], i * 4);
  let q = [rand() * 2 - 1, rand() * 2 - 1, rand() * 2 - 1, rand() * 2 - 1];
  l = Math.hypot(...q) || 1;
  quats.set(q.map((v) => v / l), i * 4);
  floats.set([(rand() - 0.5) * 1e4, (rand() - 0.5) * 1e-3, rand() * 100], i * 3);
}
for (const [stride, bits] of [[4, 8], [8, 12], [8, 16]]) {
  const filtered = MeshoptEncoder.encodeFilterOct(normals, n, stride, bits);
  vector(`oct_${stride}_${bits}`, MeshoptEncoder.encodeVertexBuffer(filtered, n, stride), n, stride, 'ATTRIBUTES', 'OCTAHEDRAL');
}
for (const bits of [9, 12, 16]) {
  const filtered = MeshoptEncoder.encodeFilterQuat(quats, n, 8, bits);
  vector(`quat_${bits}`, MeshoptEncoder.encodeVertexBuffer(filtered, n, 8), n, 8, 'ATTRIBUTES', 'QUATERNION');
}
for (const mode of ['Separate', 'SharedVector', 'SharedComponent', 'Clamped']) {
  const filtered = MeshoptEncoder.encodeFilterExp(floats, n, 12, 15, mode);
  vector(`exp_${mode}`, MeshoptEncoder.encodeVertexBuffer(filtered, n, 12), n, 12, 'ATTRIBUTES', 'EXPONENTIAL');
}

// the v7 .glb user assets
const ids = [48931, 50329, 50330, 50331, 50572, 50573, 50575, 50576, 50577, 50578, 52615];
const assets = [];
for (const id of ids) {
  const file = `assets/user-assets/${id}/model.glb`;
  const buf = fs.readFileSync(file);
  const jsonLen = buf.readUInt32LE(12);
  const json = JSON.parse(buf.subarray(20, 20 + jsonLen).toString('utf8'));
  const binOff = 20 + jsonLen + 8;
  json.bufferViews.forEach((bv, view) => {
    const e = bv.extensions && bv.extensions.EXT_meshopt_compression;
    if (!e) return;
    const src = new Uint8Array(buf.buffer, buf.byteOffset + binOff + (e.byteOffset || 0), e.byteLength);
    const decoded = new Uint8Array(e.count * e.byteStride);
    MeshoptDecoder.decodeGltfBuffer(decoded, e.count, e.byteStride, src, e.mode, e.filter || 'NONE');
    assets.push({ file, view, mode: e.mode, filter: e.filter || 'NONE', count: e.count, stride: e.byteStride, fnv64: fnv64(decoded) });
  });
}

process.stdout.write(JSON.stringify({ generator: 'tools/meshopt_golden/gen_golden.mjs', meshoptimizer: JSON.parse(fs.readFileSync(path.resolve(pkg, 'package.json'))).version, vectors, assets }, null, 1) + '\n');
