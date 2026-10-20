# Опыт: профилактика регресса playable и производительности

Дата фиксации: 2026-08-31. Текущая рабочая ветка: `8gen3fin` в
`D:\LProj\PetProj\lsx4-a840-hud-wt`.

Этот документ является обязательным regression contract для изменений CPU/JIT/FEX,
HLE, потоков, памяти и Android-профилей. Он фиксирует три связанных инцидента:

1. общие оптимизации и изменения lifecycle сделали пять ранее playable-игр
   неиграбельными;
2. восстановление этих игр вместе с новой маршрутизацией общего CPU-профиля
   снизило Bloodborne на Vivo A840 примерно с 45 до 28--31 FPS.
3. общий Current-JIT сохранил resident GPR через PS5 HLE boundary и сломал уже
   запускавшиеся Dreaming Sarah и Void Terrarium++ падением `SIGSEGV @ 0x38`.

Главный вывод: множество playable-игр и производительность принятых игр должны быть
монотонными одновременно. Нельзя закрывать новый frontier ценой уже playable title.
Нельзя сохранять playable-набор ценой незамеченного падения производительности
принятого performance anchor. Улучшение по одной оси не компенсирует регресс по другой.
PS4/FEX и PS5/Current являются отдельными обязательными backend-gates: успех одного
не доказывает корректность другого даже при общем lowering core.

## Непересекаемая граница принятия

Любая общая правка допускается только когда одновременно выполнены два гейта:

- **Playable gate:** все игры из затронутого golden-набора доходят до реального
  gameplay, а не только до boot/logo/menu. Нужны temporal screenshots или bounded
  input trace, progress counters, отсутствие freeze/0 FPS и корректный повторный
  запуск.
- **Performance gate:** каждый затронутый performance anchor запускается с тем же
  save/scene, requested/effective backend, настройками, patches, driver, clocks и
  диагностической нагрузкой. Сравниваются FPS/present/draw, CPU-seconds/frame и
  frame-time tails. Для Bloodborne/Vivo A840 текущий контрольный ориентир после
  восстановления — около 41--43 FPS в сохранённой сцене; исторический Aug-30
  artifact показал 45 FPS.

Запрещено принимать изменение со словами «остальные игры стали playable», если
Bloodborne не прошёл performance gate. Запрещено принимать ускорение Bloodborne,
если хотя бы одна golden-игра не прошла gameplay gate. Menu/boot не являются
доказательством playable; один screenshot не является доказательством стабильности.

## Кейс 1. Пять ранее playable-игр перестали запускаться до gameplay

### Контроль и наблюдавшаяся граница

Пользователь подтвердил реальный gameplay на историческом Vivo control для:

- Inside;
- Cyber Shadow;
- Just Shapes & Beats (`CUSA10349`);
- Rain World (`CUSA05667`);
- The Binding of Isaac: Rebirth (`CUSA00792`).

Rollback-контроль сохранён как
`artifacts/vivo-five-games-20260831/apk-backups/vivo-working-control-root9f-oldfex-20260831.apk`,
SHA-256 `c52d749894a9a6e0cbc6e44a2b2fd096c67655277fe06afbb2f8ac9541df9caf`.
Регресс не имел одной универсальной причины: несколько общих «оптимизаций» пересекли
разные correctness/lifetime contracts и дали похожий внешний симптом — загрузка,
чёрный экран, 0 FPS либо остановка до gameplay.

### Конкретные причины

#### 1. Kernel AIO и lifetime файловых handle

Новый dedicated AIO worker нарушил несколько обязательных инвариантов:

- terminal request ID не возвращался в пул без явного delete;
- terminal ID мог становиться видимым раньше guest result record;
- при ошибке batch хвост оставался в `PROCESSING`;
- cancel позволял повторно использовать slot до фактического завершения worker;
- queued operation сохраняла только числовой fd: после close/reuse она могла
  обратиться уже к другому файлу.

