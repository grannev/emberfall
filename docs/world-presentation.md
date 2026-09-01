# Представление мира

`EnvironmentRenderer` превращает пустое пространство за cellular world в
атмосферный разрушенный индустриальный горизонт. Это только presentation:
модуль хранится внутри `Renderer`, не получает `GameState` или `World`, не
использует gameplay RNG и не влияет на deterministic digest.

## Palettes

| Preset | CLI | Характер |
|---|---|---|
| Ember Waste | `ember` | грязно-тёплый горизонт, холодные тени, orange accents |
| Abyssal Blue | `abyss` | тёмно-синий простор, cyan haze и distant lights |
| Verdigris Storm | `storm` | muted green/grey atmosphere и pale energy accents |

По умолчанию preset выбирается чистой hash-функцией от world seed. Одинаковый
seed всегда даёт одинаковые palette и geometry. `--palette NAME` фиксирует
presentation для screenshots, не меняя seed или simulation.

## Слои и порядок

```text
sky gradient + sparse details
        ↓ 0.8% camera parallax
far peaks / horizon
        ↓ 1.8%
ruined industrial structures
        ↓ 4.5%
near spires + haze
        ↓ 7.5% / 2.8%
translucent EMPTY world pages
        ↓
solid world / TerrainBody / player / abilities / FX
```

Фон рисуется первым в существующем full-resolution `sceneTarget`. Окна и редкие
energy columns повторяются в существующем `emissiveTarget`, поэтому они получают
тот же half-resolution bloom, но обычные sky/haze не светятся. Новых render
targets, shaders или passes нет.

World page остаётся pixel-perfect: material pixels и dynamic `TerrainBody`
используют существующие point-filtered textures. Только `MATERIAL_EMPTY` имеет
depth-dependent alpha 150..220 поверх прежнего cave tint. Это открывает фон в
воздухе и одновременно сильнее гасит его на глубине.

## Камера

Environment geometry строится в screen space. `Camera2D.target` и `zoom`
используются только для parallax, поэтому lookahead и high-speed zoom-out
ощущаются как движение между слоями. Transient rotation/impulse не применяется
как transform полноэкранного фона: gradient имеет overscan и shake не может
открыть пустой угол. Gameplay foreground продолжает использовать обычную
presentation camera, а reticle — отдельную stable aim camera.

## Lifecycle и budgets

- 47 descriptors фиксированного размера: 16 peaks, 8 structures, 5 haze bands,
  12 sky details и 6 near spires;
- каждый descriptor — 20 B, то есть массивы занимают 940 B, весь state — около
  1 KiB;
- максимум текущей сцены — 76 scene и 11 emissive raylib primitive submissions;
- steady-state allocations, texture uploads и full-world scans — 0;
- descriptors перестраиваются только при смене world seed;
- resize меняет только размеры процедурных координат и не создаёт environment
  resources;
- pipeline сохраняет прежние 5 offscreen passes и 4 render targets.

Счётчики palette, validity и submissions находятся в `RendererFrameStats` и
видны в debug HUD. Они считают renderer submissions, а не число фактических GL
batch draw calls.

## Smoke и визуальная проверка

GL smoke последовательно выбирает все три presets, меняет размер окна, включает
camera impulse и принудительный high-speed view scale. Он сохраняет:

```text
build/emberfall-smoke-ember.png
build/emberfall-smoke-abyss.png
build/emberfall-smoke-storm.png
```

Ограничение v1: palette пока не тонирует сами material colors. Это намеренно
сохраняет единую material palette и гарантирует, что foreground, emissive combat
FX и detached terrain остаются визуально согласованными.
