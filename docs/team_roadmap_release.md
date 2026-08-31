# Emberfall — Team Roadmap to Trailer-Quality Physics Sandbox

> **Назначение:** рабочий документ тимлида для двух параллельных разработчиков:
> **Codex** и **Claude**.
>
> **Технологии фиксированы:** C11 + raylib.
>
> **Референс:** `moltyn-trailer.mp4` используется только как ориентир по уровню
> динамики, читаемости эффектов, разрушению, воде, освещению и game feel.
> Нельзя копировать чужие ассеты, персонажей, UI, уровни, название или lore.

---

## 0. Executive summary

Текущая версия Emberfall уже имеет хорошую инженерную основу:

- `GameState` и явный gameplay update;
- deterministic RNG;
- fixed-step simulation;
- active-chunk scheduler;
- `Cell` = 12 bytes;
- paged world renderer;
- локализованный lighting solve;
- headless tests и benchmark;
- отдельные player/ability/particle renderers;
- fixed-capacity gameplay events;
- отсутствие heap allocations в обычных simulation hot paths.

Поэтому следующая стадия — **не очередной большой архитектурный рефакторинг**.
Нужно наращивать игровые и presentation-системы поверх уже существующих
границ и менять архитектуру только там, где новая механика действительно
требует нового ownership/API.

### Четыре главных разрыва с уровнем референса

1. **Presentation stack.**
   Сейчас мир читаемый, но плоский. В референсе кадр строится из нескольких
   слоёв: яркое emission, мягкое свечение, атмосферный фон, вспышки,
   послесвечение, плотные частицы, camera impulse и контрастная палитра.

2. **Dynamic terrain bodies.**
   В диапазоне примерно `12–18 s` визуально видны отделившиеся куски породы,
   которые существуют как отдельные тела, перемещаются и вращаются. Это
   принципиально другой уровень разрушения, чем просто удаление клеток.

3. **Fluid/destruction feedback.**
   В `6–8 s` и `23–25 s` вода имеет выраженную поверхность, всплески и крупные
   выбросы. В `1–5 s`, `9–10 s`, `19–25 s` разрушение читается одновременно
   через crater, debris, glow, dust, shockwave и изменение света.

4. **Game feel.**
   Высокая скорость читается через trail, camera lead/zoom, вспышки, частицы,
   звук и сильные переходные эффекты. Каждое действие имеет attack → impact →
   decay, а не один визуальный primitive.

---

# 1. Что видно в референсном видео и что из этого следует

Таймкоды приблизительные; это визуальный анализ трейлера, а не утверждение о
внутренней реализации другой игры.

| Время | Наблюдаемое качество | Что нужно Emberfall |
|---|---|---|
| 0–1 s | Атмосферный цветной sky, silhouettes/set dressing, маленький читаемый герой | procedural background, parallax, original ruins/set dressing |
| 1–5 s | Очень яркий impact/comet/beam, большая область разрушения, glowing debris, bloom | multi-pass presentation, emissive mask, bloom, staged explosion/impact FX |
| 6–8 s | Большой объём воды, выраженная поверхность, spray/splash; электрический эффект | water surface presentation, splash emitters, lightning effect/ability |
| 9–10 s | Сильная световая вспышка и разлетающиеся частицы | flash + shock ring + debris + camera impulse |
| 11–18 s | Крупные куски terrain отделяются, летят и вращаются | dynamic terrain body subsystem |
| 16–18 s | Удары/выстрелы по отдельному куску terrain | body collision, body impulse, body damage/fracture |
| 19–21 s | Плотная среда из dust/sand/water particles, beam/shot читается сквозь неё | dense but budgeted particles, depth-aware palette, beam polish |
| 22–25 s | Lightning + water + крупный displacement | lightning gameplay + conductive/water feedback + splash |
| 27–29 s | Даже титр разрушается и рассыпается — демонстрация плотности FX | высокий particle/impact quality bar для showcase |

## Необходимый итог

Emberfall не должен стать копией Moltyn. Целевой результат:

- собственный визуальный язык Emberfall;
- та же **плотность причинно-следственной физической реакции**;
- тот же класс читаемости силы способностей;
- мир, который выглядит физически живым даже в одном скриншоте;
- разрушение, которое интересно наблюдать само по себе.

---

# 2. Team model и git strategy

## 2.1 Main — интеграционная ветка

`main` считается защищённой веткой.

**Только пользователь/тимлид сливает изменения в `main`.**

