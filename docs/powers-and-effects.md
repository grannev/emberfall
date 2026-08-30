# Способности и эффекты

## PowerSystem

`PowerSystem` одновременно хранит долгоживущее состояние ресурсов и
короткоживущие события текущего frame:

- laser active/hit и координаты луча;
- explosion cooldown;
- explosion triggered, position и shock radius;
- время визуального shockwave;
- energy;
- laser heat и overheat.

`PowersUpdate` получает уже вычисленные `laserHeld` и `explosionPressed`. Это
позволяет smoke-test подставлять автоматический input без эмуляции мыши.

## Energy и heat

Начальные параметры:

| Параметр | Значение |
|---|---:|
| maximum energy | 100 |
| explosion cost | 38 |
| passive regeneration | 19/s |
| laser consumption | 13/s |
| maximum laser heat | 100 |
| heating while active | 31/s |
| normal cooling | 34/s |
| overheated cooling | 43/s |
| recovery threshold | 34 |

При достижении heat == 100 лазер блокируется. Удержание ЛКМ не отменяет
блокировку: сначала heat должен опуститься до 34. Взрыв выполняется только при
нулевом cooldown и energy не меньше 38.

## Контактный лазер

Направление определяется от игрока к world-space cursor. Максимальная точка
ограничивается краем мира и дальностью 280 cells.

`WorldApplyLaser` идёт по лучу с шагом около 0.65 cell. Луч проходит через:

- empty;
- water/lava;
- steam/smoke/fire/ash.

Он останавливается на первой dirt, sand или rock cell. Вокруг контакта
применяется brush радиусом 2.25 cells. Воздействие является тепловым:

- dirt получает 2500 C/s;
- sand получает 3100 C/s;
- rock получает 1080 C/s.

Последующие material transitions выполняются общей температурной системой.
`LaserResult` возвращает фактическую точку и материал попадания. Визуальный луч,
свечение и sparks используют именно эту точку, а не исходную позицию cursor.

## Взрыв

ПКМ создаёт взрыв в cell под cursor:

1. `WorldDestroyCircle` удаляет материал в радиусе 17.
2. 38% затронутых rock cells превращаются в lava вместо удаления.
3. `WorldApplyShockwave` выталкивает динамические материалы до радиуса 42.
4. Создаётся 120 explosion particles.
5. Устанавливается cooldown 0.7 секунды и списывается 38 energy.
6. Публикуется `explosionTriggered` для player impulse, damage, audio и camera
   shake.
7. В течение 0.32 секунды рисуется расширяющееся кольцо.

## Частицы

`ParticleSystem` содержит 1024 элемента и индекс следующей записи. При
переполнении новые частицы заменяют самые старые. Это предсказуемо ограничивает
память и стоимость update.

Spawn presets:

- explosion — оранжевые, жёлтые и серые частицы с положительной gravity;
- laser sparks — короткие жёлтые частицы, летящие назад от контакта;
- steam — светлые частицы с отрицательной gravity.

Particles являются только визуальным эффектом и не участвуют в cell physics.
Физический steam представлен отдельным материалом мира.

## Процедурный звук

`GameAudioInit` создаёт mono 16-bit PCM с sample rate 22050 Hz:

- laser — тон с harmonic и frequency wobble;
- explosion — затухающий rumble плюс noise;
- reaction — короткий hiss/fizz.

Laser sound повторно запускается, пока луч активен. Reaction sound имеет
cooldown 0.13 секунды, чтобы серия реакций не создавала чрезмерное число
одновременных звуков.

Если `InitAudioDevice` не готов, `GameAudio.ready` остаётся false, а все audio
функции безопасно становятся no-op.
