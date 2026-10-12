const { GAME, hook, done, HP, NE, O, A, L, FC } = require('./prelude.js');
function twice(x) { return x * 2; }
function shout(s) { GAME.log(s + '!'); }
function useFn(f, v) { GAME.log('apply', f(v), f(v, 6), L(f)); }
hook(() => {
  const o = { a: 1, b: 'x', c: { d: true }, 'quoted key': 5 };
  o.c.d = false; o.c.e = [1]; o.z = 26;
  GAME.log(o);
  GAME.log(HP(o, 'a'), HP(o, 'nope'), HP(o.c, 'e'), NE(o), NE({}), NE(''), NE('a'), NE([]), NE([0]), NE(o.nope));
  delete o.a; delete o.nope; o.a = 2;
  GAME.log(o);
  const alias = o; alias.b = 'changed';
  GAME.log(o.b, alias === o);
  GAME.log(A(5, O), A('s', O), A(o.nope, O), A(o.c, O), A([1, 2], O));
  const fns = { double: twice, loud: shout };
  GAME.log(FC(fns.double, 'double')(21));
  FC(fns.loud, 'loud')('hey');
  useFn(twice, 4);
  GAME.log(fns);
  GAME.log([1], {}, { e: {}, a: [] });
});
done();