Агенты:

- не merge-ят свои ветки в `main`;
- не force-push-ят `main`;
- не делают unrelated cleanup во время feature-task;
- не начинают следующую задачу до завершения текущей, если тимлид явно не
  разрешил иначе.

## 2.2 Не работать параллельно в одном worktree

Два агента не должны одновременно менять один root working tree.

Создать постоянные worktree:

```sh
# выполняется каждым агентом только для своего workspace
git worktree add ../emberfall-codex  -b agent/codex/<TASK-ID>-<slug>  main
git worktree add ../emberfall-claude -b agent/claude/<TASK-ID>-<slug> main
```

После merge задачи старую ветку можно удалить, а worktree переключить на новую
task branch, созданную от свежего `main`.

## 2.3 Branch naming

```text
agent/codex/EF-RND-001-render-pipeline
agent/claude/EF-DYN-001-detached-components
```

Формат:

```text
agent/<agent>/<TASK-ID>-<short-slug>
```

## 2.4 Одна ветка = одна атомарная задача

Задача должна быть:

- самостоятельно ревьюваема;
- самостоятельно тестируема;
- mergeable без незаконченного half-feature;
- ограничена одним основным subsystem;
- по возможности не больше ~300–800 net LOC; если значительно больше —
  сначала разбить задачу.

Большой refactor и новая gameplay-механика не должны идти одним task.

## 2.5 Merge policy

Рекомендуемый процесс:

1. Агент создаёт task branch от актуального `main`.
2. Реализует только одну задачу.
3. Запускает Definition of Done.
4. Делает понятные commit(s).
5. Сообщает SHA и короткий handoff.
6. Второй агент делает code review.
7. Автор исправляет `BLOCKER`/`MAJOR`.
8. Reviewer делает короткий re-review.
9. Пользователь squash-merge-ит ветку в `main`.
10. Следующие зависимые задачи создаются уже от обновлённого `main`.

Для `main` предпочтителен **squash merge на одну задачу**, чтобы один commit
соответствовал одному `TASK-ID`.

Пример итогового commit:

```text
feat(render): [EF-RND-001] add offscreen render pipeline
```

## 2.6 Rebase rule

Перед финальным review автор:

```sh
git fetch
git rebase main
```

только если `main` действительно изменился после создания ветки.

Нельзя rebase-ить чужую ветку.

---

# 3. Code review protocol

Каждая задача обязана получить review второго агента.

Reviewer **не исправляет код молча** и не переписывает архитектуру автора.

Review должен проверять:

1. correctness;
2. ownership/lifetime;
3. gameplay invariants;
4. deterministic behaviour;
5. hot-path allocations;
6. performance implications;
7. API size;
8. tests;
9. docs;
10. соответствие task scope.

Severity:

- `BLOCKER` — нельзя merge;
- `MAJOR` — исправить до merge;
- `MINOR` — желательно исправить сейчас;
- `NIT` — style/readability;
- `QUESTION` — требуется объяснение.

Reviewer обязан отдельно написать:

```text
VERDICT: APPROVE
```

или

```text
VERDICT: CHANGES_REQUESTED
```

### Как ревьювать чужую branch без повреждения своей

Можно смотреть diff прямо из текущего worktree:

```sh
git diff main...agent/claude/EF-DYN-001-detached-components
git log --oneline main..agent/claude/EF-DYN-001-detached-components
```

Для запуска чужой ветки создать временный detached worktree:

```sh
git worktree add --detach ../emberfall-review <branch>
cd ../emberfall-review
# build/test
cd -
git worktree remove ../emberfall-review
```

Если peer branch ещё не готова — завершить свою задачу и явно написать:
`READY_FOR_PEER_REVIEW`. Не ждать в фоне и не начинать следующую roadmap-task
самостоятельно.

---

# 4. Global Definition of Done

Для каждой code-task применимо всё релевантное из списка:

```sh
make
make debug
make test
make asan
make ubsan
make bench
```

Для presentation-задач дополнительно:

```sh
xvfb-run -a make run RUN_ARGS=--smoke-test
```

и ручная визуальная проверка обычного запуска, когда display доступен.

Требования:

- zero new compiler warnings;
- zero sanitizer findings;
- no unrelated formatting/rewrite;
- no heap allocations in established steady-state frame/tick paths без
  отдельного обоснования;
