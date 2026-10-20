# LSX4 Android: полный контекст для общего Codex-чата

> [!IMPORTANT]
> Before any device/FPS/playable measurement, run `scripts/preflight-vivo-performance-test.ps1`;
> stale Android Studio process-trackers and known profiler loads make the result invalid. See the
> mandatory [performance-test preflight](PERFORMANCE_TEST_PREFLIGHT.md).

> Актуальная точка продолжения на 2026-08-11:
> [`JIT_BACKEND_PERF_HANDOFF_2026-08-11.md`](JIT_BACKEND_PERF_HANDOFF_2026-08-11.md).
> Она заменяет приведённые ниже performance-цифры, состояние устройства/маркеров и приоритет
> следующих шагов. Этот документ остаётся историей реализации и уже закрытых дефектов.

Срез: **2026-08-01, Europe/Moscow**. Репозиторий:
`D:\LProj\PetProj\lsx4-jit-independent-wt`.

Этот документ предназначен для вставки или передачи в новый общий/мозговой
чат. Он объединяет устойчивые знания длинной сессии, текущее состояние кода и
устройства, подтверждённые причины регрессий, неудачные направления и задачу
для следующего мозгового штурма.

Новый чат должен сначала прочитать этот документ и связанные документы,
потом дождаться от пользователя способа подключения/явной команды на работу.
До этого не выполнять ADB-команды, не менять код и не запускать сборку.

## 1. Короткая текущая точка

- Главная цель: довести **Bloodborne (`CUSA03173`) до устойчивых 30 FPS при
  960x544** на текущем Vivo без графических регрессий, 0-FPS зависаний и
  переноса нагрузки из одного потока/слоя в другой.
- Последний полностью проверенный визуально корректный запуск доходил до
  Hunter's Dream и показывал **15 FPS**, но был сделан при `skin=48.0 C` и
  thermal status 4, поэтому не является регрессией относительно исторических
  18–22 FPS. До нагрева тот же класс тяжёлых сцен доходил до 20–22 FPS; чистый
  cold deterministic A/B ещё нужен.
- Последние ADPF-отчёты показывали фактические окна кадра примерно 33–83 мс,
  чаще 50–58 мс. Для 30 FPS нужно 33.3 мс.
- Старые профили показывали CPU-bound картину: около 64.8–64.9% CPU внутри
  сгенерированного JIT-кода; `RunGuest` 11.8–12.5%; Vulkan driver около
  1.7–1.9%; `GpuComm` около 8.6%; syscalls около 6.1%; `mprotect` около 1.5%.
  Один горячий FMOD-регион занимал менее 2%, то есть цена размазана по всему
  корпусу кода, а не сосредоточена в одной функции.
- В одном предыдущем профиле GPU при 960x544 занимал около 8 мс. Это нужно
  перепроверить на текущем срезе, но основной открытый предел пока CPU/JIT.
- Все семь задуманных классов управляемых оптимизаций присутствуют в коде, но
  **«7/7 реализовано» не означает «7/7 реально срабатывает и даёт выигрыш»**.
  Ранее несколько путей были выключены guard'ами, имели `callsSaved=0` или
  компенсировали выигрыш собственной диагностикой.
- Сейчас на Vivo запущен новый build с переработанным T2 loop planner; маршрут
  до геймплея выполняется. Перед запуском один skin sensor всё ещё показывал
  47.0 C/status 3, поэтому следующий HUD нельзя считать cold A/B.

## 2. Жёсткие требования пользователя

1. Сейчас тестировать только на явно указанном Vivo. **Xiaomi не трогать** ни
   одной командой, пока пользователь явно не изменит это правило.
2. Любая ADB-команда должна содержать точный `-s`; не использовать первый
   найденный target. В списке также есть offline emulator.
3. Игру запускать прямым параметром title/path. Не листать каталог: высок риск
   выбрать другой тайтл.
4. После запуска самостоятельно пройти известную последовательность до
   геймплея и двигать персонажем. Не ждать пользователя для кликов.
5. Скриншоты смотреть только при неожиданном состоянии маршрута. Для
   мерцаний, ABA/старых кадров и кратких артефактов использовать 12–30-секундный
   60-FPS скринкаст: одиночный скрин не диагностичен.
6. Не тратить минуты на синтетические тесты, если проблема находится до
   первого кадра. После каждого крупного пакета нужен быстрый запусковой gate;
   полные матрицы — после функционального кадра или перед фиксацией.
7. Изменения должны быть общими, title-independent и управляемыми галочками.
   Не хардкодить title ID, guest address, shader hash или конкретный кадр.
8. Общий JIT используется PS4 и PS5, но PS4 нельзя ломать. Изолированный PS5
   runtime и его 16-KiB контракты нельзя смешивать с PS4 4-KiB страницами.
9. Запрещена «оптимизация переносом»: ускорение Game:Main ценой FMOD/Havok,
   снятие load с CPU ценой скрытого GPU stall или уменьшение статтера без
   уменьшения постоянной работы не считается достижением цели 30 FPS.
10. Нужны крупные связные пакеты, бьющие в общую цену гостевой операции.
    Неделя уже была потеряна на микроправки и дрейфующую базу.
11. Разрешение для сравнения — **960x544**. Старые 360p, 720p и случайные 1080p
    результаты не сравнивать напрямую.
12. Coarse fragment 2x2 сильно портит кадр; оставлять отдельной опцией и по
    умолчанию выключенным.
13. Если новый путь даёт 0 FPS, audio-only продолжение, порчу шрифтов или
    мерцание, сначала считать это нарушением state/sync/lifetime-контракта, а
    не «медленной загрузкой» или проблемой метрики.

## 3. История задачи в одном абзаце

Работа началась с изолированного PS5 runtime на основе SharpEmu-семантики:
loader, relocations, TLS/DTV, module graph, отдельная `.so`, PS5 ABI и ранний
Gen5/Vulkan путь. Затем PS5 импорт был встроен в тот же Android-каталог, что и
PS4: общий список, PS5 badge, те же item/launch/input сценарии. Две небольшие
PS5-игры, включая Dreaming Sarah, были доведены до меню/геймплея; общие audio,
input и frame-contract исправления проверялись итеративно. Третья большая игра
Demon's Souls (`PPSA01342`, около 80 GB) дошла до splash/раннего GPU пути, но
меню и геймплей не завершены; работа была поставлена на паузу как слишком
тяжёлая для текущего синхронного Gen5 renderer. После этого фокус перешёл на
общие PS4/PS5 JIT, Vulkan, readback, cache и mobile GPU оптимизации, а
Bloodborne стал основным CPU-бенчмарком. Параллельно Nidhogg 2, Dead Cells,
Limbo и другие игры использовались как строгие regression controls.

## 4. Репозиторий, ветки и сохранность дерева

### Зафиксированный HEAD

- Основной репозиторий: branch `main`, HEAD
  `a09197bf24a76b91a8415bb33d4e45f7bfa67948`
  (`perf: optimize tiered JIT and mobile runtime`).
- `main` совпадает с `lsx4/main`: ahead 0, behind 0.
- Funnel submodule: branch `main`, HEAD
  `58fbbe860190652ff21908b676f95b0e758fe9ce`
  (`perf: stabilize mobile Vulkan synchronization`).
- Funnel совпадает с `origin/main`: ahead 0, behind 0.

Последние ключевые main-коммиты:

```text
a09197bf2 perf: optimize tiered JIT and mobile runtime
be51f9d19 fix: keep faultable trace startup safe
a3693601d fix: gate adaptive resolution on measured GPU pressure
697ee8eb0 perf: recover faultable T2 register state from signals
26f1b4c87 perf: remove legacy fault frames from chained JIT
63e23aeee perf: widen active JIT regions adaptively
c5963750d perf: resume faultable JIT regions from signal context
f97109330 perf: widen resident JIT register state
6626f2bd9 perf: coalesce guest store barriers across JIT edges
e677328ee perf: cache hot guest registers in linked blocks
94fe88378 perf: keep hot guest registers resident across JIT blocks
bd9660ba4 perf: checkpoint managed JIT and Vulkan optimizations
```

