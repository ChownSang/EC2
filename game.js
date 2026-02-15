// ============================================================
// Super Pixel Adventure – HTML5 Canvas Platformer
// ============================================================

const canvas = document.getElementById('gameCanvas');
const ctx = canvas.getContext('2d');
const W = canvas.width;
const H = canvas.height;

// ---- Constants ----
const GRAVITY = 0.6;
const FRICTION = 0.8;
const TILE = 40;

// ---- Input ----
const keys = {};
window.addEventListener('keydown', e => { keys[e.code] = true; e.preventDefault(); });
window.addEventListener('keyup', e => { keys[e.code] = false; });

// ---- Utility ----
function rect(x, y, w, h, color) {
    ctx.fillStyle = color;
    ctx.fillRect(x, y, w, h);
}

function overlap(a, b) {
    return a.x < b.x + b.w && a.x + a.w > b.x &&
           a.y < b.y + b.h && a.y + a.h > b.y;
}

// ---- Level definitions ----
// Legend: 1=ground, 2=brick, 3=coin, 4=enemy spawn, 5=flag(goal), 6=spike
const LEVELS = [
    // Stage 1 – 튜토리얼: 기본 점프와 코인 수집
    [
        '                                        ',
        '                                        ',
        '                                        ',
        '                                        ',
        '                                  5     ',
        '       3 3 3       3 3          11111    ',
        '      222222      22222    11           ',
        '  P        4         4    11            ',
        ' 1111 1111 1111 1111 1111111            ',
        '11111 1111 1111 1111 1111111            ',
        '111111111111111 11111111111111111       ',
        '111111111111111 11111111111111111       ',
    ],
    // Stage 2 – 가시와 적이 추가
    [
        '                                        ',
        '                                        ',
        '                                        ',
        '                                 5      ',
        '   3 3        3 3             1111      ',
        '  22222      22222       11              ',
        '          4         4   11              ',
        '  P  4  11111  1111 111111              ',
        ' 111 111    6  6   6                    ',
        '111111111111111  1111111111111111       ',
        '111111111111111  1111111111111111       ',
        '111111111111111  1111111111111111       ',
    ],
    // Stage 3 – 최종: 정밀 점프 필요
    [
        '                                        ',
        '                                        ',
        '                                5       ',
        '                              1111      ',
        '     3       3           11             ',
        '    222     222    4    11              ',
        '         4       111  11               ',
        '  P     111  6       11                ',
        ' 111       1666  111111                ',
        '11111  111111111111111111111            ',
        '11111  111111111111111111111            ',
        '11111  111111111111111111111            ',
    ],
];

// ---- Parse level into game objects ----
function parseLevel(levelIndex) {
    const map = LEVELS[levelIndex];
    const platforms = [];
    const coins = [];
    const enemies = [];
    const spikes = [];
    let flag = null;
    let playerStart = { x: 60, y: 200 };

    for (let row = 0; row < map.length; row++) {
        for (let col = 0; col < map[row].length; col++) {
            const ch = map[row][col];
            const x = col * TILE;
            const y = row * TILE;
            if (ch === '1' || ch === '2') {
                platforms.push({ x, y, w: TILE, h: TILE, type: ch === '1' ? 'ground' : 'brick' });
            } else if (ch === '3') {
                coins.push({ x: x + 10, y: y + 10, w: 20, h: 20, collected: false });
            } else if (ch === '4') {
                enemies.push({ x, y: y - TILE, w: 36, h: TILE, vx: 1, alive: true, startX: x - 60, endX: x + 60 });
            } else if (ch === '5') {
                flag = { x: x + 10, y: y - 60, w: 20, h: 60 };
            } else if (ch === '6') {
                spikes.push({ x, y: y + 20, w: TILE, h: 20 });
            } else if (ch === 'P') {
                playerStart = { x, y: y - TILE };
            }
        }
    }
    return { platforms, coins, enemies, spikes, flag, playerStart };
}

// ---- Player ----
function createPlayer(x, y) {
    return {
        x, y, w: 32, h: 38,
        vx: 0, vy: 0,
        onGround: false,
        lives: 3,
        score: 0,
        facing: 1, // 1 = right, -1 = left
        frame: 0,
        frameTimer: 0,
        invincible: 0,
    };
}

