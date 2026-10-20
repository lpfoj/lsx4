# Android Vulkan capability bridge

> [!IMPORTANT]
> Before any device/FPS/playable measurement, run `scripts/preflight-vivo-performance-test.ps1`;
> stale Android Studio process-trackers and known profiler loads make the result invalid. See the
> mandatory [performance-test preflight](PERFORMANCE_TEST_PREFLIGHT.md).

## Цель

Renderer не должен считать Adreno эталоном, а Mali отдельным портом. При создании `VkDevice`
формируется один профиль из трёх фактов: версия API устройства, включённое расширение и
соответствующий feature bit. После загрузки device dispatch дополнительно проверяется наличие
entry point. Все renderer paths используют только этот профиль.

Профиль потребляют оба Vulkan-слоя: общий PS4 renderer и изолированный PS5 presenter. PS5
presenter пока намеренно запрашивает Vulkan 1.1 и использует legacy render pass/barriers/descriptor
sets на всех GPU; это переносимый baseline, а не Mali-ветка. Быстрые optional функции там
включаются отдельно по тому же контракту.

Vendor/модель GPU не являются признаком поддержки функции. Отдельный quirk допустим только для
воспроизводимого дефекта драйвера, который ложно заявляет рабочую возможность.

Quirks также входят в единый профиль, а не разбросаны по renderer. Сейчас через него маршрутизируются
только подтверждённые ограничения: barrier внутри активного rendering scope, динамический
depth/stencil, physical-DRS resolve и PS5 wave-shader. Добавление нового vendor check непосредственно
в pipeline/scheduler/presenter запрещено: сначала он должен стать именованным quirk с безопасным
fallback и отдельной проверкой на втором семействе GPU.

## Матрица Vulkan

| Device API | Максимальный обязательный SPIR-V | Возможности, важные для renderer |
|---|---:|---|
| 1.0 | 1.0 | Базовые render pass, barriers, descriptor sets. Без timeline semaphore используется fence-backed virtual timeline; без buffer device address — flatbuffer/descriptor route. |
| 1.1 | 1.3 | Базовый Android-совместимый путь. Timeline/BDA могут прийти как KHR; dynamic rendering и synchronization2 не предполагаются. |
| 1.2 | 1.5 | Timeline semaphore и buffer device address входят в core, но соответствующие feature bits всё равно проверяются. |
| 1.3 | 1.6 | Dynamic rendering, synchronization2 и maintenance4 входят в core, но включаются только при положительных feature bits. |
| 1.4 | 1.6 | В core продвинуты push descriptors, dynamic rendering local read, maintenance5/6 и другие расширения; минимальный гарантированный `maxPushConstantsSize` увеличен с 128 до 256 байт. |

Версии обратно совместимы, но номер версии сам по себе не разрешает optional feature. На старом
API наличие расширения также не разрешает вызов core-имени команды: должен использоваться KHR
entry point либо проверенный alias.

## Карта маршрутов

| Возможность | Core route | Extension route | Безопасный fallback | Обязательность |
|---|---|---|---|---|
| Timeline semaphore | Vulkan 1.2+ + feature | `VK_KHR_timeline_semaphore` + feature + entry points | Fence-backed virtual timeline: один fence на физический submit, CPU wait для зависимости между scheduler streams | Опциональна |
| Buffer device address | Vulkan 1.2+ + feature | `VK_KHR_buffer_device_address` + feature + entry point | Flatbuffer/descriptor lowering без `PhysicalStorageBufferAddresses` и без BDA buffer usage | Опциональна |
| Dynamic rendering | Vulkan 1.3+ + feature | `VK_KHR_dynamic_rendering` + feature + KHR entry points | Кэшированные `VkRenderPass`/`VkFramebuffer` | Опциональна |
| Synchronization2 | Vulkan 1.3+ + feature | `VK_KHR_synchronization2` + feature + KHR entry points | Конвертация stage/access masks в `vkCmdPipelineBarrier` | Опциональна |
| Maintenance4 | Vulkan 1.3+ + feature | `VK_KHR_maintenance4` + feature + entry points | Старые memory-requirement/bind операции | Опциональна |
| Push descriptors | Core 1.4 либо KHR route | `VK_KHR_push_descriptor` + property + entry point | Обычный descriptor pool/set/update/bind | Опциональна |
| Dynamic rendering local read | Vulkan 1.4+ либо KHR | Extension + feature | Отключить и использовать обычные attachments | Опциональна |
| Fragment shading rate | Feature + поддерживаемый размер fragment | KHR route | 1x1 shading | Опциональна |
| Vertex attribute divisor | Поддержанная core/promoted семантика | EXT/KHR + feature | Статический vertex input без неподдержанного divisor | Опциональна |

