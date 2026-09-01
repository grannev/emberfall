# Архитектура

## Общий подход

Emberfall — однопоточное приложение на C11 и raylib. Архитектура разделена на
небольшие data-oriented модули. `main.c` композитит platform/presentation, а
`GameState` владеет gameplay state и явно передаёт его подсистемам. Скрытого
глобального игрового состояния нет; глобальным остаётся внутреннее состояние
raylib.

```text
raylib input -> input.c -> GameInput
                              |
                              v
                         GameUpdate
                        /    |     \
                  Player Abilities World fixed ticks
                        \    |     /
                         GameState + GameEvents
                              |
                 +------------+-------------+
                 v            v             v
              renderer      audio       camera/HUD
```

## Модули

### `main.c`

Точка композиции приложения. Отвечает за:

- создание окна и `GameState`/renderer/audio lifecycle;
- владение `GameAudio`, `Camera2D` и presentation state;
- camera follow и camera shake;
- потребление `GameEvents` звуком и камерой;
- порядок platform update и отрисовки;
- HUD и smoke-test integration.

### `input.c/.h` и `game_input.h`

`InputPoll` — единственное место, которое опрашивает gameplay keys/mouse. Оно
переводит raw raylib input и screen-space mouse в `GameInput`: move, aimWorld,
boost и команды способностей/reset. Отдельный `toggleDebugPressed` остаётся
app/presentation-командой. Gameplay и headless tests не вызывают `IsKey*` или
`GetMousePosition`.

### `game.c/.h`

`GameState` владеет `World`, `Player`, `AbilitySystem`, `ParticleSystem`,
`DynamicTerrainSystem`, fixed-step
accumulator и streaming position. `GameUpdate` задаёт единый gameplay order:
player, activation, abilities, particles, необходимое число world ticks,
reaction events и post-simulation collision. `GameConfig` собирает world size,
fixed step и размеры active region в одном месте.

Particle ownership пока переходное: debris действительно меняет World, но в
том же pool остаются чисто визуальные частицы. Разделение выполняется в
presentation phase, не маскируется в текущей схеме.

### `rng.h`

Детерминированный источник случайности для всего, что влияет на gameplay:
SplitMix64 с явным состоянием. raylib `GetRandomValue` берёт из одного
process-wide генератора, общего со всем остальным, поэтому один и тот же seed
давал разный мир в зависимости от того, сколько раз до него успел взять
кто-то ещё. Собственные потоки есть у генерации, мировых эффектов, powers и
particles; все они выводятся из одного seed через `RngStreamSeed`, поэтому
добавление броска в одну систему не сдвигает результат другой.

Presentation-случайность (camera shake) по-прежнему может брать из raylib: она
не способна изменить симуляцию.

### `game_events.c/.h`

Один буфер на render frame хранит до 256 transient events без allocation:
reaction, impact, drill, boost stage, force, explosion и попадания beams.
`GameUpdate` очищает и заполняет буфер; audio/camera/smoke-test читают его после
update. При переполнении новые события отбрасываются и увеличивают `dropped`,
не повреждая память и порядок уже записанных событий.

### Модуль мира: `world.h` + `world_*.c` + `materials.*`

`world.h` — единственный публичный заголовок мира. За ним стоят несколько
файлов с раздельными responsibilities; `world_internal.h` и `world_thermal.h`
внутренние и включаются только другими файлами мира.