- deterministic gameplay не должен зависеть от presentation RNG;
- public API минимален;
- при изменении behaviour есть regression tests;
- при изменении architecture обновлена соответствующая документация;
- при performance-sensitive изменении есть before/after measurement;
- roadmap-файл не переписывается агентом без отдельной задачи.

---

# 5. Архитектурные правила следующей стадии

## 5.1 Simulation остаётся headless

Не возвращать `Draw*`, `Texture2D`, shader state и window state в gameplay/world.

## 5.2 Presentation может быть богатой

Renderer может владеть:

- RenderTexture2D;
- shaders;
- emissive masks;
- post-process targets;
- visual-only particles;
- trails;
- screen-space effects.

## 5.3 Dynamic terrain — отдельный тип объектов

Нельзя превращать каждую cell в Entity/RigidBody.

Предполагаемая модель:

```text
Static cellular World
        +
DynamicTerrainSystem
   └── TerrainBody[N]
        +
Player / future entities
```

`TerrainBody` — это крупный связный фрагмент клеточного terrain, а не одна cell.

## 5.4 Никакого generic engine внутри Emberfall

Не писать:

- generic ECS;
- generic render graph framework;
- generic physics engine;
- generic scripting VM;
- generic event bus.

Пишем ровно то, что нужно игре.

## 5.5 Third-party dependency

Новая dependency допускается только через отдельный ADR/task, если:

- она permissive/open-source;
- реально экономит сложный код;
- не превращает cellular world в чужую object model;
- имеет понятную стоимость build/support.

Для первых milestones новых dependencies не требуется.

---

# 6. Quality gates / milestones

## Milestone A — Visual floor

Считается закрытым, когда:

- игра рисуется через offscreen presentation pipeline;
- emissive objects имеют controlled glow;
- есть atmospheric sky/parallax;
- explosion/laser/boost выглядят многослойно;
- camera feedback читается без раздражающего random jitter;
- normal frame не создаёт heap allocations.

## Milestone B — Dynamic destruction

Закрыт, когда:

- небольшой полностью отделённый solid cluster может стать `TerrainBody`;
- body перемещается и вращается;
- рисуется pixel-perfect;
- сталкивается со static world;
- может получить impulse от explosion/force;
- имеет budgets/sleep/culling;
- не повреждает deterministic cellular simulation.

## Milestone C — Fluid spectacle

Закрыт, когда:

- water визуально имеет поверхность;
- сильный impact создаёт splash/spray;
- движение больших объёмов читается на расстоянии;
- water остаётся cellular gameplay state, visual foam/spray — presentation.

## Milestone D — Ability spectacle

Закрыт, когда:

- explosion, laser, force, cryo имеют attack/impact/decay presentation;
- есть оригинальная lightning ability;
- есть один крупный projectile/meteor-like impact;
- dynamic terrain bodies получают воздействие способностей.

## Milestone E — World identity

Закрыт, когда:

- 2–3 оригинальные environment palettes/biomes;
- procedural background;
- original industrial/ruin silhouettes или аналогичный set dressing;
- ambient/emissive world details;
- кадр Emberfall узнаваем без HUD.

## Milestone F — Showcase-ready

Закрыт, когда:

- deterministic showcase scenes;
- automated screenshots;
- 10-minute soak;
- perf budgets соблюдаются;
- debug overlay выключается для capture;
- минимум 5 сцен демонстрируют разные physics verbs.

---

# 7. Task backlog

Ниже задачи уже разрезаны так, чтобы их можно было выдавать отдельно.

---

## RENDER / PRESENTATION

### EF-RND-001 — Offscreen render pipeline foundation
**Priority:** P0  
**Preferred owner:** Codex  
**Dependencies:** none

Цель: перестать рисовать финальный world-space кадр напрямую в backbuffer и
создать минимальную основу для post-processing.

Scope:

- `Renderer` владеет scene `RenderTexture2D`;
- `Renderer` владеет отдельной emissive `RenderTexture2D`;
- корректный resize/recreate;
- steady-state без allocation;
- current world/player/abilities/particles всё ещё выглядят максимально
  эквивалентно до включения post-FX;
- backbuffer получает итоговый scene texture с правильным raylib Y-flip;
- emissive target пока может содержать только явно выбранные существующие
  emissive элементы или оставаться инфраструктурным контрактом;
- gameplay state не меняется.

Out of scope:

- bloom;
- new abilities;
- terrain bodies;
- background redesign.

Acceptance:

- release/debug/test/sanitizers/smoke pass;
- resize не ломает изображение;
- нет per-frame `LoadRenderTexture`;
- `RendererUnload` освобождает все GPU resources;
- architecture docs кратко описывают новый ownership.

---

### EF-RND-002 — Bloom + emissive composite
**Priority:** P0  
**Preferred owner:** Codex  
**Dependencies:** EF-RND-001

Добавить маленький специализированный post-process:

- emissive extraction/explicit emissive pass;
- separable blur или другой дешёвый bloom;
- controlled additive composite;
- tunable threshold/intensity/radius;
- graceful fallback, если shader не загрузился;
- pixel art остаётся резким, blur применяется к glow, а не ко всему scene.

Acceptance:

- lava/fire/laser/boost/emissive particles читаются ярче;
- обычный terrain не становится мыльным;
- resize работает;
- измерены CPU-side cost и количество passes/targets;
- shader sources лежат в понятном `assets/shaders/` или аналогичном каталоге.

---

### EF-RND-003 — Color grading + flash/vignette hooks
**Priority:** P1  
**Preferred owner:** Codex  
**Dependencies:** EF-RND-002

Лёгкий final composite:

- exposure;
- saturation/contrast;
- event-driven white/orange flash;
- optional subtle vignette;
- никаких постоянных тяжёлых full-screen effects без budget.

---

### EF-RND-004 — Distortion/heat-haze pass
**Priority:** P2  
**Preferred owner:** Codex  
**Dependencies:** EF-RND-002, EF-FX-002

Локальный heat/distortion mask для lava, fresh drilling, explosion.

---

## CAMERA / GAME FEEL

### EF-CAM-001 — Deterministic camera impulse stack
**Priority:** P0  
**Preferred owner:** Codex  
**Dependencies:** EF-RND-001

Заменить один `cameraShake float` на presentation-only impulse system:

- positional trauma;
- rotational impulse;
- short zoom kick;
- exponential decay;
- event strength;
- deterministic/noisy presentation seed отдельно от gameplay RNG;
- stacking without unbounded shake.

Explosion, impact, boost-stage, body impact могут публиковать параметры.

---

### EF-CAM-002 — High-speed camera behaviour
**Priority:** P1  
**Preferred owner:** Codex  
**Dependencies:** EF-CAM-001, EF-PLY-002

- speed-dependent lookahead;
- smooth zoom-out;
- directional lead;
- fast reversal damping;
- no motion sickness / no jitter.

---

## VISUAL EFFECTS

### EF-FX-001 — Presentation FX manager
**Priority:** P0  
**Preferred owner:** Codex  
**Dependencies:** EF-RND-001

Небольшой fixed-capacity presentation subsystem для transient visual effects:

- rings;
- flashes;
- arcs;
- trails;
- shockwaves;
- short-lived sprites/primitives.

Не generic particle engine. Gameplay events создают visual instances.

---

### EF-FX-002 — Staged explosion presentation
**Priority:** P0  
**Preferred owner:** Codex  
**Dependencies:** EF-FX-001, EF-RND-002

Explosion = последовательность:

1. instant flash;
2. bright core;
3. expanding ring;
4. debris/sparks;
5. dust/smoke;
6. fading glow;
7. camera impulse.

Gameplay crater/shockwave не переписывать без отдельной задачи.

---

### EF-FX-003 — Laser presentation pass
**Priority:** P1  
**Preferred owner:** Codex  
**Dependencies:** EF-RND-002, EF-FX-001

- bright core;
- colored halo;
- impact flare;
- sparks;
- heated contact point;
- stable thickness at zoom.

---

### EF-FX-004 — Boost trail / Mach cone polish
**Priority:** P1  
**Preferred owner:** Codex  
**Dependencies:** EF-FX-001, EF-RND-002

- persistent short trail;
- stage-specific exhaust;
- clearer Mach transition;
- speed lines/afterimage only when useful;
- bounded fixed-capacity storage.

---

### EF-FX-005 — Procedural lightning renderer
**Priority:** P1  
**Preferred owner:** Codex  
**Dependencies:** EF-FX-001

Renderer for deterministic-looking branched arcs:

- polyline segments;
- branches;
- core + glow;
- short persistence;
- target endpoints supplied by gameplay.

Это presentation half будущей lightning ability.

---

## DYNAMIC TERRAIN

### EF-DYN-001 — Detached solid component detector
**Priority:** P0  
**Preferred owner:** Claude  
**Dependencies:** none