## Таблица driver quirks

| Quirk | Где применяется | Безопасный маршрут |
|---|---|---|
| `AvoidInRenderingBarrier` | Scheduler | Завершить rendering scope и выполнить barrier вне него. |
| `RequireStaticDepthStencil` | Graphics pipeline | Включить depth/stencil state в ключ pipeline вместо неподдержанного dynamic state. |
| `DisablePhysicalDrsResolve` | Rasterizer/mobile DRS | Оставить логическое масштабирование без проблемного physical resolve. |
| `DisablePs5WaveShader` | PS5 presenter | Использовать portable shader path без неподтверждённого wave lowering. |
| `DeferAsyncPipelineReplacement` | Pipeline cache | Компилировать и сохранять оптимизированный pipeline в фоне, но публиковать его со следующего запуска. |
| `ForegroundPipelineWithoutDisableOptimization` | Pipeline cache | Не передавать проблемный флаг быстрого fallback pipeline. |
| `BrokenSpirvFloatControls` | Shader profile | Использовать уже существующую эмуляцию float-controls. |
| `RequirePostBindDynamicState` | Rasterizer | Повторно публиковать dynamic state после bind pipeline. |
| `AllowManualBufferBoundsOverride` | Shader profile | Разрешить диагностический manual-bounds fallback только на подходящем драйвере. |
| `CompactLegacyUnusedColorAttachments` | Legacy render pass и graphics pipeline | Удалять только хвостовые `VK_ATTACHMENT_UNUSED` слоты с `VK_FORMAT_UNDEFINED`, сохраняя внутренние MRT-индексы. Старый Qualcomm ICD иначе может разыменовать отсутствующий attachment внутри `vkCreateGraphicsPipelines`. Dynamic-rendering path не меняется. |
| `ConservativeImageTransitionStages` | Image transitions | Для marker-gated A/B только на точном proprietary Adreno 750 использовать прежнюю консервативную destination-stage маску `ALL_GRAPHICS | COMPUTE` для нетрансферных переходов. A840/Gen5, Turnip, Mali и desktop сохраняют точные attachment stage masks. |
| `ExplicitFullImageSubresourceCounts` | Image transitions | Для marker-gated A/B только на точном proprietary Adreno 750 заменять sentinel-значения `VK_REMAINING_MIP_LEVELS/LAYERS` в полном image barrier фактическими mip/layer counts. Это изолирует подтверждённый crash фирменного ICD в `vkCmdPipelineBarrier2`; Gen5/A840, Turnip, Mali и desktop не меняются. |
| `ReadOnlyDepthStencilAccess` | Depth/stencil attachment transitions | Для marker-gated A/B только на точном proprietary Adreno 750 публиковать `DEPTH_STENCIL_ATTACHMENT_READ` без `WRITE`, когда layout является полностью read-only. Это устраняет наблюдавшуюся в crash-capture несогласованную пару `oldLayout=DEPTH_STENCIL_READ_ONLY` + `srcAccess=DEPTH_STENCIL_ATTACHMENT_WRITE`; A840/Gen5, Turnip, Mali и desktop сохраняют текущие access masks. |
| `SplitDepthStencilImageBarriers` | Depth/stencil image barriers | Для marker-gated A/B только на точном proprietary Adreno 750 и только при поддержке `separateDepthStencilLayouts` разделять совместный `DEPTH|STENCIL` barrier на два непересекающихся aspect barrier с aspect-specific layouts. Это обходит исследуемый proprietary-ICD crash в `vkCmdPipelineBarrier2`; A840/Gen5, Turnip, Mali и desktop сохраняют текущий единый barrier. |
| `RequireStorageBufferReadWriteBarrier` | Buffer-cache SSBO dependencies | Только для точного proprietary Adreno 750 writable storage descriptor получает `SHADER_READ | SHADER_WRITE`, а повторный read/write-доступ после shader write не отбрасывает barrier из-за совпавших stage/access. Это закрывает пропущенную RAW/WAW dependency внутри одного command buffer; A840/Gen5, Turnip, Mali и desktop сохраняют прежний маршрут. |
| `AvoidRequiredSubgroupSizeForSubgroupIndependentCompute` | Compute pipeline shader stage | Только для exact proprietary Adreno 840 не передавать лишний `VkPipelineShaderStageRequiredSubgroupSizeCreateInfo`, когда SPIR-V не использует subgroup semantics. Реальные subgroup shaders сохраняют Wave64. Отрицательный A750 A/B остаётся отключённым; A750, Turnip, Mali и desktop сохраняют baseline path. |
| `DeferFormattedBufferTypesUntilLowering` | Shader resource tracking/lowering | Только для compute shader на точном proprietary Adreno 750 не фиксировать консервативную маску `U32|F32|U16|U8` у formatted buffer до `LowerBufferFormatToRaw`; после выбора raw opcode в descriptor OR-ится только фактический `U8`, `U16`, `U32` или `F32`. Binding/layout/write semantics не меняются, runtime synchronization не добавляется. A840/Gen5, Turnip, Mali, graphics stages и desktop сохраняют прежний codegen и прежнюю shader-cache identity. |
| `LowerAliasedSharedU64ToU32` | Shader shared-memory simplification | Только для compute shader на точном proprietary Adreno 750, который одновременно использует `U32` LDS view и неатомарные `U64` load/store, разложить каждый `U64` доступ на две существующие `U32` операции. Turnip, Mali, A840 и desktop сохраняют этот маршрут выключенным. |
| `LowerNon32BitSharedToU32` | Shader shared-memory simplification | Только для compute shader на точном proprietary Adreno 840 понижать неатомарные `U16` и `U64` LDS load/store до `U32`. Это воспроизводит уже подтверждённый на устройстве безопасный результат missing-explicit-layout path для shader `0x615f180c`; более узкое понижение только `U64` снова воспроизвело `libllvm-qgl+0x91bae0`, поэтому было отклонено. После прохода единственный `U32` view возвращается до storage fallback: новых SSBO, descriptor, синхронизаций и runtime-проверок нет. Рекламируемая explicit-layout capability остаётся доступна остальным shaders; Turnip, Mali, A750 и desktop не затрагиваются. |
| `ElideCpuCompletionSidecarBarrier` | PM4 completion ordering | Только для точной связки Mesa Turnip + Adreno 840 не испускать глобальный `ALL_COMMANDS` barrier для CPU-side EOP/EOS completion-label sidecar. Реальные buffer/image hazards и transfer barriers сохраняются. На Vivo A840 короткий moving B-прогон сохранил корректную геометрию и дал 30 FPS; marker opt-out оставлен для fail-closed A/B. A750/8gen3, proprietary Qualcomm, Mali и desktop сохраняют прежний маршрут. |

