# Производительность и память

Этот документ хранит воспроизводимые измерения Emberfall. Wall-clock числа
информационные: сравнивать результаты нужно на одной машине и в одинаковом
режиме. Счётчики `processedCells`, `processedChunks`, active/sleeping chunks и
dirty regions не зависят от частоты CPU и подходят для regression-проверок.

## Команды

```sh
make bench
make bench BENCH_ARGS="--ticks 60"
make profile
make asan
make ubsan
```

`make bench` не открывает окно и не инициализирует GPU/audio. Он использует мир
реального размера 16384×864, фиксированный seed `0x00e6be11` и по 180 ticks в
десяти сценариях: settled world, sand, water, fire/lava, explosion, mass
destruction, boost drilling, force, cryo и mixed chaos. Время каждого scenario
включает его gameplay action и `WorldUpdate`, но не повторную генерацию перед
сценарием.

GPU uploads и render preparation в headless таблице помечены `n/a`, потому что
benchmark намеренно не создаёт GL context. Runtime `WorldRendererStats`
показывает dirty regions, texture uploads, uploaded bytes, число видимых и
резидентных страниц, число привязок страниц за кадр и совокупное время CPU
preparation/upload — всё это выводится в debug HUD строкой `RENDER`.

## Baseline 2026-08-31

Commit до audit: `34bf7c3`. Машина: Intel Core i5-6300U (2 cores/4 threads),
Linux x86_64 7.1.4, GCC 16.1.1, raylib 6.0.0. Release: `-O2`.

- Чистая release-сборка: 4,39 s.
- Все 41 headless tests: 10,16 s с компиляцией.
- Xvfb/llvmpipe smoke-test: 5,49 s, код возврата 0.
- Временный pre-suite probe: generation 463 ms; 120 локально активированных
  ticks — 1,66 ms/tick; 23 active chunks в конце.
- Valgrind Massif peak полезного heap: 288 483 448 B (275,12 MiB).

Первый постоянный `make bench` после добавления только instrumentation:

| Scenario | avg ms | p50 | p95 | p99 | cells/tick | chunks/tick |
|---|---:|---:|---:|---:|---:|---:|
| settled world | 0.594 | 0.530 | 0.889 | 0.991 | 0 | 0 |
| falling sand | 2.042 | 1.801 | 2.863 | 6.619 | 59 630 | 58 |
| large water | 4.333 | 4.045 | 5.871 | 7.215 | 86 664 | 84 |
| fire and lava | 2.678 | 2.247 | 4.404 | 5.257 | 52 929 | 51 |
| large explosion | 1.727 | 1.711 | 2.595 | 6.962 | 44 691 | 43 |
| mass destruction | 0.998 | 0.888 | 1.358 | 2.205 | 23 045 | 22 |
| boost drilling | 1.050 | 0.954 | 1.528 | 2.251 | 27 904 | 27 |
| force ability | 2.138 | 1.876 | 3.190 | 5.191 | 48 372 | 47 |
| cryo ability | 3.966 | 3.719 | 5.671 | 6.278 | 87 648 | 85 |
| chaotic mixed | 5.864 | 5.535 | 7.871 | 9.815 | 139 628 | 136 |

Init: 57,2 ms; generate: 465,3 ms; regenerate: 488,7 ms. Текущий resident
на момент измерения был 221 MiB, потому что Linux ещё не materialized все
страницы `malloc`-буфера pixels; Massif и расчёт allocation дают полный объём.

## Memory breakdown baseline

Для 14 155 776 cells:

| Owner | Layout | Bytes | MiB |
|---|---|---:|---:|
| World cells | 14 155 776 × 16 B | 226 492 416 | 216.00 |
| World pixels | 14 155 776 × 4 B | 56 623 104 | 54.00 |
| Chunk flags | 13 824 × 4 B | 55 296 | 0.05 |
| Lighting | 221 184 × 6 × 4 B | 5 308 416 | 5.06 |
| Итого CPU estimate | плюс `World`/minor | ~288 489 232 | 275.12 |

Одна GPU texture 16384×864 RGBA8 требует ещё примерно 54 MiB VRAM. Во время
`GenImageColor` существует временный CPU image такого же размера, поэтому пик
startup выше steady state.

## Проверенные hypotheses на baseline

- **Подтверждено:** persistent `World.pixels` стоит 54 MiB и используется как
  staging для dirty texture uploads; постоянное full-world хранение не нужно.