Создать **headless, behaviour-neutral** foundation.

Нужен алгоритм, который на маленьком/ограниченном регионе умеет:

- определить связные solid components;
- отличить component, которая явно связана с anchored terrain;
- вернуть bounds, cell count и component cells через caller-owned/fixed
  workspace;
- корректно работать через simulation chunk boundaries;
- не делать heap allocation в query hot path;
- безопасно сказать `unknown/too_large`, а не сканировать весь production world.

На этой задаче **ничего ещё не отрывается от мира**.

Добавить tests на:

- single island;
- two islands;
- bridge intact → anchored;
- bridge destroyed → detached;
- component crossing chunk boundary;
- region boundary → conservative anchored/unknown;
- capacity overflow is safe.

---

### EF-DYN-002 — TerrainBody data model + manager
**Priority:** P0  
**Preferred owner:** Claude  
**Dependencies:** EF-DYN-001

Создать fixed-capacity `DynamicTerrainSystem`.

`TerrainBody` минимум:

- active/sleep state;
- local cell/pixel mask or compact raster;
- local bounds;
- world position;
- angle;
- linear velocity;
- angular velocity;
- mass/inertia estimate;
- material data needed for rendering/interactions.

Пока body не создаётся автоматически из World.

No heap allocation during update.

---

### EF-DYN-003 — Atomic extraction World → TerrainBody
**Priority:** P0  
**Preferred owner:** Claude  
**Dependencies:** EF-DYN-001, EF-DYN-002

По explicit call:

1. validate detached component;
2. copy required cells into body;
3. clear original world cells;
4. wake/dirty affected chunks;
5. preserve material/temperature where practical;
6. rollback/no-op on capacity failure.

Добавить headless tests mass/material conservation.

---

### EF-DYN-004 — TerrainBody kinematics
**Priority:** P0  
**Preferred owner:** Claude  
**Dependencies:** EF-DYN-002

- translation;
- angular motion;
- configurable gravity/drag;
- sleep threshold;
- deterministic fixed-step integration;
- no world collision yet.

---

### EF-DYN-005 — TerrainBody renderer
**Priority:** P0  
**Preferred owner:** Codex  
**Dependencies:** EF-DYN-002, EF-RND-001

Pixel-perfect render of dynamic fragments:

- local texture/cache owned by presentation;
- rotation around body centre;
- nearest filtering;
- lighting/emissive approximation compatible with existing palette;
- GPU lifetime detached from simulation ownership.

---

### EF-DYN-006 — Body vs static world collision
**Priority:** P0  
**Preferred owner:** Claude  
**Dependencies:** EF-DYN-003, EF-DYN-004

Начать с robust conservative collision, не с идеальной физики:

- broad phase bounds;
- sampled/mask narrow phase;
- positional correction;
- bounce/friction;
- angular response where stable;
- no tunnelling at expected speeds.

---

### EF-DYN-007 — Player vs TerrainBody collision
**Priority:** P1  
**Preferred owner:** Claude  
**Dependencies:** EF-DYN-006

Player не проходит через fragment; сильный impact публикует event.

---

### EF-DYN-008 — Abilities apply impulse to TerrainBody
**Priority:** P0  
**Preferred owner:** Claude  
**Dependencies:** EF-DYN-004

Explosion и force получают body query/impulse path.

Не связывать `abilities.c` с renderer.

---

### EF-DYN-009 — Fracture / split on severe damage
**Priority:** P1  
**Preferred owner:** Claude  
**Dependencies:** EF-DYN-003, EF-DYN-006, EF-DYN-008

Body можно повредить, после чего connectivity пересчитывается и fragment
делится на 2+ bodies в рамках budgets.

---

### EF-DYN-010 — TerrainBody budgets, sleep, culling
**Priority:** P0  
**Preferred owner:** Claude  
**Dependencies:** EF-DYN-004

Hard limits:

- max bodies;
- max total raster cells;
- max awake bodies;
- safe eviction/reintegration/despawn policy;
- counters in debug HUD/benchmark.

---

### EF-DYN-011 — Automatic detach trigger after destructive events
**Priority:** P1  
**Preferred owner:** Claude  
**Dependencies:** EF-DYN-003, EF-DYN-010

После explosion/drill/other structural cut проверять только bounded changed
region и автоматически извлекать небольшие полностью detached components.

Не запускать flood-fill всего мира после каждой cell mutation.

---

## WATER / FLUID PRESENTATION