// ---- Camera ----
const camera = { x: 0, y: 0 };

// ---- Game State ----
let currentLevel = 0;
let player;
let level;
let gameState = 'menu'; // menu, playing, gameover, clear

function initLevel() {
    level = parseLevel(currentLevel);
    player = createPlayer(level.playerStart.x, level.playerStart.y);
    camera.x = 0;
    camera.y = 0;
}

// ---- Physics & Update ----
function updatePlayer() {
    // Horizontal movement
    if (keys['ArrowLeft'] || keys['KeyA']) {
        player.vx -= 0.8;
        player.facing = -1;
    }
    if (keys['ArrowRight'] || keys['KeyD']) {
        player.vx += 0.8;
        player.facing = 1;
    }

    // Jump
    if ((keys['Space'] || keys['ArrowUp'] || keys['KeyW']) && player.onGround) {
        player.vy = -12;
        player.onGround = false;
    }

    // Apply physics
    player.vx *= FRICTION;
    player.vy += GRAVITY;

    // Clamp velocity
    if (Math.abs(player.vx) < 0.1) player.vx = 0;
    if (player.vy > 15) player.vy = 15;

    // Move X
    player.x += player.vx;
    for (const p of level.platforms) {
        if (overlap(player, p)) {
            if (player.vx > 0) player.x = p.x - player.w;
            else if (player.vx < 0) player.x = p.x + p.w;
            player.vx = 0;
        }
    }

    // Move Y
    player.y += player.vy;
    player.onGround = false;
    for (const p of level.platforms) {
        if (overlap(player, p)) {
            if (player.vy > 0) {
                player.y = p.y - player.h;
                player.onGround = true;
            } else if (player.vy < 0) {
                player.y = p.y + p.h;
            }
            player.vy = 0;
        }
    }

    // Fall off screen
    if (player.y > H + 100) {
        hurtPlayer(true);
    }

    // Animation frame
    player.frameTimer++;
    if (player.frameTimer > 6) {
        player.frame = (player.frame + 1) % 4;
        player.frameTimer = 0;
    }

    if (player.invincible > 0) player.invincible--;
}

function hurtPlayer(fall) {
    if (player.invincible > 0 && !fall) return;
    player.lives--;
    if (player.lives <= 0) {
        gameState = 'gameover';
        document.getElementById('final-score').textContent = `점수: ${player.score}`;
        document.getElementById('game-over-screen').classList.remove('hidden');
        return;
    }
    // Respawn
    player.x = level.playerStart.x;
    player.y = level.playerStart.y;
    player.vx = 0;
    player.vy = 0;
    player.invincible = 90; // 1.5 seconds
}

function updateEnemies() {
    for (const e of level.enemies) {
        if (!e.alive) continue;
        e.x += e.vx;
        if (e.x <= e.startX || e.x >= e.endX) e.vx *= -1;

        if (overlap(player, e)) {
            // Stomp from above
            if (player.vy > 0 && player.y + player.h - e.y < 20) {
                e.alive = false;
                player.vy = -8;
                player.score += 100;
            } else {
                hurtPlayer(false);
            }
        }
    }
}

function updateCoins() {
    for (const c of level.coins) {
        if (c.collected) continue;
        if (overlap(player, c)) {
            c.collected = true;
            player.score += 50;
        }
    }
}

function checkSpikes() {
    for (const s of level.spikes) {
        if (overlap(player, s)) {
            hurtPlayer(false);
        }
    }
}

function checkGoal() {
    if (level.flag && overlap(player, level.flag)) {
        player.score += 500;
        if (currentLevel < LEVELS.length - 1) {
            gameState = 'clear';
            document.getElementById('clear-score').textContent = `점수: ${player.score}`;
            document.getElementById('clear-screen').classList.remove('hidden');
        } else {
            gameState = 'clear';
            document.getElementById('clear-screen').querySelector('h1').textContent = 'All Clear!';
            document.getElementById('clear-score').textContent = `최종 점수: ${player.score}`;
            document.getElementById('nextBtn').textContent = '처음부터';
            document.getElementById('clear-screen').classList.remove('hidden');
        }
    }
}

