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
- владение `CameraFeedback`, camera follow и применение его bounded output;
- потребление `GameEvents` звуком и камерой;
- порядок platform update и отрисовки;
- HUD.

Сам smoke-прогон живёт в `smoke_test.c`: `main.c` лишь вызывает его фазы в
известных точках кадра (`SmokeTestBeginFrame` до опроса ввода,
`SmokeTestScriptInput` после, `SmokeTestObserve*` после update/камеры/рендера,
`SmokeTestCapture` внутри кадра до `EndDrawing`, `SmokeTestAdvance` и
`SmokeTestReport`). Композиционный корень не знает, что именно прогон проверяет,
а прогон не тянется в состояние цикла иначе как через эти вызовы. В smoke-режиме
окно создаётся без vsync и без лимита FPS: прогон шагает фиксированными тиками и
не смотрит на часы, а композитор отдаёт незасфокусированному окну один кадр в
секунду — с vsync десятисекундный прогон превращался в семиминутный.

### `input.c/.h` и `game_input.h`

`InputPoll` — единственное место, которое опрашивает gameplay keys/mouse. Оно
переводит raw raylib input и screen-space mouse в `GameInput`: move, aimWorld,
boost и команды способностей/reset. Отдельный `toggleDebugPressed` остаётся
app/presentation-командой. Gameplay и headless tests не вызывают `IsKey*` или
`GetMousePosition`.

### `game.c/.h`

`GameState` владеет `World`, `Player`, `AbilitySystem`, `ParticleSystem`,
`DynamicTerrainSystem`, `TerrainDetachSystem`, `TerrainImpulseSystem`,
`TerrainDamageSystem`, `TerrainInteractionSystem`, fixed-step
accumulator и streaming position. `GameUpdate` задаёт единый gameplay order:
player, activation, abilities, particles, необходимое число world ticks,
reaction events и post-simulation collision. Внутри каждого fixed tick порядок
тоже фиксирован: `WorldUpdate`, автоматический detach, удары способностей по телам, физика тел —
связность спрашивают только у мира, закончившего свои записи, а удар доставляют
только тому, что к этому моменту уже стало телом. `GameConfig` собирает world size,
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

Presentation randomness имеет отдельные локальные состояния в FX/audio и не
трогает gameplay streams. Camera вообще не делает случайный draw каждый frame:
её bounded impulses используют однажды вычисленный phase и гладкие волны.

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
| `world_generation.c` | Lifecycle/reset генерации и `WorldPlayerSpawn`. |
| `world_biomes.c` | Coordinate-seeded рельеф, биомные strata, пещеры, жидкости и surface landmarks. |
| `world_lighting.c/.h` | Грубое двухканальное поле света и его solve. |
| `world_effects.c` | Мировая половина способностей: бурение, взрыв, силовой удар, лазер, криолуч. |
| `material_render.c/.h` | Общая CPU-конверсия material/temperature в scene+emissive pixels; static world и detached bodies не имеют двух расходящихся palette paths. |
| `world_render_data.c` | World-specific dirty traversal и light sampling; делегирует palette conversion в `material_render` и отдаёт renderer готовые прямоугольники pixels. |
| `world_components.c/.h` | Bounded-поиск связных solid components: отвечает, отделён ли кусок породы от земли. Мир только читает; вызывается из `terrain_detach.c` после разрушения. |
| `world_fluid.c/.h` | Давление и импульс в жидкости: напор в `lifetime`, подъём столба под напором, очередь импульсов. Приватен модулю мира; вход снаружи — `WorldPushLiquid`. |

`world_internal.h` держит горячие accessors (`WorldCell`, `WorldMaterialAt`,
`CoordinateHash`) как `static inline`. Разделение файлов не должно вставлять
cross-module call в самый горячий цикл проекта; benchmark после разделения
совпал с baseline во всех десяти сценариях.

Материал добавляется одной записью таблицы — см.
[development/adding-a-material.md](development/adding-a-material.md).

### `dynamic_terrain.c/.h`

Fixed-capacity хранилище кусков породы, переставших быть частью клеточного
мира: `DynamicTerrainSystem` владеет `TerrainBody[64]`, material/temperature
raster arena на 8.4 MiB и surface-coordinate arena на 3 MiB, выделенными
при init, и рабочим пространством контактов около 90 KiB внутри себя. `TerrainBody` — один крупный связный кусок terrain, а не entity на
каждую клетку.