### Незакоммиченный слой поверх HEAD

Дерево сейчас намеренно dirty. Не выполнять `reset`, `checkout`, `clean`,
`stash`, массовое форматирование или удаление artifacts. Не считать изменения
своими без проверки: в дереве шла параллельная работа.

Tracked changes основного репозитория:

```text
android-app/app/src/main/AndroidManifest.xml
android-app/app/src/main/cpp/runtime_bridge.cpp
android-app/app/src/main/java/app/lsx4/android/MainActivity.java
android-app/app/src/main/java/app/lsx4/android/RuntimeBridge.java
android-app/app/src/main/java/app/lsx4/android/SettingsActivity.java
android-app/app/src/main/res/values-ru/strings.xml
android-app/app/src/main/res/values/strings.xml
src/executor/android_performance_hint.cpp
src/executor/native_runtime_api.cpp
funnel-arm (modified submodule content)
```

Новые важные файлы:

```text
android-app/app/src/main/java/app/lsx4/android/PlatformPerformanceController.java
android-app/app/src/main/res/xml/game_mode_config.xml
```

В Funnel dirty:

```text
src/video_core/mobile_gpu_optimization.cpp
src/video_core/mobile_gpu_optimization.h
```

`externals/fmt`, `externals/openal-soft`, Funnel Rust target и сотни PNG/MP4,
perf-map/data/log/bin artifacts тоже dirty/untracked. Ничего из этого не
чистить автоматически.

На момент среза `git diff --check` не выдавал ошибок, только предупреждения о
будущем LF -> CRLF для части Android/C++ файлов.

## 5. Что находится в незакоммиченном performance-пакете

Это не просто косметика; его нельзя случайно потерять при начале мозгового
штурма.

### Android/ADPF

- Приложение объявлено игрой: `android:appCategory="game"`,
  `android:isGame="true"`.
- Добавлен Android game-mode config: performance mode поддержан, системный
  downscale/FPS override запрещены, потому что разрешением управляет renderer.
- Java loading-session через `PerformanceHintManager` может временно собрать
  до 10 активных thread TID и передаёт управление native frame-session после
  первого present.
- Native ADPF поддерживает Android 12+ через динамический NDK API; на API 36
  дополнительно использует workload increase/reset и
  `setPreferPowerEfficiency(false)`.
- Native frame-session выбирает наиболее активные `Game:Main` и `GpuComm`,
  обновляет TID раз в две секунды и сообщает скользящее окно из четырёх
  presents.
- Target CPU work установлен в 25 ms при 30-Hz frame budget. Это сделано,
  чтобы CPU producer завершался раньше present deadline; значение требует
  A/B, но не должно подменяться случайным HUD.
- Путь пытается поставить `uclamp.min=512` и nice=-4. На Vivo `uclamp`
  возвращал `EPERM`, nice применялся успешно.

### Vivo performance mode

- `PlatformPerformanceController` сообщает `GameState` loading/gameplay.
- Для Vivo используется `VivoPerfManager` при доступе; из-за hidden-API
  ограничений есть безопасный fallback на
  `/system/bin/service call vendor.vivoperfservice`.
- Vendor hint: `VENDOR_HINT_PERFORMANCE_MODE = 0x1091`, finite duration 30 s,
  renewal через 20 s, старый handle явно освобождается.
- В реальном запуске binder путь выдавал и продлевал handles
  `9744 -> 9759 -> 9766 -> 9854 -> ...`; release вернул 0.
- Опциональная root-галочка умеет временно выставлять performance governor и
  Adreno min=max с восстановлением исходных значений. На текущем Vivo root
  отсутствует; галочка по умолчанию выключена.

### Реальное внутреннее разрешение

- `res_mode=4` означает 960x544.
- Surface/output setting дополнен native bridge
  `executor_lsx4_runtime_set_render_scale_percent`.
- Для 544p текущий Java mapping использует 900p reference и просит 60%; лог
  последнего запуска: `requested=60 applied=60 adaptive=0`.
- Funnel хранит `forced_scale_percent`, который имеет приоритет над adaptive
  scale и влияет на physical DRS/FSR/coarse policy.
- Это исправляет прежний класс ошибок, когда UI показывал 960x544, а гостевые
  attachments оставались 1080p. Однако в следующем чистом профиле всё равно
  требуется строка фактического GNM/render-target extent: 60% bridge ещё нужно
  доказать как правильные 960x544 real attachment work, а не второй downscale.

### Мелкий общий runtime tax

Несколько часто вызываемых `getenv()` в HLE boundary переведены в process
static. Это безопасное сокращение, но не считать его крупным FPS-результатом.

## 6. Текущее устройство и воспроизводимый запуск

### Устройство

```text
serial: adb-10AG263F1B001HM-2MNzzP._adb-tls-connect._tcp
model: V2546A
Android API: 36
GPU family: Qualcomm Adreno
package: app.lsx4.android
game process: app.lsx4.android:game
app targetSdk: 35
app version: 0.1.0
```

`cmd game` сейчас сообщает:

```text
current mode: performance
available: standard, performance, custom
```

Последний cold read CPU policy при незапущенной игре:

```text
policy0 cpuinfo_max=3628800 scaling_max=3628800
policy6 cpuinfo_max=4608000 scaling_max=3648000
```

Ранее под нагрузкой caps наблюдались ниже: около 2.36 GHz и 2.44 GHz. Vendor
performance hint закреплял текущую частоту у действующего cap, но не всегда
поднимал сам `scaling_max`. Поэтому cap динамический/OEM policy dependent;
нельзя объявлять простой thermal throttle или гарантированный x1.8 без
одновременного чтения частот, температуры и workload.

Thermal service сейчас имеет status 0. В выводе встречаются старые/дублируемые
99.6 °C records и текущие около 31 °C; использовать status и свежие skin/battery
records, а не одиночную старую строку.

### Bloodborne

```text
title: CUSA03173
path: /data/user/0/app.lsx4.android/files/lsx4-home/runtime-fs/user/app/CUSA03173/eboot.bin
platform: ps4
```

Точный запуск:

```powershell
$dev = 'adb-10AG263F1B001HM-2MNzzP._adb-tls-connect._tcp'
adb -s $dev shell am start -n app.lsx4.android/.MainActivity `
  --es launch_game_path /data/user/0/app.lsx4.android/files/lsx4-home/runtime-fs/user/app/CUSA03173/eboot.bin `
  --es game_platform ps4
```

Основная кнопка X для известного маршрута:

```powershell
adb -s $dev shell input keyevent 96
```

Не копировать старые координаты Xiaomi: разрешение/ориентация Vivo другие.
Сначала использовать известные keyevent; touch — только при необходимости.

### Сборка и установка

Native:

```powershell
& 'C:\Users\Litesav\AppData\Local\Android\Sdk\cmake\3.22.1\bin\cmake.exe' `
  --build build\android-arm64-release --target lsx4_executor_android -j 8
```

Android:

```powershell
android-app\gradlew.bat assembleDebug --no-daemon
```

APK сам по себе не содержит актуальную native `.so`; runtime копируется в
sandbox отдельно:

```powershell
adb -s $dev push build\android-arm64-release\liblsx4_executor_android.so `
  /data/local/tmp/liblsx4_executor_android.so
adb -s $dev shell run-as app.lsx4.android cp `
  /data/local/tmp/liblsx4_executor_android.so `
  files/runtime/liblsx4_executor_android.so
```

Критическое уточнение, найденное после исходного среза: процесс игры реально
загружает именно `files/runtime/liblsx4_executor_android.so`. Старые команды
часто обновляли только `files/liblsx4_executor_android.so`, из-за чего несколько
итераций фактически тестировали прежний код. До дальнейшего изменения loader
deploy обязан обновлять **оба** пути и сверять hash активного runtime-файла:

```powershell
adb -s $dev shell run-as app.lsx4.android cp `
  /data/local/tmp/liblsx4_executor_android.so `
  files/liblsx4_executor_android.so
adb -s $dev shell run-as app.lsx4.android cp `
  /data/local/tmp/liblsx4_executor_android.so `
  files/runtime/liblsx4_executor_android.so
adb -s $dev shell run-as app.lsx4.android sha256sum `
  files/runtime/liblsx4_executor_android.so
```

Хостовая и активная device `.so` на текущем срезе совпадают:

```text
SHA-256 043673D9DF5E17308248317DC8D39DEBC0A4A9782D99D15FFD3BD93A03EF8CA6
deployed 2026-08-01 15:03 Europe/Moscow
```

Последний debug APK:

```text
SHA-256 77766C3E11FF2E10235487D0E60334B85BBD99FE947666DC57C863FAAA7F0137
```

## 7. Активные настройки последнего запуска

```text
resolution: 960x544 (res_mode=4)
performance HUD: on
ADPF: on
ARM GPU fast path: on
tiered JIT: on
JIT trace compilation: on
persistent JIT cache: on
fast guest memory: on
Vulkan driver-call optimization: on
adaptive mobile GPU: on
async pipeline: on
readback batching: on
audio SIMD: on
relaxed FP fusion: on
anisotropy mode: 2
coarse fragment 2x2: off
disable Vulkan robustness: off
motion blur/AA/chromatic aberration/DoF/SSAO/dynamic shadows: disabled
```

Persistent cache checkbox управляет полным путём decoded/native JIT, hot PGO,
shader и Vulkan pipeline cache. В Settings уже есть вертикальный per-game
список: icon/title, JIT/Vulkan size и delete action; выключение запрещает
load/write со следующего запуска, существующие файлы остаются до удаления.

## 8. Текущее состояние JIT по device JSON

Это cumulative status последнего запуска, не per-frame профиль:

```text
decoded/native blocks: 333116 / 333116
persistent IR: hits 332583, misses 533, loaded 334126
persistent native: restored 316516 segments, hits 158258

T1: queued 2914, compiled/active 1601, rejected 1313
T1 generated: 1,899,404 bytes

T2: queued 3132, compiled 1212, faultable 1116
T2 active 1004, retired 208, rejected 1920
T2 member blocks 3170, guest instructions 20075
T2 generated 1,751,496 bytes; active 1,417,428 bytes
T2 entries 1,808,735,552; internal edges 3,118,915,904
guard deopts 3,762,951; guard rollbacks 62

native SIMD blocks 18293; fully direct 18290
XMM residency blocks 9995; halves 28381
estimated XMM loads/stores avoided 185482

native scalar blocks 172461; direct instructions 782333
GPR-cache blocks 140625; registers selected 1156727
estimated GPR loads/stores avoided 64863

