# Сборка

Как собрать драйвер для RG52 Mini (dArkOS, Debian trixie arm64): быстро для
отладки, так же, как в образе (LTO + PGO), и как снять профиль PGO. Сборка
под Android и ROCKNIX — в [../README.upstream.md](../README.upstream.md).

## Что нужно

- исходники Mesa в `mesa/` (ниже);
- **meson 1.12 и новее**: meson 1.7.0 из Debian trixie падает на Rust-крейте,
  который Mesa собирает из сгенерированных исходников («'CustomTargetIndex'
  object has no attribute 'subdir'»); 1.12.1 собирает. Его можно запускать
  прямо из распакованного архива (`python3 meson.py`) или поставить в venv;
- ninja;
- clang и lld 19, llvm 19 (`llvm-ar`, `llvm-strip`, `llvm-profdata`);
- rustc 1.85 и новее; bindgen 0.71.1 и новее, но не 0.72.0, 0.73.0 и
  0.73.1; с libclang 22 bindgen 0.71.x даёт сломанные привязки — тогда 0.72.1.
  Rust нужен для kraid (компилятора Valhall): G52 он не нужен, но без него
  Mesa не собирается;
- Python 3 с mako и PyYAML;
- для arm64: заголовки и библиотеки libdrm, libgbm, libudev (в sysroot или в
  chroot);
- `meson setup` скачивает восемь маленьких Rust-крейтов (`subprojects/`) —
  нужен интернет.

## Исходники Mesa

