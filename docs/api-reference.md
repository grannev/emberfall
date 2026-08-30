# Справочник публичного API

Заголовки в `src/` являются источником истины. Этот документ объясняет
назначение публичных функций и правила их вызова.

## World API

### Lifecycle

```c
bool WorldInit(World *world, int width, int height);
void WorldUnload(World *world);
void WorldGenerate(World *world);
```

- `WorldInit` выделяет cells, pixels, два chunk buffer и создаёт texture. Требует
  уже открытого raylib window/OpenGL context.
- `WorldGenerate` заполняет уже инициализированный мир; повторный вызов не
  выделяет память.
- `WorldUnload` выгружает texture и освобождает все world allocations.

### Simulation и rendering

```c
void WorldUpdate(World *world);
void WorldDraw(World *world);
```

- `WorldUpdate` выполняет ровно один fixed tick.
- `WorldDraw` обновляет полный Color buffer и texture, затем рисует её в world
  origin. Вызывать внутри `BeginMode2D`.

### Cell access

```c
CellMaterial WorldGetCell(const World *world, int x, int y);
float WorldGetTemperature(const World *world, int x, int y);
bool WorldMaterialIsSolid(CellMaterial material);
void WorldSetCell(World *world, int x, int y, CellMaterial material);
```

`WorldSetCell` сбрасывает temperature/lifetime к начальному состоянию материала
и пробуждает chunks. Для изменения только температуры внутри world module нужно
также соблюдать wake-up invariant.

### Effects

```c
void WorldDestroyCircle(World *world, int centerX, int centerY, int radius,
                        float rockToLavaChance);
void WorldApplyShockwave(World *world, int centerX, int centerY,
                         int innerRadius, int outerRadius);
LaserResult WorldApplyLaser(World *world, Vector2 start, Vector2 end,
                            float radius, float deltaTime);
```

`WorldApplyLaser` возвращает:

- `position` — конец луча или точку контакта;
- `material` — материал первого контакта;
- `hit` — был ли найден solid target.

### Coordinates и labels

```c
Vector2 WorldScreenToCell(const World *world, Vector2 screenPosition,
                          Camera2D camera);
const char *WorldMaterialName(CellMaterial material);
```

## Player API

```c
void PlayerInit(Player *player, Vector2 position);
void PlayerUpdate(Player *player, const World *world, float deltaTime);
void PlayerResolveWorldCollision(Player *player, const World *world);
void PlayerApplyExplosionImpulse(Player *player, Vector2 center,
                                 float radius, float force);
void PlayerDraw(const Player *player, Vector2 aimPosition);
```

Рекомендуемый порядок: `PlayerUpdate` до world ticks и
`PlayerResolveWorldCollision` после них. `PlayerApplyExplosionImpulse` добавляет
velocity с линейным ослаблением к краю shockwave, но не наносит урон.

## Powers API

```c
void PowersInit(PowerSystem *powers);
void PowersUpdate(PowerSystem *powers, World *world,
                  ParticleSystem *particles, Vector2 origin,
                  Vector2 aimPosition, float deltaTime,
                  bool laserHeld, bool explosionPressed);
void PowersDrawWorld(const PowerSystem *powers, Vector2 aimPosition);
const char *PowersCurrentName(const PowerSystem *powers);
```

После `PowersUpdate` вызывающий код должен проверить `explosionTriggered` и
применить player/camera/audio feedback. `PowersDrawWorld` вызывается внутри
world-space camera mode.

## Particle API

```c
void ParticlesInit(ParticleSystem *system);
void ParticlesUpdate(ParticleSystem *system, float deltaTime);
void ParticlesDraw(const ParticleSystem *system);
void ParticlesSpawnExplosion(ParticleSystem *system, Vector2 position);
void ParticlesSpawnLaserSparks(ParticleSystem *system, Vector2 position,
                               Vector2 direction);
void ParticlesSpawnSteam(ParticleSystem *system, Vector2 position);
```

`ParticlesInit` полностью очищает pool и используется также при полном reset.
Все spawn-функции работают только с фиксированным массивом.

## Audio API

```c
bool GameAudioInit(GameAudio *audio);
void GameAudioUpdate(GameAudio *audio, bool laserActive, float deltaTime);
void GameAudioPlayExplosion(GameAudio *audio);
void GameAudioPlayReaction(GameAudio *audio);
void GameAudioUnload(GameAudio *audio);
```

`GameAudioInit` вызывается после `InitWindow`, а `GameAudioUnload` — до
`CloseWindow`. Возвращаемое false означает silent mode, а не ошибку всего
приложения.
