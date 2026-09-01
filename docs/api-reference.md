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
void RendererUpdatePresentation(Renderer *renderer,
                                const GameEventBuffer *events,
                                float deltaTime);
void RendererClearPresentation(Renderer *renderer);
void RendererRenderScene(Renderer *renderer, GameState *game, Camera2D camera,
                         Vector2 aimPosition, Rectangle visible);
void RendererComposite(const Renderer *renderer);
const WorldRendererStats *RendererWorldStats(const Renderer *renderer);
const RendererFrameStats *RendererStats(const Renderer *renderer);
void RendererUnload(Renderer *renderer);
```

`Renderer` владеет presentation lifecycle. Его `WorldRenderer` создаёт и
освобождает `Texture2D`, принимает CPU staging blocks 32×32 и выполняет
`UpdateTextureRec`. Persistent full-world `Color` buffer отсутствует.
`RendererRenderScene` компонует резкую scene, затем explicit emissive mask,
half-resolution downsample и separable horizontal/vertical blur. Scene не
проходит через blur. `RendererComposite` выводит scene в текущий backbuffer и
аддитивно накладывает blurred emissive, в каждом случае исправляя Y-ориентацию
raylib render texture. Ошибка shader/half-resolution target оставляет рабочий
sharp fallback. HUD рисуется после composite.

`WorldRendererStats` публикует dirty regions, uploads/bytes и время подготовки
world pages. `RendererStats` сообщает фактический размер scene target, активен
ли bloom, его resolution, число offscreen passes/targets и CPU submission time
emissive/filter passes, active/peak/dropped presentation FX, а также
cached/visible TerrainBody, body draw calls, texture updates и RGBA8 bytes. Это
не GPU timer: на software renderer значение включает стоимость rasterization.

`RendererRenderScene` вызывается до `BeginDrawing`; `RendererComposite` — между
`BeginDrawing` и `EndDrawing`. Это не допускает вложения backbuffer и
render-texture passes и оставляет HUD вне будущего post-processing.

### Presentation FX API

```c
void PresentationFxInit(PresentationFxSystem *system);
void PresentationFxClear(PresentationFxSystem *system);
bool PresentationFxSpawn(PresentationFxSystem *system,
                         PresentationFxDescription description);
uint16_t PresentationFxConsumeEvents(PresentationFxSystem *system,
                                     const GameEventBuffer *events);
void PresentationFxUpdate(PresentationFxSystem *system, float deltaTime);
```

`RendererUpdatePresentation` сначала обновляет уже существующие instances, а
затем один раз потребляет события кадра, поэтому новый flash впервые рисуется с
нулевым age. `RendererClearPresentation` вызывается при regeneration/reset.

Pool содержит 128 элементов и не выделяет память. При overflow новый instance
заменяет самый близкий к expiration эффект с минимальным priority, только если
его priority не выше incoming; иначе incoming отбрасывается. `dropped`
учитывает отказы, invalid descriptions и заменённые instances.

Доступные primitives: `FLASH`, `RING`, `GLOW`, `LINE`, `TRAIL`. Scene geometry
рисуется резкой, а только descriptions с `emissive=true` повторяются в bloom
mask. Модуль принимает `const GameEventBuffer *`, не входит в `GameState` и
никогда не получает `World *`: visual FX не способны повлиять на simulation.

Внутренний `WorldPrepareVisible` не является gameplay API. Он синхронно
передаёт renderer-у scene и emissive staging только для видимых dirty chunks;
невидимые сохраняют флаг до попадания в кадр. Headless tests проверяют первый
полный build, нулевую работу settled кадра, локальное изменение и то, что
lava/fire входят в mask, а bright sand — нет.

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
`PlayerRendererDraw` и `PlayerRendererDrawEmissive` относятся к presentation
API и объявлены отдельно в `player_renderer.h`. Второй entry point рисует
только boost/drill glow в renderer-owned emissive target.

## Dynamic terrain API

```c
bool DynamicTerrainInit(DynamicTerrainSystem *system);
void DynamicTerrainUnload(DynamicTerrainSystem *system);
void DynamicTerrainReset(DynamicTerrainSystem *system);

TerrainBodyHandle DynamicTerrainAllocBody(DynamicTerrainSystem *system,
                                          int width, int height);
