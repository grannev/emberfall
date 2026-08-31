# Справочник публичного API

Заголовки в `src/` являются источником истины. Этот документ объясняет
назначение публичных функций и правила их вызова.

## Game API

```c
GameConfig GameDefaultConfig(void);
bool GameInit(GameState *game, GameConfig config);
void GameReset(GameState *game, uint64_t seed);
void GameRegenerate(GameState *game);
void GameUpdate(GameState *game, const GameInput *input, float deltaTime,
                GameEventBuffer *events);
void GameUnload(GameState *game);
```

`GameState` — gameplay owner верхнего уровня. `GameUpdate` принимает уже
преобразованный input, ограничивает frame delta значением 0.05, оркестрирует
player/abilities/particles/fixed world ticks и возвращает transient feedback через
`GameEventBuffer`. Функция не опрашивает input, не рисует и не проигрывает звук.

`GameEventBuffer` имеет fixed capacity 256. `GameEventsPush` сохраняет порядок,
при переполнении увеличивает `dropped`; `GameEventsClear` начинает новый frame.
События не живут дольше одного вызова `GameUpdate`, поэтому consumer должен
прочитать их до следующего update.

`InputPoll` относится к app/platform boundary: он единственный преобразует
raylib keyboard/mouse и `Camera2D` в `GameInput` и `cursorCell`.

## World API

`world.h` — единственный публичный заголовок мира. Реализация разделена на
`materials.c`, `world_storage.c`, `world_simulation.c`, `world_thermal.c`,
`world_generation.c`, `world_lighting.c`, `world_effects.c` и
`world_render_data.c`. Заголовки `world_internal.h`, `world_thermal.h` и
`world_lighting.h` внутренние: их включают только файлы модуля мира.
`materials.h` описывает таблицу материалов и нужен всем, кто добавляет материал.

### Lifecycle

```c
bool WorldInit(World *world, int width, int height);
void WorldUnload(World *world);
void WorldGenerate(World *world, uint64_t seed);
Vector2 WorldPlayerSpawn(const World *world);
void WorldActivateRegion(World *world, Rectangle region);
```

- `WorldInit` выделяет cells, два active-chunk buffer, отдельные буферы
  pixel/light dirty chunks и поля света, затем выставляет всем cells ambient
  temperature. `World` не владеет GPU ресурсами и работает без окна.
- `WorldGenerate` детерминирована: один и тот же seed всегда даёт один и тот
  же мир. Он сохраняется в `World.seed`, а `World.rng` получает отдельный поток
  для последующих мутаций мира, чтобы взрывы и бурение не сдвигали рельеф,
  который производит seed.
- `WorldGenerate` заполняет уже инициализированный мир; повторный вызов не
  выделяет память и оставляет simulation chunks спящими.
- `WorldPlayerSpawn` возвращает свободную точку над сгенерированной поверхностью,
  чтобы вызывающий код не знал о форме рельефа.
- `WorldActivateRegion` сканирует только запрошенные chunks и будит находящиеся
  там динамические или нагретые cells. `GameUpdate` вызывает её при переходе игрока
  в новый chunk для окна 960×576. Обычные изменения мира всё равно используют
  собственный wake-up и не зависят от streaming.
- `WorldUnload` освобождает только CPU world allocations.

### Simulation и lighting state

```c
void WorldUpdate(World *world);
void WorldSetPointLight(World *world, Vector2 position, float radius,
                        float strength);
float WorldLightAt(const World *world, int x, int y);
```

- `WorldUpdate` выполняет ровно один fixed tick. После вызова
  `world.lastTickStats` содержит число реально пройденных scheduler-ом chunks и
  cells. Это структурные performance counters: они предназначены для HUD,
  benchmark и regression-проверок, но не влияют на gameplay.
- `WorldSetPointLight` задаёт единственный перемещаемый источник света, которым
  владеет вызывающий код; strength 0 выключает его. Применяется на следующем
  renderer preparation.
- `WorldLightAt` возвращает суммарную освещённость клетки, 0..1.

### Renderer API