function updateCamera() {
    const targetX = player.x - W / 3;
    camera.x += (targetX - camera.x) * 0.1;
    if (camera.x < 0) camera.x = 0;
}

function update() {
    if (gameState !== 'playing') return;
    updatePlayer();
    updateEnemies();
    updateCoins();
    checkSpikes();
    checkGoal();
    updateCamera();
}

// ---- Rendering ----
function drawBackground() {
    // Sky gradient
    const grad = ctx.createLinearGradient(0, 0, 0, H);
    grad.addColorStop(0, '#0f3460');
    grad.addColorStop(1, '#16213e');
    ctx.fillStyle = grad;
    ctx.fillRect(0, 0, W, H);

    // Parallax mountains
    ctx.fillStyle = '#1a1a40';
    for (let i = 0; i < 5; i++) {
        const mx = i * 200 - (camera.x * 0.2) % 200;
        ctx.beginPath();
        ctx.moveTo(mx, H);
        ctx.lineTo(mx + 100, H - 160);
        ctx.lineTo(mx + 200, H);
        ctx.fill();
    }

    // Stars
    ctx.fillStyle = '#ffffff44';
    for (let i = 0; i < 40; i++) {
        const sx = (i * 73 + 10) % W;
        const sy = (i * 47 + 5) % (H * 0.5);
        ctx.fillRect(sx, sy, 2, 2);
    }
}

function drawPlatforms() {
    for (const p of level.platforms) {
        const x = p.x - camera.x;
        const y = p.y;
        if (p.type === 'ground') {
            rect(x, y, TILE, TILE, '#2d6a4f');
            // Grass top
            if (!level.platforms.some(pp => pp.x === p.x && pp.y === p.y - TILE)) {
                rect(x, y, TILE, 6, '#52b788');
            }
            // Dirt texture
            ctx.fillStyle = '#1b4332';
            ctx.fillRect(x + 4, y + 12, 6, 4);
            ctx.fillRect(x + 20, y + 24, 8, 4);
        } else {
            rect(x, y, TILE, TILE, '#b07d56');
            ctx.strokeStyle = '#8b5e3c';
            ctx.strokeRect(x + 1, y + 1, TILE - 2, TILE - 2);
            // Brick lines
            ctx.fillStyle = '#8b5e3c';
            ctx.fillRect(x, y + TILE / 2, TILE, 2);
            ctx.fillRect(x + TILE / 2, y, 2, TILE);
        }
    }
}

function drawPlayer() {
    if (player.invincible > 0 && Math.floor(player.invincible / 4) % 2 === 0) return;

    const x = player.x - camera.x;
    const y = player.y;
    const f = player.facing;

    // Body
    rect(x + 6, y + 10, 20, 18, '#e94560');

    // Head
    rect(x + 8, y, 16, 14, '#ffd6a5');

    // Eyes
    ctx.fillStyle = '#fff';
    ctx.fillRect(x + (f === 1 ? 16 : 10), y + 4, 6, 5);
    ctx.fillStyle = '#222';
    ctx.fillRect(x + (f === 1 ? 19 : 11), y + 5, 3, 3);

    // Hat
    rect(x + 4, y - 4, 24, 6, '#e94560');
    rect(x + 8, y - 8, 16, 6, '#e94560');

    // Legs – animated
    const legOffset = player.onGround && Math.abs(player.vx) > 0.5
        ? Math.sin(player.frame * Math.PI / 2) * 4 : 0;
    rect(x + 8, y + 28, 6, 10, '#0f3460');
    rect(x + 18, y + 28, 6, 10, '#0f3460');
    if (Math.abs(player.vx) > 0.5 && player.onGround) {
        ctx.fillStyle = '#0f3460';
        ctx.fillRect(x + 8 - legOffset, y + 28, 6, 10);
        ctx.fillRect(x + 18 + legOffset, y + 28, 6, 10);
    }
}

