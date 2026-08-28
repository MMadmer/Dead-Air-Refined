# Аудит IX-Ray 1.6 (stcop) → Dead Air: Refined

Донор: `D:\Games\ixray-1.6-stcop`, 4400 коммитов, 2019-06…2026-08.
Реципиент: `D:\Games\Dead Air\DeadAir-x64`.
Восемь областей промайнено, каждая перепроверена вторым проходом. Ниже — только то, что выжило проверку. Где майнер и верификатор разошлись — вердикт верификатора, и я это отмечаю явно.

Короткий итог: **донор в этой области в основном позади нас.** По load-time, стримингу текстур, CDB-кэшу, occlusion, crash-репортам, save-пайплайну, luabind и string pool мы впереди. Реально брать есть что, но это не «большие фичи» — это ~15 мелких патчей на память/потоки плюс один наш собственный баг, который дороже всего донорского списка вместе взятого.

---

## 1. TL;DR — что реально стоит времени

| # | Что | SHA | Наш файл | Цена |
|---|-----|-----|----------|------|
| 1 | **Наш собственный** O(n) на каждой строке лога после 40k строк — регрессия 5-дневной давности | нет (наш `41ef72ba0`) | `src\xrCore\log.cpp:41-45` | 10 мин |
| 2 | `CParticleGroup`: локи в dtor, `Compile`, `OnDeviceCreate/Destroy` **и в обходе `pG->items` в dsgraph** | `8d8f425c1` + `51d83094c` | `ParticleGroup.cpp:472,541,586`, `r__dsgraph_build.cpp:324,540` | час |
| 3 | Гонка на `CBlendInstance::Blend` — писатель реально живой через `mtObjectHandler` | `1fe8ffc51` | `SkeletonAnimated.cpp:26,41` | 1 строка |
| 4 | `intrusive_base::m_ref_count` — не атомарный | `cbeb34f6e` | `src\xrCore\intrusive_ptr.h:32` | 1 строка |
| 5 | `u8* dest;` без инициализации + проглоченный bool `_decompressLZ` | `935b6448b` (только 2 строки) | `src\xrCore\FS.cpp:312, :362` | 5 мин |
| 6 | `ISpatial_DB::insert` дерефает `m_root` без проверки | `f16870228` (**не** `efcda373a`) | `src\xrCDB\ISpatial.cpp:310-323` | 1 строка |
| 7 | `CCF_Shape::_RayQuery` считает дистанцию хита в смешанных системах координат | `8f8dda20c` (только хунк `_RayQuery`) | `src\xrEngine\xr_collide_form.cpp:407-429` | 15 мин |
| 8 | Лут трупа: null-трейдер уходит в `RELATION_REGISTRY()` | `01487d11b` + сосед | `src\xrGame\ui\UICharacterInfo.cpp:296, :130` | 10 мин |
| 9 | `set_switch_online/offline/interactive` — `VERIFY` + деref | `3a510ebad` | `src\xrGame\alife_update_manager.cpp:365-384` | 5 мин |
| 10 | Null-гарды в control-командах монстров (`data()` реально возвращает 0) | `a87e7042d` + `cda4c9783` + `d34966c3e` | `control_run_attack.cpp:34,41,102`, `control_threaten.cpp:21,29,43`, `controller_psy_hit.cpp:84,142,275` | час |

Ниже по списку, но тоже дёшево: `GAMEMTL_SUBITEM_COUNT` (крашит на контент-моде), `g_bRendering` → atomic, торговля с бесконечными деньгами, `m_throw_ignore_object`.

---

## 2. Критично

### 2.1. Наш `log.cpp` — O(n) на строку, навсегда

Нашёл верификатор перф-области, донор тут ни при чём.

```
// src\xrCore\log.cpp:41-45
constexpr size_t log_lines_kept = 40000;
if (LogFile.size() >= log_lines_kept)
    LogFile.erase(LogFile.begin(), LogFile.begin() + (LogFile.size() - log_lines_kept + 1));
LogFile.push_back(split);
```

Как только `LogFile` дорос до 40000, размер там и залипает, `>=` срабатывает на **каждом** последующем `Msg()`, и каждый раз стирается ровно один элемент с **начала** вектора из 40000 `xr_string` — ~40k move-assign'ов на строку лога, внутри `logCS`, на вызывающем потоке. До конца сессии. Пришло с нашим же `41ef72ba0` (2026-08-23), подтверждено `git log -S"log_lines_kept"`.