```c
bool RendererInit(Renderer *renderer, const GameState *game);
void RendererDrawWorldSpace(Renderer *renderer, GameState *game,
                            Vector2 aimPosition, Rectangle visible);
const WorldRendererStats *RendererWorldStats(const Renderer *renderer);
void RendererUnload(Renderer *renderer);
```

`Renderer` владеет presentation lifecycle. Его `WorldRenderer` создаёт и
освобождает `Texture2D`, принимает CPU staging blocks 32×32 и выполняет
`UpdateTextureRec`. Persistent full-world `Color` buffer отсутствует.
`RendererDrawWorldSpace` вызывается внутри `BeginMode2D` и компонует мир,
частицы, игрока и способности. `WorldRendererStats` публикует dirty regions,
texture uploads, uploaded bytes и время подготовки последнего кадра.

Внутренний `WorldPrepareVisible` не является gameplay API. Он синхронно
передаёт renderer-у только видимые dirty chunks; невидимые сохраняют флаг до
попадания в кадр. Headless test проверяет первый полный build, нулевую работу
settled кадра и один region после локального изменения.

### Cell access

```c
CellMaterial WorldGetCell(const World *world, int x, int y);
float WorldGetTemperature(const World *world, int x, int y);
bool WorldMaterialIsSolid(CellMaterial material);
void WorldSetCell(World *world, int x, int y, CellMaterial material);
void WorldSetTemperature(World *world, int x, int y, float temperature);
int WorldCountDynamicCells(const World *world);
```

`WorldCountDynamicCells` считает динамические cells только внутри текущих active
chunks. Он вызывается для HUD и тестов, а не каждый tick; это число отражает
активно симулируемую область, а не все спящие жидкости огромной карты.

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
void WorldApplyForceBlast(World *world, Vector2 origin, Vector2 direction,
                          float length, float spreadCosine, int reach);
```

`WorldApplyChill` — термическая инверсия лазера: замораживает воду в `ICE` и
осаждает лаву обратно в rock.

`WorldApplyForceBlast` — один удар: отбрасывает динамические cells вдоль конуса и
скалывает тонкий слой с открытой грани твёрдых. `spreadCosine` — косинус
половинного угла, `reach` — насколько далеко отбрасываются ближайшие cells. Удар
загорожен рельефом и не достаёт из-за препятствия.

`WorldApplyLaser` возвращает:

- `position` — конец луча или точку контакта;
- `material` — материал первого контакта;
- `hit` — был ли найден solid target.

`WorldDrillCircle` удаляет только solid cells (dirt, rock, sand) в границах
окружности и возвращает число реально удалённых cells. Небольшая доля из них
остаётся ash вместо empty. Жидкости и газы не удаляются, а cells на границе
тоннеля получают остаточный нагрев. Внешняя граница мира игнорируется и остаётся
неразрушимой.

### Labels

```c
const char *WorldMaterialName(CellMaterial material);
```

## Player API

```c
typedef enum PlayerBoostStage {
    PLAYER_BOOST_NONE = 0,
    PLAYER_BOOST_STAGE_ONE,
    PLAYER_BOOST_STAGE_TWO,
    PLAYER_BOOST_STAGE_THREE
} PlayerBoostStage;
```

`boostStageChanged` живёт один frame; `GameUpdate` преобразует его в
`GAME_EVENT_BOOST_STAGE`. `boostBurstStage` сохраняется до окончания визуального
кольца.

```c
void PlayerInit(Player *player, Vector2 position);
void PlayerUpdate(Player *player, World *world, Vector2 input,
                  bool boostHeld, float deltaTime);
void PlayerResolveWorldCollision(Player *player, World *world);
void PlayerApplyImpulse(Player *player, Vector2 impulse);
void PlayerSetPose(Player *player, PlayerPose pose, float holdTime);
```

Рекомендуемый порядок: `PlayerUpdate` до world ticks и
`PlayerResolveWorldCollision` после них. Последняя принимает изменяемый мир,
потому что бурящий игрок прорезает материал, в который его затянуло, вместо
перемещения с потерей скорости.

Отдача способностей приходит к игроку через `GameEvent.playerImpulse`: сама
способность вычисляет толчок, а `game.c` применяет его, не зная, какая
способность его создала. Прежняя `PlayerApplyExplosionImpulse` удалена именно
поэтому — `Player` больше не перечисляет существующие powers.
`PlayerApplyImpulse` просто добавляет velocity и не наносит урон.
`PlayerRendererDraw` относится к presentation API и объявлен отдельно в
`player_renderer.h`.

## Abilities API

```c
const AbilityDefinition *AbilityDefinitionAt(AbilityId id);
const AbilityState *AbilityStateAt(const AbilitySystem *abilities, AbilityId id);
const char *AbilitiesCurrentName(const AbilitySystem *abilities);
bool AbilitiesValidate(void);

