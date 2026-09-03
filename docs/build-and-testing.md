# Сборка и тестирование

## Зависимости

- Linux;
- GCC с C11;
- GNU Make;
- pkg-config/pkgconf;
- системный raylib.

Arch Linux:

```sh
sudo pacman -S --needed base-devel pkgconf raylib
```

Makefile получает флаги только через:

```sh
pkg-config --cflags raylib
pkg-config --libs raylib
```

## Targets

### Release

```sh
make
```

Флаги: `-std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wconversion -O2`.
Предупреждений должно быть ноль — включая `-Wconversion`, который проект держит
чистым: сейчас это три исправленных неявных `int → float` и ни одного шумного
срабатывания.

Результат: `build/release/emberfall`.

### Debug

```sh
make debug
```

Флаги: те же плюс `-g -O0`.

Результат: `build/debug/emberfall`.

### Run

```sh
make run
```

Дополнительные аргументы передаются через `RUN_ARGS`:

```sh
make run RUN_ARGS=--smoke-test
```

### Test

```sh
make test
```

## Benchmark, sanitizers и profiling

Десять воспроизводимых headless-сценариев на production-size world:

```sh
make bench
make bench BENCH_ARGS="--ticks 60"
```

Сценарии, методика и сохранённый baseline описаны в
[`performance.md`](performance.md). Wall-clock результаты информационные;
workload counters нужны для переносимых regression-проверок.

Отдельные конфигурации не смешивают instrumented objects с release/debug:

```sh
make asan
make ubsan
make profile
```

`asan` и `ubsan` собирают приложение и запускают весь headless test suite.
`profile` создаёт бинарник с `-pg` в `build/profile/emberfall`; после обычного
запуска его `gmon.out` можно анализировать через `gprof`.

`make bench` печатает отдельную строку `lighting`: стоимость `WorldUpdateLighting`
на неподвижной сцене, на движущемся фонаре игрока и при бурении каждый кадр.
Свет работает целиком на CPU, поэтому измеряется без GL-контекста.

### Tooling

```sh
make compile_commands.json
```

Файл нужен clangd и другим инструментам. Он не хранится в репозитории и
удаляется через `make clean`.

Собирает и запускает `tests/world_tests.c` — headless-набор проверок CPU-side
gameplay. Результат: `build/release/emberfall-tests`.

### Clean

```sh
make clean
```

Удаляет только генерируемый каталог `build/`.

## Headless-тесты

`make test` — основная и самая дешёвая проверка. Она никогда не открывает окно:
`World` владеет только CPU-состоянием, а `Renderer`/`WorldRenderer` не
линкуются в test binary. Это разделение нужно сохранять; из-за него набор
гоняется без display, аудио и X-сервера.

Набор собирается из `tests/world_tests.c`, файлов модуля мира
(`$(WORLD_SOURCES)` в Makefile), `src/game.c`, `src/player.c`, `src/abilities.c` и
`src/particles.c` и покрывает инварианты, которые иначе держались
бы только на памяти разработчика:

- полнота таблицы `MATERIALS` и согласованность solid-семантики;
- headless incremental render preparation: первый полный build, нулевая работа
  settled кадра и один dirty region после локального изменения;
- сохранение массы sand и water при падении и растекании;
- «одна cell — одно обновление за tick»;
- пороги фазовых переходов rock→lava, water→steam и реакция water+lava;
- локализация огня из нескольких точек поджига;
- лавовый карман не съедает собственную rock-обкладку, но всё ещё поджигает
  соседний dirt;
- осевшие cells засыпают, а внешнее воздействие будит только локальную область;
- генератор начинает полностью спящим, а `WorldActivateRegion` будит только
  динамические/нагретые chunks внутри запрошенной области;
- `WorldDrillCircle` режет только solids, возвращает верное число cells, не
  пробивает границу мира и не может поджечь мир своим нагревом;
- boost пробуривает rock и песок, запускается с места упором в стену,
  сопротивление бурения никогда не останавливает игрока, и игрок не заканчивает
  кадр внутри твёрдой породы;
- обычный полёт на крейсерской скорости сохраняет небольшой hover-наклон, а
  полноценная head-first поза достигается только на boost;
- буст даёт ровно один velocity kick на нажатие, сколько бы клавишу ни держали,
  доходит до sonic threshold и сбрасывается отпусканием Shift; порода не отнимает
  у полёта скорости;
