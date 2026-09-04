// Hotel Tower — three.js edition for hotel-universe.travel
// Stack floors on alternating axes, street to the stars. Floor 50 earns a
// promo code; every extra floor a bigger one. Star tier every 20 floors
// changes the architecture. Five perfect drops = +5 bonus floors.

import * as THREE from 'three';
import { EffectComposer } from './vendor/postprocessing/EffectComposer.js';
import { RenderPass } from './vendor/postprocessing/RenderPass.js';
import { UnrealBloomPass } from './vendor/postprocessing/UnrealBloomPass.js';
import { OutputPass } from './vendor/postprocessing/OutputPass.js';

// ---------- promo codes (mirrors the ESP32 + promo_codes.js) ----------
const MASK = (1n << 64n) - 1n;
const ALPHA = '23456789abcdefghjkmnpqrstuvwxyz';
function promoCode(floors) {
  let x = ((BigInt(floors) * 0x9e3779b97f4a7c15n) ^ 0x48554e4956455253n) & MASK;
  x ^= x >> 30n; x = (x * 0xbf58476d1ce4e5b9n) & MASK;
  x ^= x >> 27n; x = (x * 0x94d049bb133111ebn) & MASK;
  x ^= x >> 31n;
  let h = '';
  for (let i = 0; i < 8; i++) { h += ALPHA[Number(x % 31n)]; x /= 31n; }
  return 'hu-' + h;
}

// ---------- constants ----------
const BASE = 10;            // footprint of the base floor (x and z)
const FLOOR_H = 1.15;
const PERFECT_EPS = 0.38;
const PROMO_FLOORS = 50;
const TRAVEL = 11;          // slider travel from center
const COLORS = {
  cream: 0xfaf6ec, ink: 0x171614, yellow: 0xffcd3c,
};
const starRating = f => Math.min(6, Math.floor(f / 20));
const balconyCadence = t => (t >= 4 ? 2 : t === 3 ? 3 : 5);

// ---------- tier facade textures (canvas-built) ----------
const TIERS = [
  { base: '#a8a49e', roof: '#8b8781', lit: .18, glow: '#ffd98c' },   // concrete
  { base: '#eee2c8', roof: '#c9b795', lit: .35, glow: '#ffd98c' },   // cream + awnings
  { base: '#d8b284', roof: '#b08b55', lit: .45, glow: '#ffd98c' },   // terracotta + gold
  { base: '#f2f0ea', roof: '#c9c5ba', lit: .55, glow: '#ffd98c' },   // white + shutters
  { base: '#f6f2ea', roof: '#d8d0c0', lit: .6,  glow: '#ffd98c' },   // marble + pilasters
  { base: '#37517a', roof: '#24374f', lit: .7,  glow: '#bfe0ff' },   // glass tower
  { base: '#eac048', roof: '#c69f2c', lit: .8,  glow: '#fff3c0' },   // royal gold
];

