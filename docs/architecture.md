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

### `game_events.c/.h`

Один буфер на render frame хранит до 256 transient events без allocation:
reaction, impact, drill, boost stage, force, explosion и попадания beams.
`GameUpdate` очищает и заполняет буфер; audio/camera/smoke-test читают его после
update. При переполнении новые события отбрасываются и увеличивают `dropped`,
не повреждая память и порядок уже записанных событий.

### `world.c/.h`

Владеет физическим миром и GPU-текстурой мира:

- непрерывный массив `Cell`;
- постоянный `Color`-буфер;
- `Texture2D` размером с симуляцию;
- active-chunk буферы;
- потоковая активация динамики вокруг игрока через `WorldActivateRegion`;
- материалы, температура и фазовые переходы;
- генерация карты;
- лазерное и криогенное воздействие, разрушение, ударная волна и силовой конус;
- очередь событий water/lava reaction фиксированного размера.

### `player.c/.h`

Содержит движение, collision и отрисовку игрока:

- инерционный полёт без гравитации;
- три последовательные ступени ускоренного полёта, сверхзвук и бурение мира на
  Shift;
- упругий circle-vs-cell collision и impact events;
- защита от tunneling с помощью substeps;
- state-based отрисовка компактного пиксельного героя.

### `powers.c/.h`

Хранит состояние способностей и координирует их эффекты:

- трассировка контактного лазера;
- cooldown взрыва и силового удара;
- разрушение мира и ударная волна;
- силовой конус, криолуч и их visual state;
- события для camera shake, player impulse и audio;
- world-space визуализация луча, прицела и кольца взрыва.

### `particles.c/.h`

Фиксированный циклический пул из 1024 частиц. Частицы читают мир для контакта
с рельефом и могут оседать в него настоящими cells. Они не выделяют память во
время кадра. Разные spawn-функции задают скорость, цвет, lifetime, размер и
индивидуальную gravity.

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
8. Обновить texture мира и отрисовать world-space объекты.
9. Отрисовать debug HUD.

## Владение памятью

| Ресурс | Создание | Освобождение |
|---|---|---|
| `World.cells` | `WorldInit` | `WorldUnload` |
| `World.pixels` | `WorldInit` | `WorldUnload` |
| chunk buffers | `WorldInit` | `WorldUnload` |
| буфер грязных chunks | `WorldInit` | `WorldUnload` |
| буфер грязных световых chunks | `WorldInit` | `WorldUnload` |
| поля света (sky, ember, показанные копии, emission, opacity) | `WorldInit` | `WorldUnload` |
| world `Texture2D` | `WorldInitRenderer` | `WorldUnload` |
| particle pool | встроен в `ParticleSystem` | автоматически |
| sounds | `GameAudioInit` | `GameAudioUnload` |

`GameState` агрегирует CPU gameplay ownership; `GameInit`/`GameUnload` являются
верхней lifecycle-парой. GPU texture ещё принадлежит `World` и поэтому пока
освобождается внутри `GameUnload -> WorldUnload`; renderer phase перенесёт этот
ресурс в отдельного владельца.

Heap allocation в frame loop запрещён. Размеры world buffers и particle pool не
меняются во время игры. В стандартном мире 14 155 776 cells; `Cell` уплотнена до
16 bytes (`material` — `uint8_t`), поэтому основной cell buffer занимает около
216 MiB, а постоянный `Color`-буфер — около 54 MiB.

## Координатные пространства

- Cell/world space использует одну world unit на одну cell.
- World texture имеет размер 16384×864 и рисуется в начале world space. Это
  около 8192 cells влево и вправо от центрального spawn.
- Камера показывает логическую область 320×180 и масштабирует её к окну.
- `TEXTURE_FILTER_POINT` сохраняет nearest-neighbor вид.
- `InputPoll` применяет `GetScreenToWorld2D`, округляет вниз и ограничивает
  результат границами мира.