REP MOVS bulk calls 3,652,190; bytes 155,708,267
leaf HLE fused blocks 2102
```

Числа показывают важную проблему: coverage GPR selection большой, но оценка
реально снятых loads/stores мала относительно корпуса; XMM residency тоже
охватывает лишь часть блоков. Огромный cumulative internal-edge count не
доказывает, что state traffic снят: переход может быть native, но продолжать
писать machine image.

## 9. Реализованная архитектура оптимизаций

### Seven managed areas

1. A/B controls, runtime routing, HUD/live counters.
2. CPU-side readback sidecar, submit snapshot, coalesced page copies, timeline
   wait.
3. Vulkan state/descriptor dedup, barrier/render-scope coalescing,
   command-buffer reuse and submit batching.
4. T0/T1/T2 JIT, loop OSR, guarded deopt, per-title PGO, persistent
   decoded/IR/native artifacts.
5. Fast/optimized shader tiers, bounded background pipeline work, disk and
   negative caches.
6. Mobile GPU scale/upscale, anisotropy, residency budget, optional fragment
   shading rate.
7. Batched/SIMD audio with event pacing and underrun guards.

### Tiered JIT

- T0: immediate legacy-compatible native block.
- T1: hot local background recompile with retained T0 fallback.
- T2: one low-priority worker, multi-block paths/closed loops, guards,
  rollback and loop OSR.
- T2 code is not persisted; per-title edge PGO and member IR/native are.
- Every composite witness is revalidated before publish; SMC retires trace.
- Closed loops require a periodic safepoint. A native cycle must never be
  published as a straight-line trace.

### Host ISA work already present

- Runtime HWCAP profile and fail-closed feature gating exist.
- FlagM/FlagM2 lowering uses `SETF8`, `SETF16`, `RMIF`, `CFINV` on supported
  devices; fallback remains for other devices.
- RCpc path emits `LDAPR` where its memory semantics were proven safe; ordinary
  baseline remains on devices without LRCPC.
- SVE2 path exists for selected `VPSHUFB256`/permutation cases with NEON
  fallback.
- JIT stubs were moved to
  `src/executor/dynamic_translation/jit_aarch64_stubs.S` and built through
  CMake. Unrelated Funnel platform primitives still contain small inline asm
  (`rdtsc`, TLS, spin wait, traps); do not mistake those for JIT helper ABI.

Этот ISA-пакет обязателен, но сам по себе не дал 30 FPS: постоянный machine
state traffic и observer boundaries всё ещё доминируют.

## 10. Подтверждённые renderer/memory причины и инварианты

### Previous-frame/partial-frame flicker

Первичная причина Nidhogg 2: логическое состояние page watcher расходилось с
реальной protection, а обычный fault спекулятивно инвалидировал соседние
страницы. Рабочие инварианты:

- watcher protection синхронна;
- обычный write fault инвалидирует ровно одну 4-KiB PS4 page;
- широкий range только для доказанного bulk write;
- никаких polling wait, фиксированных delays или title hardcodes.

Acceptance делается скринкастом. Подтверждённый strict-watcher run: 30 s,
1801 frames, severe=0, ABA=0.

### Bloodborne late-render-scope regression

Глобальный late dynamic-rendering scope возвращал missing character/terrain,
white regions и разноцветные деревья. Обычный Qualcomm path должен оставаться:

```text
BindResources/barriers -> BeginRendering -> pipeline -> vertex/index -> draw
```

Late scope допускается только при active physical DRS target или pending
transfer, который ещё может завершить scope.

### Nidhogg 2 Adreno DEVICE_LOST

Read-only rename использовал общий `StreamBuffer` ring как backing UBO/SSBO,
а slice переиспользовался до GPU completion. Barrier/flush обеспечивают
visibility, но не lifetime. Исправление: Uniform/Storage snapshots hard-gated
на stable tracked buffer-cache backing; vertex/index rename остаётся.
Fingerprint сбоя: `0x721c15d6dc5462f3`.

### Dead Cells nondeterministic missing texture layers

Причина: cross-request delayed upload batching переживало lifetime staging/ring
source. Между cold launches одна и та же сцена случайно теряла masonry,
foreground и weapon layers при живых UI/particles.

Исправление: coalescing только внутри одного upload request, затем немедленно
materialize copy. HLE/GNM DMA могут иметь отдельный доказанный lifetime.
Barrier shape не продлевает allocation lifetime.

### Audio

- Nidhogg 1/Shovel Knight/Owlboy: общий SysV variadic contract, 8 XMM lanes и
  176-byte register save area; не forced gain/title patch.
- Dead Cells NGS2: canonical format/resampling и system render grain 256, а не
  полный 4096-capacity buffer.
- Bloodborne использует FMOD/AudioOut control path и должен оставаться
  non-regression control.

## 11. Подтверждённые причины 0 FPS и запрет повторять их

1. **Mode-2 full GPR layout across chains.** 12 selected GPRs переносились в
   caller-saved `x1/x2/x3/x5`; не все fault/helper/HLE/scheduler landings их
   сохраняли. Bloodborne детерминированно останавливался около 203297 compiled
   blocks. Dirty layout bits и read-only narrowing не помогли; был
   `jit_host_access_no_resume` на `0x100000`. Mode 2 hard-gated.
2. **Unbounded fault sidecar.** Descriptor предыдущей memory instruction
   оставался активным на control/metadata code. Game:Main застывал на 0 FPS,
   audio продолжалось. Каждый descriptor теперь ограничен точным host-PC
   interval и закрывается zero-publication marker.
3. **Trace cycle classified as straight line.** Planner находил backedge после
   начала построения, но публиковал `loopOsr=0`; native self-cycle обходил
   scheduler/witness safepoint. Exact seed closure теперь loop OSR, цикл в
   другой member rejected/reprofiled.
4. **Direct RET chaining.** Кадр появлялся, затем цепь переставала отдавать
   управление scheduler/HLE. Идея полностью снята; sentinel root был
   недостаточен. Если возвращаться к теме, нужен bounded quantum, а не
   бесконечное chaining.
5. **Write-back без полного observer contract.** Попытки снять write-through
   faultable regions зависали до кадра. Без exact register recovery metadata
   для каждого observer это state-authority corruption.
6. **GPR cache through implicit/helper boundaries.** IDIV/BT и prefix variants
   давали crash/freeze; локальное сокращение native region не оправдывало
   нарушение boundary.
7. **Nidhogg 2 death/match transition.** Это был не HUD 0, а delayed Vulkan
   device loss из-за descriptor snapshot lifetime; исправлять JIT ожиданием
   было бы ошибкой.

Главный общий инвариант: любой наблюдатель состояния должен быть явным —
fault signal, guard rollback, computed edge, HLE/helper, event/interrupt,
bounded quantum landing. На каждом escape exact dirty state либо
восстанавливается из metadata, либо публикуется до выхода.

## 12. Что уже пробовали и почему это не крупный путь

- Front block lookup cache: task-clock стал хуже примерно на 1.4%, RunGuest
  10.86% -> 11.66%; снято.
- Broad deferred-flags contract: зависание около 128778 blocks до первого
  submit; снято. Дешёвый FlagM lowering предпочтительнее ослабления authority.
- Broad GPR write-back: заменил 148 loads, но добавил 144 stores и 66 moves;
  region вырос 7044 -> 7352 bytes. Это перенос работы, снято.
- RET chaining: 0 FPS из-за потери scheduler/HLE observation; снято.
- GPR prefix cache through IDIV/BT: freeze около 126764 blocks при выигрыше
  region всего около 4.5%; снято.
- Общий BIF scalar merge: горячие regions выросли на 28–36 bytes и control
  frame просел; снято.
- Узкий pair fusion не совпадал с реальным FMOD pattern из-за перемежающегося
  `VMOVSS`; сам по себе не мог изменить FPS.
- Blanket UBO/SSBO stream rename, cross-request upload deferral и глобальный
  late scope ускоряли путь, но возвращали device loss/texture loss/flicker.
- Shader/pipeline caches, sparse coherence, submit batching и persistent
  caches в основном убрали редкие дорогие события и статтеры. Это полезно, но
  не уменьшило постоянную цену гостевой операции достаточно для 30 FPS.
- Coarse 2x2 действительно может резко уменьшить fragment work, но визуально
  неприемлем как default и не попадает в текущий CPU bottleneck.
- Понижение разрешения ниже 960x544 раньше почти не меняло FPS, потому что GPU
  не был главным пределом; часть старых тестов ещё и не применяла реальное
  внутреннее разрешение.

## 13. Почему прогресс раньше казался положительным, но FPS не рос

1. Диагностический readback trace, частая PGO запись, повторная transfer-range
   validation, completion reconstruction и TLS resolution съедали полученный
   выигрыш.
2. Некоторые оптимизации формально были ON, но фактически не исполнялись:
   Vulkan batch имел maxBatch=1/callsSaved=0; game patch guard ожидал APP_VER
   01.09 вместо фактического 01.00; T2 отвергал основной faultable loop.
3. Механизмы делались частично: 4/16 GPR residency без signal fault resume и
   без снятия write-through почти ничего не меняла.
4. Baseline дрейфовал между 720p, 1080p, 360p, 544p, cold/hot cache и разными
   сценами. Падение принималось за новую базу, а последующее частичное
   восстановление — за прирост.
5. В кадре одновременно существовали сотни completion/wait/draw операций,
   page protection и PM4. Локальное снятие одного равного слагаемого давало
   1–2 FPS и открывало следующее.

Отсюда новое правило: любой performance claim должен уменьшать **общий
task-clock/retired instructions/guest-op cost** в одном и том же сценарии, а
не только FPS HUD или долю одной функции.

## 14. Главный оставшийся архитектурный пакет

Цель при 20 FPS: сократить около 16.7 ms из 50 ms кадра, то есть примерно 33%.
При 18 FPS требуется около 22.2 ms, то есть около 40%. Это не возьмётся суммой
нескольких getenv/lookup правок.

Самое перспективное связное направление:

1. **Dedicated callee-saved native guest-state ABI.** Полный GPR/XMM carrier
   должен жить в гарантированно сохранённых host registers, а не в x1/x2/x3/x5.
2. **Cross-block SSA/liveness.** Удерживать значения через T0/T1/T2 chain,
   материализовать flags и machine image только в точках реального
   наблюдения.
3. **Exact host-PC recovery maps.** На каждой fault/deopt/helper/interrupt
   точке описывать `guest PC <-> host PC <-> register layout/dirty mask`.
   Signal handler восстанавливает точное state из host context. Это заменяет
   blanket write-through, не ослабляя fault correctness.
4. **Custom JIT helper ABI in `.S`.** Helper stubs должны объявлять минимальный
   clobber set и сохранять carrier. Обычный C++ AAPCS вызов заставляет
   `SpillAll()` и обесценивает residency.
5. **Bounded continuous native chain.** Dispatcher/HLE/events получают
   гарантированный quantum landing раз в N переходов/возвратов; внутри quantum
   обычные edges/RET не делают полный state roundtrip. Нельзя повторять
   бесконечный RET chain.
6. **Flags as cheap lowering, not risky laziness.** Расширить FlagM/FlagM2 на
   все частые 8/16/32/64-bit CMP/TEST/ADD/SUB/Jcc patterns; PF/AF вычислять
   минимальными проверенными sequences. Измерять instruction count на общем
   корпусе.
7. **SIMD/NEON pattern lowering.** TBL/TBX для shuffle/pack/unpack, нативные
   REP MOVS/STOS и общие FMOD/Havok memory-shuffle patterns. Не хардкодить
   адрес конкретного mixer loop.
8. **Guest spin-wait recognition.** Найти polling loops, которые в нативном
   x86 ожидали event/futex, а сейчас активно жгут ARM CPU; безопасно park/wake
   только при доказанном memory/event contract.

Это нужно проектировать одним ABI-пакетом. Частичная residency без recovery
metadata уже трижды приводила к 0 FPS.

Второй эшелон после измеримого JIT-сокращения:

- page epochs и объединение protect/unprotect по диапазону/submit при строгих
  watcher invariants;
- PM4 -> canonical Vulkan state compiler, state-delta/render-scope DAG и
  устранение повторных bind/push/barrier;
- managed scene simplification, которое реально пропускает/упрощает дорогие
  desktop effects, а не только меняет final presenter scale;
- driver/Turnip-like paths там, где устройство и лицензия позволяют.

## 15. Что показало сравнение с FEX, Box64, Winlator и GameHub

- Обычное unrooted Android-приложение не может универсально записывать
  cpufreq/GPU sysfs или гарантировать max clocks. Winlator/GameHub выигрывают
  главным образом не «магической частотой», а архитектурой Box64/FEX,
  affinity, driver/Turnip path, pipeline caches и уменьшением общего объёма
  работы.
- Winlator на устройстве — version 11.0, targetSdk 28; GameHub — 6.0.9,
  targetSdk 36. Статический осмотр APK показал чтение частот/root detection,
  но не универсальный non-root hard clock forcing.
- У Vivo есть OEM service `vendor.vivoperfservice` и framework
  `com.vivo.framework.vperf.VivoPerfManager`; текущий пакет уже использует его
  легальный finite hint. Возможен OEM allowlist, который нельзя честно
  воспроизвести обычным package name.
- Главное, что стоит перенять у FEX/Box64: единый thread context, callee-saved
  SRA ABI, signal-based fault reconstruction, широкий IR/liveness до
  emission, cheap strong-memory lowering, специализированные SIMD/string
  primitives и bounded dispatcher exits.

Полезные внешние референсы:

- `https://github.com/FEX-Emu/FEX`
- `https://github.com/ptitSeb/box64`
- `https://github.com/brunodev85/winlator`
- `https://github.com/utkarshdalal/GameNative`
- `https://github.com/sharpemu/sharpemu`
- `https://github.com/dev-Ali2008/Kyty-Android`
- `https://github.com/Brendan250/Kyty-Android`
- Android ADPF:
  `https://developer.android.com/ndk/reference/group/a-performance-hint`