Скрипты Dead Air болтливые, 40000 строк перебиваются задолго до конца сессии. Это ровно тот post-load steady-state столл, который мы гоняли в `_analysis\delayed-load-freeze\`.

Чинится ring buffer'ом или амортизацией (сбрасывать по 4000, не по 1). Заодно в той же функции: `log.cpp:37-38` зовёт `OutputDebugString` безусловно, дважды на строку. Ваниль и донор оба гардили это `IsDebuggerPresent()` — мы гард потеряли. Вернуть (закешировать один раз в `xrDebug::Initialize`, не звать per-line).

**Плюс четвёртая alloca-площадка ровно рядом:** `log.cpp:65` `Log(pcstr s)` делает `xr_alloca(xr_strlen(s)+1)` на каждой строке, а двухаргументный `Log(msg, dop)` на `:109` аллокает `strlen+strlen+2` и потом зовёт `Log(buf)`, который аллокает столько же **ещё раз**. Через `Msg()` это ограничено `string2048`, но прямые вызовы — нет, и конкретный — `src\Layers\xrRenderPC_R4\r4_shaders.cpp:656`: `Log("! error: ", pErrorBuf->GetBufferPointer())` кормит сырой D3D-блоб ошибок компилятора через обе alloca. Модифицированный `.hlsl` с парой сотен ошибок — 100+ КБ на стеке дважды, на пути отказа. Правится одним заходом вместе с ring buffer'ом.

Риск: нулевой. Формат сейва, геймплей, парити — не трогается.

---

### 2.2. `CParticleGroup` — тот же класс, что мы уже чинили в 1.3.4, но дыры остались

**Что видит игрок:** случайный краш или мусорная геометрия в `CParticleManager::Update` / `CModelPool` — та же сигнатура, что дала 1.3.3.

Донор: `8d8f425c1` (2025-05-25) + предшественник `51d83094c`.

Наше состояние — **четыре** незалоченных места, майнер краш-области нашёл одно:

| Место | Наш файл |
|---|---|
| `~CParticleGroup` чистит `items` | `ParticleGroup.cpp:472-478` |
| `Compile()` — `items.clear()` + `resize()` | `ParticleGroup.cpp:541-556` |
| `OnDeviceCreate` / `OnDeviceDestroy` | `ParticleGroup.cpp:586-596` |
| **Обход `pG->items` в dsgraph** | `r__dsgraph_build.cpp:324-334` (`add_leafs_dynamic`), `:540-552` (`add_Dynamic`) |

Для сравнения, локи уже стоят в `OnFrame` (:482), `UpdateParent` (:537), `Stop` (:575), `ParticlesCount` (:602), `SetHudMode`/`GetHudMode` (:611/:621). `Lock render_lock` объявлен приватным на `ParticleGroup.h:80` — под dsgraph-хунк придётся сделать public или дать аксессор.

**Триггер уточнён верификатором, майнер описал его неверно.** «Игровой поток зовёт `Stop()`/`Play()` пока рендер-таска итерирует» невозможно: `device.cpp:283-372` доводит `FrameMove()` до конца и только потом запускает `processSeqParallel` и `DoRender`. Реальный триггер — `CLevel::script_gc` (`src\xrGame\Level.cpp:580`, `mtLUA_GC` есть в дефолтном `g_mt_config` на `console_commands.cpp:139`): Lua-финализаторы крутятся на seqParallel-воркере **одновременно** с `DoRender`, собранный `CParticlesObject` уходит в `~CParticleGroup` → `SItem::Clear` → `model_Delete` (`ParticleGroup.cpp:188`) на этом воркере, пока рендер-таска ходит по тем же `items`.

Наш `Lock` — это `CRITICAL_SECTION` (`src\xrCore\Threading\Lock.cpp:8-13`), то есть рекурсивный, так что опасений по самодедлоку нет.

**Риск:** нулевой для парити/сейвов, чистое добавление локов. Оговорка: лок закрывает гонку итерации, но **не** дыру по времени жизни — объект-то освобождается. См. п. 2.3.

---

### 2.3. Время жизни `CPS_Instance` — контейнер мутируется вообще без лока

Это шире донорского фикса и я выношу отдельно, потому что тут донорский код брать **нельзя**, а дыра настоящая.

`CPS_Instance` в конструкторе делает `g_pGamePersistent->ps_active.insert(this)` (`src\xrEngine\PS_instance.cpp:15`), в деструкторе — `erase` из `ps_active` и чистку `ps_destroy` (`:25-35`). Оба — без единого лока. Поскольку `script_gc` крутит Lua-финализаторы на seqParallel-воркере параллельно с `DoRender`, `xr_set::insert`/`erase` может выполниться, пока рендер разыменовывает инстансы из того же контейнера.

Донорский `51d83094c`/`1379ef8c4` (переход на `xr_vector<xr_shared_ptr<CPS_Instance>>` + флаг `m_NeedDestroy` + `remove_if`) — **не брать**: у них `m_NeedDestroy` это plain `volatile bool`, в `PSI_internal_delete` оставлен per-frame отладочный `Msg` и `VERIFY` с кракозябрами. Наш путь — либо свой shared_ptr, либо defer-to-main-thread, отдельным изменением, с перетестом порядка `destroy_on_game_load`.

Наш код: `src\xrEngine\IGame_Persistent.cpp:503-535` (`OnFrame`), `:541-577` (`destroy_particles`), потребитель — `src\xrGame\ParticlesObject.cpp:295-321`.

---

### 2.4. `CBlendInstance::Blend` — гонка с реальным писателем

**Здесь верификаторы разошлись, и я беру сторону threading-верификатора: он проследил поток, краш-верификатор — нет.**

- Краш-область: «skip, профилактика, мы не считаем кости в отдельном потоке».
- Threading-область: писатель есть. `CAI_Stalker::update_object_handler` ставится в `seqParallel` на `src\xrGame\ai\stalker\ai_stalker.cpp:826-838` при выставленном `mtObjectHandler` — а `mtObjectHandler` **есть в дефолтном `g_mt_config`** (`src\xrGame\console_commands.cpp:139-140`). Он крутит `CObjectHandler::update()` (стейт-машина доставания/убирания оружия, которая гоняет анимации) на воркере, пока рендер-воркеры сидят в `CalculateBones`.

Наше: `SkeletonAnimated.cpp:26` (`blend_add`) и `:41` (`blend_remove`) — без лока. Читатель `src\Layers\xrRender\SkeletonRigid.cpp:53` берёт `UCalc_mtlock` (глобальный `Lock UCalc_Mutex`, `SkeletonCustom.h:23-28`), которого писатели не берут.

Писатели на `SkeletonAnimated.cpp:268/623` уже под локом — но не потому, что они его берут (в этом файле `UCalc` не встречается вообще, threading-верификатор тут ошибся в атрибуции). Лок берётся на этаж выше, в `CKinematics::CalculateBones` (`SkeletonRigid.cpp:53`), и доходит через `OnCalculateBones()` → `UpdateTracks()`. Открыты `LL_PlayCycle`/`LL_FadeCycle` на `:425/:529/:543`.

**Фикс:** взять `UCalc_mtlock` в `Bone_Motion_Start/Stop` — одна строка, переиспользует мьютекс, который читатели уже держат, не плодит per-bone SRWLOCK (а костей много). Донорский `xrSRWLock` в `CBlendInstance` не нужен.

**Связка, которую нельзя проморгать:** это работает только потому, что `Lock` рекурсивный. Наш собственный комментарий на `SkeletonRigid.cpp:57` это фиксирует. Если кто-то потом возьмёт донорский `xrSRWLock` (`55c0c366a`) и переведёт на него `UCalc_Mutex` — `CalculateBones` задедлочится на первом же кадре. Правило: новый shared-lock к `UCalc_Mutex` не подпускать.

---

### 2.5. `CCF_Shape::_RayQuery` — дистанция хита считается в двух разных системах координат

Ни одна из восьми областей это не покрыла: три открывали `xr_collide_form.cpp` и все три смотрели только на `CCF_Skeleton`.

`src\xrEngine\xr_collide_form.cpp:407-429`, ветка box:
- `B.transform_tiny(S1, dS)`, где `B = shape.data.ibox` — перевод object → box-local;
- `box.Pick2(S1, D1, P)` возвращает `P` **в box-local**;
- `float d = P.distance_to_sqr(dS)` — а `dS` всё ещё в **object space**.

Решение hit/miss (`rp_res`) корректно, а вот возвращаемый `range` — бессмысленное число в смешанных единицах, доминируемое `|dS|`. Ломает упорядочивание `OPT_ONLYNEAREST` и всех потребителей дистанции: пулевые рейпики, накопление видимости в `Feel::Vision`, `CHUDTarget`.

Донор `8f8dda20c` (2026-01-13) переводит `P` обратно через `shape.data.box` и меряет вдоль луча. Механически применимо: `box` и `ibox` — это анонимная структура **внутри** union'а (`xr_collide_form.h:190-198`), не альтернативы union'а, и `add_box` (`:445-451`) пишет обе. **Поставь комментарий**, иначе будущая «уборка», которая сделает их настоящими альтернативами union'а, превратит фикс в чтение обратной матрицы без единой ошибки компиляции.

**Второй хунк того же коммита не брать.** Он меняет `K->LL_GetTransform(I->elem_id)` на `LL_GetBoneLocalTransform` в `CCF_Skeleton::BuildState` — world→local для каждого элемента костной коллизии. Это сдвинет все хит-сферы и боксы скелетов в игре. Противоположный профиль риска, тот же SHA.

---

### 2.6. Атомарность рефкаунтов

| Что | Донор | Наше | Вердикт |
|---|---|---|---|
| `intrusive_base::m_ref_count` | `cbeb34f6e` | `src\xrCore\intrusive_ptr.h:32` — `size_t`, `++`/`--` голые | Брать. **Наш тип `size_t` (8 байт), не донорский `u32`** — использовать `std::atomic<size_t>`, иначе схлопнется ручной подсчёт «= 64» в комментарии `SkeletonCustom.h:31`. `try/catch` вокруг `xr_delete` пока оставить. |
| `ref_smem` (`smem_value::dwReference`) | **нет фикса у донора** — их `xrsharedmem.h:10` до сих пор `u32` | `src\xrCore\xrsharedmem.h:18, :94-95` | Писать самим по образцу `xrstring.h` |
| `shared_motions` (`m_dwReference`) | **нет фикса** — их `SkeletonMotions.h:222` до сих пор `u32` | `src\xrCore\Animation\SkeletonMotions.hpp:211, :258-259`; `motions_container::dock` (`SkeletonMotions.cpp:388-407`) вообще без лока, в отличие от нашего же `smem_container` | Писать самим |
| `str_value` | `1379ef8c4` | `src\xrCore\xrstring.h:20` — уже atomic, ordering лучше донорского | Ничего |
| `xr_resource` | `8656cfa90` | `src\xrCore\xr_resource.h:22-23` — уже atomic | Ничего |

**Прямой ответ на твой вопрос про `ref_smem`/`shared_motions`: у донора этого нет и никогда не было.** Проверено чтением их HEAD, не логов. Порт невозможен, это наша работа. `cbeb34f6e` ценен как подтверждение диагноза: они пришли к атомику в `intrusive_base` ровно по той же причине.

Отдельно: threading-верификатор **опроверг** конкретный сценарий отказа, который майнер приписал `intrusive_base` (wallmark'и через воркер пуль). `CBulletManager::UpdateWorkload` вейлмарки не создаёт — только рейпикает и ставит события (`Level_Bullet_Manager.cpp:204-231`), а создание идёт в `CommitEvents` на главном потоке из `CLevel::OnFrame`. Плюс `CWallmarksEngine` всё сериализует одним `Lock`. Так что это гигиена, не доказанный баг — но одна строка.

---

### 2.6-bis. Наша собственная гонка на `CalculateWallmarks` (донорского фикса нет)

`src\Layers\xrRender\SkeletonCustom.cpp:753-777` итерирует per-visual вектор `wallmarks`, копирует `intrusive_ptr<CSkeletonWallmark>` в `add_SkeletonWallmark` и `erase`'ит в ветке `need_remove`. Зовётся из dsgraph на `r__dsgraph_build.cpp:374` (там гард `o.phase == PHASE_NORMAL`) и — **без гарда** — на `:603` внутри `add_Dynamic`, который `build_subspace()` крутит для каждого теневого контекста, порождаемого параллельно на `src\Layers\xrRender_R2\r2_R_lights.cpp:276`.

Единственная сериализация — неатомарный гейт `if (!wallmarks.empty() && (wm_frame != Device.dwFrame))` / `wm_frame = Device.dwFrame` (`SkeletonCustom.cpp:757-759`, член объявлен plain `u32` на `SkeletonCustom.h:139`). Два световых контекста, дошедшие до одного `CKinematics` в одном кадре, оба проходят гейт и параллельно копируют рефкаунты и erase'ят из одного вектора.

Это **строго сильнее** п. 2.6: атомарный рефкаунт починит только половину. Фикс наш: либо atomic CAS на `wm_frame`, либо поднять `CalculateWallmarks` только в `PHASE_NORMAL` — как `:374` уже делает, а `:603` нет.

---

### 2.7. Мелкие null-дерефы, которые игрок ловит регулярно

| Симптом | Донор | Наш файл | Риск |
|---|---|---|---|
| Краш при луте трупа сталкера. `ch_info_get_from_id` возвращает null, оба трейдера идут в `RELATION_REGISTRY()`, а `GetAttitude` (`relation_registry_inline.h:60-63`) сразу делает `from->object_id()` | `01487d11b` | `src\xrGame\ui\UICharacterInfo.cpp:296-299` | нет |
| **Сосед, которого не нашла ни одна область:** `chInfo.Init(T)` и следом `T->m_character_name.c_str()` вообще без гарда — тот же класс, шире поверхность | (тот же) | `src\xrGame\ui\UICharacterInfo.cpp:130-137` | нет |
| Скрипт зовёт `set_switch_online/offline/interactive` со стухшим id → `VERIFY` выпилен в релизе → деref | `3a510ebad` | `src\xrGame\alife_update_manager.cpp:365-384` | нет. Соседний хунк того же коммита у нас уже дословно есть (`alife_online_offline_group.cpp:305-314`) — хороший знак, что семейство переносится |
| Control-команды монстров пишут через `m_man->data(...)` под одним `VERIFY`. `CControl_Manager::data()` (`control_manager.cpp:186-198`) **реально** возвращает 0, когда спрашивающий com не текущий захватчик | `a87e7042d` | `control_critical_wound.cpp:20`, `control_manager.cpp:392`, `control_run_attack.cpp:34,41`, `control_threaten.cpp:21,29,43`, `controller/controller_psy_hit.cpp:84,142,275` | нет |
| То же место: `CBlend* blend = m_man->animation().current_blend(); VERIFY(blend);` и сразу `blend->timeTotal / blend->speed` | `cda4c9783` | `control_run_attack.cpp:102-106` | нет |
| Уничтоженные объекты в danger/memory manager | `d34966c3e` — **брать в текущей форме донора, не в форме 2023** | `danger_manager.cpp:213,227,278`; `memory_manager.cpp:157-171` | см. ниже |
| `make_object_visible_somewhen` с null/destroyed врагом. `ai_stalker_misc.cpp:211` зовёт с `memory().enemy().selected()`, который штатно бывает null | `25e08d81e` | `src\xrGame\memory_manager.cpp:334-353` | нет |
| Скриптовый `obj:set_const_force(...)` на не-shell-holder'е: `object().cast_physics_shell_holder()->PPhysicsShell()` дерефает null **до** собственных гардов на `:214`/`:219` | `d31289364` | `src\xrGame\script_game_object_use.cpp:211` | нет |
| `m_throw_ignore_object` не чистится в `net_Relcase` — сталкер начал бросать гранату, цель умерла, следующий `check_throw_trajectory()` рейпикает по освобождённому объекту | `1d926eed8` | `src\xrGame\ai\stalker\ai_stalker.cpp:1136-1162`; указатель пишется в `ai_stalker_fire.cpp:1023`, читается через кадры на `:1059` | нет. Сигнатура у нас `net_Relcase(IGameObject* O)`, и чистить надо **до** `if (!g_Alive()) return;` |
| `CCF_Skeleton::_RayQuery` без проверки `owner`/`getDestroy` | `4012087ed` | `src\xrEngine\xr_collide_form.cpp:242-253` | нет. Гард ставить **выше** нашей проверки `PKinematics(owner->Visual())`, которая сама уже дерефает owner. У нас возврат `bool`, не `BOOL` |

**Про `d34966c3e` важно.** Рекомендация майнера в изначальном виде **вносит новый null-деref**: `object.m_object` легитимно бывает null для `CSoundObject` (`sound_memory_manager.cpp:222` это явно обрабатывает, и наш `danger_manager.cpp` уже проверяет `if (object.m_object)` в ветке `SOUND_TYPE_INJURING`). Донор на это тоже налетел и позже поправил — их текущий HEAD: `if (!object.m_object || !object.m_enabled || object.m_object->getDestroy())`. Брать эту форму, и такую же null-проверку добавить в цикл `memory_manager`.
**Хунк `item_manager.cpp` того же коммита — не брать ни в каком виде**, см. раздел 5.

---

### 2.8. Ещё две тихие порчи

**`IReader::open_chunk` с неинициализированным `dest`.** `src\xrCore\FS.cpp:312-315` и `:361-364`: `u8* dest;` без инициализации, скармливается `_decompressLZ(&dest, &dest_sz, ...)`, bool-результат выбрасывается, и `dest` уходит в `xr_new<CTempReader>(dest, dest_sz, ...)`. `_decompressLZ` при `if (!Decode(total_size)) return false;` (`LzHuf.cpp:695-696`) выходит **до** записи `*dest` и `*dest_sz`. Битый сжатый чанк из повреждённого архива или мода → ридер поверх мусорного указателя → чтение и потом `xr_free` по дикому адресу. `FS.cpp:248` в том же файле уже делает `u8* dest = 0;` — то есть это несогласованность, не решение. Донор `935b6448b` только зануляет; у нас сигнатура возвращает bool (`src\xrCore\lzhuf.h:8`) — проверять его и валить открытие чанка. Остальное из `935b6448b` (расCRTP-ивание `IReaderBase` в виртуальный интерфейс) — категорически нет, см. раздел 5.

**`ISpatial_DB::insert` без проверки `m_root`.** `src\xrCDB\ISpatial.cpp:310-323` — ветка `verify_sp` зовёт `_insert(m_root, ...)`, `else`-ветка `m_root->_insert(S)`, обе без гарда. `m_root` — null до `initialize()` и после `destroy()`.
**Брать `f16870228`, не `efcda373a`.** `efcda373a` сам по себе сломан: `if (m_root != nullptr && verify_sp(...))` просто уводит объект с null-`m_root` в else-ветку, которая всё равно делает `m_root->_insert(S)`. Полный фикс — в `f16870228`, он оборачивает тело else.

---

### 2.9. `g_bRendering` — plain `bool`, читается с воркера

`src\xrEngine\device.cpp:23` `ENGINE_API bool g_bRendering = false;`, пишется на `:76` (`RenderBegin`) и `:112` (`RenderEnd`), читается с воркера в `src\Layers\xrRender\ModelPool.cpp:362`, где решает: ставить визуал в очередь под локом или удалять прямо сейчас. Несинхронизированный читатель — это воркер `processSeqParallel`, который стартует на `device.cpp:299` **до** `RenderBegin`, и доходит до `CModelPool::Delete` через `script_gc` → Lua-финализатор → `~CParticlesObject` → `~CParticleGroup` → `model_Delete`.

**Ловушка:** символ экспортируется по имени — `src\xrEngine\xrEngine.def:1260` `?g_bRendering@@3_NA @1258`. Смена типа меняет декорированное имя, и `.def` надо править **в том же коммите**, иначе игровая DLL не слинкуется.

Донорское разделение очереди удаления на две (`62a164d0e`, `xr_set` для дедупа) — **не брать**: наш copy-out на `ModelPool.cpp:375-388` уже починил реентрантный краш, а `set` меняет порядок удаления, на чём форки X-Ray уже горели.

---

### 2.10. Переполнение пакета сейва — **два** живых писателя, не один

Тут краш- и скрипт-области дали разные куски картины, и ни одна не собрала её целиком.

Инвариант: «на входе итерации занято не больше 8 КБ» при буфере 16 КБ (`src\xrCore\net_utils.h:76` `u8 data[16384]`), а проверка бюджета стоит **после** записи объекта. Единственная защита внутри `NET_Packet::w` — `VERIFY` (`net_utils.h:86-89`), а `VERIFY` в релизе no-op (`xrDebug_macros.h:198`). Любой объект, чей `net_Save` превысит остаток, пишет за буфер.

Живые писатели с этим багом:

| Функция | Файл | Зовётся из |
|---|---|---|
| `CLevel::Objects_net_Save` | `src\xrGame\Level_network.cpp:238-269` (проверка на `:265`) | `alife_update_manager.cpp:189`, `Level_network_messages.cpp:333` |
| `client_save_snapshot_step` | `src\xrGame\alife_storage_manager.cpp:1120-1146` (свой `maximumObjectBytes = 8*1024`) | `alife_storage_manager.cpp:1343` |

Третья копия — `CLevel::ClientSaveStep` (`Level_network.cpp:290-317`) — **мёртвая**: объявлена на `Level.h:339`, вызовов нет. Майнер краш-области ошибочно принял её за живой путь.

**Что делать:** правку класть в **оба** живых писателя одновременно и мёртвую копию удалить в том же изменении, иначе она станет миной для следующего, кто её подключит. Донорский `3e54386bb` (по пакету на объект) — **не брать**, он переписывает поток сообщений `M_SAVE_PACKET` целиком. Наш вариант: перенести проверку остатка **перед** записью и флашить пакет, если объект не влезает; плюс поднять `net_utils.h:86` из `VERIFY` в `R_ASSERT`, чтобы переполнение падало громко, а не тихо.

Достижимо ли это в Dead Air — **не доказано**. Отладочный fatal на 65536 (`Level_network.cpp:256`) — только DEBUG и, судя по всему, ни разу не срабатывал. Сначала померить худший `net_Save` (см. раздел 7).

---

## 3. Производительность

Честно: **у донора для нас перф-выигрышей почти нет.** Их «крупная» работа по load-time (`d09931ec8` дисковый кэш CDB-дерева, `f758a0737`/`3de1ab7a4` параллельная загрузка текстур, `046fca348` растущий quadtree, `4db96cc09` crc32, `18217f5f5` реентрантность `Recurse`) — у нас везде уже есть более сильный аналог. Ни в одном их коммите нет ни одной цифры в миллисекундах; все оценки ниже — чтение кода, не их замеры.

| Что | SHA | Наш файл | Честный размер выигрыша |
|---|---|---|---|
| **`log.cpp` (наш)** | — | `src\xrCore\log.cpp:41-45` | Единственный пункт здесь с реально большим и измеримым эффектом на твоём железе. 40k move-assign'ов на строку под глобальным локом. Тратить время в первую очередь сюда |
| Shared-lock на `ISpatial_DB` | `b9a9ea8dc` → `095755ca5` | `ISpatial.h:224,237`; `ISpatial_q_box.cpp:78`, `q_frustum.cpp:65`, `q_ray.cpp:357` | Померить до. Параллельность реальная (`render_phase_sun.cpp:344-372` гонит каскады через `xr_parallel_for`, `r2_R_lights.cpp:276` шлёт per-light `build_subspace` в TaskScheduler) — но худший случай, который майнер описывал («шесть теневых контекстов дерутся за дерево»), у нас **уже смягчён**: при `ps_r__light_dyn_shared` мы поднимаем один `q_box` (`r2_R_lights.cpp:58`) в `s_light_dynamic_casters` и раздаём его контекстам как `o.dynamic_source` (`r__dsgraph_build.cpp:906-923`), минуя `q_frustum` для теневых источников. Остаются каскады солнца |
| Инлайн `CFrustum::testSphere/testAABB/testSAABB` | `a7b028754` | `src\xrCDB\Frustum.h:83-86`, тела в `Frustum.cpp:84,105,163,185` | **Заявленный механизм неверен.** Обходчики октодерева живут внутри `xrCDB`, в той же DLL, а IPO/LTCG у нас включён для shipping-конфигов (`cmake/XRay.Build.cmake:38-39`) — MSVC `/GL+/LTCG` инлайнит через TU внутри одного link unit'а. Выигрывают только 20 кросс-DLL вызовов, из них per-object-per-frame: `r__dsgraph_build.cpp:530,629,919,1026`, `DetailManager.cpp:317,342,469`, `r2_R_sun_support.h:268,530`. Механически, риска нет, но не жди рекламных цифр |
| `Feel::Vision` не рейтестит собственную модель наблюдателя | `c0a254093` | `src\xrEngine\Feel_Vision.cpp:211` — `RayQuery(..., NULL, NULL)` | Всё готово: `IGameObject const* m_owner` на `Feel_Vision.h:28`, `CObjectSpace::_RayQuery2` параметр уже уважает (`xr_area_raypick.cpp:239-240`). Сейчас cform владельца полноценно рейтестится на каждой трассе, и `feel_vision_callback` (`Feel_Vision.cpp:25-39`) себя не фильтрует, так что материал владельца ещё и множится в `fp->vis`. `Feel::Vision` явно светится в нашем `delayed-load-freeze`-трейсе. **Парити-флаг:** НПС начнут видеть чуть больше. Перед шипом проверить на сталкерах DA |
| `intrusive_base` atomic | `cbeb34f6e` | — | Не перф, но платится тут: одна `lock inc` вместо `inc`. На этих частотах неважно |

**Опровергнуто (майнер предлагал, верификатор снял):**

- `3c68aa6da` **fast_dynamic_cast в luabind** — код-факт верный (`Externals/luabind/luabind/back_reference.hpp:41-44`, `CScriptGameObject` полиморфен), но порт не тот, что описан: у донора **другой форк luabind** (`sdk/include/...`, структура `get_back_reference_aux<T>::extract` против нашей цепочки aux0/aux1/aux2), самого хедера `fast_dynamic_cast` в их `src/` нет (тянется их автозагрузкой), а сам он — кеширующий dcast с одной парой (vtable→offset) на call site: на зоопарке объектов X-Ray промахи могут дать ноль или минус. Донор потом отдельно чинил его (`d97206ab2`). Плюс luabind у нас сабмодуль → `patches/` + `Apply-RequiredPatch`. Сначала профилировать конвертер; если каст правда всплывёт — писать свои две строки MSVC-фастпаса, а не тянуть чужой хедер.
- `a03f3888d`/`02ec7a084`/… **замена smart_cast на `cast_*` виртуали** — предпосылка ложная. Наш `smart_cast` — не fallback на Loki-RTTI, а таблица диспатча: `src\xrServerEntities\smart_cast.h` несёт ~65 `DECLARE_SPECIALIZATION`, включая ровно те «горячие» касты — `cast_hud_item` (:151), `cast_stalker` (:187), `cast_actor` (:119), `cast_weapon_ammo` (:163), `dcast_GameObject` (:91). То есть они уже одиночный vtable-lookup. Плюс майнер ошибся в инвентаре: `cast_restrictor` есть (`xr_object.h:261`), как и `cast_attachable_item`/`cast_holder_custom`/`cast_base_monster`/`cast_shell_launcher` (:262-265). Не делать.
- `13c7d4e70` **`psSoundCacheSizeMB` 32→256** — переменная у нас **мёртвая**. Пять вхождений во всём дереве, ни одно ничего не размеряет: extern, определение, `.def`, регистрация `CMD4` и телеметрия (`console_commands.cpp:209`). Стриминговый кэш страниц наша ветка OpenXRay выпилила; источники декодируются напрямую в `CSoundRender_Source::decompress`. Поднятие дефолта меняет ровно одну строку телеметрии.
- `ba1b3e8c0` (space restriction O(n²)), `833605ec1` (`parse_anim_params`) — обе на холодных путях. Вторая — вообще чистая загрузка конфигов (`control_animation_base.cpp:750-782`, раз на класс монстра), а `strtof`-цепочка меняет семантику парсинга на конфигах DA. Не стоит.
- `f17c308a6` асинхронный логгер — 117 файлов, формат всё равно делается на вызывающем потоке, и ломает порядок лога относительно краша. Не надо.
- `e802dd514` `xr_ini` → `xr_string_map` — большой рефактор хранилища конфигов ради log-n→1, с парити-поверхностью. Нет.

---

## 4. Дешёвые улучшения качества жизни

| Что | SHA | Наш файл | Заметка |
|---|---|---|---|
| **`GAMEMTL_SUBITEM_COUNT = 4`** — `R_ASSERT` (не `VERIFY`, срабатывает в релизе) на любом материале с >6 звуками шагов или >4 партиклами/вэйлмарками | `00f1c9280` | `src\xrMaterialSystem\GameMtlLib.h:44`; асерты `GameMtlLib_Engine.cpp:17,27,36` | Хранилище — `xr_vector`, за константой нет фиксированного массива, это чисто искусственный потолок. У донора 24 + отдельная `GAMEMTL_STEPSOUND_SUBITEM_COUNT`. Поднять — значит перестать ронять процесс на контенте, а не «включить фичу» |
| **Торговля: продажа торговцу с бесконечными деньгами молча не проходит** | `f65b4a2fe` | `src\xrGame\ui\UIActorMenuTrade.cpp:462-499` | `OnBtnPerformTradeSell` проверяет `partner_money >= 0` без escape'а `InfinitiveMoney()`, тогда как **путь покупки такой escape уже имеет** (`:375`). `set_money` для inf-money делает `m_money = _max(m_money, amount)` (`InventoryOwner.cpp:560-566`), баланс не растёт → продажа предмета дороже стака торговца падает в «не хватает денег». Одно условие, поверхности сейва нет |
| **Отрицательный радиус света дульной вспышки** | `3b2868710` (концепт) | `src\xrGame\ShootingObject.cpp:176, :214` | `light_build_range = Random.randFs(light_var_range, light_base_range)`, а наш `randFs(range, offs)` = `offs + randF(-range, range)` (`src\xrCore\_random.h:57`). Любой конфиг с `light_var_range > light_base_range` даёт **отрицательный** радиус прямо в `set_range()`. Донор назвал свой коммит про фриз — непозитивный радиус в spatial-дереве это правдоподобный механизм. **Их `std::min/std::max` не копировать** — они читают `randFs` как `(min,max)`. Клампить. Вторая половина их фикса (инициализация времени жизни) у нас уже закрыта через `m_bLightShotEnabled` (`ShootingObject.cpp:49`) |
| **`#include` с маской из подкаталога молча грузит ноль файлов** | `1d6da0096` | `src\xrCore\Xr_ini.cpp:598-603` | Важная половина — **не** хардкод `*.ltx`, а то, что маска в `FS.file_list` идёт сырым `inc_name` вместе с префиксом каталога, а наше сопоставление — только по имени файла (`LocatorAPI.cpp:1439`). То есть `#include "subdir\*.ltx"` **сегодня** тихо не грузит ничего. Фикс — `_splitpath` донора. Прямо касается генерируемых XMS-фрагментов. Можно взять только `_splitpath`-половину и оставить гард `*.ltx` — так строго безопаснее |
| **Логировать активную permutation при падении компиляции шейдера** | `1badb86ff` | `src\Layers\xrRenderPC_R4\r4_shaders.cpp:602-658`; `ShaderResourceTraits.h:707-716` | Сейчас мы пишем имя файла и блоб ошибки, но не дефайны. `shader_options_holder`/`m_ShaderOptions` у нас уже есть (`r4_shaders.cpp:54+`), `-shader_warnings_as_errors` тоже (`:608`). Дамп — в лог, не в мессадж-бокс |
| **ASan на MSVC у нас не работает вообще** | `IXRAY_ASAN` в их `CMakeLists.txt:12,49-52` | `CMakeLists.txt:35` объявляет `XRAY_USE_ASAN`, но потребитель один — `cmake/XRay.Compiler.GNULike.cmake:88`. В `XRay.Compiler.MSVC.cmake` санитайзера нет | Ставишь `-DXRAY_USE_ASAN=ON`, CMake молча соглашается, сборка проходит, инструментирования нет. **Четыре предусловия:** `MEMORY_ALLOCATOR=standard` (дефолт mimalloc, ASan сквозь него не видит), IPO/LTCG выключить (`/GL` несовместим), `/INCREMENTAL` выключить, и **снять `/RTC1` с Debug** (`build/ninja-x64/CMakeCache.txt:104`, `/fsanitize=address` с `/RTC` несовместим). Это ровно тот инструмент, который поймал бы `ref_smem`/`shared_motions` |
| **Уровень предупреждений** | — | — | Мы фактически на **`/W1`**, а не на дефолтном `/W3`: `CMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc` (`CMakeCache.txt:101`), CMP0092 при `cmake_minimum_required(3.23)` по умолчанию NEW. То есть `C4700` уже сыплется и просто не читается, а `C4701/C4703/C4789` (level 4) вообще не эмитятся. Сначала поднять уровень на **своих** таргетах, потом уже думать про `/we`. `/WX` — только в CI-джобе, не в дефолтных конфигах |
| Non-unity CI-джоба | их `nonunity-build.yml` | `src/xrGame/CMakeLists.txt:2554` — `UNITY_BUILD_BATCH_SIZE 50`; `.github/workflows/build.yml` — одна джоба, все конфиги unity | Забытый `#include` компилится, потому что сосед по батчу его затянул. Сломается при любом переупорядочивании файлов. Стоит только машинного времени CI |
| Отладка `PPInfo` | `e1f3ca994` | `src\xrCore\PostProcess\PPInfo.hpp:9-66` | `float r,g,b` / `blur,gray` / `intensity,grain,fps` без инициализации. Спасает только то, что `CCameraManager` в конструкторе делает `pp_affected = pp_identity` (`CameraManager.cpp:58`) до `validate()` на `:290`. Две строки `= 0` |
| Наши собственные тикеты (не порты) | — | `src\xrUICore\EditBox\UICustomEdit.cpp:250-255` — `string256 passText` на стеке заполняется `m_text.size()` звёздочками без проверки границы. `UILines.cpp:311-333` — то же, но буфер `static`, портится статика, а не стек | У донора эти же баги не починены, брать нечего — просто починить |

