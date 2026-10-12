const { GAME, hook, done } = require('./prelude.js');
const UTILS = require('./utils.js');
const Math_ = UTILS.math;
const GT = { getReadable: UTILS.getReadable };
const d = { s: '5', t: 'abc', none: null };
hook(() => {
  const M = Math_;
  GAME.log(M.sin(1), M.cos(0), M.tan(0.5), M.asin(2), M.acos(0.5), M.atan(1), M.atan2(1, -1));
  GAME.log(M.sinh(1), M.cosh(1), M.tanh(1), M.asinh(1), M.acosh(0.5), M.roundDecimal(M.atanh(0.5), 12));
  GAME.log(M.log(0), M.log(-1), M.exp(1), M.sqrt(2), M.sqrt(-1), M.abs(-0), M.abs(-3.5));
  GAME.log(M.round(2.5), M.round(-2.5), M.round(0.49999999999999994), M.round(-0.4), M.floor(-1.5), M.ceil(1.2), M.trunc(-1.7));
  GAME.log(M.min(), M.max(), M.min(3, 1, 2), M.max(3, d.t), M.hypot(3, 4), M.hypot(), M.pow(2, 0.5), M.pow(1, 1 / 0));
  GAME.log(M.lerp(0, 10, 0.25), M.lerp(d.s, 10, 0.5), M.toRad(180), M.toDeg(Math.PI), M.calcPerc(200, 15));
  GAME.log(M.roundToNearest(7, 5), M.roundToNearest(-7, 5), M.roundToNearest(0, 5), M.roundToNearest(d.s, 5), M.roundToNearest(7, 0));
  GAME.log(M.roundDecimal(1.236, 2), M.roundDecimal(1.5), M.roundDecimal(d.s, 2), M.roundDecimal(1234.5, -2), M.E, M.PI);
  GAME.log(UTILS.getDist2D(0, 0, 3, 4), UTILS.getDist3D(1, 2, 3, 4, 6, 3), Math_.roundDecimal(UTILS.anglDst(0, Math.PI * 1.5), 12), UTILS.getDir2D(10, 10, 0, 0));
  const dir = UTILS.getDir3D(0, 0, 0, 1, 1, 1);
  GAME.log(dir.x, dir.y, UTILS.getDir3D(0, 0, 0, 1, d.s, 1));
  GAME.log(UTILS.hexFromHue(0), UTILS.hexFromHue(120), UTILS.hexFromHue(210), UTILS.hexFromHue(359), UTILS.hexFromHue(-30), UTILS.hexFromHue(d.t));
  GAME.log(UTILS.invertHex('#ff0000'), UTILS.invertHex('00ff00'), UTILS.invertHex('zz'), UTILS.invertHex(''));
  GAME.log(UTILS.RGBtoHEX(255, 128, 0), UTILS.RGBtoHEX(1, 2, 3), UTILS.RGBtoHEX(d.s, 300, -1), UTILS.RGBtoHEX(12.7, 0, 0));
  GAME.log(UTILS.HEXtoRGB('#abc'), UTILS.HEXtoRGB('#102030'), UTILS.HEXtoRGB('red'));
  GAME.log(GT.getReadable(61000), GT.getReadable(0), GT.getReadable(90061000), GT.getReadable(400 * 86400000 + 3600000), GT.getReadable(d.t));
  let ok = 0;
  for (let i = 0; i < 200; i++) {
    const r = UTILS.randInt(3, 6), f = UTILS.randFloat(-1, 1);
    if (r >= 3 && r <= 6 && Math.floor(r) === r && f >= -1 && f < 1) ok++;
  }
  GAME.log('random ok', ok);
});
done();