## Профили тестовых устройств

| Устройство | GPU/драйвер | Device API | Выбранный профиль |
|---|---|---:|---|
| Tecno | Mali-G77 MC9, r32p1 | 1.1.177 | Timeline=KHR, BDA=KHR, rendering=legacy, sync2=legacy, maintenance4=legacy, push=descriptor sets, SPIR-V 1.3. |
| Redmi Note 6 Pro | Adreno 509, Android 9 | 1.1 | Rendering/sync2/timeline/BDA use legacy routes, SPIR-V 1.3. Auxiliary RectList/QuadList shaders must use that same target; SPIR-V 1.5 makes this driver crash in `vkCreateGraphicsPipelines`. Legacy subpasses compact trailing undefined color slots before pipeline creation. |
| Vivo | Adreno 840 | 1.4 | PS4: timeline/BDA/rendering/sync2/maintenance4=core, push=KHR, local-read и FSR доступны, SPIR-V 1.6. PS5 target 1.1: timeline=KHR, остальное legacy, SPIR-V 1.3. |
| Xiaomi/POCO SM8650 | Adreno 750, proprietary Qualcomm ICD | 1.3 | Timeline/BDA/rendering/synchronization2/maintenance4 выбираются обычным capability profile. Exact-A750 legacy-sync2 A/B отклонён: он перенёс SIGSEGV из `vkCmdPipelineBarrier2` в `vkCmdPipelineBarrier`, не исправив геометрию. Required-subgroup-size omission также отклонён; baseline Wave64 восстановлен. Активные compile-time меры уточняют stale formatted-buffer type mask после lowering и убирают перекрывающийся неатомарный `U64` LDS view только когда уже нужен `U32` view. |

