// KrunkScript GAME.* binding table (doc/v7/krunkscript.md 3.x, 6.5).
// Every member of the v7.1.4 API context (REF/src/libs/createKrunkScriptApiContext.js
// `context`, cross-checked against the 211 autocomplete entries of
// krunkScriptSyntax.js) is listed once. Paths omit the "GAME." root. Return
// types come from krunkScriptTypes.js (TYP); a path not in TYP returns `any`.
// Frozen ids: append new entries only; KsHost.api is indexed by KsApiId.
#ifndef KS_API_H
#define KS_API_H

#include "ks_ast.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { KS_API_KIND_NS, KS_API_KIND_FN, KS_API_KIND_PROP } KsApiKind;
enum { KS_API_SIDE_CLIENT = 1, KS_API_SIDE_SERVER = 2, KS_API_SIDE_BOTH = 3 };

// X(id, path, name, parent, kind, sides, ret): name = last path segment, parent = enclosing
// namespace id (-1 = GAME root, always listed before its children), kind NS|FN|PROP,
// sides C|S|B, ret ANY|NUM|STR|BOOL|OBJ|OBJARR
#define KS_API_LIST(X) \
    X(LOG, "log", "log", -1, FN, B, ANY) \
    X(CHANGEGAME, "changeGame", "changeGame", -1, FN, C, ANY) \
    X(SETTITLE, "setTitle", "setTitle", -1, FN, C, ANY) \
    X(DEFAULT, "DEFAULT", "DEFAULT", -1, NS, B, ANY) \
    X(DEFAULT_DISABLEPLAYERBEHAVIOUR, "DEFAULT.disablePlayerBehaviour", "disablePlayerBehaviour", KS_API_DEFAULT, FN, B, ANY) \
    X(DEFAULT_DISABLE3D, "DEFAULT.disable3D", "disable3D", KS_API_DEFAULT, FN, C, ANY) \
    X(DEFAULT_DISABLEPREDICTION, "DEFAULT.disablePrediction", "disablePrediction", KS_API_DEFAULT, FN, B, ANY) \
    X(DEFAULT_DISABLESERVERSYNC, "DEFAULT.disableServerSync", "disableServerSync", KS_API_DEFAULT, FN, B, ANY) \
    X(NETWORK, "NETWORK", "NETWORK", -1, NS, B, ANY) \
    X(NETWORK_SEND, "NETWORK.send", "send", KS_API_NETWORK, FN, B, BOOL) \
    X(NETWORK_BROADCAST, "NETWORK.broadcast", "broadcast", KS_API_NETWORK, FN, S, BOOL) \
    X(URLS, "URLS", "URLS", -1, NS, B, ANY) \
    X(URLS_OPENDISCORD, "URLS.openDiscord", "openDiscord", KS_API_URLS, FN, B, ANY) \
    X(URLS_OPENTWITTER, "URLS.openTwitter", "openTwitter", KS_API_URLS, FN, B, ANY) \
    X(URLS_OPENYOUTUBE, "URLS.openYoutube", "openYoutube", KS_API_URLS, FN, B, ANY) \
    X(URLS_OPENOPENSEA, "URLS.openOpensea", "openOpensea", KS_API_URLS, FN, B, ANY) \
    X(NFT, "NFT", "NFT", -1, NS, B, ANY) \
    X(NFT_HASWALLET, "NFT.hasWallet", "hasWallet", KS_API_NFT, FN, B, BOOL) \
    X(NFT_OWNEDASSETS, "NFT.ownedAssets", "ownedAssets", KS_API_NFT, FN, S, OBJARR) \
    X(CHAT, "CHAT", "CHAT", -1, NS, S, ANY) \
    X(CHAT_SEND, "CHAT.send", "send", KS_API_CHAT, FN, S, ANY) \
    X(CHAT_BROADCAST, "CHAT.broadcast", "broadcast", KS_API_CHAT, FN, S, ANY) \
    X(UTILS, "UTILS", "UTILS", -1, NS, B, ANY) \
    X(UTILS_MATH, "UTILS.math", "math", KS_API_UTILS, NS, B, ANY) \
    X(UTILS_MATH_E, "UTILS.math.E", "E", KS_API_UTILS_MATH, PROP, B, NUM) \
    X(UTILS_MATH_PI, "UTILS.math.PI", "PI", KS_API_UTILS_MATH, PROP, B, NUM) \
    X(UTILS_MATH_PI2, "UTILS.math.PI2", "PI2", KS_API_UTILS_MATH, PROP, B, NUM) \
    X(UTILS_MATH_SIN, "UTILS.math.sin", "sin", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_SINH, "UTILS.math.sinh", "sinh", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_COS, "UTILS.math.cos", "cos", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_COSH, "UTILS.math.cosh", "cosh", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_TAN, "UTILS.math.tan", "tan", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_TANH, "UTILS.math.tanh", "tanh", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_ASIN, "UTILS.math.asin", "asin", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_ASINH, "UTILS.math.asinh", "asinh", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_ACOS, "UTILS.math.acos", "acos", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_ACOSH, "UTILS.math.acosh", "acosh", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_ATAN, "UTILS.math.atan", "atan", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_ATAN2, "UTILS.math.atan2", "atan2", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_ATANH, "UTILS.math.atanh", "atanh", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_LOG, "UTILS.math.log", "log", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_EXP, "UTILS.math.exp", "exp", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_POW, "UTILS.math.pow", "pow", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_ABS, "UTILS.math.abs", "abs", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_ROUND, "UTILS.math.round", "round", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_FLOOR, "UTILS.math.floor", "floor", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_CEIL, "UTILS.math.ceil", "ceil", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_MIN, "UTILS.math.min", "min", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_MAX, "UTILS.math.max", "max", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_HYPOT, "UTILS.math.hypot", "hypot", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_SQRT, "UTILS.math.sqrt", "sqrt", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_TRUNC, "UTILS.math.trunc", "trunc", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_LERP, "UTILS.math.lerp", "lerp", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_TORAD, "UTILS.math.toRad", "toRad", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_TODEG, "UTILS.math.toDeg", "toDeg", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_CALCPERC, "UTILS.math.calcPerc", "calcPerc", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_ROUNDTONEAREST, "UTILS.math.roundToNearest", "roundToNearest", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_MATH_ROUNDDECIMAL, "UTILS.math.roundDecimal", "roundDecimal", KS_API_UTILS_MATH, FN, B, NUM) \
    X(UTILS_GETDIST2D, "UTILS.getDist2D", "getDist2D", KS_API_UTILS, FN, B, NUM) \
    X(UTILS_GETDIST3D, "UTILS.getDist3D", "getDist3D", KS_API_UTILS, FN, B, NUM) \
    X(UTILS_ANGLDST, "UTILS.anglDst", "anglDst", KS_API_UTILS, FN, B, NUM) \
    X(UTILS_GETDIR2D, "UTILS.getDir2D", "getDir2D", KS_API_UTILS, FN, B, NUM) \
    X(UTILS_GETDIR3D, "UTILS.getDir3D", "getDir3D", KS_API_UTILS, FN, B, OBJ) \
    X(UTILS_RANDINT, "UTILS.randInt", "randInt", KS_API_UTILS, FN, B, NUM) \
    X(UTILS_RANDFLOAT, "UTILS.randFloat", "randFloat", KS_API_UTILS, FN, B, NUM) \
    X(UTILS_HEXFROMHUE, "UTILS.hexFromHue", "hexFromHue", KS_API_UTILS, FN, B, STR) \
    X(UTILS_INVERTHEX, "UTILS.invertHex", "invertHex", KS_API_UTILS, FN, B, STR) \
    X(UTILS_RGBTOHEX, "UTILS.RGBtoHEX", "RGBtoHEX", KS_API_UTILS, FN, B, STR) \
    X(UTILS_HEXTORGB, "UTILS.HEXtoRGB", "HEXtoRGB", KS_API_UTILS, FN, B, OBJ) \
    X(UTILS_TOLOWER, "UTILS.toLower", "toLower", KS_API_UTILS, FN, B, STR) \
    X(UTILS_TOUPPER, "UTILS.toUpper", "toUpper", KS_API_UTILS, FN, B, STR) \
    X(UTILS_TRUNCATETXT, "UTILS.truncateTxt", "truncateTxt", KS_API_UTILS, FN, B, STR) \
    X(UTILS_REPLACETEXT, "UTILS.replaceText", "replaceText", KS_API_UTILS, FN, B, STR) \
    X(UTILS_TEXTCONTAINS, "UTILS.textContains", "textContains", KS_API_UTILS, FN, B, BOOL) \
    X(UTILS_COPYTOCLIPBOARD, "UTILS.copyToClipboard", "copyToClipboard", KS_API_UTILS, FN, C, ANY) \
    X(RAYCAST, "RAYCAST", "RAYCAST", -1, NS, B, ANY) \
    X(RAYCAST_FROM, "RAYCAST.from", "from", KS_API_RAYCAST, FN, B, OBJ) \
    X(RAYCAST_FROMPLAYER, "RAYCAST.fromPlayer", "fromPlayer", KS_API_RAYCAST, FN, B, ANY) \
    X(TIME, "TIME", "TIME", -1, NS, B, ANY) \
    X(TIME_NOW, "TIME.now", "now", KS_API_TIME, FN, B, NUM) \
    X(TIME_FIXEDDELTA, "TIME.fixedDelta", "fixedDelta", KS_API_TIME, FN, B, NUM) \
    X(TIME_GETREADABLE, "TIME.getReadable", "getReadable", KS_API_TIME, FN, B, STR) \
    X(TIME_FREEZE, "TIME.freeze", "freeze", KS_API_TIME, FN, S, ANY) \
    X(TIME_UNFREEZE, "TIME.unfreeze", "unfreeze", KS_API_TIME, FN, S, ANY) \
    X(INPUTS, "INPUTS", "INPUTS", -1, NS, C, ANY) \
    X(INPUTS_KEYDOWN, "INPUTS.keyDown", "keyDown", KS_API_INPUTS, FN, C, BOOL) \
    X(INPUTS_MOUSEPOS, "INPUTS.mousePos", "mousePos", KS_API_INPUTS, FN, C, OBJ) \
    X(INPUTS_MOUSEPOSOVERLAY, "INPUTS.mousePosOverlay", "mousePosOverlay", KS_API_INPUTS, FN, C, OBJ) \
    X(INPUTS_MOUSEMOVEMENT, "INPUTS.mouseMovement", "mouseMovement", KS_API_INPUTS, FN, C, OBJ) \
    X(INPUTS_FREEMOUSE, "INPUTS.freeMouse", "freeMouse", KS_API_INPUTS, FN, C, ANY) \
    X(INPUTS_UNLOCKMOUSE, "INPUTS.unlockMouse", "unlockMouse", KS_API_INPUTS, FN, C, ANY) \
    X(INPUTS_LOCKMOUSE, "INPUTS.lockMouse", "lockMouse", KS_API_INPUTS, FN, C, ANY) \
    X(INPUTS_DISABLEDEFAULT, "INPUTS.disableDefault", "disableDefault", KS_API_INPUTS, FN, C, ANY) \
    X(INPUTS_ENABLEDEFAULT, "INPUTS.enableDefault", "enableDefault", KS_API_INPUTS, FN, C, ANY) \
    X(UI, "UI", "UI", -1, NS, C, ANY) \
    X(UI_GETSIZE, "UI.getSize", "getSize", KS_API_UI, FN, C, OBJ) \
    X(UI_HIDECROSSHAIR, "UI.hideCrosshair", "hideCrosshair", KS_API_UI, FN, C, ANY) \
    X(UI_HIDEDEFAULT, "UI.hideDefault", "hideDefault", KS_API_UI, FN, C, ANY) \
    X(UI_ADDIMAGE, "UI.addImage", "addImage", KS_API_UI, FN, C, STR) \
    X(UI_ADDDIV, "UI.addDIV", "addDIV", KS_API_UI, FN, C, STR) \
    X(UI_MOVEDIV, "UI.moveDIV", "moveDIV", KS_API_UI, FN, C, STR) \
    X(UI_REMOVEDIV, "UI.removeDIV", "removeDIV", KS_API_UI, FN, C, ANY) \
    X(UI_GETPROP, "UI.getProp", "getProp", KS_API_UI, FN, C, STR) \
    X(UI_UPDATEDIV, "UI.updateDIV", "updateDIV", KS_API_UI, FN, C, ANY) \
    X(UI_UPDATEDIVTEXT, "UI.updateDIVText", "updateDIVText", KS_API_UI, FN, C, ANY) \
    X(UI_GETDIVTEXT, "UI.getDIVText", "getDIVText", KS_API_UI, FN, C, STR) \
    X(SETTINGS, "SETTINGS", "SETTINGS", -1, NS, C, ANY) \
    X(SETTINGS_SET, "SETTINGS.set", "set", KS_API_SETTINGS, FN, C, ANY) \
    X(SETTINGS_GET, "SETTINGS.get", "get", KS_API_SETTINGS, FN, C, STR) \
    X(SETTINGS_LIST, "SETTINGS.list", "list", KS_API_SETTINGS, FN, C, STR) \
    X(OVERLAY, "OVERLAY", "OVERLAY", -1, NS, C, ANY) \
    X(OVERLAY_GETSIZE, "OVERLAY.getSize", "getSize", KS_API_OVERLAY, FN, C, OBJ) \
    X(OVERLAY_OFFSET, "OVERLAY.offset", "offset", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_SCALE, "OVERLAY.scale", "scale", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_CLEAR, "OVERLAY.clear", "clear", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_MEASURETEXT, "OVERLAY.measureText", "measureText", KS_API_OVERLAY, FN, C, NUM) \
    X(OVERLAY_DRAWCIRCLE, "OVERLAY.drawCircle", "drawCircle", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_DRAWRECT, "OVERLAY.drawRect", "drawRect", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_DRAWLINE, "OVERLAY.drawLine", "drawLine", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_DRAWTEXT, "OVERLAY.drawText", "drawText", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_DRAWIMAGE, "OVERLAY.drawImage", "drawImage", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_BEGINPATH, "OVERLAY.beginPath", "beginPath", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_CLOSEPATH, "OVERLAY.closePath", "closePath", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_SAVE, "OVERLAY.save", "save", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_RESTORE, "OVERLAY.restore", "restore", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_TRANSLATE, "OVERLAY.translate", "translate", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_TRANSFORM, "OVERLAY.transform", "transform", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_SETTRANSFORM, "OVERLAY.setTransform", "setTransform", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_MOVETO, "OVERLAY.moveTo", "moveTo", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_LINETO, "OVERLAY.lineTo", "lineTo", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_FILL, "OVERLAY.fill", "fill", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_FILLSTYLE, "OVERLAY.fillStyle", "fillStyle", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_STROKE, "OVERLAY.stroke", "stroke", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_STROKETEXT, "OVERLAY.strokeText", "strokeText", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_STROKERECT, "OVERLAY.strokeRect", "strokeRect", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_STROKESTYLE, "OVERLAY.strokeStyle", "strokeStyle", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_LINEWIDTH, "OVERLAY.lineWidth", "lineWidth", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_LINEDASHOFFSET, "OVERLAY.lineDashOffset", "lineDashOffset", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_LINEJOIN, "OVERLAY.lineJoin", "lineJoin", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_ELLIPSE, "OVERLAY.ellipse", "ellipse", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_ARC, "OVERLAY.arc", "arc", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_ARCTO, "OVERLAY.arcTo", "arcTo", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_BEZIERCURVETO, "OVERLAY.bezierCurveTo", "bezierCurveTo", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_QUADRATICCURVETO, "OVERLAY.quadraticCurveTo", "quadraticCurveTo", KS_API_OVERLAY, FN, C, ANY) \
    X(OVERLAY_GLOBALALPHA, "OVERLAY.globalAlpha", "globalAlpha", KS_API_OVERLAY, FN, C, ANY) \
    X(STORAGE, "STORAGE", "STORAGE", -1, NS, S, ANY) \
    X(STORAGE_SET, "STORAGE.set", "set", KS_API_STORAGE, FN, S, ANY) \
    X(STORAGE_UPDATE, "STORAGE.update", "update", KS_API_STORAGE, FN, S, ANY) \
    X(STORAGE_TRANSACT, "STORAGE.transact", "transact", KS_API_STORAGE, FN, S, ANY) \
    X(STORAGE_LOAD, "STORAGE.load", "load", KS_API_STORAGE, FN, S, ANY) \
    X(COOKIES, "COOKIES", "COOKIES", -1, NS, C, ANY) \
    X(COOKIES_SAVE, "COOKIES.save", "save", KS_API_COOKIES, FN, C, ANY) \
    X(COOKIES_REMOVE, "COOKIES.remove", "remove", KS_API_COOKIES, FN, C, ANY) \
    X(COOKIES_LOAD, "COOKIES.load", "load", KS_API_COOKIES, FN, C, STR) \
    X(COOKIES_HAS, "COOKIES.has", "has", KS_API_COOKIES, FN, C, BOOL) \
    X(SOUND, "SOUND", "SOUND", -1, NS, C, ANY) \
    X(SOUND_STOP, "SOUND.stop", "stop", KS_API_SOUND, FN, C, ANY) \
    X(SOUND_PLAY2D, "SOUND.play2D", "play2D", KS_API_SOUND, FN, C, OBJ) \
    X(SOUND_PLAY3D, "SOUND.play3D", "play3D", KS_API_SOUND, FN, C, OBJ) \
    X(ANIM, "ANIM", "ANIM", -1, NS, B, ANY) \
    X(ANIM_PLAYCLIP, "ANIM.playClip", "playClip", KS_API_ANIM, FN, B, ANY) \
    X(ANIM_STOPCLIP, "ANIM.stopClip", "stopClip", KS_API_ANIM, FN, B, ANY) \
    X(CAMERA, "CAMERA", "CAMERA", -1, NS, C, ANY) \
    X(CAMERA_ENVZONE, "CAMERA.envZone", "envZone", KS_API_CAMERA, PROP, C, ANY) \
    X(CAMERA_DETACH, "CAMERA.detach", "detach", KS_API_CAMERA, FN, C, ANY) \
    X(CAMERA_ATTACH, "CAMERA.attach", "attach", KS_API_CAMERA, FN, C, ANY) \
    X(CAMERA_MOVE, "CAMERA.move", "move", KS_API_CAMERA, FN, C, ANY) \
    X(CAMERA_ROTATE, "CAMERA.rotate", "rotate", KS_API_CAMERA, FN, C, ANY) \
    X(CAMERA_LOOKAT, "CAMERA.lookAt", "lookAt", KS_API_CAMERA, FN, C, ANY) \
    X(CAMERA_SHAKE, "CAMERA.shake", "shake", KS_API_CAMERA, FN, C, ANY) \
    X(CAMERA_FOV, "CAMERA.fov", "fov", KS_API_CAMERA, FN, C, ANY) \
    X(CAMERA_GETOBJ, "CAMERA.getObj", "getObj", KS_API_CAMERA, FN, C, OBJ) \
    X(CAMERA_POSITION, "CAMERA.position", "position", KS_API_CAMERA, NS, C, ANY) \
    X(CAMERA_POSITION_X, "CAMERA.position.x", "x", KS_API_CAMERA_POSITION, PROP, C, ANY) \
    X(CAMERA_POSITION_Y, "CAMERA.position.y", "y", KS_API_CAMERA_POSITION, PROP, C, ANY) \
    X(CAMERA_POSITION_Z, "CAMERA.position.z", "z", KS_API_CAMERA_POSITION, PROP, C, ANY) \
    X(CAMERA_ROTATION, "CAMERA.rotation", "rotation", KS_API_CAMERA, NS, C, ANY) \
    X(CAMERA_ROTATION_X, "CAMERA.rotation.x", "x", KS_API_CAMERA_ROTATION, PROP, C, ANY) \
    X(CAMERA_ROTATION_Y, "CAMERA.rotation.y", "y", KS_API_CAMERA_ROTATION, PROP, C, ANY) \
    X(CAMERA_ROTATION_Z, "CAMERA.rotation.z", "z", KS_API_CAMERA_ROTATION, PROP, C, ANY) \
    X(SCENE, "SCENE", "SCENE", -1, NS, C, ANY) \
    X(SCENE_POSTOSCREEN, "SCENE.posToScreen", "posToScreen", KS_API_SCENE, FN, C, OBJ) \
    X(SCENE_CLEAR, "SCENE.clear", "clear", KS_API_SCENE, FN, C, ANY) \
    X(SCENE_SETFOG, "SCENE.setFog", "setFog", KS_API_SCENE, FN, C, ANY) \
    X(SCENE_SETAMBIENTLIGHT, "SCENE.setAmbientLight", "setAmbientLight", KS_API_SCENE, FN, C, ANY) \
    X(SCENE_SETSKYLIGHT, "SCENE.setSkyLight", "setSkyLight", KS_API_SCENE, FN, C, ANY) \
    X(SCENE_SETSKYCOLOR, "SCENE.setSkyColor", "setSkyColor", KS_API_SCENE, FN, C, ANY) \
    X(SCENE_SETSKYDOME, "SCENE.setSkyDome", "setSkyDome", KS_API_SCENE, FN, C, ANY) \
    X(SCENE_REMOVESKYDOME, "SCENE.removeSkyDome", "removeSkyDome", KS_API_SCENE, FN, C, ANY) \
    X(SCENE_ADDCUSTOM, "SCENE.addCustom", "addCustom", KS_API_SCENE, FN, C, OBJ) \
    X(SCENE_ADDCUBE, "SCENE.addCube", "addCube", KS_API_SCENE, FN, C, OBJ) \
    X(SCENE_ADDPLANE, "SCENE.addPlane", "addPlane", KS_API_SCENE, FN, C, OBJ) \
    X(SCENE_ADDSPHERE, "SCENE.addSphere", "addSphere", KS_API_SCENE, FN, C, OBJ) \
    X(SCENE_ADDASSET, "SCENE.addAsset", "addAsset", KS_API_SCENE, FN, C, OBJ) \
    X(SCENE_ADDSPRITE, "SCENE.addSprite", "addSprite", KS_API_SCENE, FN, C, OBJ) \
    X(SCENE_ADDPOINTLIGHT, "SCENE.addPointLight", "addPointLight", KS_API_SCENE, FN, C, OBJ) \
    X(SCENE_ADDDIRLIGHT, "SCENE.addDirLight", "addDirLight", KS_API_SCENE, FN, C, OBJ) \
    X(SCENE_ADDSPOTLIGHT, "SCENE.addSpotLight", "addSpotLight", KS_API_SCENE, FN, C, OBJ) \
    X(SCENE_ADDRECTLIGHT, "SCENE.addRectLight", "addRectLight", KS_API_SCENE, FN, C, OBJ) \
    X(SCENE_ADDSIGN, "SCENE.addSign", "addSign", KS_API_SCENE, FN, C, OBJ) \
    X(SCENE_ADDLIGHTCONE, "SCENE.addLightCone", "addLightCone", KS_API_SCENE, FN, C, OBJ) \
    X(SCENE_ADDLIGHTBAR, "SCENE.addLightBar", "addLightBar", KS_API_SCENE, FN, C, OBJ) \
    X(SCENE_MOVOBJ, "SCENE.movObj", "movObj", KS_API_SCENE, FN, C, ANY) \
    X(SCENE_ROTOBJ, "SCENE.rotObj", "rotObj", KS_API_SCENE, FN, C, ANY) \
    X(SCENE_SCALEOBJ, "SCENE.scaleObj", "scaleObj", KS_API_SCENE, FN, C, ANY) \
    X(PAYMENTS, "PAYMENTS", "PAYMENTS", -1, NS, B, ANY) \
    X(PAYMENTS_GIVEKR, "PAYMENTS.giveKR", "giveKR", KS_API_PAYMENTS, FN, S, ANY) \
    X(PAYMENTS_CHARGE, "PAYMENTS.charge", "charge", KS_API_PAYMENTS, FN, B, ANY) \
    X(PLAYERS, "PLAYERS", "PLAYERS", -1, NS, B, ANY) \
    X(PLAYERS_GETSELF, "PLAYERS.getSelf", "getSelf", KS_API_PLAYERS, FN, C, OBJ) \
    X(PLAYERS_TOGGLELOD, "PLAYERS.toggleLOD", "toggleLOD", KS_API_PLAYERS, FN, C, ANY) \
    X(PLAYERS_DISABLEMESHES, "PLAYERS.disableMeshes", "disableMeshes", KS_API_PLAYERS, FN, C, ANY) \
    X(PLAYERS_LIST, "PLAYERS.list", "list", KS_API_PLAYERS, FN, B, OBJARR) \
    X(PLAYERS_FINDBYID, "PLAYERS.findByID", "findByID", KS_API_PLAYERS, FN, B, OBJ) \
    X(AI, "AI", "AI", -1, NS, S, ANY) \
    X(AI_SPAWN, "AI.spawn", "spawn", KS_API_AI, FN, S, OBJ) \
    X(AI_REMOVE, "AI.remove", "remove", KS_API_AI, FN, S, ANY) \
    X(AI_LIST, "AI.list", "list", KS_API_AI, FN, S, OBJARR) \
    X(TRIGGERS, "TRIGGERS", "TRIGGERS", -1, NS, B, ANY) \
    X(TRIGGERS_LIST, "TRIGGERS.list", "list", KS_API_TRIGGERS, FN, B, ANY) \
    X(TRIGGERS_EXECUTE, "TRIGGERS.execute", "execute", KS_API_TRIGGERS, FN, B, ANY) \
    X(OBJECTS, "OBJECTS", "OBJECTS", -1, NS, B, ANY) \
    X(OBJECTS_GETPATHNODES, "OBJECTS.getPathNodes", "getPathNodes", KS_API_OBJECTS, FN, B, OBJARR) \
    X(OBJECTS_LIST, "OBJECTS.list", "list", KS_API_OBJECTS, FN, B, OBJARR) \
    X(ADS, "ADS", "ADS", -1, NS, C, ANY) \
    X(ADS_PLAYVIDEO, "ADS.playVideo", "playVideo", KS_API_ADS, FN, C, ANY) \
    X(MODS, "MODS", "MODS", -1, NS, C, ANY) \
    X(MODS_LOAD, "MODS.load", "load", KS_API_MODS, FN, C, ANY) \
    X(MODS_RESET, "MODS.reset", "reset", KS_API_MODS, FN, C, ANY) \
    X(ADMIN, "ADMIN", "ADMIN", -1, NS, S, ANY) \
    X(ADMIN_KICK, "ADMIN.kick", "kick", KS_API_ADMIN, FN, S, ANY) \
    X(ADMIN_BAN, "ADMIN.ban", "ban", KS_API_ADMIN, FN, S, ANY) \
    X(PLATFORM, "PLATFORM", "PLATFORM", -1, NS, B, ANY) \
    X(PLATFORM_ISCLIENT, "PLATFORM.isClient", "isClient", KS_API_PLATFORM, FN, B, BOOL) \
    X(PLATFORM_ISBROWSER, "PLATFORM.isBrowser", "isBrowser", KS_API_PLATFORM, FN, B, BOOL) \
    X(PLATFORM_ISMOBILE, "PLATFORM.isMobile", "isMobile", KS_API_PLATFORM, FN, B, BOOL) \
    X(CONFIG, "CONFIG", "CONFIG", -1, NS, B, ANY) \
    X(CONFIG_GETHOST, "CONFIG.getHost", "getHost", KS_API_CONFIG, FN, S, OBJ) \
    X(CONFIG_GETMATCH, "CONFIG.getMatch", "getMatch", KS_API_CONFIG, FN, B, ANY) \
    X(CONFIG_GETSETTINGS, "CONFIG.getSettings", "getSettings", KS_API_CONFIG, FN, B, ANY) \
    X(CONFIG_GETWEAPONS, "CONFIG.getWeapons", "getWeapons", KS_API_CONFIG, FN, B, ANY) \
    X(CONFIG_GETCLASSES, "CONFIG.getClasses", "getClasses", KS_API_CONFIG, FN, B, ANY)

typedef enum {
#define KS_API_X_ENUM(id, path, name, parent, kind, sides, ret) KS_API_##id,
    KS_API_LIST(KS_API_X_ENUM)
#undef KS_API_X_ENUM
    KS_API_COUNT
} KsApiId;

typedef struct {
    const char *path;     // without "GAME.", e.g. "SCENE.addCube"
    const char *name;     // last path segment, e.g. "addCube"
    uint8_t kind;         // KsApiKind
    uint8_t sides;        // KS_API_SIDE_* where it does something (else gated no-op)
    KsType ret;           // FN: result type (TYP); PROP: value type; NS: any
    int parent;           // enclosing namespace id, -1 for GAME root members
} KsApiEntry;

const KsApiEntry *ks_api_entry(int id);          // NULL when out of range
int               ks_api_find(const char *path); // id or -1 ("SCENE.addCube", no "GAME.")
int               ks_api_child(int parent, const char *name); // parent -1 = GAME root; id or -1

#ifdef __cplusplus
}
#endif

#endif
