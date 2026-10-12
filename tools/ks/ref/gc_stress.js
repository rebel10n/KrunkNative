const { GAME, hook, done, L, BC } = require('./prelude.js');
const keep = {};
function mk(n) { const junk = []; for (let i = 0; i < 20000; i++) junk.push({ v: i, self: {} }); return { n, junk: L(junk) }; }
function sum3(a, b, c) { return a.n + b.n + c.n; }
hook(() => {
  const kept = [];
  for (let i = 0; i < 300000; i++) {
    const a = { i }; const b = { peer: a, list: [i, i + 1] }; a.peer = b; a.self = a;
    if (i % 1000 === 0) kept.push(a);
  }
  let sum = 0;
  for (let j = 0; j < L(kept); j++) sum += kept[j].peer.peer.i;
  keep.kept = kept;
  GAME.log(L(kept), sum);
  let s = '';
  for (let k = 0; k < 20000; k++) { s = s + 'x' + String(k); if (L(s) > 300) s = ''; }
  GAME.log(L(s), s);
  const built = [mk(1), mk(2), mk(3)];
  GAME.log(built, sum3(mk(4), mk(5), mk(6)));
});
hook(() => { const last = keep.kept[299]; GAME.log(L(keep.kept), last.i, last.peer.list, last.self === last); });
done();