Хранилище тел `World` не получает и изменить его не может. Столкновения живут
в отдельных модулях: `terrain_physics.c` **читает** мир через `const World *`
— гарантия компилятора, а не обещание — и ведёт порядок шага;
`terrain_body_collision.c` находит контакты тел друг с другом (растр против
растра, в порядке слотов, спящее тело — стена, пока его не ударят);
`terrain_contact.c` решает контакты с миром и между телами в одном цикле
projected Gauss-Seidel с warm start — порознь куча обломков не засыпала бы
никогда. Всё вызывается на фиксированном шаге из `GameAdvanceWorld`. Тела
рисуются presentation-cache, но пока не сталкиваются с частицами. Presentation
читает систему через `const DynamicTerrainSystem *`; GPU-кэшем владеет
отдельный `TerrainBodyRenderer`. Владеет gameplay-подсистемой `GameState`.
Жёсткие бюджеты вынесены в `terrain_limits.h`, потому что рабочее
пространство контактов — член хранилища и размерено ими.

Стоимость кадра ограничена сверху тремя runtime-бюджетами — тел, занятых клеток
и **активных** тел, — и каждый из них обеспечивается отказом, а не вытеснением.
Спящее тело почти бесплатно (0.001 мс против 0.618 мс за те же 32 тела), поэтому
старые обломки не удаляются никогда: уничтожается только тело, покинувшее мир.
Это правило безопасности мира, а не камеры.

### `fluid_interaction.c/.h` и `terrain_fluid.c/.h`

Единственные места, где жидкость знает о персонаже и о телах. Первый никогда
не замедляет персонажа: он расталкивает воду при нырке и выходе, под быстрым
низким пролётом выбрасывает вверх целую полосу воды (несколько рядов под
поверхностью, импульсы с шагом в несколько клеток за тик), на сверхзвуке
вскипячивает поверхность под собой и шлёт
`GAME_EVENT_LIQUID_SPLASH`/`GAME_EVENT_LIQUID_RIPPLE`; второй на фиксированном
шаге даёт бодрствующим телам плавучесть по плотности, сопротивление, всплеск
и вытеснение воды по поверхности тела. Оба пишут мир только через
`WorldPushLiquid`/`WorldPushLiquidFast`/`WorldSplashLiquid`. Владеет обоими
`GameState`.

### `menu.c/.h` и `settings.c/.h`

Главное меню и настройки — уровень приложения, не геймплея. Игра открывается
в меню поверх только что созданного мира (камера медленно плывёт вдоль
него); Esc в игре ставит мир на паузу и открывает меню. Экраны: главный
(ИГРАТЬ/ПРОДОЛЖИТЬ, НОВЫЙ МИР, НАСТРОЙКИ, ВЫХОД), новый мир с необязательным
сидом (десятичным или `0x`-шестнадцатеричным; пусто — следующий из
последовательности сессии) и настройки. Меню ничего не делает с игрой само:
возвращает `MenuAction`, а `main.c` его исполняет. Раскладка — одна функция
для обновления и отрисовки, поэтому мышь попадает ровно туда, что нарисовано.

Настройки — одна структура и одна таблица `SETTINGS_TABLE` (ключ в файле,
подпись, раздел, тип bool/float/choice, диапазон и шаг). Меню рисует и меняет
настройки только по таблице, файл читается и пишется по ней же, так что
новая настройка — это поле, строка таблицы и то, что приводит её в действие.
Файл — `key = value` в `$XDG_CONFIG_HOME/emberfall/settings.ini` (или
`~/.config/...`); неизвестные ключи пропускаются, отсутствующие остаются по
умолчанию, значения вне диапазона зажимаются. Ни одна настройка не доходит до
симуляции: сид и последовательность ввода обязаны воспроизводиться одинаково
на любой машине. Smoke-тест настроек игрока не читает и фотографирует меню
(`emberfall-menu.png`, `emberfall-settings.png`).

### `material_render.c/.h`

Одна дорога от клетки к пикселю для страниц мира и растров тел: палитра
материала, его узор, оттенок клетки (`Cell.shade`), грани и глубина жидкости
из `MaterialRenderContext`, который заполняет тот, кто обходит клетки.
Солнце и луна — в `environment_renderer.c`, по `dayPhase`.

### `atmosphere.c/.h`