**Про crash-дампы отдельно.** Донорский `cade2b97b` (`MiniDumpWithDataSegs | MiniDumpWithFullMemoryInfo`) **не брать**: `MiniDumpWithDataSegs` прямо противоречит нашему `src\xrCore\Debug\CrashReport.cpp`, который намеренно вырезает стеки потоков и сегменты данных и потом в JSON заявляет `contains_data_segments:false`. Приватно-нейтральна только половина `MiniDumpWithFullMemoryInfo` (метаданные VA-регионов без содержимого — она говорит, был ли падающий указатель освобождён/размаплен/guard-page). Это локальное решение, а не порт.

**Про Lua-трейсбеки.** Скриптовый майнер записал их в «absent» — **верификатор это снял**: `script_log` у нас сам зовёт `print_stack`, а вторая половина (luabind) всё равно живёт под `#ifndef LUABIND_NO_ERROR_CHECKING`, а `LUABIND_NO_ERROR_CHECKING` мы определяем для ReleaseMasterGold (`cmake/XRay.Build.cmake:29-30`) — то есть для шипящейся конфигурации это ничего не даёт. Единственный остаток без обеих оговорок — двухстрочный `print_stack()` в `lua_cast_failed` (`src\xrScriptEngine\script_engine.cpp:713`).