function makeTierTextures(tier) {
  const W = 512, H = 128;
  const face = document.createElement('canvas'); face.width = W; face.height = H;
  const glow = document.createElement('canvas'); glow.width = W; glow.height = H;
  const f = face.getContext('2d'), g = glow.getContext('2d');
  const T = TIERS[tier];
  f.fillStyle = T.base; f.fillRect(0, 0, W, H);
  g.fillStyle = '#000'; g.fillRect(0, 0, W, H);

  const rng = (seed => () => (seed = (seed * 1664525 + 1013904223) >>> 0) / 2 ** 32)(tier * 999 + 7);

  if (tier === 5) {
    // full curtain wall
    for (let x = 8; x < W - 8; x += 42) for (let y = 10; y < H - 10; y += 52) {
      const lit = rng() < T.lit;
      f.fillStyle = lit ? '#9cc6ee' : '#274064'; f.fillRect(x, y, 34, 44);
      f.strokeStyle = '#16233a'; f.lineWidth = 3; f.strokeRect(x, y, 34, 44);
      if (lit) { g.fillStyle = T.glow; g.fillRect(x, y, 34, 44); }
    }
    const t = new THREE.CanvasTexture(face), e = new THREE.CanvasTexture(glow);
    t.colorSpace = THREE.SRGBColorSpace;
    return { map: t, emissiveMap: e };
  }

  for (let x = 14; x < W - 40; x += 52) {
    for (let y = 16; y < H - 30; y += 56) {
      const lit = rng() < T.lit;
      // window
      f.fillStyle = lit ? '#ffe9b0' : (tier === 0 ? '#3a3835' : '#46423c');
      f.fillRect(x, y, 30, 38);
      if (lit) { g.fillStyle = T.glow; g.fillRect(x + 2, y + 2, 26, 34); }
      if (tier === 1) {           // red striped awning
        for (let i = 0; i < 5; i++) {
          f.fillStyle = i % 2 ? '#fff' : '#c83228';
          f.fillRect(x - 4 + i * 8, y - 10, 8, 10);
        }
      } else if (tier === 2 || tier === 6) {   // frames
        f.strokeStyle = tier === 2 ? '#8a6c14' : '#ffffff';
        f.lineWidth = 4; f.strokeRect(x - 2, y - 2, 34, 42);
      } else if (tier === 3) {    // green shutters
        f.fillStyle = '#2e5a30';
        f.fillRect(x - 9, y, 7, 38); f.fillRect(x + 32, y, 7, 38);
      }
      f.fillStyle = 'rgba(0,0,0,.18)'; f.fillRect(x, y, 30, 5); // lintel shadow
    }
    if (tier === 4) {             // pilasters between windows
      f.fillStyle = '#ddd0b0'; f.fillRect(x + 38, 6, 10, H - 12);
      f.fillStyle = 'rgba(0,0,0,.12)'; f.fillRect(x + 46, 6, 2, H - 12);
    }
  }
  if (tier === 2) { f.fillStyle = '#8a6c14'; f.fillRect(0, H - 8, W, 8); }
  if (tier === 4) { f.fillStyle = '#b4941f'; f.fillRect(0, 0, W, 6); f.fillRect(0, H - 6, W, 6); }
  if (tier === 6) {
    f.fillStyle = '#fff';
    for (let i = 0; i < 9; i++) {
      const sx = 40 + i * 52, sy = H - 18;
      f.beginPath();
      for (let k = 0; k < 10; k++) {
        const r = k % 2 ? 2.4 : 6, a = (k / 10) * Math.PI * 2 - Math.PI / 2;
        f[k ? 'lineTo' : 'moveTo'](sx + Math.cos(a) * r, sy + Math.sin(a) * r);
      }
      f.fill();
    }
    const grad = f.createLinearGradient(0, 0, W, H);
    grad.addColorStop(.3, 'rgba(255,255,255,0)');
    grad.addColorStop(.5, 'rgba(255,255,255,.35)');
    grad.addColorStop(.7, 'rgba(255,255,255,0)');
    f.fillStyle = grad; f.fillRect(0, 0, W, H);
  }
  const t = new THREE.CanvasTexture(face), e = new THREE.CanvasTexture(glow);
  t.colorSpace = THREE.SRGBColorSpace;
  t.wrapS = e.wrapS = THREE.RepeatWrapping;
  return { map: t, emissiveMap: e };
}
const tierTex = TIERS.map((_, i) => makeTierTextures(i));

// ---------- renderer / scene ----------
const app = document.getElementById('app');
const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
renderer.setSize(innerWidth, innerHeight);
renderer.shadowMap.enabled = true;
renderer.shadowMap.type = THREE.PCFSoftShadowMap;
renderer.toneMapping = THREE.ACESFilmicToneMapping;
renderer.toneMappingExposure = 1.08;
app.appendChild(renderer.domElement);

const scene = new THREE.Scene();
scene.fog = new THREE.Fog(0xcfe0ec, 60, 180);

const camera = new THREE.PerspectiveCamera(46, innerWidth / innerHeight, 0.1, 800);

const composer = new EffectComposer(renderer);
composer.addPass(new RenderPass(scene, camera));
const bloom = new UnrealBloomPass(new THREE.Vector2(innerWidth, innerHeight), 0.55, 0.65, 0.82);
composer.addPass(bloom);
composer.addPass(new OutputPass());

addEventListener('resize', () => {
  camera.aspect = innerWidth / innerHeight;
  camera.updateProjectionMatrix();
  renderer.setSize(innerWidth, innerHeight);
  composer.setSize(innerWidth, innerHeight);
});

// lights
const hemi = new THREE.HemisphereLight(0xdfeaf5, 0x6a6258, 0.85);
scene.add(hemi);
const sun = new THREE.DirectionalLight(0xfff2d8, 2.2);
sun.castShadow = true;
sun.shadow.mapSize.set(2048, 2048);
sun.shadow.camera.left = sun.shadow.camera.bottom = -30;
sun.shadow.camera.right = sun.shadow.camera.top = 30;
sun.shadow.bias = -0.0004;
scene.add(sun, sun.target);

