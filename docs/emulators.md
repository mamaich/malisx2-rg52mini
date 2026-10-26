# Эмуляторы: что они делают и что из этого следует для драйвера

Только то, что проверено по исходникам эмуляторов или на устройстве.

## Dolphin (GameCube, Wii)

Тестовые игры: ND Demo (Tech Demo), Metroid Prime.

- **Пул occlusion-запросов при старте.** Видео-бэкенд Vulkan создаёт пул на
  512 occlusion-запросов и при отказе бросает весь бэкенд: «Failed to create
  query pool» → «Failed to initialize video backend!». Сами запросы Dolphin
  выдаёт, только когда игра пишет регистры счётчиков пикселей GameCube (PE
  performance metrics, настройка PerfQueries). У malisx2 — заглушка: пул
  создаётся, результаты 0 (`0488ddb`).
- **Развёртка линий и точек в вершинном шейдере.** Без `geometryShader` и
  `wideLines` (у malisx2 их нет, Dolphin пишет «Missing both geometryShader
  and wideLines features») Dolphin рисует линию квадом из четырёх углов, а
  вершинный шейдер сам читает вершины из storage-буфера:

  ```glsl
  bool is_bottom = (gl_VertexID & 2) != 0;
  uint vertex_id = gl_VertexID >> 2;
  InputData i = input_buffer[vertex_id];
  ...
  uint other_id = vertex_id;
  if (is_bottom) other_id -= 1; else other_id += 1;   // другой конец линии
  InputData other = input_buffer[other_id];
  ```

  Индексы берут «нижние» углы только у второй вершины пары, поэтому на GPU,
  который шейдит только вершины из индексов, `other_id` не уходит в −1. На v7
  Vertex-задание шейдит весь диапазон [min, max] (см.
  [hardware.md](hardware.md)): для одной линии индексы 0, 1, 6, 7 дают
  диапазон 8, вершины 2 и 3 тоже шейдятся, и у них `vertex_id = 0`,
  `is_bottom` — чтение `input_buffer[0xFFFFFFFF]`. 32-битное смещение
  заворачивает его в «буфер + 0xFFFFFFF0» — page fault и потеря устройства
  (ND Demo, первая отрисовка в EFB). Исправлено проверкой границ SSBO в
  вершинных шейдерах на v7 (`80994d9`). С убершейдерами (они тоже читают
  вершины из storage-буфера) до исправления было то же падение по тому же
  адресу; после исправления этот режим не перепроверялся.
- **Результаты** (Dolphin из dArkOS 1.3, 26.10.2026): ND Demo — ~41 кадр на
  malisx2 против ~37 на libmali, Metroid Prime — ~31–40 кадров; ошибок нет.
  Другие игры не проверялись.
- **Лог в консоль:** `-C Logger.Options.WriteToConsole=True -C
  Logger.Options.Verbosity=4 -C Logger.Logs.VIDEO=True`.

## ARMSX2 (PS2)

Форк PCSX2; файл, о котором речь, — `pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp`.
Тестовые игры: God of War (демо, сохранение на палубе корабля), Jak and
Daxter (вступление).

- **Раскладка дескрипторов отрисовки (TFX):**
  - set 0 — обычный сет: binding 0 — динамический UBO вершинного шейдера
    (cb0), binding 1 — динамический UBO фрагментного (cb1), binding 2 —
    storage-буфер для развёртки в вершинном шейдере (`vs_expand`);
    привязывается `vkCmdBindDescriptorSets` с новыми динамическими смещениями
    всякий раз, как меняется cb0 или cb1;
  - set 1 — push-дескрипторы с текстурами (только фрагментный шейдер). На
    malisx2 ARMSX2 использует push-дескрипторы (для libmali они выключены:
    падает vkCmdPushDescriptorSetKHR). Пушатся только изменившиеся привязки,
    остальные переносятся с прошлого пуша.
- **Когда текстура пушится заново:** при смене текстуры или её image layout
  (`PSSetShaderResource`), и все привязки — при возврате из utility-раскладки
  к TFX. Каждый пуш даёт сету новый блок командной памяти, адрес сета
  меняется, и на v7 таблицы дескрипторов фрагментного шейдера собираются
  заново с заданием копии. Дескрипторы malisx2 не хранят layout, поэтому
  повторный пуш после смены layout даёт те же байты и мог бы взять прежний
  блок — это в планах «шага 0» ([plans.md](plans.md)).
- **Развёртка спрайтов, линий и точек в вершинном шейдере** (`vs_expand`,
  включена по умолчанию). Перед каждой такой отрисовкой —
  `vkCmdPushConstants(VK_SHADER_STAGE_VERTEX_BIT)` с base vertex и base index.
  malisx2 сейчас пересобирает на это FAU обеих стадий, не глядя на
  `stageFlags`, и делает лишнюю копию uniform-слов фрагментного шейдера —
  тоже в планах. Шейдеры развёртки читают storage-буфер, поэтому на них
  действует проверка границ из `80994d9`.
- **Состояние.** ARMSX2 копит грязные флаги и применяет их перед отрисовкой
  (`ApplyTFXState`): сначала uniform-буферы (новые смещения → привязка set 0),
  потом текстуры (push set 1).

## Azahar (3DS)

В dArkOS — ядро RetroArch `azahar_libretro`. Тестовые игры: Castlevania Mirror
of Fate, Luigi's Mansion, Super Mario 3D Land.

- **Компиляция пайплайнов** — главное преимущество malisx2: Luigi's Mansion,
  390 пайплайнов за 3,8 с против 24–25 с у libmali (замер сессии dArkOS,
  25.10.2026). Первый запуск игр почти не подтормаживает.
- **Отрисовка медленнее libmali:** Castlevania — 64 мс на кадр против 50
  (разбор по слотам — в [plans.md](plans.md)). Профиль PGO для образа
  снимается на Super Mario 3D Land и Luigi's Mansion; Castlevania в
  тренировку не входит, на ней меряют результат.
- **Память:** пик RetroArch 1067 МБ против 1132 на libmali.

## Остальные

PPSSPP, DuckStation/SwanStation, Flycast и порты на malisx2 систематически не
проверялись — это в [plans.md](plans.md).