---

## 5. Осторожно / не брать

| Коммит | Почему нельзя |
|---|---|
| `54fdfaa8f` «Replace critical section with SRW lock» (`shared_string.cpp`) | **Самая опасная мина в списке.** Разносит `find()` (shared) и `insert()` (exclusive) в `dock()`, ничего не удерживая между ними — классический TOCTOU на пуле строк. Это ровно тот баг, что дал нам иероглифы. Наш `xrstring.cpp:210-253` держит один exclusive `Lock` через весь find-then-insert, и комментарий на `:247-248` объясняет почему (`clean()` берёт тот же лок, узел с нулевым рефкаунтом не должен вымететься между lookup и оживлением). Выглядит как «снять контеншн со строк на загрузке» — и потому будет портировано не глядя |
| `a72376c3d`, половина `policy.hpp` | Делает так, что `nil` **матчится** с bool/int/unsigned/char/... и конвертится в 0/false. Молча меняет разрешение перегрузок для каждого экспорта движка сразу, без единого сигнала на компиляции. Легаси-скрипт DA, который сейчас получает громкий «No matching overload», начнёт тихо звать не ту перегрузку с 0 — и результат уедет в сейв. Ровно класс изменения, который убил нас в 1.3.2. (Половина `function.cpp` того же коммита безобидна.) У нас это к тому же неприменимо физически: в дебустифицированном luabind нет ни `PRIMITIVE_CONVERTER`, ни `enum_converter` |
| `a11378b75`, половина `lua_game_object()` | `return NULL` для уничтоженного объекта вместо ассерта. Пихает `nil` в функцию, от которой каждый легаси-колбэк DA (`bind_stalker`, `xr_logic`, таск/репутационные хендлеры) ждёт валидный объект и сразу делает `obj:...()`. Переносит краш в сотню неотсмотренных Lua-мест. Диагностическая половина (`ID()` вместо `%x`) — нормально |
| `3e54386bb` «Fix corrupted saves» | Переписывает `ClientSave` на пакет-на-объект, меняя поток `M_SAVE_PACKET` целиком, и выкидывает DEBUG-проверку размера. Наш вариант — проверка бюджета **до** записи, см. 2.10 |
| `935b6448b`, основная часть | Расшивает CRTP `IReaderBase` в виртуальный интерфейс. Каждый `r_u32/r_float/r()` на каждом чанке `.ogf/.dm/.cform`/спавна станет виртуальным вызовом без инлайна — это самый горячий путь загрузки уровня, то есть прямо в нашу больную точку. У нас `FS.h:208-219` шаблон с `impl()` и явной инстанциацией на `:447`. Заголовок коммита «Fix» и файл `FS.h` — приманка |
| `4c8636354` «Delete allocator in ISpatial_DB» | Убирает `poolSS<ISpatial_NODE,128>` и делает `_node_create` = `new`, `_node_destroy` = линейный `std::find` + `erase`. Узлы создаются/удаляются на каждом пересечении ячейки октодерева, то есть постоянно. Наш пул на `ISpatial.h:227-229` оставить. Коммит трогает те же два файла, что и полезный `f16870228`, — легко смести заодно |
| `d34966c3e`, хунк `item_manager.cpp` | Донор написал `if (gameObject == nullptr && gameObject->UsedAI_Locations()) return false;` — мёртвое условие, которое молча удаляет исходную проверку `UsedAI_Locations`, — и `if (!&object->ai_location())`, что UB. Это регрессия, а не фикс |
| `65081999f` | Пишет `r.x1`/`r.x2` там, где окружающий код использует `r_` — либо не собирается, либо пишет не в тот прямоугольник |
| `2c2c16170` `r2_ls_squality` 1.0→6.0 | Ретюн бюджета smap под видом фикса «полосатых теней». Ложится ровно на наш растяжной провод: `r__light_shadow_budget` должен стоять строго в parity-дефолте, 1.3.2 с ненулевым сломал тени ламп и фонарей НПС. 6× множит адаптивные размеры smap и fill-стоимость. Если полосатость реальна — это в систему пресетов качества, а не в глобальный дефолт |
| `d4c74b6a8` `MaxCBuffers` 14→32 | 14 — это лимит D3D11 на стадию (`D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT`). У нас значение уходит прямо в `PSSetConstantBuffers`/`VSSetConstantBuffers` (`dx11R_Backend_Runtime.h:877,886,895`) и завязано `static_assert`'ом на `:777`. Подъём делает вызовы невалидными |
| `3dc635f09` «Fix 127 bones support» | Снимает `*3` только для 1W-пути в `CSkeletonX_ext::_Load_hw`, оставляя 2W со скейлом — значит их HLSL эти пути различает. Без порта их скиннинг-шейдеров все скиннутые модели молча ломаются. Наш `FSkinned.cpp:83,91` |
| `30832c945` (particle strict-B2F) | Если аддитивные эффекты рисовались дважды всё это время, «фикс» вдвое срежет видимую яркость аномалий, огня и дульных вспышек. Внешний вид DA авторили под текущее поведение. Только через A/B-скриншоты, и готовься отклонить |
| `cd5474cd7` (`LL_SetBoneVisible` + позиция родителя) | Механизм настоящий (наш `CLBone` на `SkeletonRigid.cpp:232` не пересчитывает скрытые кости, стухшая матрица правда живёт). Но это импорт донора из «Awakening — 2055» для совместимости с тем модом, не отчёт о дефекте, а трансформы скрытых костей кормят путь пиков на `SkeletonCustom.cpp:675,705` — то есть регистрация попаданий по моделям со скрытыми костями поедет |
| `1e9b8c57d` (рекурсивное скрытие HUD-кости) | Дети скрытой кости и так считаются от `scale(0,0,0)`-родителя и схлопываются в точку, так что визуальная дельта мала — а парити-риск покрывает иерархию костей каждой пушки DA |
| `c4beb4b94` + `cd78094dc` (collision_disable) | Анализ верный, но **брать 13 без 14 — регрессия проваливания сквозь мир**, причём не только для кровососа: `CPHMovementControl::CollisionEnable` зовётся и из `ActorCameraCollision.cpp:241,270,286,291` на самом актёре. Пара целиком — это изменение ощущения кровососа в моде, где его поведение игроки знают наизусть |
| `6696d4fce` (`CanPutInSlot` на трупе) | Игровое правило, не дефект. Скрипты DA складывают вещи в трупы |
| `4f8e4e407` (удаление `R_ASSERT` в `doors_door`) | Донор просто сносит асерт, оставляя `m_closed_vector`/`m_open_vector` неинициализированными, и через три строки они уходят в `transform_dir` и умножение на 1.1 (`doors_door.cpp:35-42`) — мусор или NaN в стейт-машину двери. Хуже, чем чистый асерт |
| `3b6619b0b` (валидация dest-вершины) | Их запасной вариант помечен в самом диффе как `###DITRYHACK###` и хардкодит вершину 1. Если делать — то формой `cbaa2dc8e` («просто не ставить target point»), и своими руками |
| `e6bcba6c0` (пустой список типов вершин) | `IGameGraph::Search` из их `src/xrEngine/AI/game_graph.cpp` у нас **не существует**. Плюс у нас окно отказа уже уже: `path_manager_game_vertex_inline.h:33-34` делает `if (!m_start_is_accessible) return true;`. Донор сам пометил как экспериментальное, а меняет оно оффлайн-движение отрядов по всей карте |
| `a5c9ca7b0` «Fix sprint stutter after jump», `3439103a16` «Fix stuttering after reloading grenade launcher» | Заголовок про стоны, дифф — геймплей: первый удаляет `f_CoefReturnSpeed`/`s_fDecreaseSpeed`/штраф на ускорение ходьбы после прыжка из `CActor::g_cl_CheckControls`; второй — правка состояния `bMisfire`. Ровно класс мислейбла, который этот обзор должен ловить |
| `fb964a764` (бустеры «сильнее побеждает»), `f9db9d66c` (пси-урон без ПДА), `d61c6120b` (mcFall у псевдогиганта) | Тюнинг баланса, не фиксы |
| `b808ecb87`, `97c8f2f63`, `acab756fb`, `193c8b184` | Переписывают математику затухания в `CSoundRender_Emitter::update_culling`/`priority()` (первый меняет кривую min_distance/rolloff на линейный рамп из LostAlphaRus). Слышимое парити-изменение |
| `e429c1302` + EFX-серия | Переписывание бэкенда реверба, EFX EAXREVERB и legacy EAX не бит-в-бит. И сеьёзность у нас переоценена: наш `soft_oal.dll` — openal-soft 1.25.1 с EAXSet/EAXGet/EAX2.0/EAX5.0, эмуляция скомпилирована и включена, реверб сейчас работает. Плюс `5df9ca8ae` закомментировал половину чтений полей `.env` |
| `cade2b97b` (`MiniDumpWithDataSegs`) | Пишет глобалы процесса в дамп — ровно та PII, которую наш санитайзер вырезает. Сделало бы краш-репорты неанонимными |
| `3262784e9` (`ClearAll` + `m_pData`), `54a2b92a2` (`WeaponStatMgun` касты), `61c6fce30` (`m_agent_manager`), `087328540` (в части OOB), `d2a7fdc30`, `26491cc3e`, `1e9c0884c` | Плацебо или no-op в обоих деревьях, проверено верификаторами. `54a2b92a2`, например, не делает ничего даже у донора: `CPhysicsShellHolder::cast_physics_shell_holder()` и `CGameObject::cast_game_object()` уже возвращают `this` в обоих деревьях |
| Хунк `BuildState` из `8f8dda20c` | См. 2.5 — сдвинет все костные хит-сферы. Тот же SHA, что и полезный `_RayQuery` |
| `43faad816`/`29210a98e` (локи в `CUIWindow`) | Наш UI однопоточный: на `seqFrameMT` только `g_sound_renderer` (`Engine.cpp:76`) и `g_pNetProcessor` (`Level_network_start_client.cpp:156`). Мьютекс в каждый виджет ради невозможной у нас гонки. К тому же донор потом чинил собственный локинг (`96e4f0aa1`) |
| `ddf35a28f` (`CPHJoint` CS) | Наша физика однопоточная (в `src/xrPhysics/` нет ни одного `xr_parallel_for`/`std::thread`). Плюс их гард — per-instance member, он не исключает `dJointDestroy` против добавления в остров другим потоком, для чего нужен world-level лок, и читает `bActive` до взятия лока |