// ground: asphalt disc + brand-yellow glow ring
const ground = new THREE.Mesh(
  new THREE.CylinderGeometry(60, 60, 1, 72),
  new THREE.MeshStandardMaterial({ color: 0x232326, roughness: 0.95 })
);
ground.position.y = -0.5;
ground.receiveShadow = true;
scene.add(ground);
const ring = new THREE.Mesh(
  new THREE.TorusGeometry(11.5, 0.16, 10, 90),
  new THREE.MeshBasicMaterial({ color: COLORS.yellow })
);
ring.rotation.x = -Math.PI / 2; ring.position.y = 0.06;
scene.add(ring);
for (let i = 0; i < 3; i++) {                       // curb rings
  const r = new THREE.Mesh(
    new THREE.TorusGeometry(18 + i * 12, 0.05, 6, 90),
    new THREE.MeshBasicMaterial({ color: 0x3a3a40 })
  );
  r.rotation.x = -Math.PI / 2; r.position.y = 0.04;
  scene.add(r);
}

// stars & celestial bodies (fade in with altitude)
function makeStarField(count, radius, size) {
  const pos = new Float32Array(count * 3);
  for (let i = 0; i < count; i++) {
    const v = new THREE.Vector3().randomDirection().multiplyScalar(radius * (0.8 + Math.random() * 0.4));
    v.y = Math.abs(v.y) + 6;
    pos.set([v.x, v.y, v.z], i * 3);
  }
  const geo = new THREE.BufferGeometry();
  geo.setAttribute('position', new THREE.BufferAttribute(pos, 3));
  const mat = new THREE.PointsMaterial({
    color: 0xffffff, size, sizeAttenuation: true,
    transparent: true, opacity: 0, depthWrite: false,
  });
  return new THREE.Points(geo, mat);
}
const starsNear = makeStarField(700, 220, 0.9);
const starsFar = makeStarField(900, 380, 0.6);
scene.add(starsNear, starsFar);

const moon = new THREE.Mesh(
  new THREE.SphereGeometry(6, 32, 32),
  new THREE.MeshBasicMaterial({ color: 0xfff3d0, transparent: true, opacity: 0 })
);
moon.position.set(-90, 120, -140);
scene.add(moon);
const planet = new THREE.Group();
const planetBody = new THREE.Mesh(
  new THREE.SphereGeometry(9, 32, 32),
  new THREE.MeshBasicMaterial({ color: 0xd9a06a, transparent: true, opacity: 0 })
);
const planetRing = new THREE.Mesh(
  new THREE.TorusGeometry(14, 0.9, 8, 64),
  new THREE.MeshBasicMaterial({ color: 0xf0d9b0, transparent: true, opacity: 0 })
);
planetRing.rotation.x = Math.PI / 2.6;
planet.add(planetBody, planetRing);
planet.position.set(110, 190, -160);
scene.add(planet);

// ---------- sky phases ----------
const SKY = [
  { h: 0,   top: 0xbcd4e8, fog: 0xcfe0ec, hemi: 0.85, sun: 2.2,  night: 0 },
  { h: 22,  top: 0x9078b8, fog: 0xf0b878, hemi: 0.55, sun: 1.4,  night: .25 },
  { h: 42,  top: 0x1a2148, fog: 0x35304e, hemi: 0.3,  sun: 0.55, night: .8 },
  { h: 66,  top: 0x05060f, fog: 0x0a0c1a, hemi: 0.18, sun: 0.3,  night: 1 },
];
const bgColor = new THREE.Color(SKY[0].top);
scene.background = bgColor;
function skyAt(h) {
  let i = 0;
  while (i < SKY.length - 2 && h > SKY[i + 1].h) i++;
  const a = SKY[i], b = SKY[i + 1];
  const t = THREE.MathUtils.clamp((h - a.h) / (b.h - a.h), 0, 1);
  return {
    top: new THREE.Color(a.top).lerp(new THREE.Color(b.top), t),
    fog: new THREE.Color(a.fog).lerp(new THREE.Color(b.fog), t),
    hemi: THREE.MathUtils.lerp(a.hemi, b.hemi, t),
    sun: THREE.MathUtils.lerp(a.sun, b.sun, t),
    night: THREE.MathUtils.lerp(a.night, b.night, t),
  };
}

// ---------- tower ----------
const tower = new THREE.Group();
scene.add(tower);