### EF-FLD-001 — Water surface classification for renderer
**Priority:** P1  
**Preferred owner:** Claude  
**Dependencies:** none

CPU-side cheap query/metadata for visible water:

- surface cell;
- body/interior cell;
- local depth estimate or small category;
- no change to cellular simulation.

---

### EF-FLD-002 — Water depth/surface shading
**Priority:** P1  
**Preferred owner:** Codex  
**Dependencies:** EF-FLD-001, EF-RND-002

- brighter surface;
- deeper/cooler interior;
- subtle emissive/specular-like crest;
- preserve pixel aesthetic.

---

### EF-FLD-003 — Splash/foam emitters
**Priority:** P1  
**Preferred owner:** Codex  
**Dependencies:** EF-FLD-001, EF-FX-001

Visual-only foam/spray generated from:

- cells entering/leaving surface;
- player/body impact;
- explosion/force.

Budgeted and fixed-capacity.

---

### EF-FLD-004 — Large impact fluid displacement
**Priority:** P1  
**Preferred owner:** Claude  
**Dependencies:** EF-DYN-008 or existing shockwave path

Tune/add bounded world impulse so a strong event visibly throws a large sheet of
water without repeatedly moving one cell in the same effect.

---

### EF-FLD-005 — Player submersion behaviour
**Priority:** P2  
**Preferred owner:** Claude  
**Dependencies:** EF-PLY-002

- drag;
- optional buoyancy;
- entry/exit events;
- no health system required.

---

## PLAYER

### EF-PLY-001 — Original procedural character readability pass
**Priority:** P1  
**Preferred owner:** Codex  
**Dependencies:** EF-RND-002

Сделать Emberfall hero более читаемым в motion, не копируя референс:

- clear silhouette;
- aim direction;
- velocity lean;
- boost pose;
- recoil pose;
- small emissive accents.

---

### EF-PLY-002 — High-speed flight feel pass
**Priority:** P1  
**Preferred owner:** Claude  
**Dependencies:** none

Не просто увеличить max speed.

Исследовать/tune:

- acceleration curve;
- turning authority vs speed;
- momentum;
- braking;
- boost transitions;
- collision response;
- tunnel/drill feel.

Behaviour changes покрыть tests where deterministic.

---

### EF-PLY-003 — Impact events with strength/material
**Priority:** P1  
**Preferred owner:** Claude  
**Dependencies:** none

GameEvent impact payload:

- strength;
- normal;
- material;
- position.

Presentation/audio получают достаточно данных для richer feedback.

---

## ABILITIES

### EF-ABL-001 — Lightning gameplay ability
**Priority:** P1  
**Preferred owner:** Claude  
**Dependencies:** EF-FX-005

Оригинальная electric ability:

- deterministic gameplay target selection;
- short range/chain rules;
- interaction with water designed explicitly;
- event carries arc endpoints;
- renderer only visualizes supplied path;
- cooldown/tuning in ability registry.

Не превращать всю воду в глобальную мгновенную conductive graph simulation.

---

### EF-ABL-002 — Meteor / plasma impact ability
**Priority:** P1  
**Preferred owner:** Claude  
**Dependencies:** EF-FX-002

Крупный projectile/strike:

- travel phase;
- visible world impact;
- heat;
- crater;
- shockwave;
- terrain body impulse.

Projectile должен быть entity-like gameplay object, не cell.

---

### EF-ABL-003 — Force + TerrainBody integration
**Priority:** P1  
**Preferred owner:** Claude  
**Dependencies:** EF-DYN-008

Силовой удар должен убедительно толкать большие detached fragments.

---

### EF-ABL-004 — Explosion gameplay scale pass
**Priority:** P1  
**Preferred owner:** Claude  
**Dependencies:** EF-DYN-011, EF-FLD-004

Сделать несколько параметризованных blast profiles вместо одного визуально
одинакового взрыва:

- normal;
- heavy;
- showcase/large.

Не создавать новый ability framework.

---

### EF-ABL-005 — Laser impact/thermal tuning
**Priority:** P2  
**Preferred owner:** Claude  
**Dependencies:** EF-FX-003

Подкрутить gameplay-side burn/drill/heat feedback только после visual upgrade.

---

## ENVIRONMENT

### EF-ENV-001 — Procedural sky/parallax
**Priority:** P0  
**Preferred owner:** Codex  
**Dependencies:** EF-RND-001

Presentation-only:

- gradient sky;
- star/noise field;
- distant clouds/haze;
- 2–3 parallax depth layers;
- seed stable within session;
- no borrowed assets.

---

### EF-ENV-002 — Environment palette zones
**Priority:** P1  
**Preferred owner:** Codex  
**Dependencies:** EF-ENV-001

2–3 original visual zones using world position/biome metadata:

- ember dusk;
- cold night;
- deep cavern/industrial haze.

Gameplay materials remain same unless separate task says otherwise.

---

### EF-ENV-003 — Procedural ruin/set-dressing primitives
**Priority:** P1  
**Preferred owner:** Claude  
**Dependencies:** none

World generation of original non-Moltyn silhouettes:

- towers;
- broken frames;
- pipes/arches;
- sparse structures.

Сначала static terrain/material composition; никакого asset system ради пары
форм.

---

### EF-ENV-004 — Emissive terrain accents
**Priority:** P2  
**Preferred owner:** Claude  
**Dependencies:** EF-RND-002

Original world details feeding existing ember/emission path.

---

## AUDIO

### EF-AUD-001 — Audio event parameterization
**Priority:** P1  
**Preferred owner:** Codex  
**Dependencies:** EF-PLY-003

Существующие procedural sounds получают параметры:

- event strength;
- distance;
- material;
- pitch/variation from presentation RNG.

Не затрагивать gameplay RNG.

---

### EF-AUD-002 — Layered explosion/boost/body impacts
**Priority:** P1  
**Preferred owner:** Codex  
**Dependencies:** EF-AUD-001

Layer attack/body/tail rather than single flat sound.

---

### EF-AUD-003 — Ambient soundscape
**Priority:** P2  
**Preferred owner:** Codex  
**Dependencies:** EF-ENV-002

Low-cost procedural/loop ambience tied to environment.

---

### EF-AUD-004 — Water and debris impact audio
**Priority:** P2  
**Preferred owner:** Codex  
**Dependencies:** EF-FLD-003, EF-DYN-007

---

## TOOLING / QA / PERFORMANCE

### EF-QA-001 — Deterministic showcase capture CLI
**Priority:** P0  
**Preferred owner:** Claude  
**Dependencies:** none

Добавить reproducible capture mode поверх существующей seeded infrastructure.

Пример идеи:

```sh
make run RUN_ARGS="--showcase explosion --capture-frame 180"
```

Минимум сцены:

- explosion;
- water;
- boost;
- laser.

Capture не должен требовать ручного input.

---

### EF-QA-002 — Visual screenshot gallery
**Priority:** P1  
**Preferred owner:** Codex  
**Dependencies:** EF-QA-001, EF-RND-002

Скрипт/target создаёт несколько PNG для ручного сравнения.

Не вводить хрупкий pixel-perfect CI threshold для bloom/shader output без
отдельной причины.

---

### EF-PERF-001 — Presentation performance counters
**Priority:** P1  
**Preferred owner:** Codex  
**Dependencies:** EF-RND-002

HUD/telemetry:

- render target size;
- resident pages;
- post-process passes;
- FX active count;
- terrain body draw count;
- uploaded bytes;
- CPU preparation times.

---

### EF-PERF-002 — Dynamic terrain benchmark
**Priority:** P0  
**Preferred owner:** Claude  
**Dependencies:** EF-DYN-004

Headless scenarios:

- 1 body;
- 16 bodies;
- maximum expected awake bodies;
- body extraction;
- collision workload later.

No wall-clock assertion in tests.

---

### EF-QA-003 — 10-minute deterministic soak
**Priority:** P1  
**Preferred owner:** Claude  
**Dependencies:** EF-DYN-011, EF-ABL-001

Scripted inputs/actions. Проверять:

- no crash;
- fixed capacities;
- body counts;
- active chunk counts;
- deterministic digest.

---

### EF-QA-004 — Final sanitizer + leak gate
**Priority:** P0 before milestone release  
**Preferred owner:** reviewer of release candidate

ASan/UBSan + repeated init/reset/unload + renderer resize cycles.

---

## SHOWCASE

### EF-SHOW-001 — Five deterministic showcase scenes
**Priority:** P1  
**Preferred owner:** Claude  
**Dependencies:** Milestones A–D

Сцены:

1. high-speed drilling;
2. heavy explosion detaching terrain;
3. force throwing a detached island;
4. lightning over water;
5. meteor/plasma impact.

---