Вход в атмосферу. Коридор между линией космоса и линией облаков
(`AtmosphereCorridorAt`, из того же `WorldGravityScaleAt`, по которому гаснет
притяжение) — там, где воздух есть, но тонок, — и быстрое падение сквозь него
греет. Персонажа воздух **не замедляет** никогда: он только накапливает жар
(с гистерезисом), опаливает то, чего касается, пока горит, и шлёт
`GAME_EVENT_REENTRY`. Тело на фиксированном шаге перед интегрированием
тормозится воздухом квадратично по скорости и греет переднюю грань через
`TerrainDamageTemperAround` — тот же путь, которым бур греет тела, — а вне
коридора грань остывает. Жар хранится по слоту с поколением, поэтому новое
тело в старом слоте холодное. Presentation рисует оболочку огня и хвост
(`presentation_fx.c`) и купол ударной волны перед горящим
(`reentry_renderer.c`, каждый кадр из жара, через const-указатели), даёт
персонажу рокот камеры и рёв воздуха (`SYNTH_REENTRY`); тела через полкарты
камеру не трясут и не слышны. Ниже `entrySpeed` (200, выше крейсерской
скорости) воздух не делает ничего. Владеет `GameState`.

### `terrain_stability.c/.h`

Обвалы. Потолок — solid-клетка, под которой ничего нет, — держится между
опорами на прочность материала (`MaterialInfo.span`); что не держится,
становится `MATERIAL_RUBBLE`, а у концов рухнувшего пролёта, где он встречал
опоры, вверх прорезаются трещины, так что кровля между ними больше ни к чему
не присоединена и `terrain_detach` извлекает её как тело. Никогда не сканирует
мир: спрашивает только там, где лог разрушений сообщил о вырезе, и следует за
обвалом по очереди фиксированной ёмкости с фиксированным числом проверок за
тик. Владеет `GameState`; вызывается после detach-проверки, чтобы то, что
рухнуло в этом тике, попало в лог следующего.

### `terrain_interaction.c/.h`

Единственное место, где игрок и тела знают друг о друге: столкновение, толчок,
захват, перенос и бросок. Ни `Player`, ни `DynamicTerrainSystem` не получают
поля друг о друге — связь целиком здесь. Вызывается из `GameUpdate` последним,
после `PlayerResolveWorldCollision`, поэтому поправка от тела — последнее слово
о том, где игрок оказался.

Проверка — его же проверка против мира, перенесённая в систему координат тела:
поворот сохраняет расстояния, поэтому круг остаётся кругом, и второй
collision-конвенции не появляется. Из перекрытия выталкивается игрок, никогда не
тело.

Захват — пружина с демпфированием в точке, записанной в координатах растра тела,
а не присвоение позиции: тело, телепортированное к курсору, прошло бы сквозь
стену. `maxPullForce / m` — предельное ускорение захвата, и как только оно падает
ниже гравитации, тело можно только тащить.

### `terrain_damage.c/.h`

Выгрызание клеток из растра тела и раскол, когда разрез прошёл насквозь. Работает
только с `DynamicTerrainSystem` и никогда не видит `World`, поэтому через этот
путь статический мир изменить нельзя.

Это не клеточная симуляция внутри движущегося тела и не модель напряжений: вся
модель — связность, и пересчитывается она только после настоящего повреждения
растра. Обе операции заканчиваются поправкой позиции и скорости на сдвиг центра
масс — без неё тело утащило бы каждую уцелевшую клетку.

### `terrain_impulse.c/.h`

Мост «способность → тело». Владеет `TerrainImpulseSystem` (`GameState`) —
очередью из восьми описаний удара; вызывается из `GameAdvanceWorld` между
detach и физикой тел.

Способность не бьёт по телу напрямую: куска, который она освобождает, в момент
срабатывания ещё не существует. Она ставит удар в очередь, фиксированный шаг
сначала отделяет то, что отвалилось, и только потом доставляет удар — поэтому
плита, срезанная взрывом, улетает от того же взрыва. Очередь опустошается при
применении, так что один взрыв срабатывает ровно один раз, сколько бы
фиксированных шагов ни выполнил кадр.

Своей физики здесь нет: `DynamicTerrainApplyImpulse` уже переводит импульс в
точке в линейную и угловую скорость через массу и момент инерции тела. Модуль
решает только, какие тела задеты, насколько сильно и куда приходится удар.
Крупные тела не отвергаются по размеру — их держит масса.

### `terrain_detach.c/.h`

Мост «разрушение → тело»: единственное место, где рельеф отделяется сам.
Владеет `TerrainDetachSystem` (`GameState`), одним `WorldComponentWorkspace` и
bitmap покрытия; вызывается из `GameAdvanceWorld` между `WorldUpdate` и
`TerrainPhysicsUpdate`.