// materials are cached by (tier, width bucket): identical floors share GPU
// textures, and the night-glow loop only has to touch the cache
const matCache = new Map();
const roofCache = new Map();
function sideMat(tier, rep) {
  const key = `${tier}:${rep.toFixed(2)}`;
  if (!matCache.has(key)) {
    const tex = tierTex[tier];
    const map = tex.map.clone(); map.repeat.set(rep, 1); map.needsUpdate = true;
    const em = tex.emissiveMap.clone(); em.repeat.set(rep, 1); em.needsUpdate = true;
    matCache.set(key, new THREE.MeshStandardMaterial({
      map, emissiveMap: em, emissive: 0xffffff, emissiveIntensity: 0,
      roughness: tier === 5 ? 0.25 : tier === 6 ? 0.35 : 0.8,
      metalness: tier === 6 ? 0.55 : tier === 5 ? 0.3 : 0.05,
    }));
  }
  return matCache.get(key);
}
function floorMaterials(tier, w, d) {
  if (!roofCache.has(tier)) {
    roofCache.set(tier, new THREE.MeshStandardMaterial({ color: TIERS[tier].roof, roughness: 0.9 }));
  }
  const roof = roofCache.get(tier);
  const q = v => Math.max(0.25, Math.round((v / BASE) * 20) / 20);
  const sideX = sideMat(tier, q(w));
  const sideZ = sideMat(tier, q(d));
  return [sideZ, sideZ, roof, roof, sideX, sideX]; // +x,-x,+y,-y,+z,-z
}

function makeFloorMesh(w, d, tier, floorIdx) {
  const mesh = new THREE.Mesh(new THREE.BoxGeometry(w, FLOOR_H, d), floorMaterials(tier, w, d));
  mesh.castShadow = mesh.receiveShadow = true;
  // balconies by cadence
  if (floorIdx > 0 && floorIdx % balconyCadence(tier) === 0) {
    const ledge = new THREE.Mesh(
      new THREE.BoxGeometry(w + 0.9, 0.14, d + 0.9),
      new THREE.MeshStandardMaterial({ color: 0x2a2825, roughness: 0.8 })
    );
    ledge.position.y = -FLOOR_H / 2 + 0.07;
    ledge.castShadow = true;
    mesh.add(ledge);
    const railMat = new THREE.MeshStandardMaterial({ color: 0x2a2825, roughness: 0.6, metalness: 0.4 });
    const railH = 0.34;
    for (const [sx, sz, lx, lz] of [[0, (d + 0.9) / 2, w + 0.9, 0.06], [0, -(d + 0.9) / 2, w + 0.9, 0.06], [(w + 0.9) / 2, 0, 0.06, d + 0.9], [-(w + 0.9) / 2, 0, 0.06, d + 0.9]]) {
      const rail = new THREE.Mesh(new THREE.BoxGeometry(lx, 0.05, lz), railMat);
      rail.position.set(sx, -FLOOR_H / 2 + railH, sz);
      mesh.add(rail);
    }
  }
  return mesh;
}

// ---------- particles: fireworks + debris ----------
const debris = [];
function spawnDebris(w, d, x, z, y, tier) {
  const m = new THREE.Mesh(new THREE.BoxGeometry(w, FLOOR_H, d), floorMaterials(tier, w, d));
  m.position.set(x, y, z);
  m.castShadow = true;
  scene.add(m);
  debris.push({
    mesh: m,
    vel: new THREE.Vector3((Math.random() - 0.5) * 2, 1.5, (Math.random() - 0.5) * 2),
    rot: new THREE.Vector3((Math.random() - 0.5) * 3, (Math.random() - 0.5) * 3, (Math.random() - 0.5) * 3),
  });
}

const fireworks = [];
const FW_COLORS = [0xffcd3c, 0xffffff, 0xff8c28, 0xeb4632, 0x8cc8ff];
function spawnFirework(y) {
  const n = 130;
  const pos = new Float32Array(n * 3), vel = [];
  const cx = (Math.random() - 0.5) * 26, cy = y + 4 + Math.random() * 10, cz = (Math.random() - 0.5) * 26;
  for (let i = 0; i < n; i++) {
    pos.set([cx, cy, cz], i * 3);
    vel.push(new THREE.Vector3().randomDirection().multiplyScalar(4 + Math.random() * 9));
  }
  const geo = new THREE.BufferGeometry();
  geo.setAttribute('position', new THREE.BufferAttribute(pos, 3));
  const mat = new THREE.PointsMaterial({
    color: FW_COLORS[(Math.random() * FW_COLORS.length) | 0],
    size: 0.34, transparent: true, opacity: 1,
    blending: THREE.AdditiveBlending, depthWrite: false,
  });
  const pts = new THREE.Points(geo, mat);
  scene.add(pts);
  fireworks.push({ pts, vel, life: 1 });
  sound.pop();
}