- **Подтверждено:** `Cell` в 16 B доминирует в памяти (216 MiB). Менять layout
  можно только с before/after benchmark и regression tests. Закрыто: 12 B,
  162 MiB.
- **Подтверждено как архитектурный limit:** одна texture шириной 16384 зависит
  от GPU maximum texture size и не масштабируется дальше. Закрыто render
  paging-ом: резидентны только видимые страницы 256×256.
- **Частично подтверждено:** sleeping world не обрабатывает cells, но каждый
  tick всё равно сканирует 13 824 chunk flags; это видно по 0,59 ms settled.
  Закрыто active chunk scheduler-ом: settled world стоит 0.000 ms.
- **Не измерено:** цена moving player light, GPU uploads и render preparation.
  Свет измерен и оптимизирован: 10.19 → 1.73 ms на кадр.
- **Подтверждено:** generation eager — каждый reset записывает все 14,1 млн
  cells и занимает около 0,47 s.
- **Подтверждено аудитом:** `world.c` совмещает storage, materials, simulation,
  thermal, generation, effects, lighting и renderer; World владеет GPU state.
  Обе части закрыты: GPU state перенесён в `WorldRenderer`, а `world.c`
  разделён на модули по ответственностям.
- **Подтверждено аудитом:** player, abilities и particles смешивают simulation
  с `Draw*`; gameplay RNG зависит от глобального raylib RNG.

Результаты следующих крупных phases добавляются ниже, а baseline не
перезаписывается.

## После Game/Input/Events boundary

Gameplay orchestration перенесена из `main.c` в `GameUpdate`, raw input — в
`input.c`, transient flags собраны в fixed-capacity `GameEventBuffer`. World hot
loops и layout не менялись. Повторный `make bench` дал:

| Scenario | avg ms | p50 | p95 | p99 | cells/tick | chunks/tick |
|---|---:|---:|---:|---:|---:|---:|
| settled world | 0.565 | 0.498 | 0.854 | 0.945 | 0 | 0 |
| falling sand | 1.874 | 1.608 | 2.899 | 3.540 | 59 630 | 58 |
| large water | 4.129 | 3.705 | 5.849 | 7.002 | 86 664 | 84 |
| fire and lava | 2.267 | 2.042 | 3.493 | 4.062 | 52 929 | 51 |
| large explosion | 1.668 | 1.667 | 2.911 | 6.635 | 44 691 | 43 |
| mass destruction | 0.999 | 0.843 | 1.537 | 2.143 | 23 045 | 22 |
| boost drilling | 1.029 | 0.908 | 1.587 | 1.902 | 27 904 | 27 |
| force ability | 2.132 | 1.815 | 3.419 | 4.099 | 48 372 | 47 |
| cryo ability | 3.933 | 3.508 | 5.630 | 6.602 | 87 648 | 85 |
| chaotic mixed | 5.999 | 5.311 | 9.232 | 14.392 | 139 628 | 136 |

Structural workload совпал с baseline во всех сценариях. Разброс wall-clock
остался обычным для этой машины; ускорение не заявляется. Memory layout и
estimate не изменились.

## После разделения presentation и GPU ownership

`PlayerDraw`, `PowersDrawWorld` и `ParticlesDraw` механически перенесены в
dedicated renderer modules. `Renderer` стал presentation owner, а
`WorldRenderer` — единственным владельцем world `Texture2D`. Из `World`
удалён persistent full-world `Color *pixels`; видимый dirty chunk теперь
строится в stack staging 32×32 (4 KiB) и немедленно загружается.

Memory для production world:

| Owner | Baseline MiB | После MiB | Изменение |
|---|---:|---:|---:|
| Cells | 216.00 | 216.00 | 0 |
| Persistent world pixels | 54.00 | 0.00 | -54.00 |
| Chunk flags | 0.05 | 0.05 | 0 |
| Lighting | 5.06 | 5.06 | 0 |
| CPU estimate | 275.12 | 221.12 | **-54.00 MiB (-19.6%)** |
| Renderer staging | 0 | 0.0039 | stack, не persistent heap |

Полный повторный `make bench`:

| Scenario | avg ms | p50 | p95 | p99 | cells/tick | chunks/tick |
|---|---:|---:|---:|---:|---:|---:|
| settled world | 0.587 | 0.535 | 0.923 | 1.182 | 0 | 0 |
| falling sand | 1.884 | 1.634 | 3.059 | 3.550 | 59 630 | 58 |
| large water | 4.421 | 3.930 | 6.591 | 8.623 | 86 664 | 84 |
| fire and lava | 2.313 | 2.084 | 3.657 | 4.418 | 52 929 | 51 |
| large explosion | 1.665 | 1.667 | 2.814 | 6.750 | 44 691 | 43 |
| mass destruction | 0.971 | 0.893 | 1.259 | 1.986 | 23 045 | 22 |
| boost drilling | 1.059 | 0.919 | 1.633 | 1.961 | 27 904 | 27 |
| force ability | 2.068 | 1.807 | 3.307 | 5.008 | 48 372 | 47 |
| cryo ability | 4.024 | 3.653 | 5.558 | 6.614 | 87 648 | 85 |
| chaotic mixed | 5.930 | 5.353 | 8.736 | 10.365 | 139 628 | 136 |

Init 49.6 ms, generation 427.4 ms, regeneration 459.7 ms; measured RSS and
peak RSS after materializing the world were 220.95/220.87 MiB. Structural
workload снова полностью совпал с baseline. Simulation algorithms не менялись,
поэтому ускорение CPU tick не заявляется; результат этой phase — измеримое
снижение памяти и чистая ownership boundary.

Одна texture 16384×864 всё ещё требует примерно 54 MiB VRAM и остаётся hard
limit текущего renderer. Теперь ограничение локализовано в `WorldRenderer`, так
что render paging не потребует менять `World` или gameplay modules.

## После декомпозиции world.c

`world.c` (2399 строк) разделён на `materials.c`, `world_storage.c`,
`world_simulation.c`, `world_thermal.c`, `world_generation.c`,
`world_lighting.c`, `world_effects.c` и `world_render_data.c` без изменения
алгоритмов. Гипотеза «`world.c` совмещает слишком много responsibilities»
подтверждена и закрыта.

Единственный риск разделения — потеря inlining в горячем цикле: `WorldGetCell`,
`WorldCell`, `WorldInBounds`, `MaterialAt` и `CoordinateHash` вызываются
несколько раз на клетку за tick и раньше жили в одном translation unit.
Поэтому они перенесены в `world_internal.h`/`materials.h` как `static inline`,
а внутренние вызывающие используют `WorldMaterialAt` вместо публичного
`WorldGetCell`. Значение cryo-скорости переехало из `switch` в колонку
`chillRate` таблицы материалов; числа не менялись.

| Scenario | до avg ms | после avg ms | cells/tick |
|---|---:|---:|---:|
| settled world | 0.533 | 0.398 | 0 |
| falling sand | 1.734 | 1.727 | 59 630 |
| large water | 4.294 | 4.131 | 86 664 |
| fire and lava | 2.214 | 2.159 | 52 929 |
| large explosion | 1.634 | 1.534 | 44 691 |
| mass destruction | 0.941 | 0.859 | 23 045 |
| boost drilling | 1.005 | 0.935 | 27 904 |
| force ability | 1.974 | 1.872 | 48 372 |
| cryo ability | 4.218 | 3.813 | 87 648 |
| chaotic mixed | 5.819 | 5.639 | 139 628 |

Оба замера сделаны в одной сессии на одной машине. Структурные счётчики
совпали до единицы во всех десяти сценариях: активные/спящие chunks,
`cells/tick`, `chunks/tick` и `dirty/tick` идентичны, то есть работа не
изменилась. Wall-clock во всех сценариях не хуже прежнего; регрессии от
разделения нет. Ускорение не заявляется — разброс этой машины сопоставим с
разницей, а алгоритмы не менялись. Память не изменилась: estimate 221.12 MiB,
RSS 220.96 MiB.

## После детерминированного RNG

Gameplay больше не берёт из process-wide генератора raylib. Генерация мира,
мировые эффекты, powers и particles получили собственные потоки SplitMix64,
выведенные из одного seed. `WorldGenerate(world, seed)` теперь принимает seed
явно, `GameConfig.seed` задаёт seed сессии, а `--seed VALUE` и строка `SEED` в
debug HUD делают любой мир воспроизводимым.

**Это создаёт новый structural baseline, а не regression.** Benchmark
генерирует мир другим генератором, значит рельеф под сценариями другой, и
`cells/tick` сравнивать со строками выше нельзя. Числа до этой phase
сопоставимы между собой, числа начиная с этой — между собой.