Исправление не подменяет результат игры и не отключает AIO. Worker и inline path
используют один lifecycle. Accepted request удерживает lease на handle до завершения;
drain предыдущей сессии выполняется до новой; result fields публикуются до terminal
state; inactive terminal IDs циклически освобождаются; весь batch tail переводится в
terminal state при ошибке; cancel не переиспользует slot до retirement worker.

Regression contract: переключатель `Separate I/O thread` может менять только место
исполнения работы, но не ownership, publication order, cancellation semantics или
guest-visible completion.

#### 2. Admission limit для постоянных Unity worker cohorts

Just Shapes & Beats создаёт 21 постоянный Unity worker, Rain World — 12. Оба startup
barrier ждут входа каждого участника. Общий малый lifetime admission limit парковал
часть cohort до barrier и детерминированно оставлял первый чёрный кадр/0 FPS.

Исправление title-scoped: GameDB задаёт `max_guest_workers=32` только для
`CUSA10349` и `CUSA05667`. Это не причина переносить 32 workers в общий default:
на других играх это может создать oversubscription и снизить FPS.

Regression contract: нельзя оптимизировать число guest workers по числу big cores.
Сначала нужно различать lifetime admission (сколько guest threads обязаны войти для
progress) и placement/priority (сколько из них одновременно получают тяжёлые ядра).

#### 3. Mono suspend/safepoint и condition wake

Unity/Mono может запросить SIGUSR1 suspend, пока translated guest thread находится на
HLE boundary или blocking lock. Общий быстрый HLE путь не должен терять pending signal,
держать глобальный lock во время callback или пропускать wake после acknowledge.

Исправление оставляет pending Mono suspend до точной safe point, проверяет его при
входе/выходе HLE и выполняет condition/event wake в правильном порядке. Этот маршрут
активируется только при наличии Mono-модуля; non-Mono titles не входят в его lock/state.

Regression contract: lock-elision/HLE fast path обязан отдельно пройти Mono negative
и positive tests. «Leaf» не означает «не участвует в runtime safepoint».

#### 4. Nested FEX HLE frame pool и Isaac

Nested callbacks используют нижнюю половину leased HLE frame-pool slot как настоящий
SysV stack. Эта память host-owned и ранее не считалась допустимой guest-visible
областью. В результате Isaac терял:

- седьмой и последующие typed HLE arguments, лежащие на stack;
- stack-resident строки путей, которые читает AeroLib libc при загрузке
  `rebirth.prx`.

Исправление регистрирует frame-pool как exact mutable sideband, а не как общую PS4
VMA и не как GC root. Strict non-reentrant helpers получают lease только на полностью
входящий span. Typed HLE stack-arguments и libc string readers читают только полностью
покрытый зарегистрированный диапазон. Любое частичное/неизвестное покрытие остаётся
fail-closed.

Regression contract: host-owned callback stack не следует ни безусловно отвергать,
ни публиковать как произвольную guest memory. Нужен exact range lease с ограниченным
набором читателей и тестами на boundary/partial span.

### Доказательство закрытия кейса

Текущий compatibility APK с SHA-256
`172291767bf8bf02301d0f93682248b07804422d6913bce56da9c28b1cee2c81`
дошёл до gameplay во всех пяти играх. Evidence:

- `artifacts/vivo-five-games-20260831/inside-historical-fex-after-150s.png`;
- `artifacts/vivo-five-games-20260831/cyber-shadow-historical-fex-series-01`;
- `artifacts/vivo-five-games-20260831/just-shapes-gameplay-proof-*`;
- `artifacts/vivo-five-games-20260831/rain-world-gameplay-proof-02`;
- `artifacts/vivo-five-games-20260831/isaac-gameplay-proof-final`.

Это доказало playable gate, но ещё не разрешало считать всю сборку принятой: после
него потребовался отдельный Bloodborne performance gate.

## Кейс 2. После восстановления пяти игр Bloodborne потерял около 15 FPS

### Точный A/B и исключённые объяснения

На одном Vivo A840, с сохранёнными application data, одинаковым Bloodborne save/scene
и неизменным settings XML были сопоставлены два APK:

- исторический Aug-30 artifact
  `C:\Users\Litesav\AppData\Local\Temp\lsx4-build-artifact-stash-20260830\app-debug.apk`,
  SHA-256 `750A91...`: 45 FPS;
- APK до финального five-game fix
  `artifacts/vivo-five-games-20260831/apk-backups/vivo-current-before-five-game-fix.apk`,
  SHA-256 `DA0B8...`: 28--31 FPS.

Settings file оставался тем же, SHA-256
`8F8F9235...`. Частоты в сравнимых окнах были `policy0=2.3616 GHz` и
`policy6=2.4384 GHz`; более быстрый старый APK работал не в более холодном состоянии.
Следовательно, причина не в старых процессах, температуре, frequency cap, сцене,
разрешении или финальных AIO/Mono/Isaac исправлениях.

Причинный log delta:

```text
45 FPS control:
[EXECUTOR_FEX_EXACT_MULTIBLOCK] requested=1 configured=1
[EXECUTOR_FEX_MAXINST_GEOMETRY] requested256=1 exactMultiblock=1 configured256=1

28--31 FPS regression:
[EXECUTOR_FEX_EXACT_MULTIBLOCK] requested=0 configured=0
[EXECUTOR_FEX_MAXINST_GEOMETRY] requested256=1 exactMultiblock=0 configured256=0
```

### Причина

В настройках был включён новый universal CPU scope:

```text
cpu.all_games.enabled=true
cpu.all_games.preset=1        # Balanced
cpu.all_games.block_linking=0
cpu.all_games.exceptions=[]
```

Named preset материализовал все поля, включая `block_linking=0`, и общий scope имел
приоритет над индивидуальным профилем. Bloodborne GameDB возвращал Safe geometry и
`requested256=1`, но не восстанавливал обязательный exact block-linking contract.
Без exact multiblock FEX не мог сконфигурировать retained 256-instruction geometry,
что и отняло основную часть FPS.

Связь с five-game работой была временной, а не причинной: APK с compatibility fixes
оказался первым проверенным инстансом, в котором новая all-games маршрутизация реально
начала применяться к Bloodborne. AIO lifecycle, Mono safepoint и Isaac sideband lease
не являются причиной 15-FPS падения.

### Исправление и семантика приоритетов

В `CpuTuningSettings.resolveValues` зафиксирован порядок:

- для named global/all-games/per-game preset сначала материализуются его поля, затем
  GameDB повторно накладывает hard title constraints;
- для `Custom` GameDB является базой, после чего явные пользовательские поля остаются
  авторитетными и могут намеренно изменить title default.

В Bloodborne GameDB рядом с Safe 256 geometry теперь явно закреплено
`block_linking=1`. Поэтому обычный Balanced universal profile больше не может случайно
выключить exact multiblock. При этом пользовательский Custom остаётся обратимым и не
превращается в фиктивный UI.

Focused contracts:

- `tests.test_android_cpu_tuning_contract`;
- `tests.test_android_cpu_all_games_profile_contract`.

Они проверяют hard Bloodborne value и порядок named-preset/GameDB/Custom overlays.

### Результат и остаточный A/B

Исправленный APK SHA-256
`DA153629CEBF88B2D6E48D5DAC88723E041A3AD1DDB6248416FA0ED5A511155C`
на том же save/scene включил:

```text
[EXECUTOR_FEX_EXACT_MULTIBLOCK] requested=1 configured=1
[EXECUTOR_FEX_MAXINST_GEOMETRY] requested256=1 exactMultiblock=1 configured256=1
[EXECUTOR_FEX_EXACT_MULTIBLOCK_EFFECTIVE] effective=1
[EXECUTOR_FEX_MAXINST_GEOMETRY_EFFECTIVE] effective256=1 maxInst=256
```

FPS восстановился с 28--31 до 41 в первом causal repeat при корректной геометрии.
После снятия временного preserve marker и чистого финального relaunch та же сцена
показала 46 FPS, то есть исторический 45-FPS уровень фактически восстановлен полностью.