// ---------- sound (synthesized, no assets) ----------
const sound = (() => {
  let ctx = null;
  const ac = () => (ctx ??= new (window.AudioContext || window.webkitAudioContext)());
  function env(node, t0, a, d, peak = 0.3) {
    const g = ac().createGain();
    g.gain.setValueAtTime(0, t0);
    g.gain.linearRampToValueAtTime(peak, t0 + a);
    g.gain.exponentialRampToValueAtTime(0.001, t0 + a + d);
    node.connect(g).connect(ac().destination);
    return g;
  }
  return {
    unlock() { try { ac().resume(); } catch {} },
    thunk() {
      try {
        const t = ac().currentTime, o = ac().createOscillator();
        o.type = 'sine'; o.frequency.setValueAtTime(150, t);
        o.frequency.exponentialRampToValueAtTime(48, t + 0.14);
        env(o, t, 0.004, 0.16, 0.5); o.start(t); o.stop(t + 0.2);
      } catch {}
    },
    ding(step = 0) {
      try {
        const t = ac().currentTime, o = ac().createOscillator();
        o.type = 'triangle';
        o.frequency.value = 660 * Math.pow(1.122, step);
        env(o, t, 0.004, 0.28, 0.22); o.start(t); o.stop(t + 0.32);
      } catch {}
    },
    fanfare() {
      try {
        [523, 659, 784, 1046, 1318].forEach((f, i) => {
          const t = ac().currentTime + i * 0.07, o = ac().createOscillator();
          o.type = 'square'; o.frequency.value = f;
          env(o, t, 0.005, 0.3, 0.12); o.start(t); o.stop(t + 0.34);
        });
      } catch {}
    },
    pop() {
      try {
        const t = ac().currentTime;
        const buf = ac().createBuffer(1, 4410, 44100);
        const d = buf.getChannelData(0);
        for (let i = 0; i < d.length; i++) d[i] = (Math.random() * 2 - 1) * Math.pow(1 - i / d.length, 2.5);
        const s = ac().createBufferSource(); s.buffer = buf;
        env(s, t, 0.001, 0.3, 0.25); s.start(t);
      } catch {}
    },
    over() {
      try {
        [392, 330, 262, 196].forEach((f, i) => {
          const t = ac().currentTime + i * 0.16, o = ac().createOscillator();
          o.type = 'sawtooth'; o.frequency.value = f;
          env(o, t, 0.01, 0.24, 0.1); o.start(t); o.stop(t + 0.28);
        });
      } catch {}
    },
  };
})();

// ---------- game state ----------
const $ = id => document.getElementById(id);
const hud = $('hud'), floorsEl = $('floors'), starsEl = $('stars');
const pips = [...document.querySelectorAll('#pips span')];

let state = 'menu';          // menu | playing | promo | over
let floors = 0;
let stack = [];              // { mesh, x, z, w, d }
let moving = null;           // { mesh, axis, w, d, t }
let speed = 5.2;
let streak = 0;
let bonus = 0, bonusTimer = 0;
let promoShownThisRun = false;
let best = +(localStorage.getItem('hu_tower_best') || 0);
let shake = 0;
let pendingDebug = 0;  // ?debug=N floors still to auto-build (spread over frames)
let orbit = Math.PI * 0.28, orbitAuto = 0.05;
let camPos = new THREE.Vector3(24, 16, 24), camTgt = new THREE.Vector3(0, 2, 0);
let overOrbit = 0;

function topBlock() { return stack[stack.length - 1]; }
function topY() { return stack.length * FLOOR_H; }

function resetGame() {
  for (const b of stack) tower.remove(b.mesh);
  for (const d of debris) scene.remove(d.mesh);
  debris.length = 0;
  if (moving) { tower.remove(moving.mesh); moving = null; }
  stack = [];
  floors = 0; streak = 0; bonus = 0; speed = 5.2;
  promoShownThisRun = false;
  const baseMesh = makeFloorMesh(BASE, BASE, 0, 0);
  baseMesh.position.set(0, FLOOR_H / 2, 0);
  tower.add(baseMesh);
  stack.push({ mesh: baseMesh, x: 0, z: 0, w: BASE, d: BASE });
  spawnMoving();
  updateHud();
}

