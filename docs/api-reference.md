# Справочник публичного API

Заголовки в `src/` являются источником истины. Этот документ объясняет
назначение публичных функций и правила их вызова.

## World API

### Lifecycle

```c
bool WorldInit(World *world, int width, int height);
bool WorldInitRenderer(World *world);
void WorldUnload(World *world);
void WorldGenerate(World *world);
Vector2 WorldPlayerSpawn(const World *world);
```

- `WorldInit` выделяет cells, pixels, два chunk buffer и буфер грязных chunks и
  выставляет всем cells ambient temperature. Он не трогает GPU, поэтому вызывается
  и без окна.
- `WorldInitRenderer` создаёт Texture2D. Это единственная функция мира, требующая
  открытого raylib window/OpenGL context; headless-тесты её не вызывают, и это
  разделение нужно сохранить.
- `WorldGenerate` заполняет уже инициализированный мир; повторный вызов не
  выделяет память.
- `WorldPlayerSpawn` возвращает свободную точку над сгенерированной поверхностью,
  чтобы вызывающий код не знал о форме рельефа.
- `WorldUnload` выгружает texture и освобождает все world allocations.

### Simulation и rendering

```c
void WorldUpdate(World *world);
void WorldDraw(World *world, Rectangle visible);
void WorldSetPointLight(World *world, Vector2 position, float radius,
                        float strength);
float WorldLightAt(const World *world, int x, int y);
```

- `WorldUpdate` выполняет ровно один fixed tick.
- `WorldDraw` решает свет, пересчитывает Color buffer для грязных chunks внутри
  `visible`, загружает одним `UpdateTextureRec` полосу строк во всю ширину и
  рисует texture в world origin. Вызывать внутри `BeginMode2D`. `visible`
  задаётся в cells; грязный chunk вне его сохраняет флаг и перестраивается,
  когда попадёт в кадр. Результат обязан быть попиксельно идентичен полному
  перестроению того же региона.
- `WorldSetPointLight` задаёт единственный перемещаемый источник света, которым
  владеет вызывающий код; strength 0 выключает его. Применяется на следующем
  `WorldDraw`.
- `WorldLightAt` возвращает суммарную освещённость клетки, 0..1.

### Cell access

```c
CellMaterial WorldGetCell(const World *world, int x, int y);
float WorldGetTemperature(const World *world, int x, int y);
bool WorldMaterialIsSolid(CellMaterial material);
void WorldSetCell(World *world, int x, int y, CellMaterial material);
void WorldSetTemperature(World *world, int x, int y, float temperature);
int WorldCountDynamicCells(const World *world);
```

`WorldCountDynamicCells` считает динамические cells по запросу и линейно проходит
весь мир, поэтому вызывается для HUD и тестов, а не каждый tick.

`WorldSetCell` сбрасывает temperature/lifetime к начальному состоянию материала
и пробуждает chunks. Для изменения только температуры внутри world module нужно
также соблюдать wake-up invariant.

### Effects

```c
void WorldDestroyCircle(World *world, int centerX, int centerY, int radius,
                        float rockToLavaChance);
int WorldDrillCircle(World *world, int centerX, int centerY, int radius);
void WorldApplyShockwave(World *world, int centerX, int centerY,
                         int innerRadius, int outerRadius);
LaserResult WorldApplyLaser(World *world, Vector2 start, Vector2 end,
                            float radius, float deltaTime);
LaserResult WorldApplyChill(World *world, Vector2 start, Vector2 end,
                            float radius, float deltaTime);
void WorldApplyForceCone(World *world, Vector2 origin, Vector2 direction,
                         float length, float spreadCosine, int reach);
```

`WorldApplyChill` — термическая инверсия лазера: замораживает воду в `ICE` и
осаждает лаву обратно в rock.

`WorldApplyForceCone` толкает динамические cells вдоль конуса и не разрушает ни
одной. `spreadCosine` — косинус половинного угла, `reach` — насколько далеко
отбрасываются ближайшие cells.

`WorldApplyLaser` возвращает:

- `position` — конец луча или точку контакта;
- `material` — материал первого контакта;
- `hit` — был ли найден solid target.

`WorldDrillCircle` удаляет только solid cells (dirt, rock, sand) в границах
окружности и возвращает число реально удалённых cells. Небольшая доля из них
остаётся ash вместо empty. Жидкости и газы не удаляются, а cells на границе
тоннеля получают остаточный нагрев. Внешняя граница мира игнорируется и остаётся
неразрушимой.

### Coordinates и labels

```c
Vector2 WorldScreenToCell(const World *world, Vector2 screenPosition,
                          Camera2D camera);
const char *WorldMaterialName(CellMaterial material);
```

## Player API

```c
void PlayerInit(Player *player, Vector2 position);
void PlayerUpdate(Player *player, World *world, Vector2 input,
                  bool boostHeld, float deltaTime);
void PlayerResolveWorldCollision(Player *player, World *world);
void PlayerApplyExplosionImpulse(Player *player, Vector2 center,
                                 float radius, float force);
void PlayerDraw(const Player *player, Vector2 aimPosition);
```

Рекомендуемый порядок: `PlayerUpdate` до world ticks и
`PlayerResolveWorldCollision` после них. Последняя принимает изменяемый мир,
потому что бурящий игрок прорезает материал, в который его затянуло, вместо
перемещения с потерей скорости. `PlayerApplyExplosionImpulse` добавляет
velocity с линейным ослаблением к краю shockwave, но не наносит урон.

## Powers API

```c
void PowersInit(PowerSystem *powers);
void PowersUpdate(PowerSystem *powers, World *world,
                  ParticleSystem *particles, Vector2 origin,
                  Vector2 aimPosition, float deltaTime,
                  bool laserHeld, bool explosionPressed,
                  bool forceHeld, bool chillHeld);
void PowersDrawWorld(const PowerSystem *powers, Vector2 aimPosition);
const char *PowersCurrentName(const PowerSystem *powers);
```

После `PowersUpdate` вызывающий код должен проверить `explosionTriggered` и
применить player/camera/audio feedback. `PowersDrawWorld` вызывается внутри
world-space camera mode.

## Particle API

```c
void ParticlesInit(ParticleSystem *system);
void ParticlesUpdate(ParticleSystem *system, World *world, float deltaTime);
void ParticlesDraw(const ParticleSystem *system);
void ParticlesSpawnExplosion(ParticleSystem *system, Vector2 position);
void ParticlesSpawnLaserSparks(ParticleSystem *system, Vector2 position,
                               Vector2 direction);
void ParticlesSpawnImpact(ParticleSystem *system, Vector2 position,
                          Vector2 normal, float strength);
void ParticlesSpawnBoostTrail(ParticleSystem *system, Vector2 position,
                              Vector2 velocity);
void ParticlesSpawnDrillDebris(ParticleSystem *system, Vector2 position,
                               Vector2 velocity, int destroyedCells);
void ParticlesSpawnSteam(ParticleSystem *system, Vector2 position);
```

`ParticlesInit` полностью очищает pool и используется также при полном reset.
Все spawn-функции работают только с фиксированным массивом.

## Audio API

```c
bool GameAudioInit(GameAudio *audio);
void GameAudioUpdate(GameAudio *audio, bool laserActive, bool drilling,
                     float deltaTime);
void GameAudioPlayExplosion(GameAudio *audio);
void GameAudioPlayReaction(GameAudio *audio);
void GameAudioUnload(GameAudio *audio);
```

`GameAudioInit` вызывается после `InitWindow`, а `GameAudioUnload` — до
`CloseWindow`. Возвращаемое false означает silent mode, а не ошибку всего
приложения.
