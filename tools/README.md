# Инструменты форка

Рабочие инструменты, которыми проверяются и измеряются правки драйвера на RG52 Mini с dArkOS:
компилятор шейдеров вне консоли, тесты, скрипты замеров, разбор захватов и логов, снятие
профиля PGO. Пути на консоли (`/home/ark`, `/roms`, `/opt/...`, `/etc/darkos/vulkan.conf`) —
как в dArkOS.

Игр, сохранений, дампов шейдеров и чужих бинарников здесь нет: игры и сохранения — свои, дампы
снимаются на консоли. Дампы и трассы драйвера Arm содержат его скомпилированные шейдеры и в
репозиторий не кладутся ([../AGENTS.md](../AGENTS.md)).

## Что нужно

- **ПК** с Linux (годится WSL): bash, python3, ssh; `sshpass`, если вход по паролю; clang 19
  с sysroot arm64 — для программ, которые собираются под консоль
  ([../docs/building.md](../docs/building.md)).
- **Консоль** с dArkOS: ssh, `sudo` без пароля, tracefs; `ffmpeg` (снимки экрана через
  kmsgrab), `python3-evdev` (`press.py`), `glslangValidator` (шейдеры `fitest`).
- **Адрес консоли** — в окружении: `RG52_HOST` (обязательно), `RG52_USER` (по умолчанию `ark`),
  `RG52_PASS` (пароль для `sshpass`; без него ssh берёт ваши ключи).

      export RG52_HOST=<адрес> RG52_PASS=<пароль>
      bash tools/dev.sh uptime

## Состав

| каталог | что |
|---|---|
| `dev.sh` | команда на консоли по ssh; им пользуются остальные скрипты |
| `nirc/` | компилятор Bifrost из Mesa на дампах шейдеров, на ПК: правка компилятора проверяется за секунды, без консоли |
| `tests/` | `fitest` (инстансные отрисовки с `firstInstance`, прямые и indirect), `bctest` (текстуры BC), `segvtrace` (место падения без gdb) |
| `bench/` | замеры частоты кадров и времени GPU: ARMSX2, Dolphin, Azahar; снимки экрана |
| `hwcnt/` | аппаратные счётчики GPU (kinstr_prfcnt) |
| `capture/` | разбор захвата заданий `MALISX2_MEASURE=csf` |
| `logs/` | разбор логов драйвера: образы, статистика шейдеров, решения FP16 |
| `mtrace/` | трасса заданий, которые драйвер (malisx2 или libmali) отдаёт ядру, с разбором pandecode |
| `pgo/` | тренировка профиля PGO и сравнение профилей |

У каждого скрипта в начале написано, где он запускается («On the PC» или «On the console»),
что принимает и что печатает.

## nirc: компилятор вне консоли

1. Снять дампы на консоли: `MALISX2_NIR_DUMP=<каталог>` и `MALISX2_NO_PIPELINE_CACHE=1` в
   окружении эмулятора, кэш шейдеров самого эмулятора тоже убрать — иначе драйвер ничего не
   компилирует. На шейдер пишется файл `.mxnir`. Каталог перенести на ПК.
2. Собрать дерево под ПК с assert'ами (один раз):
   `meson setup build-host -Dbuildtype=debugoptimized --native-file=native.ini`.
3. `bash tools/nirc/build.sh` — пересобирает библиотеки компилятора из текущего `mesa/` и
   линкует `build-host/nirc`. `NIRC_OUT=<имя>` — другое имя, чтобы держать рядом сборки
   разных вариантов компилятора; `HOST_BUILD=<каталог>` — другой каталог сборки.
4. Запуск:

       build-host/nirc файл.mxnir...                             # строка на шейдер
       DUMPS=<каталог> bash tools/nirc/runall.sh <итог> [VAR=значение...]   # все дампы параллельно
       python3 tools/nirc/sum.py <итог>...                       # суммы по прогонам

   Строка на шейдер: время компиляции, размер и хэш кода (одинаковый код виден сразу),
   инструкции, клаузы, статические такты, регистры, вытеснения и заполнения — для главной
   половины и для varying-половины IDVS. Ручки компилятора (`BIFROST_MESA_DEBUG=...`) берутся
   из окружения, как на консоли.