### EF-SHOW-002 — Presentation polish pass
**Priority:** P1  
**Preferred owner:** Codex  
**Dependencies:** EF-SHOW-001

На этих сценах сделать финальный tuning:

- palette;
- bloom;
- camera;
- particle budgets;
- readability;
- no debug HUD.

---

# 8. Рекомендуемый порядок двух параллельных дорожек

Главная идея — первые недели держать специализацию, чтобы уменьшить merge
conflicts:

```text
CODEX LANE                       CLAUDE LANE
presentation/render/audio        simulation/world/gameplay
----------------------------------------------------------------
RND-001                          DYN-001
RND-002                          DYN-002
FX-001                           DYN-003
ENV-001                          DYN-004
DYN-005                          DYN-006
CAM-001                          DYN-010
FX-002                           DYN-008
FLD-002                          FLD-001
FLD-003                          FLD-004
FX-005                           ABL-001
FX-003                           ABL-002
PLY-001                          PLY-002 / PLY-003
AUD-001/002                      DYN-009/011
QA-002/PERF-001                  PERF-002/QA-003
SHOW-002                         SHOW-001
```

Каждая следующая строка не означает обязательный календарный sprint.
Dependency из task description важнее таблицы.

---

# 9. Что НЕ делать в ближайших milestones

Не тратить время на:

- новый язык/движок;
- ECS rewrite;
- networking;
- inventory;
- quests;
- save format до появления реальной потребности;
- editor;
- scripting language;
- Steam integration;
- generic asset manager;
- procedural animation framework;
- multithreaded cellular simulation;
- infinite world до решения dynamic terrain/presentation quality.

Эти вещи не сокращают текущий визуальный и физический разрыв с target.

---

# 10. Первые две задачи

## Codex — EF-RND-001

Branch:

```text
agent/codex/EF-RND-001-render-pipeline
```

Зачем первой:

Все дальнейшие bloom, flash, distortion, color grading и выразительный
emission требуют корректного offscreen pipeline. Это presentation-only работа и
почти не пересекается с первой задачей Claude.

## Claude — EF-DYN-001

Branch:

```text
agent/claude/EF-DYN-001-detached-components
```

Зачем первой:

Самая глубокая physics-разница с референсом — независимые вращающиеся куски
ландшафта. Сначала нужен безопасный headless detector, который ничего не меняет
в gameplay. Это даёт foundation без giant rewrite.

### Merge order

Эти задачи независимы. После peer review их можно merge в любом порядке.

После обоих merge:

- Codex получает `EF-RND-002`;
- Claude получает `EF-DYN-002`.

---

# 11. Team lead acceptance for first pair

## EF-RND-001

Я не принимаю задачу, если:

- gameplay code начинает зависеть от RenderTexture/Shader;
- RenderTexture пересоздаётся каждый frame;
- resize течёт;
- smoke screenshot перестаёт создаваться;
- output явно теряет pixel crispness без причины;
- branch одновременно добавляет bloom/новый background/новые abilities.

## EF-DYN-001

Я не принимаю задачу, если:

- алгоритм может случайно flood-fill 14 млн cells из hot path;
- для каждого query делается malloc/free;
- найденная component сразу удаляется из World;
- anchored/unknown случай трактуется как detached;
- detector зависит от renderer/raylib GPU;
- нет chunk-boundary tests.

---

# 12. Long-term performance budgets

Это не жёсткие CI asserts, а engineering targets.

На машине класса текущего baseline:

- fixed simulation typical: желательно < 4 ms;
- chaotic cellular case: желательно < 6 ms;
- moving light: ~2 ms или лучше;
- steady-state presentation CPU prep: < 2 ms;
- no unbounded GPU upload growth;
- no steady-state heap allocations;
- dynamic bodies: hard configured cap;
- visual FX: hard configured cap;
- 120 FPS target в обычной сцене на разумном desktop GPU;
- 60 FPS minimum target в showcase chaos после tuning.

Если эффект требует 10+ ms сам по себе, его архитектуру надо пересматривать,
а не просто скрывать за красивым кадром.

---

# 13. Когда менять этот roadmap

Roadmap — направление, не религия.

Можно изменить задачу/порядок, если:

- profiling опроверг предположение;
- API предыдущего task делает следующий ненужным;
- gameplay test показывает плохой feel;
- review находит более простой путь;
- новая feature требует ADR.

Но изменение должно быть **явным**: причина записывается в task handoff или ADR,
а не возникает как случайный scope creep внутри ветки.