| Файл | Ответственность |
|---|---|
| `materials.c/.h` | Таблица материалов: цвет, плотность дизеринга, thermal thresholds, фазовые переходы, реакция на лазер и криолуч. `MaterialsValidate` проверяет таблицу на старте. |
| `world_storage.c` | Владение `Cell`-массивом, chunk-флаги active/dirty/light-dirty, wake-логика, публичные accessors, `WorldActivateRegion`. |
| `world_simulation.c` | Правила движения за tick и фиксированный traversal: bottom-to-top, чередование горизонтального направления, `updatedTick`. |
| `world_thermal.c` | Теплопередача, фазовые переходы, возгорание, реакция water/lava. |
| `world_generation.c` | Генерация карты и `WorldPlayerSpawn`. |
| `world_lighting.c/.h` | Грубое двухканальное поле света и его solve. |
| `world_effects.c` | Мировая половина способностей: бурение, взрыв, силовой удар, лазер, криолуч. |
| `material_render.c/.h` | Общая CPU-конверсия material/temperature в scene+emissive pixels; static world и detached bodies не имеют двух расходящихся palette paths. |
| `world_render_data.c` | Единственное место, превращающее `Cell` в `Color`; отдаёт renderer готовые прямоугольники пикселей. |
| `world_components.c/.h` | Bounded-поиск связных solid components: отвечает, отделён ли кусок породы от земли. Мир только читает, никем пока не вызывается. |

`world_internal.h` держит горячие accessors (`WorldCell`, `WorldMaterialAt`,
`CoordinateHash`) как `static inline`. Разделение файлов не должно вставлять
cross-module call в самый горячий цикл проекта; benchmark после разделения
совпал с baseline во всех десяти сценариях.

Материал добавляется одной записью таблицы — см.
[development/adding-a-material.md](development/adding-a-material.md).

### `dynamic_terrain.c/.h`

Fixed-capacity хранилище кусков породы, переставших быть частью клеточного
мира: `DynamicTerrainSystem` владеет `TerrainBody[32]`, material/temperature
raster arena на 1.25 MiB и surface-coordinate arena на 0.50 MiB, выделенными
при init. `TerrainBody` — один крупный связный кусок terrain, а не entity на
каждую клетку.

Хранилище тел `World` не получает и изменить его не может. Столкновения живут
в отдельном модуле `terrain_physics.c`, который **читает** мир через
`const World *` — гарантия компилятора, а не обещание, — и вызывается на
фиксированном шаге из `GameAdvanceWorld`. Тела рисуются presentation-cache, но
пока не сталкиваются друг с другом, с игроком или с частицами. Presentation читает
систему через `const DynamicTerrainSystem *`; GPU-кэшем владеет отдельный
`TerrainBodyRenderer`. Владеет gameplay-подсистемой `GameState`.

### `terrain_extraction.c/.h`

Атомарный перенос доказанно отделённой component из `World` в `TerrainBody`.
Отдельный модуль потому, что связывает две подсистемы, ни одна из которых не
должна знать о другой: `DynamicTerrainSystem` не получает `World`, а модуль
мира не знает о телах.

Операция либо завершается целиком, либо мир не меняется вообще. Атомарность
структурная: всё, что может отказать, происходит до первой очистки клетки, а
растр тела служит staging-областью, поэтому неудача — это освобождение тела,
которого никто не видел, а не откат половины мутации.

Автоматически не вызывается: подключение к взрыву и буру — EF-DYN-011.

Подробности — [dynamic-terrain.md](dynamic-terrain.md),
решения — [ADR 0009](adr/0009-terrain-body-storage.md).

### `player.c/.h`

Содержит только simulation игрока:

- инерционный полёт без гравитации;
- три последовательные ступени ускоренного полёта, сверхзвук и бурение мира на
  Shift;
- упругий circle-vs-cell collision и impact events;
- защита от tunneling с помощью substeps;
- gameplay/animation state, который renderer читает без обратной связи.

### `abilities.c/.h`

Небольшой реестр способностей: таблица `ABILITIES` описывает то, что одинаково
у всех (имя, trigger, cooldown, follow-through, поза игрока), а функция `apply`
— то, что делает конкретная способность с миром. Драйвер `AbilitiesUpdate`
владеет триггерами, cooldown-ами и таймерами эффекта, поэтому ни одна
способность их не переписывает.