---

## 6. Уже есть у нас

Чтобы не пересматривать. Всё проверено по нашему исходнику, не по предположению.

**Ядро/потоки:** `str_value::dwReference` atomic (`xrstring.h:20`, наш `049c63096` — донор пришёл к тому же через год, `1379ef8c4`); `xr_resource::ref_count` atomic (`xr_resource.h:22`, ≈`8656cfa90`); `thread_local` scratch в `CObjectSpace` (`xr_area.h:28-32`, ≈`873fc1355`); `oldSize`-реентрантность в `Recurse` (`LocatorAPI.cpp:887`, ≈`18217f5f5`); slicing-by-8 crc32 (`crc32.cpp:47`) — **сильнее** донорского `4db96cc09`; растущий `CStorage` в quadtree (`quadtree.h:32-52`) — и у нас правильная пара `xr_alloc`/`xr_free`, у донора `unique_ptr` зовёт `delete` на `xr_alloc`-памяти; FS file-mapping registry за `#ifdef FS_DEBUG` (`FS.cpp:16`); `LoadVfsIndexCache`/`SaveVfsIndexCache` (`LocatorAPI.cpp:82,129`) — у донора аналога нет вообще.

**Загрузка/рендер:** дисковый кэш CDB (`xr_area.cpp:428-470`, `cdb_cache\<level>\objspace.bin` с crc32, учитывающим XMS-хеш склейки коллизий, + `prune_inactive_level_caches`) — строго впереди `d09931ec8`; бюджетированная параллельная загрузка текстур (`ResourceManager.cpp:150-200,547`, 128 МБ на таску / 512 МБ потолок) — у донора голый `par_unseq`, который они дважды выключали (`f04c92f19`, `e9fb78f9e`); эпоха `UCalc_Epoch` делает повторные `CalculateBones` в кадре бесплатными (`SkeletonCustom.cpp:442-446`) — их `b952b9272` нам не нужен; `WaitForCalc()` в `CDetailManager::Unload` и dtor (`DetailManager.cpp:137,250`) — сильнее `0549c36c5`; `occRasterizer::clear` через `ZeroMemory(sizeof)` (`:76-82`, ≈`97da55b66`); `m_fast->dwPrimitives = m_fast->iCount/3` (`FVisual.cpp:84`); инверсия `-no_occq` (`r__occlusion.cpp:12`) плюс наша система generations/abandoned-query/`occq_try_get` — поколение впереди; `MAX_TRIS = 1024*16` (`WallmarksEngine.cpp:35`); `mapHUDSorted` + `HUD_VIEWPORT_NEAR`; вся пачка DX11-гигиены 2023 (`dx11StateUtils.cpp:249-291,503-507`, `dx11SH_Texture.cpp:145-168`, `VertexCache.cpp:6`, `dx113DFluidGrid.cpp:40,140`, `Blender_Recorder_R3.cpp:162`, `dx11Texture.cpp:371`); merge таблицы констант с `table_tmp` + копией `handler` (`r_constants.cpp:71-131`); валидация шейдер-кэша по CRC исходника **и** CRC байткода (`r4_shaders.cpp:564-600`) — сильнее обоих их фиксов; классификация device-removed с `GetDeviceRemovedReason` (`dx11HW.cpp:752-756,783-792`) — у донора её нет вообще; `-dxdebug` с дренажом `ID3D11InfoQueue` и дедупом по ID; `-disasm`; RenderDoc-хук с `SetCaptureFilePathTemplate`/`RefAllResources`/`CaptureCallstacks`; `.natvis` на 44 типа против их 8; Tracy; загрузчик THM, специально закалённый под битые `.thm` Dead Air (`ETextureParams.cpp` — **сильнее** их `d2f4e3310`, чей re-read заголовка как раз полагается на то, что DA ломает).