Предполагаемые на первом repeat оставшиеся примерно 4 FPS были отдельно проверены same-binary marker-ом
`run-preserve-bloodborne-snapdragon-strong-bundle`. При отключении strong bundle FPS
стал 37, то есть пакет даёт около +4 FPS и не является источником остаточной разницы
с историческими 45 FPS. Marker удалён; strong bundle должен оставаться включённым.
Финальная установленная base.apk на устройстве имеет тот же полный SHA-256
`DA153629CEBF88B2D6E48D5DAC88723E041A3AD1DDB6248416FA0ED5A511155C`.

## Кейс 3. PS5-игры падали после HLE boundary в Current-JIT

### Точная граница и исключённые причины

На одном Vivo A840 с сохранёнными данными и одинаковым PS5 launch route были
сопоставлены исторические PS5 runtime-библиотеки:

- рабочая от 7 августа:
  `liblsx4_executor_ps5_android.so`, SHA-256
  `EDF961487EB9A335E7F04559EA84B673075664E8B21566176AB49D0BC30D5CCB`;
- уже падающая от 10 августа (`r270`): SHA-256
  `B6E7AD66D8560C3338D62F29224A983DCBB0FE4A20DD40F1445291CD000CE76D`.

Dreaming Sarah (`PPSA02929`) на плохой и текущей библиотеках падала примерно через
1.6 секунды после открытия `GameActivity`: `SIGSEGV`, `fault addr 0x38`, LR=0.
Чистый persistent JIT cache не менял результат. Рабочая и падающая библиотеки
получали один набор managed CPU options, поэтому all-games/per-title settings не
были причинной границей. Старый runtime от 2 августа доходил до игры, но имел старую
перевёрнутую ориентацию кадра; runtime от 7 августа был корректным upright oracle.

Отдельный диагностический запуск с `run-jit-force-helper` на текущей библиотеке
стабильно отрисовал меню Dreaming Sarah. Это исключило renderer, PS5 loader, HLE
surface и сами настройки и локализовало дефект в native ARM64 lowering.

### Доказанная причина

JIT perf-map сопоставил crash PC с блоком
`LSX4_HELPER_9202e15d5_bytes_4a_ops_15`, offset `+0x9c`. Гостевой x64-блок
использовал `rsi` как указатель и выполнял запись `[rsi+0x18]`; на crash path
перенесённое значение соответствовало старому guest pointer `0x20`, поэтому host
адресом стал `0x38`.

PS5 Current HLE bridge публикует возвращённое состояние через `LsxMachineImage`.
Общий JIT при этом мог оставить x23--x30 в resident GPR cache и войти в следующий
нативный блок с pre-HLE значениями, не перечитав memory-authoritative image. Это
нарушение coherency происходило после корректного HLE-вызова и потому не
воспроизводилось в helper interpreter.

### Исправление без регресса PS4/FEX

До появления явного post-HLE resident-register invalidation handshake PS5 Current-JIT
держит GPR state memory-authoritative: `generic_gpr_cache_eligible=false` только под
`LSX4_PS5_DESKTOP_PATH`. PS4 Android/FEX не компилирует эту ветку и сохраняет свой
resident GPR fast path. Current и FEX HLE guest stubs также имеют раздельные builders,
чтобы их разные ABI/return contracts нельзя было снова неявно смешать.

`android-app/app/build.gradle` теперь считает общий
`retiring_execution_core.cpp/.h` входом свежести PS5 runtime. Новый APK не может
тихо упаковать старую PS5 `.so` после изменения общего JIT-файла. Контракты:

- `tests.test_ps5_current_hle_gpr_coherency_contract`;
- native target `lsx4_ps5_fex_contract`.

### Результат на финальном установленном APK

PS5 runtime SHA-256:
`11481F724FD32923D7BE01F832D4D97A96DE3CEAEFBE467F853904AE2F0A1072`.
Финальный APK и установленный `base.apk` имеют одинаковый SHA-256
`1CD0D4AFBB3D49F3EA7D3A75E640E53966109D44C2DDC74DE63BB495610ADF6E`.
Без force-helper/perf-map markers:

- Dreaming Sarah дошла до живого upright main menu, coverage
  `decoded=16384 native=16381 bridges=2 rejected=0`, HUD `present 60/s`;
- Void Terrarium++ (`PPSA03061`) дошла до живого upright main menu, coverage
  `decoded=16384 native=16379 bridges=4 rejected=0`, HUD `present 60/s`.

В обоих temporal repeats счётчики block/draw/present продолжали расти, `SIGSEGV` и
fatal signal отсутствовали. Это доказывает общий PS5 Current-JIT repair, а не
title-specific обход и не переход на helper.

## Кейс 4. Inside: одиночный кадр был корректным, но серия показывала белые/raw кадры

### Симптом и ложные направления

Inside (`CUSA02754`) доходил до gameplay, а отдельный screenshot часто выглядел
правильно. При временной серии, однако, появлялись полностью белые кадры, сырой шум и
кадры со старым/неверным render target. Первый 30-секундный контроль
`inside-a840-safe-final-30s` дал 9 событий, 20 плохих кадров из 248 и максимальную
связную область 100% экрана.

Проверка source extent, VO preferred target, DRS transaction и full-resolution tile
alias была необходима, но не объяснила зависимость дефекта от чистоты профиля. Узкие
исправления identity/coherency были сохранены только там, где восстанавливали общий
контракт; они не стали title-specific Vulkan-обходом.

### Доказанная причина: глобальные экспериментальные маркеры

В `files/lsx4-home` оставались 17 глобальных `run-enable-*` маркеров от профилирования
Bloodborne. Они включали для каждого последующего title рискованные shader payload,
SRT/TLS и PageManager fast paths. Поэтому A840 safe defaults фактически не были
effective для Inside: ручной глобальный маркер имел более высокий приоритет и переносил
непринятый Bloodborne A/B на другую игру.

Same-binary causal control с глобальным диагностическим preserve marker дал чистую
30-секундную серию `inside-preserve-clean-30s`: 251 кадр, 0 событий, максимальная
связная область 0,0036%. Это отделило marker leakage от DRS, температуры, разрешения и
случайного удачного screenshot.

### Исправление без отключения Bloodborne-профиля

Введён общий resolver `Common::TitleScopedOptimizationMarkerEnabled` в
`funnel-arm/src/common/title_scoped_optimization_marker.h`. Рискованные ручные routes
читают маркер только из
`files/lsx4-home/title-markers/<TITLE_ID>/<run-enable-...>`; старые глобальные
`run-enable-*` для них больше не авторитетны. Все 17 существующих маркеров на Vivo
скопированы в `title-markers/CUSA03173`, поэтому Bloodborne сохраняет свой принятый
набор, а Inside и остальные игры его не наследуют. Глобальные preserve/diagnostic
markers оставлены только для короткого причинного A/B и не являются production policy.

Изменение покрывает выбранные routes в `vk_pipeline_cache.cpp`, `page_manager.cpp`,
`amdgpu/liverpool.cpp` и `flatten_extended_userdata_pass.cpp`. На A840 автоматические
structural defaults остаются включёнными, а не доказанные payload defaults требуют
явного per-title маркера. Focused contracts (26/26) и Vulkan capability bridge прошли:

- `tests.test_title_scoped_render_optimization_markers_contract`;
- `tests.test_a840_inside_shader_payload_regression_contract`;
- SM8650 program/shader/data hot-stack contracts;
- `python tools/check_vulkan_capability_bridge.py`.

### Финальный temporal и performance gate Inside

Исправленный установленный APK и `base.apk` совпадают по SHA-256:
`31D6DAE78BDDE02ADECF2AE69A0C09510ECBC3FE061D299D777BAB0868856FAE`.
PS4 runtime внутри APK:
`3F5126590117280FB862F1447416C2EBE64A7AEEF46B778F6C5F4E74FFDF0228`;
PS5 runtime не изменён:
`11481F724FD32923D7BE01F832D4D97A96DE3CEAEFBE467F853904AE2F0A1072`.