Способность не рисует себя, не трогает `Player` напрямую и не проигрывает звук.
Она заполняет `AbilityState` для renderer и публикует `GameEvent` для audio,
камеры и физической реакции игрока: отдача едет в `GameEvent.playerImpulse`, и
`game.c` применяет её, не зная, какая способность её создала.

Данными способность сделана только там, где это действительно данные.
Что power делает с клеточным миром — это код, и он остаётся C-функцией.

Добавление способности — см.
[development/adding-an-ability.md](development/adding-an-ability.md).

### `particles.c/.h`

Фиксированный циклический пул из 1024 частиц без allocation во время кадра.

Пул один, но ролей две, и они разделены типом. Visual-частицы
(`PARTICLE_CONTACT_PASS`, `PARTICLE_CONTACT_BOUNCE`) обновляются функцией,
которая видит `const World *`: они читают рельеф, чтобы отскакивать от него, и
структурно не способны изменить клетку. Debris (`PARTICLE_CONTACT_SETTLE`) —
единственная роль, которой разрешено писать в мир, и только оседая в пустую
клетку. Поэтому её случайность seeded, а поведение покрыто headless-тестами.
`Particle.emission` задаёт только presentation contribution в bloom mask и
сбрасывается при каждом reuse слота; на поведение частицы и мир оно не влияет.

### `presentation_fx.c/.h` и `presentation_fx_renderer.c/.h`

`PresentationFxSystem` — renderer-owned компактный массив из максимум 128
короткоживущих world-space primitives: flash, expanding ring, glow core, line и
trail segment. Он обновляет только `age`, удаляет истёкшие instances swap-remove
и не делает allocation во время кадра.

Поток данных однонаправленный:

```text
GameEvent -> PresentationFxConsumeEvents -> fixed array
                                           |          |
                                           v          v
                                      scene pass  emissive pass
```

Сейчас explosion event создаёт flash + ring, а player impact — небольшой
flash. Это proof интеграции, не staged explosion polish. При заполненной
ёмкости incoming effect заменяет instance с наименьшим priority, ближайший к
expiration; low-priority effect не может вытеснить high-priority. Каждая
замена/отказ увеличивает `dropped`, а HUD показывает active/peak/dropped.

**Visual `PresentationFx` никогда не читает и не меняет `World`.** Это отличает
его от `PARTICLE_CONTACT_SETTLE` debris, который может осесть реальной cell.
Система сейчас не использует randomness; любой будущий visual jitter обязан
иметь отдельный presentation RNG и не касаться seeded gameplay streams.

### `renderer.c/.h` и renderer-модули

`Renderer` — presentation owner, создаваемый в `main.c`. Он компонует:

- `sceneTarget` — window-sized offscreen target для резкой world-space сцены;
- `emissiveTarget` — отдельный window-sized target только для выбранных
  источников свечения;
- `bloomPingTarget`/`bloomPongTarget` — half-resolution ping-pong targets для
  threshold/downsample и separable blur;
- два GLSL fragment shader из `assets/shaders/`;
- `WorldRenderer` — единственный владелец GPU-состояния мира: кэш страниц
  256×256 cells, по scene и emissive texture на слот, dirty uploads и renderer
  counters. Резидентны только видимые страницы, поэтому размер мира больше не
  ограничен `GL_MAX_TEXTURE_SIZE`;
- `player_renderer` — процедурную модель героя и speed/impact effects;
- `ability_renderer` — непрерывные beams, force cone и прицел;
- `particle_renderer` — чтение фиксированного particle pool.
- `PresentationFxSystem` и его renderer — event-driven transient geometry в
  sharp scene и, только для помеченных instances, в emissive target.
- `TerrainBodyRenderer` — фиксированный cache scene/emissive texture для 32
  `TerrainBody`, generation/revision invalidation, COM rotation и camera
  culling. Simulation не получает GPU state.