**Звук/UI/платформа:** requeue при underrun OpenAL (`SoundRender_TargetA.cpp:93-140`); `_valid(pos)` на эмиттере (`SoundRender_Emitter.cpp:15`); `if (_feedback())` на сеттерах `ref_sound` (`Sound.h:390-393`); гарды `!S._handle()` (`SoundRender_Scene.cpp:158,175,204`); `READ_IF_EXISTS` для `hud_sound` (`HudSound.cpp:10-11`); скобки в громкости HUD-звука (`:116`); фолбэк `$no_sound.ogg` (`SoundRender_Source.cpp:291-295`); отсутствие асерта на 44100 Гц; `psSoundTargets = 256`; нефатальная загрузка XML с диагностикой глубины include и длины строки (`XMLDocument.cpp:75-205`); `GetColorFromText` без асертов (`UILines.cpp:412-453`); выход из фуллскрина/минимизация перед краш-диалогом (`Device_mode.cpp:258-280`); `-silent_error_mode` (`xrDebug.cpp:617`); `FormatMessage` без `ALLOCATE_BUFFER` (`:353`); MOUSE_1..MOUSE_8 и раздельные геймпад-бинды (`xr_input.h:23-24`, `xr_input.cpp:179`, `UIEditKeyBind.cpp:132-135`); текстовый ввод через `SDL_TEXTINPUT` (весь класс их багов со scancode-таблицами у нас невозможен); блокировка ввода актёра на загрузочном экране (`ActorInput.cpp:74`); `EXCEPTION_EXECUTE_HANDLER` в `UnhandledFilter` (`xrDebug.cpp:505`); полный dbghelp-стеквокер с `ContextRecord` падающего потока (`Debug/StackTrace.h`, `xrDebug.cpp:441`) — `std::stacktrace` донора этого не умеет в принципе; ImGui-спавнер (`object_factory_spawner.cpp`, с фильтрацией, которой у донора нет).