## Семейства Mali и драйверные стеки

Эта таблица служит для выбора тестового покрытия и performance policy, но не для включения Vulkan
features. Единственный источник истины для features — фактические properties/features/extensions и
device entry points. Даже один GPU IP может получить разные DDK от OEM.

| Семейство | Примеры, не полный список | Frontend/особенности | Политика LSX4 |
|---|---|---|---|
| Utgard | Mali-400/450 | Vulkan отсутствует; Mesa использует Lima, а не PanVK. | Чистый отказ инициализации Vulkan. Не пытаться эмулировать обязательные возможности программно. |
| Midgard v4/v5 | T600/T620/T720, T760/T8xx | Старый Job Manager, tile-based deferred renderer. Некоторые proprietary DDK предоставляли Vulkan 1.0; актуальная таблица PanVK Vulkan для Midgard не заявляет. | Только фактически обнаруженный минимальный профиль; legacy render pass/barriers/descriptors, fence timeline и flatbuffer route. |
| Bifrost v6/v7 | G71/G72, G31/G51/G52/G76 | Job Manager, IDVS и quad-based shader ISA. PanVK перечисляет Vulkan 1.3 для части v6/v7, но не заявляет их Vulkan-conformant. | Не доверять только `apiVersion`. Проверять feature bits, entry points и driver identity; использовать отдельный pipeline-cache namespace. |
| Ранний Valhall | G57/G68/G77/G78 | Job Manager; более быстрый texture path и Valhall scalar ISA, но ещё без CSF. До G710 tile-memory dependencies имеют один tracker на pixel и могут сериализовать сложные merged subpasses. | Профиль уровня установленного DDK. Tecno G77 r32p1 фактически выбирает Vulkan 1.1/KHR/legacy. Не считать dynamic rendering/sync2 доступными по имени GPU. |
| Valhall CSF | G310/G510/G610/G710 и последующие Valhall | Начиная с G710-семейства Command Stream Frontend заменяет Job Manager, снижает CPU driver overhead и даёт нативные secondary command buffers/indirect draws. | Можно разрешать более агрессивное command-buffer coalescing только по измерениям. Capability route всё равно выбирается независимо от поколения. |
| 5th Gen | G720/G725, G1 Pro/Premium/Ultra | CSF; более новые PanVK-профили перечисляют Vulkan 1.4 для поддержанных v12–v14. Конкретный OEM proprietary DDK может отличаться. | Core 1.3/1.4 paths только после feature/entry-point проверки; legacy fallback сохраняется и регулярно тестируется. |

По состоянию на 2026-08-03 Mesa документирует PanVK как Vulkan-conformant только на Mali-G610;
остальные перечисленные реализации могут предоставлять Vulkan API без conformance. Поэтому ключ
дискового pipeline cache обязан включать как минимум `vendorID`, `deviceID`, `driverID`,
`driverVersion` и `pipelineCacheUUID`, а кэш другого ICD использовать нельзя.