function spawnMoving() {
  const prev = topBlock();
  const axis = stack.length % 2 ? 'x' : 'z';
  const tier = starRating(floors + 1);
  const mesh = makeFloorMesh(prev.w, prev.d, tier, floors + 1);
  const y = topY() + FLOOR_H / 2;
  mesh.position.set(axis === 'x' ? -TRAVEL : prev.x, y, axis === 'z' ? -TRAVEL : prev.z);
  tower.add(mesh);
  moving = { mesh, axis, w: prev.w, d: prev.d, t: Math.random() * 0.4 };
}

function placeFloor(px, pz, w, d) {
  const tier = starRating(floors + 1);
  if (moving) tower.remove(moving.mesh);
  const mesh = makeFloorMesh(w, d, tier, floors + 1);
  mesh.position.set(px, topY() + FLOOR_H / 2, pz);
  tower.add(mesh);
  stack.push({ mesh, x: px, z: pz, w, d });
  floors++;
  speed = Math.min(11, 5.2 + floors * 0.12);

  if (starRating(floors) > starRating(floors - 1) && pendingDebug === 0) {
    for (let i = 0; i < 4; i++) setTimeout(() => spawnFirework(topY()), i * 320);
    sound.fanfare();
  }
  if (floors === PROMO_FLOORS && !promoShownThisRun && pendingDebug === 0) {
    promoShownThisRun = true;
    showPromo();
  }
  updateHud();
}

function drop() {
  if (!moving || bonus > 0) return;
  const prev = topBlock();
  const m = moving;
  const pos = m.mesh.position;
  const delta = m.axis === 'x' ? pos.x - prev.x : pos.z - prev.z;

  if (Math.abs(delta) <= PERFECT_EPS) {
    // perfect
    streak++;
    sound.ding(streak);
    pulseRing(prev.x, topY() + FLOOR_H / 2, prev.z, Math.max(m.w, m.d));
    placeFloor(prev.x, prev.z, Math.min(BASE, m.w + 0.25), Math.min(BASE, m.d + 0.25));
    if (streak >= 5) {
      streak = 0;
      bonus = 5; bonusTimer = 0.35;
      $('perfect').classList.remove('show'); void $('perfect').offsetWidth;
      $('perfect').classList.add('show');
      sound.fanfare();
    }
  } else {
    streak = 0;
    const size = m.axis === 'x' ? m.w : m.d;
    const overlap = size - Math.abs(delta);
    if (overlap <= 0.4) {
      // total miss
      spawnDebris(m.w, m.d, pos.x, pos.z, pos.y, starRating(floors + 1));
      tower.remove(m.mesh); moving = null;
      gameOver();
      return;
    }
    sound.thunk();
    shake = 0.5;
    // new block occupies the overlap; the rest breaks off
    const sign = Math.sign(delta);
    if (m.axis === 'x') {
      const newX = prev.x + delta / 2;
      const cutW = Math.abs(delta);
      spawnDebris(cutW, m.d, newX + sign * (overlap / 2 + cutW / 2), pos.z, pos.y, starRating(floors + 1));
      placeFloor(newX, prev.z, overlap, m.d);
    } else {
      const newZ = prev.z + delta / 2;
      const cutD = Math.abs(delta);
      spawnDebris(m.w, cutD, pos.x, newZ + sign * (overlap / 2 + cutD / 2), pos.y, starRating(floors + 1));
      placeFloor(prev.x, newZ, m.w, overlap);
    }
  }
  if (state === 'playing') spawnMoving();
}

// perfect-drop shockwave ring
const pulses = [];
function pulseRing(x, y, z, size) {
  const ring = new THREE.Mesh(
    new THREE.TorusGeometry(size * 0.7, 0.09, 8, 48),
    new THREE.MeshBasicMaterial({ color: COLORS.yellow, transparent: true, opacity: 0.95 })
  );
  ring.rotation.x = Math.PI / 2;
  ring.position.set(x, y - FLOOR_H / 2 + 0.05, z);
  scene.add(ring);
  pulses.push({ ring, life: 1 });
}

