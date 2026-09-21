# 3D PDA: план доведения до 1-в-1 с Gunslinger

Хендофф-документ для реализации. Предыдущий документ — `docs/dead-air/pda-3d-port-plan.md` (первичный порт, уже внедрён: cadd50e73 .. 7c7238177).

**Источники, распакованные локально:**

* Геймдата Gunslinger (17275 файлов, из `gunslingermod/mod_data`, ветка patches): `%TEMP%\claude\...\scratchpad\gunsdata\unpacked`
* Исходники механики — Pascal-патчер `gunslingermod/gunslinger_wpnpatch` (UTF-8 копия): `...\scratchpad\u8`
* Ранее выделенный фрагмент: `D:\Games\Dead Air\_analysis\gunslinger-3dpda`

---

# План интеграции: доведение 3D-PDA до паритета с Gunslinger

Документ самодостаточен. Все пути абсолютные либо от корня репозитория `D:/Games/Dead Air/DeadAir-x64`. Ссылки вида `file:line` проверены по исходникам, а не по памяти. Где ревьюеры поправили аудит — следуем ревьюерам; где они спорят — решение и его причина названы прямо в тексте.

---

## 0. Три подтверждённых блокера — прочитать до планирования работ

Аудит содержал утверждения, которые не выдержали проверки. Каждое из них обрушивает целый пласт плана, поэтому они вынесены наверх.

**Б1. Focused-стадия, скорее всего, вообще не зумится.** `CPdaAnimatorItem::OnZoomIn` (`src/xrGame/PdaAnimatorItem.cpp:104-119`) сначала делает `inherited::OnZoomIn()`, потом `ui->FocusHeldDialog(pda, true)`. А `CDialogHolder::FocusHeldDialog` заканчивается блоком (`src/xrGame/UIDialogHolder.cpp:112-120`):

```
CActor* A = smart_cast<CActor*>(Level().CurrentViewEntity());
if (A) { A->IR_OnKeyboardRelease(kWPN_ZOOM); A->IR_OnKeyboardRelease(kWPN_FIRE); }
```

Дальше: `CActor::IR_OnKeyboardRelease` (`ActorInput.cpp:279`) уходит в `inventory().Action(cmd, CMD_STOP)`; `CWeaponMagazined::Action` (`WeaponMagazined.cpp:939`) зовёт `inherited::Action` **до** проверки `IsPending()`; `CWeapon::Action` case `kWPN_ZOOM` (`Weapon.cpp:1393`) проходит `IsZoomEnabled()` (у нас `zoom_enabled = true` в `dead_air_x64_pda3d_items.ltx:18`), `b_toggle_weapon_aim` равен `FALSE` (`Weapon.cpp:140`), значит исполняется ветка `else if (IsZoomed()) OnZoomOut();`. Плюс `CWeaponBinoculars::Action` ремапит `kWPN_FIRE` в `kWPN_ZOOM` (`WeaponBinoculars.cpp:33`) — срабатывает дважды.

Наш `CPdaAnimatorItem::OnZoomOut` в этот момент имеет гард `if (da_pda3d::ui_focused())`, а `set_ui_focused(true)` выполняется **после** `FocusHeldDialog` — значит расфокуса не происходит и баг не виден, но `inherited::OnZoomOut()` уже снял `m_bIsZoomModeNow`. Итог на бумаге: `focused == true`, `IsZoomed() == false`.

Подтверждающий симптом, который стоит поискать в живой игре: `PdaAnimatorItem.cpp:143` это `if (da_pda3d::consume_unzoom_request() && IsZoomed()) OnZoomOut();` — `consume_` вычисляется первым и всегда гасит флаг, так что при `IsZoomed() == false` ESC в focused-стадии молча съедается и девайс не опускается.

От этого зависят: S2 (лерп hud fov по `m_fZoomRotationFactor`, который двигается только при `pActor->IsZoomAimingMode()`), S3 (латч aim start/end мгновенно сыграет `_end` на следующем же idle), M8 (предложение выводить `ui_focused()` из `IsZoomed()`). **Пока Б1 не закрыт, всё, что завязано на `IsZoomed()`, считается непроверенным.**

**Б2. `CWeapon::GetHudFov` не виртуальна.** `src/xrGame/Weapon.h:106` — `float GetHudFov();`, единственный вызов `src/xrGame/ActorCameras.cpp:347` через `CWeapon*`. Вариант «перекрыть на предмете, нулевой радиус поражения» из стадийного плана — мёртвый код, который скомпилируется, задеплоится и ничего не изменит без единого диагностического сообщения. Работает только правка общего пути.

**Б3. `CInventory::m_iPrevActiveSlot` занят.** Утверждение «в дерево ничего не пишет» ложно: пишут `Inventory.cpp:579` (Activate при попадании в заблокированный слот), `Inventory.cpp:1519`, `Inventory.cpp:1524`, `Actor_Network.cpp:692`; читают `Inventory.cpp:1485-1496` (`TryActivatePrevSlot`, вызывается из `SetSlotsBlocked` при каждом снятии блокировки) и `ActorInput.cpp:854`/`:885` (следующее/предыдущее оружие). Захват поля ломает переключение стволов и восстановление после no-weapon зоны; хуже, наш же `Activate(ANIMATION_SLOT)` в заблокированном состоянии затрёт поле значением 13, и «вернуть предыдущий слот» навсегда станет означать «поднять PDA».

Четвёртая правка того же класса, поменьше: **`ANIMATION_SLOT` — это 13, но ключ `slot` в ltx равен 12.** `[anm_base]` (`D:/Games/Dead Air/_analysis/base_configs/configs/misc/items/items_animations.ltx:58`) ставит `slot = 12`, а `CInventoryItem::Load` (`inventory_item.cpp:109-112`) делает `base_slot_id = sl + 1` с комментарием про переход SOC→CoP. Предложение «читать слот из секции» даёт 12 и молча смотрит не туда. Правильный ход — константа `ANIMATION_SLOT` (`src/xrServerEntities/inventory_space.h:23-24`, `== RESERVED_SLOT == 13`), сегодня используемая ровно в одном месте (`CustomDetector.cpp:60`).

И пятая: **параметр `W` в `PlayHUDMotion` в нашем форке не используется** (`HudItem.cpp:392-407`) — `m_bStopAtEndAnimIsRunning` взводится исключительно по `anim_time > 0`. Два пункта аудита давали про него противоположные инструкции, обе неверные. Последствие важнее самой ошибки: флаг истинен на протяжении всего зацикленного idle, поэтому `CHudItem::OnMovementChanged` (`HudItem.cpp:548-556`) уже сейчас практически мёртв и не годится как хук пере-выбора анимации.

---

## 1. Что есть и где

### 1.1 Наша реализация

Отгружено коммитами `cadd50e73 .. 7c7238177`. Работающая схема: UI растеризуется в RT размером с устройство под именем `$user$ui` внутри `CRender::BeforeWorldRender`; скрытый `CPdaAnimatorItem : CWeaponBinoculars` (class `WP_PDA3D`, animation slot 13) спавнится Lua-машиной состояний; шейдер экрана `models_pda.s` + `model_pda_screen.ps` управляется одним `float4 m_affects` и живым sub-rect экрана; две стадии ввода — held (только рендер) и RMB-zoom (фокус).

Ключевые файлы движка:

| Файл | Роль |
|---|---|
| `src/xrGame/da_pda3d.{h,cpp}` | конфиг, capability-гейт, Lua-опрос, свап рук, экранные константы, watchdog |
| `src/xrGame/PdaAnimatorItem.{h,cpp}` | сам предмет: состояния, зум, свап рук, UpdateCL |
| `src/xrGame/ui/UIPdaWnd.{h,cpp}` | окно, RT-проход, `GetScreenRectUV`, перехваты Show/HideDialog |
| `src/xrGame/HUDManager.cpp` | `RenderPdaScreenUI`, троттл RT |
| `src/xrGame/UIDialogHolder.cpp` | `FocusHeldDialog` / `UnfocusHeldDialog` |
| `src/xrGame/player_hud.cpp` | загрузка `anm_*`, `anim_play`, камер-эффектор по имени мотиона |
| `src/Layers/xrRender_R2/r2_rendertarget.cpp` | создание `rt_ui` |

Данные — оверлей `packaging/dead-air-x64/compatibility/gamedata/`, конфиги `configs/dead_air_x64_pda3d.ltx` и `configs/dead_air_x64_pda3d_items.ltx`, скрипт `scripts/dead_air_x64_pda3d.script`.

### 1.2 Оригинал

Механики Gunslinger живут в паскалевском бинарном патчере; исходники (UTF-8) в `C:/Users/admin/AppData/Local/Temp/claude/D--Games-Dead-Air-DeadAir-x64/3387d2f3-57b3-4032-9a25-c536d815e79a/scratchpad/u8`: `ActorUtils.pas`, `WeaponAnims.pas`, `Misc.pas`, `UIUtils.pas`, `WeaponEvents.pas`, `gunsl_config.pas`, `r_constants.pas`. Их gamedata распакована в `.../scratchpad/gunsdata/unpacked` (17 275 файлов). Источник истины по числам — `configs/action_animators/pda_show.ltx` (303 строки).

### 1.3 Что мы делаем лучше — не «чинить обратно»

1. Камер-эффектор по имени мотиона: `player_hud.cpp:519-538` строит `camera_effects\weapon\<motion>.anm`, снимает старый `eCEWeaponAction` и ставит новый. У них — ручной асм к камер-менеджеру плюс явный обход утечки (`ActorUtils.pas:2139-2145`).
2. Алиасинг мотионов с рандомными вариантами `name`, `name1..name8` (`player_hud.cpp:110-127`) — у оригинала аналога нет.
3. Разделение рук и предмета с безопасным откатом на `idle` (`player_hud.cpp:481-493`) при том же comma-формате, что у них.
4. Сам фид экрана: мы рендерим настоящий диалог в свой RT и маппим через `pda_screen_rect`, поэтому `screen_kx` и вся возня с пересчётом map-спотов (`UIUtils.pas:1509-1590`) нам не нужны и порт их удвоит масштаб.
5. Fail-closed проверка возможностей (`da_pda3d.cpp:83-94`): нет секции/hud/item_visual — фича молча выключается, 2D остаётся целым. Они логируют и едут дальше (`ActorUtils.pas:828-831`).
6. Порядок свапа рук в `OnStateSwitch` (`PdaAnimatorItem.cpp:53-83`): руки внутрь до резолва MotionID, наружу — только после `detach_item` в `eHidden`. Два реальных бага записаны там же в комментариях.

---

## 2. Инвентарь данных и рост проекта

### 2.1 Категории

**(a) Уже отгружено байт-в-байт — 37 файлов, 18 286 133 B** (все сверены по SHA-256): меши `dev_pda_hud.ogf` 288 888 + `dev_pda_hud_animation.omf` 1 846; звуки `pda_draw.ogg` 29 418, `pda_vibros.ogg` 126 410, `detector_draw.ogg` 18 425; текстуры `item_kpk*` 13 981 776; `act_arm_3*` 1 398 404; UI-набор экрана 22 файла 2 485 266.

**(b) Отгружено с намеренными правками — 2 шейдера.** `shaders/r3/models_pda.s`: 543 B у них → 1 489 B у нас (убран `: distort (true)`, добавлен `dx10sampler("smp_rtlinear")`). `shaders/r3/model_pda_screen.ps`: 1 747 → 3 368 B. Их пороги для справки при погоне за паритетом помех: 0.09 / 0.15 / 0.27 / 0.38 / 0.41, шум процедурный `frac(sin(dot(co, float2(12.9898,78.233)))*43758.5453)*0.5`, загрузочный экран гейтится на `m_affects.a > 0 && m_affects.x >= 0.08`. **Откатывать к оригиналам нельзя** — дельта и есть документированная DX11-адаптация.

**(c) Нужно добавить.** `anims/camera_effects/weapon/pda_draw.anm` 6 395 B — обязательно; `pda_headflash.anm` 2 531 B — только если берём headlamp/NV. Каталога `anims/` в оверлее ещё не было; скрипт упаковки копирует дерево целиком (`Get-ChildItem $compatibilityGameRoot | Copy-Item -Recurse`), правка не нужна, но одну намеренную сборку с проверкой round-trip через `converter.exe` сделать обязательно — `.anm` этим путём ещё не ходили. Проверено, что в базовых базах (`xtra.xdb0`, `configs.xdb0`, `xtra_dar2.xdb0/1`) `pda_draw.anm` отсутствует, т.е. это новый контент, а не оверрайд.

