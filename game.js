// Hot-reloadable ZQJS gameplay script for zqjs-raylib.
//
// Try editing CONFIG below and saving this file while the window is open:
// the C host reloads game.js live without restarting Raylib!
//
// Want to test the sandbox? Uncomment one of these and save:
//   while (true) {}                  // caught by max_steps in <2ms, window stays alive!
//   throw new Error("live crash!");  // caught and shown in the HUD overlay!

const CONFIG = {
  orbCount: 36,
  playerSpeed: 310,
  gravity: 1100,
  burstForce: 420,
  orbSpeed: 130,
  bg: [13, 17, 28],
};

const PALETTE = [
  [88, 196, 255],
  [255, 118, 178],
  [124, 242, 168],
  [255, 206, 92],
  [184, 146, 255],
];

const W = 900;
const H = 560;
const TOP_BAR = 44;
const BOT_BAR = 38;

const player = {
  x: W * 0.5,
  y: H * 0.5,
  r: 14,
  pulse: 0,
  score: 0,
};

let well = { active: false, x: W * 0.5, y: H * 0.5 };
const orbs = [];
const sparks = [];

function spawnOrb(i) {
  const angle = (i / CONFIG.orbCount) * Math.PI * 2;
  const speed = CONFIG.orbSpeed * (0.6 + Math.random() * 0.8);
  const col = PALETTE[i % PALETTE.length];
  return {
    x: W * 0.5 + Math.cos(angle) * (110 + (i % 5) * 32),
    y: H * 0.5 + Math.sin(angle) * (80 + (i % 4) * 28),
    vx: Math.cos(angle + 1.2) * speed,
    vy: Math.sin(angle + 1.2) * speed,
    r: 5 + (i % 4) * 2.5,
    c: col,
  };
}

for (let i = 0; i < CONFIG.orbCount; i++) {
  orbs.push(spawnOrb(i));
}

function addSparks(x, y, col, count) {
  for (let i = 0; i < count; i++) {
    const a = Math.random() * Math.PI * 2;
    const sp = 50 + Math.random() * 190;
    sparks.push({
      x,
      y,
      vx: Math.cos(a) * sp,
      vy: Math.sin(a) * sp,
      life: 0.35 + Math.random() * 0.35,
      c: col,
    });
  }
}

function update(input) {
  const dt = Math.min(input.dt, 0.05);

  // Move player with WASD / Arrow keys
  let mx = (input.right ? 1 : 0) - (input.left ? 1 : 0);
  let my = (input.down ? 1 : 0) - (input.up ? 1 : 0);
  if (mx !== 0 && my !== 0) {
    mx *= 0.7071;
    my *= 0.7071;
  }
  player.x = Math.max(player.r, Math.min(W - player.r, player.x + mx * CONFIG.playerSpeed * dt));
  player.y = Math.max(
    TOP_BAR + player.r,
    Math.min(H - BOT_BAR - player.r, player.y + my * CONFIG.playerSpeed * dt)
  );

  // Hold mouse button to place an active gravity well
  well.active = input.mouseDown;
  if (input.mouseDown) {
    well.x = input.mouseX;
    well.y = input.mouseY;
  }

  // Space triggers a radial shockwave that repels nearby orbs
  if (input.space && player.pulse <= 0) {
    player.pulse = 1.0;
    addSparks(player.x, player.y, [255, 224, 130], 14);
    for (let i = 0; i < orbs.length; i++) {
      const o = orbs[i];
      const dx = o.x - player.x;
      const dy = o.y - player.y;
      const d = Math.hypot(dx, dy) || 1;
      if (d < 220) {
        const push = ((220 - d) / 220) * CONFIG.burstForce;
        o.vx += (dx / d) * push;
        o.vy += (dy / d) * push;
      }
    }
  }
  if (player.pulse > 0) {
    player.pulse = Math.max(0, player.pulse - dt * 2.4);
  }

  // Update orbiting orbs
  for (let i = 0; i < orbs.length; i++) {
    const o = orbs[i];
    if (well.active) {
      const dx = well.x - o.x;
      const dy = well.y - o.y;
      const d = Math.hypot(dx, dy) + 1;
      const falloff = 1 / (1 + d / 260);
      const accel = CONFIG.gravity * falloff * dt;
      // Pull inward + slight tangential swirl so orbs orbit instead of clumping flat
      const nx = dx / d;
      const ny = dy / d;
      o.vx += (nx * accel) + (-ny * accel * 0.25);
      o.vy += (ny * accel) + (nx * accel * 0.25);
    }

    // Mild damping + speed cap so orbits stay energetic and controllable
    const drag = 1 - 0.35 * dt;
    o.vx *= drag;
    o.vy *= drag;
    const spd = Math.hypot(o.vx, o.vy);
    if (spd > 520) {
      o.vx = (o.vx / spd) * 520;
      o.vy = (o.vy / spd) * 520;
    }
    o.x += o.vx * dt;
    o.y += o.vy * dt;

    if (o.x - o.r < 0) { o.x = o.r; o.vx = Math.abs(o.vx); }
    if (o.x + o.r > W) { o.x = W - o.r; o.vx = -Math.abs(o.vx); }
    if (o.y - o.r < TOP_BAR) { o.y = TOP_BAR + o.r; o.vy = Math.abs(o.vy); }
    if (o.y + o.r > H - BOT_BAR) { o.y = H - BOT_BAR - o.r; o.vy = -Math.abs(o.vy); }

    // Collect / bounce on player contact
    const px = o.x - player.x;
    const py = o.y - player.y;
    const minD = o.r + player.r;
    if (px * px + py * py < minD * minD) {
      player.score += 10;
      addSparks(o.x, o.y, o.c, 6);
      const a = Math.atan2(py, px);
      o.x = player.x + Math.cos(a) * (minD + 2);
      o.y = player.y + Math.sin(a) * (minD + 2);
      o.vx = Math.cos(a) * 210;
      o.vy = Math.sin(a) * 210;
    }
  }

  // Update short-lived sparks (exercises ZQJS generational nursery GC smoothly)
  for (let i = sparks.length - 1; i >= 0; i--) {
    const s = sparks[i];
    s.x += s.vx * dt;
    s.y += s.vy * dt;
    s.life -= dt;
    if (s.life <= 0) sparks.splice(i, 1);
  }
}

