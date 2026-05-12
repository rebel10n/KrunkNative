#include <weapons.h>

const Weapon g_knife = {
    .name = "Combat Knife",
    .icon = "icon_0",
    .secondary = 0,
    .no_spread = 0,
    .akimbo = 0,
    .can_throw = 1,
    .melee = 1,
    .equipment = 0,
    .swap_time = 0.25f,
    .aim_speed = 0.1f,
    .speed_mlt = 1.1f,
    .damage = 50.0f,
    .range = 15.0f,
    .rate = 0.25f,
    .spread = 100.0f,
    .recoil = 0.006f,
    .recover = 0.98f,
    .recover_f = 0.98f,
    .zoom = 1.2f,
};

// {
// name: "Combat Knife",
// icon: "icon_0",
// melee: true,
// noSkins: true,
// canThrow: true,
// aimLean: 0.01,
// holdW: 0.9,
// swapWiggle: 0.3,
// sound: "melee",
// insX: 0.1,
// inspAnim: {
// arZ: 0.3,
// arX: 0.6,
// arY: -2.2,
// apZ: -1.3,
// apX: -0.1,
// apY: 1,
// wpZ: -0.19,
// wpY: -0.4,
// wrY: -0.2
// },
// anim: function (f, g, h) {
// if (h == 1) {
// f.handAnimInd = (f.handAnimInd || 1) * -1;
// var j = false;
// if (f.meleeAnim.anim) {
// f.meleeAnim.anim.stop();
// j = f.meleeAnim.armM >= 2;
// }
// f.resetMeleeAnim();
// var k = -1.2;
// var l = 0.7;
// var m = -0.4;
// if (j) {
// k += 0.1;
// f.meleeAnim.weaR = k;
// m += 0.8;
// f.meleeAnim.armR = m;
// l += 0.2;
// f.meleeAnim.weaM = l;
// f.meleeAnim.flipW = Math.PI;
// } else {
// k -= 0.4;
// }
// f.meleeAnim.anim = new g.Tween(f.meleeAnim).to({
// armR: m,
// lArm: 1,
// armT: j ? -0.2 : -0.8,
// armY: -3,
// armM: j ? -10 : 13,
// armE: -2,
// weaR: k,
// weaM: l
// }, (j ? 1.3 : 1) * 220).easing(g.Easing.Cubic.Out).onComplete(function () {
// f.meleeAnim.anim = new g.Tween(f.meleeAnim).to({
// armR: 0,
// armT: 0,
// armY: 0,
// lArm: 0,
// armM: 0,
// armE: 0,
// weaR: 0,
// weaM: 0,
// flipW: 0
// }, 350).easing(g.Easing.Cubic.Out).start();
// }).start();
// } else if (h == 2) {
// f.handAnimInd = (f.handAnimInd || 1) * -1;
// if (f.meleeAnim.anim) {
// f.meleeAnim.anim.stop();
// }
// f.resetMeleeAnim();
// var p = f.handAnimInd ? "lArm" : "";
// p = {};
// if (f.handAnimInd == 1) {
// p.lArm = -2.7;
// } else {
// p.rArm = -2.7;
// }
// f.meleeAnim.anim = new g.Tween(f.meleeAnim).to(p, 10).easing(g.Easing.Linear.None).onComplete(function () {
// f.meleeAnim.anim = new g.Tween(f.meleeAnim).to({
// lArm: 0,
// rArm: 0
// }, 150).easing(g.Easing.Linear.None).delay(100).start();
// }).start();
// } else if (h == 3) {
// if (f.meleeAnim.anim) {
// f.meleeAnim.anim.stop();
// }
// f.resetMeleeAnim();
// f.meleeAnim.armR = -1.3;
// f.meleeAnim.armM = 1.7;
// f.meleeAnim.armE = 1.1;
// f.meleeAnim.armY = 2.9;
// f.meleeAnim.armT = 1;
// f.meleeAnim.armS = 1.7;
// f.meleeAnim.weaR = -0.6;
// f.meleeAnim.anim = new g.Tween(f.meleeAnim).to({
// armY: -9,
// armS: -0.1,
// armE: -7.5,
// armM: -1.5,
// lArm: 0.5,
// weaR: -3
// }, 300).easing(g.Easing.Cubic.Out).onComplete(function () {
// f.meleeAnim.anim = new g.Tween(f.meleeAnim).to({
// armR: 0,
// armT: 0,
// armY: 0,
// lArm: 0,
// armM: 0,
// armS: 0,
// armE: 0,
// weaR: 0
// }, 350).easing(g.Easing.Cubic.Out).start();
// }).start();
// }
// },
// type: 2,
// swapTime: 250,
// aimSpd: 100,
// rate: 250,
// dmg: 50,
// dmgDrop: 0,
// range: 15,
// spdMlt: 1.1,
// spread: 100,
// leftHoldY: -0.82,
// leftHoldX: 1.5,
// rightHoldX: -1.5,
// rightHoldY: -0.82,
// leftHoldZ: -0.5,
// rightHoldZ: -0.5,
// xOff: 0,
// yOff: -0.6,
// zOff: -3.6,
// xOrg: 0.0001,
// yOrg: -0.6,
// zOrg: -3.6,
// zRM: 0.35,
// zoom: 1.2,
// leanMlt: 0.8,
// recoil: 0.006,
// recoilR: 0.01,
// recover: 0.98,
// recoverF: 0.98,
// rumble: 0.4,
// rumbleDur: 150,
// icnPad: -10
// }