void AbilitiesInit(AbilitySystem *abilities, uint64_t seed);
void AbilitiesUpdate(AbilitySystem *abilities, World *world,
                     ParticleSystem *particles, GameEventBuffer *events,
                     Vector2 origin, Vector2 aim, float deltaTime,
                     const bool *requested);
```

`requested` — по одному флагу на способность в порядке `AbilityId`. Для
`HELD`-способностей это состояние кнопки, для `PRESSED` — фронт нажатия;
`input.c` выбирает нужное по `AbilityDefinition.trigger`, поэтому новая
способность ведёт себя правильно сразу после привязки.

`AbilitiesUpdate` владеет триггерами, cooldown-ами и таймерами эффекта.
Функция `apply` конкретной способности заполняет `AbilityState` для renderer и
публикует `GameEvent` для audio, камеры и отдачи. `AbilitiesValidate`
вызывается из `GameInit`.

Тюнинг, общий для симуляции и рисования (`ABILITY_FORCE_LENGTH`,
`ABILITY_FORCE_SPREAD_COSINE`, `ABILITY_EXPLOSION_SHOCK_RADIUS` и т. д.),
объявлен в `abilities.h` в единственном экземпляре.

## Particle API

```c
void ParticlesInit(ParticleSystem *system, uint64_t seed);
void ParticlesUpdate(ParticleSystem *system, World *world, float deltaTime);
void ParticlesSpawnExplosion(ParticleSystem *system, Vector2 position);
void ParticlesSpawnLaserSparks(ParticleSystem *system, Vector2 position,
                               Vector2 direction);
void ParticlesSpawnImpact(ParticleSystem *system, Vector2 position,
                          Vector2 normal, float strength);
void ParticlesSpawnBoostTrail(ParticleSystem *system, Vector2 position,
                              Vector2 velocity, int stage);
void ParticlesSpawnBoostBurst(ParticleSystem *system, Vector2 position,
                              Vector2 velocity, int stage);
void ParticlesSpawnDrillDebris(ParticleSystem *system, Vector2 position,
                               Vector2 velocity, int destroyedCells);
void ParticlesSpawnForceBlast(ParticleSystem *system, Vector2 origin,
                              Vector2 direction);
void ParticlesSpawnSteam(ParticleSystem *system, Vector2 position);
```

`stage` в boost-функциях принимает `PlayerBoostStage`: с каждой ступенью шлейф
становится плотнее и быстрее, а burst крупнее. `ParticlesInit` полностью очищает
pool и используется также при полном reset. Все spawn-функции работают только с
фиксированным массивом.
`ParticleRendererDraw` объявлен отдельно и не участвует в simulation API.

## Audio API

```c
bool GameAudioInit(GameAudio *audio);
void GameAudioUpdate(GameAudio *audio, GameAudioState state, float deltaTime);
void GameAudioPlayExplosion(GameAudio *audio);
void GameAudioPlayReaction(GameAudio *audio);
void GameAudioPlayImpact(GameAudio *audio, float strength);
void GameAudioPlayForce(GameAudio *audio);
void GameAudioPlayBoost(GameAudio *audio, int stage);
void GameAudioUnload(GameAudio *audio);
```

`GameAudioState` объединяет held-состояния лазера, бура и криолуча и материал,
который сейчас режет бур. `GameAudioPlayBoost` играет отдельный one-shot на
границе ступени; номер меняет высоту и громкость. `GameAudioInit` вызывается
после `InitWindow`, а `GameAudioUnload` — до `CloseWindow`. Возвращаемое false
означает silent mode, а не ошибку всего приложения.