| Scenario | avg ms | p50 | p95 | p99 | cells/tick | chunks/tick |
|---|---:|---:|---:|---:|---:|---:|
| settled world | 0.391 | 0.376 | 0.475 | 0.551 | 0 | 0 |
| falling sand | 1.519 | 1.413 | 1.940 | 2.687 | 47 627 | 46 |
| large water | 3.882 | 3.643 | 4.996 | 6.814 | 74 660 | 72 |
| fire and lava | 2.094 | 1.892 | 3.108 | 3.518 | 40 925 | 39 |
| large explosion | 1.295 | 1.427 | 1.792 | 6.685 | 32 375 | 31 |
| mass destruction | 0.658 | 0.574 | 0.850 | 1.415 | 11 042 | 10 |
| boost drilling | 0.687 | 0.640 | 0.940 | 1.185 | 15 900 | 15 |
| force ability | 1.632 | 1.486 | 2.071 | 4.183 | 36 369 | 35 |
| cryo ability | 3.691 | 3.487 | 4.710 | 6.877 | 75 645 | 73 |
| chaotic mixed | 5.426 | 5.297 | 6.743 | 8.247 | 127 624 | 124 |

Init 71.2 ms, generation 370.0 ms, regeneration 346.3 ms. Память не изменилась:
estimate 221.12 MiB, RSS 220.89 MiB. `Rng` — 8 bytes на систему, вне cell
storage.

Гипотеза «gameplay RNG мешает reproducibility» подтверждена и закрыта:
`test_a_seeded_session_replays_identically` прогоняет 40 фиксированных кадров с
лазером, взрывом, силовым ударом и криолучом в двух независимых `GameState` и
требует побитового совпадения позиции игрока и digest мира.

## После разделения abilities и particles

`PowerSystem` заменён реестром `AbilitySystem`: общая часть (trigger, cooldown,
follow-through, выбранная способность) принадлежит драйверу, специфика — одной
функции `apply` на способность. Отдача уходит к игроку через
`GameEvent.playerImpulse`, поэтому `PlayerApplyExplosionImpulse` удалена, а
`Player` больше не перечисляет существующие powers. Частицы остались одним
fixed-capacity пулом, но visual-роль теперь обновляется через `const World *` и
не может изменить клетку.

Симуляция мира не менялась. Структурные счётчики совпали со всеми десятью
сценариями предыдущего запуска до единицы:

| Scenario | avg ms | cells/tick | chunks/tick |
|---|---:|---:|---:|
| settled world | 0.441 | 0 | 0 |
| falling sand | 1.484 | 47 627 | 46 |
| large water | 3.970 | 74 660 | 72 |
| fire and lava | 2.013 | 40 925 | 39 |
| large explosion | 1.281 | 32 375 | 31 |
| mass destruction | 0.718 | 11 042 | 10 |
| boost drilling | 0.681 | 15 900 | 15 |
| force ability | 1.664 | 36 369 | 35 |
| cryo ability | 3.697 | 75 645 | 73 |
| chaotic mixed | 5.548 | 127 624 | 124 |

Память не изменилась: estimate 221.12 MiB, RSS 220.90 MiB. `AbilitySystem` —
одна структура на игрока, вне cell storage.

Гипотезы «ability simulation смешана с presentation» и «particles смешивают
gameplay и visual effects» подтверждены и закрыты.

## После active chunk scheduler

Гипотеза «simulation всё ещё перебирает слишком много inactive chunks»
подтверждена и закрыта. `WorldUpdate` перебирал chunk-сетку целиком: для
каждой из 864 строк он проходил все 512 колонок, то есть около 442 000 слотов
за tick, даже когда весь мир спал.

Расписание теперь хранится в двух представлениях: флаг на chunk для проверки
принадлежности за O(1) и компактный список активных колонок на каждую строку
chunk-ов для обхода. Списки заполняются только пробуждениями, поэтому спящая
строка не стоит ничего. Порядок обхода сохранён точно: перед проходом строка
сортируется вставками, поэтому она по-прежнему идёт слева направо или справа
налево, а не в порядке поступления пробуждений.

Одно поведенческое изменение зафиксировано намеренно: множество, симулируемое
в этом tick, заморожено в начале `WorldUpdate`, поэтому пробуждение внутри tick
планирует **следующий** tick. Раньше chunk, разбуженный по ходу tick и ещё не
посещённый, мог отработать в том же tick, а разбуженный ниже по обходу — нет;
это была асимметрия, а не инвариант. `updatedTick` по-прежнему гарантирует одно
перемещение клетки за tick, а пробуждение — что chunk отработает. Видно это
только как сдвиг счётчиков на доли процента в сценариях с огнём и лавой.