**Скрипты/сейвы:** полный набор хуков CoC (`CALifeStorageManager_before_save/_save/_load/_after_load`) **плюс** весь бюджетированный async-capture пайплайн (`alife_storage_manager.cpp:1436-1964`, `continue_save`, `SaveExtensionGameplay`), которому у донора аналога нет — `a08765fc2` брать нельзя, он откатил бы нас на синхронный путь; `script_callback_ex.h` с per-subscriber try/catch, XMS-мультикастом и `compare_safe`; `VisualCallback`, `functor<bool>` для `IterateInventory`, `GameTask::CreateMapLocation`, `map_manager::AddRelationLocation`, try/catch в `script_binder`, `string4096` в консольном буфере, гарды inistream — всё независимо от OpenXRay; `alife_switch_manager.cpp:68-69` уже закомментирован (≈`4cb75ffb1`); `CanTakeItem`/`patrol_path_params`/`CPlanner::update` (краш-половина)/`Restrictions.cpp`/`UIDragDropListEx::AddSimilar` с гардом Alundaio — всё на месте.

**Геймплей:** `f598ea37f` (комбат-гарды) уже есть целиком с комментариями Alundaio (`stalker_combat_actions.cpp:817,841-843,1220-1222`, `stalker_danger_in_direction_actions.cpp:149-151`); `2292a5752`, `d1cfef201`, `26491cc3e` — тоже; бустеры радиации/пси/химии считаются через аддитивные хелперы правильно (`ActorCondition.cpp:665-667,693-695`), донорский `adad73f18` нам не нужен; `player_hud::OnMovementChanged` уже переписан и null-safe (`player_hud.cpp:999-1020`); SWI освобождаются (`r2_loader.cpp:182-184`); `CDetailManager::Unload` не оставляет висячий `dtFS` — `FS.r_close` берёт указатель **по ссылке** (`LocatorAPI.h:229`) и `xr_delete` его зануляет, так что майнер тут ошибся; unary `operator-` у `_vector3` у нас просто нет (`358f6813b` неприменим); `461e6f223` (баг `insert_item` в двойном контейнере `Sect`) неприменим — у нас один `Data` + `LineIndex`.