void DynamicTerrainFreeBody(DynamicTerrainSystem *system, TerrainBodyHandle handle);
TerrainBody *DynamicTerrainGet(DynamicTerrainSystem *system, TerrainBodyHandle handle);
const TerrainBody *DynamicTerrainGetConst(const DynamicTerrainSystem *system,
                                          TerrainBodyHandle handle);

void DynamicTerrainSetCell(DynamicTerrainSystem *system, TerrainBodyHandle handle,
                           int localX, int localY, CellMaterial material,
                           float temperature);
CellMaterial DynamicTerrainCellAt(const DynamicTerrainSystem *system,
                                  TerrainBodyHandle handle, int localX, int localY);
float DynamicTerrainTemperatureAt(const DynamicTerrainSystem *system,
                                  TerrainBodyHandle handle, int localX, int localY);
void DynamicTerrainFinalizeBody(DynamicTerrainSystem *system,
                                TerrainBodyHandle handle);
const DynamicTerrainStats *DynamicTerrainStatistics(const DynamicTerrainSystem *system);

DynamicTerrainConfig DynamicTerrainDefaultConfig(void);
void DynamicTerrainUpdate(DynamicTerrainSystem *system, float deltaTime);
/* false when the handle is dead or the awake budget is full. SetVelocity and
   ApplyImpulse do nothing at all when the wake is refused, so a body never
   holds motion it is not allowed to use. */
bool DynamicTerrainWakeBody(DynamicTerrainSystem *system, TerrainBodyHandle handle);
void DynamicTerrainSetVelocity(DynamicTerrainSystem *system, TerrainBodyHandle handle,
                               Vector2 velocity, float angularVelocity);
void DynamicTerrainApplyImpulse(DynamicTerrainSystem *system, TerrainBodyHandle handle,
                                Vector2 impulse, Vector2 worldPoint);

/* The transform every other system must read rather than re-derive. */
Vector2 TerrainBodyLocalToWorld(const TerrainBody *body, float localX, float localY);
Vector2 TerrainBodyWorldToLocal(const TerrainBody *body, float worldX, float worldY);

/* World-space AABB of the occupied box: culling metadata for the simulation,
   readonly, and false for a body with nothing in it. It decides nothing about
   visibility — being outside a viewport never destroys a body. */
bool TerrainBodyWorldBounds(const TerrainBody *body, Vector2 *minimum,
                            Vector2 *maximum);
```

Бюджеты живут в `DynamicTerrainConfig`: `maxAwakeBodies` (24),
`maxDynamicCells` (65 536) и `killBoundsMargin` (512.0). Каждый обеспечивается
отказом, а не вытеснением, и каждый отказ виден в `DynamicTerrainStatistics`
(`allocationFailures`, `cellCapacityFailures`, `awakeBudgetRefusals`,
`bodiesRemovedOutOfBounds`). Подробности — `docs/dynamic-terrain.md`.

## Terrain physics API

```c
void TerrainPhysicsUpdate(DynamicTerrainSystem *system, const World *world,
                          float deltaTime);
bool TerrainPhysicsConfigIsSafe(const DynamicTerrainConfig *config,
                                float boundingRadius, float deltaTime);
```

Объявлено в `terrain_physics.h`. Единственная точка входа шага тела:
интегрирование плюс столкновение со статическим миром, на фиксированном шаге из
`GameAdvanceWorld`. `world` берётся как `const` — изменить клетку столкновение
не может по сигнатуре; `NULL` даёт чистую кинематику. Это же единственное место,
где тело может быть уничтожено за то, что покинуло мир, — проверка выполняется
до проверки `awake`, потому что потерянным бывает и спящее тело. Спящие тела
пропускаются
целиком; обход — плоский по 32 слотам, без аллокаций.

Модель, границы стоимости и стратегия anti-tunnelling —
[dynamic-terrain.md](dynamic-terrain.md).

Объявлено в `dynamic_terrain.h`; владеет подсистемой `GameState`. Ни одна
функция не принимает `World` — изменить мир подсистема не может по сигнатуре.
Зависимостей от GPU-типов нет.

- Все лимиты compile-time и обеспечиваются отказом: `AllocBody` возвращает
  невалидный handle, когда слотов нет или форма не помещается.
- Handle с поколением: обращение к освобождённому телу даёт `NULL`, запись
  через устаревший handle — no-op.
- `SetCell` поддерживает `cellCount`; `FinalizeBody` пересчитывает bounds,
  массу, центр масс и момент инерции. Реальная material/temperature mutation
  также увеличивает `TerrainBody.rasterRevision`; transform её не меняет.
- Единицы массы относительные (плотность материала × площадь клетки).

Модель, лимиты и бюджет памяти — [dynamic-terrain.md](dynamic-terrain.md).

## Terrain body presentation API

```c
void TerrainBodyRendererInit(TerrainBodyRenderer *renderer);
void TerrainBodyRendererDrawScene(TerrainBodyRenderer *renderer,
                                  const DynamicTerrainSystem *terrain,
                                  Rectangle visible);