function gameOver() {
  state = 'over';
  overOrbit = 0;
  sound.over();
  best = Math.max(best, floors);
  localStorage.setItem('hu_tower_best', best);
  setTimeout(() => {
    const fl = n => `${n} floor${n === 1 ? '' : 's'}`;
    $('overFloors').textContent = fl(floors);
    $('overStars').textContent = '★'.repeat(starRating(floors)) || '—';
    $('overBest').textContent = `Personal best: ${fl(best)}`;
    if (floors >= PROMO_FLOORS) {
      $('overCodeWrap').style.display = '';
      $('overCode').querySelector('span').textContent = promoCode(floors);
    } else {
      $('overCodeWrap').style.display = 'none';
    }
    $('overVeil').classList.add('show');
    $('hint').classList.remove('show');
  }, 1400);
}

function showPromo() {
  state = 'promo';
  $('promoStars').textContent = '★'.repeat(starRating(floors));
  $('promoCode').querySelector('span').textContent = promoCode(PROMO_FLOORS);
  $('promoVeil').classList.add('show');
  for (let i = 0; i < 5; i++) setTimeout(() => spawnFirework(topY()), i * 260);
}

function updateHud() {
  floorsEl.textContent = floors;
  starsEl.textContent = '★'.repeat(starRating(floors));
  pips.forEach((p, i) => p.classList.toggle('on', i < streak));
  hud.classList.toggle('dark', skyAt(topY()).night < 0.15);
}

// ---------- input ----------
function primaryAction() {
  sound.unlock();
  if (state === 'playing') drop();
}
addEventListener('pointerdown', e => {
  if (e.target.closest('.card')) return;
  primaryAction();
});
addEventListener('keydown', e => {
  if (e.repeat) return;
  if (e.code === 'Space' || e.code === 'Enter') {
    if (state === 'menu') startGame();
    else if (state === 'promo') dismissPromo();
    else primaryAction();
  }
});

function startGame() {
  $('menuVeil').classList.remove('show');
  $('overVeil').classList.remove('show');
  $('hint').classList.add('show');
  setTimeout(() => $('hint').classList.remove('show'), 3500);
  resetGame();
  state = 'playing';
  // QA shortcut: ?debug=N pre-builds N aligned floors (a few per frame)
  const dbg = +(new URLSearchParams(location.search).get('debug') || 0);
  if (dbg > 0) {
    tower.remove(moving.mesh); moving = null;
    pendingDebug = Math.min(dbg, 380);
  }
}
function dismissPromo() {
  $('promoVeil').classList.remove('show');
  state = 'playing';
  if (!moving) spawnMoving();
}
$('startBtn').addEventListener('click', () => { sound.unlock(); startGame(); });
$('againBtn').addEventListener('click', () => { sound.unlock(); startGame(); });
$('continueBtn').addEventListener('click', dismissPromo);
for (const id of ['promoCode', 'overCode']) {
  $(id).addEventListener('click', async () => {
    const code = $(id).querySelector('span').textContent;
    try { await navigator.clipboard.writeText(code); $(id).querySelector('small').textContent = 'COPIED!'; } catch {}
    setTimeout(() => { $(id).querySelector('small').textContent = 'COPY'; }, 1400);
  });
}
if (best > 0) $('bestLine').innerHTML = `Personal best: <b>${best} floor${best === 1 ? '' : 's'}</b>`;

// ---------- main loop ----------
const clock = new THREE.Clock();