`WorldPrepareVisible` — узкий внутренний CPU bridge: world за один проход
готовит два stack-backed блока 32×32 (scene и explicit emissive mask) и
синхронно отдаёт их `WorldRenderer`. Brightness extraction не используется:
обычный яркий sand остаётся вне bloom, а material `emission` и нагретые solid
faces попадают в mask. Visitor
возвращает `bool`: chunk, который renderer не смог разместить (его страница не
резидентна), сохраняет dirty flag и перестраивается позже, а не теряется.
GPU calls, `Draw*` и texture lifecycle в `World` отсутствуют. Persistent
full-world `Color` buffer удалён.

`TerrainBodyRenderer` использует ту же `MaterialRenderCell`, что и world pages,
но не получает `World`: moving body пока освещается постоянным neutral ambient,
а material emission и heat формируют explicit emissive texture. Каждый cache
slot соответствует simulation slot и проверяет и generation handle, и
`rasterRevision`, поэтому reuse не показывает старую texture, а изменение
материала/температуры делает два `UpdateTexture` без пересоздания GPU objects.
Неизменившийся raster не обходится. Две exact-size RGBA8 texture создаются один
раз при появлении body, используют `TEXTURE_FILTER_POINT` и освобождаются при
free/reset либо `RendererUnload`; resize окна их не затрагивает.

Четыре offscreen target принадлежат только `Renderer`. Full-resolution scene и
emissive используют point filtering; half-resolution bloom targets — bilinear.
Они переиспользуются в steady-state и заменяются лишь при фактическом resize.
Если обязательная full-resolution пара не выделилась, предыдущая остаётся
валидной; если не выделился bloom pair или не загрузились shaders, renderer
gracefully выводит резкую scene без bloom. Неудачная allocation не повторяется
каждый frame: renderer делает следующий retry через 120 кадров либо сразу после
нового изменения фактического размера окна.

Специализированный pipeline состоит из пяти offscreen passes: sharp scene,
explicit emissive, threshold/downsample, horizontal blur и vertical blur.
`RendererComposite` сначала выводит sharp scene, затем аддитивно накладывает
только blurred emissive. Каждый переход исправляет Y-ориентацию raylib render
texture отрицательной высотой source rectangle. Поэтому terrain не размывается,
а gameplay по-прежнему не знает о `RenderTexture2D`/`Shader`.

Tuning собран в `BLOOM` внутри `renderer.c`: intensity 0.72, radius 1.35,
threshold 0.08, downsample factor 2. При 1280×720 четыре RGBA8/depth targets
занимают на текущем OpenGL backend примерно 17.58 MiB VRAM: 14.06 MiB для двух
full-resolution и 3.52 MiB для двух 640×360 targets.

### `audio.c/.h`

При старте синтезирует короткие PCM wave-буферы для лазера, криолуча, бура,
ступеней ускорения, столкновений, силового удара, взрыва и реакции материалов. После
`LoadSoundFromWave` временные CPU-буферы освобождаются. Ошибка инициализации audio
device не является фатальной.

## Порядок одного render frame

Текущий порядок между `main.c` и `GameUpdate` важен:

1. Ограничить `deltaTime` значением 0.05 секунды.
2. `InputPoll` создать `GameInput`; F1 переключить на app-уровне.
3. `GameUpdate` при необходимости выполнить reset, обновить player и streaming.
4. Обновить abilities, gameplay/visual particle pool и накопить transient
   `GameEvents`.
5. Выполнить необходимое число fixed ticks мира по 1/60 секунды; преобразовать
   world reactions в `GameEvents` и повторно разрешить player collision.
6. Обновить held audio state и передать events audio/camera consumers.
7. Состарить прежние `PresentationFx`, затем один раз преобразовать события
   текущего кадра в новые instances; при reset очистить presentation pool.