- Android game mode interventions:
  `https://developer.android.com/games/optimize/adpf/gamemode/gamemode-interventions`

## 16. Объективный protocol для следующего A/B

1. Проверить exact HEAD/diff и hash device `.so`; не очищать cache без причины.
2. Убедиться, что `res_mode=4`, runtime log подтверждает requested/applied
   960x544/scale, а renderer сообщает фактический GNM attachment extent.
3. Остудить Vivo до согласованного thermal status/skin range.
4. Запустить CUSA03173 прямым intent.
5. Известными keyevent пройти в один и тот же save/тяжёлый участок.
6. Воспроизвести один и тот же 20–30-секундный movement script.
7. Одновременно сохранить:
   - FPS/present/draw/submit counters;
   - wall/task-clock и perf samples;
   - per-thread CPU для Game:Main/GpuComm/FMOD/Havok/workers;
   - scaling_cur/max_freq и thermal status;
   - JIT coverage/guest-op/native instruction counts;
   - 60-FPS screencast для графической корректности.
8. Сравнить не одно значение FPS, а median/p10/p90 frame time, общий
   task-clock, retired instructions и работу каждого потока.
9. Acceptance крупного пакета:
   - нет 0-FPS stop/audio-only continuation;
   - нет font corruption;
   - severe/ABA flicker = 0;
   - Dead Cells full layers на нескольких cold launches;
   - Nidhogg 2 проходит match/death/restart без DEVICE_LOST;
   - общий CPU work уменьшается, не переезжает;
   - затем короткий PS5/PS4 non-regression.

## 17. PS5 runtime: отдельная незавершённая ветка задачи

Полный checkpoint: [`../PS5_RUNTIME_HANDOFF.md`](../PS5_RUNTIME_HANDOFF.md).

### Реализовано

- Отдельная `.so` `lsx4_executor_ps5_android`, включаемая только
  `LSX4_ENABLE_PS5_DESKTOP_PATH=ON`.
- ABI v7, version node `LSX4_PS5_RUNTIME_7.0`.
- Изолированный PS5 cache/artifact namespace.
- PS5 guest page 16 KiB; PS4 page остаётся 4 KiB.
- NextGen raw ELF и decrypted/uncompressed fSELF, два SELF magic.
- Standard/SCE dynamic metadata; relocation types
  0,1,2,4,6,7,8,10,11,16,17,18,24,32,33,38.
- `DT_NEEDED`, aliases/NIDs, deferred function/data/TLS imports, late rebind.
- Variant-II TLS/TCB/DTV, late module TLS, init/fini arrays, module ordering
  and discovery.
- EXTRQ/INSERTQ/MONITORX/MWAITX в PS5-only JIT path.
- ARM64 smoke, 38-case loader matrix, ABI gates, ASan/UBSan coverage.
- Android catalog/import path распознаёт `param.json`, размещает PS5 в общем
  списке, рисует badge `PS5` и запускает с `game_platform=ps5`.

### Не реализовано/не доказано

- retail SELF decryption, protected/compressed/encrypted input;
- полный Prospero HLE/system modules;
- production AGC/GPU и end-to-end совместимость;
- полная EH-frame public ABI и metadata/content HLE;
- реальный loader/HLE coverage всех decrypted games.

SharpEmu тоже ожидает decrypted/fake-signed input; от него не следует ждать
retail decryption.

### Игры

- Две малые PS5-игры исторически были доведены до меню и геймплея; одна из
  них Dreaming Sarah. Их нужно считать regression controls, но повторно
  проверить на текущем общем JIT перед новым PS5 этапом.
- Третья — Demon's Souls, `PPSA01342`, около 80 GB. Дошла до корректного splash
  и ранних Gen5 passes; после splash scanout оставался чёрным/FPS 0. Старый
  checkpoint был создан на Xiaomi и теперь **не даёт разрешения трогать Xiaomi**.
- PPSA01342 bottleneck: на каждый draw заново создавались buffers/images/
  samplers/layouts/pools/shader modules/render pass/framebuffer/pipeline/
  command buffer/fence, затем sync wait/readback/retile. Около 4 draws/s,
  RSS около 3.18 GB, 3840x2160 swapchain. Сначала нужен persistent resource/
  pipeline architecture и корректный surface ownership, затем меню/gameplay.

## 18. Главные локальные документы и артефакты

- [`tiered_jit_trace_strategy.md`](tiered_jit_trace_strategy.md) — T0/T1/T2,
  managed rollout, register-authority failures and safe loop write-back.
- [`android_guest_memory_frame_flicker_postmortem.md`](android_guest_memory_frame_flicker_postmortem.md)
  — page watchers, Bloodborne render ordering, trace cycle, Nidhogg 2 device
  loss, Dead Cells upload lifetime.
- [`android_audio_compatibility_postmortem.md`](android_audio_compatibility_postmortem.md)
  — variadic XMM and NGS2 fixes.
- [`arm_desktop_client_layer_architecture.md`](arm_desktop_client_layer_architecture.md)
  — ownership границ Funnel/client and build graph.
- [`ps5_desktop_isolation.md`](ps5_desktop_isolation.md) — isolated PS5 ABI.
- [`ps5_sharpemu_compatibility_matrix.md`](ps5_sharpemu_compatibility_matrix.md)
  — implemented/synthetic/unimplemented PS5 matrix.
- [`../PS5_RUNTIME_HANDOFF.md`](../PS5_RUNTIME_HANDOFF.md) — PPSA01342 exact
  state, logs, hashes and old launch commands.

Особенно полезные текущие visual/log artifacts:

```text
current-vivo-platform-perf-gameplay2.png
current-vivo-adpf-game-route.png
current-vivo-cusa07640-final-nobisection2.png
current-vivo-deadcells-upload-lifetime-r2.png
vivo-bb-a09197bf2-device-lost-logcat.txt
cusa07640-vk-journal-r4.txt
deadcells-broken-live.mp4
perf-r52-fullmap-report.csv
perf-r59-callgraph-symbols.csv
perf-r62-hostsym.csv
```

Их timestamps относятся к разным ревизиям. Не складывать цифры из разных
builds в один A/B без проверки marker/hash.

## 19. Что именно попросить у мозгового штурма

Нужен не список из двадцати микроидей и не параллельное делегирование ради
делегирования. Нужен архитектурный разбор, способный найти связный пакет с
реальным потенциалом убрать **12–22 ms CPU на кадр** без нарушения перечисленных
инвариантов.

Желаемый результат общего чата:

1. Построить cost model текущего generated code: сколько на guest op приходится
   useful ALU/SIMD, machine-state traffic, flags, fault metadata, helper ABI,
   edge/quantum work.
2. Нарисовать полный observer graph и предложить один exact register-authority
   ABI, а не новые частичные кэши.
3. Доказать, какие host registers можно зарезервировать как callee-saved
   carrier на Android AArch64 с учётом Funnel/HLE/signals.
4. Спроектировать metadata format для fault/deopt/interrupt recovery и способ
   его проверки дифференциальными тестами.
5. Оценить instruction-count и frame-time эффект каждого компонента пакета;
   отклонить идеи с потолком 1–2% до реализации.
6. Предложить staged rollout, где каждый промежуточный stage либо полностью
   сохраняет старый authority contract, либо уже содержит все observers — без
   состояния «оптимизация наполовину и игра иногда висит».
