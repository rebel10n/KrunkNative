// Generates shared/tests/ks/run/number_table.txt: "<f64 bits hex> <String(x)>" rows checked by
// ks_vm_test against ks_num_format (ECMAScript Number::toString, doc/v7/krunkscript.md 1.12).
// Run once with Node when the table needs refreshing:
//   node tools/ks/gen_number_table.js > shared/tests/ks/run/number_table.txt
'use strict';
const dv = new DataView(new ArrayBuffer(8));
let seed = 12345;
const rnd = () => (seed = (Math.imul(seed, 1103515245) + 12345) >>> 0);
const vals = [0.1 + 0.2, 1 / 3, 1e21, 1e-7, -0, NaN, Infinity, -Infinity, 2 ** 53, 0xff,
  123456789012345680000, 1e-6, 1.5e-6, 5e-324, 1.7976931348623157e308, 1e20, 1e22, -1.5, 100,
  4.35, 0.5, 2 ** 63, 9007199254740993, 1.1e21, 123e-20, 0.000001, 999999999999999900000];
for (let i = 0; i < 1500; i++) {
  dv.setUint32(0, rnd()); dv.setUint32(4, rnd());
  const v = dv.getFloat64(0);
  if (isFinite(v)) vals.push(v);
}
for (let i = 0; i < 600; i++) {
  vals.push((rnd() % 100000) / 10 ** (rnd() % 8));
  vals.push(rnd() * 2 ** ((rnd() % 200) - 100));
}
for (let e = -1074; e < 1024; e += 7) vals.push(2 ** e);
const out = [];
for (const v of vals) {
  dv.setFloat64(0, v);
  out.push(dv.getUint32(0).toString(16).padStart(8, '0') + dv.getUint32(4).toString(16).padStart(8, '0') + ' ' + String(v));
}
process.stdout.write(out.join('\n') + '\n');