Финальная gameplay-серия без глобального preserve marker
`inside-title-scoped-v2-30s` дала 255 кадров, 0 событий и максимум 0,0096%.
После этого только для `CUSA02754` включён уже существующий managed ARM GPU fast path;
глобальное значение осталось `false`. В той же стартовой сцене HUD вырос с
исторических 6 до 8 FPS (около +33%). Повторный независимый temporal gate
`inside-title-scoped-armgpu-proof-24s` дал 24 кадра за 24 секунды, 0 событий и
максимальную связную область 0,0207%. Ускорение принято только как per-game override,
поскольку оно не меняет effective настройки остальных titles.

Обязательный вывод: одиночный screenshot не может закрыть render regression. Любой
ручной performance toggle должен иметь явную область действия; экспериментальные
`run-enable-*` по умолчанию title-scoped. Global preserve допустим только для
краткого same-binary A/B и должен отсутствовать в финальном запуске.

## Кейс 5. Общая оптимизация ownership registry сломала Isaac, а узкая блокировка дала Inside 30+

### Отвергнутая оптимизация: нельзя менять общий recursive-lock registry по одному hot path

Попытка заменить legacy `thread_local std::unordered_map` в
`funnel-arm/src/common/recursive_lock.cpp` на inline registry выглядела как безопасное
удаление allocation/hash overhead. На практике новый registry изменил общий контракт
владения блокировками. The Binding of Isaac: Rebirth стал немедленно падать в typed HLE
переходе: `SIGSEGV`, чтение адреса `0x20`, стек через `internal_strcpy_s` и
`ExecutorFexDispatchHle`.

Причина была отделена one-factor A/B. Возврат HLE TLS scope и три независимых
preserve-проверки HLE locks не устранили crash. Возврат только
`recursive_lock.cpp` к legacy registry на том же executor APK восстановил загрузку
`rebirth.prx`, savedata и живой процесс. Поэтому inline registry полностью отвергнут,
а его contract-test удалён: тест не должен закреплять семантически неверную
«оптимизацию».

Обязательный вывод: ownership/recursion registry относится ко всему runtime, даже если
профиль показывает его внутри одной горячей подсистемы. Его нельзя менять без повторного
gameplay gate всех mutex/HLE-heavy golden titles. Crash в HLE-функции не доказывает, что
ошибка находится в HLE ABI: сначала нужен one-file A/B нижележащей синхронизации.

### Принятое общее исправление: recursive shared-first только у mapped-ranges

Вместо изменения общего registry добавлен generic `Common::RecursiveSharedFirstMutex`.
Он хранит reader ownership внутри конкретного mutex и применён только к
`Vulkan::Rasterizer::mapped_ranges_mutex`. Read-only mapped-range queries используют
`std::shared_lock`, mutation остаётся под exclusive `std::scoped_lock`. В коде нет
проверки `CUSA02754`, названия Inside или отдельной ветки по title: область действия
определяется структурой данных renderer, а не игрой.

Итоговый APK SHA-256:
`C9717C50A9CDAD7280AEFDEAB79BF859B47F820C4552202B0B10B60E44C75F66`.
На Vivo SM8850/A840 Inside дошёл до реального opening gameplay с правильной геометрией
и освещением. Proof-window `inside-c971-final-live60` дал:

- runtime present `34.167/s`, SurfaceFlinger `34.590 FPS` за `62.20 s`;
- SurfaceFlinger p50/p95 `33 ms`, p99 `50 ms`, доля `>40 ms` — `1.1147%`;
- средняя загрузка процесса `2.1248` ядра;
- `tz_shell` `37.42 -> 37.90 C`, то есть результат не объясняется холодным коротким
  burst;
- 5 новых JIT blocks за окно: измерение относится к steady gameplay, а не startup
  compilation.