function drawEnemies() {
    for (const e of level.enemies) {
        if (!e.alive) continue;
        const x = e.x - camera.x;
        const y = e.y;

        // Body
        rect(x + 2, y + 10, 32, 22, '#9b59b6');
        // Head
        rect(x + 6, y + 2, 24, 14, '#9b59b6');
        // Eyes
        ctx.fillStyle = '#fff';
        ctx.fillRect(x + 8, y + 6, 8, 6);
        ctx.fillRect(x + 20, y + 6, 8, 6);
        ctx.fillStyle = '#e74c3c';
        ctx.fillRect(x + 12, y + 8, 4, 4);
        ctx.fillRect(x + 22, y + 8, 4, 4);
        // Feet
        rect(x + 4, y + 32, 10, 8, '#7d3c98');
        rect(x + 22, y + 32, 10, 8, '#7d3c98');
    }
}

function drawCoins() {
    for (const c of level.coins) {
        if (c.collected) continue;
        const x = c.x - camera.x;
        const y = c.y;

        // Coin glow
        ctx.fillStyle = 'rgba(255, 215, 0, 0.2)';
        ctx.beginPath();
        ctx.arc(x + 10, y + 10, 14, 0, Math.PI * 2);
        ctx.fill();

        // Coin body
        ctx.fillStyle = '#ffd700';
        ctx.beginPath();
        ctx.arc(x + 10, y + 10, 9, 0, Math.PI * 2);
        ctx.fill();

        // Shine
        ctx.fillStyle = '#fff8dc';
        ctx.beginPath();
        ctx.arc(x + 7, y + 7, 3, 0, Math.PI * 2);
        ctx.fill();
    }
}

function drawSpikes() {
    for (const s of level.spikes) {
        const x = s.x - camera.x;
        const y = s.y;
        ctx.fillStyle = '#c0c0c0';
        for (let i = 0; i < 4; i++) {
            ctx.beginPath();
            ctx.moveTo(x + i * 10, y + s.h);
            ctx.lineTo(x + i * 10 + 5, y);
            ctx.lineTo(x + i * 10 + 10, y + s.h);
            ctx.fill();
        }
    }
}

function drawFlag() {
    if (!level.flag) return;
    const x = level.flag.x - camera.x;
    const y = level.flag.y;

    // Pole
    rect(x + 8, y, 4, level.flag.h + 10, '#ccc');

    // Flag
    ctx.fillStyle = '#e94560';
    ctx.beginPath();
    ctx.moveTo(x + 12, y);
    ctx.lineTo(x + 35, y + 12);
    ctx.lineTo(x + 12, y + 24);
    ctx.fill();

    // Star on flag
    ctx.fillStyle = '#ffd700';
    ctx.font = '14px sans-serif';
    ctx.fillText('★', x + 16, y + 17);
}

function drawHUD() {
    // Score
    ctx.fillStyle = '#fff';
    ctx.font = 'bold 18px monospace';
    ctx.fillText(`SCORE: ${player.score}`, 16, 30);

    // Lives
    ctx.fillText('LIVES: ', 16, 56);
    for (let i = 0; i < player.lives; i++) {
        ctx.fillStyle = '#e94560';
        ctx.fillRect(100 + i * 24, 44, 16, 16);
    }

    // Stage
    ctx.fillStyle = '#fff';
    ctx.fillText(`STAGE ${currentLevel + 1}`, W - 120, 30);
}

function draw() {
    ctx.clearRect(0, 0, W, H);
    drawBackground();
    drawPlatforms();
    drawCoins();
    drawSpikes();
    drawFlag();
    drawEnemies();
    drawPlayer();
    drawHUD();
}

// ---- Game Loop ----
function gameLoop() {
    update();
    draw();
    requestAnimationFrame(gameLoop);
}

// ---- UI ----
document.getElementById('startBtn').addEventListener('click', () => {
    document.getElementById('start-screen').classList.add('hidden');
    currentLevel = 0;
    initLevel();
    gameState = 'playing';
});

document.getElementById('restartBtn').addEventListener('click', () => {
    document.getElementById('game-over-screen').classList.add('hidden');
    currentLevel = 0;
    initLevel();
    gameState = 'playing';
});

document.getElementById('nextBtn').addEventListener('click', () => {
    document.getElementById('clear-screen').classList.add('hidden');
    if (currentLevel < LEVELS.length - 1) {
        const prevScore = player.score;
        currentLevel++;
        initLevel();
        player.score = prevScore;
    } else {
        currentLevel = 0;
        initLevel();
    }
    gameState = 'playing';
});

// Start render loop (menu is shown initially)
initLevel();
gameLoop();