7. Отдельно определить, есть ли ещё CPU work, которое можно полностью убрать:
   spin polling, redundant HLE/PM4, repeated memory shuffles, а не только
   ускорять инструкции.
8. Сформировать минимальный deterministic benchmark and regression suite.

### Готовый bootstrap prompt для общего чата

> Прочитайте полностью `docs/CODEX_BRAINSTORM_HANDOFF_2026-08-01.md` и все
> документы, на которые он ссылается. Пока не меняйте код и не используйте
> устройство. Наша цель — устойчивые 30 FPS Bloodborne при 960x544 на Vivo.
> Нельзя оптимизировать переносом нагрузки, ослаблять frame/memory/state
> correctness, повторять уже отклонённые partial GPR/RET/deferred-flags пути
> или предлагать title hardcode. Сначала коллективно постройте единый cost
> model и observer-complete JIT ABI пакет, способный убрать минимум 12–22 ms
> CPU на кадр. Разделяйте факты текущего профиля, гипотезы и требующие нового
> измерения данные. После этого выдайте один согласованный план с ожидаемым
> выигрышем, рисками, fail-closed gates и точным A/B protocol. Подключение к
> workspace/устройству пользователь пришлёт отдельно.

## 20. Неопределённости, которые нельзя выдавать за факты

- Последние 20 FPS не являются строгим clean A/B против 18–22: нужен новый
  одинаковый movement run.
- Старое GPU=8 ms нужно перепроверить после `a09197bf2` и dirty render-scale
  bridge.
- Фактическая эффективность vendor Vivo hint не отделена от cold-device и
  scene variation. Логи доказывают принятие hint, но не x% FPS.
- `scaling_max` меняется по OEM policy; ADPF не гарантирует max silicon clock.
- Cumulative JIT counters не показывают стоимость одного текущего кадра.
- Две ранние PS5 игры были пользовательски приняты как работающие на старом
  срезе, но не перепроверены после всех общих JIT/Vulkan изменений.
- «7/7» означает наличие управляемых реализаций, не закрытие performance goal.

## 21. Live update 2026-08-01 15:05: активный CPU/JIT этап

Этот раздел новее всех чисел выше и имеет приоритет при расхождении.

### Что подтверждено новым профилем

В визуально корректной Hunter's Dream был снят 8-секундный userspace
`simpleperf` (`perf-safe-wb-r4.data`): 116516 samples и 29.129 s суммарного
task-clock, то есть около 3.64 полностью занятых CPU cores в среднем.

Распределение по DSO:

```text
generated JIT / unknown       66.78%
liblsx4_executor_android.so   22.99%
libc                           6.01%
Vulkan driver                  2.93%
linker                         0.59%
```

Распределение по потокам:

```text
Game:Main        16.91%
HavokWorker      12.72%
Thread17         10.29%
GpuComm           9.61%
FMOD mixer        5.94%
Thread18/19/20/21/22 примерно по 5.4–5.8%
```

Ни renderer, ни один конкретный FMOD block не являются одиночной целью для
скачка до 30 FPS. Постоянная цена равномерно повторяется в generated code на
Game/Main, Havok, FMOD и workers. Среди именованных общих налогов крупнейшие
всё ещё малы: `memmove` 0.83%, mutex lock 0.72%, dynamic TLS resolver 0.58%,
Scudo lock 0.46%, mutex unlock 0.42%, `getenv` 0.36%, synchronous guest fault
frame query 0.29%. Оптимизировать их по одному — запрещённый микродрейф.

### ADPF и частоты

- Native ADPF session действительно активна: `sessionActive=1`, обновление
  hot TID работает, `reportResult=0` подтверждает принятие фактического work
  duration системой.
- В session попадают Game:Main, GpuComm, FMOD mixer, активные workers и Havok;
  это не статический список адресов/тайтла.
- Прямая попытка `uclamp.min=512` стабильно получает `EPERM`; это нормальное
  ограничение unrooted app, а не доказательство неработающего ADPF.
- `nice` остаётся доступен. Vivo vendor performance hint и Android game mode
  продолжают использоваться.
- При последнем длительном запуске skin достиг 48.0 C/status 4. HUD 15 FPS в
  таком состоянии нельзя сравнивать с холодными 18–22. Это одновременно
  подтверждает, что код должен снижать total work: удерживать максимальную
  частоту политикой бесконечно устройство не сможет.

### Точная причина прежнего серого/0-FPS startup

Guarded faultable T2 regions были переведены на write-back до того, как все
deopt observers получили точную register map. В timing loop это оставляло
неавторитетные GPR и зависание около 159850 blocks. Исправления:

1. После `ExecuteTieredGuardDeopt` reload больше не предполагает сохранность
   callee-saved guest GPR: выполняется полный cache reload.
2. Faultable local write-back разрешён только для T2 с signal sidecar, local
   backedge и **без guards**.
3. Guarded T2 остаётся memory-authoritative; безопасные guard-free loops имеют
   до 12 локальных cached GPR, но не публикуют этот layout как cross-block ABI.
4. Fault descriptor публикуется region-sidecar, а точный guest op находится
   по host PC в редком signal path.

После установки в правильный `files/runtime` путь эта схема прошла warning,
title, загрузку и gameplay без 0 FPS, порчи шрифтов или кадра. Важно: несколько
предыдущих «неудачных исправлений» этой причины были ложными испытаниями старой
`.so`, потому что обновлялся неактивный sandbox path.

### Текущий KPI register residency

Последний cumulative counter после выхода в gameplay:

```text
GPR cache blocks                 126051
cached GPR registers            1036824
estimated GPR accesses avoided    63387
shape rejections                   2450
SIMD cache blocks                 11220
SIMD halves                       29285
estimated SIMD accesses avoided  205916
```

GPR cache формально покрывает много blocks, но экономит лишь около 0.5 state
access на cached block. Это доказывает, что «cache enabled» пока почти не
равно настоящей residency. Следующий KPI — не количество blocks, а avoided
state traffic на guest instruction и ширина реально активных T2 regions.

### Найденный системный дефект T2 planner

PGO загрузил 4968 edges, но значительная часть T2 candidates отвергалась по
`conditional_profile_not_dominant`, `internal_cycle_requires_loop_seed`,
`loop_close_target_mismatch`, `loop_close_terminator_before_seed`,
`successor_direct_entry_missing` и `terminator_not_fusable`. Особенно это
видно на общих FMOD chains из 5–8 members.

Два дефекта уже исправлены в dirty source:

1. Запрос `require_loop_closure` раньше заранее ставил
   `plan.loop_head_osr=true`. Если профиль обрывался до реального backedge,
   весь полезный многоблочный prefix затем ошибочно проверялся как замкнутый
   loop и отбрасывался. Теперь loop-OSR включается только после доказанного
   возврата к seed; безопасный prefix остаётся обычным trace, а найденный
   внутренний loop отдельно ставится в queue.
2. Локальный цикл распознавался только когда **taken branch target** находился
   внутри combined region. Реальные loops могут возвращаться к head по горячему
   **fallthrough** (наблюдался 4-member пример, latch непосредственно перед
   seed). Compiler теперь симметрично распознаёт taken/fallthrough local edge,
   использует тот же bounded safepoint counter и материализует deferred flags
   только на правильном non-local exit.

Оба подпакета прошли warning/title/load/gameplay и сохранили корректные
шрифты, персонажа, foliage и кадр. В первых 64 logged rejects после второго
пакета стало `loop_close_target_mismatch=0` и
`internal_cycle_requires_loop_seed=0`; раньше 4-member loop стабильно попадал
в `loop_close_target_mismatch`. Build развёрнут с hash `043673...`. HUD был
13 FPS при skin status 2–3 и потому подтверждает только correctness, не
performance. Новый cold A/B ещё обязателен.

### Research/conclusions: куда бить после T2 coverage

Исследование FEX/Box64 и текущего generated code оставляет три крупных
последовательных слоя, каждый должен уменьшать общий retired-work, а не долю
одного потока:

1. **Observer-complete register authority.** Exact host-PC maps для
   fault/deopt/guard/interrupt должны позволить держать GPR/XMM write-back во
   всех широких regions без blanket memory-authoritative stores. Частичный
   write-back до завершения observers запрещён.
2. **Custom helper ABI в отдельном `.S`.** JIT carrier живёт в callee-saved
   host registers; asm stubs объявляют минимальный clobber set. Это убирает
   `SpillAll/CrossCallBoundary` на общих flag/guard/semantic helpers и делает
   уже внедрённую SRA окупаемой. `jit_aarch64_stubs.S` уже существует и
   подключён CMake; новый asm нельзя возвращать inline в C++.
3. **Широкий lowering общей цены guest op.** FlagM/FlagM2 для дешёвых NZCV,
   LDAPR только там, где RCpc доказан достаточным для x86-TSO, TBL/TBX и NEON
   для shuffle/pack/REP/string patterns. SVE2 должен быть runtime-dispatched и
   иметь идентичный NEON fallback; он не является первым рычагом из-за ABI и
   частоты переходов между vector domains.

Рукописный asm под один FMOD address не нужен: ни один region не превышает
примерно 2% CPU. Ценность asm — только в общем helper/string/SIMD ABI,
исполняемом всеми guest threads.

### Ближайший fail-closed порядок

1. Завершить текущий taken/fallthrough T2 gate: gameplay, отсутствие 0 FPS,
   font/texture/flicker regression; сравнить compiled/rejected/member-width.
2. На холодном Vivo повторить exact scene и 8-second task-clock. Принять пакет
   только если reduced total work подтверждается независимо от HUD.
3. Расширить categorical GPR/XMM residency: GPR-only operation по умолчанию не
   сбрасывает vector cache; явный короткий deny-list содержит только настоящих
   machine-state observers.
4. Завершить exact recovery metadata и только затем снять write-through у
   guarded/faultable wide regions одним связанным пакетом.
5. Подключить custom helper ABI и измерить снижение load/store/move доли в
   generated code. После этого продолжать до стабильных 30 FPS; Vulkan снова
   становится приоритетом только если новый профиль покажет реальный GPU/driver
   предел, а не по предположению.

## 22. Shared-room measurements after the handoff

The Codex/Claude room replaced three large estimates with direct measurements
on Vivo in the same Bloodborne route. These results supersede the speculative
numbers above.

### Measured frame-boundary cost

The first valid gameplay probe produced 18.191 FPS (about 55 ms/frame), GPU
busy 51--58%, unplugged, with the CPU cap stable at 2.4384 GHz. Normalized over
4440 frames:

```text
Presenter::Present                    1.08 ms/frame
  ImGui inside Present                0.07 ms/frame
SubmitExecution                       2.68 ms/frame
FlushPendingBufferTransfers           0.68 ms/frame
timeline/readback waits              0.003 ms/frame
all measured boundaries               4.44 ms/frame
```

Consequences:

- The earlier 11 ms presentation estimate was false. Direct instrumentation
  measured about 1.1 ms; the old difference came from SurfaceFlinger/surface
  composition outside `Presenter::Present`.
- Roughly 2.94 million transfer flush calls are real, but each averages about
  1 microsecond. UMA/direct host upload cannot save the previously assumed
  6--10 ms; its absolute ceiling from this path is about 0.68 ms/frame.
- GPU waits are effectively zero. CPU and GPU work overlap; neither thread is
  spending the frame blocked on the other.
- A rejected run reported 30.209 FPS, but draw rate was only 719/s and GPU busy
  7%: it was the title menu. Valid gameplay is 6000--8000 draw/s with GPU busy
  around 50%+. The benchmark must reject menu runs automatically.

### Sampled native guard exits

Codex moved guarded T2 side-exit accounting to a custom AArch64 helper:
architectural state is flushed once, the native atomic counter samples only
1/256 exits into C++, and the redundant reload plus second flush were removed.
The package passed gameplay without a 0-FPS hang. A/B result:

```text
immediate baseline (present probe)   18.318 FPS
sampled native guard exits           18.238 FPS
difference                            -0.44% (noise / neutral)
```

The run accumulated 3,376,896 guard deopts and 63 rollbacks, but removing their
C++ work did not move steady-state FPS. Guard exits were not the large constant
cost. The safe mechanism can remain for latency/stutter, but it is not a route
to 30 FPS.

### Rejection histogram now available

The current runtime exports structured T1/T2 rejection reasons. In the valid
guard run:

```text
T1 rejected 1287
  noStaticCostWin 913
  ineligibleTier0 175
  tier2Active 160
  indirectControl 32
  stalePublication 7

T2 rejected 1340
  compilePolicy 821
  conditionalProfile 273
  terminator 168
  directEntry 78
```

This confirms that further local lowering work will not expand region coverage
until the compile-policy and control-shape gates are addressed categorically.

### Current two active directions

1. **GPU pass decomposition.** The valid run implies about 30 ms of Adreno work
   per 55 ms frame. Vulkan timestamps must report each render/compute pass,
   its real color/depth extents, draw count and GPU duration. In particular,
   `render_resolution_mode=4` is configured as 960x544, while the observed
   scene was described as 1600x900. It must be established whether 1600x900 is
   only the canonical guest extent or the physical offscreen/depth/G-buffer
   workload. Scaling only the final presenter while heavy passes stay at
   1600x900 would explain the missing GPU time.
2. **Observer-complete adaptive GPR chain.** The eight static GPRs can cross
   native edges, but the four adaptive GPRs must still use the old
   memory-authoritative boundary. Exact-layout carry was tested in three forms:
   dirty carry with an x27 authority flag, dirty carry with unconditional
   observer publication, and read-only exact-layout carry. The two dirty forms
   crashed reproducibly; removing x27 did not change the failure. The read-only
   form survived the title/menu route but later produced a deferred SIGSEGV at
   guest RIP `0x8002b9eb4` (`fault=0x10`) followed by SIGBUS in
   `jit_host_access_no_resume`. Therefore the feature is hard-gated with
   `kFullAdaptiveGprChainIsObserverComplete=false`. Do not re-enable even the
   read-only subset without per-edge history and exact observer/fault proof.

### Current validated state after the adaptive-carry A/B

- The fail-closed build is `A9C4790972E878135E8680FEFDD6EA0450568FAE2D1FB39035234F69A674D3BF`.
- Both runtime copies on Vivo were verified against that SHA before launch.
- It passed the former crash point, reached the heavy gameplay route and stayed
  alive without SIGSEGV/SIGBUS.
- A 15-second SurfaceFlinger window reported 18.341 FPS, 279 frames and zero
  dropped frames. The device was AC-powered, so the number is a functional
  datapoint rather than a clean thermal performance comparison.
- The preceding read-only build `45733D07...` is rejected. It can appear stable
  for minutes in menu/loading and still fault later; menu survival is not a
  sufficient validation for a native-chain authority change.

### Exact dirty-carry crash evidence

With the diagnostic native map enabled, a dirty-carry failure resolved to guest
RIP `0x8020b5ed1`. The decrypted eboot bytes are:

```text
8b 46 10          mov eax,[rsi+0x10]
48 89 47 10       mov [rdi+0x10],rax
48 89 f8          mov rax,rdi
c3                ret
0f 1f 40 00       nop
```

The observed block is mainly static-register code, so it is the first visible
consumer of corruption introduced at an earlier adaptive edge, not proof that
this block itself is faulty. The next investigation must record edge history or
enable carry one adaptive register at a time. Re-enabling the complete package
and waiting for the same consumer is explicitly forbidden.

### GPU target after pass decomposition

The 16384x16384 depth atlas is only 1.3% of summed viewport area and is closed as
a target. The later attachment classifier invalidated the apparent 4096x4096
target completely: across 5.375 million binds there were zero real 4096x4096
images. The earlier 57% estimate read the nominal/max `RenderState` extent, not
`image->info.size`. The dominant real color surface is 960x544, and it is both a
sampled texture and a render target, so independently scaling it would break its
consumer UV contract. Do not implement a 4096 scaling patch.