Temporal gate `inside-c971-final-temporal30` содержит 30/30 уникальных кадров,
0 соседних дублей и 0 renderer-corruption events. Все кадры имеют размер
`3168x1440`; максимальная white fraction `0.02499%`, near-white `0.03200%`, диапазон
средней яркости `34.678..35.110`. Выборочная визуальная проверка кадров 0/10/20/29
подтвердила дождь, персонажа, геометрию и отсутствие белых/raw/stale кадров.

Этот результат принят как стабильные 30+ FPS без снижения разрешения, без апскейлера,
без нового per-title marker и без hardcode по Inside. Дополнительные рискованные
ускорения после достижения gate не вносились.

Обратный Bloodborne anchor выполнен на том же APK: adaptive route дошёл до Hunter's
Dream gameplay за `49.06 s`, runtime gate показал `23849 draw/s` и `4.00` занятых
ядра. Последующий кадр дал `38 FPS`, `23177 draw/s`, корректную геометрию и отсутствие
fatal signal. Сохранённый golden после прежнего Isaac fix в сопоставимой Hunter's Dream
сцене показывал `34 FPS` и примерно `16.2k draw/s`, поэтому новый общий renderer lock
не создал Bloodborne performance regression.

## Обязательный порядок для следующих изменений

1. До изменения сохранить APK SHA, settings SHA, requested/effective log, marker list,
   exact save/scene и temporal proof для затрагиваемой игры.
2. Менять одну причинную ось. Общий preset, GameDB, all-games scope и per-title scope
   считаются разными осями и не должны смешиваться без отдельной resolver-проверки.
3. Сначала focused static/unit contracts, затем gameplay gate пяти Vivo PS4 titles
   и затронутых PS5 Current-JIT titles.
4. После любого изменения CPU/JIT/FEX/HLE/worker/memory profile обязательно повторить
   Bloodborne/Vivo performance anchor до принятия APK.
5. После любой Bloodborne optimization обязательно повторить gameplay gate как минимум
   для titles, пересекающих изменённый слой: AIO, Unity/Mono, worker admission или HLE
   stack memory.
6. Общие defaults не могут скрыто перекрывать GameDB boot/performance constraints.
   Named presets сохраняют hard title constraints; Custom сохраняет явный выбор
   пользователя.
7. Title-specific значения допустимы только для title behavior (например, размер
   обязательного worker cohort). Vulkan/driver defects маршрутизируются через общий
   capability/quirk bridge, а не через title ID.
8. Не принимать boot/menu и одиночный screenshot. Нужны gameplay, progress, temporal
   stability и обратный запуск anchor title.
9. Любой accepted APK должен пройти обе оси в одной сборке. Результаты разных APK нельзя
   складывать в один verdict.
10. При регрессе сначала сравнивать effective logs и resolver output, а не повторять
    старые рискованные оптимизации или списывать разницу на thermal без доказательства.
11. После общего JIT/HLE изменения отдельно прогонять PS4/FEX и PS5/Current. Для PS5
    проверять `native>0`, `rejected=0`, растущие counters и отсутствие helper marker;
    PS4-прохождение не является substitute.
12. Перед каждым cross-title gate сохранять inventory `run-enable-*`. Рискованные
    маркеры обязаны быть title-scoped; render correctness принимается только по
    20--30-секундной temporal серии без белых/raw/старых кадров.

## Краткий запрет для будущего AI

Не расширяй playable-набор ценой уже playable-игр. Не исправляй compatibility ценой
Bloodborne performance. Не ускоряй Bloodborne ценой AIO/Mono/Unity/Isaac correctness.
Не принимай общий JIT/HLE patch по одному PS4/FEX gate: он обязан сохранить PS5
Current-JIT GPR coherency после каждого host/HLE boundary.
Любое общее изменение профиля обязано сохранить title hard constraints и пройти
двумерный `playable × performance` gate на одном итоговом APK.
Никогда не оставляй глобальный экспериментальный `run-enable-*`: он переносит
непроверенный performance A/B на весь playable-набор.
