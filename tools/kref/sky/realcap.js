// usage: node realcap.js <posesFile> <outDir> <map>...   (map names as in Game.map.maps, lower-case file names)
// poses: [{name, pos:[x,y,z], pitch, yaw}] ; camera fov forced to 90 (vertical), matching KrunkNative.
const { CDP, sleep } = require('/home/user/src/KrunkNative/tools/kref/cdp.js');
const fs = require('fs');
const [posesFile, outDir, ...mapsWanted] = process.argv.slice(2);
const allPoses = JSON.parse(fs.readFileSync(posesFile, 'utf8'));
(async () => {
  fs.mkdirSync(outDir, { recursive: true });
  const c = await CDP.connect(+(process.env.CDP_PORT || 7774));
  await c.send('Page.enable'); await c.send('Runtime.enable');
  await c.send('Page.addScriptToEvaluateOnNewDocument', { source: fs.readFileSync('/home/user/src/KrunkNative/tools/kref/shim.js', 'utf8') });
  await c.setViewport(1280, 720);
  await c.send('Page.navigate', { url: '' + (process.env.URL || 'http://127.0.0.1:6774/') + '' });
  await c.waitFor(`(()=>{const e=document.getElementById('instructions');return !!e && /PLAY/i.test(e.textContent||'')})()`, 120000);
  await sleep(3000);
  const names = await c.eval(`JSON.stringify(Game.map.maps.map(m=>m.name.toLowerCase().replace(/ /g,'_')))`);
  const idxOf = JSON.parse(names);
  for (const tag of mapsWanted) {
    const poses = allPoses[tag];
    await c.eval(`window.__skyPose=null; Game.map.generate(${idxOf.indexOf(tag)}, Game.map.modes[0], true); 0`);
    await sleep(5000);
    const info = await c.eval(`(()=>{
      const R=RENDER, cv=R.renderer.domElement;
      for (const e of document.body.querySelectorAll('body > *')) if (e!==cv && !e.contains(cv)) e.style.setProperty('display','none','important');
      let n=cv.parentElement; while(n && n!==document.body){ for(const s of n.children) if(s!==cv && !s.contains(cv)) s.style.setProperty('display','none','important'); n=n.parentElement; }
      if (!R.__origRender) { R.__origRender = R.render; R.render = function(){ const p=window.__skyPose; if(p){ const cam=R.camera; const ctl=CONTROLS;
        ctl.object.position.set(p.pos[0],p.pos[1],p.pos[2]); ctl.object.rotation.set(0,0,0); ctl.pitchObj.rotation.set(0,0,0); ctl.pitchObj.position.set(0,0,0);
        cam.position.set(0,0,0); cam.rotation.set(p.pitch,p.yaw,0,'YXZ'); if(cam.fov!==90){cam.fov=90;cam.updateProjectionMatrix();} }
        return R.__origRender.apply(this, arguments); }; }
      return JSON.stringify({map: document.getElementById('mapInfo') && document.getElementById('mapInfo').textContent, sky: !!R.skyDome, fog: R.scene.fog && [R.scene.fog.color.getHexString(), R.scene.fog.far], far: R.camera.far, shadows: R.shadows, shadType: R.renderer.shadowMap.type, light: R.skyLight && [R.skyLight.position.toArray(), R.skyLight.shadow.camera.left, R.skyLight.shadow.camera.top, R.skyLight.shadow.camera.far, R.skyLight.shadow.mapSize.x]});
    })()`);
    console.log(tag, info);
    for (const p of poses) {
      await c.eval(`window.__skyPose=${JSON.stringify(p)};0`);
      await sleep(1500);
      await c.shot(`${outDir}/${tag}-${p.name}.png`);
      console.log('shot', `${outDir}/${tag}-${p.name}.png`);
    }
  }
  c.close(); process.exit(0);
})().catch(e => { console.error(e); process.exit(1); });