The next valid GPU measurement is a Vulkan timestamp-query ring around actual
render/compute scopes. It must read frame N-2 without `WAIT_BIT`, convert using
`timestampPeriod`/`timestampValidBits`, and aggregate by the physical image
extent and format. Do not infer GPU cost from nominal state or counter deltas.

Do not implement the proposed UMA buffer rewrite before new evidence: direct
timing reduced its maximum possible contribution by an order of magnitude.

### Rejected uniform-12 fixed GPR experiment

A uniform fixed mapping of twelve guest GPRs removed layout variation and passed
its direct two-block R8 chain self-test (`41 -> 49`, about 1.82x self-test chain
speedup). It is nevertheless rejected. In Bloodborne, build
`B641B460EF02155F180C5B760FE76C96176F7E82C8DD24AE819582A8B7853926`
crashed after 124 seconds while entering heavy gameplay. The immediate fault was
a null dereference at `0x4dc` in Adreno's vertex-buffer binding, with the caller
chain `vk::CommandBuffer::bindVertexBuffers -> Rasterizer::Draw:3393` on
`shadPS4:GpuComm`. This is compatible with earlier guest CPU corruption reaching
PM4/vertex state later. A passing isolated chain test is therefore insufficient
proof for adding caller-saved fixed registers. The experiment was removed and
the fixed-eight fail-closed layout restored.

### Vertex-bind crash root cause and corrected interpretation

The `B641...` crash was initially attributed to the uniform-12 JIT experiment,
but the same Adreno fault later reproduced with the fixed-eight layout. The
immediate failure remained identical: address `0x4dc` inside
`vkCmdBindVertexBuffers` on `shadPS4:GpuComm`. This disproves the earlier claim
that the call stack by itself demonstrated guest-register corruption.

A marker A/B isolated the failure to Android vertex/index bind deduplication.
With `run-bisect-disable-vk-vertex-bind-dedup` present, the full-corpus build
survived 352 seconds of heavy Bloodborne gameplay; the earlier enabled runs had
failed near 124 and 218 seconds. The first permanent-fix candidate kept
deduplication inside a dynamic-rendering scope and invalidated cached vertex,
vertex-input and index state whenever `Scheduler::RenderingEpoch()` changed.
Its first marker-free run survived 317 seconds, but a second run ended in
SIGSEGV near 316 seconds. That second death had no tombstone or fatal stack and
coincided with another emulator process starting, so it does not prove a
repeated Adreno fault. It does prove that the epoch-only result was not strong
enough to call the blocker closed.

The current fail-closed policy retains the epoch key and safe dynamic
vertex-input deduplication (about 29--32% hits), but disables raw vertex- and
index-buffer bind deduplication. Their measured hit rates were only about 2.1%
and 0.6%, so this removes the stale-lifetime risk at negligible driver-call
cost. Raw dedup must not be re-enabled until the key includes a monotonic
buffer-allocation generation and eviction/destruction explicitly invalidates
every cached binding; a Vulkan handle alone is never sufficient identity on
Qualcomm.

### Local terminator GPR package

CALL, RET, indirect control and counted-loop blocks now use the same local-only,
cost-gated GPR residency as ordinary blocks. This does not carry authority
across an edge: it flushes before HLE/semantic observers and reloads after
helpers. A native-cache ABI tag forced a true full-corpus rebuild; the resulting
coverage was 47,568 terminator blocks, 85,059 resident registers and 258,671
estimated machine-state accesses avoided. Bloodborne reached correct heavy
gameplay without font, frame or fault regression. The measured steady-state
gain was small rather than transformative, so the package is useful groundwork,
not the route from 18--21 to 30 FPS.

### Global GPR pinning launch blocker and fix (2026-08-02)

The first full 16-GPR mapping made multiple titles stall before rendering. Cache
clearing did not help because the failure was generated-code semantics, not
persistent data. A production differential self-test reduced it to the sequence
starting with `48 11 d8` (`adc rax,rbx`). The ADC/SBB lowering kept its result in
host `x15`, copied it to `x12` for flag derivation, then called the shared parity
lowering. That lowering also used `x15` as scratch. For example, the correct
result `0x8000000000000011` was overwritten with `0xffffffe1`; subsequent guest
branches and sign extension then diverged and games waited indefinitely.

Native ABI tag `0x4747505241424907` restores the result from `x12` after flag
publication. The differential mismatch disappeared. ABI `...4909` additionally
made the already bridged instruction families body-aware: implicit GPR users,
ADC/SBB/XCHG, rotate/SHLD, RDTSC/RDTSCP, string moves, one-operand MUL/DIV,
memory BT/XADD, fences and prefetches. Scalar, extended-scalar, semantic, stack,
counted-control and memory-RMW parity tests pass.

Short Vivo validation after each immediate force-stop:

- Bloodborne passed the former startup stall and produced GNM frames at the
  configured `960x544` extent.
- Limbo produced scanout frames.
- Dead Cells reached about 59 submits/s during its early scene at 1280x720.

Do not reintroduce the old result/scratch alias. Any new fixed-register mapping
must run the production differential test before a title launch, and generated
results that remain live across flag publication may not occupy `x13`, `x14`,
`x15`, `x17` or `x9`, which are the shared flag-lowering scratch set.

### Selective helper liveness, shadow returns and indirect PIC (2026-08-02)

The next package adds four general mechanisms on top of global GPR pinning:

- an instruction-level GPR/XMM live-in/live-out fixed point with a bounded
  256-block successor CFG for T1/T2; unknown, HLE, indirect and return exits
  remain all-live;
- selective helper publication/reload instead of unconditional architectural
  cache flushes;
- a 2048-entry per-machine shadow return stack with guest-return validation and
  a safe fallback to the existing edge fanout;
- a two-entry hot indirect PIC in front of the existing bounded 8/16-way table,
  plus region-local CMP/TEST-to-Jcc fusion through transparent instructions.

Two independent bugs initially produced a deterministic SIGBUS after several
seconds at the visible consumer RIP `0x802650100`, with the first invalid access
at `0x1fffffffe`:

1. Whole-register liveness treated every 8/16-bit GPR write as a complete kill.
   A caller-clobbered pinned register could therefore be omitted before a
   helper, then a later partial write preserved corrupted high bits. Narrow GPR
   writes are now read-modify-write in the dataflow model. Legacy SSE writes are
   similarly partial at whole-YMM granularity; VEX/EVEX destinations retain
   their full-definition/upper-zero contract.
2. The decoder exported implicit GPR masks but no implicit XMM/YMM masks. The
   Iced bridge and C ABI now export both vector read and write masks. Merely
   rebuilding native code was insufficient: old persistent IR shards still
   decoded under ABI `0x202607190001` with zero vector metadata. The persistent
   IR ABI was bumped to `0x202608020001`, so incompatible shards are rejected
   automatically instead of requiring manual cache deletion.

Diagnostic A/B results that must not be repeated:

- disabling only the shadow-return fast path did not remove the SIGBUS;
- limiting selective-save to one-operand MUL/IMUL still reproduced it;
- disabling selective-save entirely while leaving shadow returns active was
  stable, proving that RET prediction was not the fault class;
- preserving all vector registers while retaining exact GPR liveness was
  stable, which isolated the remaining defect to vector metadata rather than
  the GPR fixed point;
- adding vector masks without changing the persistent IR ABI was insufficient
  because restored old shards silently lacked the new fields.

The final ABI combination is persistent IR `0x202608020001`, native semantic
`0x20260802000d`, global native tag `0x4747505241424913`. The Android build and
Rust decoder tests pass. A 20-second Vivo Bloodborne runtime validation survived
the former failure point with both processes alive and no SIGSEGV/SIGBUS. A
clean FPS comparison is still required: the first SurfaceFlinger run was invalid
because `com.vivo.vhome` repeatedly became the foreground activity, yielding
`NO_FRAMES_NAV_FAILED`; it is not a performance result.
