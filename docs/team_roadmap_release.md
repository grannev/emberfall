# Emberfall — team roadmap release

> Актуализировано по состоянию проекта на 2026-09-01.
>
> Стек: **C11 + raylib**.
>
> Команда: **Claude** и **Codex**.
>
> Главная цель: не наращивать инфраструктуру ради инфраструктуры, а доводить
> Emberfall крупными вертикальными срезами до состояния, где каждая новая задача
> заканчивается реально заметной игровой возможностью.

---

# 1. Новый принцип разработки

Предыдущий этап был намеренно разбит на маленькие архитектурные задачи.

Это дало хороший фундамент, но оказалось слишком дорогим по времени и лимитам:
на каждую маленькую задачу повторялись setup, tests, sanitizers, benchmark,
docs, PR и handoff.

С этого момента:

## Одна roadmap-задача = одна законченная feature

Агент сам внутри задачи:

- делит работу на внутренние этапы;
- делает несколько commits;
- запускает релевантные проверки;
- при необходимости делает небольшие refactors;
- доводит feature до playable/visible состояния;
- открывает один PR на всю feature.

Не открывать отдельный PR на каждый helper/subsystem, если они являются частью
одной законченной механики.

---

# 2. Фактическое состояние проекта

Ниже — foundation, который уже присутствует в актуальном коде.

## Готово: core / world

- fixed-step cellular simulation;
- deterministic RNG;
- active/sleeping chunks;
- paged world renderer;
- explicit emissive material rendering;
- localized lighting architecture;
- GameState/GameEvents boundary.

## Готово: dynamic terrain foundation

Есть отдельные модули:

```text
dynamic_terrain.*
world_components.*
terrain_extraction.*
terrain_detach.*
terrain_physics.*
terrain_impulse.*
terrain_body_render_data.*
terrain_body_renderer.*
```

Фактически реализованы:

- bounded connected-component detector;
- fixed-capacity `DynamicTerrainSystem`;
- stable TerrainBody handles/generations;
- material + temperature raster storage;
- mass;
- center of mass;
- inertia;
- World → TerrainBody atomic extraction;
- automatic detachment after destructive world mutations;
- deterministic linear/angular kinematics;
- gravity/damping;
- sleeping/waking;
- TerrainBody ↔ static World collision;
- anti-tunnelling/substeps;
- collision contacts;
- hard body/cell/awake budgets;
- out-of-world cleanup;
- scene/emissive TerrainBody renderer;
- cached body textures;
- rotation around simulation COM;
- explosion/force impulses;
- mass/inertia-sensitive impulse response.

**Это больше не infrastructure TODO. Это готовый фундамент, который теперь
нужно превратить в полноценный gameplay.**

## Готово: presentation foundation

Есть:

- renderer-owned offscreen scene target;
- explicit emissive target;
- half-resolution bloom ping/pong;
- bloom shaders;
- sharp-scene + additive blurred-emissive composite;
- graceful bloom fallback;
- scene/emissive world pages;
- scene/emissive TerrainBody rendering;
- scene/emissive player/abilities/particles;
- fixed-capacity `PresentationFxSystem`;
- event-driven transient visual FX;
- renderer telemetry.

## Сделано частично

### Dynamic terrain gameplay

TerrainBody уже:

- появляется;
- падает;
- вращается;
- сталкивается со static World;
- получает explosion/force impulse;
- рендерится.

Но полноценной player-facing механики ещё нет:

- player ↔ TerrainBody collision;
- natural pushing;
- grab/drag/carry;
- throw;
- damage/carving dynamic body;
- fracture/splitting dynamic body.

### Combat presentation

Foundation есть, но качество текущих abilities ещё не считается законченным
vertical slice уровня target:

- explosion;
- laser;
- force;
- cryo;
- boost/drill;
- camera feedback;
- layered audio.

Их нужно довести как единый presentation/game-feel pass.

## Пока не является готовой крупной feature

- dynamic terrain interaction руками игрока;
- fracture dynamic terrain;
- spectacular water presentation;
- lightning ability;
- high-speed movement full polish;
- atmospheric world identity/biomes;
- full impact/audio pass;
- showcase/release scenes.

---

# 3. Git workflow

## Branch naming

```text
agent/<agent>/<TASK-ID>-<slug>
```

Примеры:

```text
agent/claude/EF-PHY-001-dynamic-terrain-gameplay-v1
agent/codex/EF-VFX-001-combat-presentation-v1
```

## Один vertical slice = один PR

Внутри branch агент может делать несколько commits.

Агенты:

- сами commit;
- сами push;
- сами открывают PR;
- не merge-ят `main`.

Merge выполняет пользователь.

## Если предыдущий PR ещё не merge-нут

Допускается stacked branch.

В PR явно указать dependency.

После merge parent branch — rebase/update base при необходимости.

---

# 4. Review workflow

Review остаётся lightweight.

Reviewer не обязан повторять tests автора.

Он быстро смотрит diff и ищет:

- correctness bugs;
- unsafe memory/lifetime;
- ownership violations;
- deterministic gameplay regressions;
- render/gameplay coupling;
- unbounded work;
- hot-path allocations;
- architectural dead ends;
- scope creep.

Комментарии оставляются прямо в PR.

Автор в конце каждой feature проверяет новые comments и исправляет существенные.

---

# 5. Verification policy

## Во время работы

Обычно:

```sh
make
```

плюс релевантные subsystem tests.

Не запускать полный suite после каждого внутреннего commit.

## Перед PR

Обязательно:

```sh
make
make test
```

Плюс manual/deterministic acceptance самой feature.

## ASan / UBSan

Только если feature затрагивает:

- pointers;
- memory ownership;
- pools;
- raster mutation;
- collision;
- dynamic storage;
- GPU resource lifetime.

## Benchmark

Только если feature меняет:

- simulation hot path;
- collision workload;
- body counts;
- world scans;
- render passes;
- GPU uploads;
- particle/FX budgets.

## Smoke test

Для end-to-end gameplay/rendering feature:

```sh
xvfb-run -a make run RUN_ARGS=--smoke-test
```

если среда позволяет.

## Полный release gate

Полный набор:

```sh
make
make debug
make test
make asan
make ubsan
make bench
xvfb-run -a make run RUN_ARGS=--smoke-test
```

запускается на milestone/release integration, а не на каждый внутренний шаг.

---

# 6. Архитектурные правила

## Cellular World остаётся специализированной simulation

Не превращать cells в ECS entities.

## TerrainBody = крупный connected fragment

Большие полностью отделённые куски должны оставаться допустимыми физическими
телами.

Их тяжесть определяется:

- mass;
- inertia;
- collision;
- strength игровых воздействий.

Не запрещать большие тела искусственным маленьким size threshold только потому,
что они большие.

## Simulation остаётся headless

Gameplay не должен владеть:

- Texture2D;
- RenderTexture2D;
- Shader;
- Draw* state.

## Presentation не меняет gameplay

Направление:

```text
GameState + GameEvents
        ↓
Renderer / PresentationFx / Audio
```

## Не строить новый generic engine

Без отдельной реальной причины не нужны:

- generic ECS;
- generic rigid-body engine;
- generic render graph;
- scripting VM;
- plugin framework;
- generic event bus.

---

# 7. Ближайшие две крупные задачи

---

## EF-PHY-001 — Dynamic Terrain Gameplay v1

**Owner:** Claude  
**Priority:** P0

### Цель

Превратить уже работающий dynamic-terrain foundation в полноценную игровую
механику.

После feature игрок должен уметь:

```text
вырезать connected fragment
→ fragment отделяется
→ падает / вращается
→ player сталкивается с ним
→ может толкать
→ может захватить / тащить / переносить
→ может отпустить / бросить
→ explosion / force продолжают воздействовать
→ dynamic body можно локально разрушить
→ сильное разрушение может расколоть body
```

### Входит

#### Player ↔ TerrainBody collision

- player не проходит сквозь body;
- body может блокировать движение;
- player может приземлиться/опереться, если это устойчиво с текущей моделью;
- столкновение может передавать bounded impulse;
- масса body влияет на ощущение.

#### Push

Маленькие тела естественно сдвигаются легче больших.

#### Grab / drag / carry

Нужен простой оригинальный interaction:

- выбрать nearby body;
- захватить world/local grab point;
- тянуть к aim/hold target ограниченной силой;
- никакого teleport;
- collision с World продолжает работать;
- mass/inertia влияют на управление;
- слишком тяжёлый body двигается медленно, а не запрещается автоматически.

#### Throw

Release сохраняет momentum и/или добавляет bounded aim impulse.

#### Dynamic body damage

Existing destructive mechanics должны уметь локально удалить клетки из
TerrainBody raster.

Минимум использовать те mechanics, которые естественно подходят:

- explosion;
- laser/drill.

Не переносить полную cellular simulation внутрь moving body.

#### Fracture v1

После meaningful raster damage:

```text
TerrainBody raster
→ bounded connectivity recompute
→ connected components
→ original/new TerrainBody objects
```

Нужны:

- material preservation;
- transform conversion;
- mass/COM/inertia recompute;
- velocity inheritance;
- budgets;
- deterministic component ordering.

Не нужен stress solver.

### Acceptance scene

Feature не закончена без сцены:

```text
large platform
→ cut support
→ platform detaches
→ player pushes it
→ grabs and carries/drags it
→ throws/releases it
→ explosion spins it
→ laser/explosion breaks it
→ it fractures into several moving pieces
```

---

## EF-VFX-001 — Combat Presentation & Game Feel v1

**Owner:** Codex  
**Priority:** P0

### Цель

Используя уже готовые bloom/emissive/PresentationFx systems, сделать current
gameplay визуально и тактильно значительно сильнее.

В одной feature довести:

- explosion;
- laser;
- force;
- cryo;
- boost/drilling;
- camera feedback;
- соответствующий audio feedback.

### Explosion

Полный staged effect:

```text
flash
→ bright core
→ shock ring
→ sparks/debris
→ dust/smoke
→ glow decay
→ camera impulse
→ layered audio
```

### Laser

- sharp core;
- emissive halo;
- impact flare;
- sparks;
- hot contact;
- afterglow;
- consistent thickness.

### Force

- readable directional wave;
- origin pulse;
- pressure/ring/cone feedback;
- terrain/debris reaction;
- camera response.

### Cryo

- cold flash;
- frost shards/particles;
- clear affected area;
- cold palette;
- short decay.

### Boost / drill

- stage-specific exhaust;
- trail;
- speed/Mach cue;
- drilling sparks;
- dust;
- contact glow.

### Camera

Законченный presentation-only feedback:

- positional impulses;
- subtle rotational impulse;
- short zoom kick;
- stacking;
- decay;
- velocity lookahead;
- high-speed zoom-out;
- reversal damping.

Без uncontrolled random jitter.

### Audio

В рамках feature допустимо:

- explosion attack/body/tail layers;
- boost variation;
- laser/force/cryo accents;
- impact strength;
- presentation RNG variation.

### Acceptance

В normal gameplay подряд использовать все текущие abilities.

Каждая должна иметь:

```text
attack
→ readable impact
→ decay
```

Игра должна визуально ощущаться заметно лучше без добавления новой gameplay
ability.

---

# 8. Следующие vertical slices

После merge двух P0 задач.

---

## EF-FLD-001 — Water & Fluid Spectacle v1

**Priority:** P1

Цель:

```text
large pool
→ readable surface/depth
→ player/body entry splash
→ explosion/force displacement
→ spray/foam
→ surface reforms
```

Включает:

- surface classification;
- depth/surface palette;
- foam;
- spray;
- body impact splash;
- large blast displacement;
- optional player drag/buoyancy.

Gameplay water остаётся cellular.

Visual spray/foam — presentation-only.

---

## EF-ABL-001 — Lightning Ability v1

**Priority:** P1

Одна законченная ability:

- deterministic target/arc selection;
- short chain behaviour;
- explicit water interaction;
- gameplay damage/force;
- branched visual arcs;
- emissive core/bloom;
- impact FX;
- camera;
- audio;
- tests;
- showcase.

Не делать глобальную expensive conductivity simulation.

---

## EF-MOV-001 — High-Speed Movement v1

**Priority:** P1

- acceleration curve;
- turning authority vs speed;
- braking;
- momentum;
- boost transitions;
- high-speed collision feel;
- drilling;
- camera;
- trails;
- water transition;
- tuning.

Movement должно быть интересно само по себе.

---

## EF-ENV-001 — World Presentation & Identity v1

**Priority:** P1

- atmospheric procedural sky;
- parallax;
- haze/clouds/stars;
- 2–3 original palette zones;
- ruins/industrial silhouettes;
- emissive environment details;
- local lighting tuning.

Без копирования assets/level composition референса.

---

## EF-AUD-001 — Audio & Impact v1

**Priority:** P2

Привести audio events к выразительной системе:

- strength;
- material;
- distance;
- context.

Добавить/доработать:

- body impacts;
- debris;
- water;
- explosions;
- boost;
- ambience;
- lightning.

---

## EF-SHOW-001 — Showcase & Release Polish

**Priority:** final

Deterministic showcase scenes:

1. high-speed drilling;
2. detach large platform;
3. carry/throw terrain;
4. fracture body;
5. explosion over water;
6. lightning over water;
7. large impact;
8. environment fly-through.

Затем:

- profiling;
- sanitizer release gate;
- memory pass;
- 10-minute soak;
- final visual/audio tuning;
- screenshot/video capture.

---

# 9. Parallel ownership

На ближайшем этапе:

```text
CLAUDE                          CODEX
---------------------------------------------------------------
EF-PHY-001                      EF-VFX-001
dynamic terrain gameplay        combat presentation/game feel

EF-MOV-001                      EF-ENV-001
movement gameplay               world presentation

EF-ABL-001 gameplay             ability visual/audio polish

fluid gameplay                  fluid presentation
```

Разрешены маленькие cross-layer hooks, если без них feature нельзя закончить.

Но не переписывать subsystem другого агента без необходимости.

---

# 10. Definition of Done vertical feature

Feature считается готовой только если:

1. её можно реально увидеть/почувствовать;
2. architecture не сломана;
3. dangerous invariants покрыты tests;
4. нет очевидного unbounded workload;
5. автор сделал manual/deterministic acceptance;
6. docs обновлены только там, где это реально нужно;
7. один понятный PR описывает конечный пользовательский результат.

Главный вопрос перед PR:

> Что нового теперь реально может сделать или почувствовать игрок?

Если ответ сводится только к "появился новый internal API", задача слишком
мелкая для нового roadmap.