function draw() {
  const [br, bg, bb] = CONFIG.bg;
  clearBg(br, bg, bb);

  // Subtle connection lines between nearby orbs
  for (let i = 0; i < orbs.length; i++) {
    const a = orbs[i];
    for (let j = i + 1; j < orbs.length; j++) {
      const b = orbs[j];
      const dx = a.x - b.x;
      const dy = a.y - b.y;
      if (dx * dx + dy * dy < 7200) {
        drawLine(a.x, a.y, b.x, b.y, 90, 120, 175, 42);
      }
    }
  }

  // Gravity well indicator + pull tethers
  if (well.active) {
    for (let i = 0; i < orbs.length; i++) {
      const o = orbs[i];
      const dx = well.x - o.x;
      const dy = well.y - o.y;
      if (dx * dx + dy * dy < 90000) {
        drawLine(well.x, well.y, o.x, o.y, 110, 200, 255, 55);
      }
    }
    drawRing(well.x, well.y, 44, 120, 210, 255, 110);
    drawRing(well.x, well.y, 24, 160, 230, 255, 190);
    drawCircle(well.x, well.y, 6, 210, 245, 255, 240);
  }

  // Orbs
  for (let i = 0; i < orbs.length; i++) {
    const o = orbs[i];
    drawCircle(o.x, o.y, o.r + 4, o.c[0], o.c[1], o.c[2], 45);
    drawCircle(o.x, o.y, o.r, o.c[0], o.c[1], o.c[2], 255);
  }

  // Sparks
  for (let i = 0; i < sparks.length; i++) {
    const s = sparks[i];
    const alpha = Math.max(0, Math.min(255, s.life * 400));
    drawCircle(s.x, s.y, 2.5, s.c[0], s.c[1], s.c[2], alpha);
  }

  // Player + shockwave ring
  if (player.pulse > 0) {
    const ringR = player.r + (1 - player.pulse) * 180;
    drawRing(player.x, player.y, ringR, 255, 224, 130, player.pulse * 220);
  }
  drawCircle(player.x, player.y, player.r + 5, 255, 255, 255, 40);
  drawCircle(player.x, player.y, player.r, 245, 250, 255, 255);

  // Top HUD bar (20px = 2x Raylib's crisp 10px default font)
  drawRect(0, 0, W, TOP_BAR, 18, 24, 40, 245);
  drawText(`SCORE: ${player.score}   ORBS: ${orbs.length}`, 16, 12, 20, 235, 244, 255);
  drawText("WASD: move  SPACE: pulse  CLICK: gravity  R: reload", 320, 12, 20, 155, 180, 220);
}

registerGame({ update, draw });