function animate() {
  requestAnimationFrame(animate);
  const rawDt = clock.getDelta();
  const dt = Math.min(rawDt, 0.05);
  const time = clock.elapsedTime;

  // ?debug builder: time-based so background-tab rAF throttling can't stall it
  if (pendingDebug > 0 && state === 'playing') {
    const n = Math.min(pendingDebug, Math.max(3, Math.round(rawDt * 120)));
    for (let k = 0; k < n; k++) {
      pendingDebug--;
      placeFloor(0, 0, BASE, BASE);
    }
    if (pendingDebug === 0) spawnMoving();
  }

  // slider
  if (moving && state === 'playing' && bonus === 0) {
    moving.t += dt * speed / (TRAVEL * 2);
    const ph = moving.t % 1;
    const tri = ph < 0.5 ? ph * 2 : 2 - ph * 2;      // 0..1..0
    const p = -TRAVEL + tri * TRAVEL * 2;
    if (moving.axis === 'x') moving.mesh.position.x = p;
    else moving.mesh.position.z = p;
  }

  // streak bonus: auto-build
  if (bonus > 0 && state === 'playing') {
    bonusTimer -= dt;
    if (bonusTimer <= 0) {
      bonusTimer = 0.16;
      bonus--;
      const prev = topBlock();
      tower.remove(moving.mesh); moving = null;
      placeFloor(prev.x, prev.z, prev.w, prev.d);
      pulseRing(prev.x, topY() + FLOOR_H / 2, prev.z, Math.max(prev.w, prev.d));
      if (state === 'playing') spawnMoving();
    }
  }

  // debris physics
  for (let i = debris.length - 1; i >= 0; i--) {
    const d = debris[i];
    d.vel.y -= 22 * dt;
    d.mesh.position.addScaledVector(d.vel, dt);
    d.mesh.rotation.x += d.rot.x * dt;
    d.mesh.rotation.y += d.rot.y * dt;
    d.mesh.rotation.z += d.rot.z * dt;
    if (d.mesh.position.y < -25) { scene.remove(d.mesh); debris.splice(i, 1); }
  }

  // fireworks
  for (let i = fireworks.length - 1; i >= 0; i--) {
    const fw = fireworks[i];
    fw.life -= dt * 0.55;
    const pos = fw.pts.geometry.attributes.position;
    for (let j = 0; j < fw.vel.length; j++) {
      fw.vel[j].y -= 5.5 * dt;
      pos.array[j * 3] += fw.vel[j].x * dt;
      pos.array[j * 3 + 1] += fw.vel[j].y * dt;
      pos.array[j * 3 + 2] += fw.vel[j].z * dt;
    }
    pos.needsUpdate = true;
    fw.pts.material.opacity = Math.max(0, fw.life);
    if (fw.life <= 0) { scene.remove(fw.pts); fireworks.splice(i, 1); }
  }

  // pulse rings
  for (let i = pulses.length - 1; i >= 0; i--) {
    const p = pulses[i];
    p.life -= dt * 1.6;
    p.ring.scale.setScalar(1 + (1 - p.life) * 2.2);
    p.ring.material.opacity = Math.max(0, p.life);
    if (p.life <= 0) { scene.remove(p.ring); pulses.splice(i, 1); }
  }

  // sky
  const h = topY();
  const sky = skyAt(h);
  bgColor.copy(sky.top);
  scene.fog.color.copy(sky.fog);
  hemi.intensity = sky.hemi;
  sun.intensity = sky.sun;
  const starO = THREE.MathUtils.clamp((sky.night - 0.35) / 0.5, 0, 1);
  starsNear.material.opacity = starO;
  starsFar.material.opacity = starO * 0.8;
  moon.material.opacity = THREE.MathUtils.clamp((sky.night - 0.3) / 0.4, 0, 1);
  planetBody.material.opacity = planetRing.material.opacity =
    THREE.MathUtils.clamp((sky.night - 0.6) / 0.35, 0, 1);
  bloom.strength = 0.32 + sky.night * 0.28;

  // window glow ramps with night
  const glow = 0.12 + sky.night * 0.6;
  for (const mt of matCache.values()) mt.emissiveIntensity = glow;

  // camera
  if (state === 'over') {
    overOrbit += dt;
    orbit += dt * 0.5;
    const r = 20 + Math.min(30, h * 0.55);
    const midY = h * 0.5;
    camPos.lerp(new THREE.Vector3(Math.cos(orbit) * r, midY + r * 0.35, Math.sin(orbit) * r), 0.02);
    camTgt.lerp(new THREE.Vector3(0, midY, 0), 0.03);
  } else {
    orbit += dt * orbitAuto;
    const r = 26;
    const y = h + 10.5;
    camPos.lerp(new THREE.Vector3(Math.cos(orbit) * r, y, Math.sin(orbit) * r), 0.045);
    camTgt.lerp(new THREE.Vector3(0, Math.max(2, h - 2.4), 0), 0.06);
  }
  camera.position.copy(camPos);
  if (shake > 0) {
    shake -= dt * 2.4;
    camera.position.x += (Math.random() - 0.5) * shake * 0.5;
    camera.position.y += (Math.random() - 0.5) * shake * 0.5;
  }
  camera.lookAt(camTgt);

  // light follows the action
  sun.position.set(camTgt.x + 18, h + 26, camTgt.z + 10);
  sun.target.position.set(0, Math.max(0, h - 4), 0);
  ring.material.color.setHex(COLORS.yellow);
  ring.material.transparent = true;
  ring.material.opacity = 0.4 + 0.6 * Math.abs(Math.sin(time * 1.8));

  composer.render();
}
animate();
updateHud();