- частицы не проходят сквозь рельеф, обломки оседают в мир, не затирая его, а
  газовые эффекты рельеф игнорируют;
- игровая конфигурация Q действительно создаёт тяжёлый одиночный удар, сохраняет
  массу динамического материала и публикует достаточные burst/recoil feedback.

Любой найденный или изменённый инвариант симуляции должен получать здесь тест.
Набор уже окупил себя: он обнаружил, что `WorldInit` оставлял cells с
температурой 0 °C при ambient 20 °C, из-за чего chunk sleeping был фактически
отключён во всём мире.

## Smoke-test

`--smoke-test` запускает обычное raylib-приложение, но подставляет
детерминированную scripted input sequence и завершает его после renderer,
dynamic-terrain gameplay и полного movement acceptance phases. Без явного
`--seed` он использует фиксированный seed, иначе reference screenshot нечего
сравнивать между запусками.

Отдельный флаг `--seed VALUE` воспроизводит конкретный мир: debug HUD печатает
seed текущего мира строкой `SEED`, и его достаточно, чтобы повторить сессию.

```sh
make run RUN_ARGS="--seed 0x1234"
```

Palette выбирается из seed и не входит в gameplay state. Для screenshot или
сравнения presets её можно зафиксировать:

```sh
make run RUN_ARGS="--seed 0x1234 --palette ember"
```

Допустимые значения: `auto`, `ember`, `abyss`, `storm`.

Проверяются:

- water/lava reaction;
- laser contact;
- explosion event;
- collision и отскок с отрицательной velocity от тонкой rock-стены;
- boost-разгон и проход сквозь rock-блок толщиной 11 cells; та же проба
  прогоняет boost trail и drill debris particles, поэтому основной цикл больше
  не подменяет ввод игрока;
- локализация fire внутри тестового блока dirt;
- переход части chunks в sleeping state;
- smoke-only остров проходит штатный `WorldFindComponent` + atomic extraction,
  исчезает из static World, падает, вращается и сталкивается с
  static-world smoke shelf; scene/emissive cache делает ровно два initial
  uploads и после `FreeBody` оставляет ноль cached textures/bytes;
- resize offscreen targets не пересоздаёт body-local textures.
- procedural environment рисуется в scene и explicit emissive pass, корректно
  переживает resize, transient camera rotation и high-speed zoom-out;
- smoke принудительно проходит все три palettes и требует mask `0x7`.

Проверка fire создаёт отдельный мир 48×32, зажигает в центре dirt-блока область
3×3 и выполняет 240 simulation ticks. Тест требует, чтобы большая часть грунта
осталась на месте: так возврат бесконечного распространения огня обнаруживается
без изменения основной сгенерированной карты.

При отсутствии любого события процесс возвращает код 2 и печатает значения
проваленных checks. При успехе создаются `build/emberfall-smoke.png` и три
palette reference: `build/emberfall-smoke-ember.png`,
`build/emberfall-smoke-abyss.png`, `build/emberfall-smoke-storm.png`.

Smoke-test требует display и OpenGL context. В headless Linux:

```sh
xvfb-run -a make run RUN_ARGS=--smoke-test
```

Недоступный audio device не должен приводить к падению игры.

## Обязательная проверка перед handoff

```sh
make clean
make
make debug
make test
xvfb-run -a make run RUN_ARGS=--smoke-test
git diff --check
git status --short
```

Ожидается:

- обе конфигурации собираются без warnings;
- все headless-тесты проходят;
- smoke-test возвращает 0;
- screenshot имеет размер 1280×720;
- нет trailing whitespace;
- рабочее дерево содержит только намеренные изменения.

## Что проверять при изменениях симуляции

- Cell не перемещается дважды за tick.
- Falling materials обходятся снизу вверх.
- Horizontal traversal не получает постоянный bias.
- Новые material mutations пробуждают active chunk и соседей.
- Новые материалы добавлены записью в таблицу `MATERIALS`, включая имя.
- Solid semantics согласованы с `WorldMaterialIsSolid` и collision игрока.
- Новые временные эффекты не выделяют heap memory в frame loop.
- Новое smoke-поведение остаётся детерминированно достижимым за короткий запуск.
- Найденный или изменённый инвариант закреплён тестом в `tests/world_tests.c`.
- Изменение размера мира проверено на реальном GL renderer: целевая система
  должна поддерживать Texture2D шириной 16384.