**Emberfall не сканирует весь World в поисках detached terrain.** Проверки
запускаются только локально после известных destructive mutations: `World` ведёт
маленький журнал вырезов (`WorldRecordDestruction`), модуль осушает его,
просматривает коробку повреждения на seed-ы и спрашивает ограниченный детектор
внутри окна фиксированного размера. Подключены explosion и drill; shockwave,
force, laser и thermal — нет, и почему именно, записано в
[dynamic-terrain.md](dynamic-terrain.md).

Стоимость одного вызова целиком выводится из констант и не зависит от размера
мира: регионы × проверки × клеток на проверку. Тик без разрушения не делает
ничего и стоит 0.0000 мс.

Извлечение идёт через обычный `TerrainExtractComponent`, поэтому атомарность и
бюджеты достаются бесплатно: своего кода записи клеток здесь нет.

### `terrain_extraction.c/.h`

Атомарный перенос доказанно отделённой component из `World` в `TerrainBody`.
Отдельный модуль потому, что связывает две подсистемы, ни одна из которых не
должна знать о другой: `DynamicTerrainSystem` не получает `World`, а модуль
мира не знает о телах.

Операция либо завершается целиком, либо мир не меняется вообще. Атомарность
структурная: всё, что может отказать, происходит до первой очистки клетки, а
растр тела служит staging-областью, поэтому неудача — это освобождение тела,
которого никто не видел, а не откат половины мутации.

Вызывается явно и — с EF-DYN-011 — автоматически, из `terrain_detach.c`.

Подробности — [dynamic-terrain.md](dynamic-terrain.md),
решения — [ADR 0009](adr/0009-terrain-body-storage.md).

### `player.c/.h`

Содержит только simulation игрока:

- инерционный полёт без гравитации;
- ускоренный полёт одной скорости, сверхзвук и бурение мира на Shift;
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
короткоживущих world-space primitives: flash, expanding ring, glow core, line,
trail segment и smoke/dust puff. Он обновляет age/delay и три held-contact
cooldown, удаляет истёкшие instances swap-remove и не делает allocation во
время кадра.

Поток данных однонаправленный:

```text
GameEvent -> PresentationFxConsumeEvents -> fixed array
                                           |          |
                                           v          v
                                      scene pass  emissive pass
```

Explosion, laser/cryo contact, force, boost-stage, drill и player impact имеют
конкретные event converters. Explosion создаёт staged flash/core/rings/sparks/
dust/afterglow; delay находится только в presentation state. При заполненной
ёмкости incoming effect заменяет instance с наименьшим priority, ближайший к
expiration; low-priority effect не может вытеснить high-priority. Каждая
замена/отказ увеличивает `dropped`, а HUD показывает active/peak/dropped.

**Visual `PresentationFx` никогда не читает и не меняет `World`.** Это отличает
его от `PARTICLE_CONTACT_SETTLE` debris, который может осесть реальной cell.
Visual spread использует собственный xorshift state. Он не входит в
`GameState`, не касается seeded gameplay streams и не меняет deterministic
simulation digest.

### `renderer.c/.h` и renderer-модули

`Renderer` — presentation owner, создаваемый в `main.c`. Он компонует:

- `sceneTarget` — window-sized offscreen target для резкой world-space сцены;
- `emissiveTarget` — отдельный window-sized target только для выбранных
  источников свечения;
- `bloomPingTarget`/`bloomPongTarget` — half-resolution ping-pong targets для
  threshold/downsample и separable blur;
- два GLSL fragment shader из `assets/shaders/`;
- `EnvironmentRenderer` — seed-derived фиксированные descriptors sky details,
  far peaks, ruined structures, near spires и haze. Он рисует фон прямо в уже
  существующие scene/emissive targets и не имеет доступа к `GameState`/`World`;
- `WorldRenderer` — единственный владелец GPU-состояния страниц мира: кэш
  страниц 256×256 cells, по scene и emissive texture на слот, dirty uploads и
  renderer counters. Резидентны только видимые страницы, поэтому размер мира
  больше не ограничен `GL_MAX_TEXTURE_SIZE`;
- `LightRenderer` — GPU-половина освещения: текстура коарсного светового поля
  и шейдер `world_light.vs/.fs`, который освещает страницы мира и отделённые
  тела по мировой позиции фрагмента. Страницы неосвещённые, поэтому лампа и
  время суток не перестраивают chunks. Без шейдера мир рисуется плоско и
  неосвещённо;
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
full-world `Color` buffer удалён. Pixels `MATERIAL_EMPTY` несут depth tint и
маркер воздуха (`MATERIAL_RENDER_AIR_ALPHA`), а их фактическую прозрачность —
окно в фон над землёй, стена под ней — вычисляет шейдер из светового поля.

