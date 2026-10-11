const { CDP } = require('./cdp.js');
(async () => {
  for (const port of [9333, 9334]) {
    const c = await CDP.connect(port);
    const r = await c.eval(`JSON.stringify({url:location.href, gid:(window.gameId||null), lobby: (typeof Game!=='undefined' && (Game.lobbyId||Game.gameId||Game.id))||null, players: typeof Game!=='undefined' && Game.players.list.map(p=>({sid:p.sid,alias:p.alias,you:!!p.isYou,active:p.active,x:Math.round(p.x),y:Math.round(p.y),z:Math.round(p.z),hp:p.health,team:p.team})), igu: document.getElementById('inGameUI').style.display, timer: document.getElementById('timerVal').textContent, gmsg: document.getElementById('gameMessage') && document.getElementById('gameMessage').textContent, leader: document.getElementById('leaderContainer')&&document.getElementById('leaderContainer').textContent.replace(/\\s+/g,' ').slice(0,200)})`);
    console.log(port, r); c.close();
  }
  process.exit(0);
})().catch(e => { console.error(e); process.exit(1); });