| Scenario | до avg ms | после avg ms | изменение |
|---|---:|---:|---:|
| settled world | 0.441 | **0.000** | практически исчез |
| falling sand | 1.484 | 1.362 | −8% |
| large water | 3.970 | 3.807 | −4% |
| fire and lava | 2.013 | 1.562 | −22% |
| large explosion | 1.281 | 1.397 | шум p99 |
| mass destruction | 0.718 | 0.316 | −56% |
| boost drilling | 0.681 | 0.298 | −56% |
| force ability | 1.664 | 1.529 | −8% |
| cryo ability | 3.697 | 3.435 | −7% |
| chaotic mixed | 5.548 | 5.536 | −0% |

Выигрыш тем больше, чем меньше активная доля мира, — то есть именно в обычной
игре. Сценарии, где реальная работа с клетками доминирует (water, chaotic),
почти не изменились: там перебор сетки и раньше был малой долей.

Побочно исчезли ещё два полных прохода по сетке за tick: `memset` буфера
next-active и объединение active-флагов в dirty/light-dirty теперь идут по
списку. `activeChunkCount` поддерживается инкрементально, поэтому он точен и
между tick-ами — раньше лазер между двумя tick-ами оставлял HUD с устаревшим
числом.

Память выросла на 4 массива расписания: `chunkCount × 4 B × 2` плюс
`chunkRows × 4 B × 2` — около 0.11 MiB на production world. Estimate 221.12 MiB,
RSS 221.00 MiB.

## После сжатия Cell до 12 bytes

Гипотеза «`Cell` в 16 B доминирует в памяти» подтверждена и закрыта. `Cell`
уплотнена до 12 bytes перестановкой полей и сужением обоих счётчиков-штампов
до 16 bit:

```c
typedef struct Cell {
    float temperature;   /* 0..3  */
    uint16_t updatedTick;/* 4..5  */
    uint16_t effectStamp;/* 6..7  */
    uint16_t lifetime;   /* 8..9  */
    uint8_t material;    /* 10    */
} Cell;                  /* 12, один байт padding */
```

Оба штампа сравниваются на равенство со счётчиком мира, то есть осмысленны
только по модулю своей ширины. Цена сужения ограничена и измерима: клетка, чей
штамп случайно совпал с усечённым счётчиком, пропускает ровно один tick или
ровно один эффект и дальше ведёт себя нормально. Для `updatedTick` это требует,
чтобы клетка простояла бодрствующей и нетронутой ровно кратное 65 536 tick-ов —
восемнадцать минут — и стоит ей одного кадра падения.

Единственная популяция, для которой это было бы заметно, — никогда не
записанные клетки со штампом 0. Поэтому оба счётчика пропускают значение 0:
`WorldUpdate` увеличивает tick ещё раз, если усечённое значение обнулилось, а
`WorldNextEffectStamp` делает то же для серии эффектов.
`test_the_tick_counter_survives_wrapping_its_cell_stamp` прогоняет мир через
переполнение и требует сохранения массы песка и продолжения падения.

Память для 14 155 776 cells:

| Owner | Baseline MiB | Сейчас MiB | Изменение |
|---|---:|---:|---:|
| Cells (16 B → 12 B) | 216.00 | 162.00 | -54.00 |
| Persistent world pixels | 54.00 | 0.00 | -54.00 |
| Chunk metadata и расписание | 0.05 | 0.16 | +0.11 |
| Lighting | 5.06 | 5.06 | 0 |
| **CPU estimate** | **275.12** | **167.22** | **-107.90 MiB (-39%)** |
| Измеренный RSS | ~275 | 167.00 | |

| Scenario | до avg ms | после avg ms | cells/tick |
|---|---:|---:|---:|
| settled world | 0.000 | 0.000 | 0 |
| falling sand | 1.362 | 1.310 | 47 627 |
| large water | 3.807 | 3.976 | 74 660 |
| fire and lava | 1.562 | 1.542 | 40 772 |
| large explosion | 1.397 | 0.912 | 32 375 |
| mass destruction | 0.316 | 0.265 | 11 042 |
| boost drilling | 0.298 | 0.374 | 15 900 |
| force ability | 1.529 | 1.419 | 36 363 |
| cryo ability | 3.435 | 3.370 | 75 593 |
| chaotic mixed | 5.536 | 5.530 | 127 414 |