`EnvironmentRenderer` принадлежит `Renderer` и синхронизирует только числовой
world seed. Одинаковый seed даёт одинаковые descriptors и palette; CLI/debug
override меняет только presentation. Background рисуется в screen space до
world pages: target/zoom камеры дают parallax, а transient rotation не вращает
сам полноэкранный фон и потому не открывает пустые углы. Три редких energy
columns и окна повторяются в explicit emissive target; brightness extraction и
новый pass не добавляются. Подробнее — [представление мира](world-presentation.md).

`TerrainBodyRenderer` использует ту же `MaterialRenderCell`, что и world pages,
и по-прежнему не получает `World`: тело рисуется под шейдером `LightRenderer`
и освещается по тому месту мира, где оно находится, — плита, унесённая в
пещеру, темнеет в ней; material emission и heat формируют explicit emissive
texture, а негорящие клетки в ней непрозрачно чёрные. Каждый cache
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

При старте синтезирует короткие PCM wave-буферы для лазера/контакта, криолуча/
cold crack, бура, включения ускорения, столкновений, силового удара, трёх слоёв
взрыва и реакции материалов. После
`LoadSoundFromWave` временные CPU-буферы освобождаются. Ошибка инициализации audio
device не является фатальной.

## Порядок одного render frame

Текущий порядок между `main.c` и `GameUpdate` важен:

1. Ограничить `deltaTime` значением 0.05 секунды.
2. Зафиксировать stable aim camera без transient feedback; `InputPoll` создать
   из неё `GameInput`, а F1 переключить на app-уровне.
3. `GameUpdate` при необходимости выполнить reset, обновить player и streaming.
4. Обновить abilities, gameplay/visual particle pool и накопить transient
   `GameEvents`.
5. Выполнить необходимое число fixed ticks мира по 1/60 секунды; преобразовать
   world reactions в `GameEvents` и повторно разрешить player collision.
6. Обновить held audio state, передать events audio и bounded
   `CameraFeedback` consumers.
7. Состарить прежние `PresentationFx`, затем один раз преобразовать события
   текущего кадра в новые instances; при reset очистить presentation pool.
8. Сгладить speed lookahead/view scale, собрать camera impulse stack; вывести
   из stable aim camera отдельную presentation camera и обновить player point
   light. Reticle рисуется через stable camera, world-space scene — через
   presentation camera.
9. `RendererRenderScene` при необходимости пересоздать targets, синхронизировать
   environment seed, нарисовать procedural background, обновить обе paged world
   layers, синхронизировать generation/revision cache динамических тел,
   отрисовать static и detached terrain в sharp scene и выполнить
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
| material/temperature arena динамического terrain (8.4 MiB) | `DynamicTerrainInit` из `GameInit` | `DynamicTerrainUnload` |
| surface-coordinate arena динамического terrain (3 MiB) | `DynamicTerrainInit` из `GameInit` | `DynamicTerrainUnload` |
| контакты, манифолды и warm start тел (~90 KiB) | встроены в `DynamicTerrainSystem` | автоматически |
| scene/emissive texture динамических тел (до 2 MiB RGBA8) | лениво в `TerrainBodyRenderer`, один раз на generation | free/reset sync или `RendererUnload` |
| staging динамических тел (64 KiB) | встроен в `TerrainBodyRenderer` | автоматически |
| particle pool | встроен в `ParticleSystem` | автоматически |
| presentation FX pool (128 instances) | встроен в `Renderer` | автоматически |
| environment descriptors (47 × 20 B + state/stats) | встроены в `Renderer`, regenerated только при смене seed | автоматически |
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
- Мир 16384×4096 выводится через резидентный кэш страниц 256×256, а не через
  одну гигантскую texture. Это около 8192 cells влево и вправо от spawn; нижние
  1440 строк — земля, остальное — небо.
- Камера показывает логическую область 320×180 и масштабирует её к окну.
- Page textures и offscreen targets используют `TEXTURE_FILTER_POINT`, сохраняя
  nearest-neighbor вид; финальный render-texture composite выполняет Y-flip.
- Environment geometry остаётся screen-space и всегда перекрывает target с
  overscan; world-camera target и zoom влияют только на bounded parallax.
- Terrain body texture имеет одну texel на локальную cell. `DrawTexturePro`
  получает `position` как destination position, simulation `centerOfMass` как
  origin и `angle` в градусах, что точно реализует
  `position + rotate(local - centerOfMass, angle)` без второго transform.
- `InputPoll` применяет `GetScreenToWorld2D`, округляет вниз и ограничивает
  результат границами мира.
