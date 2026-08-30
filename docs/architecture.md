# Архитектура

## Общий подход

Emberfall — однопоточное приложение на C11 и raylib. Архитектура разделена на
небольшие data-oriented модули. Главный цикл владеет состоянием верхнего уровня
и явно передаёт его подсистемам. Скрытого глобального игрового состояния нет;
глобальными остаются только внутреннее состояние raylib и его input API.

```text
raylib input
     |
     v
 main.c -------------------------> Camera2D + HUD
   |                                  |
   +--> player.c <---- world.c -------+
   |        |            |
   |        |            +--> Texture2D / Color buffer
   |        |
   +--> powers.c ---------+--> particles.c
   |        |
   |        +-----------------> world destruction / heat / shockwave
   |
   +--> audio.c <--------------- power and reaction events
```

## Модули

### `main.c`

Точка композиции приложения. Отвечает за:

- создание окна и audio device;
- владение `World`, `Player`, `PowerSystem`, `ParticleSystem`, `GameAudio` и
  `Camera2D`;
- чтение ввода;
- fixed-step симуляцию мира;
- преобразование координат мыши;
- camera follow и camera shake;
- порядок обновления и отрисовки;
- HUD, reset и smoke-test.

### `world.c/.h`

Владеет физическим миром и GPU-текстурой мира:

- непрерывный массив `Cell`;
- постоянный `Color`-буфер;
- `Texture2D` размером с симуляцию;
- active-chunk буферы;
- материалы, температура и фазовые переходы;
- генерация карты;
- лазерное воздействие, разрушение и ударная волна;
- очередь событий water/lava reaction фиксированного размера.

### `player.c/.h`

Содержит движение, collision и отрисовку игрока:

- плавный полёт без гравитации;
- circle-vs-cell collision;
- защита от tunneling с помощью substeps;
- процедурная отрисовка персонажа.

### `powers.c/.h`

Хранит состояние способностей и координирует их эффекты:

- трассировка контактного лазера;
- explosion cooldown;
- разрушение мира и ударная волна;
- события для camera shake, player impulse и audio;
- world-space визуализация луча, прицела и кольца взрыва.

### `particles.c/.h`

Фиксированный циклический пул из 1024 частиц. Частицы не выделяют память во
время кадра. Разные spawn-функции задают скорость, цвет, lifetime, размер и
индивидуальную gravity.

### `audio.c/.h`

При старте синтезирует короткие PCM wave-буферы для лазера, взрыва и реакции
материалов. После `LoadSoundFromWave` временные CPU-буферы освобождаются. Ошибка
инициализации audio device не является фатальной.

## Порядок одного render frame

Текущий порядок в `main.c` важен:

1. Ограничить `deltaTime` значением 0.05 секунды.
2. Обработать `F1` и `R`.
3. Обновить игрока и collision относительно текущего мира.
4. Обновить camera follow и затухание shake.
5. Преобразовать позицию мыши из screen space в world/cell space.
6. Обновить способности и применить мгновенные эффекты к миру.
7. Применить explosion impulse, camera shake и звук.
8. Обновить частицы.
9. Выполнить необходимое число fixed ticks мира по 1/60 секунды.
10. Обработать reaction events: частицы пара и звук.
11. Повторно разрешить collision игрока — динамический sand мог войти в его
    область во время simulation tick.
12. Обновить texture мира и отрисовать world-space объекты.
13. Отрисовать debug HUD.

## Владение памятью

| Ресурс | Создание | Освобождение |
|---|---|---|
| `World.cells` | `WorldInit` | `WorldUnload` |
| `World.pixels` | `WorldInit` | `WorldUnload` |
| chunk buffers | `WorldInit` | `WorldUnload` |
| world `Texture2D` | `WorldInit` | `WorldUnload` |
| particle pool | встроен в `ParticleSystem` | автоматически |
| sounds | `GameAudioInit` | `GameAudioUnload` |

Heap allocation в frame loop запрещён. Размеры world buffers и particle pool не
меняются во время игры.

## Координатные пространства

- Cell/world space использует одну world unit на одну cell.
- World texture имеет размер 512×288 и рисуется в начале world space.
- Камера показывает логическую область 320×180 и масштабирует её к окну.
- `TEXTURE_FILTER_POINT` сохраняет nearest-neighbor вид.
- `WorldScreenToCell` применяет `GetScreenToWorld2D`, округляет вниз и ограничивает
  результат границами мира.