Структурные счётчики совпали во всех сценариях. Wall-clock в пределах разброса
машины; ускорение не заявляется, хотя `WorldInit` ожидаемо стал вдвое дешевле
(68 → 36 ms), потому что нулевой инициализации подлежит на 54 MiB меньше.

## После render paging

Гипотеза «одна texture шириной 16384 — плохое long-term limitation»
подтверждена и закрыта. Единственная world-texture заменена кэшем страниц
256×256 cells: резидентны только те страницы, которые видит камера.

| | До | После |
|---|---|---|
| Texture objects | 1 × 16384×864 | до 12–24 × 256×256 |
| VRAM для world | ~54.0 MiB, всегда | 0.25 MiB на резидентную страницу |
| Типичный кадр (окно 1280×720) | 54.0 MiB | 4 видимых страницы, 12 слотов = 3.0 MiB |
| Предел ширины мира | `GL_MAX_TEXTURE_SIZE` | память и симуляция |

Кэш растёт под размер вида (окно + zoom) и не уменьшается, поэтому resize
выделяет текстуры один раз, а установившийся рендер не аллоцирует ничего.
Вытеснение — LRU, при этом страница, уже заявленная в этом кадре, вытеснена
быть не может. Панорамирование стоит одной загрузки страницы на переднем крае
вместо чего-либо, пропорционального карте.

Важная деталь корректности: `WorldRenderChunkVisitor` теперь возвращает
`bool`. Chunk, который renderer не смог разместить, **сохраняет** свой dirty
flag и перестраивается в кадре, когда его страница станет резидентной. Иначе он
был бы тихо потерян, и на экране остались бы устаревшие пиксели до следующего
случайного изменения этого chunk — баг, который проявился бы только на машине с
меньшим кэшем страниц, чем у разработчика.
`test_a_refused_chunk_keeps_its_dirty_flag` закрывает это.

Симуляция не менялась; benchmark headless и страниц не создаёт. CPU estimate и
RSS прежние: 167.22 / 167.00 MiB.

## После локализации lighting

`make bench` теперь измеряет и свет: `WorldUpdateLighting` работает целиком на
CPU, GL-контекст ему не нужен. Первое же измерение подтвердило гипотезу
«moving player light вызывает слишком много lighting work» — и с большим
запасом:

| Режим | До | После |
|---|---:|---:|
| ничего не изменилось | 0.022 ms | 0.013 ms |
| движущийся фонарь игрока | **10.189 ms** | **1.732 ms** |
| p95 движущегося фонаря | 11.612 ms | 2.340 ms |
| бурение (терраформинг каждый кадр) | 10.108 ms | 1.649 ms |

10 ms на кадр — это больше всего бюджета кадра при 120 FPS, и платились они
каждый раз, когда игрок сдвигался хотя бы на 4 cells, то есть в полёте —
каждый кадр. Причина: solve шёл по всему полю света 2048×108, при том что
игрок видит около 80 колонок из 2048.

Solve теперь ограничен окном колонок: видимая область плюс запас
`WORLD_LIGHT_WINDOW_MARGIN = 128` световых cells. Открытый воздух пропускает
0.97 на световую cell, поэтому источник за пределами окна приходит к видимому
краю с коэффициентом 0.97¹²⁸ ≈ 2% — ниже шага квантования, с которым сравнивает
renderer, и ниже того, что вообще может показать байт на канал. Sky-канал окно
не портит принципиально: он заполняется проходом по колонке сверху, а колонка
решается независимо от соседей.

Окно входит в условие пересолвинга: если оно сдвинулось, колонки, которых
прошлый solve не касался, устарели.

**Отвергнуто измерением:** разделение каналов. Идея выглядела правильной — sky
меняется только вместе с рельефом, поэтому движущийся фонарь мог бы solve-ить
только ember. Реализовано и измерено: полёт выиграл 0.2 ms, а бурение
подорожало с 1.6 до 3.2 ms, потому что каналы делят между собой поиск
transmission и весь расчёт индексов, а бурение — основной глагол этой игры.
Разделение откачено, причина записана в комментарии рядом с `WorldSolveLight`,
чтобы её не «улучшили» обратно.

Заодно удалён `WorldLightAt`: публичная функция, которую никто не вызывал.

Остаточный долг: `WorldUpdateLighting` всё ещё сканирует все 13 824 флага
light-dirty за кадр (0.013 ms). Это следующий кандидат на компактный список —
тот же приём, что и в active chunk scheduler.
