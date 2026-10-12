const { GAME, hook, done, A, O } = require('./prelude.js');
function cb() {}
const holder = {};
hook(() => {
  GAME.log();
  GAME.log('plain', 1.5, true, holder.nope, -0, 0 / 0);
  GAME.log({ s: 'q"b\\n\nt\tc\u0001', u: holder.nope, f: cb, nested: { empty: [], nul: null } });
  GAME.log([{ a: 1 }, {}], [0 / 0, 1 / 0], ['é', '😀']);
  const arr = { list: [1, 2] };
  A(holder.missingList, O).push({ x: 1 });
});
hook((id) => {
  const o = { a: null, cbs: [] };
  GAME.log(o, null, id);
}, '12');
done();
