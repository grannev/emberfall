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
                   Player  Powers  World fixed ticks
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

`GameState` владеет `World`, `Player`, `PowerSystem`, `ParticleSystem`, fixed-step
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

### `powers.c/.h`

Хранит состояние способностей и координирует их эффекты:

- трассировка контактного лазера;
- cooldown взрыва и силового удара;
- разрушение мира и ударная волна;
- силовой конус, криолуч и их visual state;
- события для camera shake, player impulse и audio.

### `particles.c/.h`

Фиксированный циклический пул из 1024 частиц. Частицы читают мир для контакта
с рельефом и могут оседать в него настоящими cells. Они не выделяют память во
время кадра. Разные spawn-функции задают скорость, цвет, lifetime, размер и
индивидуальную gravity.

### `renderer.c/.h` и renderer-модули

`Renderer` — presentation owner, создаваемый в `main.c`. Он компонует:

- `WorldRenderer` — единственный владелец world `Texture2D`, dirty uploads и
  renderer counters;
- `player_renderer` — процедурную модель героя и speed/impact effects;
- `ability_renderer` — beams, force cone, shockwave и прицел;
- `particle_renderer` — чтение фиксированного particle pool.

`WorldPrepareVisible` — узкий внутренний CPU bridge: world готовит один
stack-backed блок 32×32 и синхронно отдаёт его `WorldRenderer`. GPU calls,
`Draw*` и texture lifecycle в `World` отсутствуют. Persistent full-world
`Color` buffer удалён.

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
8. `RendererDrawWorldSpace` обновить видимые dirty chunks и отрисовать
   world-space presentation.
9. Отрисовать debug HUD.

## Владение памятью

| Ресурс | Создание | Освобождение |
|---|---|---|
| `World.cells` | `WorldInit` | `WorldUnload` |
| chunk buffers | `WorldInit` | `WorldUnload` |
| буфер грязных chunks | `WorldInit` | `WorldUnload` |
| буфер грязных световых chunks | `WorldInit` | `WorldUnload` |
| поля света (sky, ember, показанные копии, emission, opacity) | `WorldInit` | `WorldUnload` |
| world `Texture2D` | `RendererInit -> WorldRendererInit` | `RendererUnload` |
| chunk upload staging 32×32 | stack внутри `WorldPrepareVisible` | возврат из вызова |
| particle pool | встроен в `ParticleSystem` | автоматически |
| sounds | `GameAudioInit` | `GameAudioUnload` |

`GameState` агрегирует CPU gameplay ownership; `GameInit`/`GameUnload`
являются верхней lifecycle-парой. Независимая пара
`RendererInit`/`RendererUnload` владеет GPU presentation и вызывается пока
raylib window/context ещё жив.

Heap allocation в frame loop запрещён. Размеры world buffers и particle pool не
меняются во время игры. В стандартном мире 14 155 776 cells; `Cell` уплотнена до
16 bytes (`material` — `uint8_t`), поэтому основной cell buffer занимает около
216 MiB. После удаления persistent pixels текущий CPU estimate равен
221.12 MiB; renderer использует временный staging размером 4 KiB.

## Координатные пространства

- Cell/world space использует одну world unit на одну cell.
- World texture имеет размер 16384×864 и рисуется в начале world space. Это
  около 8192 cells влево и вправо от центрального spawn.
- Камера показывает логическую область 320×180 и масштабирует её к окну.
- `TEXTURE_FILTER_POINT` сохраняет nearest-neighbor вид.
- `InputPoll` применяет `GetScreenToWorld2D`, округляет вниз и ограничивает
  результат границами мира.