8. Обновить camera follow, затухание shake и player point light.
9. `RendererRenderScene` при необходимости пересоздать targets, обновить обе
   paged world layers, синхронизировать generation/revision cache динамических
   тел, отрисовать static и detached terrain в sharp scene и выполнить
   emissive/downsample/horizontal-blur/vertical-blur passes.
10. `RendererComposite` вывести sharp scene и аддитивно наложить blurred
   emissive в backbuffer с корректным Y-flip.
11. Отрисовать debug HUD и controls hint напрямую поверх composite.

## Владение памятью

| Ресурс | Создание | Освобождение |
|---|---|---|
| `World.cells` | `WorldInit` | `WorldUnload` |
| chunk buffers | `WorldInit` | `WorldUnload` |
| буфер грязных chunks | `WorldInit` | `WorldUnload` |
| буфер грязных световых chunks | `WorldInit` | `WorldUnload` |
| поля света (sky, ember, показанные копии, emission, opacity) | `WorldInit` | `WorldUnload` |
| scene/emissive кэш страниц (`Texture2D` × 2N) | `WorldRendererInit`, растёт под размер вида | `RendererUnload` |
| scene/emissive `RenderTexture2D` | `RendererInit`, пересоздаются только при resize | `RendererUnload` |
| bloom ping/pong `RenderTexture2D` | `RendererInit`, half-resolution, только при resize | `RendererUnload` |
| bloom shaders | `RendererInit`, ошибка включает sharp fallback | `RendererUnload` |
| scene/emissive staging 32×32 × 2 | stack внутри `WorldPrepareVisible` | возврат из вызова |
| material/temperature arena динамического terrain (1.25 MiB) | `DynamicTerrainInit` из `GameInit` | `DynamicTerrainUnload` |
| surface-coordinate arena динамического terrain (0.50 MiB) | `DynamicTerrainInit` из `GameInit` | `DynamicTerrainUnload` |
| scene/emissive texture динамических тел (до 2 MiB RGBA8) | лениво в `TerrainBodyRenderer`, один раз на generation | free/reset sync или `RendererUnload` |
| staging динамических тел (64 KiB) | встроен в `TerrainBodyRenderer` | автоматически |
| particle pool | встроен в `ParticleSystem` | автоматически |
| presentation FX pool (128 instances) | встроен в `Renderer` | автоматически |
| sounds | `GameAudioInit` | `GameAudioUnload` |

`GameState` агрегирует CPU gameplay ownership; `GameInit`/`GameUnload`
являются верхней lifecycle-парой. Независимая пара
`RendererInit`/`RendererUnload` владеет GPU presentation и вызывается пока
raylib window/context ещё жив.

Heap allocation в steady-state frame loop запрещён. Размеры world buffers и
particle pool не меняются во время игры; renderer allocations допустимы только
при реальном resize, когда меняются targets и при необходимости ёмкость page
cache. В стандартном мире 14 155 776 cells; `Cell` уплотнена до 12 bytes,
поэтому основной cell buffer занимает 162 MiB. После удаления persistent pixels
текущий persistent CPU estimate равен 167.22 MiB; renderer использует временный
staging размером 8 KiB.

## Координатные пространства

- Cell/world space использует одну world unit на одну cell.
- Мир 16384×864 выводится через резидентный кэш страниц 256×256, а не через
  одну гигантскую texture. Это около 8192 cells влево и вправо от spawn.
- Камера показывает логическую область 320×180 и масштабирует её к окну.
- Page textures и offscreen targets используют `TEXTURE_FILTER_POINT`, сохраняя
  nearest-neighbor вид; финальный render-texture composite выполняет Y-flip.
- Terrain body texture имеет одну texel на локальную cell. `DrawTexturePro`
  получает `position` как destination position, simulation `centerOfMass` как
  origin и `angle` в градусах, что точно реализует
  `position + rotate(local - centerOfMass, angle)` без второго transform.
- `InputPoll` применяет `GetScreenToWorld2D`, округляет вниз и ограничивает
  результат границами мира.