---

## 7. Открытые вопросы

| Вопрос | Конкретная проверка |
|---|---|
| Достижимо ли переполнение пакета сейва в Dead Air? | Инструментировать `net_Save` и залогировать максимальный размер на объект за полное прохождение с тяжёлым тайником и НПС с забитым инвентарём. Если максимум сильно ниже 8 КБ — правим ради корректности и снимаем с приоритета; если приближается — это критично и надо чинить оба писателя немедленно |
| Стоит ли трогать shared-lock на `ISpatial_DB`? | Померить долю кадра в `ISpatial_DB::q_frustum` при активных каскадах солнца, **до** какой-либо правки. Отдельно: `Lock cs` объявлен **private** (`ISpatial.h:225`), так что подъём лока в `spatial_move` — это изменение интерфейса, а не «расширение». И таймеры `Stats.Query.Begin()/End()` сейчас стоят **внутри** запертой области, а `CStatTimer` явно не потокобезопасен (`FTimer.h:191-194`) — под shared-локом это гонка на день первый |
| Роутер `OpenAL32.dll` форвардит `alcGetProcAddress` для `alcReopenDeviceSOFT`? | `sdk\binaries\x64\soft_oal.dll` (openal-soft 1.25.1) экспортирует `ALC_SOFT_reopen_device` и `ALC_EXT_disconnect` — проверено. Если роутер форвардит, то смена дефолтного аудиоустройства (наушники воткнули → звук пропал до перезапуска) чинится ~20 строками: опрос `ALC_CONNECTED` на звуковом потоке + `alcReopenDeviceSOFT(pDevice, nullptr, ...)`. **Донорский `076015d3d` не копировать**: он переоткрывает `deviceDesc.name_al` — снимок, взятый в `Enumerate()`, то есть **старую** дефолтную точку, плюс пересоздаёт устройство с WASAPI-колбэка без синхронизации и течёт `ALCdevice` |
| `CalculateWallmarks` (2.6-bis) — гейт или CAS? | Прогнать сцену с несколькими теневыми источниками и моделью с вэйлмарками под ThreadSanitizer/ASan (после раздела 4), либо просто закрыть гардом `PHASE_NORMAL` на `r__dsgraph_build.cpp:603` по образцу `:374` и посмотреть, пропадут ли вэйлмарки в тенях (не должны — они и так только в normal-фазе значимы) |
| Профилируется ли `dynamic_cast` в конвертерах luabind? | Поставить один Tracy-зон вокруг `get_back_reference_aux0` (`Externals/luabind/luabind/back_reference.hpp:41-44`) и снять кадр в бою. Сэмплы `luabind::meta/detail` в нашем трейсе — это атрибуция шаблонного метапрограммирования, не доказательство |
| Есть ли в шипящихся конфигах `#include` с `*`? | `rg '#include.*\*' gamedata/` перед тем как ослаблять гард в `Xr_ini.cpp:600`. Половину с `_splitpath` можно взять и без ослабления гарда |
| Спавнит ли хоть один уровень DA раздвижную дверь через схему doors? | Если в краш-логах когда-нибудь всплывёт `R_ASSERT` на `doors_door.cpp:32` — писать свой early-return. До тех пор не трогать |
| Есть ли в игре модель со >85 костями? | Если нет, `3dc635f09` закрыт навсегда. Если есть — это порт вместе с их скиннинг-шейдерами, то есть не порт |
| Утечка `dtFS` в dtor `CDetailManager` | `DetailManager.cpp:137-141` не закрывает `dtFS`, так что менеджер, уничтоженный без `Unload`, теряет один `IReader`. Не краш, отдельного патча не стоит — вопрос только, происходит ли это вообще |
