# malisx2 для RG52 Mini (dArkOS)

> Это форк [bmdhacks/malisx2](https://github.com/bmdhacks/malisx2). Исходный
> README апстрима (на английском) сохранён как
> [README.upstream.md](README.upstream.md): в нём сборка под Android и ROCKNIX,
> требования к инструментам и общее описание драйвера.

malisx2 — Vulkan-драйвер для GPU Arm Mali, который работает поверх штатного
драйвера ядра Arm (kbase). Шейдеры компилирует Mesa: на Bifrost — её
бэкенд для Bifrost, на Valhall и новее — kraid. Работу с GPU (job manager
или CSF, дескрипторы, WSI) драйвер ведёт сам, на общем Vulkan runtime Mesa.

В апстриме драйвер пишется под одно приложение, PS2-эмулятор ARMSX2, и в
первую очередь под Mali-G615. **Этот форк дорабатывает его под эмуляторы на
Anbernic RG52 Mini в прошивке [dArkOS](https://github.com/mamaich/dArkOS_rg52mini)**:
не только под ARMSX2, но и под остальные эмуляторы образа, которые рисуют
через Vulkan.

## Цели

Главное — **скорость** и **отсутствие падений**:

- никаких потерь устройства (device lost) и зависаний GPU в играх: драйвер
  должен выдерживать то, что делают настоящие эмуляторы, в том числе то, на
  что апстрим не рассчитан;
- скорость отрисовки — не хуже штатного драйвера Arm (libmali) там, где это
  достижимо;
- сохранить главное преимущество malisx2 — быструю компиляцию пайплайнов
  (на Mali-G52 в 6 раз быстрее libmali, первый запуск игр почти без
  подтормаживаний).

## Устройство

| | |
|---|---|
| SoC | Rockchip RK3562 |
| GPU | Mali-G52, одно шейдерное ядро; Bifrost, архитектура Mesa v7; job manager (не CSF) |
| ОЗУ | 2 ГБ |
| драйвер ядра | kbase g25p0, интерфейс JM UK 11.47 (драйвер договаривается на 11.36) |

Особенности железа и драйвера ядра, найденные по ходу работы, — в
[docs/hardware.md](docs/hardware.md).

## Где malisx2 используется в dArkOS

В dArkOS malisx2 — второй драйвер рядом со штатным libmali. Он выбирается
для каждой программы и для каждой системы EmulationStation (EMULATOR
SETTINGS → система → VULKAN DRIVER); по умолчанию стоит libmali. Выбор есть у
систем, эмуляторы которых рисуют через Vulkan:

| система | эмулятор |
|---|---|
| PS2 | ARMSX2 |
| GameCube | Dolphin |
| 3DS | Azahar (ядро RetroArch) |
| PSP | PPSSPP (отдельный и в RetroArch) |
| PS1 | DuckStation, SwanStation |
| Dreamcast, Naomi, Atomiswave | Flycast |
| порты | разные |

Как устроен выбор драйвера и как malisx2 собирается в образе, описано в
[docs/darkos.md](docs/darkos.md).

## Документация

- [docs/README.md](docs/README.md) — оглавление;
- [docs/building.md](docs/building.md) — сборка: отладочная, как в образе (LTO
  + PGO), снятие профиля PGO;
- [docs/changes.md](docs/changes.md) — что сделано по сравнению с апстримом;
- [docs/plans.md](docs/plans.md) — планы;
- [docs/decisions.md](docs/decisions.md) — принятые решения, отложенное и
  отброшенное;
- [docs/hardware.md](docs/hardware.md) — особенности Mali-G52, RK3562 и kbase;
- [docs/emulators.md](docs/emulators.md) — что делают эмуляторы и что из
  этого следует для драйвера;
- [docs/debugging.md](docs/debugging.md) — отладка и замеры;
- [docs/darkos.md](docs/darkos.md) — malisx2 в образе dArkOS.

## Сборка

Нужны исходники Mesa в каталоге `mesa/`: ветка `malisx2` форка
[bmdhacks/armsx2-turnip](https://github.com/bmdhacks/armsx2-turnip) на
коммите, который закреплён в `meson.build` (`mesa_commit`):

    git init mesa
    git -C mesa fetch --depth 1 https://github.com/bmdhacks/armsx2-turnip.git <mesa_commit>
    git -C mesa checkout FETCH_HEAD

Для dArkOS (Debian trixie, arm64) драйвер собирается кросс-компиляцией
clang и lld 19 с sysroot arm64:

    meson setup build -Dbuildtype=release --cross-file=<aarch64.ini> --native-file=<native.ini>
    ninja -C build

Результат: `build/src/libvulkan_malisx2.so`. Это быстрая сборка для отладки;
образ dArkOS собирается с **LTO и PGO**, и у этого есть нюансы (meson не
старше 1.12, профиль PGO привязан к пути дерева `/home/ark/malisx2`, флаги
`-fprofile-use` нужны и при линковке, снятие профиля на устройстве). Всё
это, пример кросс-файла и требования к инструментам — в
[docs/building.md](docs/building.md).

## Отладка

Включается переменными окружения (на Android — свойствами
`debug.malisx2.*`), подробно в [docs/debugging.md](docs/debugging.md):

| переменная | что делает |
|---|---|
| `MALISX2_MEASURE` | время GPU по проходам и отрисовкам, захват отправленных заданий, шейдеры |

## Правила

- **Декомпиляция.** Чужой код — в частности драйверы и библиотеки Rockchip
  и Arm — можно декомпилировать, чтобы понять логику его работы. Полученный
  код не переиспользуется и в репозиторий не попадает: логику мы
  реализуем сами, с доработкой под наше устройство и наши задачи. Бинарников
  Arm и вендоров в репозитории нет.
- В репозитории — исходники драйвера, файлы сборки и документация форка
  (`README.md`, `docs/`). Рабочие заметки, отчёты о замерах, тесты и
  инструменты хранятся вне его.
- Всё внутри исходников — комментарии, сообщения в лог и прочие строки — на
  английском, в стиле апстрима (подробные комментарии); сообщения коммитов
  тоже на английском, вида `v7: ...`. Документация форка — на русском.
- Лицензия — MIT (`LICENSE`). Mesa не входит в репозиторий и остаётся под
  своими лицензиями.
