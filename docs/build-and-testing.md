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

Флаги: `-std=c11 -Wall -Wextra -Wpedantic -O2`.

Результат: `build/release/emberfall`.

### Debug

```sh
make debug
```

Флаги: `-std=c11 -Wall -Wextra -Wpedantic -g -O0`.

Результат: `build/debug/emberfall`.

### Run

```sh
make run
```

Дополнительные аргументы передаются через `RUN_ARGS`:

```sh
make run RUN_ARGS=--smoke-test
```

### Clean

```sh
make clean
```

Удаляет только генерируемый каталог `build/`.

## Smoke-test

`--smoke-test` запускает обычное raylib-приложение, но подставляет короткую
последовательность input и завершает его через несколько frames.

Проверяются:

- water/lava reaction;
- laser contact;
- explosion event;
- collision и отскок с отрицательной velocity от тонкой rock-стены;
- boost-разгон и проход сквозь rock-блок толщиной 11 cells; та же проба
  прогоняет boost trail и drill debris particles, поэтому основной цикл больше
  не подменяет ввод игрока;
- локализация fire внутри тестового блока dirt;
- переход части chunks в sleeping state.

Проверка fire создаёт отдельный мир 48×32, зажигает в центре dirt-блока область
3×3 и выполняет 240 simulation ticks. Тест требует, чтобы большая часть грунта
осталась на месте: так возврат бесконечного распространения огня обнаруживается
без изменения основной сгенерированной карты.

При отсутствии любого события процесс возвращает код 2 и печатает значения
проваленных checks. При успехе создаётся `build/emberfall-smoke.png`.

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
xvfb-run -a make run RUN_ARGS=--smoke-test
git diff --check
git status --short
```

Ожидается:

- обе конфигурации собираются без warnings;
- smoke-test возвращает 0;
- screenshot имеет размер 1280×720;
- нет trailing whitespace;
- рабочее дерево содержит только намеренные изменения.

## Что проверять при изменениях симуляции

- Cell не перемещается дважды за tick.
- Falling materials обходятся снизу вверх.
- Horizontal traversal не получает постоянный bias.
- Новые material mutations пробуждают active chunk и соседей.
- Новые динамические материалы учитываются в `MaterialIsDynamic`.
- Solid semantics согласованы с `WorldMaterialIsSolid` и collision игрока.
- Новые временные эффекты не выделяют heap memory в frame loop.
- Новое smoke-поведение остаётся детерминированно достижимым за короткий запуск.
