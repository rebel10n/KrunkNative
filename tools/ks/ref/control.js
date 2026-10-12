const { GAME, hook, done, OE, BC, S } = require('./prelude.js');
let calls = 0;
function fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }
function noReturn(x) { if (x > 0) { return x; } return 0; } // KN: typed actions fall off with their default
function greet(who) { return 'hi ' + who; }
function count() { calls += 1; }
hook(() => {
  let total = 0;
  for (let i = 0; i < 10; i++) { if (i === 3) continue; if (i === 8) break; total += i; }
  GAME.log(total);
  let j = 0, k = 0;
  while (j < 5) { j++; for (let m = 0; m < 3; m++) { if (m === j) break; k += 1; } }
  GAME.log(j, k);
  const o = { a: 1, b: 'two', c: [3] };
  for (const e of OE(o)) GAME.log(e.key, e.value);
  delete o.a; o.a = 9; o.d = true;
  let keys = '';
  for (const e of OE(o)) keys += e.key;
  GAME.log(keys);
  for (const e of OE([10, 20])) GAME.log(e.key, e.value);
  for (const e of OE('hi')) GAME.log(e.key + '=' + S(e.value));
  let n = 0;
  for (const e of OE(o)) { n++; if (n === 2) break; }
  GAME.log(n, fib(15), noReturn(0), noReturn(4), greet('bob'));
  count(); count();
  GAME.log(calls);
  for (let a = 0; a < 3; a++) { const inner = a * 2; GAME.log(inner); }
  let x = 1; { const y = 2; x += y; }
  GAME.log(x);
});
done();