**(c-доп, не учтено аудитом.** Четыре звука для headlamp/NV: `pda_show.ltx:23-26` требует `weapons\headflash_on` (62 151 B), `headflash_off` (60 994), `nv_act` (58 339), `nv_deact` (55 831) — суммарно 237 315 B. Есть ли они в DA — **не проверено и методом аудита не проверяемо**: таблицы путей `sounds.xdb` не текстовые (контрольный grep по `detector_draw` и `generic_pin` даёт 0 при заведомом наличии). Проверять распаковкой `converter.exe` либо `FS.exist` в рантайме. Заголовок аудита «портирование ассетов по сути закончено, не хватает ровно двух файлов» верен только при отказе от headlamp/NV.

**(d) Намеренно не берём — 24 файла, 714 119 B.** `ui_mono_noise` (6 файлов, 656 224 B) — сэмплер `s_noise` есть только в `shaders/r1/models_pda.s:13`, в r2/r3 его нет вообще, у нас зерно процедурное. `pda_vibros111.anm` (11 225 B) — мотиона с таким именем нет нигде, у оригинала камер-эффектора на вибрацию не было; переименовывать в `pda_vibros.anm` «чтобы починить» — значит добавить тряску, которой не было. Шейдеры r1/r2 (3 789 B) — мы DX11-only. Все 11 `configs/action_animators/*.ltx` (40 283 B) и `action_animators.script` (2 598 B) — это модель данных их патчера, наш движок её не парсит: **майним значения, не отгружаем**.

**Висячая ссылка в оригинале:** `dev_pda_hud.ogf` и `scope_texture` ссылаются на `wpn\zero_alpha`, которой нет ни в их дереве, ни в одной базе DA. Это benign dangling reference, резолвящаяся в fallback движка. Не искать, не сочинять замену; `scope_texture` мы и так не используем — зум у нас движковый.

### 2.2 Размеры и упаковка

Текущий оверлей: 166 файлов / 21 228 528 B raw. Текущий `D:/Games/Dead Air/database/xtra_dead_air_x64.xdb0` — 21 251 167 B, то есть архив **больше** входа на 22 639 B (+0.107 %, ~136 B на файл): `converter.exe -pack -xdb` пакует в STORE, сжатия нет. Отсюда правило: packed-дельта = raw-дельта + ~136 B на новый файл. Скрипт дополнительно подкладывает 3 файла из `res\gamedata`, поэтому счётчик файлов в архиве на 3 больше, чем в оверлее.

Доля PDA в оверлее сегодня: 41 файл / 20 068 493 B — 94.5 % всего архива совместимости, из них 13 981 776 B это `item_kpk*`.

Проекции:
- только два `.anm`: raw 21 237 454 B (+0.042 %), xdb0 ~21 260 365 B;
- два `.anm` + обрезка OMF: raw 20 321 939 B, xdb0 ~20 344 850 B — **чистое уменьшение на 906 317 B**.

### 2.3 Обрезка `pda_hands_animation.omf` — обязательна, а не опциональна

Наш `meshes/dynamics/weapons/hud_hands_animation/pda_hands_animation.omf` **не** является выжимкой на 35 мотионов: sha256 `f3bb325ee1ecc62f66...` совпадает с `meshes/gwr/food/hands/hands.omf` (1 198 783 B) байт-в-байт. Разбор через таблицу `OGF_S_SMPARAMS` (0x000F) со страйдом из `src/xrCore/Animation/SkeletonMotions.cpp:217-231` и `CMotionDef::Load` (:458-483) даёт: одна партиция `default` на 42 кости, **58** motion-def, из них 35 `pda_*`. Полезная нагрузка: `pda_*` = 280 017 B, остальные 23 = **915 515 B** (`conserva_eat`, `biotic`, `antirad`, `vodka`, `water`, `hercules`, `drug`, `fire_on_the_hand`, `boar_hit_*`, `burer_snatch_the_weapon`, `energy_drink` и прочее). Заодно: в `hands.omf` 58 мотионов, а не 246, как утверждалось в брифинге.

Аудит ранжировал обрезку как опциональную («ничего не даёт функционально»). **Ревьюер прав, вердикт переворачивается.** Причина — механизм, который никто не аудировал: `dead_air_x64_pda3d.ltx` содержит

```
[player_hud_extra_omf]
pda_hands = dynamics\weapons\hud_hands_animation\pda_hands_animation.omf
```

а потребитель — `src/Layers/xrRender/SkeletonAnimated.cpp:879-889`: пока взведён `g_player_hud_model_loading`, **каждая** запись `loadOMF`-ится на **каждую** модель рук игрока, с одним лишь `Msg` при отсутствии файла. То есть 915 515 B мёртвой нагрузки грузятся на рижок DA, на рижок Revolution II и заново при каждой смене костюма. Это рантайм-память, а не размер архива.

Второе следствие того же механизма — совместимость: если чужой мод рук уже определяет мотион с таким же именем, коллизия разрешается порядком в `m_Motions` молча, без диагностики. И 23 лишних имени — ровно те, которые чаще всего встречаются в анимационных модах.

Риск обрезки реален: нужен редактор OMF, умеющий выкидывать мотионы и переиндексировать сабчанки `OGF_S_MOTIONS` против таблицы параметров, потому что загрузчик делает `MS->find_chunk(m_idx + 1)` (`SkeletonMotions.cpp:260`), а при сбое тихо уходит в bind pose через legacy-путь (:291-317). После пересборки проверить, что все 34 ссылаемых значения резолвятся и `ID_Cycle_Safe` на `wpn_hand_pda3d.ogf` (42 кости, `act\act_arm_3`) связывается. Целевой размер ~283 KB.

### 2.4 Отдельно: `rt_ui` выделяется всегда

`src/Layers/xrRender_R2/r2_rendertarget.cpp:331` создаёт `rt_ui` в безусловном блоке рядом с `rt_Generic_0/1` — ни проверки пресета, ни `da_pda3d::available()`, ни ленивого создания. На 2560×1440 это 14 745 600 B VRAM, пересоздаваемых на каждом device reset, которые платят все игроки на всех пресетах, даже без архива совместимости. Это прямо противоречит контрольному тесту R-D («переименуй ltx — всё должно вести себя как сток») и нашей же политике `graphics-features-policy` (фичи заходят через пресеты): строки `pda3d_by_preset` рядом с `hud_shadow_by_preset` / `smaa_by_preset` (`xrRender_console.cpp:855-915`) нет, рантайм-выключателя нет вообще, только `g_pda3d_dbg`.

---

## 3. Матрица анимаций и алгоритм композиции имени

### 3.1 Форма имени

Имя **собирается**, литералов нет. Грамматика (порядок суффиксов строгий):

```
anm_idle [ _aim ] [ JOYSTICK ] [ _moving ] [ _crouch ] [ _slow ]
```

плюс пять хвостов, обходящих грамматику: `_aim_start[_fastzoom]`, `_fastzoom`, `_aim_end[_hide]`, `_sprint[_start|_end]`.

`JOYSTICK` ∈ `""`, `_click`, `_up`, `_up_right`, `_right`, `_down_right`, `_down`, `_down_left`, `_left`, `_up_left`.

Псевдокод селектора (из `WeaponAnims.pas:100-241` + `:55-83` + `ActorUtils.pas:1944-1959`):

```
name = "anm_idle"
if IsAimNow(item):
    name += "_aim"
    if actAimStarted: ModifierMoving(name)
    else: name += "_start"; if fastzoom: name += "_fastzoom"; actAimStarted = true
elif fastzoom:            name += "_fastzoom"
elif actAimStarted:       name += "_aim_end"; if window_gone or slot_changed: name += "_hide"; actAimStarted = false
elif actSprint:           name += "_sprint"; if !started: name += "_start"; started = true
elif sprintStarted:       name += "_sprint_end"; started = false
else:                     ModifierMoving(name); if crouch: name += "_crouch"; if slow: name += "_slow"
if not hud.has_line(name): log(...); name = "anm_idle"

ModifierMoving(name):
    name += joystick_suffix()          # ПЕРВЫМ, до _moving
    if actor_moving: name += "_moving" # дальше per-key ветка мертва: enable_directions_* не задан
```

Проверка на файле: строка 250 `anm_idle_up_left_moving_crouch_slow` = idle + joystick + _moving + _crouch + _slow; строка 277 `anm_idle_aim_up_left_moving`. Совпадает.

`anm_show` строится отдельно (`WeaponAnims.pas:380-402`): база + `_fastzoom` при взведённом флаге. `anm_bore` (`:404-421`): при `actModNeedBlowoutAnim` подменяется на `anm_blowout`, флаг гасится, звук `sndBore` меняется на `sndBlowout`.

### 3.2 Джойстик: числа

Аккумулятор (`ActorUtils.pas:4035-4049`) наполняется **только** в курсорном режиме; в lookout он остаётся нулём и направление залипает в `Idle`.

Тик (`ActorUtils.pas:2278-2301`) раз в `animation_update_period`:
1. Клик побеждает: изменился `m_dwLastClickTime` окна **и** `use_clicks` — направление `Click`.
2. Мёртвая зона: `abs(x) < 2 && abs(y) < 2` → `Idle`. `PDA_CURSOR_MOVE_TREASURE = 2` сырых счётчика мыши за период.
3. Иначе угол → сектор.
Затем: если направление сменилось и предмет не pending — коммит + запрос пере-выбора. Аккумулятор обнуляется всегда.

Угол — `atan2`, нормированный в `[0, 2π)` (`Misc.pas:898-909`); экранный `dy` растёт **вниз**, поэтому положительный угол = вниз. Секторы (`ActorUtils.pas:1923-1942`), **копировать литералы как есть** — это не ровные 22.5°:

```
[0.393, 1.18)  DownRight     [2.74, 3.53)  Left
[1.18,  1.96)  Down          [3.53, 4.32)  UpLeft
[1.96,  2.74)  DownLeft      [4.32, 5.10)  Up
                             [5.10, 5.89)  UpRight
остальное ([5.89,2π) ∪ [0,0.393))  Right
```

**Ловушка, на которую наступят:** порядок слов в ключе и в имени ассета на диагоналях **перевёрнут**. `anm_idle_up_right` → мотион `pda_idle_right_up`; `anm_idle_down_left` → `pda_idle_left_down`. Ключ — вертикаль-затем-горизонталь, ассет — наоборот. Так во всех четырёх диагоналях обоих гридов.

Сброс при активации (`ActorUtils.pas:2257-2262`): время, `Idle`, текущее время клика, обнулённый аккумулятор.

Часы клика: `CUIWindow::m_dwLastClickTime` пишется значением `Device.dwTimeContinual` (`UIWindow.cpp:210,217`). Сравнение «изменилось ли» от этого не страдает, но любое вычисление возраста клика поедет под time dilation focused-стадии. Часы тика указать явно: оригинал берёт `GetGameTickCount`, т.е. игровое время.

### 3.3 Что где лежит

Таблица блоков `pda_show.ltx` (121 `anm_`-строка, 120 различных ключей, 34 различных мотиона):

| Блок | Строки | Ключей | Что это |
|---|---|---|---|
| A | 130-148 | 8 + 8 марок | headlamp / NV, все четыре сводятся к двум мотионам `pda_headflash` и `pda_aim_headflash` |
| B | 155-161 | 7 | show / hide / fastzoom-цепочка / `anm_hide_emerg` |
| C–J | 164-250 | 79 | восемь одинаковых гридов по 10 строк (пусто / `_slow` / `_crouch` / `_crouch_slow` / `_moving` / `_moving_slow` / `_moving_crouch` / `_moving_crouch_slow`) |
| K | 252-255 | 4 | sprint, sprint_start, bore, blowout |
| L–M | 257-277 | 20 | два aim-грида; **единственные comma-строки** во всей матрице |

В гридах C–J **восемь строк направлений идентичны во всех восьми гридах** — поза джойстика полностью перекрывает походку. Отличаются ровно четыре базовые строки: 164/175/186/197 = `pda_idle`, 208/219 = `pda_walk`, 230/241 = `pda_walk_slow`. То есть `_slow` сам по себе не меняет ничего нигде: `anm_idle_slow` == `anm_idle`, `anm_idle_moving_slow` == `anm_idle_moving`. Гриды `_slow` существуют только чтобы имя резолвилось.

Блок M (aim + moving) целиком дублирует блок L значение в значение (`anm_idle_aim_moving = pda_aim_idle`, не walk-цикл). Отгружать только L.

### 3.4 Проверка OMF

Руки: 35 `pda_*` цикла, все 34 ссылаемых на месте, ноль недостающих. 35-й, `pda_aim_draw`, ссылается только из закомментированной строки 163 — это неразрезанный draw-to-aim, заменённый парой `_1stpart`/`_2ndpart`.

Устройство `dev_pda_hud_animation.omf` (1 846 B): **11 циклов** — `pda_draw`, `pda_hide`, восемь `pda_idle_<dir>` и `idle`. Ни `pda_click`, ни `pda_walk`, ни `pda_aim_*`. Это безопасно: `player_hud.cpp:487-489` делает `ID_Cycle_Safe(item_anm_name)`, а при неудаче `ID_Cycle_Safe("idle")`. Именно поэтому оригинал держит мотион `idle`, на который не ссылается ни один ключ. Кости: `default`, `display`, `conlrol` (опечатка в ассете — **не чинить**, к ней биндится байт-идентичный OMF), `wpn_body`.

Следствие для дизайна: корпус устройства реально шевелится только на draw, hide и восьми наклонах джойстика. Всё прочее — движение рук с жёстко зажатым девайсом.

### 3.5 Насколько фатальны недостающие ключи — точная формулировка

Три пункта аудита пугали `R_ASSERT` по всем `anm_`-ключам. Точная истина: `player_hud_motion_container::load` (`player_hud.cpp:79-131`) ассертит на `R_ASSERT2(!pm.m_animations.empty(), ...)` (:128) **только по `m_base_name`** — первому полю строки, и только после перебора `name`, `name1..name8`. Второе поле (мотион предмета) при загрузке не валидируется вовсе; оно резолвится лениво в `anim_play` через `ID_Cycle_Safe` с откатом на `idle` и `R_ASSERT3` на существование самого `idle`, который у нас есть.

Значит: весь aim-грид в comma-форме `= pda_aim_idle_up, pda_idle_up` безопасен по построению, а офлайн-претфлайт должен проверять **только первые токены** против списка `pda_*` в omf. Это дёшево автоматизируется и должно жить в `tools/package`.

Отдельно: `_16x9` дописывается в `anim_play` только при `m_attach_place_idx == 1` (`player_hud.cpp:458`), и `isHUDAnimationExist` зеркалит это условие (`HudItem.cpp:507-510`). У нас `attach_place_idx = 0`, проба и проигрывание согласованы — не менять ключ, не пересмотрев обе точки.

### 3.6 Что просит наш движок сегодня

Ровно 8 литералов на всём дереве для предмета нашей ветки наследования: `anm_show` (`WeaponMagazined.cpp:1287`), `anm_hide` (:1293), `anm_idle` (`HudItem.cpp:466`), `anm_bore` (:244), `anm_idle_moving` (:538), `anm_idle_moving_crouch` (:537), `anm_idle_sprint` (:542), `anm_idle_aim` (`WeaponMagazined.cpp:1321`). Остальные 112 ключей матрицы недостижимы без нового кода, а 7 `anm_`-ключей в нашем отгруженном конфиге плюс `hud_fov` — инертны.

### 3.7 Баги оригинала — не переносить дословно

1. Дубль ключа: строка 187 задаёт `anm_idle_click_crouch`, строка 198 задаёт его **снова** внутри блока crouch+slow, где должно быть `anm_idle_click_crouch_slow`. Отсюда 121 строка при 120 ключах. Последствие у них: клик вприсядку в медленной ходьбе не резолвится и молча глотается.
2. `anm_idle_sprint_end` селектор запрашивает (`WeaponAnims.pas:169`), а в файле его нет — выход из спринта всегда падает в `anm_idle`.
3. `animation_update_period` объявлен дважды: строка 6 = 100, строка 17 = 30.
4. `use_clicks = false` в их же отгруженном конфиге: весь клик-набор авторски сделан и выключен.
5. `blowout_anim_level = 4.5` — на неограниченном внутреннем счётчике; против нашего канала 0..1 бессмысленно.

**Вопрос про дубли закрыт, снять его из открытых.** Наш `CInifile` при загрузке файла разрешает повтор ключа по правилу «последний побеждает» и не ассертит: `insert_item` (`src/xrCore/xr_ini.cpp:395-415`) при найденном `LineIndex` просто перезаписывает значение и выходит. `R_ASSERT2` на :1120 относится к рантайм-пути записи (под `eOverrideNames`) и при загрузке не достигается. Значит `animation_update_period` = 30 (это же и есть задуманное значение), и дословный порт `pda_show.ltx` на дублях не упадёт. В нашем конфиге всё равно объявить один раз явно, чтобы знание правила не требовалось.

---

## 4. Архитектурное решение: инвертировать в window-led

### 4.1 Честное сравнение

Уточнение к постановке: оригинал **не** без состояния. В `ActorUtils.pas` живут `_was_pda_animator_spawned` (:467), `_is_pda_lookout_mode` (:414), `_pda_cursor_state`, `_need_pda_zoom`, `_last_pda_zoom_state`. Различие тоньше и полезнее: **у них хранимые биты пишутся только авторитетом и читаются только для детекции ребра**. У `_was_pda_animator_spawned` ровно четыре точки присваивания (:2255, :2271, :2321, :2754), и каждая запись `false` безусловно сидит внутри ветки, управляемой `IsPDAWindowVisible`. Никто никогда не спрашивает «поднят ли PDA» у хранимого бита. Рассинхрон лечится за кадр по построению, без кода лечения.

У нас `da_pda3d::presenter` — это **авторитет**. Пишется из четырёх мест и **читается пятью точками для принятия решений**: `UIGameCustom.cpp:234`, `:256`, `UIGameSP.cpp:267`, `UIPdaWnd.cpp:227`, `:239`, `:464`, `:567`. Неверное значение не лечится — оно меняет поведение игры.

Шесть из семи багов, оплаченных четырьмя коммитами этой фичи, — один и тот же баг: расхождение флага предмета с реальным состоянием окна. P открывал 2D пока M поднимал девайс; RMB падал на `R_ASSERT(!pDialog->IsShown())` в `StartMenu` (`UIDialogHolder.cpp:39`); удваивались инфопорции `ui_pda` и руки; M с поднятым девайсом релизил предмет напрямую, `eHidden` не достигался, флаг тёк навсегда; P-затем-M ассертил; `ShowDialog` был тихим no-op на уже показанном окне, потому что база гардит `if (!IsShown())` (`UIDialogWnd.cpp:77`). Седьмой (краш на выходе) существует **только потому**, что предмету вообще было что убирать за собой.

Самая красноречивая улика — orphan watchdog на `da_pda3d.cpp:271-297`: односекундный таймер, принудительно гасящий авторитет, который мы не смогли удержать корректным, с комментарием «какой-то путь, которого мы не предусмотрели». Это не страховка поверх здоровой схемы, это реконсилятор, поставленный не туда, делающий одну ветку из четырёх и вдобавок работающий из-под рендер-флага (см. M2). Window-led схема у нас уже написана — просто написана как запасной вариант для item-led.

Что item-led действительно делает лучше: стадию зума/фокуса. `IsZoomed()` — состояние предмета, предмет владеет анимацией, которая его производит, и никакой бит окна над ним не авторитет. Эта часть остаётся item-led — это ограниченное исключение, а не хедж.

**Вердикт: инвертировать.** `CUIPdaWnd::IsShown()` (`m_bShowMe`) становится единственным авторитетом «идёт ли эпизод PDA», ровно как `IsPDAWindowVisible` у них (`UIUtils.pas:733` читает `m_bShowMe` и больше ничего). Один реконсилятор в `CActor::UpdateCL`, транслитерация `ActorUtils.pas:2246-2322`. Пять точек перехвата → одна. Три авторитета (флаг предмета, Lua-машина, состояние окна) → один. Watchdog перестаёт быть хаком и становится главным циклом. Ориентировочно −250 строк движка и −120 строк Lua против +80 строк реконсилятора.

### 4.2 Шаги миграции

**M1 — режим показа на `CUIPdaWnd`, два override как единственные хуки.** `enum EPresent { ePresentNone, ePresentFullscreen, ePresentHeld }`, `IsHeld()`, `ShowHeld()`, `HideHeld()`. `ShowDialog`/`HideDialog` (`UIPdaWnd.cpp:222-245`) становятся селекторами режима, не читающими глобалов.

Мина, которую M1 закрывает и которую надо сохранить в плане дословно: `CUIDialogWnd::HideDialog` — это `if (GetHolder() && IsShown())` (`UIDialogWnd.cpp:81-85`), а `SetHolder` зовётся только из `FocusHeldDialog` (`UIDialogHolder.cpp:104`). Значит на held-но-нефокусном окне неперекрытый `HideDialog()` — **тихий no-op**, а этим путём ходят все туториалы и все моды. Без override инверсия мертва на старте.

`ShowHeld()` зовёт **только** `AddDialogToRender(this)` — он сам делает `Show(true)` (`UIDialogHolder.cpp:199`), а `CUIPdaWnd::Show` шлёт инфопорцию `ui_pda` (`UIPdaWnd.cpp:255-256`). Свой `Show(true)` удвоит порцию; этот дубль уже чинили в `3fa251d57`.

Три поправки к M1 от ревьюеров, все принимаются:
- `HideHeld()` **обязан** сбрасывать `m_present = ePresentNone`, иначе после каждого скрытия окно висит в `IsHeld() == true && IsShown() == false` — ровно второй рассинхронизуемый бит, который инверсия обещала сделать непредставимым.
- `HideActorMenu()` нельзя терять: обе текущие ветки его зовут.
- Гейт на `available()` недостаточен. `available()` (`da_pda3d.cpp:233-237` → `load_cfg` :83-93) — статическая проверка возможностей, она ничего не знает про мёртвого актёра, транспорт, лестницу и заблокированный слот. Сегодня `ShowPdaMenu` при неудачном подъёме проваливается в 2D-диалог; после наивного M1 в этих ситуациях клавиша PDA не откроет ничего. Нужен рантайм-гейт `can_raise()`: актёр жив, уровень готов, `!inventory().IsSlotBlocked(ANIMATION_SLOT)`.

**M2 — реконсилятор в `CActor::UpdateCL`, не в `HUDManager`.** `da_pda3d::update()` зовётся из `CHUDManager::OnFrame` (`HUDManager.cpp:60`), который выходит на :52 при снятом `HUD_DRAW_RT2`; флаг снимают `bloodsucker_vampire_execute_inline.h:35` и `controller_psy_hit.cpp:224`. Тот же ранний выход пропускает и `pUIGame->OnFrame()` на :63 — во время захвата встаёт весь игровой UI, не только наш watchdog. Реконсилятор, который является единственным гарантом корректности, не может жить из-под рендер-флага. Экранные константы (`da_pda3d.cpp:299-336`) остаются под флагом: экран PDA замирает на последнем кадре во время захвата и psy-hit, и это правильное поведение — не «чинить» его потом.

**M3 — реконсилятор, транслитерация `ActorUtils.pas:2246-2322`.** `held = pda->IsHeld() && pda->IsShown()`, `mine = smart_cast<CPdaAnimatorItem*>(inventory().ItemFromSlot(ANIMATION_SLOT))` — именно `smart_cast`, а не «что-нибудь в слоте»: сегодня `da_pda3d.cpp:263` и `:275` проверяют только `ItemFromSlot(13) != nullptr`, и любой мод, припарковавший там свой аниматор, обманывает watchdog. Ветви:

1. `held && !mine && !spawn_pending` → просить Lua заспавнить; неудача → `pda->HideDialog()`. Это дословно `ActorUtils.pas:2250-2253`, и ветвь самоограничена: закрытие окна делает условие ложным на следующем кадре.
2. `held && pending && !mine` → по таймауту `HideDialog()`, снять pending.
3. `held && mine && активный слот не наш` → `Activate(slot)`. Поглощает Lua-состояния 2 и 3 вместе с ре-подъёмом после ухода в другой слот, потому что ретраит каждый кадр вместо таймаутов.
4. `!held && mine` → активировать предыдущий слот, релизить по достижении `eHidden`. Сюда же принудительный `anm_hide_emerg`, когда текущая анимация оканчивается на `_aim_end`, а состояние ещё не `eHiding`.
5. `!g_Alive()` → немедленный релиз без возни со слотами.
6. **Новая ветвь, которой не было в аудите:** `held && mine && слот заблокирован` → убрать девайс и закрыть окно, **не ретраить**. Без неё ветвь 3 каждый кадр уходит в `Inventory.cpp:576-579`, где `Activate` на заблокированном слоте делает `SetPrevActiveSlot(slot)` — и это ещё и портит поле, о котором Б3.

**M4 заменяется.** Предыдущий слот держим в собственном состоянии `da_pda3d` (u16, чистится тем же `reset()`), не в `m_iPrevActiveSlot`. Причина — Б3. Записать в план явно: поле `m_iPrevActiveSlot` вне игры.

**M5 — снять пять перехватов.** `UIGameCustom.cpp:234-238`, `:244-248`, `:256-260`, `UIGameSP.cpp:267-268` — к апстриму (сохранив `HideActorMenu()` и строки `TimeDilator` на `UIGameSP.cpp:274-275`); `UIPdaWnd.cpp:464` и `:567` — заменить `presenter_active()` на `IsHeld()`, не потеряв на :464 вторую половину условия `m_rt_frame == Device.dwFrame`.

Аргумент аудита «апстримовский тоггл сам уберёт девайс» надо переформулировать честно, иначе в него поверят: апстримовская первая строка `if (PdaMenu->IsShown()) { PdaMenu->HideDialog(); return false; }` после M1 доходит до `HideHeld()` и лишь снимает рендер; девайс убирается кадром позже ветвью 4 реконсилятора. **Поэтому M1+M5 как первый самостоятельный коммит не годится — первым идёт M1+M3.**

**M6 — Lua до двух примитивов.** Из `dead_air_x64_pda3d.script` уходят строки 13-162: шесть состояний, `tick`, оба таймаута, ре-подъём посреди холстера, adopt-хак, 30-секундный дворник, регистрация `actor_on_update`. Остаются `_G.da_pda3d_spawn()`, `_G.da_pda3d_release()`, `get_power()`, `get_interference()` плюс новый сметатель на `on_game_load`. Спавн оставляем в Lua — рецепт `dinamic_hud` проверен, переопределяем модами, а перенос в C++ замутил бы bisect, если инверсия даст регресс. Для протокола: `CLevel::spawn_item` (`Level_network_spawn.cpp:198`) и `CALifeSimulatorBase::release` (`alife_simulator_base.cpp:285`) из C++ достижимы, так что доделать это можно позже без проектной работы.

**M7 — снять владение диалогом с предмета.** Удалить `attach_ui`/`detach_ui` (`PdaAnimatorItem.cpp:11-51`) и `m_ui_attached`; удалить override `net_Destroy` (:85-102) вместе со спецслучаем `Level().bReady`, добавленным в `7c7238177` (поковырять полумёртвое окно карты будет уже некому); в `UpdateCL` (:135-152) оставить только `consume_unzoom_request`. `OnStateSwitch` (:53-83) не трогать, кроме удаления вызова `attach_ui`. `da_pda3d::on_shown()` переехать в `OnStateSwitch(eShowing)`.

**M8 — удалить хранимое состояние, но `focused` оставить.** Уходят `presenter` (:40), `set_presenter_active`/`presenter_active` (:221-227), `toggle()` (:168-179), `deactivate_at` и троттл (:139, :184-188), orphan watchdog (:271-297).

Предложение вывести `ui_focused()` из `mine && mine->IsZoomed()` **отклоняется** по двум причинам. Первая: до закрытия Б1 зум в focused-стадии может быть `false`, и одним махом умрут ветка ESC/P/M в `UIPdaWnd.cpp:566-572`, расфокус в watchdog `da_pda3d.cpp:286-290` и троттл RT в `HUDManager.cpp:240`. Вторая, более принципиальная: даже если Б1 окажется безобидным, выводить факт про input-стек из флага зума оружия — значит склеить ровно те две вещи, которые третий режим ввода специально разъединяет. Оставить хранимым, переименовать в `cursor_mode`/`input_focused`, чтобы имя говорило, что это.

**M9 исправлен.** Не читать `slot` из секции (даст 12), а использовать `ANIMATION_SLOT` из `inventory_space.h:23-24`. Заменить литералы в `da_pda3d.cpp:263` и `:275`.

### 4.3 Намеренное расхождение с оригиналом — не «чинить»

Оригинал держит окно PDA на input-стеке весь эпизод и пропускает клавиши актёра через спецслучаи (`ActorUtils.pas:2554`). У нас held-стадия вне стека вообще, поэтому игрок сохраняет полный боевой контроль с поднятым девайсом. Инверсия этого не меняет. Следствие: наше состояние `IsShown() && !GetHolder()` не имеет аналога у них, и их код **не является референсом ни для чего, связанного с holder**.

---

## 5. Инженерная работа, механика за механикой

Порядок — по отношению «видимая игроку ценность / риск кода».

### P0. Магнификация `hud_fov_factor` → `hud_fov_zoom_factor` (~3.1×)

Главная ощущаемая разница между нашим портом и оригиналом, и это не анимация. Их формула (`ActorUtils.pas:2881-2934`): `hud_fov = hud_fov_factor`, при прицеливании `hud_fov -= (hud_fov - hud_fov_zoom_factor) * af`, где `af` = прогресс зума; затем `SetHudFOV(base * hud_fov)`. `0.9 → 0.29` = сужение в 3.10 раза.

Наш `hud_fov = 0.9` в `dead_air_x64_pda3d_items.ltx:47` **не читается ничем** — `hud_fov` у нас консольная команда (`console_commands.cpp:2532`, привязана к `psHUD_FOV_def`). Удалить строку.

Реализация — **общий путь, не override** (Б2). В `CWeapon::Load` рядом с `Weapon.cpp:520`:

```
m_hud_fov_factor      = READ_IF_EXISTS(pSettings, r_float, hud_sect, "hud_fov_factor", 1.0f);
m_hud_fov_zoom_factor = READ_IF_EXISTS(pSettings, r_float, hud_sect, "hud_fov_zoom_factor", m_hud_fov_factor);
```

и в `return` `CWeapon::GetHudFov` (`Weapon.cpp:2598`) домножить `m_nearwall_last_hud_fov` на `lerp(factor, zoom_factor, af)` — **после** nearwall-сглаживания, чтобы прицельный разгон не сглаживался дважды; это и есть порядок оригинала. При отсутствии обоих ключей `k == 1.0` и вывод бит-в-бит прежний для всех стволов DA — **это и есть критерий приёмки диффа**, а не приятная мелочь.

`af` — `m_zoom_params.m_fZoomRotationFactor` (`Weapon.h:280`, интегрируется в `Weapon.cpp:2378-2383` по `pActor->IsZoomAimingMode()`), полярность прямая. Наш `zoom_rotate_time = 0.25` уже отгружен, длительность верна.

**Второе плечо триггера, которое аудит потерял.** У них лерп гейтится не только на `IsAimNow`, но и на `leftstr(GetActualCurrentAnim(wpn), len('anm_idle_aim')) = 'anm_idle_aim'`. Именно это плечо удерживает магнификацию на кадрах `anm_idle_aim_start`, `_start_fastzoom` и `_aim_end`, то есть ровно там, где зум-флаг и анимация расходятся — на мотионах, которые вводят P1 и P4. Без него FOV дёрнется в неверный момент. Добавить проверку префикса `m_current_motion` (`HudItem.h:81`).

Потребитель — `ActorCameras.cpp:346-350`, дальше `psHUD_FOV` идёт в проекции `r__dsgraph_render.cpp:455`, `ParticleEffect.cpp:695/862`, `r4_rendertarget_phase_hud_shadow.cpp:90` и `HudItem.cpp:575/599`. Изменение в 3.1 раза корректно, **но подвинет самозатенение HUD** — перепроверить PDA против работы по hud-shadow. И не клампить `k` в консольный диапазон 0.1..1.0 — он принадлежит `psHUD_FOV_def`, а не произведению.

Заодно закрыть висящий вопрос по `scope_zoom_factor = 1.05` (`dead_air_x64_pda3d_items.ltx:19`, скопирован из `pda_show.ltx:13`): у них ось мирового FOV (`fov_factor` на секции предмета) для PDA не задана и мертва, а у нас `scope_zoom_factor` — это настоящий множитель мирового зума на `CWeapon`. Либо подтвердить, что 5 % мирового зума — то, что нужно, либо выкинуть ключ.

### P1. `anm_idle_aim_start` / `anm_idle_aim_end`

Оба ключа у нас **уже отгружены** (`dead_air_x64_pda3d_items.ltx:61-62`) и уже резолвятся в OMF, но ни один литерал не встречается в `src/` — они инертны. Игрок сейчас щёлкает между бедренной и прицельной позой, прикрытый только 0.334-секундным блендом.

Механизм у них — **латч ребра**, а не состояние (`WeaponAnims.pas:126-151`): `actAimStarted` взводится на первом выборе при `IsAimNow` и первый же выбор получает `_start`; гасится на первом выборе после падения прицела, и этот выбор получает `_aim_end`. Никаких новых hud-состояний.

У нас: `bool m_aim_started` на `CPdaAnimatorItem`, логика внутри override `PlayAnimIdle`. Рёбра доставляются бесплатно: `CWeaponMagazined::OnZoomIn` и `OnZoomOut` оба зовут `PlayAnimIdle()` при `GetState() == eIdle` (`WeaponMagazined.cpp:1345`, `:1376`). Гейт через `isHUDAnimationExist` (`HudItem.cpp:502`), чтобы обрезанный конфиг деградировал в сегодняшнее поведение вместо ассерта.

Порядковая ловушка: `OnZoomOut()` уже зовётся из `OnStateSwitch(eHiding)`, чтобы снять фокус до холстера. Переход `_end` там играть **нельзя** — оригинал решает ту же коллизию через `anm_idle_aim_end_hide` и `anm_hide_emerg`. Простейшее корректное поведение: если зум-аут пришёл из `eHiding`, переход пропускается, холстер примешивается сам.

**Этот же латч бесплатно открывает** `anm_idle_aim_end_hide` (то же ребро + «окно закрылось») и `anm_idle_aim_start_fastzoom` (то же ребро + флаг fastzoom).

### P2. Вибрация выброса `anm_blowout`

Сильнейший одномотионный выигрыш за минимальный код: механика bore у нас уже работает.

Их проводка: `play_blowout_anim = true` (`pda_show.ltx:31`) + `blowout_anim_level = 4.5` (:19); условие `level <= CurrentElectronicsProblemsCnt()` (`ActorUtils.pas:2004-2010`); при истине — флаг `actModNeedBlowoutAnim` (:2346-2348), затем принудительный `eBore` вне прицела либо сперва зум-аут при прицеле (:2352-2362); `anm_bore_selector` подменяет имя и звук (`WeaponAnims.pas:404-421`); срез обратно в `eIdle` при падении условия (:2340-2344).

У нас: override `PlayAnimBore` на `CPdaAnimatorItem`; флаг взводится в `UpdateCL` по пересечению порога; принудительный `SwitchState(eBore)` **в обход** 20-секундного гейта `Weapon.cpp:1264-1276`, который требует `!IsZoomed()` и неподвижности и иначе выброс не пропустит; при `IsZoomed()` сперва зум-аут (мотиона `pda_aim_vibros` не существует); срез в `eIdle` при падении условия с `StopAllSounds`.

Порог **не копировать**: 4.5 — на неограниченном счётчике. Ключ `blowout_anim_level` в `[pda3d]` в шкале 0..1.

**Спецслучай, который аудит пропустил, а ревьюер нашёл.** `ActorUtils.pas:2306-2313`: при уборке PDA из состояния `eBore` они уходят на низкоуровневый `ActivateActorSlot__CInventory(prev, false)` вместо обычного, с комментарием «ActivateActorSlot не сможет скрыть». Сегодня это для нас недостижимо (bore только после 20 секунд простоя), но становится достижимым ровно в момент, когда P2 принудительно загоняет предмет в `eBore` во время выброса — а игрок в этот момент точно нажмёт клавишу PDA. Проверить, уводит ли наш `CInventory::Activate` предмет из `eBore` в `eHiding`, и при необходимости добавить эквивалент. Проверяется дёшево (загнать в `eBore` из консоли/Lua и нажать клавишу), в поле находится дорого.

Звук `snd_blowout = items\pda\pda_vibros` есть в оригинале (`pda_show.ltx:27`) и ассет у нас отгружен, но **проигрываться он не будет**: `CWeaponMagazined::Load` (`WeaponMagazined.cpp:76-77`) грузит только `snd_draw`/`snd_holster`. Алиас нужно грузить в `CPdaAnimatorItem::Load`.

### P3. `anm_hide_emerg`

Самая дешёвая видимая победа. Источник: `ActorUtils.pas:2314-2319` — когда окно PDA исчезает, а имя текущего мотиона оканчивается на `_aim_end` и предмет ещё не в `eHiding`, играется `anm_hide_emerg` напрямую с mix-in, реальный холстер обрабатывается в `OnAnimationEnd`.

Точка вызова у нас уже существует: `PdaAnimatorItem.cpp:148-151` уже детектит «кто-то принудительно спрятал диалог под нами». Добавить прямо перед `request_deactivate()`. `m_current_motion` уже отслеживается на `CHudItem` (`HudItem.cpp:423`). Мотион — `pda_hide` на обеих моделях, тот же, что у `anm_hide`: нулевой ассетный риск, вся ценность в том, что девайс не замирает посреди опускания, когда скрипт или туториал выдёргивает диалог.

### P4. Fastzoom-подъём

Три ключа, один флаг. `bool m_fastzoom` на предмете, взводится в `eShowing` из сохранённого «был ли девайс у лица».

- override `PlayAnimShow`: `anm_show_fastzoom` вместо `anm_show`;
- авто-зум: зеркало `ActorUtils.pas:2274-2277` — в `UpdateCL`, как только `m_fastzoom && GetState()==eIdle && !IsPending()`, вызвать зум и снять флаг; следующий idle даст `anm_idle_aim_start_fastzoom`;
- `anm_idle_fastzoom` — кадр удержания между ними;
- персистентность: у них `_last_pda_zoom_state` каждый кадр = `_need_pda_zoom or IsAimNow(active)` (`:2273`) под опцией сохранения. У нас — сессионная статика в `da_pda3d`, пишется в `OnZoomIn`/`OnZoomOut`, читается в `eShowing`, с тумблером в `[pda3d]`.

Авто-зум должен выдаваться ровно один раз: `CWeapon::Action kWPN_ZOOM` при `b_toggle_weapon_aim` вытолкнет обратно на втором нажатии.

Автор оригинала закомментировал `mark_anm_show_fastzoom = 0.05` (`pda_show.ltx:283`) — быстрый подъём задумывался почти мгновенно освобождающим гейт действия.

### P5. Джойстик aim-грида: 8 направлений + клик

Фирменный жест Gunslinger и единственные имена, которые вообще анимируют модель устройства. Достижим **без** третьего режима ввода, потому что focused-стадия уже владеет курсором.

**Точка съёма мыши — единственный по-настоящему спорный вопрос механики, и обе версии аудита были неверны.**

Вариант из `engine-gaps` — `CDialogHolder::IR_UIOnMouseMove` (`UIDialogHolder.cpp:541`) — в held-стадии мёртв: функция начинается с `CUIDialogWnd* TIR = TopInputReceiver(); if (!TIR) return false;` (:543-545), а held-окно намеренно не на input-стеке. Для focused-стадии он работает, для hip — структурно недостижим.

Вариант из `staging` — `CUICursor::GetCursorPositionDelta()` (`UICursor.h:46`) — хуже: он возвращает `vPos - vPrevPos`, а `vPrevPos` присваивается ровно в одном месте (`UICursor.cpp:114`, первая строка `UpdateCursorPosition`), которое вызывается только из `IR_UIOnMouseMove` (:553). То есть это дельта «с последнего сообщения мыши», а не «с прошлого кадра»: при неподвижной мыши поллинг из `UpdateCL` вернёт один и тот же ненулевой вектор навсегда, мёртвая зона не сработает, направление залипнет. Это гарантированный, а не возможный «шторм перезапусков», которого сам же стадийный план боится. Заодно снимается заголовочное утверждение S5 «джойстику не нужен ни один хук ввода».

**Решение: `CLevel::IR_OnMouseMove` (`src/xrGame/Level_input.cpp:106-118`), до диспетча в UI на :117.** Это прямой аналог их точки патча (`ActorUtils.pas:4035-4049`, которая тоже сидит выше развилки UI). Один сырой хук обслуживает обе стадии, гейтится по режиму.

Резольвер — дословно из 3.2. Период оценки 30 мс, аккумулятор обнуляется каждую оценку.

Клик: `CUIWindow::m_dwLastClickTime` лежит под `protected` (`UIWindow.h:412`, пишется в `UIWindow.cpp:217` на `WINDOW_LBUTTON_DOWN` **до** диспетча детям), а `CUIPdaWnd::OnMouseAction` форвардит в `CUIDialogWnd::OnMouseAction` — значит поле корневого окна PDA действительно обновляется на клик в любом месте диалога. Добавить `u32 LastClickTime() const` на `CUIPdaWnd` — это подкласс, `xrUICore` трогать не нужно.

Пере-выбор анимации: **не** через `m_bStopAtEndAnimIsRunning` (он истинен почти всегда, см. блокеры). Гейтить на ребро смены направления плюс минимальный интервал; это намеренно прерывает идущий idle, блендом прикрывается. Наивная правка — убрать проверку флага без ребра — и есть шторм.

Данные: `use_clicks` и `animation_update_period` в конфиг, не в код. Оригинал отгружает `use_clicks = false`; ставим по умолчанию так же.

### P6. `anm_idle_sprint_start`

Наш движок литерал `anm_idle_sprint` **запрашивает** (`HudItem.cpp:540-546` через `WhichHUDAnimationExist`), так что ключ загружен и сыграл бы — он мёртв по конфигу, а не по движку. Причём наша строка `sprint_allowed = false` (`dead_air_x64_pda3d_items.ltx:23`) не была самостоятельным решением: `[anm_base]` сам ставит `sprint_allowed = false` (`items_animations.ltx:109`), так ведут себя **все** анимационные предметы DA. Удаление нашей строки ничего не даст — нужно явное `true`. Для `_start` нужен латч `m_sprint_started` рядом с `m_aim_started`.

### P7. Анимации headlamp / NV

Четыре ключа, два мотиона (`pda_headflash`, `pda_aim_headflash`, оба у нас есть), но требуется **совершенно новый хук** на переключении фонаря и ПНВ, которого в `src/` нет вообще: литералы `anm_headlamp`, `anm_nv`, `anm_torch` не встречаются нигде, `Torch.cpp`/`ActorInput.cpp` переключают свет без участия предмета.

Наш хук — `CActor::IR_OnKeyboardPress`, кейсы `kNIGHT_VISION` (`ActorInput.cpp:142-149`) и `kTORCH` (:150-154). **Lua-путь DA (`itms_manager.on_key_press` на :146) сохранить и отдать ему приоритет** — он владеет логикой масок.

Механизм класть на `CHudItem`, не на `CPdaAnimatorItem`: гейт — само наличие `anm_`-ключа в hud-секции, то есть чистые данные, ни одного чужого имени в C++, и любой будущий предмет DA получает поведение даром.

Свет включается **посреди** анимации, по метке (0.58 с фонарь, 0.38/0.30 с ПНВ), а не по нажатию. При `can_use_*_when_aim == false` и прицеливании переключение — **отказ**, а не откат на бедренную анимацию; это поведение, а не баг.

Данные: наш items-ltx сейчас не содержит ни одного `anm_headlamp_*`/`anm_nv_*`. Плюс четыре звука из 2.1, наличие которых не проверено. Ниша дорогая и непропорционально инвазивная — брать последней.

### P8. Джойстик hip-грида и третий режим ввода — рекомендуется резать

У них `_is_pda_lookout_mode` (`ActorUtils.pas:414`) — **ортогональная** зуму ось: зум-ин форсирует её в false (`WeaponEvents.pas:1949-1950`), зум-аут в true (:1898-1899), но `kWPN_ZOOM_ALTER` (`ActorUtils.pas:2683-2692`) щёлкает её сам по себе. Достижимых состояний четыре: {опущен, поднят} × {обзор, курсор}. У нас склеено 1:1.

Без hip-курсора 72 из 79 ключей hip-грида недостижимы никогда, каким бы хорошим ни был компоновщик — аккумулятор у бедра всегда нулевой. Плюс, как показано в P5, `IR_UIOnMouseMove` в held-стадии структурно не выполняется, что даёт вторую независимую причину.

Цена: развязать `FocusHeldDialog`/`Unfocus` от `OnZoomIn`/`OnZoomOut`; рычаг маршрутизации мыши — `CUIPdaWnd::NeedCursor()` (`UIPdaWnd.cpp:486-491`), плюс явный `GetUICursor().Hide()` на переключении, потому что `UpdateCursorVisibility` (`UIDialogHolder.cpp:349-374`) держит курсор ещё `psControllerCursorAutohideTime` секунд; плюс гард на `IR_UIOnKeyboardPress` (`UIDialogHolder.cpp:385-392`), синтезирующий `WINDOW_LBUTTON_DOWN` независимо от видимости курсора; плюс сохранение/восстановление позиции курсора через `SetUICursorPosition` (`UICursor.cpp:100-107`) — наш эквивалент их `GetSysMousePoint`/`SetSysMousePoint`, но в канвасных координатах и потому независимый от разрешения; плюс перенос time dilation с зума на режим курсора, потому что в четвёртом состоянии (поднят + обзор) игрок смотрит на живой мир.

**Рекомендация: резать.** Наибольший риск в плане ради выигрыша, визуально почти полностью дублирующего P5. Если режем — режут вместе с ним и строки направлений блоков C–J.

### P9. Ось `_slow` — не реализовывать

В этом наборе ассетов у неё нулевая визуальная дельта: каждая `_slow`-строка дублирует свою не-slow сестру, и наш собственный конфиг это уже доказывает (`dead_air_x64_pda3d_items.ltx:56-58` мапят три разных ключа на один `pda_walk_slow`). Не реализовывать состояние, дать лестнице отката его схлопнуть, ключи не отгружать: минус 40 ключей.

Если кто-то всё же возьмётся: условие в аудите **инвертировано**. `isActorAccelerated` (`Actor_Movement.cpp:582-589`) возвращает `true`, когда модификатор ходьбы **не** зажат, то есть при беге. `actSlow` оригинала — это `mcAccel`. Правильно `!isActorAccelerated(...)` или прямо `(MovingState() & mcAccel) != 0`.

### Компоновщик имени — общий фундамент P1/P4/P5/P6

Живёт на `CPdaAnimatorItem`, override `PlayAnimIdle` (виртуальна на `HudItem.h:128`) плюс приватный сборщик имени. **Не обобщать в `CHudItem::TryPlayAnimIdle`** (`HudItem.cpp:469-499`): это общий путь каждого ствола, ножа, детектора и гранаты в DA, и валидировать изменение против тысяч чужих секций невозможно.

Также override `MovingAnimAllowedNow() → true`: версия `CWeapon` (`Weapon.cpp:2601`) возвращает `!IsZoomed()`, из-за чего aim-moving грид недостижим в принципе.

Обязательна лестница отката справа налево (`_slow` → `_crouch` → `_moving` → джойстик) через `isHUDAnimationExist(..., silent=true)` до гарантированного `anm_idle`. Оригинал не обрезает — у него определены все комбинации; нам обрезка нужна, потому что отгружаем подмножество. Отдельный публичный заголовок `hud_anim_name.h` не делать: примитив уже есть — `CHudItem::WhichHUDAnimationExist` (`HudItem.h:201`, `HudItem.cpp:528-535`), он умеет двух кандидатов, N-кандидатный помощник кладём приватным методом или перегрузкой рядом, а не новым публичным хедером, приглашающим чужие вызовы менять поведение.

Источники состояния: `IsZoomed()`; `pActor->g_State(st)` для `bCrouch`/`bSprint` (как в `HudItem.cpp:476-477`); `pActor->AnyMove()`; направление — из `da_pda3d`.

### `mark_anm_*` — своего места нет, добавляем рядом с OMF-метками

Прямого крючка нет. Наш единственный колбэк — `CHudItem::OnMotionMark` (`HudItem.h:126`, диспетч из `HudItem.cpp:302`), и метки **запечены в файл анимации** (`SkeletonMotions.cpp:477-481`). Ганслингеровские `mark_anm_*` — это секунды в LTX (`ActorUtils.pas:2196`, дефолт 100 = «никогда»). Два разных механизма.

Место: `CHudItem::UpdateCL`, сразу после цикла OMF-меток (`HudItem.cpp:284-306`) и до блока конца мотиона (:307) — там уже посчитаны `motion_prev_time` и `motion_curr_time`. Тест пересечения уровня, один выстрел на мотион. Время резолвить **один раз** в `PlayHUDMotion_noCB` (`HudItem.cpp:420`), где уже присваивается `m_current_motion`, а не конкатенировать строку и лезть в ini каждый кадр (оригинал платит это каждый апдейт — не копировать).

Громко задокументировать главный источник «метка молча не сработала»: весь блок `HudItem.cpp:280-318` гейтится на `m_current_motion_def && m_bStopAtEndAnimIsRunning`, а `m_dwMotionCurrTm` двигается только внутри него. Значит мотион обязан быть запущен через `PlayHUDMotion`, а не `PlayHUDMotion_noCB`. Сбрасывать в `StopCurrentAnimWithoutCallback` (:441-448) и в сбросе конца мотиона (:310-314). Существующий OMF-цикл не трогать: механизмы сосуществуют.

Область: строить **одну** метку `mark_anm_show` (0.85 — момент, когда экран считается живым), общий механизм — только если P7 состоится.

### Счётчик электроники: своя пара файлов, не внутри `da_pda3d`

Модель оригинала (`Misc.pas:250-320`): `previous`/`current`/`target` + `last_update_was_decrease`; `Inc`, `Dec` (кламп в 0), `Reset`, `ImmediateApply`; `Update(dt)` с `max_delta = dt/2000`, т.е. **ровно 1 единица за 2 секунды**, причём `last_decrease` намеренно не обновляется на кадре финального защёлкивания. Полный сброс в `CActor::net_Spawn` (`ActorUtils.pas:2737-2738`).

Lua-экспорты (`UIUtils.pas:1131-1134`): `electronics_break`, `electronics_restore`, `electronics_reset`, `electronics_apply`. **Их собственная gamedata эти функции не зовёт ни разу** (grep по всем 17 275 файлам — 0 совпадений): DLL — механизм, лестницу поставляет мод-хост. Искать у них лестницу бессмысленно, её нет.

**Находка про паритет `m_affects`.** `r_constants.pas:117-120` шлёт `(current/10, random, target/10, is_decreasing ? 1 : 0)`. Сверка с их пиксельным шейдером: `.x` — лестница помех, `.y` — свежий рандом на бинд, `.z` **шейдером не используется**, `.a` гейтит загрузочный экран как `(m_affects.a > 0 && m_affects.x >= 0.08)`. То есть «загрузка PDA» у них — не таймер, а «счётчик сейчас спадает и всё ещё выше 0.08»: девайс перезагружается ровно столько, сколько стекает урон от выброса. Наш `boot_time` (`da_pda3d.cpp:308-326`) — аппроксимация другой механики в той же текстуре.

Наш контракт канала отличается намеренно и **лучше**: `(interference, phase, brightness, boot)`. `.z` под яркость вместо неиспользуемого `target/10`. Записать это в заголовок, иначе кто-нибудь портирует `r_constants.pas:114-120` дословно и сломает `z` и `w`.

Файлы: `src/xrGame/da_electronics.{h,cpp}` — отдельно от `da_pda3d`, потому что счётчик у них потребляют вещи, к PDA отношения не имеющие (отключение фонаря/ПНВ при выбросе, отказы прицелов и коллиматоров). Апдейт звать из тика актёра, не из `HUDManager` — счётчик должен идти независимо от того, поднят ли девайс. Ставка **1/2000 на миллисекунду**, в наших секундах это `dt/2.0f`; брать `Device.fTimeDelta`. Lua-обёртки безопасны к вызову до загрузки уровня (мод зовёт `electronics_break` из схемы выброса, которая может выстрелить на загрузочном экране).

Развилка, которую надо решить явно: существующие `level_base_interference()` и Lua-опросы (`da_pda3d.cpp:97-133`) должны стать **полом счётчика**, а не вторым независимым определением того же входа шейдера.

---

## 6. Данные и скрипты

### 6.1 Что удалить прямо сейчас из `dead_air_x64_pda3d_items.ltx`

Инертны (движок никогда не спрашивает): `anm_idle_moving_slow` (56), `anm_idle_moving_crouch_slow` (58), `anm_idle_aim_moving` (63), `anm_idle_aim_moving_slow` (64), `anm_idle_aim_crouch` (65). Плюс `hud_fov = 0.9` (47) — не читается ничем. `anm_idle_aim_start`/`_end` (61-62) тоже инертны, но их оживляет P1 — **решить один раз**: либо чистим все семь сейчас и возвращаем два в P1, либо не трогаем ни одного до P1. Половинчатый вариант — способ накопить инертный конфиг.

### 6.2 Целевой набор ключей

При наличии лестницы отката минимально-полный набор — **36 ключей**:

- ядро (7): `anm_show`, `anm_hide`, `anm_idle`, `anm_bore`, `anm_blowout`, `anm_idle_moving`, `anm_idle_moving_crouch`
- переходы (7): `anm_show_fastzoom`, `anm_idle_fastzoom`, `anm_idle_aim_start_fastzoom`, `anm_idle_aim_start`, `anm_idle_aim_end`, `anm_idle_aim_end_hide`, `anm_hide_emerg`
- спринт (2): `anm_idle_sprint`, `anm_idle_sprint_start`
- джойстик hip (9): `anm_idle_click` + восемь направлений — **только если P8 берём**
- прицел (10): `anm_idle_aim`, `anm_idle_aim_click` + восемь направлений
- фонарь/ПНВ (4 или 8) — только с P7

Намеренно опущены: все 40 `_slow` (дубли), все 10 `anm_idle_aim_*_moving` (дубли), 20 строк направлений в `_crouch*`/`_moving_crouch*` гридах (идентичны базовым), все `mark_anm_*` (у нас отдельный механизм).

**Без лестницы отката придётся отгружать все 120 ключей дословно**, иначе первая необработанная комбинация убьёт игру.

Добавить недостающую у них строку `anm_idle_click_crouch_slow = pda_click`, если `_slow` вдруг возьмут (см. баг 1 в 3.7).

### 6.3 Инерция: пять параметров из семи расходятся с оригиналом

Мы не отгружаем ни одного `inertion_`-ключа, работают дефолты `player_hud.cpp:41-48` (читаются в `hud_item_measures::load_inertion_params`, :391-401). Против `pda_show.ltx:285-295`:

| Параметр | наш дефолт | оригинал |
|---|---|---|
| PITCH_OFFSET_R | 0.0 | 0 ✔ |
| PITCH_OFFSET_N | 0.0 | 0 ✔ |
| PITCH_OFFSET_D | 0.02 | **0** |
| ORIGIN_OFFSET | −0.05 | **+0.03** (противоположный знак) |
| ORIGIN_OFFSET_AIM | −0.03 | **0** |
| TENDTO_SPEED | 5.0 | **10** |
| TENDTO_SPEED_AIM | 8.0 | **10** |

Аудит подал это как «отгружать нечего или конвертировать осознанно», что читается как выбор, — на деле это описание уже неверного текущего состояния. Имена ключей у нас другие: `pitch_offset_right/up/forward`, `inertion_origin_offset`, `inertion_origin_aim_offset`, `inertion_tendto_speed`, `inertion_tendto_aim_speed`. Дословно копировать строку `inertion_origin_offset` нельзя — знак перевернёт инерцию. Правка дешёвая, чисто данные, и это одна из тех ощущаемых разниц, которые обычно приписывают FOV.

### 6.4 `zoom_hide_ui` и `zoom_dof_near` — вердикт

`zoom_hide_ui = true` (`pda_show.ltx:14`) у них гасит весь игровой UI на время прицеливания. Наш ближайший аналог уже, но у́же по смыслу: `zoom_hide_crosshair`, причём `m_bHideCrosshairInZoom` и так по умолчанию `true` (`Weapon.cpp:633`). То есть прицельная марка скрыта, а здоровье/индикаторы висят за поднятым девайсом, чего у оригинала нет. Либо портировать (ltx-управляемое подавление индикаторов в `OnZoomIn`/`OnZoomOut` рядом с сохранением/восстановлением флагов `recvItem` в `FocusHeldDialog`), либо записать как намеренный отказ.

`zoom_dof_near = −100` (`pda_show.ltx:150`) — **ловушка, записать как явный не-порт**. У них это скаляр ближней плоскости на hud-секции; у нас `zoom_dof` — `fvector3` на секции предмета (`Weapon.cpp:640-641`), причём `m_bZoomDofEnabled = !def_dof.similar(m_ZoomDof)` при `def_dof = (-1,-1,-1)`. У них −100 значит «нет ближнего размытия»; наш дефолт уже значит «эффектора DOF нет». Кто-то, сводя конфиги, напишет `zoom_dof = -100,...` и **включит** DOF, который оригинал намеренно убивает.

### 6.5 Скрипты

Кроме S0-D и S1 (см. items):

- Привязать глушение к `axr_battery.is_jammed()` вместо ручного списка уровней: в `[pda3d_interference_levels]` у нас закомментированы ровно те два уровня (`l10_red_forest`, `l13_generators`), которые лежат в собственной таблице DA (`axr_battery.script:283-289`) и которыми DA уже гасит вкладки PDA. Ключ `jammed_interference = 0.30` в `[pda3d]`; таблицу уровней оставить как расширение для чужих карт, записи не раскомментировать (получится двойная привязка).
- Сметатель на `on_game_load`: сбросить состояние, релизить наш предмет в слоте, проверяя `left:section() == section()` — слот 13 разделяемый, DA сама туда кладёт свои аниматоры, и текущая логика подхвата подняла бы чужой.
- Хрупкая привязка: `configs/script.ltx` читается напрямую из `$game_config$` (`ai_space.cpp:115-125`) и **не** участвует в XMS/pSettings-мердже. Мод, переопределивший `script.ltx`, выкидывает наши функторы. Деградация сегодня корректная (2D-фолбэк), но стоит читать имя модуля из данных (`[pda3d] script_module`) и дописывать его в список, если его там нет. Имя наше, из нашего же ltx — правило «никаких чужих имён в C++» не нарушается.
- Порядок оверлеев: `x_ray.cpp:203` выполняет `XMS::ApplyConfigStage`, и только потом `:208-217` мерджит наш items-ltx. Мы приземляемся последними, и ни один XMS-модуль не может нас перенастроить. Перенести мердж pda3d перед `ApplyConfigStage`, чтобы модули могли патчить (`.ltxp` уже умеет `+key`/`-key`). Риск нулевой: этих имён секций в DA нет.

### 6.6 Совместимость сейвов

Асимметрия классов и секций: класс `WP_PDA3D` зарегистрирован в **бинарнике** (`object_factory_register.cpp:279-280`), а секция `pda_show_animator` живёт только в оверлее. Сейв с поднятым девайсом ломается по-разному в двух направлениях: снят оверлей при том же бинаре — неизвестная секция; откачен бинарь при живом оверлее — неизвестный clsid. Записать двусторонний контракт версий явно.

Серверный близнец — `CSE_ALifeItemWeaponMagazined`, то есть в сейв, пока девайс поднят, пишется оружейное состояние (патроны/аддоны/кондиция). Что это чисто ходит туда-обратно для предмета без ammo-секции — **не подтверждено**.

`CPdaAnimatorItem` не имеет ни одного override save/load/net_Save, и всё новое состояние (`m_aim_started`, `m_fastzoom`) плюс файловые статики `da_pda3d` (`x_smooth`, `boot_until`, `phase_at`, `hands_swapped`, `prev_hands`) не сохраняются. Сметатель на `on_game_load` срабатывает **после** того, как `OnStateSwitch(eShowing)` на восстановленном предмете мог уже вызвать `swap_hands_in()`. Поэтому `da_pda3d::reset()` из `CActor::net_Spawn` — обязательный спутник сметателя, а не опция; и порядок `eShowing` / `on_game_load` надо проверить, а не предположить.

---

## 7. Поставка по стадиям, QA и откат

### 7.1 Инвариант, ради которого всё это

**Каждая стадия гейтится на наличие ключа конфига, и поведение при отсутствующем ключе воспроизводит предыдущую стадию точно.** Это то, что позволяет откатить неудачную стадию правкой одной строки ltx и пересборкой xdb вместо отката бинарника и полного ритуала деплоя. Стадия, чей дифф этим свойством не обладает, считается незаконченной.

### 7.2 Порядок

```
S0 (A,B,C,D)  блокеры + чёрный экран        — сначала, всё остальное неизмеримо
     |
S2 -> S3 -> S4 -> S4b -> S5                 критический путь
     |
M1+M3 -> M2 -> M4' -> M5 -> M6 -> M7 -> M8 -> M9   инверсия
     |
S1, S1b, S6, обрезка OMF, инерция, rt_ui    вне пути, ставятся куда угодно
     |
P7 (фонарь/ПНВ), P8 (третий режим)          за решением пользователя
```

Порядок S0 продиктован тем, что критерий приёмки S2 — измерить высоту строки `letterica16` на скриншоте. (Правка: на боевой установке `enable_use_battery = true`, экран не чёрный, поэтому S0-D приёмку S2 НЕ блокирует — см. поправку в самом S0-D.) S3 зависит от S0-A **технически**, не «по смыслу»: при `IsZoomed() == false` латч мгновенно проиграет `_end` на следующем же idle.

Инверсию и анимационный путь можно вести параллельно разными коммитами: они пересекаются только в `PdaAnimatorItem.cpp`, и M7 явно оставляет `OnStateSwitch` в покое.

Оценка (часы реализации, без QA): S0 ≈ 3–5; S2 4–6; S3 3–4; S4+S4b 6–9; S5 6–8; S1+S1b 4–6; S6 2–3; обрезка OMF 4–6; инверсия M1–M9 12–18; P7 8–12; P8 6–10. Плюс ~1–1.5 ч QA на стадию и ~20 мин на деплой. Критический путь до готовой фичи ≈ 30–40 ч, полный объём без P7/P8 ≈ 50–65 ч.

### 7.3 Общий ритуал (один раз прочитать, применяется ко всем стадиям)

**Сборка.** `tools\build\build_x64.ps1 -Configuration Release`, затем по `PROJECT_RULES.md:196-198` **вторая** инкрементальная сборка, которая обязана отчитаться, что работы не осталось. `-Clean` — только на изменения CMake/тулчейна/зависимостей. `-NoUnity` один раз на пачку перед коммитом: unity-сборка позволяет забытому `#include` скомпилироваться за счёт соседа.

**Перештамповка.** Тронуть `src/xrCore/xrCore.cpp:64` (`buildDate = __DATE__`) и пересобрать, чтобы деплоенная DLL несла сегодняшнюю дату и любой лог был атрибутируем этому кандидату.

**Деплой бинарей.** Игра закрыта. Копировать **весь** список из `packaging/dead-air-x64/installer/runtime-files.txt` (39 файлов), не одну DLL. Потом SHA-256 каждого файла манифеста «установленный против собранного»: `tools/qa/render/Run-RenderQa.ps1:96-110` делает то же сравнение и падает с «The installed <file> does not match» — при устаревшей установке все дальнейшие измерения недействительны.

**Деплой gamedata.** Быстрый цикл — robocopy подкаталога на `D:\Games\Dead Air\gamedata\`. Релизная форма — дот-сорснуть `tools/package/dead_air_x64_compatibility_archive.ps1` и вызвать `New-DeadAirCompatibilityArchive`, затем положить полученный `xtra_dead_air_x64.xdb0` в `database\`. **Ловушка:** loose `gamedata\` затеняет архив, поэтому устаревшая распакованная копия маскирует регресс в xdb — перед финальным проходом стадии удалять loose-копии того, что только что упаковали.

**Кэши шейдеров.** Любая правка `models_pda.s`, `model_pda_screen.ps` или читаемой ими константы: снести `appdata\shaders_cache_xfr` и `appdata\shaders_cache`. `Run-RenderQa.ps1:135-142` специально засевает копии в QA-рантайм — если снесли настоящие, пересейте, иначе первые числа систематически пессимистичны.

**Headless-рантайм `_qa\pda3d_<stage>`.** Все файлы из runtime-files.txt; `New-Item -ItemType Junction` для `database` на настоящую; **настоящая копия** loose `gamedata` (записи архива регистрируются как `<root>\gamedata\`, редирект `$game_data$` спрячет system.ltx); свой `appdata` с `user.ltx` и полной группой сейва (`.scoc`/`.scop`/`.scov` ездят вместе). Запуск через `tools\qa\Start-DetachedHiddenDesktopProcess.ps1` с `-i -silent_error_mode -force_flushlog -always_active -start "server(<save>/single/alife/load)"`. Зонд — по форме `tools/qa/render/qa_render_profile.script`: `actor_on_first_update` → `g_pause_in_background 0`, `main_menu off`, `device():pause(false)`, дальше `level.add_call`, в конце `quit`. Регистрировать дописыванием в строку `script =` **зеркального** `<rt>\gamedata\configs\script.ltx`; после прогона зонд и регистрацию удалить.

Грабли, оплаченные ранее: консольная команда `screenshot` на скрытом десктопе молча ничего не пишет — снимать `tools\qa\Capture-HiddenDesktopWindow.ps1` по маркеру в логе; лог `<rt>\appdata\logs\openxray_admin.log` ротируется в `.bkp` на каждом запуске, поэтому `tail -f` немеет, нужен `tail -F`; движковый Lua `printf` **не** форматирует `%d` — только `string.format`; консольные переменные, затираемые синхронизацией пресета, класть в `<rt>\appdata\qa_autoexec.ltx`, который исполняется после `xrRender_sync_preset_derived`. Разбор: `cmd /c rmdir <rt>\database` **до** любого рекурсивного удаления, иначе сносится настоящая база; потом убедиться, что не выжили `xrEngine`, лаунчер, cmake, ninja и порождённые тестом pwsh.

**Рендер/перф-замеры** — по `PROJECT_RULES.md:247-295` скрытый десктоп запрещён: `Run-RenderQa.ps1` на основном десктопе, фуллскрин 2560×1440, физические границы DWM проверить до того, как чему-то верить. Абсолютный FPS на скрытом десктопе занижен фоновым GPU-приоритетом; валидно только A/B внутри одной среды. Первый вопрос на любое «стало медленнее» — какие были флаги запуска: один `-dxdebug` вдвое режет кадр и уже сжёг однажды полный день расследования.

### 7.4 Приёмка и откат по стадиям

| Стадия | Наблюдаемый результат | Килл-свитч |
|---|---|---|
| S0-A | Msg показывает `IsZoomed()` на входе/выходе фокуса | инструментальная сборка, не коммитится |
| S0-B | ESC в focused-стадии опускает девайс; RMB даёт `IsZoomed() == true` | откат правки |
| S0-C | Debug-сборка переживает зум-аут | — |
| S0-D | экран не чёрный при выключенном `enable_use_battery` / без wpn_upd (на боевой установке опция включена, регресс не виден — проверять на конфиге с выключенной) | восстановить прежний `get_power` |
| S1 | шесть различимых состояний помех по форсированным значениям; затем один прогон на реальном выбросе | `return 0.0` |
| S2 | высота строки `letterica16` в прицельной позе ≥ 22.5 px при ~9–10 px в held; **и** снимки AK/пистолета/детектора бит-в-бит равны доpatch-снимкам | удалить два ключа из ltx |
| S3 | отдельный мотион подъёма и опускания; холстер из прицела не анимируется дважды | удалить `anm_idle_aim_start`/`_end` |
| S4 | шаг/присед-шаг/присед-стойка визуально различимы; плюс офлайн-претфлайт конфига | удалить добавленные ключи, лестница схлопнется |
| S5 | восемь направлений достижимы без стробирования; клик даёт `pda_click` **ровно один раз**, а не каждый кадр | `joystick = off` |
| S6 | загрузочная последовательность стартует на одном и том же кадре во всех пяти подъёмах | удалить `mark_anm_show` |
| M1+M3 | все входы открывают/закрывают один раз, ни одного проглоченного нажатия | ревёрт коммита (это не data-gated) |

Появление в QA-логе строки `! [pda3d] presenter flag with no item in the slot - force reset` — **провал стадии**, а не успешное самолечение.

---

## 8. Регрессии, что не портируем, открытые вопросы

### 8.1 Список регрессий — прогонять в конце каждой стадии

Каждая позиция уже однажды оплачена.

**Рендер (`cadd50e73`).** `UI().RenderFont()` остаётся внутри `RenderPdaScreenUI` при связанном `$user$ui` — иначе глифы вываливаются на бэкбуфер. RT-проход остаётся в `CRender::BeforeWorldRender` — внутри `Render()` он попадает в окно параллельных контекстов (тот же класс гонок, что убивал партиклы в 1.3.3) и замерзает под меню паузы. `rmNormal(RCache)` на входе и выходе вокруг `u_setrt`. Forward, не emissive: `sorting(2,true)` + `blend(srcalpha,invsrcalpha)` + `zb(true,false)`; связать UI и как albedo, и как emissive — значит возвести картинку в квадрат. `smp_rtlinear`, не `smp_base`. Никакого `float3 tc1 : TEXCOORD1` в PS — VS его не эмитит. Sub-rect экрана — из живой разметки каждый кадр, не запечён в меш. В RT попадает только `CUIPdaWnd::Draw()` (+хинты/курсор), никогда `CUIGameCustom::Render()` — тот рисует CustomStatics безусловно, и индикатор кислорода из DAR2 запёкся бы на экране PDA. Никогда не кэшировать `CUIPdaWnd*` между кадрами: `OnUIReset` пересоздаёт окно на смене разрешения/языка/ui_style.

**Входы (`3fa251d57`).** Никакого движкового обработчика клавиши PDA: ею владеет Lua (`itms_manager`), а DAR2 этот скрипт заменяет — движковый обработчик даст двойной тоггл. Фокус только через `FocusHeldDialog`/`Unfocus`, никогда `StartDialog`/`StartMenu` на показанном окне. Time dilation только в focused-стадии — held-но-нефокусная стадия обязана не замедлять, иначе это перманентное слоу-мо в бою. Никаких явных `Show()` вокруг `AddDialogToRender`/`Remove` — они делают это сами.

Механизм удвоения инфопорций назвать явно, а не только симптом: `operator==` для `dlgItem` включает поле `enabled` (`UIDialogHolder.cpp:18`), `RemoveDialogToRender` (:202-220) запись не стирает, а гасит, поэтому `Add` в том же кадре не находит совпадения, пушит дубль и вызывает `Show(true)` второй раз — две порции `ui_pda` в `m_known_info_registry`, то есть в сейв навсегда. Мусор пожинается раз в кадр (:335-337), так что это не течь, но внутри кадра — две активные записи. Реконсилятор делает такой сценарий достижимым (M1 гасит, ветвь позже поднимает). QA — счётный зонд на десять циклов, а не «на глаз». Апстримовая однострочная правка (убрать `enabled` из `operator==`) — отдельный, независимо бисектируемый коммит, после проверки остальных пользователей.

**Разборка (`2b63a8adf`, `7c7238177`).** Сперва холстер, потом релиз: активировать предыдущий слот, дать предмету пройти `eHiding → eHidden` и только тогда релизить. Спавн в полёте не сиротить. В `eHidden` руки DA возвращаются **до** появления следующего оружия: `detach_item(this)`, потом `swap_hands_out()`; наоборот — и ствол поднимется на рижке PDA. Focused-стадия отвечает на P/M/контакты опусканием от лица, а не полноэкранным диалогом.

**Проектные правила, которые эта фича постоянно задевает.** Никогда не опознавать PDA через `GetPDA()`/сырой каст на PDA_SLOT: `dev_flash_1`, `dev_flash_2` и `pri_a25_explosive_charge_item` — все `class = D_PDA` в слоте 7, причём квестовый — это боевая взрывчатка; опознание только по списку секций из ltx, и в тесты положить «сунуть `pri_a25_explosive_charge_item` в слот и нажать клавишу PDA». Никаких чужих имён в C++ (`wpn_upd`, имена уровней, `axr_battery`). Compat-скрипты живут в своей namespace-таблице: `_G.da_pda3d_activate`/`_deactivate` нужен префикс `_G.`, а `get_power`/`get_interference` резолвятся движком как `dead_air_x64_pda3d.get_*` и обязаны остаться namespace-локальными — не «чинить» их на `_G.`. Дым гонять на обоих скриптовых слоях: базовом и с `database/xtra_dar2.xdb0` (Revolution II везёт свои `pda.script`, `itms_manager.script`, `pda_tasks_16.xml`).

**Матрица сценариев для каждой стадии:** смерть с поднятым девайсом; смена уровня; выход в главное меню; сейв с поднятым → загрузка; загрузка сейва, сделанного до фичи; уход в оружейный слот и обратно, и уход навсегда; захват кровососом и psy-hit контроллера (проверить, что реконсилятор после M2 продолжает тикать); переименованный (сломанный) Lua-скрипт; no-weapon зона, лестница, транспорт, инвентарь, окно торговли — каждое с поднятым девайсом; смена аспекта `pda.xml` ↔ `pda_16.xml`; смена костюма; чужой предмет, припаркованный в слоте 13.

**Самый дешёвый тест в списке — прогонять первым и последним:** переименовать `dead_air_x64_pda3d.ltx`, чтобы `available()` вернул false, и убедиться, что каждый путь ведёт себя ровно как сток. Это контракт, на котором стоит весь дизайн. Учесть, что на уровне VRAM он сегодня **не** выполняется (см. 2.4).

Мелочь на один взгляд во время UI-прохода: `std::sort` по `m_dialogsToRender` идёт каждый кадр (`UIDialogHolder.cpp:335`) с компаратором, упорядочивающим только по `enabled`, так что порядок отрисовки среди включённых не определён. Апстримовое, сегодня безобидное (PDA обычно единственная запись), но held-стадия впервые заставляет PDA долго сосуществовать со списком рендера HUD.

### 8.2 Что намеренно не портируем

**Строительные леса бинарного патчера** — портировать любую из них значит завести вторую конкурирующую реализацию того, что у нас уже есть:
`OnActorSwithesSmth` со спавном аниматора и танцем `ActivateActorSlot` (у нас Lua-примитивы + настоящий класс); рукописный асм `virtual_CActor__IR_OnMouseMove` через vtable-смещение 0x298 (мы зовём `IR->IR_OnMouseMove` напрямую); ветка `correction` в `MoveMouse`, компенсирующая синтетические дельты (мы их не синтезируем); `GetSysMousePoint`/`SetSysMousePoint` в экранных пикселях (наш `CUICursor::SetUICursorPosition` работает в канвасе и сам пишет системный указатель — строго лучше); `IsPDAAnimatorInSlot`, опрашивающий слот каждый кадр (мы владеем объектом); `_was_pda_animator_spawned` + `HidePDAMenu()` на неудаче (у нас fail-closed `available()`); их ini-кэш `cached_cfg_param_float` (у нас `Load` читает в члены); и весь несвязанный ворох из того же `ActorUpdate` (пинки, горение, быстрая граната, спринт с детектором).

**`screen_kx`** — существует только чтобы перемасштабировать map-споты, потому что их PDA рендерился в другом аспекте, чем экран. У нас его порт даст двойное масштабирование.

**Все 11 `configs/action_animators/*.ltx` и `action_animators.script`** — референс значений, не отгружаемые ассеты. Из них `base_animator.ltx` даёт `class = WP_BINOC`, слот, `animation_slot 13`, `not_weapon`, `action_animator` — всё это уже воспроизведено у нас в C++. `headlamp_enable/disable.ltx` и `nv_enable/disable.ltx` — **точно вне области**: они гоняют `headflash_hud.ogf`, то есть случай «в руках ничего нет», а PDA обрабатывает свой тумблер сам.

**`pda_vibros111.anm`, `ui_mono_noise`, шейдеры r1/r2** — по разобранным в 2.1 причинам.

**`_slow` как состояние** — нулевая визуальная дельта на этом наборе ассетов.

**Блок M (aim + moving)** — полностью дублирует блок L, значение в значение.

**`zoom_dof_near = -100`** — не переносить и не «сводить»: у нас это включит DOF, который оригинал выключает.

**`mark_anm_*` как общий механизм** — строим ровно одну метку, пока P7 не принят.

### 8.3 Что осталось действительно открытым

Вынесено в `open_questions`. Коротко: судьба третьего режима ввода (и, следом, клавиши для него), работа PDA в no-weapon зонах, `sprint_allowed`, переименование секций, поведение `m_affects.y`, комбинирование источников интерференции на глушащих уровнях во время выброса, и стоит ли записывать 3D-инвентарь как follow-on. Вопросы про дубли ключей в ini и про `animation_update_period` **закрыты** и в список не входят.

---

## Постскриптум: руки (30.08.2026, вечер)

Попытка ретаргета ганс-анимаций на DA-риги (чтобы убрать своп рук) упёрлась в доказанный тупик: у ганс-рига и DA-ригов **разные позы покоя кистей** — skin-дельты левого и правого запястья расходятся на ~116°, и жёсткий двуручный девайс (висящий на анкер-кости `lead_gun`, см. `player_hud::calc_transform`) невозможно удержать в обеих «правильных» ладонях ни одной схемой переноса (замерено: чистый skin-перенос уводит хваты на 0.7–1.1 м / 106–164°). Итоговое решение: своп на родной ганс-риг ОСТАВЛЕН, а исходная жалоба (камуфляжные перчатки) закрыта **перекраской текстуры** `act_arm_3.dds` — флектарн → тёмный DA-олив, кожа нетронута (зонально-цветовая маска, скрипт остался в истории сессии). Инструменты `tools/animation/` сохранены — для ретаргета ОДНОРУЧНЫХ предметов между этими ригами схема skin-дельты валидна.

## Решения пользователя (30.08.2026) и статус реализации

* **Курсорный (третий) режим — отрезан навсегда.** Наша focused-стадия (RMB: приближение + курсор + WASD) покрывает и их курсорный, и их зум разом; их разделение существовало из-за нечитаемости текста без зума, что признавали сами авторы. Джойстик реализован в aim-сетке (pda_aim_idle_*), hip-грид не нужен.
* **No-weapon зоны: КПК достаётся.** Механизм: флаг `ignore_slots_blocked` на предмете (движок: `CInventory::IsSlotBlocked(PIItem)`), счётчики блокировок слотов не трогаются — реальное оружие подчиняется зоне как раньше, prev-slot restore зоны жив. Холстер в заблокированной зоне падает в `activate_slot(0)` (fallback в скрипте, state 5).
* **Сейв/мод-совместимость обязательна** → секции НЕ переименованы (остались `pda_show_animator`/`pda_show_animator_hud`).
* **3D-инвентарь (их `inventory_show`, та же железка) — «на потом».** Новых ассетов не требует; конфиг + та же движковая механика.
* Открытые вопросы 6 (m_affects.y) и 7 (комбинирование источников) закрыты реализацией: держанная фаза 0.125 с; max() по источникам + движковый rate-ramp.

Реализация выполнена в этой же сессии (window-led инверсия, джойстик, aim-латч, hud_fov факторы, screen-on-mark, электроника от выброса/пси/глушения, обрезка OMF до 35 мотионов, ленивый rt_ui, камерные эффекторы, no-weapon зоны, reset на спавне актёра) — см. коммиты после cadd50e73.

---

## Чек-лист работ

### S0-A: инструментальная сборка — проверить, зоомится ли focused-стадия вообще

*трудоёмкость: trivial · риск: low*

Msg() в CPdaAnimatorItem::OnZoomIn/OnZoomOut (src/xrGame/PdaAnimatorItem.cpp:104-133) печатает IsZoomed(), GetState(), m_zoom_params.m_fZoomRotationFactor. Причина: FocusHeldDialog (src/xrGame/UIDialogHolder.cpp:112-120) в конце делает A->IR_OnKeyboardRelease(kWPN_ZOOM) и kWPN_FIRE; CActor::IR_OnKeyboardRelease (ActorInput.cpp:279) -> inventory().Action(cmd, CMD_STOP) -> CWeaponMagazined::Action (WeaponMagazined.cpp:939) вызывает inherited ДО проверки IsPending -> CWeapon::Action kWPN_ZOOM (Weapon.cpp:1393); b_toggle_weapon_aim = FALSE (Weapon.cpp:140), zoom_enabled = true в нашей секции, значит идёт ветка `else if (IsZoomed()) OnZoomOut();`. CWeaponBinoculars::Action (WeaponBinoculars.cpp:33) ремапит kWPN_FIRE в kWPN_ZOOM — срабатывает дважды. Гард в нашем OnZoomOut `if (da_pda3d::ui_focused())` в этот момент ещё false, поэтому расфокус пропускается, но inherited::OnZoomOut уже снял m_bIsZoomModeNow.

### S0-B: починить порядок в CPdaAnimatorItem::OnZoomIn

*трудоёмкость: small · риск: medium · зависит от: S0-A*

Если S0-A подтверждает: добавить `bool m_in_focus_switch` в src/xrGame/PdaAnimatorItem.h; выставлять его вокруг вызова ui->FocusHeldDialog(pda, true) в OnZoomIn (PdaAnimatorItem.cpp:116) и делать CPdaAnimatorItem::OnZoomOut ранним no-op при взведённом флаге. Альтернатива: da_pda3d::set_ui_focused(true) ДО FocusHeldDialog, тогда существующий гард сработает — но inherited::OnZoomOut всё равно снимет зум, поэтому нужен именно re-entrancy guard, а не перестановка. Третий вариант — убрать две строки IR_OnKeyboardRelease из FocusHeldDialog (они скопированы из StartMenu, где нужны для отпускания залипшей клавиши при открытии меню; held-стадия — не тот случай).

### S0-C: заглушить VERIFY(m_binoc_vision) на пути зум-аута

*трудоёмкость: trivial · риск: low*

[anm_base] (D:/Games/Dead Air/_analysis/base_configs/configs/misc/items/items_animations.ltx:113) ставит vision_present = false, поэтому CWeaponBinoculars::OnZoomIn (WeaponBinoculars.cpp:44-48) не создаёт m_binoc_vision, а CWeaponBinoculars::OnZoomOut (WeaponBinoculars.cpp:56-64) выполняет VERIFY(m_binoc_vision) при H_Parent() && IsZoomed() && !IsRotatingToZoom(). В Debug это Fail. Override CPdaAnimatorItem::OnZoomOut, вызывающий CWeaponMagazined::OnZoomOut напрямую в обход бинокулярной ветки. Без этого Debug-сборка недоступна на всё время работы над S3/S5/M3.

### S0-D: get_power() — снять постоянный blackout экрана

*трудоёмкость: trivial · риск: low*

ВАЖНАЯ ПОПРАВКА К АУДИТУ (проверено на боевой установке): в `D:/Games/Dead Air/gamedata/configs/axr_options.ltx:71` стоит `enable_use_battery = true`, батарея реально работает и экран в игре НЕ чёрный — скриншоты пользователя это подтверждают. Это не текущий баг и не блокер приёмки, а скрытый риск: на установке с выключенной опцией или без wpn_upd `axr_battery.getBatteryCondition()` (_analysis/base_configs/scripts/axr_battery.script:270-276) вернёт 0, и цепочка packaging/dead-air-x64/compatibility/gamedata/scripts/dead_air_x64_pda3d.script:167-178 -> da_pda3d.cpp:303 (lua_power <= cfg.power_low = 0.05) -> x_target = cfg.blackout_level (0.55) -> шейдерный порог 0.41 даст сплошную черноту. Правка та же и остаётся нужной, но приоритет обычный, а не блокирующий: по канону DA (itms_manager.script:330/408/682) выключенная опция или отсутствующий wpn_upd = полный заряд. Скрипт-онли, без ребилда.

### S1: реальный источник интерференции (скрипт-онли)

*трудоёмкость: small · риск: low · зависит от: S0-D*

get_interference() в dead_air_x64_pda3d.script сейчас возвращает 0.0, весь ladder в model_pda_screen.ps мёртв. Источники: surge_manager.is_started()/get_surge_manager() (.inited_time, .surge_time=222), psi_storm_manager (.psi_storm_duration=127), axr_battery.is_jammed(). Кривые в [pda3d_surge_curve]/[pda3d_psi_curve] в dead_air_x64_pda3d.ltx, breakpoint+линейная интерполяция. Все обращения через `if X and X.fn then`. Ни одного чужого имени в C++.

### S1b: линейный ramp интерференции + da_pda3d::reset()

*трудоёмкость: small · риск: low · зависит от: S1*

da_pda3d.cpp:313-315 сглаживает экспонентой tau 0.15 c — фактически мгновенно. Оригинал (Misc.pas:305-324) даёт max_delta = dt/2000, т.е. 0.05 нормированной единицы в секунду. Заменить на rate-limit, ключ interference_ramp в [pda3d]. Отдельно добавить da_pda3d::reset() (обнуляет x_smooth, boot_until, phase_at, hands_swapped, prev_hands) и звать из CActor::net_Spawn — аналог ActorUtils.pas:2737-2738. Без этого prev_hands переживает загрузку сейва.

### S2: hud_fov_factor / hud_fov_zoom_factor в общем пути CWeapon

*трудоёмкость: small · риск: medium · зависит от: S0-B*

ПОДТВЕРЖДЕНО: src/xrGame/Weapon.h:106 объявляет `float GetHudFov();` без virtual, единственный вызов — src/xrGame/ActorCameras.cpp:347 через CWeapon*. Перекрытие на CPdaAnimatorItem — мёртвый код. Делать в CWeapon::Load рядом с Weapon.cpp:520 два READ_IF_EXISTS (hud_fov_factor default 1.0, hud_fov_zoom_factor default = hud_fov_factor), и в return CWeapon::GetHudFov (Weapon.cpp:2598) умножать m_nearwall_last_hud_fov на lerp(factor, zoom_factor, af). При отсутствии ключей k == 1.0 и вывод бит-в-бит прежний — это и есть критерий приёмки диффа. af = m_zoom_params.m_fZoomRotationFactor ТОЛЬКО после закрытия S0-A/S0-B. Добавить второе плечо триггера из оригинала (ActorUtils.pas:2861-2938): лерп держится и когда m_current_motion начинается с "anm_idle_aim", иначе на переходных мотионах S3 FOV дёрнется. Удалить мёртвый hud_fov = 0.9 из dead_air_x64_pda3d_items.ltx:47.

### S3: aim start/end как edge-latch

*трудоёмкость: small · риск: medium · зависит от: S2*

Один bool m_aim_started на CPdaAnimatorItem, логика внутри override PlayAnimIdle. Ключи anm_idle_aim_start/anm_idle_aim_end уже лежат в items ltx:61-62 и уже резолвятся в omf, но ни один литерал не встречается в src/ — они инертны. Гейт через isHUDAnimationExist (HudItem.cpp:502). Параметр W в PlayHUDMotion НЕ используется (HudItem.cpp:392-407) — не строить на нём ничего. Переход из eHiding не проигрывать: пусть holster примешивается сам.

### S4: компоновщик имени idle-анимации на CPdaAnimatorItem

*трудоёмкость: medium · риск: medium · зависит от: S3*

Порядок суффиксов: anm_idle + _aim + <joystick> + _moving + _crouch + _slow (WeaponAnims.pas:55-83/100-191, ActorUtils.pas:1944-1959; сверено с pda_show.ltx:164-250). Override PlayAnimIdle и MovingAnimAllowedNow()->true (база CWeapon::MovingAnimAllowedNow, Weapon.cpp:2601, возвращает !IsZoomed()). Обязательна лестница отката справа налево через isHUDAnimationExist(silent=true) до anm_idle. НЕ обобщать в CHudItem::TryPlayAnimIdle — это общий путь всех стволов. Ось _slow не реализовывать (см. отдельный пункт).

### S4b: re-trigger idle по смене направления — не через m_bStopAtEndAnimIsRunning

*трудоёмкость: small · риск: medium · зависит от: S4*

CHudItem::OnMovementChanged (HudItem.cpp:548-556) гейтится на !m_bStopAtEndAnimIsRunning, а этот флаг взводится в PlayHUDMotion на ЛЮБОМ мотионе, включая зацикленный idle (HudItem.cpp:396-401), т.е. хук уже практически мёртв. Реальный пере-выбор происходит раз за цикл в блоке конца мотиона HudItem.cpp:307-315 -> OnAnimationEnd -> switch2_Idle -> PlayAnimIdle. Для джойстика: в CPdaAnimatorItem::UpdateCL гейтить на РЕБРО смены направления + минимальный интервал (animation_update_period), без проверки m_bStopAtEndAnimIsRunning. Без ребра — шторм перезапусков и дрожь рук.

### S5: 8-way джойстик — аккумулятор в CLevel::IR_OnMouseMove

*трудоёмкость: medium · риск: high · зависит от: S4b*

CDialogHolder::IR_UIOnMouseMove (UIDialogHolder.cpp:541-545) начинается с `if (!TIR) return false;` — в held-стадии окно не на input-стеке, TopInputReceiver() == nullptr, функция не выполняется вовсе. CUICursor::GetCursorPositionDelta() (UICursor.h:46) — дельта относительно vPrevPos, обновляемого только внутри UpdateCursorPosition, т.е. только по событию мыши: поллинг в UpdateCL даст один и тот же ненулевой вектор навсегда. Единственная корректная точка — CLevel::IR_OnMouseMove (Level_input.cpp:106-118), до диспетча в UI на :117. Резольвер: dead zone 2 сырых счётчика, atan2 в [0,2pi), границы 0.393/1.18/1.96/2.74/3.53/4.32/5.10/5.89, остаток = Right. Клик — через новый аксессор LastClickTime() на CUIPdaWnd поверх protected CUIWindow::m_dwLastClickTime (UIWindow.h:412, пишется в UIWindow.cpp:217 значением Device.dwTimeContinual).

### S6: mark_anm_show вместо boot_time-таймера

*трудоёмкость: trivial · риск: low*

Ровно одна метка, не общий механизм. Разрешать её один раз в CHudItem::PlayHUDMotion_noCB (HudItem.cpp:420), где уже присваивается m_current_motion, конкатенацией "mark_" + алиас и READ_IF_EXISTS из HudSection(); проверку пересечения уровня класть в CHudItem::UpdateCL сразу после цикла OMF-марок (HudItem.cpp:284-306), где уже посчитаны motion_prev_time/motion_curr_time. Сбрасывать в StopCurrentAnimWithoutCallback (HudItem.cpp:441-448) и в блоке конца мотиона. Весь блок гейтится на m_bStopAtEndAnimIsRunning — мотион должен быть запущен через PlayHUDMotion, а не PlayHUDMotion_noCB.

### M1: режим показа на CUIPdaWnd + два override как единственные хуки

*трудоёмкость: small · риск: medium*

enum EPresent { ePresentNone, ePresentFullscreen, ePresentHeld } + IsHeld()/ShowHeld()/HideHeld() в src/xrGame/ui/UIPdaWnd.h. Переписать ShowDialog/HideDialog (UIPdaWnd.cpp:222-245) в селекторы режима без чтения глобалов. МИНА: CUIDialogWnd::HideDialog — это `if (GetHolder() && IsShown())` (UIDialogWnd.cpp:81-85), а SetHolder зовётся только из FocusHeldDialog (UIDialogHolder.cpp:104), т.е. на held-но-нефокусном окне неперекрытый HideDialog — тихий no-op; этим путём ходят все туториалы и моды. ShowHeld() зовёт ТОЛЬКО AddDialogToRender (он сам делает Show(true) на UIDialogHolder.cpp:199). HideHeld() ОБЯЗАН сбрасывать m_present = ePresentNone, иначе появляется второй рассинхронизуемый бит. Сохранить HideActorMenu() и добавить рантайм-гейт can_raise() (актор жив, уровень готов, слот не заблокирован) — иначе теряется 2D-фолбэк.

### M2: реконсилятор в CActor::UpdateCL, а не в HUDManager

*трудоёмкость: trivial · риск: low · зависит от: M1*

da_pda3d::update() зовётся из CHUDManager::OnFrame (HUDManager.cpp:60), который выходит на :52 при снятом HUD_DRAW_RT2; флаг снимают bloodsucker_vampire_execute_inline.h:35 и controller_psy_hit.cpp:224. Тот же ранний выход пропускает и pUIGame->OnFrame() на :63 — там встаёт весь игровой UI, не только наш watchdog. Реконсилятор переносить в CActor::UpdateCL; экранные константы (da_pda3d.cpp:299-336) оставить под флагом — экран PDA замирает на последнем кадре во время захвата, и это правильно.

### M3: реконсилятор как транслитерация ActorUtils.pas:2246-2322

*трудоёмкость: medium · риск: medium · зависит от: M2*

held = pda->IsHeld() && pda->IsShown(), mine = smart_cast<CPdaAnimatorItem*>(inventory().ItemFromSlot(ANIMATION_SLOT)) — именно smart_cast, а не проверка на непустой слот (da_pda3d.cpp:263/275 сейчас проверяет только ItemFromSlot(13) != nullptr). Пять ветвей: спавн; таймаут спавна -> HideDialog; held+mine с чужим активным слотом -> Activate; !held+mine -> Activate(prev)+release по достижении eHidden; !g_Alive -> немедленный release. ДОБАВИТЬ шестую: held+mine, но слот заблокирован -> убрать девайс и закрыть окно, не ретраить (иначе каждый кадр Activate уходит в ветку Inventory.cpp:576-579).

### M4-ЗАМЕНА: хранить предыдущий слот в da_pda3d, НЕ в m_iPrevActiveSlot

*трудоёмкость: trivial · риск: low · зависит от: M3*

Утверждение исходного плана «в дерево ничего не пишет m_iPrevActiveSlot» ложно: пишут Inventory.cpp:579, :1519, :1524, Actor_Network.cpp:692; читают Inventory.cpp:1485-1496 (TryActivatePrevSlot на снятии блокировки) и ActorInput.cpp:854/885 (next/prev weapon). Захват поля ломает переключение стволов и восстановление после no-weapon зоны, а в заблокированном слоте наш же Activate затирает поле значением 13. Держать u16 в состоянии da_pda3d, чистить в том же reset().

### M5: снять пять перехватов точек входа

*трудоёмкость: small · риск: low · зависит от: M3*

UIGameCustom.cpp:234-238 и :244-248 (ShowPdaMenu), :256-260 (HidePdaMenu), UIGameSP.cpp:267-268 (StartDialog-воронка) — вернуть к апстриму, сохранив HideActorMenu() и строки TimeDilator на UIGameSP.cpp:274-275. UIPdaWnd.cpp:464 и :567 — заменить presenter_active() на IsHeld(); на :464 не потерять вторую половину условия m_rt_frame == Device.dwFrame. ВАЖНО: M1+M5 в одиночку девайс не убирают — холстер делает ветвь 4 реконсилятора, так что первым коммитом идёт M1+M3, а не M1+M5.

### M6: свести Lua к двум примитивам

*трудоёмкость: small · риск: low · зависит от: M3*

dead_air_x64_pda3d.script теряет строки 13-162 (шесть состояний, tick, таймауты, adopt-хак, 30-секундный дворник, actor_on_update). Остаются _G.da_pda3d_spawn(), _G.da_pda3d_release(), get_power(), get_interference() и новый on_load-сметатель (RegisterScriptCallback("on_game_load")), который сбрасывает состояние и релизит наш предмет в слоте, проверяя left:section() == section(). Спавн оставить в Lua — рецепт проверен и переопределяем модами.

### M7: убрать владение диалогом с предмета

*трудоёмкость: small · риск: medium · зависит от: M3*

PdaAnimatorItem.cpp: удалить attach_ui/detach_ui (:11-51) и m_ui_attached; удалить override net_Destroy (:85-102) вместе со спецслучаем Level().bReady; в UpdateCL (:135-152) оставить только consume_unzoom_request. Сохранить без изменений OnStateSwitch (:53-83) минус вызов attach_ui: swap_hands_in на eShowing и порядок detach_item -> swap_hands_out на eHidden (:71-80) выстраданы. da_pda3d::on_shown() перенести в OnStateSwitch(eShowing).

### M8: удалить хранимое состояние в da_pda3d, НО оставить focused

*трудоёмкость: small · риск: low · зависит от: M7*

Удаляются presenter (:40), set_presenter_active/presenter_active (:221-227), toggle() (:168-179), deactivate_at + троттл (:139, :184-188), orphan watchdog (:271-297). focused (:41) НЕ выводить из IsZoomed(): во-первых, до закрытия S0-A зум в focused-стадии может быть false и разом умрут UIPdaWnd.cpp:566-572, da_pda3d.cpp:286-290 и троттл RT в HUDManager.cpp:240; во-вторых, третий режим ввода специально делает lookout ортогональным зуму. Переименовать в cursor_mode/input_focused и оставить хранимым.

### M9-ИСПРАВЛЕНО: ANIMATION_SLOT вместо литерала 13 и вместо чтения ltx

*трудоёмкость: trivial · риск: low · зависит от: M3*

Чтение pSettings->r_u32(section, "slot") даст 12: [anm_base] ставит slot = 12, а CInventoryItem::Load (inventory_item.cpp:109-112) делает base_slot_id = sl + 1. Правильный ход — константа ANIMATION_SLOT (src/xrServerEntities/inventory_space.h:23-24, == RESERVED_SLOT == 13), сейчас используемая ровно в одном месте (CustomDetector.cpp:60). Заменить литералы в da_pda3d.cpp:263 и :275.

### Блокируемые слоты: отказ активации + ветвь на блокировку в полёте

*трудоёмкость: small · риск: medium · зависит от: M3*

INV_STATE_BLOCK_ALL = 0xffff (Inventory.cpp:29), бит 13 внутри; INV_STATE_CAR/LADDER/INV_WND/BUY_MENU — его алиасы (Inventory.cpp:30-33). Сегодня в no-weapon зоне da_pda3d_activate возвращает true после alife():create, 2D-фолбэк подавляется, объект сиротеет. Добавить проверку CInventory::IsSlotBlocked(ANIMATION_SLOT) (Inventory.h:198, приватная — сделать const-обёртку) в can_raise()/request_activate и ветвь 6 реконсилятора для блокировки посреди эпизода. Регресс-кейсы: no-weapon зона, лестница, транспорт, инвентарь, торговля — с поднятым девайсом.

### Обрезать pda_hands_animation.omf до 35 pda_* мотионов

*трудоёмкость: medium · риск: medium*

Файл байт-в-байт равен gunsdata hands.omf (1 198 783 B, sha256 f3bb325ee1ecc62f66...), 58 мотионов, 23 лишних на 915 515 B полезной нагрузки. Из-за [player_hud_extra_omf] (dead_air_x64_pda3d.ltx) и SkeletonAnimated.cpp:879-889 этот omf грузится на КАЖДУЮ модель рук при каждой перезагрузке рук — это рантайм-память, а не размер архива, поэтому обрезка обязательна, а не опциональна. Риск: переиндексация OGF_S_MOTIONS против таблицы OGF_S_SMPARAMS, загрузчик делает MS->find_chunk(m_idx + 1) (SkeletonMotions.cpp:260); при ошибке — тихий откат в bind pose. После пересборки проверить все 34 ссылаемых значения и ID_Cycle_Safe на wpn_hand_pda3d.ogf. Целевой размер ~283 KB.

### Досыпать anims/camera_effects/weapon/pda_draw.anm (+ pda_headflash.anm условно)

*трудоёмкость: trivial · риск: low*

6 395 B и 2 531 B из .../gunsdata/unpacked/anims/camera_effects/weapon/. Хукап нулевой: player_hud.cpp:525 строит "camera_effects\\weapon\\<motion>.anm" и проверяет FS.exist по $game_anims$. tools/package/dead_air_x64_compatibility_archive.ps1 копирует дерево целиком, правка скрипта не нужна, но каталога anims/ у нас ещё не было — сделать одну намеренную сборку и проверить round-trip .anm через converter.exe. НЕ тащить pda_vibros111.anm: мотиона с таким именем нет нигде, у оригинала камер-эффектора на вибрацию не было.

### Инерция: пять из семи параметров расходятся с оригиналом

*трудоёмкость: trivial · риск: low*

Мы не шлём ни одного inertion_-ключа, работают дефолты player_hud.cpp:41-48 (читаются в hud_item_measures::load_inertion_params, :391-401). Расхождения с pda_show.ltx:285-295: PITCH_OFFSET_D 0.02 против 0, ORIGIN_OFFSET -0.05 против +0.03 (ПРОТИВОПОЛОЖНЫЙ знак), ORIGIN_OFFSET_AIM -0.03 против 0, TENDTO_SPEED 5.0 против 10, TENDTO_SPEED_AIM 8.0 против 10. Наши имена ключей другие: pitch_offset_right/up/forward, inertion_origin_offset, inertion_origin_aim_offset, inertion_tendto_speed, inertion_tendto_aim_speed. Прописать конвертированные значения и проверить знак на экране, а не по числу.

### rt_ui выделяется безусловно на всех пресетах

*трудоёмкость: small · риск: medium*

src/Layers/xrRender_R2/r2_rendertarget.cpp:331 создаёт rt_ui в безусловном блоке рядом с rt_Generic_0/1 — 14 745 600 B на 2560x1440, платят все, включая тех, у кого архива совместимости нет. Это ломает контракт R-D («feature off == стоку»). Сделать создание ленивым или гейтить на da_pda3d::available(); добавить строку в пресетную лестницу рядом с hud_shadow_by_preset/smaa_by_preset (xrRender_console.cpp:855-915) и строку VRAM в рендер-замер.

### Проверить четыре звука headlamp/NV и добавить загрузку алиасов

*трудоёмкость: small · риск: low*

pda_show.ltx:23-26 требует weapons\headflash_on/off и weapons\nv_act/nv_deact (в распаковке 62 151 / 60 994 / 58 339 / 55 831 B = 237 315 B). Наличие в DA НЕ проверено: таблицы путей sounds.xdb не текстовые (контрольный grep по detector_draw и generic_pin даёт 0). Проверять либо распаковкой converter.exe, либо FS.exist в рантайме. Отдельно: CWeaponMagazined::Load (WeaponMagazined.cpp:76-77) грузит только snd_draw/snd_holster — алиасы snd_headlamp_*, snd_nv_*, snd_blowout нужно грузить в CPdaAnimatorItem::Load, иначе snd_blowout = items\pda\pda_vibros непроигрываем.

### anm_blowout через eBore + спецслучай холстера из eBore

*трудоёмкость: small · риск: medium · зависит от: S1b*

Override PlayAnimBore на CPdaAnimatorItem, флаг взводится в UpdateCL при пересечении blowout_anim_level по da_pda3d::interference(); принудительный SwitchState(eBore) в обход 20-секундного гейта Weapon.cpp:1264-1276; при IsZoomed() сперва зум-аут (нет мотиона pda_aim_vibros); срез обратно в eIdle при падении условия. Порог НЕ копировать (оригинальные 4.5 — на неограниченном счётчике), брать из [pda3d] в шкале 0..1. ДОБАВИТЬ: оригинал холстерит из eBore через низкоуровневый ActivateActorSlot__CInventory (ActorUtils.pas:2306-2313, комментарий «ActivateActorSlot не сможет скрыть») — проверить, что наш CInventory::Activate из eBore вообще уводит предмет в eHiding, иначе девайс залипнет во время выброса.

### Оффлайн-претфлайт конфига в скрипт упаковки

*трудоёмкость: small · риск: low · зависит от: S4*

R_ASSERT2 в player_hud_motion_container::load (player_hud.cpp:128) стреляет ТОЛЬКО по m_base_name — первому полю строки, и только после перебора name, name1..name8. Второе поле (мотион предмета) вообще не валидируется при загрузке, оно резолвится лениво через ID_Cycle_Safe с откатом на "idle" (player_hud.cpp:481-493). Значит проверять надо только первые токены всех anm_-строк против списка pda_* в omf — это дёшево автоматизируется и должно жить в tools/package.

### Счётный QA-зонд на ui_pda / ui_pda_hide

*трудоёмкость: trivial · риск: low*

operator== для dlgItem включает поле enabled (UIDialogHolder.cpp:18), RemoveDialogToRender не стирает запись, а гасит её (:202-220), поэтому Add в том же кадре не находит совпадения, пушит дубль и вызывает Show(true) второй раз — две инфопорции в m_known_info_registry, т.е. в сейв навсегда. Реконсилятор делает такой сценарий достижимым (M1 гасит, ветвь позже поднимает). Msg в обеих точках CUIPdaWnd::Show (UIPdaWnd.cpp:250-278), десять циклов поднять/убрать, точные счётчики. Правку operator== (убрать enabled) рассматривать отдельным коммитом после проверки других пользователей Add/RemoveDialogToRender.

## Открытые вопросы

* Третий режим ввода (lookout/курсор) — берём или режем? Он гейтит 72 из 120 ключей матрицы (весь hip-fire джойстик), но визуально почти полностью дублирует aim-грид, который наша focused-стадия уже тянет. Решать ДО написания компоновщика имени (S4), потому что от этого зависит, нужна ли в нём hip-ветка джойстика. Рекомендация: резать, оставить только aim-грид.
* Клавиша переключения режима курсора, если третий режим берём. kWPN_ZOOM_ALTER у нас нет вообще (проверено по всей таблице xr_level_controller.cpp:30-120). kWPN_FUNC свободен на нашей ветке наследования и уже везде проплетён, но в меню настроек он подписан как переключение подствольника, и надо сперва прогрепать скриптовый слой DA — itms_manager владеет клавишами, а двойной обработчик мы уже ловили в 3fa251d57. Альтернатива — четыре файла на честный новый экшен.
* Работает ли 3D-PDA в no-weapon зонах вообще? 2D-диалог работал всегда. Оригинал открывает слот принудительно (restore_weapon -> поднять -> hide_weapon на разборке, action_animators.script:13-26/66-75), что меняет собственный контракт DA по этим зонам. Отказ активации с откатом в 2D контракт сохраняет. Это дизайнерское решение, не техническое.
* sprint_allowed: наша строка в dead_air_x64_pda3d_items.ltx:23 не была самостоятельным решением — [anm_base] и так ставит false (items_animations.ltx:109), т.е. так ведут себя ВСЕ анимационные предметы DA. Вопрос не «был ли это placeholder», а «хотим ли мы разойтись с остальным DA ради двух мотионов». Удаление строки ничего не даст, нужно явное true.
* Переименовываем ли секции с ганслингеровских имён (pda_show_animator / pda_show_animator_hud) в da_pda3d_animator / *_hud? Плюс: пользователь, поставивший ганслингер-производный пак, получает молчаливый last-writer-wins мердж вместо конфликта. Минус: ломается сейв, сделанный с поднятым девайсом (меняется секция спавна). Если делать — то в одном релизе со сметателем сейвов, иначе не делать вовсе.
* m_affects.y: оригинал рерандомит на каждый бинд константы, т.е. на отрисовку (r_constants.pas:119); у нас значение держится 0.125 с (da_pda3d.cpp:320-324). Per-frame — это 1:1, но читается как белый шум; держанное — как аналоговый глитч. Решить и записать, а не оставлять как есть по умолчанию.
* Какой источник интерференции побеждает во время выброса на глушащем уровне (l10_red_forest / l13_generators)? max() по всему — простой ответ, но он упирает выброс в уровень глушения ровно там, где эскалация была бы драматичнее всего. Аддитивно-с-клампом — альтернатива.
* inventory_show.ltx — это твин pda_show.ltx с пятью дельтами, гоняющий ТОТ ЖЕ dev_pda_hud.ogf: ганслингер переиспользовал железку PDA под свой 3D-инвентарь. Записать как follow-on возможность в docs/dead-air/pda-3d-port-plan.md или считать вне области целиком?
