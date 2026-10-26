# malisx2 в dArkOS

Сборкой образа, эмуляторами и выпусками занимается репозиторий
[dArkOS_rg52mini](https://github.com/mamaich/dArkOS_rg52mini); там же подробное
описание в `docs/KNOWN-ISSUES.md` («Vulkan: Arm's driver or malisx2, chosen
per system»). Здесь — то, что нужно знать со стороны драйвера.

## Два драйвера и выбор между ними

- Файл на устройстве: `/usr/lib/aarch64-linux-gnu/malisx2/libvulkan_malisx2.so`.
- Загрузчику Vulkan виден только селектор `libvulkan_darkos.so`, на который
  указывает ICD-файл. Он загружает libmali или malisx2 и отдаёт загрузчику
  точки входа выбранного. Оба сразу нельзя: libmali захватывает DRM master, и
  swapchain malisx2 через VK_KHR_display тогда не создаётся.
- Порядок выбора:
  1. `DARKOS_VULKAN` в окружении: `mali`, `malisx2` или **путь к любой .so** —
     так удобно пробовать свою сборку, не трогая системную;
  2. строка `<программа> = <драйвер>` в `/etc/darkos/vulkan.conf` (программа —
     имя исполняемого файла: `armsx2-sdl`, `retroarch`, `dolphin-emu-nogui`,
     `PPSSPPSDL`…);
  3. `default` там же;
  4. libmali.

  Драйвер, который не загрузился, заменяется на libmali, причина пишется в
  stderr; `DARKOS_VULKAN_DEBUG=1` печатает выбор.
- В EmulationStation: EMULATOR SETTINGS → система → VULKAN DRIVER (AUTO, MALI,
  MALISX2). ES запускает игру с `DARKOS_VULKAN`. Пункт есть только у систем
  из строки `es_systems` в `vulkan.conf`.
- Переменные окружения из вашей оболочки до игры, запущенной ES, не доходят:
  для опытов со своей сборкой драйвера удобнее строка `<программа> = <путь>`
  в `vulkan.conf` (потом вернуть как было).

## Сборка в образе

`build_malisx2.sh` в dArkOS: форк на закреплённом коммите (`MALISX2_COMMIT`),
Mesa на коммите из `meson.build`, сборка внутри chroot по пути
`/home/ark/malisx2`, LTO + PGO с профилем из репозитория dArkOS
(`malisx2/pgo/malisx2-<дата>.profdata.gz`, sha256 в рецепте). Новый коммит
форка — пересборка, около 33 минут с нуля. Флаги, нюансы PGO (привязка
профиля к пути `/home/ark/malisx2`, снятие профиля, когда его переснимать) и
локальная кросс-сборка — в [building.md](building.md).

## Выпуск

Новая версия попадает в образ так: коммит в `main` этого форка → сессия или
человек, ведущий dArkOS, получает хеш коммита, что изменилось, что проверено
на устройстве и нужно ли переснимать профиль PGO (если да — новый
`.profdata` и его sha256) → обновляет `MALISX2_COMMIT` (и профиль) в
`build_malisx2.sh`. Подменять malisx2 в dArkOS как-то иначе нельзя: образ
берёт его только из форка по закреплённому коммиту.