Источники: [актуальная таблица Panfrost/PanVK](https://docs.mesa3d.org/drivers/panfrost.html),
[Mali-G710 и переход Job Manager → CSF](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/mali-g710-developer-overview),
[Valhall shader-core guide](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/The%20Valhall%20Shader%20Core.pdf).

## Вехи proprietary Mali DDK

Номер DDK — диагностический признак, но не замена extension enumeration: OEM может задержать,
отключить или модифицировать возможность.

| DDK | Возможность, впервые указанная Arm | Действие bridge |
|---|---|---|
| r36p0 | `VK_ARM_rasterization_order_attachment_access` | Не включать ARM-вариант автоматически; только extension + feature. |
| r38p0 | `VK_EXT_subpass_merge_feedback` | Использовать feedback для телеметрии, не считать merge гарантированным на старых OEM. |
| r40p0 | `VK_EXT_rasterization_order_attachment_access` | Предпочитать кросс-вендорный EXT-вариант ARM-варианту при одинаковом контракте. |
| r44p1 | `VK_EXT_shader_tile_image` | Optional tile-memory path; обычный sampled attachment остаётся fallback. |
| r49p1 | `VK_KHR_dynamic_rendering_local_read` | Предпочтительный modern tile-read route; проверять feature и все entry points. |
| r51+ | Mali Timeline debug markers через `VK_EXT_debug_utils` в Arm Performance Studio | Только диагностика; отсутствие не влияет на совместимость. |

Arm отмечает, что совместимые subpasses могут merge на всех Arm Vulkan implementations, но старые
OEM иногда отключали merge. Для G710-серии и новее Arm не знает таких отключений. Это всё равно
performance hint, а не correctness contract. Источник:
[Arm: Vulkan subpasses](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/vulkan-subpasses-the-good-the-bad-and-the-ugly).

## Mali correctness contract

### Tile memory и render scopes

1. Legacy render pass на старом Mali — полноценный оптимальный маршрут, а не временная заглушка.
   Dynamic rendering само по себе не гарантирует более быстрый кадр.
2. Не разрывать render scope между проходами, которым достаточно same-pixel attachment access.
   Однако merge нельзя предполагать: при наличии `VK_EXT_subpass_merge_feedback` записывать его
   результат, иначе оценивать только по counters/capture.
3. Если старое содержимое attachment не нужно, использовать `loadOp=CLEAR` или `DONT_CARE`; если
   результат не читается после scope — `storeOp=DONT_CARE`. Нельзя менять LOAD/STORE ради
   производительности без доказанной liveness attachment.
4. Transient depth/G-buffer могут использовать `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT` и
   lazily-allocated memory только если подходящий memory type реально возвращён устройством.
5. Не заменять два корректных passes на merged subpasses без измерения. До G710 сложный tile access
   может терять clock-for-clock performance из-за одного dependency tracker на pixel; G710+ имеет
   более точные trackers и 32×32 tiles.

Источник: [Arm load/store recommendations](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/picking-the-most-efficient-load-store-operations).

### Barriers, layouts и Transaction Elimination

1. На G710/G610/G510/G310 и новее CSF разделяет transfer, compute, geometry и fragment command
   streams. Слишком широкий barrier создаёт межпоточные bubbles. На ранних Job Manager GPU цена и
   mapping другие, поэтому архитектурно-зависимый barrier hardcode запрещён.
2. Для images задавать точные subresource/stage/access/layout. Общий `VkMemoryBarrier` применять
   только когда ресурс неизвестен: он может зря инвалидировать texture cache.
3. Для buffer barrier на современных CSF Arm рекомендует один диапазон `offset=0,
   size=VK_WHOLE_SIZE`, без дублирования одного buffer в списке. Это performance policy, не причина
   ослаблять требуемую memory dependency.
4. Не переходить каждый кадр из `VK_IMAGE_LAYOUT_UNDEFINED`, если содержимое живо. Такой переход
   инвалидирует metadata Transaction Elimination и заставляет снова писать неизменённые tiles.
5. Не моделировать Mali image как runtime-состояние «compressed/uncompressed»: Arm указывает, что
   image создаётся в одном из состояний. Выбор AFBC принадлежит ICD/модификатору, а не названию GPU.
6. Legacy sync2-конвертер остаётся консервативным для неизвестных bits. `ALL_COMMANDS +
   MEMORY_READ/WRITE` допустим как редкий correctness fallback, но должен логироваться и не
   становиться steady-state путём.

Источник: [Arm: mapping Vulkan barriers to CSF Mali](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/vulkan-synchronization-how-pipeline-barriers-map-to-mali-gpu-hardware).

### Geometry memory и `VK_ERROR_DEVICE_LOST`

Desktop-игры могут превысить внутреннее хранилище intermediate geometry даже при формально
корректном Vulkan. Arm документировал лимит 180 MB для рассмотренного поколения Mali. В расход
входят varyings для диапазона индексов между min/max, а также все вершины tessellation/geometry
shader до clipping.

При device-lost с большим vertex load bridge должен записать: число draws, min/max index span,
vertex count, bytes of varyings и наличие tessellation/geometry. Безопасный fallback — завершить
hardware render pass раньше и продолжить incremental rendering. Нельзя бесконечно повторять тот же
submit или считать ошибку shader compilation.

Источник: [Arm: memory limits with Vulkan on Mali](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/memory-limits-with-vulkan-on-mali-gpus).

### Formats, MSAA, descriptors и shader semantics

1. Все sampled/storage/color/depth возможности проверять через
   `vkGetPhysicalDeviceFormatProperties2`; наличие Vulkan версии не гарантирует конкретную
   комбинацию format + tiling + usage.
2. Host texture compression выбирается только из реально поддержанных ASTC/ETC2/BC format
   features. Guest compression и host compression — разные контракты.
3. Более 4× MSAA на старых Mali может не иметь полного throughput. Separate
   `vkCmdResolveImage` крайне bandwidth-heavy; при возможности использовать inline/on-tile resolve
   attachment. Sample count всегда ограничивается device properties.
4. `VK_EXT_descriptor_indexing` впервые появился у proprietary Mali начиная с G77, но каждый
   descriptor-indexing feature bit проверяется отдельно. Динамически неоднородный bindless index
   нельзя генерировать без соответствующего non-uniform feature.
5. Не понижать fp32 до fp16 только по поколению Mali: это управляемая quality optimization после
   доказательства допустимой точности guest shader. Аналогично anisotropy/trilinear/filtered-fp32
   имеют мультипликативную стоимость и регулируются настройкой, а не compatibility quirk.
6. PS4/PS5 wave/subgroup semantics не выводить из nominal subgroup size. Проверять subgroup stages,
   supported operations и required-size features; иначе использовать portable lowering.

Arm best-practice validation отдельно предупреждает о >4× MSAA и separate resolve:
[Arm checks in Khronos validation](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/arm-best-practice-warnings-in-vulkan-sdk).

### Command buffers, WSI и память

1. До G710 secondary command buffers и indirect multi-draw не имели нативного CSF исполнения и
   могли быть дороже; G710+ исполняет их нативно. Correctness одинаков, а batching policy выбирается
   по профилю/измерениям.
2. Не путать внутренние CSF hardware queues с Vulkan queue families: наружные queue capabilities
   могут остаться теми же.
3. Swapchain `preTransform` должен учитывать `currentTransform`; иначе Android compositor может
   добавить отдельный rotation pass и bandwidth cost.
4. Mali обычно использует unified memory, но комбинации DEVICE_LOCAL/HOST_VISIBLE/HOST_COHERENT и
   memory budgets берутся только из конкретного устройства. Нельзя переносить discrete-GPU staging
   policy с desktop без измерения.
5. Pipeline/shader cache разделяется по GPU и ICD identity. После driver update cache считается
   несовместимым, даже если модель GPU та же.

Источник по WSI: [Khronos surface rotation sample](https://docs.vulkan.org/samples/latest/samples/performance/surface_rotation/README.html).

## Чек-лист закрытия нового frontier на Mali

1. Сохранить normalized capability log и полный список enabled extensions/features.
2. Проверить фактические format properties всех render/storage/depth targets.
3. Убедиться, что профиль не активировал core-имя команды на extension-only DDK.
4. Проверить legacy render-pass/sync/descriptor route даже на новом Mali.
5. Для артефакта сравнить два GPU capture: resource lifetime, load/store, layouts, descriptor
   contents и ring-buffer retirement; quirk добавлять только после локализации дефекта драйвера.
6. Для device-lost отдельно проверить intermediate geometry pressure и VRAM/RAM budget.
7. Для низкого FPS смотреть Mali counters: external read/write bytes, tiler utilization, fragment
   utilization, cache misses и barrier bubbles. Средний FPS в разных сценах не доказывает причину.
8. На debug-сборке запускать synchronization validation и Arm vendor best-practice validation.
9. Повторить тот же frontier на Adreno: Mali fallback не должен отключить общий fast path.
10. Добавить выявленную capability или quirk в эту таблицу и в автоматический guard до завершения
    задачи.

## Проверенные маршруты 2026-08-03

| Устройство | Слой/игра | Результат |
|---|---|---|
| Tecno Mali-G77 | PS4 Sonic Mania | Корректные цвет/геометрия, title/save/gameplay, 56–61 FPS, без device-lost. |
| Tecno Mali-G77 | PS4 Minit | Intro/title корректны, 60 FPS, без device-lost. |
| Tecno Mali-G77 | PS5 Dreaming Sarah | Splash корректен; PS5 профиль `1.1/KHR timeline/legacy renderer`, без device-lost. |
| Vivo Adreno 840 | PS4 Dead Cells | Core 1.4 fast paths сохранены, меню корректно, 60 FPS. |

## Инварианты для следующих изменений

1. Capability считается поддержанной только как `core version + feature` либо
   `enabled extension + feature + entry point`.
2. Отсутствующая optional capability всегда выбирает заранее реализованный fallback; ветка не
   должна оставаться частично активной.
3. SPIR-V target вычисляется только из device API и никогда из render-pass/vendor path.
4. `VkPhysicalDevice*Features` в device chain содержит только реально выбранные возможности.
5. Прямые `pipelineBarrier2`, `beginRendering` и push-descriptor calls запрещены вне route-aware
   обёртки или ветки.
6. Неизвестный stage/access bit при переводе synchronization2 в legacy превращается в
   консервативный `ALL_COMMANDS + MEMORY_READ/WRITE`, а не отбрасывается.
7. Любой новый optional fast path обязан иметь строку в этой таблице, нормализованный лог и
   проверку как минимум на одном Mali и одном Adreno.
8. Vendor quirk не меняет сведения о capability: он только выбирает более консервативный маршрут.
   Поэтому отключение fast path на Mali не отключает его на Adreno и не маскирует отсутствие feature.
9. Поведенческие сравнения vendor/driver вне `vk_instance.cpp` и
   `vk_capability_profile.h` запрещены. Renderer и presenter используют только `HasQuirk()`.
10. `tools/check_vulkan_capability_bridge.py` является build dependency обеих Android runtime.
    Изменение, которое обходит профиль, должно остановить локальную сборку и CI.
11. `CapabilityRoute::Legacy` для timeline означает fence-backed retirement, а не пропуск
    зависимости. Между scheduler streams готовность кадра подтверждается CPU wait по master tick.
12. `CapabilityRoute::Legacy` для BDA запрещает одновременно buffer usage
    `eShaderDeviceAddress`, вызов `getBufferAddress` и SPIR-V capability
    `PhysicalStorageBufferAddresses`; пользовательская настройка direct memory этот gate не обходит.
13. Вспомогательные renderer shaders (включая RectList/QuadList TCS/TES) получают SPIR-V target из
    `CapabilityProfile` так же, как guest shaders. При загрузке сериализованного pipeline их magic и
    version перепроверяются; кэш с target выше профиля пересобирается до вызова драйвера.

## Источники

- Khronos Vulkan Versions and Porting Guide: https://docs.vulkan.org/guide/latest/versions.html
- Khronos Vulkan Release Summary: https://docs.vulkan.org/guide/latest/vulkan_release_summary.html
- Android Vulkan Profiles: https://developer.android.com/ndk/guides/graphics/android-vulkan-profile
- Android native engine Vulkan support: https://developer.android.com/games/develop/vulkan/native-engine-support
