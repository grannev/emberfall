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

GPU uploads, render preparation и lighting solve пока помечены `n/a`: на момент
baseline они находятся внутри `WorldDraw` и требуют GL context. Их счётчики
будут добавлены вместе с отделением renderer, чтобы headless benchmark не
притворялся графическим.

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
  можно только с before/after benchmark и regression tests.
- **Подтверждено как архитектурный limit:** одна texture шириной 16384 зависит
  от GPU maximum texture size и не масштабируется дальше.
- **Частично подтверждено:** sleeping world не обрабатывает cells, но каждый
  tick всё равно сканирует 13 824 chunk flags; это видно по 0,59 ms settled.
- **Не измерено:** цена moving player light, GPU uploads и render preparation.
- **Подтверждено:** generation eager — каждый reset записывает все 14,1 млн
  cells и занимает около 0,47 s.
- **Подтверждено аудитом:** `world.c` совмещает storage, materials, simulation,
  thermal, generation, effects, lighting и renderer; World владеет GPU state.
- **Подтверждено аудитом:** player, abilities и particles смешивают simulation
  с `Draw*`; gameplay RNG зависит от глобального raylib RNG.

Результаты следующих крупных phases добавляются ниже, а baseline не
перезаписывается.
