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

`GameState` владеет `World`, `Player`, `AbilitySystem`, `ParticleSystem`, fixed-step
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
| `world_render_data.c` | Единственное место, превращающее `Cell` в `Color`; отдаёт renderer готовые прямоугольники пикселей. |

`world_internal.h` держит горячие accessors (`WorldCell`, `WorldMaterialAt`,
`CoordinateHash`) как `static inline`. Разделение файлов не должно вставлять
cross-module call в самый горячий цикл проекта; benchmark после разделения
совпал с baseline во всех десяти сценариях.

Материал добавляется одной записью таблицы — см.
[development/adding-a-material.md](development/adding-a-material.md).

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

### `renderer.c/.h` и renderer-модули

`Renderer` — presentation owner, создаваемый в `main.c`. Он компонует:

- `sceneTarget` — window-sized offscreen target для всей world-space сцены;
- `emissiveTarget` — отдельный прозрачный target, зарезервированный для
  emissive contributors и следующего bloom pass;
- `WorldRenderer` — единственный владелец GPU-состояния мира: кэш страниц
  256×256 cells, dirty uploads и renderer counters. Резидентны только видимые
  страницы, поэтому размер мира больше не ограничен `GL_MAX_TEXTURE_SIZE`;
- `player_renderer` — процедурную модель героя и speed/impact effects;
- `ability_renderer` — beams, force cone, shockwave и прицел;
- `particle_renderer` — чтение фиксированного particle pool.

`WorldPrepareVisible` — узкий внутренний CPU bridge: world готовит один
stack-backed блок 32×32 и синхронно отдаёт его `WorldRenderer`. Visitor
возвращает `bool`: chunk, который renderer не смог разместить (его страница не
резидентна), сохраняет dirty flag и перестраивается позже, а не теряется.
GPU calls, `Draw*` и texture lifecycle в `World` отсутствуют. Persistent
full-world `Color` buffer удалён.

Оба offscreen target принадлежат только `Renderer`: они создаются парой при
`RendererInit`, переиспользуются в steady-state и заменяются новой парой лишь
когда фактический размер окна изменился. Если resize allocation не удался,
предыдущая пара остаётся валидной, а тот же неудачный размер не порождает новую
allocation-попытку каждый кадр. `RendererComposite` исправляет перевёрнутую
Y-ориентацию raylib render texture отрицательной высотой source rectangle.
Targets используют `TEXTURE_FILTER_POINT`, поэтому промежуточный pass не
размывает pixel-art. Gameplay видит только `GameState`/`GameInput`/events и не
знает о `RenderTexture2D` или будущих shaders. При 1280×720 две RGBA8 textures
занимают около 7.03 MiB VRAM; raylib также создаёт для них depth attachments,
что на текущем OpenGL backend добавляет ещё примерно 7.03 MiB.

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
7. Обновить camera follow, затухание shake и player point light.
8. `RendererRenderScene` при необходимости пересоздать targets, обновить
   видимые dirty chunks и отрисовать world-space presentation в `sceneTarget`.
9. `RendererComposite` вывести `sceneTarget` в backbuffer с корректным Y-flip.
10. Отрисовать debug HUD и controls hint напрямую поверх composite.

## Владение памятью

| Ресурс | Создание | Освобождение |
|---|---|---|
| `World.cells` | `WorldInit` | `WorldUnload` |
| chunk buffers | `WorldInit` | `WorldUnload` |
| буфер грязных chunks | `WorldInit` | `WorldUnload` |
| буфер грязных световых chunks | `WorldInit` | `WorldUnload` |
| поля света (sky, ember, показанные копии, emission, opacity) | `WorldInit` | `WorldUnload` |
| кэш страниц мира (`Texture2D` × N) | `WorldRendererInit`, растёт под размер вида | `RendererUnload` |
| scene/emissive `RenderTexture2D` | `RendererInit`, пересоздаются парой только при resize | `RendererUnload` |
| chunk upload staging 32×32 | stack внутри `WorldPrepareVisible` | возврат из вызова |
| particle pool | встроен в `ParticleSystem` | автоматически |
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
текущий CPU estimate равен 167.22 MiB; renderer использует временный staging
размером 4 KiB.

## Координатные пространства

- Cell/world space использует одну world unit на одну cell.
- Мир 16384×864 выводится через резидентный кэш страниц 256×256, а не через
  одну гигантскую texture. Это около 8192 cells влево и вправо от spawn.
- Камера показывает логическую область 320×180 и масштабирует её к окну.
- Page textures и offscreen targets используют `TEXTURE_FILTER_POINT`, сохраняя
  nearest-neighbor вид; финальный render-texture composite выполняет Y-flip.
- `InputPoll` применяет `GetScreenToWorld2D`, округляет вниз и ограничивает
  результат границами мира.
