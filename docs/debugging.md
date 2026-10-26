# Отладка и замеры

## Переменные окружения

| переменная | что делает |
|---|---|
| `MALISX2_MEASURE=<режимы>[,ключ=значение…]` | режимы `timing` (время GPU по проходам и диспатчам), `draws` (и по каждой отрисовке), `csf` (захват отправленных заданий; на job manager — `jm-NNNNNNNN.bin`), `both`, `shaders`; ключи `dir=`, `chunk=`, `ring=`, `start=`, `count=`, `csfmax=`. Каталог `dir=` должен существовать |
| `MESA_DEBUG=1`, `MESA_LOG_LEVEL=debug` | отладочный вывод Mesa |

На Android вместо `MALISX2_MEASURE` — свойство `debug.malisx2.measure`.

Подводные камни:

- ключи `MALISX2_MEASURE` пишутся слитно: `csfmax`, а не `csf_max`. Неизвестный
  ключ выключает измерения целиком, в логе одна строка «measurement off: cannot
  parse»;
- в dArkOS игры из EmulationStation запускаются не из вашего окружения, и
  переменные до драйвера не доходят; для опытов эмулятор запускают
  отдельно (`systemd-run --setenv=…` с той же командой, что у ES).

## Сообщения, которые не означают ошибку

- dmesg: «Stride 72 passed to job_submit isn't supported by the kernel»;
- лог: «context without a JIT zone: the kernel refused MEM_JIT_INIT»;
- лог: «occlusion query pools are accepted but not counted» — заглушка
  occlusion-запросов (см. [decisions.md](decisions.md)).

## Как искать причину потери устройства

На примере ND Demo (Dolphin), 26.10.2026.

1. **dmesg** даёт адрес и слот: «Unhandled Page fault in AS0 at VA
   0x7C75FFFF0 … TRANSLATION_FAULT … READ», «error detected from slot 1». Лог
   драйвера даёт атом: «device lost: vertex/tiler/compute atom 1 (submission 6,
   command buffer 0, batch 0)».
2. **Проверить пропущенные вызовы.** Нереализованные точки входа пишут в лог
   «malisx2: vkXxx called (…); call skipped» (или «returning
   VK_ERROR_FEATURE_NOT_PRESENT»). Пропущенная команда может оставить
   состояние драйвера несогласованным. У ND Demo таких вызовов не было.
3. **Захват заданий:** `MALISX2_MEASURE=csf,dir=<каталог>,csfmax=<N>`: атомы
   submit'а и память команд его командных буферов. Номер сбойного submit'а в
   имени файла тот же, что в сообщении о потере устройства (submit'ы без
   атомов не пишутся с `3a8c885`, раньше они затирали настоящий захват).
4. **Декодер захвата** разбирает цепочки атомов и задания v7 (Vertex, Compute,
   Tiler, Write Value, Fragment, Draw, Primitive, таблицы Attribute Buffer и
   Attribute) и с ключом `--fault <адрес>` помечает задания, чьи чтения могут
   дать этот адрес. Он хранится вне репозитория, в инструментах форка
   (`jmdec.py`). Поиск в памяти захвата 64-битного адреса буфера быстро
   показывает, кто на него ссылается (у Attribute Buffer младшие биты адреса
   заняты типом).
5. **Какое задание упало.** Job manager пишет статус в первое слово заголовка
   задания: 1 — выполнено, 0 — не начато. Временная отладка, которая при
   сбое печатает последние задания со статусами, хранится патчем вне
   репозитория (`debug-v7-drawdump-jobring.patch`; в нём же
   `MALISX2_DRAWDUMP=N` — печать дескрипторов первых N отрисовок каждого
   прохода). У ND Demo: compute-задание выполнено, следующее — Vertex без
   атрибутов — нет. Значит, вершинный буфер читал сам шейдер.
6. **Шейдер.** Дальше — исходник шейдера эмулятора (для Dolphin —
   `Source/Core/VideoCommon/VertexShaderGen.cpp`). Адрес «база + 0xFFFFFFF0» —
   это элемент −1, завёрнутый в 32 бита.

## Замер скорости

- **Кадры** считаются по флипам DRM (ftrace `drm_vblank_event_delivered`):
  среднее, p95, p99 и максимум времени кадра на заданном отрезке. Без ввода
  эмуляция детерминирована, и отрезок одинаков для всех вариантов драйвера.
  Перед каждым прогоном SoC остывает ниже 62 °C, варианты чередуются.
- **Время GPU по слотам:** tracepoint kbase `mali/mali_job_slots_event`;
  event = type << 24 | slot << 16, start = 1, stop = 2.
- **Сравнивать только одинаково собранные варианты.** Сборка образа — LTO +
  PGO ([darkos.md](darkos.md)); отладочная сборка без LTO для сравнения
  скорости не годится. Функции, изменённые после снятия профиля PGO, теряют
  его — это тоже надо учитывать.
- Стенд для ARMSX2 (God of War, Jak and Daxter) и скрипты запуска хранятся вне
  репозитория, в инструментах форка.