Чего nirc не видит:

- **Правки на стороне malisx2.** Дамп снят после проходов драйвера (понижение дескрипторов,
  вынос uniform-слов в FAU, FP16): офлайн проверяется только компилятор Mesa. Правку прохода
  драйвера проверяют на консоли, сравнением логов `BIFROST_MESA_DEBUG=shaderdb`
  (`logs/sdbcmp.py`).
- **Порядок списков использований.** Дамп NIR его не сохраняет, и ошибка, которая зависит от
  порядка, офлайн не повторяется (так было с падением Luigi's Mansion 2 в `bi_make_vec_to`).
- **Другую Mesa.** Дамп читается сборкой с теми же структурами (`pan_compile_inputs`,
  `pan_shader_info`); nirc сверяет размеры и откажется.
- **Деление на IDVS**: оно решено до дампа. `NIRC_NOIDVS=1` компилирует разделённый вершинный
  шейдер целиком.

Время на x86 при 16 параллельных задачах примерно равно времени на одном ядре консоли.

## Тесты на консоли

Сборка — в начале каждого файла (clang 19 под aarch64). Запуск на консоли при остановленной
EmulationStation:

    DARKOS_VULKAN=/путь/к/libvulkan_malisx2.so ./fitest <каталог с fi.vert.spv и fi.frag.spv> indirect
    DARKOS_VULKAN=/путь/к/libvulkan_malisx2.so ./bctest
    LD_PRELOAD=/home/ark/segvtrace.so <программа>     # при падении: адрес, PC и LR как библиотека + смещение

Код возврата 0 — всё совпало. `fitest` и `bctest` стоит прогонять после любой правки
отрисовки v7 и после слияния с апстримом.

## Замеры

Правила — в [../docs/debugging.md](../docs/debugging.md), раздел «Замер скорости». Коротко:

- сравнивать только одинаково собранные библиотеки (обе LTO + PGO или обе без);
- круг прогрева, потом не меньше двух кругов с переменой порядка; разница меньше 3 % на
  ARMSX2 требует трёх кругов и больше;
- в строке результата есть температура и частота CPU: круг, где SoC сбросил частоту, не в
  счёт;
- у каждого драйвера свой кэш конвейеров эмулятора;
- Azahar идёт в такт с переворотами страниц даже без ограничения кадров: сравнивать время
  GPU на кадр (`overlap.py`), а не только к/с;
- прогон с сохранения эмулятора может переписать настоящие сохранения игры (карты памяти
  PS2, `sdmc` у 3DS): сначала копия;
- после прогонов — `sudo dmesg | grep -i 'gpu fault'` и снимок экрана: картинка должна
  совпадать.

Примеры:

    # ARMSX2: две библиотеки, прогрев + 2 круга, God of War с сохранения и вступление Jak
    GOW_STATE=/home/ark/bench/gow.p2s bash tools/bench/armsx2-ab.sh 3 \
      a=/home/ark/libvulkan_malisx2-a.so b=/home/ark/libvulkan_malisx2-b.so@MALISX2_V7_FP16=0

    # Dolphin: Metroid Prime и ND Demo
    DOLUSER=/home/ark/dolphin-user bash tools/bench/dolphin-ab.sh 3 a=/home/ark/a.so b=/home/ark/b.so

    # Azahar: скрипт работает на консоли
    bash tools/dev.sh "cat > /tmp/azahar-bench.sh" < tools/bench/azahar-bench.sh
    bash tools/dev.sh "cat > /tmp/overlap.py" < tools/bench/overlap.py
    bash tools/dev.sh "MBROM='/roms/n3ds/<игра>.3ds' bash /tmp/azahar-bench.sh a \
      DARKOS_VULKAN=/home/ark/a.so MALISX2_STATS=5 GPUTRACE=1 GRABS=300,350
      python3 /tmp/overlap.py /tmp/cvtrace-a.txt; sudo systemctl start emulationstation"

`armsx2-ab.sh` на время прогонов дописывает строку `armsx2-sdl = <путь>` в
`/etc/darkos/vulkan.conf` и подменяет лаунчер PS2 привязкой (`mount --bind`); в конце
возвращает всё сам. Если его прервали:

    bash tools/dev.sh 'sudo cp /tmp/vulkan.conf.ab-orig /etc/darkos/vulkan.conf
      sudo umount /usr/local/bin/standalone-pcsx2sa; bash /tmp/estest.sh stop'

`slots.py` и `pace.py` читают ту же трассу, что `overlap.py`: загрузка слотов заданий и такт
кадров. `grab.sh` снимает экран в PPM, `avgrgb.py` сравнивает снимки по среднему цвету,
`ppm2png.py` переводит в PNG. `clk.sh` печатает частоты GPU, CPU и DDR и температуру.

## Аппаратные счётчики

    clang-19 --target=aarch64-linux-gnu --sysroot=<sysroot> -O2 tools/hwcnt/mhwc.c -o mhwc
    sudo ./mhwc 20 20 > a.hwc        # на консоли: через 20 с после старта, 20 с подряд
    echo "F <кадров за это окно>" >> a.hwc
    python3 tools/hwcnt/hwc-ana.py mesa/src/panfrost/perf/database/hardwarelayout/Mali-G52.xml a.hwc b.hwc

Счётчики принадлежат GPU, а не контексту: так же измеряется и libmali. Что они значат на
G52 — в [../docs/hardware.md](../docs/hardware.md).

## Захваты и логи

- `capture/jmdec.py <jm-NNNNNNNN.bin> [--fault 0xАДРЕС]` — задания одного submit'а из захвата
  `MALISX2_MEASURE=csf,dir=<каталог>`: цепочки, отрисовки, диапазоны чтения атрибутов; с
  `--fault` помечает задания, которые могли прочитать этот адрес. Порядок поиска упавшего
  задания — в [../docs/debugging.md](../docs/debugging.md).
- `capture/jcdec.py <каталог>` — сколько заданий какого типа отправлено.
- `logs/imglog.py <лог>` — образы прогона с `MALISX2_LOG_IMAGES=1` по форматам и раскладкам.
- `logs/sdbcmp.py <FRAGMENT|VERTEX> <лог A> <лог B>` — статистика одних и тех же шейдеров в
  двух логах `BIFROST_MESA_DEBUG=shaderdb`.
- `logs/fp16log.py <лог>` — решения прохода FP16 (`MALISX2_V7_FP16_LOG=1`).

## mtrace

`LD_PRELOAD`-библиотека для консоли: записывает каждый JOB_SUBMIT клиента kbase и разбирает
цепочки заданий pandecode из Mesa. Работает и с malisx2, и с libmali — так сравнивается, что
два драйвера отдают GPU на одной сцене (форматы, маски тайлера, состояние отрисовок, шейдеры).

    bash tools/mtrace/build.sh                 # нужна кросс-сборка дерева без LTO в build-dev;
                                               # результат — build-dev/mtrace.so, скопировать на консоль
    LD_PRELOAD=/home/ark/mtrace.so MTRACE_DIR=/tmp/mt MTRACE_AFTER=30 MTRACE_COUNT=10 <эмулятор>
    python3 tools/mtrace/pdsum.py <дамп pandecode>...

Переменные — в начале `mtrace.c`.

## Профиль PGO

Как собирать с инструментированием и как сливать профили — в
[../docs/building.md](../docs/building.md). Тренировка:

- `pgo/train-azahar.sh <инструментированная .so>` — на консоли: игры Azahar с удалённым кэшем
  конвейеров malisx2 (компиляция шейдеров);
- `pgo/train-armsx2.sh <.so на консоли> '<игра>|<сохранение или ->|<секунд>[|<нажатие>]'...` —
  с ПК: ARMSX2 (путь отрисовки);
- `pgo/profcmp.py <новый.proftext> <старый.proftext>` — какая доля старого профиля ещё
  применима к новому коду: по ней решают, переснимать ли профиль.