Ветка `rg52mini-20261031` форка
[mamaich/armsx2-turnip-rg52mini](https://github.com/mamaich/armsx2-turnip-rg52mini) —
ветка `malisx2` [bmdhacks/armsx2-turnip](https://github.com/bmdhacks/armsx2-turnip) и
наши правки компилятора — на коммите из `meson.build` (`mesa_commit`):

    MESA_COMMIT=$(sed -n "s/^mesa_commit = '\([0-9a-f]*\)'.*/\1/p" meson.build)
    git init mesa
    git -C mesa fetch --depth 1 https://github.com/mamaich/armsx2-turnip-rg52mini.git "$MESA_COMMIT"
    git -C mesa checkout FETCH_HEAD

Если `mesa/` на другом коммите, `meson setup` предупредит.

## Варианты сборки

Все — `-Dbuildtype=release` (`-O3`, без assert) и `-mcpu=cortex-a53`.

| вариант | зачем | время с нуля* |
|---|---|---|
| без LTO | отладка, быстрые итерации (инкрементально — секунды) | ~2,5 мин |
| LTO | | ~5 мин |
| LTO + PGO | **как в образе**; для выпуска и для замеров скорости | ~5–7 мин |
| с инструментированием PGO | только для снятия профиля | ~3 мин |

\* кросс-сборка на хосте (WSL, clang-19).

Эффект на компиляцию пайплайнов (время первого запуска игры; Castlevania, 368
пайплайнов; замер сессии dArkOS 25.10.2026): без LTO 8,1 с, LTO — на 3 %
быстрее, LTO + PGO — на 8,6 % (7,4 с). На скорость отрисовки ни LTO, ни
PGO не влияют: там упор в GPU.

**Сравнивать скорость можно только одинаково собранные варианты.** Отладочная
сборка без LTO для замеров не годится.

### Кросс-сборка на хосте

Кросс-файл (пути — свои; sysroot — корень arm64 Debian trixie с нужными
`-dev`-пакетами):

```ini
[constants]
triple = 'aarch64-linux-gnu'
sysroot = '/path/to/arm64-sysroot'
rd = '-resource-dir=' + sysroot + '/usr/lib/llvm-19/lib/clang/19'
target_args = ['--target=' + triple, '--sysroot=' + sysroot, rd]

[binaries]
c = ['clang-19'] + target_args
cpp = ['clang++-19'] + target_args
ar = 'llvm-ar-19'
strip = 'llvm-strip-19'
rust = ['rustc', '--target', 'aarch64-unknown-linux-gnu',
        '-C', 'linker=clang-19', '-C', 'link-arg=--target=' + triple,
        '-C', 'link-arg=--sysroot=' + sysroot, '-C', 'link-arg=' + rd,
        '-C', 'link-arg=--ld-path=/usr/bin/ld.lld-19']
pkg-config = 'pkg-config'

[properties]
sys_root = sysroot
pkg_config_libdir = [sysroot + '/usr/lib/aarch64-linux-gnu/pkgconfig', sysroot + '/usr/share/pkgconfig']
bindgen_clang_arguments = ['--sysroot=' + sysroot]

[built-in options]
c_args = ['-mcpu=cortex-a53']
cpp_args = ['-mcpu=cortex-a53']
c_link_args = ['--ld-path=/usr/bin/ld.lld-19']
cpp_link_args = ['--ld-path=/usr/bin/ld.lld-19']

[host_machine]
system = 'linux'
kernel = 'linux'
cpu_family = 'aarch64'
cpu = 'cortex-a53'
endian = 'little'
```

Native-файл (для крейтов, которые собираются под хост):

```ini
[binaries]
rust = ['rustc']
```

`-resource-dir` берёт заголовки clang из sysroot (той же версии 19), чтобы
не смешивать их с хостовыми.

Без LTO:

    meson setup build -Dbuildtype=release --cross-file=aarch64.ini --native-file=native.ini
    ninja -C build
    llvm-strip-19 -o libvulkan_malisx2.so build/src/libvulkan_malisx2.so

LTO: добавить `-Db_lto=true`. Аргументы `-Dc_args`/`-Dc_link_args` в командной
строке **заменяют** заданные в кросс-файле, поэтому `-mcpu=cortex-a53` и
`--ld-path` надо повторить в них.

### Как в образе: LTO + PGO

```sh
P=/path/to/malisx2.profdata
W="'-Wno-profile-instr-unprofiled','-Wno-profile-instr-out-of-date','-Wno-backend-plugin'"
LD="'--ld-path=/usr/bin/ld.lld-19'"
meson setup build-pgo -Dbuildtype=release -Db_lto=true \
  --cross-file=aarch64.ini --native-file=native.ini \
  "-Dc_args=['-mcpu=cortex-a53','-fprofile-use=$P',$W]" \
  "-Dcpp_args=['-mcpu=cortex-a53','-fprofile-use=$P',$W]" \
  "-Dc_link_args=[$LD,'-fprofile-use=$P']" \
  "-Dcpp_link_args=[$LD,'-fprofile-use=$P']"
ninja -C build-pgo
```

- `-fprofile-use` нужен и в аргументах линковки: при LTO код генерируется при
  линковке.
- `-Wno-profile-instr-out-of-date` и остальные подавляют предупреждения о
  функциях, изменившихся после снятия профиля, и о функциях без данных; их
  данные clang отбрасывает — это не ошибка, но выигрыш PGO у них пропадает.
- **Профиль привязан к пути дерева.** clang называет функции, локальные для
  файла (`static`), по пути к исходнику в том виде, в каком его получил
  компилятор. Исходники Mesa meson передаёт абсолютными путями
  (`/home/ark/malisx2/mesa/src/…`), а исходники самого драйвера и
  сгенерированные файлы — относительно каталога сборки (`../src/vk/…`,
  `mesa-glue/src/…`). Профиль образа снят с дерева в
  **`/home/ark/malisx2`** (там его собирает chroot образа), со сборкой в
  подкаталоге дерева. Чтобы локальная сборка совпала с образом, дерево должно
  лежать по тому же пути — например, `sudo mount --bind <дерево>
  /home/ark/malisx2`, — а каталог сборки быть прямо в нём (`build-*/`). По
  другому пути сборка проходит, но профиль `static`-функций Mesa — а это
  большая часть компилятора шейдеров — молча не применяется.
- `meson.build` задаёт `b_ndebug=if-release`: в release assert выключены.

### Снятие профиля PGO

1. Инструментированная сборка (без LTO), профили пишутся в каталог на
   устройстве:

   ```sh
   G=/home/ark/perf/msx-prof
   meson setup build-pgogen -Dbuildtype=release \
     --cross-file=aarch64.ini --native-file=native.ini \
     "-Dc_args=['-mcpu=cortex-a53','-fprofile-generate=$G','-fprofile-update=atomic']" \
     "-Dcpp_args=['-mcpu=cortex-a53','-fprofile-generate=$G','-fprofile-update=atomic']" \
     "-Dc_link_args=['--ld-path=/usr/bin/ld.lld-19','-fprofile-generate=$G']" \
     "-Dcpp_link_args=['--ld-path=/usr/bin/ld.lld-19','-fprofile-generate=$G']"
   ninja -C build-pgogen
   ```

   `-fprofile-update=atomic` — потому что драйвер вызывают из нескольких
   потоков (эмуляторы компилируют пайплайны параллельно), и счётчики иначе
   теряют обновления. Дерево — по тому же пути `/home/ark/malisx2`.
2. Тренировка на устройстве — то, что должно стать быстрее:
   - **компиляция пайплайнов**: игры Azahar с удалённым кэшем пайплайнов
     malisx2, чтобы всё компилировалось заново, — Super Mario 3D Land и
     Luigi's Mansion, по минуте-полторы. Castlevania в тренировку **не
     входит**: на ней меряют результат;
   - **путь отрисовки**: ARMSX2 — God of War с сохранения на палубе, Jak and
     Daxter со вступления и с сохранения в игре, Burnout 3 (демо), по
     75–90 с. Без этой части профиль знает отрисовку только по Azahar, а у
     ARMSX2 она другая (push-дескрипторы, развёртка в вершинном шейдере,
     частые смены пайплайна).

   Драйвер подставляется ICD-файлом (`VK_DRIVER_FILES`), через
   `DARKOS_VULKAN` или строкой в `vulkan.conf` (для игр, которые
   запускаются как из EmulationStation).
3. **Программа должна завершиться штатно**: профиль пишется при выходе.
   RetroArch закрывается двумя командами QUIT по сети
   (`network_cmd_enable`), а не `kill`. ARMSX2 завершается штатно по SIGTERM
   (`systemctl stop` его юнита).
4. Слить сырые профили — тем же llvm, что будет собирать:

       llvm-profdata-19 merge -o malisx2.profdata /home/ark/perf/msx-prof/*.profraw

   Несколько тренировок с разными весами: `llvm-profdata merge
   -weighted-input=<вес>,<файл> …`, веса — чтобы у каждой части был
   примерно одинаковый суммарный счёт (`llvm-profdata show
   --detailed-summary`).
5. Сколько профиля теряется при изменённом коде: собрать с `-fprofile-use=…
   -Wno-everything -Wbackend-plugin -Wno-error=backend-plugin -mllvm
   -pgo-warn-missing-function` — clang назовёт функции, у которых код
   изменился после тренировки.

**Когда снимать заново:** после заметных правок компилятора шейдеров (Mesa,
`src/compiler/`, `src/vk/mali_shader.c`) — и после правок горячего пути
отрисовки (`mali_jm_cmd_draw_v7.c`, `mali_cmd_state.c`, `mali_jm_desc_v7.c`,
`mali_jm_queue.c`). Изменённые функции теряют профиль целиком: после правок
«шага 0» (26.10.2026) его потеряли `draw`, `cmd_gfx_fau`, push-дескрипторы и
push constants, `mali_v7_desc_refresh`, `mali_v7_ubo_copy`, `queue_submit`, а
ARMSX2 на A53 во многом упирается в CPU. Сравнивать скорость правок пути
отрисовки поэтому надо либо обе сборки без PGO, либо обе с профилем, снятым
со своего кода.

## Сборка в образе dArkOS

`build_malisx2.sh` в репозитории dArkOS (подробно — [darkos.md](darkos.md)):

- скачивает форк на закреплённом коммите (`MALISX2_COMMIT`) и Mesa на
  коммите из `meson.build` в `/home/ark/malisx2` корня образа;
- собирает **внутри chroot arm64**, но C и C++ — amd64-сборкой того же clang
  (xtool, работает нативно и быстро), Rust — rustc из chroot под qemu;
- `-Dbuildtype=release -Db_lto=true`, `-mcpu=cortex-a53`, профиль
  `malisx2/pgo/malisx2-<дата>.profdata.gz` из репозитория dArkOS (с sha256 в
  рецепте), `-fuse-ld=lld`, `AR=llvm-ar` (архивы с LTO-объектами);
- meson 1.12.1 из релизного архива (с sha256);
- `llvm-strip` и установка в `/usr/lib/aarch64-linux-gnu/malisx2/`;
- кэш по коммиту, рецепту и профилю: новый коммит — пересборка, около 33
  минут с нуля.

## Проверить свою сборку на устройстве

- `DARKOS_VULKAN=/путь/к/libvulkan_malisx2.so <программа>` — без замены
  системного драйвера;
- строка `<программа> = /путь/к/.so` в `/etc/darkos/vulkan.conf` — для игр из
  EmulationStation (переменные окружения до них не доходят); потом вернуть;
- `sudo mount --bind <новая.so> /usr/lib/aarch64-linux-gnu/malisx2/libvulkan_malisx2.so`
  — подменить системный до перезагрузки;
- перед запуском Vulkan-программы остановить EmulationStation (`sudo
  systemctl stop emulationstation`): она держит DRM master, и программа
  увидит 0 дисплеев. После — запустить обратно.