void TerrainBodyRendererDrawEmissive(TerrainBodyRenderer *renderer,
                                     const DynamicTerrainSystem *terrain,
                                     Rectangle visible);
const TerrainBodyRendererStats *TerrainBodyRendererStatistics(
    const TerrainBodyRenderer *renderer);
void TerrainBodyRendererUnload(TerrainBodyRenderer *renderer);
```

Это renderer-internal lifecycle, который композитит общий `Renderer`.
`DynamicTerrainSystem` передаётся как `const`; texture/cache принадлежат только
presentation. Cache slot использует существующий generation handle плюс
`rasterRevision`, поэтому free/reset/reuse не могут показать texture старого
body. Scene/emissive RGBA8 pair создаётся один раз на generation, обновляется
двумя `UpdateTexture` только при изменении raster и не зависит от window resize.

Headless `terrain_body_render_data.h` отдельно предоставляет render key,
world-space bounds и camera intersection helpers. Bounds проходят через
`TerrainBodyLocalToWorld`, не выводят rotation convention повторно и безопасно
возвращают пустой rectangle для empty/invalid body.

## Terrain extraction API

```c
TerrainExtractResult TerrainExtractComponent(World *world,
                                             DynamicTerrainSystem *terrain,
                                             const WorldComponentWorkspace *workspace,
                                             WorldComponentResult component);
const char *TerrainExtractStatusName(TerrainExtractStatus status);
```

Объявлено в `terrain_extraction.h`. Переносит доказанно отделённую component из
мира в новое тело.

- Извлекается **только** `WORLD_COMPONENT_DETACHED`; всё остальное —
  `TERRAIN_EXTRACT_NOT_DETACHED`.
- `component` и `workspace` обязаны быть из одного вызова `WorldFindComponent`.
- При любом отказе мир не изменён, а тело не остаётся выделенным.
- Стоимость O(клеток component), аллокаций нет.
- Очистка идёт через `WorldSetCell`, поэтому wake/dirty/light-invalidation
  получаются обычным путём записи мира.

Контракт целиком — [dynamic-terrain.md](dynamic-terrain.md).

## World components API

```c
WorldComponentResult WorldFindComponent(const World *world,
                                        WorldComponentWorkspace *workspace,
                                        Rectangle region, int seedX, int seedY,
                                        int maximumCells);
```

Объявлен в `world_components.h`. Обходит связную solid component из seed-клетки
**строго внутри** `region` (в cells, та же конвенция, что у
`WorldActivateRegion`) и возвращает `WORLD_COMPONENT_DETACHED`, `ANCHORED`,
`UNKNOWN`, `TOO_LARGE` или `INVALID`.

- `const World *` — мир только читается; детектор не будит chunks и не меняет
  ни материалов, ни температур.
- `workspace` принадлежит вызывающему (34 KiB). Аллокаций в запросе нет.
- Действовать разрешено только на `DETACHED`; `cellCount` и bounds полны только
  для него. При успехе клетки лежат в
  `workspace->cellX/cellY[0 .. result.cellCount)` в мировых координатах.
- Худший случай: `4 × maximumCells` чтений клеток плюс обнуление
  visited-битмапы размером с площадь региона. От размера мира не зависит.

Правила connectivity, anchoring и bounded search — в
[ADR 0008](adr/0008-detached-component-detection.md).

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
`ParticleRendererDraw` и `ParticleRendererDrawEmissive` объявлены отдельно и не
участвуют в simulation API. Поле `Particle.emission` — presentation hint:
spawn-функция обязана задать его явно, а reuse слота всегда сбрасывает его в
ноль, чтобы несветящаяся частица не унаследовала bloom.

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
