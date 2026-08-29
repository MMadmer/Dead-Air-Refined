# 3D PDA для Dead Air x64 — анализ и план внедрения

**Статус:** только анализ и проектирование. Ни одного файла не тронуто.
**Что изменилось:** найден и распакован **оригинал Gunslinger**. Он переворачивает базу порта. Два вывода из предыдущих отчётов теперь официально **неверны** — правки ниже, по именам.

---

## 0. Две отменённые ошибки (читать первым)

**(a) «Диалект шейдеров расходится полностью, ничего не копируется» — НЕВЕРНО.**
Это было в `asset_bom.md` (Critical finding №3). Вывод сделан по IX-Ray'евской *копии* (`gamedata/shaders/d3d11/*.ps.hlsl` + `*.lua`). Оригинал Gunslinger лежит в **нашем ровно диалекте**: `shaders/r3/models_pda.s` + `shaders/r3/model_pda_screen.ps`, `#include "common.h"`. Вершинный вход `model_def_lplanes` **есть у нас**:

* исходник — `D:\Games\Dead Air\_analysis\base_configs\shaders\r3\model_def_lplanes.vs`
* скомпилированные варианты в кэше — `D:\Games\Dead Air\appdata\shaders_cache_oxr\r4\model_def_lplanes_0.vs`, `..._1.vs`

Lua-API в нашем `ResourceManager_Scripting.cpp` покрывает **все** вызовы из `models_pda.s`: `sorting` (:268), `emissive` (:269), `distort` (:270), `fog` (:272), `zb` (:273), `blend` (:274), `aref` (:275), плюс `dx10texture`/`dx10sampler`. То есть `.s` копируется почти байт-в-байт.

**(b) «Эффект аномальных помех выпилен, писать с нуля» — НЕВЕРНО.**
IX-Ray его закомментил в 8326606d0. Оригинал — целый. `model_pda_screen.ps` реализует помехи, чёрный экран **и** загрузочный экран, всё через один `uniform float4 m_affects`. Это буквально та data-driven модель состояний, которую просил юзер:

| канал | смысл | пороги |
|---|---|---|
| `m_affects.x` | уровень помех 0..1 | `>0.09` рвущаяся строка · `>0.15` бегущая волна · `>0.27` вместо UI показывается собственная «мёртвая» текстура экрана · `>0.38` горизонтальный сдвиг всей картинки · `>0.41` **полный блэкаут** |
| `m_affects.y` | 0..1 фаза/рандом-драйвер | амплитуда искажений и величина сдвига |
| `m_affects.a` | флаг «идёт загрузка» | при `a>0 && x>=0.08` рисуется `ui\ui_pda_loadscreen` |

Дохлая батарейка = `x` за 0.41. Аномалия = `x` от близости. Бутающийся КПК = ветка `a`. **Один биндер, три фичи.** Ничего не хардкодим.

**Вывод по базе порта: берём оригинал Gunslinger** (шейдеры + ассеты + hud-секция). C++ IX-Ray используем **только как референс** стейт-машины аниматора и роутинга ввода. Их шейдерный дериватив, их emissive-путь, их `rt_ui_pda`-нейминг и выпиленные помехи — всё это деградация относительно оригинала.

Что из IX-Ray-находок **остаётся в силе** и обязательно к применению:
* они **не зовут `UI().RenderFont()`** внутри бинда RT → у них на модели нет ни одной буквы. У нас есть готовый правильный рецепт — `CRender::RenderMenu()`.
* frame-guard для подавления обычного 2D-прохода.
* двухстадийная модель ввода (в руках / прицелился).
* прокидывание `screen_kx` в курсор/карту/табы.

---

## 1. Как устроен 3D PDA в IX-Ray (честная анатомия)

### 1.1 Три заголовочных факта

**(1) Модели, анимаций, звуков и самой hud-секции в репозитории IX-Ray НЕТ.**
`game_global.ltx` ссылается на `[pda] pda_animator = pda_show_animator_hud`, но секция `pda_show_animator_hud` не определена **ни на одном рефе**. `git grep` даёт только ссылки, ноль определений. Ни одного `.ogf`, ни одного `.omf` с `pda_*` моушенами, ни одного звука. IX-Ray везёт **только движковый код, шейдеры и две строки конфига**. Всё остальное — из ассет-пака Gunslinger. Их же коммит с шейдерами так и называется: «Add shaders from Gunslinger».

**(2) Ветка `3d-pda-favorite-weapon-preview-fix` — старый снапшот.**
Зрелая реализация живёт на `origin/develop`: joystick-bone callback, кости пальцев, обработка мыши, `screen_kx`, и **другой контракт экранного шейдера** (там как раз восстановлен полный gunslinger'овский `m_affects`-путь с тремя текстурами). Базу надо выбирать осознанно — и мы выбираем не их вовсе (см. §1.4).

**(3) Диалект — см. §0(a). Их вариант не копируется, оригинал копируется.**

### 1.2 Что такое их 3D PDA технически

Не `CHudItem`, не предмет инвентаря, не `attachable_hud_item`. Это отдельная стейт-машина `CHudPdaAnimator : CHudStateAnimator : CHudAnimatorBase` плюс лёгкий носитель `animator_item` внутри `player_hud`, занимающий **третий** слот рядом с `m_attached_items[0]` и `[1]`. Взаимоисключение с оружием/детектором — по политике (принудительный холстер), а не по структуре.

Ключевые точки:
* владение и тик — `CActor::m_hud_animator` (`Actor.cpp:179` создание, `:220` удаление, `:1749-1751` `Update()` из `UpdateCL`).
* руки — обычная модель `player_hud::m_model`; `anm_*` ключи секции грузятся тем же `player_hud_motion_container::load()`, что и у оружия.
* привязка — `calc_transform(0, m_attach_offset, m_item_transform)`, т.е. к нулевой кости рук. Никакого именованного attach-bone.
* рендер модели — `::Render->add_Visual(..., true)`, HUD-путь. Отсюда «дождь и свет применяются бесплатно».

### 1.3 Как UI попадает на модель

Регистрируется RT `$user$ui_pda` (`r__types.h:82`, создаётся в `r4_rendertarget.cpp:514`, `R8G8B8A8_UNORM`, размер бэкбуфера). В самом верху `CRender::Render()` (`r4_R_render.cpp:547-552`):

```cpp
Target->u_setrt(Target->rt_ui_pda, 0, 0);
rmNormal();
g_pIGameActor->RenderItemUI();
```

`CActor::RenderItemUI()` (`Actor.cpp:2500`) рисует `CUIPdaWnd::Draw()` и курсор. Двойной отрисовки нет за счёт frame-guard'ов: `static u32 pda_render_frame` в `CUIPdaWnd::Draw()` и `last_render_frame` в `CUICursor::OnRender()` — кто первый в кадре, тот и выиграл.

**Экран на модели ничем в C++ не ищется.** Ни кости, ни имени меша, ни UV-конвенции. Вся связь — имя шейдера в OGF-сабсете плюс имя RT в `.lua`. Чистая data-конвенция.

**Их фатальная дыра:** `RenderItemUI` **никогда не зовёт `UI().RenderFont()`**. Проверено: `git grep -n RenderFont origin/develop -- src` даёт ровно три места — `HUDManager.cpp:186`, `MainMenu.cpp:455`, `:472`. Ни одного внутри RT-бинда. Шрифты у них батчатся так же, как у нас (`GameFont.cpp:470` `strings.push_back`, `:414` `OnRender`, `:421` `strings.clear`). Значит все глифы, поставленные в очередь при связанном `rt_ui_pda`, выливаются позже в бэкбуфер, а frame-guard мешает 2D-проходу их перерисовать. Итог: на модели фон, иконки и карта — а весь текст размазан плоско поверх мира. Коммит-фикс так и называется, «temp».

### 1.4 Почему база — Gunslinger, а не IX-Ray

`models_pda.s` оригинала (весь файл, 543 байта):

```lua
function normal (shader, t_base, t_second, t_detail)
    shader:begin ("model_def_lplanes","model_pda_screen")
      : fog(true) : zb(true,false)
      : blend(true,blend.srcalpha,blend.invsrcalpha)
      : aref(true,0) : sorting(2,true) : distort(true)
    shader:dx10texture("s_base", t_base)              -- собственная текстура экрана модели
    shader:dx10texture("s_vp2",  "$user$ui")          -- RT с UI
    shader:dx10texture("s_load", "ui\\ui_pda_loadscreen")
    shader:dx10sampler("smp_base")
end
```

Обрати внимание: **только `normal`, никакого `l_special`**. В нашем движке (`ResourceManager_Scripting.cpp:334-410`) это значит E[0]/E[1] скомпилированы, E[2..4] пустые. А `sorting(2,true)` → `flags.bStrictB2F` → в `r__dsgraph_build.cpp:139-143` HUD-визуал уходит в **`mapHUDSorted`** → `render_sorted()` (`r__dsgraph_render.cpp:582-585`, обёрнут в `hud_transform_helper`) → вызывается из `CRender::render_forward()` (`r2_R_render.cpp:504`) внутри `phase_combine`, при связанном `rt_Generic_0_r`.

То есть **экран в оригинале — форвардная, альфа-блендящаяся, self-lit поверхность поверх освещённого кадра.** Не emissive-запись в аккумулятор, как у IX-Ray. И это правильно: наш `combine_1.ps` делает `C = D * light`, а их — `Color = Occ*Ambient + Light`. IX-Ray биндит UI-RT **и как albedo, и как emissive**; в нашей математике это даст `UI × (detonemap(UI) + hemi)` — картинку UI в квадрате. Серый 0.5 станет 0.25. Это не тюнинг, это неверное уравнение. Форвардный путь оригинала обходит проблему целиком.

### 1.5 Таблица ассетов: что где лежит и берётся ли как есть

Оригинал найден в репозиториях `github.com/gunslingermod` — CAB-обёрнутые X-Ray `.db` под `mod_data/patches/`. Рецепт распаковки проверен: `expand.exe <файл> -F:* <dir>`, затем `converter.exe -unpack -xdb <inner.db> -dir <out>` нашим же AXRToolset (`D:\Games\Dead Air\tools\AXRToolset\bin\converter.exe`).

Целевой корень для всего — `packaging/dead-air-x64/compatibility/gamedata/`, он пакуется целиком скриптом `tools/package/dead_air_x64_compatibility_archive.ps1` в `xtra_dead_air_x64.xdb0`. Список файлов править не надо, кладёшь в поддерево — оно едет.

| ассет | источник | есть? | целевой путь в нашем оверлее | как есть? |
|---|---|---|---|---|
| `models_pda.s` | Gunslinger, уже распакован в `_analysis/gunslinger-3dpda/shaders_r3/` | **ДА** | `.../shaders/r3/models_pda.s` | **почти** — убрать `distort(true)` (без `l_special` он у нас мёртв и рискует попасть под `if (E2 && !(E2->flags.bDistort))` в `r__dsgraph_render.cpp:687`); `$user$ui` придётся создать |
| `model_pda_screen.ps` | там же | **ДА** | `.../shaders/r3/model_pda_screen.ps` | **почти** — см. §3.3, одна обязательная правка сигнатуры |
| `ui\ui_pda_loadscreen` | `guns_data_textures_common.db`, 38 МБ | не вытянут | `.../textures/ui/` | да |
| `dev_pda_hud.ogf` | `guns_data_meshes.db`, 16 МБ | не вытянут | `.../meshes/dynamics/devices/dev_pda/` | **проверить сабсеты** (§8) |
| руки `.omf` с `pda_*` | `guns_data_hands.db` + `..._new_part1/2.db`, 46+77+58 МБ | не вытянут | `.../meshes/dynamics/weapons/hud_hands_animation/` | **нет** — см. §7 R1, у нас нет `PlayerHudOmfAdditional` |
| `items\pda\pda_draw`, `items\pda\pda_vibros` | `guns_data_sounds.db`, 19 МБ | не вытянут | `.../sounds/items/pda/` | да |
| текстуры корпуса КПК | `guns_data_textures_common.db` | не вытянут | `.../textures/dynamics/devices/dev_pda/` | да |
| `[pda_show_animator_hud]` | `_analysis/gunslinger-3dpda/configs/pda_show.ltx` | **ДА, целиком** | `.../configs/misc/items/items_pda_animator.ltx` | **нет** — трим и переименование ключей, §6.3 |
| `[base_animator_hud]` / `[hud_base]` | `configs/base_animator.ltx`, `configs/defines.ltx` | **ДА** | — | нет, у нас свой `[anm_base_hud]` |
| `$user$ui` (RT) | движок | **НЕТ у нас** (`grep 'user$ui' src/Layers/` → пусто) | новый `ref_rt` | пишем |
| `m_affects` (биндер) | движок | **НЕТ у нас** | `r2.cpp` рядом с `da_*` | пишем |

---

## 2. Что у нас сейчас

### 2.1 2D PDA

`CPda` (`src/xrGame/PDA.h:16`) — это `CInventoryItemObject` + `Feel::Touch`. Не `CHudItem`, `cast_hud_item()` возвращает 0, HUD-модели нет, анимаций нет. Сериализует ровно `inherited::save` + `m_sFullName` (`PDA.cpp:179-189`).

`CUIPdaWnd` (`src/xrGame/ui/UIPdaWnd.h:23`) — `final`, `CUIDialogWnd`, создаётся **эагерно** в `CUIGameCustom::Load()` и живёт весь уровень. Парсит `pda.xml`/`pda_16.xml`, всё в виртуальном холсте 1024×768. Живых C++-страниц в DA три: `CUITaskWnd` (в ней же карта), `CUIRankingWnd`, `CUILogsWnd`. `CUIMapWnd`/`CUIFactionWarWnd`/`CUIActorInfoWnd` конструируются и тут же удаляются — их XML в DA не поставляется. Ещё четыре страницы (Relations/Contacts/Encyclopedia/jammed) — чистый Lua через функтор `pda.set_active_subdialog` (`UIPdaWnd.cpp:324-329`), и все четыре **хардкодят** `Frect():set(0,0,1024,768)`.

Открывается PDA в Dead Air **из Lua, не из движка**: `itms_manager.script:350`, `dik_to_bind(key) == 52` (`kACTIVE_JOBS`), проверка заряда `wpn_upd` > 0.05, дальше `ActorMenu.get_pda_menu():ShowDialog(true)`. Движковый путь `kMAP` (`UIGameSP.cpp:122-129`) — вторичный. **Хук обязан жить в `CUIPdaWnd::Show(bool)`**, а не в обработчике клавиш.

`UINoice` (`noice_static`, текстура `ui_pda2_noice`, alpha 70) рисуется последним поверх всего в `CUIPdaWnd::Draw()` — уже сейчас накрывает **весь** PDA, а не только карту. Немодулируем.

### 2.2 Рендер, что важно

* **HUD идёт в тот же G-буфер, что и мир.** `phase_scene_begin(); dsgraph.render_hud(); ...; phase_scene_end()` (`r2_R_render.cpp:376-382`), под `hud_transform_helper` (HUD-проекция + `rmNear`, глубина зажата в `[0, r2_hud_depth_limit=0.02]`, `r2_types.h:66`). Значит корпус КПК получит солнце, динамический свет, `render_hud_shadow()` и дождь **бесплатно**.
* **Дождь по вьюмоделям уже написан.** `rain_patch_normal.ps:134` — ветка `if (position.z < 0.35)` со стекающими каплями, кольцами и «попсами» в view-space. Единственное исключение — glosss-буст режется маской `not_viewmodel = step(0.35, gbd.P.z)` в `rain_apply_normal_gloss.ps:63`. **Эту глобальную маску трогать нельзя** — она специально не даёт оружию зеркалить на вытянутой руке.
* **У нас УЖЕ ЕСТЬ рабочий UI→RT→сэмпл конвейер.** `CRender::RenderMenu()` (`r2_R_render.cpp:31-92`): бинд `rt_Generic_0` → `OnRenderPPUI_main()`; бинд `rt_Generic_1` + `ClearRT(127,127,0,127)` → `OnRenderPPUI_PP()` (канал дисторшна); бинд бэкбуфера → фуллскрин-квад шейдером `s_menu` («distort»). А `CMainMenu::OnRenderPPUI_main()` — это буквально `UI().pp_start(); DoRenderDialogs(); DrawProductVersion(); UI().RenderFont(); UI().pp_stop();`. **Вот правильный рецепт, включая флаш шрифтов внутри бинда.** То, что IX-Ray потерял.
  Оговорка: `UICore::pp_start/pp_stop` (`ui_base.cpp:293-315`) у нас вырожденные — `m_pp_scale_` и `m_2DFrustumPP` численно совпадают с обычными, `g_current_font_scale` ставится в (1,1). Механизм-каркас есть, реального ретаргета нет.
* **Дисторшн для HUD доступен** (поправка к отчётам `ours-hud`/`ours-effects`, где написано «нельзя»): `mapDistort` заполняется в `r__dsgraph_build.cpp:117-121` из `E[4]` **до** ветки `renderable_HUD()` на :138. Проблема не в постановке, а в отрисовке — `render_distort()` (`r__dsgraph_render.cpp:621-626`) не оборачивается в `hud_transform_helper` и крутится под `Device.mProject` в `phase_combine` (`r4_rendertarget_phase_combine.cpp:427-441`). Нам это, впрочем, не понадобится — оригинал делает помехи внутри пиксельного шейдера экрана.
* **`RegisterConstantSetup` — готовый механизм для `m_affects`.** 25 `da_*` биндеров зарегистрированы в `r2.cpp:761-785`. `timers` тоже есть (`Blender_Recorder_StandartBinding.cpp:603`), объявлен в `shaders/r3/shared/common.h` — шейдер оригинала им пользуется и заведётся.
* **Ловушка транспонирования:** `dx11ConstantBuffer::set(Fmatrix)` транспонирует при записи. `Fvector4` — не транспонирует. `m_affects` — float4, так что мимо.

### 2.3 Самое важное: у нас УЖЕ есть механизм «предмет-аниматор»

`D:\Games\Dead Air\_analysis\base_configs\configs\misc\items\items_animations.ltx`:

```ini
[anm_base]
animation_slot = 13
slot           = 12          ; → ANIMATION_SLOT (inventory_space.h:24)
visual         = dynamics\devices\dev_bandage\dev_bandage
hud            = anm_base_hud
class          = WP_BINOC
default_to_ruck = false
inv_grid_x/y   = 5000        ; спрятан от инвентаря
```

И `[anm_base_hud]` с `attach_place_idx = 0`, `hands_position`, `item_position`, `aim_hud_offset_*`. DA уже гоняет это для `animation_clean_mask` и шести `animation_hit_wpn_knife*`.

Рабочий рецепт активации — `dinamic_hud.script:509-534`:

```lua
alife():create("animation_clean_mask", db.actor:position(), ...)
db.actor:activate_slot(13)
-- ... анимация ...
alife():release(alife_object(anm:id()))
db.actor:activate_slot(last_slot)
```

**И Gunslinger сделал ровно то же самое:** его `[pda_show_animator]:base_animator` имеет `class = WP_BINOC`, `slot = 10`, `animation_slot = 13`, `hud = pda_show_animator_hud`. Это не совпадение — это стандартный приём.

Что это даёт даром: `CWeaponBinoculars : CWeaponCustomPistol : CWeaponMagazined : CWeapon : CHudItemObject` — у него **уже** есть `zoom_enabled` (`Weapon.cpp:577`), `PlayAnimAim()` → `anm_idle_aim` (`WeaponMagazined.cpp:1321`), стейт-машина показа/убирания, `render_item_ui/render_item_ui_query` (`WeaponBinoculars.h:34-35`). То есть **двухстадийный ввод «в руках → прицелился» приезжает почти бесплатно из зум-машинерии оружия.**

---

## 3. Разрыв

### 3.1 Берём у Gunslinger (почти как есть)

| что | комментарий |
|---|---|
| `models_pda.s` | минус `distort(true)`, плюс наш `$user$ui` |
| `model_pda_screen.ps` | одна правка сигнатуры (§3.3), опционально — sub-rect UV |
| контракт `m_affects` (пороги 0.09/0.15/0.27/0.38/0.41, канал `a`) | это и есть data-driven модель состояний из ТЗ |
| `.ogf` КПК, руки `.omf`, звуки, `ui_pda_loadscreen` | логистика, рецепт распаковки проверен |
| набор `anm_*` и hud-офсеты из `[pda_show_animator_hud]` | как **референс тюнинга**, не как готовый файл (§6.3) |
| приём «предмет-аниматор в animation slot, `class = WP_BINOC`» | у нас это `[anm_base]`, уже в шиппинге |

### 3.2 Берём у IX-Ray (только идеи, не код)

| что | комментарий |
|---|---|
| frame-guard в `CUIPdaWnd::Draw()` / `CUICursor::OnRender()` | но флаг обязан значить **«PDA был растеризован в RT в этом кадре»**, а не «3D-режим включён». Иначе `ShowDialog(true)` из `itms_manager` даст невидимый КПК. У них неявный счётчик кадров случайно даёт корректный фоллбэк в 2D — мы это свойство сохраняем осознанно |
| двухстадийный ввод (`Enable(false)` в руках → `Enable(true)` при прицеле) | вся суть `IR_process()`-гейта |
| прокидывание `screen_kx` | курсор, `CUIMapWnd`, `map_location`, ширина таб-кнопок |
| холстер оружия/детектора при показе и восстановление слота | политика, не структура |

### 3.3 Пишем сами

**Обязательная правка `model_pda_screen.ps` — верифицировано.** Он объявляет:

```hlsl
struct v2p {
    float2 tc0: TEXCOORD0;
    float3 tc1: TEXCOORD1;   // <-- НАШ model_def_lplanes.vs ЭТОГО НЕ ВЫДАЁТ
    float4 c0:  COLOR0;
};
```

Наш `model_def_lplanes.vs` возвращает:

```hlsl
struct vf { float2 tc0 : TEXCOORD0; float4 c0 : COLOR0; float4 hpos : SV_Position; };
```

В DX11 вход PS обязан быть подмножеством выхода VS → **линковка упадёт**. `tc1` в теле шейдера не используется ни разу. Фикс: удалить строку. Одна строка, но без неё ничего не соберётся.

Плюс полезное наблюдение: `c0` у `model_def_lplanes.vs` — это `abs(dot(dir_v, norm_v))`, facing-fade. Готовый множитель, чтобы экран гас под острым углом. Оригинал его игнорирует; мы можем взять.

Остальное своими руками:

| что | почему |
|---|---|
| RT `$user$ui` | у нас его нет; `grep 'user$ui' src/Layers/` пусто |
| проход UI→RT в `CRender::BeforeWorldRender()` | пустой хук в `r2_R_render.cpp:522`, зовётся из `CLevel::OnRender` **до** `Calculate()/Render()` |
| **`UI().RenderFont()` внутри бинда** | иначе повторим баг IX-Ray один-в-один |
| биндер `m_affects` | `RegisterConstantSetup("m_affects", ...)` в `r2.cpp` |
| биндер sub-rect экрана | чтобы UV не были зашиты в меш и мод-раскладки не ломались |
| источник заряда и уровня помех | опциональные Lua-функторы + Refined-овская ltx. **Ни `wpn_upd`, ни имён уровней в C++** |
| тонкий C++-класс `CPdaAnimatorItem : CWeaponBinoculars` | держит `CUIPdaWnd` открытым, дёргает `Enable()` на зуме, кормит `m_affects` |
| Lua compat-скрипт активации/деактивации | по образцу `dinamic_hud.clean_animation()` |
| throttle RT-прохода | полная частота при фокусе UI, ~15 Гц «в руках» |

### 3.4 Не подходит — делаем иначе

| их решение | почему не годится | наше |
|---|---|---|
| `EngineExternal()[Enable3DPDA]` | у нас нет `CEngineExternal` вообще, и по `graphics-features-policy` дефолтно-выключенные опт-ины запрещены | фича едет включённой; фоллбэк — **по capability** (нет RT / нет секции / нет модели → 2D), не по пресету и не по галочке |
| `IGame_Actor::RenderItemUI` | у нас нет `IGame_Actor`/`g_pIGameActor` | новый виртуал на `CCustomHUD`, до которого рендер уже достаёт через `g_pGameLevel->pHUD` |
| `deffer_model` + `deffer_base` + `:emissive(true)` | наш комбайн **умножает** аккумулятор на albedo (`C = D*light` в `combine_1.ps`), их — складывает. UI-RT в обеих ролях = UI в квадрате | форвардный `sorting(2,true)` из оригинала |
| весь стек `CHudAnimatorManager`/`CHudStateAnimator`/`animator_item` (~1500 строк) | тащит бэкпак, ожоги, свитчи девайсов; у нас ноль этой инфраструктуры | `[anm_base]`-предмет в animation slot + тонкий наследник `CWeaponBinoculars` |
| `CInventoryOwner::GetPDA()` для опознания КПК | `InventoryOwner.cpp:220` — сырой C-каст на `PDA_SLOT` без проверок. Четыре шипящихся секции с `class = D_PDA` и `slot = 7`: `device_pda`, `dev_flash_1`, `dev_flash_2`, `pri_a25_explosive_charge_item`. Взрывчатка в руке — не то, чего мы хотим | Refined-овская ltx со списком секций + требование наличия hud-ключа |
| перевод `CPda` в `CHudItem` | `CHudItem::Load` (`HudItem.cpp:126-129`) — жёсткий `r_u32(section, "animation_slot")`, FATAL. Ударит по всем четырём секциям выше | `CPda` **не трогаем вообще** |
| `screen_kx` как ручная константа | кривое значение молча ставит юзерские метки карты не туда | вычислять из реального аспекта экранного квада, константу оставить как override |
| ray→UV пикинг | у нас его негде взять, и он молча меняет то, что возвращают `GetCursorPosition`/`GetAbsoluteRect`/`GetMouseX/Y`/`FitInRect` внутри чужого Lua | обычный 2D-курсор 1:1 с холстом |
| `UITimeDilator::Pda` при показе диалога | `UIGameSP.cpp:257-265` включает slow-mo по указательному сравнению с `PdaMenu`. Держим КПК в руках минуту → минута замедления, в том числе в бою | предикат «эта PDA-сессия в UI-фокусе», проверяемый на месте |
| pttLIT / `CUIHudUI` | **четыре** независимых стоппера, а не два: (1) шрифты только `FVF::F_TL`, мировой ветки нет; (2) `PushScissor`/`PopScissor` — no-op под pttLIT (`ui_base.cpp:147, 182`); (3) `hud3d.ps` вообще не имеет COLOR-семантики → все `SetColor`, `text_color`, `_light_anim` теряются; (4) каждому виджету пришлось бы прописать `shader="hud\p3d"` в XML | закрыто окончательно |

---

## 4. Архитектура целевого решения

### 4.1 Общая схема

```
Актёр держит КПК
   │
   ├─ ПРЕДМЕТ: [pda_show_animator] : anm_base   (class = WP_BINOC, slot = 12 → ANIMATION_SLOT)
   │     hud = pda_show_animator_hud
   │       item_visual = dynamics\devices\dev_pda\dev_pda_hud.ogf
   │       attach_place_idx = 0
   │       zoom_enabled = true          ← двухстадийность из зум-машинерии оружия
   │     C++: CPdaAnimatorItem : CWeaponBinoculars  (тонкий, ~250 строк)
   │
   ├─ МОДЕЛЬ: минимум два сабсета
   │     корпус+стекло  → обычный model-шейдер → mapHUD → G-буфер
   │                      → солнце, динсвет, hud_shadow, дождь-по-вьюмодели  ✔ ТЗ
   │     экран          → шейдер "models\pda"   → mapHUDSorted → render_forward
   │                      → self-lit форвард-оверлей поверх освещённого корпуса
   │
   ├─ ЭКРАН: sample($user$ui) через model_pda_screen.ps, модулировано m_affects
   │
   └─ UI→RT: CRender::BeforeWorldRender()
         u_setrt(rt_ui) → ClearRT(black) → CCustomHUD::RenderPdaScreenUI()
           { CUIPdaWnd::Draw(); DrawHint(); [курсор]; UI().RenderFont(); }
         → restore + rmNormal()
```

### 4.2 Куда встаёт UI→RT проход и почему именно туда

**`CRender::BeforeWorldRender()`**, пустой хук в `r2_R_render.cpp:522`. Три аргумента:

1. **Вне окна параллельных контекстов.** `dxUIRender::StartPrimitive` лочит общий динамический VB `RImplementation.Vertex` (4 МБ, `R_DStreams.cpp:12`). Тот же стрим лочат шрифты, партиклы, дождь, детали, вольюм-марки. `BeforeWorldRender` зовётся из `CLevel::OnRender` **до** `IGame_Level::OnRender` → `Calculate(); Render();`, т.е. до `r_main.run()`. Вариант «внутри `Render()` после `r_main.sync()`» (место IX-Ray) попадает в окно, где живы контексты `r_sun`/`r_rain` — тот же класс гонки, что мы уже ловили на партиклах.
2. **Не глохнет под меню.** `CRender::Render()` рано выходит при `pMainMenu->CanSkipSceneRendering()` (`r2_R_render.cpp:108-118`). Проход, живущий внутри `Render()`, замерзает при Esc и, что хуже, даёт чёрный экран на скриншоте сейва (единственное исключение — флаг `flGameSaveScreenshot`).
3. **`g_bRendering` уже true**, RCache простаивает, ни один сценовый таргет не связан.

Обязательная гигиена: `u_setrt` **не сбрасывает вьюпорт** — `rmNormal(RCache)` читает `Target->get_width(cmd_list)`, который пишет последний `u_setrt`. Значит `rmNormal` на входе **и** на выходе. Это ровно те две строки, которые IX-Ray дописал в 8326606d0 (у них — из-за апскейла; у нас апскейла нет вообще, `render_scale` в `src/Layers/*` не встречается, так что причина у нас другая, а строки те же).

Ждём предупреждение D3D11 о SRV/RTV-конфликте: `rt_ui` в кадре N-1 остался связан как `s_vp2`. Ловится `-dxdebug`, лечится явным анбиндом слота.

### 4.3 Как ретаргетится UI

**Никак. RT ровно `Device.dwWidth × Device.dwHeight`, `D3DFMT_A8R8G8B8`, `SampleCount = 1`.**

Это не лень, это единственный вариант без каскада правок. К разрешению устройства прибиты **четыре** независимые вещи:

* `cl_screen_res::setup` (`Blender_Recorder_StandartBinding.cpp:321-328`) отдаёт `Device.dwWidth/dwHeight`, а `stub_notransform_t.vs` через `screen_res.zw` переводит пиксели в клип-спейс — через это идёт **каждый** UI-квад с шейдером `hud\default`;
* `UICore::PushScissor` (`ui_base.cpp:145-180`) отдаёт бэкенду device-пиксели;
* `CFontManager::GetFontTexName` (`FontManager.cpp:43-64`) выбирает атлас глифов (`texture800`/`texture`/`texture1600`) по `Device.dwHeight`;
* `dxFontRender::OnRender` эмитит `float X = float(iFloor(PS.x))` в сырых device-пикселях.

Цена: 8.29 МБ @1080p, 14.75 МБ @1440p, 33.18 МБ @2160p. Это **честная плата за то, что весь виджет-трей, все шрифты, все скиссоры и все четыре Lua-вкладки работают без единой правки.**

**UV-подрект вместо зашитых UV.** Лицо девайса в `pda_16.xml` — `background_static x=102 width=819`, т.е. u ∈ [0.0996, 0.8994]; 20% RT — мёртвая зона. А `pda.xml` (4:3) кладёт фон на весь холст 0,0,1024×768, и `UICore::get_xml_name` переключает файлы по аспекту. Значит зашивать UV в меш нельзя — сломается на 4:3 и на любом моде со своим `pda*.xml`.

Решение: биндер `da_pda_screen_rect` (float4), вычисленный из загруженного рект-а `<main>`/`background_static`, шейдер делает `tc = rect.xy + I.tc0 * rect.zw`. Данные, не геометрия. Это улучшение и над IX-Ray, и над оригиналом.

**Throttle.** Полная частота — только когда UI в фокусе (курсор живой). «Просто в руках» — 10-15 Гц. Безопасно: RT персистентный, пропущенный кадр оставляет прошлую картинку; при пропуске `CUIPdaWnd::Draw()` в очередь шрифтов ничего не кладётся, значит утечки текста в основной кадр нет. Общий dirty-flag построить нельзя (часы переформатируются каждый кадр в `CUIPdaWnd::Update()`, `_light_anim`-мигалки крутятся вечно, у карты анимированный планировщик рецентровки), но на 15 Гц это неразличимо, а интерактива в этом состоянии нет.

### 4.4 Свет и дождь на экране — что именно происходит

Разделение по сабсетам — ключевое решение:

* **Корпус + стекло** — обычный deferred model-шейдер. Идёт в `mapHUD` → `render_hud()` внутри `phase_scene_begin/end`. Получает: солнце, все динамические источники, `render_hud_shadow()` (пресет Extreme), туман, тонмап, зерно. И **дождь**: `rain_patch_normal.ps` ветка `position.z < 0.35` рисует стекающие капли и кольца в view-space. Ровно то, что просил юзер: «как любому предмету в руках».
* **Экран** — форвардный self-lit оверлей (`sorting(2,true)`, `blend(srcalpha,invsrcalpha)`, `zb(true,false)`). Не освещается, не мокнет, не затеняется — и это **правильно**: подсвеченный LCD не должен чернеть ночью и не должен выцветать в полдень. Плюс это обходит ловушку «UI в квадрате» из §1.4.

Хочешь капли **на стекле** — стекло делается частью корпусного сабсета (deferred, высокий gloss), экранный квад копланарно поверх него с `zb(true,false)`. Тогда дождь бьёт по стеклу через G-буфер, а UI светится сверху. Глобальную маску `not_viewmodel` не трогаем ни при каких условиях.

Ночная читаемость и дневное выцветание рулятся `c0` (facing-fade) и множителем яркости в `m_affects.z` — свободный канал.

### 4.5 Ввод: обычный 2D-курсор, не ray→UV

**Решение: обычный 2D-курсор.** Аргументы:

1. Холст **и есть** RT, 1:1. Координаты курсора равны UV по построению. Ноль математики.
2. Всё курсор-зависимое продолжает работать без правок: `fit_in_rect` (`UIWindow.cpp:572-614`, с зашитым `cursor_height = 43.0f`), drag-pan карты (`UIMapWnd.cpp:505,518`), `ActivatePropertiesBox` (`:585-618`), и четыре Lua-вкладки с сырым `GetCursorPosition()` (`ui_pda_relations_tab.script:46,151,396,404,549`; `ui_pda_contacts_tab.script:24,82,102`; `ui_pda_encyclopedia_tab.script:167`; `ui_pda_disabled_tab.script:13`).
3. Ray→UV **молча** меняет то, что возвращают `GetCursorPosition`, `FitInRect`, `GetAbsoluteRect`, `IsCursorOverWindow`, `GetMouseX/Y` внутри Lua, которое мы не контролируем. API компилируется, числа врут. Худший вид поломки.

**Двухстадийная модель (обязательна, не полировка):**

| стадия | как | что живо |
|---|---|---|
| «в руках» | `CUIPdaWnd` показан, но `Enable(false)` → `CUIDialogWnd::IR_process()` false → все `CDialogHolder::IR_UIOn*` сразу выходят | полное управление игроком, курсора нет, экран — глянцевый индикатор, крутятся направленные `pda_idle_up/down/left/right` |
| «прицелился» | ПКМ → `anm_idle_aim_start` → `Enable(true)` → `hud_fov_zoom_factor` подтягивает экран к лицу | курсор живой, UI ест мышь, WASD продолжают идти к актёру (`StopAnyMove()` у нас уже false, `UIPdaWnd.h:89`) |

**Почему без зума нельзя — арифметика.** Проекция HUD: `psHUD_FOV = hud_fov * 100 / (Device.fFOV * healthFactor)` (`ActorCameras.cpp:347-349`), затем `build_projection(deg2rad(psHUD_FOV * Device.fFOV), ...)` (`r__dsgraph_render.cpp:455`) — `Device.fFOV` сокращается, остаётся ~37.5° по вертикали при `psHUD_FOV_def = 0.375f`. Видимая высота на дистанции d = `2·tan(18.75°)·d = 0.679·d`. Экран 7.5 см на 30 см от глаза = 37% высоты кадра ≈ 400 px @1080p. Контент PDA — 683 холст-единицы (`noice_static height=683`), масштаб 0.586 → строка `letterica16` ≈ **9-10 device-пикселей**. Сейчас в 2D она 16·(1080/768) = 22.5 px. То есть в бедре текст читается как сегодняшний PDA на 470p — никак.

Тюнинг Gunslinger: `hud_fov_factor = 0.9`, `hud_fov_zoom_factor = 0.29`. Отношение 0.9/0.29 ≈ **3.1× линейного увеличения** → те же ~10 px становятся ~31 px, лучше сегодняшних 22.5. Вот зачем в оригинале целый параллельный набор `pda_aim_*`.

*(Формулу `psHUD_FOV` для предмета-аниматора — вывод по коду оружия; у `CWeaponBinoculars` путь свой, проверить на стенде.)*

Дополнительно к семплеру: биндить **`smp_rtlinear`** (CLAMP/LINEAR/no-mip), а не `smp_base` (WRAP + анизотропия, `Blender_Recorder_R3.cpp:101-107`). На 1-мипном RT при минификации ~2.4× анизотропия без мип-цепочки — гарантированный шиммер. IX-Ray на это наступал: 2e0874614 «disabled because flickering of model_pda_screen», через 2.5 часа реверт без объяснений.

### 4.6 Чёрный экран и помехи как состояния данных

Оба состояния — **один float4**, никакого хардкода:

```
m_affects.x = clamp(max(interference, discharge_ramp), 0, 1)
m_affects.y = frac(t * rate)              // фаза/рандом
m_affects.z = brightness / резерв
m_affects.w = boot_flag                   // > 0 пока идёт загрузка
```

Пороги (0.09/0.15/0.27/0.38/0.41) — **в ltx**, не в C++. Дефолты — из оригинала.

Источники, оба опциональные и с безопасным дефолтом:

* **Заряд.** Опциональный Lua-функтор `pda.get_power()` → 0..1, дефолт 1.0 при отсутствии. Движок **не имеет права** знать про `wpn_upd` — это секция чужого мода (`itms_manager.script:330,344,355`; наш `dead_air_x64_torch_battery.script` читает то же). Compat-скрипт Refined отдаёт значение из `axr_battery.getBatteryCondition()` там, где оно есть.
* **Помехи.** Опциональный `pda.get_interference()` → 0..1, дефолт 0. Плюс наша ltx-таблица уровней/зон с порогами — по образцу `gamedata/configs/dead_air_x64_mode_exclusive.ltx`, где неизвестные записи молча пропускаются. Список уровней в C++ **не переезжает** (правило `no-hardcoding-foreign-mods`, юзер откатывал этот класс правок дважды).

Отдельно: на этой сборке **живого механизма помех, который можно «расширить на весь девайс», нет**. `axr_battery.is_jammed()` — хардкод-таблица `{l10_red_forest, l13_generators}` в базовом `pda.script`, а установленный DAR2 (`database/xtra_dar2.xdb0`) везёт **свой** `pda.script`, где `local disabled = axr_battery.is_jammed()` вычисляется и **никогда не используется** — все ветки `ui_pda_disabled_tab` удалены. Так что мы не «переносим эффект на весь КПК», мы **пишем его впервые**, взяв готовую математику из `model_pda_screen.ps`. Это надо сказать юзеру прямо.

Ещё: `UINoice` (2D-шум поверх окна) при 3D-пути **попадёт в RT** и наложится на шейдерный шум. Либо гасим его, когда идёт RT-проход, либо оставляем как базовый «плёночный» слой, а шейдер даёт только аномальную составляющую. Решение — на стенде, глазами.

### 4.7 Как выживает 2D-фоллбэк

**Не пресетом и не галочкой.** 3D-PDA едет включённым.

* `CUIPdaWnd` **обязан** остаться эагерно создаваемым в `CUIGameCustom::Load()`. `CActor::save` (`Actor_Network.cpp:1429-1444`) читает четыре байта фильтров карты живьём из `GetPdaMenu().pUITaskWnd`, а `GetPdaMenu()` — это `return *PdaMenu;` (`UIGameCustom.h:103`) по сырому указателю, который `UnLoad()` занулил. Ленивое создание = не «сброс настроек», а **разыменование nullptr при сохранении**.
* Фоллбэк — **по capability**: нет RT / нет секции `[pda_show_animator]` / нет модели / не грузятся моушены → предмет-аниматор просто не активируется, `CUIPdaWnd` рисуется на экран как сегодня.
* Механика подавления: frame-guard, значение которого — **«PDA растеризован в RT в этом кадре»**, выставляется самим RT-проходом. Кадр без RT-прохода → обычная 2D-отрисовка. Инвариант, на который нужен тест.
* Что **можно** класть на пресетную лесенку (`graphics-features-policy` это разрешает — она про качество, не про существование фичи): размер RT, наличие мип-цепочки, частота throttle. Это стоимость, а не наличие.

---

## 5. План внедрения по этапам

После каждого этапа игра играбельна.

### Этап 0 — вытянуть ассеты (чистая логистика)

**Цель:** все файлы на диске, ни строчки кода.
**Файлы:** `guns_data_meshes.db` (16 МБ), `guns_data_hands.db` + `..._new_part1/2.db` (46+77+58 МБ), `guns_data_sounds.db` (19 МБ), `guns_data_textures_common.db` (38 МБ) из `github.com/gunslingermod/*/mod_data/patches/`.
**Рецепт (проверен):** `expand.exe <файл> -F:* <dir>` → `converter.exe -unpack -xdb <inner.db> -dir <out>`, конвертер — `D:\Games\Dead Air\tools\AXRToolset\bin\converter.exe`.
**Готово:** есть `dev_pda_hud.ogf`, руки `.omf` с `pda_*`, `pda_draw`/`pda_vibros`, `ui_pda_loadscreen`, текстуры корпуса.
**Проверка:** `converter.exe -unpack` на собственный вывод (verify-распаковка, как в нашем packaging-скрипте); распечатать список сабсетов `.ogf` и список моушенов `.omf` — это вход для Этапа 3.

### Этап 1 — RT `$user$ui` + проход UI→RT + дебаг-блит

**Цель:** доказать, что весь виджет-трей PDA, **с текстом**, ложится в текстуру 1:1.
**Файлы:**
* `src/Layers/xrRender_R2/r2_types.h` — `#define r2_RT_ui "$user$ui"` (имя свободно, проверено)
* `src/Layers/xrRenderPC_R4/r4_rendertarget.h` — `ref_rt rt_ui;`
* `src/Layers/xrRender_R2/r2_rendertarget.cpp` — `rt_ui.create(r2_RT_ui, w, h, D3DFMT_A8R8G8B8, 1)` внутри `#if RENDER == R_R4`
* `src/Layers/xrRender_R2/r2_R_render.cpp` — тело `BeforeWorldRender()`
* `src/xrEngine/CustomHUD.h` — новый виртуал `RenderPdaScreenUI()`
* `src/xrGame/HUDManager.cpp` — реализация: `CUIPdaWnd::Draw()` + `DrawHint()` + `UI().RenderFont()`
* временная консольная `r__dbg_pda_rt` — фуллскрин-блит RT

**Готово:** `r__dbg_pda_rt 1` при открытом PDA показывает пиксель-в-пиксель то же, что 2D-PDA, **включая шрифты, скиссоры и списки**.
**Проверка на стенде:**
* `_qa\lightdiag`-раннер: скрытый десктоп, автозагрузка сейва, Lua-проба, скриншот. Сравнить скриншот с `r__dbg_pda_rt 1` и без — попиксельно.
* Пройтись по всем шести вкладкам, включая Lua-вкладки Relations/Contacts/Encyclopedia — там скролл-вью и списки, т.е. проверка скиссоров.
* `-dxdebug` — лог не должен сыпать hazard'ами кроме известного SRV/RTV на первом кадре.
* **Аудит кражи шрифтов:** проверить, что до `BeforeWorldRender()` в кадре никто ничего не кладёт в `CGameFont::strings` (консоль, `StatGraph`, `HitMarker`, туториалы, всё на `Device.seqRender` с приоритетом ниже `CLevel`). `UI().RenderFont()` дренирует **все** шрифты — чужой текст утащит в текстуру PDA.
* `r__fps_log N` — базлайн: (a) PDA закрыт, (b) PDA открыт 2D сегодня, (c) 2D + RT-проход. Без (b) разговор о стоимости беспредметен: UI уже сейчас рисуется каждый кадр поверх полностью отрендеренного мира.

### Этап 2 — шейдеры экрана + биндер `m_affects`

**Цель:** материал `models\pda` собирается и реагирует на константу. Модели ещё нет — тестируем на кубе/плоскости.
**Файлы:**
* `packaging/.../gamedata/shaders/r3/models_pda.s` — оригинал минус `distort(true)`
* `packaging/.../gamedata/shaders/r3/model_pda_screen.ps` — оригинал минус `float3 tc1: TEXCOORD1`, плюс `da_pda_screen_rect`-подрект, `smp_rtlinear`
* `src/Layers/xrRender_R2/r2.cpp` — `RegisterConstantSetup("m_affects", &binder_pda_affects)` и `("da_pda_screen_rect", ...)` рядом со строками 761-785
* временные консольные `r__dbg_pda_affects_x/y/w` для ручной прокрутки

**Готово:** крутя `r__dbg_pda_affects_x` от 0 до 1, глазами видно всю лестницу: строка (0.09) → волна (0.15) → подмена фида (0.27) → сдвиг (0.38) → чёрный (0.41). `_w > 0` даёт загрузочный экран.
**Проверка:** скриншоты на каждом пороге; лог на `! shader [models\pda] replaced with [stub_default]` — если он есть, имя материала не совпало; `-shader_warnings_as_errors` на время сборки; при подозрении на протухшие константы — `-cb_nocache` (наша штатная развилка по этому классу багов).

### Этап 3 — модель и руки без UI

**Цель:** КПК физически появляется в руках, играются анимации, экран показывает свою собственную текстуру.
**Файлы:** `packaging/.../gamedata/meshes/dynamics/devices/dev_pda/dev_pda_hud.ogf`, `.../hud_hands_animation/*.omf`, `.../sounds/items/pda/*`, `.../configs/misc/items/items_pda_animator.ltx` с `[pda_show_animator]:anm_base` и `[pda_show_animator_hud]:anm_base_hud`, compat-скрипт `dead_air_x64_pda3d.script` (активация по образцу `dinamic_hud.clean_animation()`).
**Готово:** биндом или `alife():create(...)` + `activate_slot(13)` КПК поднимается в руку, играет `pda_draw`, стоит в `pda_idle`, убирается по `pda_hide`.
**Проверка:**
* Именно здесь ловится **фатальный** риск: `player_hud.cpp:100` `R_ASSERT2(!pm.m_animations.empty(), "motion not found [%s]")`. Загрузчик перебирает **все** `anm_*` ключи секции, а у Gunslinger их около двухсот. Каждый непокрытый = вылет. Секцию **триммить** под то, что реально есть в нашем `.omf`.
* Второй фатал: `anim_play` при наличии `IKinematicsAnimated` у предмета делает `R_ASSERT3(M2.valid(), "model has no motion [idle]")`. У `dev_pda_hud.ogf` обязан быть цикл `idle`.
* Третий: `hud_item_measures::load` (`player_hud.cpp:234-235`) читает `item_position`/`item_orientation` жёстким `r_fvector3`. Наследование от `[anm_base_hud]` их даёт — проверить.
* Смоук по нашему рецепту: деплой dll руками, `server(all/single/alife/new)` → уровень `fake_start`, консоль через `user.ltx`, выход через `CloseMainWindow`.
* Дождь и свет: встать под ливень и под лампу, скриншот. Корпус обязан мокнуть и ловить блики — это подтверждение G-буферного пути.

### Этап 4 — сшивка: экран показывает живой UI

**Цель:** фича собрана.
**Файлы:** `src/xrGame/PdaAnimatorItem.{h,cpp}` (`: CWeaponBinoculars`), правки `CUIPdaWnd::Show(bool)` (хук состояния), frame-guard в `CUIPdaWnd::Draw()` и `CUICursor::OnRender()`, `sources.cmake`.
**Готово:** нажал клавишу — КПК поднялся, на экране живой PDA. Закрыл — убрался. 2D-окно на экране не появляется.
**Проверка:**
* `CUICursor::OnRender` (`UICursor.cpp:61-88`) зовёт `g_btnHint->OnRender()` и `g_statHint->OnRender()` **до** проверки `IsVisible()`, а `VERIFY(last_render_frame != Device.dwFrame)` сидит под `#ifdef DEBUG` и только при видимом курсоре. Гард поднять из `#ifdef DEBUG` наверх функции — и **осознанно решить**, глотаем ли мы вместе с ним хинты кнопок.
* **Не копировать** идиому IX-Ray `SetUICursorPosition(GetCursorPosition())` — наш `SetUICursorPosition` (`UICursor.cpp:90-97`) дополнительно варпает системный курсор через `pInput->iSetMousePos`.
* Инвариант фоллбэка: принудительно отключить RT-проход в рантайме → PDA обязан нарисоваться на экране как раньше. Автотест.
* `hud_draw 0`: решить контракт явно. Рекомендация — модель остаётся, экран уходит в «выключенный» цвет, тот же кодовый путь, что и нулевой заряд. Одна ветка пустого экрана, а не две.

### Этап 5 — двухстадийный ввод, зум, `screen_kx`

**Цель:** текст читается.
**Файлы:** `PdaAnimatorItem.cpp` (`OnZoomIn/OnZoomOut` → `PdaMenu()->Enable()`), `UIGameSP.cpp` (предикат вместо указательного сравнения для `UITimeDilator::Pda`), `screen_kx` в курсор / `UIMap.cpp` / `map_location.cpp` / ширину таб-кнопок.
**Готово:** ПКМ подтягивает КПК, курсор оживает, клики попадают в виджеты, WASD работает.
**Проверка:**
* Замерить реальную высоту строки `letterica16` в device-пикселях в обеих позах (скриншот + линейка). Цель в прицеле — ≥ сегодняшних 22.5 px @1080p.
* Тыкнуть метку на карте в прицельной позе, сверить мировые координаты — это проверка `screen_kx` и `ConvertCursorPosToMap`. `screen_kx` **вычислять** из аспекта квада, ltx-константу оставить как override; помнить, что `CMapManager::OnUIReset` (`map_manager.cpp:141-149`) перезагружает `map_spots.xml` и все живые локации — kx туда тоже надо донести.
* Проверить, что мы не влетели в `UITimeDilator::Pda` slow-mo, держа КПК на ходу. Заодно: `UIMode currMode;` в `UITimeDilator.h:26` без инициализатора, а `startTimeDilation()` его читает.

### Этап 6 — состояния: заряд, помехи, буст

**Цель:** три состояния из ТЗ живут на данных.
**Файлы:** ltx с порогами, `dead_air_x64_pda3d.script` (функторы `get_power`/`get_interference`), `binder_pda_affects`.
**Готово:** нулевой заряд — чёрный экран; рядом с аномалией — помехи растут; при показе — короткий бут.
**Проверка:** Lua-проба выставляет заряд/помехи и снимает скриншоты на пороге; проверить оба слоя скриптов — базовый и **DAR2 активным** (`database/xtra_dar2.xdb0` везёт свой `pda.script` и свой `itms_manager.script`).

### Этап 7 — стоимость, throttle, лесенка

**Файлы:** `xrRender_console.cpp` (`pda_rt_by_preset[5]` для размера/мипов), throttle в `BeforeWorldRender()`.
**Готово:** `r__fps_log` показывает дельту в пределах шума на «в руках».
**Проверка:** четыре замера — (a) PDA закрыт, (b) 2D сегодня, (c) 3D полная частота, (d) 3D throttled 15 Гц. Один сейв, одно разрешение.

---

## 6. Совместимость

### 6.1 Сейвы

**Формат не меняется.** `CPda::save` не трогаем вообще. Предмет-аниматор — обычный `WP_BINOC` в `ANIMATION_SLOT`, как уже шипящиеся `animation_clean_mask` и `animation_hit_wpn_knife*`. Стейт аниматора рантаймовый.

Единственный настоящий риск — **`CActor::save` тянет UI**: четыре байта фильтров карты читаются живьём из `pUITaskWnd` (`Actor_Network.cpp:1429-1444`). Инвариант: `CUIPdaWnd` + `CUITaskWnd` строятся эагерно, всегда, в обоих режимах.

Если когда-нибудь понадобится персистить что-то своё — только **новый `.scov`-чанк** с молчаливым дефолтом, никогда не новое поле в `.scop`.

**Поведенчески сейвы НЕ нейтральны**, и это надо сказать вслух:
* `CUIPdaWnd::Show` шлёт `ui_pda` / `ui_pda_hide`, `SetActiveSubdialog` — id каждой вкладки через `SendInfoToActor`. Всё это уходит в `m_known_info_registry`, т.е. **в сейв навсегда**.
* На `ui_pda` синхронно дёргается `pda.calculate_rankings()` (`info_portions.script:63-65`).
* Пока стоит `ui_pda`, `level_weathers.script:420-424` форсит `r2_dof_kernel 8 / r2_dof_pickable 0 / r2_dof_far 1.5` — **полноэкранное DOF-размытие**. С КПК в руках игрок будет ходить по миру в блюре.

Политика: **инфопоршены шлём в те же моменты, что и сегодня** (на `Show(true)`/`Show(false)`, не по концу анимации). DOF правим через общий compat-слой (`dead_air_x64_ui_compat.script`, там сейчас ноль PDA-ссылок), а не выпиливанием инфопоршена.

### 6.2 Lua API — три группы, не одна

Разделение обязательно, иначе «всё работает» на бумаге, а моды врут числами.

**A. Переживает изменение рендера** (`UIActorMenu_script.cpp:197-215`): `IsShown`, `ShowDialog`, `HideDialog`, `SetActiveSubdialog`, `SetActiveDialog`, `GetActiveDialog`, `GetActiveSection`, `GetTabControl`, `ActorMenu.get_pda_menu()`.

**B. Ломается при любой смене холста/курсора** — API компилируется, числа врут: `GetCursorPosition` (`ui_export_script.cpp:194`), `FitInRect` (:197), `GetAbsoluteRect`/`IsCursorOverWindow`/`IsUsingCursorRightNow`/`GetMouseX`/`GetMouseY` (:199-291). **Это и есть решающий аргумент за RT в размер устройства и за обычный курсор.**

**C. Ломается семантически, даже с целым API:** `pda_menu:IsShown()` перестаёт означать «игрок смотрит в КПК» — `itms_manager.script:350-362` именно по этому решает, открывать или закрывать. Нужен либо новый предикат, либо гарантия, что `IsShown()` совпадает с «PDA в руках».

**Движок→Lua контракты, которые обязаны сохранить имя, арность и порядок вызова:** `pda.set_active_subdialog` (`UIPdaWnd.cpp:325` — обязан и дальше возвращать `CUIDialogWndEx*`, цепляемый под `UIMainPdaFrame`), `pda.calculate_rankings` (:292), `pda.property_box_clicked` / `property_box_add_properties` (`UIMapWnd.cpp:580,598`), `pda.actor_menu_mode` (`UIInventoryUtilities.cpp:559,566`), `pda.get_stat` / `get_monster_back` / `get_monster_icon` / `get_favorite_weapon` / `get_rankings_array_size` (`UIRankingWnd.cpp:219,359,376,393,414`), вся шестёрка `pda.coc_rankings_*` (`UIRankingsCoC.cpp:44-67`).

### 6.3 XML и конфиги

**XML не трогаем вообще.** Ни `pda.xml`, ни `pda_16.xml`, ни страничные. XMS публикует `ui\pda.xml` как документированную цель `.xmlp` (`MODDING.md:24`), а промах патча — молчаливый ворнинг (`xms_xml.cpp:239,255,269,284`), т.е. поломка без крика. Только добавление узлов через опциональный паттерн (`UIHelper::CreateStatic(..., false)` → nullptr при отсутствии).

**Конфиг предмета — новый файл**, ничего существующего не переопределяем. Наследование `[pda_show_animator]:anm_base` и `[pda_show_animator_hud]:anm_base_hud`.

**Трансляция ключей Gunslinger → наши** (иначе половина тюнинга молча уйдёт в дефолты):

| Gunslinger | наш движок | статус |
|---|---|---|
| `item_visual`, `hands_position[_16x9]`, `hands_orientation[_16x9]`, `aim_hud_offset_pos/rot[_16x9]`, `gl_hud_offset_*`, `attach_place_idx`, `item_position`, `item_orientation` | те же имена | **совпадает** (`player_hud.cpp:229-282, 395-409`) |
| `inertion_origin_offset` | `inertion_origin_offset` | совпадает |
| `inertion_pitch_offset_r/n/d` | `pitch_offset_right/up/forward` | **переименовать** |
| `inertion_speed`, `inertion_aim_speed` | `inertion_tendto_speed`, `inertion_tendto_aim_speed` | **переименовать** |
| `inertion_aim_origin_offset` | `inertion_origin_aim_offset` | **переименовать** |
| `hud_move_*_offset_pos/rot`, `to_crouch_time`, `mark_anm_*`, `use_clicks`, `play_blowout_anim`, `can_use_torch_when_aim` | нет | **игнорируются**, это фичи их аниматора |
| `zoom_enabled`, `scope_zoom_factor`, `hud_fov_factor`, `hud_fov_zoom_factor` | читаются оружейным путём | проверить на `CWeaponBinoculars` |
| ~200 `anm_*` | все перебираются загрузчиком | **триммить под наш `.omf`, иначе фатал** |

**Опознание КПК — в данных.** Refined-овская ltx со списком секций, для которых поднимается 3D-презентер, + требование наличия hud-ключа. Ни `wpn_upd`, ни имён уровней, ни `[pda] pda_animator` из чужого `game_global.ltx` в C++ нет.

### 6.4 Установленные моды

* `MODS/` — **ни один** не везёт `pda*.xml` и ни одного PDA-скрипта. Только DDS: `ui_actor_pda.dds` (атлас `ui_inGame2_pda_texture`), `ui_pda2_noice.dds`, `ui_actor_monsters_pda*.dds` в трёх «Интерфейс - Новое оформление». Плюс `Игра - Аккумулятор на 20 мАч` переопределяет `configs/plugins/axr_battery.ltx`. Чтобы эти свопы текстур продолжали работать, RT-проход **обязан** рисовать весь виджет-трей, включая `background_static` и `noice_static`.
* `database/xtra_dar2.xdb0` (**Revolution II установлен**) — везёт свой `pda.script` (без веток `ui_pda_disabled_tab`), свой `pda_tasks_16.xml` (прячет строку сюжетного задания в x=999 y=999), свой `itms_manager.script` и три PDA-hack диалога. Смоук обязателен на **обоих** слоях.
* Живая loose-gamedata везёт `dar2_oxygen_hud.script` + `configs/ui/dar2_oxygen_hud.xml`, регистрирующий CustomStatic. `CUIGameCustom::Render()` рисует CustomStatics **первыми и безусловно** (`UIGameCustom.cpp:87-88`). Отсюда жёсткое правило: **в RT рисуем только `CUIPdaWnd::Draw()` + хинты (+ курсор)**, а не `CUIGameCustom::Render()` целиком — иначе индикатор кислорода запечётся на экране КПК.

### 6.5 Что ещё обязано продолжать работать

Счётчик контактов миникарты (`UIZoneMap.cpp:130-132` через `GetPDA()->ActiveContactsNum()`), мигающая иконка `efiPdaTask` (`UIMainIngameWnd.cpp:498-504`, там `R_ASSERT2` на отсутствующую иконку), `CurrentGameUI()->UpdatePda()` при приходе статьи (`actor_communication.cpp:154-157`), правоклик-меню меток карты, `UpdateRankingWnd()` из `UIMainIngameWnd.cpp:646`, туториалы (`UIGameTutorialSimpleItem.cpp:260-313` — третья точка входа через `ShowOrHideDialog`, плюс форс-хайды на :338-340 и `UIGameTutorial.cpp:431-433, 467-469`).

Отдельно: `CHUDManager::OnUIReset` (`HUDManager.cpp:291-303`) делает `UnLoad(); Load();`, т.е. **`xr_delete(PdaMenu)` + `xr_new`**. Триггеры — смена разрешения (`Device_destroy.cpp:62-64`), смена языка (`console_commands.cpp:302`), смена ui_style (`ui_styles.cpp:87-90`, доступно из Lua). Значит: **никогда не кэшировать `CUIPdaWnd*` между кадрами**, всегда перезапрашивать и null-гардить; повесить на презентер `CUIResetNotifier`.

---

## 7. Риски и открытые вопросы

Отсортировано по тому, что первым убьёт этап.

**R1 — блокер. Руки: анимации `pda_*` физически негде взять без правки OGF рук.**
У нас **нет** механизма `PlayerHudOmfAdditional` (это `EngineExternal` IX-Ray, у нас его класса не существует). Motion-refs X-Ray'евский OGF тянет из своего чанка. Значит новый `.omf` требует либо переавторинга `motion_refs` в модели рук `actor_hud_*`, либо добавления такого механизма в загрузчик.
*Что закроет:* дамп чанков нашей модели рук + пробная сборка OGF с добавленным `motion_ref`. Делать на Этапе 0/3, до всего остального.

**R2 — блокер. `R_ASSERT2` на отсутствующий моушен, ~200 ключей `anm_*` в секции.**
`player_hud_motion_container::load` (`player_hud.cpp:52-101`) перебирает **каждый** ключ `anm_*` и падает на первом нерезолвнутом. Плюс `attachable_hud_item::anim_play` (`:436-439`) падает на отсутствующем алиасе при запросе, плюс `R_ASSERT3(M2.valid(), "model has no motion [idle]")` для модели предмета.
*Что закроет:* сверка списка моушенов из `.omf` со списком ключей секции, автоматом, до первого запуска. Секцию триммить.

**R3 — высокий. Опознание КПК через `GetPDA()` даст взрывчатку в руке.**
`InventoryOwner.cpp:220` — сырой каст на `PDA_SLOT`. `device_pda`, `dev_flash_1`, `dev_flash_2`, `pri_a25_explosive_charge_item` — все `class = D_PDA`, `slot = 7`. Закрыто решением §3.4 (ltx-список), но если кто-то в реализации соскользнёт обратно на `GetPDA()` — приедет баг во время квеста pri_a25.
*Что закроет:* явный тест: положить в слот `pri_a25_explosive_charge_item`, нажать открытие PDA.

**R4 — высокий. Кража очереди шрифтов.**
`UI().RenderFont()` внутри RT-бинда дренирует **все** шрифты. Всё, что положило строки до `BeforeWorldRender()`, утащит в текстуру PDA.
*Что закроет:* инструментальный проход — счётчик `strings.size()` по шрифтам непосредственно перед проходом, лог, если ненулевой.

**R5 — высокий. Читаемость в позе «в руках».**
~9-10 px на строку против 22.5 сегодня. Смягчается зумом (~3.1×), но зум обязателен, а не опционален. Плюс минификация 2.4× на 1-мипном RT и наш TAA: `da_taa.ps` — чисто камерная репроекция, per-object velocity и джиттера проекции нет, а репроекция идёт по `gbd.P`, который для HUD-пикселей записан под HUD-проекцией в срезе глубины 0..0.02. Качающийся экран будет клампиться соседством 3×3, а не стабилизироваться.
*Что закроет:* замер высоты строки скриншотом в обеих позах; при шиммере — `smp_rtlinear`, `CRT::MIPPED_RT_FLAG` + `GenerateMips`.

**R6 — высокий. `UITimeDilator::Pda`.**
`UIGameSP.cpp:257-265` включает slow-mo по указательному сравнению с `PdaMenu`, безусловно. Держим окно в стеке всё время — получаем перманентное замедление, в том числе в бою. Плюс туториалы сохраняют/восстанавливают этот режим вокруг своего показа PDA (`UIGameTutorialSimpleItem.cpp:309-311, 341`).
*Что закроет:* предикат «UI-фокус» на самом окне, проверяемый на месте (`CUIPdaWnd` — `final`, подклассом не обойти).

**R7 — средний. `.ogf` может не иметь отдельного сабсета экрана.**
Оригинальный `models_pda.s` биндит `s_base = t_base` (собственная текстура) **и** `s_vp2 = $user$ui` на одну поверхность. Значит корпус обязан быть отдельным сабсетом с обычным материалом — иначе весь КПК станет экраном.
*Что закроет:* дамп сабсетов `dev_pda_hud.ogf` на Этапе 0. Критерии приёмки — §8.

**R8 — средний. UV экрана против аспект-свопа `pda.xml` ↔ `pda_16.xml`.**
Закрыто биндером подректа (§4.3), но если реализация зашьёт UV в меш — на 4:3 и на любом моде со своим `pda*.xml` картинка поедет.
*Что закроет:* прогон на 4:3 и с включённым DAR2 (у него свой `pda_tasks_16.xml`).

**R9 — средний. `CHUDManager::OnUIReset` пересоздаёт `CUIPdaWnd` посреди сессии.**
Смена разрешения/языка/ui_style. Кэшированный указатель повиснет при КПК в руках.
*Что закроет:* сменить разрешение с открытым 3D-PDA. Обязательный тест.

**R10 — средний. Frame-guard как «3D включён» вместо «растеризован в RT».**
Даст невидимый КПК на `ShowDialog(true)` из Lua. У IX-Ray неявный счётчик кадров случайно корректен.
*Что закроет:* автотест инварианта (Этап 4).

**R11 — низкий. Выделенный сервер и MP.**
`GEnv.isDedicatedServer` ставится из `-dedicated` (`x_ray.cpp:302-303`); `g_player_hud` создаётся безусловно (`Level.cpp:112-113`); `CResourceManager::Create` возвращает **nullptr** на дедике (`ResourceManager.cpp:507,534`), а не `stub_default`. `CUIPdaWnd::Init` гардит `IsGameTypeSingle()` только на шести C++-страницах — само окно строится и в MP.
*Что закроет:* явные гарды на создание модели, аллокацию RT и резолв материала.

**R12 — низкий. Редактор (`D:\Games\XFined-Editor`) — отдельный форк.**
Свой `XrRender`, свой `player_hud`. `$user$ui` он не создаёт. Модель со ссылкой на `models\pda` будет превьюиться чёрной в контент-браузере.
*Что закроет:* плейсхолдер-текстура или editor-side заглушка.

**R13 — низкий. `distort(true)` без `l_special`.**
В нашем движке `mapDistort` читает только `E[4]`, так что флаг инертен — но `r__dsgraph_render.cpp:687` (`if (E2 && !(E2->flags.bDistort))`) пропускает distort-элементы в каком-то из служебных проходов. Рекомендация — просто убрать флаг.
*Что закроет:* прочитать, какая функция содержит :687, и убедиться, что экран не выпадает из нужного прохода.

### Открытые вопросы, которые надо закрыть решением, а не кодом

1. **`UINoice`**: гасим в 3D-режиме, оставляем как базовый слой поверх шейдерного шума, или переносим целиком в `m_affects`? Влияет на моды, свопающие `ui_pda2_noice.dds`.
2. **Курсор и `g_btnHint`/`g_statHint`**: они рисуются из рендер-слота курсора, вне дерева диалогов (`UICursor.cpp:51`, приоритет `-3`). В RT они попадут только если явно перепарентить в `CUIPdaWnd::DrawHint()`. Хотим?
3. **`hud_draw 0`**: рекомендация — модель остаётся, экран уходит в «выключенное» состояние тем же кодовым путём, что и нулевой заряд. Подтвердить.
4. **Что именно юзер называет «помехами от аномалий»**: на этой сборке живого механизма нет (DAR2 убил ветки `is_jammed`). Мы пишем эффект **впервые** по математике оригинала. Надо, чтобы юзер это знал и сказал, откуда брать уровень — близость аномалий (нужен новый запрос через feel_touch/список зон) или список уровней, как было.
5. **Заряд**: остаётся `wpn_upd` через compat-слой, или заводим отдельную PDA-батарейку? Первое ничего не ломает и переиспользует HUD-полоску (`ui_extra.script:75-96`). Второе — новое состояние, значит `.scov`.
6. **kACTIVE_JOBS**: движковый обработчик **не добавлять**. Наш `CUIPdaWnd::OnKeyboardAction` (`UIPdaWnd.cpp:511-517`) его не трогает, ключ владеется Lua (`itms_manager`), и DAR2 этот скрипт тоже подменяет. Добавим C++-хендлер — получим двойной тоггл.
7. **Одно- или двухстадийный ввод**: рекомендация — двухстадийный (без него текст нечитаем, §4.5). Но это меняет ощущение «открыл PDA» относительно сегодняшнего. Подтвердить у юзера.

---

## 8. Порт-лист ассетов и данных (BOM)

Всё едет в `packaging/dead-air-x64/compatibility/gamedata/`, пакуется автоматически в `xtra_dead_air_x64.xdb0`. Списка файлов править не надо.

### 8.1 Шейдеры — уже на диске, в нашем диалекте

| файл | откуда | куда | правки |
|---|---|---|---|
| `models_pda.s` | `_analysis/gunslinger-3dpda/shaders_r3/models_pda.s` | `shaders/r3/models_pda.s` | убрать `distort(true)`; `smp_base` → `smp_rtlinear`; `$user$ui` создаётся движком |
| `model_pda_screen.ps` | там же | `shaders/r3/model_pda_screen.ps` | **удалить `float3 tc1: TEXCOORD1`** (обязательно, иначе не слинкуется с нашим `model_def_lplanes.vs`); добавить подрект `da_pda_screen_rect`; при желании — множитель по `c0` (facing fade) |
| `model_def_lplanes.vs` | **уже есть** — `_analysis/base_configs/shaders/r3/`, кэш `model_def_lplanes_0/1.vs` | — | ничего |
| `common.h` (`timers`, `smp_base`) | **уже есть** — `shaders/r3/shared/common.h`, биндер `Blender_Recorder_StandartBinding.cpp:603` | — | ничего |

### 8.2 Меши и анимации — тянуть

| ассет | архив | путь назначения | приёмка |
|---|---|---|---|
| `dev_pda_hud.ogf` | `guns_data_meshes.db` (16 МБ) | `meshes/dynamics/devices/dev_pda/` | ≥2 сабсета; сабсет экрана несёт имя шейдера `models\pda`; UV0 экрана 0..1 по видимому квадру; скиннед, корневая кость 0; есть цикл `idle` |
| руки `.omf` с `pda_*` | `guns_data_hands.db` (46) + `..._new_part1.db` (77) + `..._new_part2.db` (58) | `meshes/dynamics/weapons/hud_hands_animation/` | **см. R1** — без механизма подключения дополнительного OMF нужен переавторинг `motion_refs` модели рук |
| текстуры корпуса + `ui\ui_pda_loadscreen` | `guns_data_textures_common.db` (38 МБ) | `textures/dynamics/devices/dev_pda/`, `textures/ui/` | `ui_pda_loadscreen` нужен для ветки `m_affects.a` |
| `items\pda\pda_draw`, `items\pda\pda_vibros` | `guns_data_sounds.db` (19 МБ) | `sounds/items/pda/` | опциональны, отсутствие безопасно (`line_exist`-гард) |

Минимальный набор моушенов рук, который стоит целить (остальное из ~40 — украшательства, но каждый лишний ключ в секции = потенциальный фатал):
`pda_draw`, `pda_hide`, `pda_idle`, `pda_walk`, `pda_walk_slow`, `pda_click`, `pda_vibros`, `pda_vibros_end`, восемь направленных `pda_idle_up/down/left/right` + диагонали, и параллельный аим-набор `pda_aim_idle`, `pda_aim_start`, `pda_aim_end`, `pda_aim_click`, `pda_aim_hide`, `pda_aim_draw_1stpart/2ndpart/idle`.

### 8.3 Конфиги — авторим на основе оригинала

Новый файл `configs/misc/items/items_pda_animator.ltx`:

```ini
[pda_show_animator]:anm_base
hud            = pda_show_animator_hud
visual         = dynamics\devices\dev_pda\dev_pda.ogf
zoom_enabled   = true
scope_zoom_factor = 1.05
snd_draw       = items\pda\pda_draw
snd_holster    = detectorshud\detector_draw
; slot / animation_slot / class = WP_BINOC / inv_grid 5000 — наследуются от [anm_base]

[pda_show_animator_hud]:anm_base_hud
item_visual         = dynamics\devices\dev_pda\dev_pda_hud.ogf
attach_place_idx    = 0
hud_fov_factor      = 0.9
hud_fov_zoom_factor = 0.29
hands_position       = -0.015,-0.018,0.17
hands_position_16x9  = -0.015,-0.018,0.15
aim_hud_offset_pos      = 0.0282,0.041,0.15
aim_hud_offset_pos_16x9 = 0.02,0.041,0.15
inertion_origin_offset  = 0.03
inertion_tendto_speed   = 10        ; было inertion_speed
; item_position / item_orientation — наследуются от [anm_base_hud]
; anm_* — ТРИММИТЬ под реальный список моушенов нашего .omf
```

Плюс Refined-овская `configs/dead_air_x64_pda3d.ltx`: список секций-КПК, пороги `m_affects`, привязка уровней/зон к уровню помех. Неизвестные записи — молча пропускаются, по образцу `dead_air_x64_mode_exclusive.ltx`.

**Не переносим:** `Enable3DPDA` (нет `EngineExternal`, и запрещено политикой), `[pda] pda_animator` из `game_global.ltx` (у нас другой механизм опознания), `screen_kx` как ручную константу (вычисляем, ltx оставляем как override), `[base_animator]`/`[base_animator_hud]`/`[hud_base]` целиком (у нас свой `[anm_base]`/`[anm_base_hud]`).

### 8.4 Скрипты

`gamedata/scripts/dead_air_x64_pda3d.script` — наш, в общем compat-слое:
* активация/деактивация предмета-аниматора (`alife():create` + `activate_slot(13)` + `release` + возврат слота), по проверенному образцу `dinamic_hud.clean_animation()`;
* функторы `pda.get_power()` / `pda.get_interference()` с безопасными дефолтами; читают `axr_battery` **только если он есть**;
* DOF-override, чтобы `ui_pda` больше не форсил полноэкранный блюр при КПК в руках.

Обязательно писать в глобал через `_G.` — иначе фикс мёртв (compat-скрипты живут в своей namespace-таблице; так была мертва обёртка `SendScriptCallback` до 1.3.4).

`_analysis/gunslinger-3dpda/scripts/pda.script` (12.8 КБ) — их скриптовая половина, полезна как референс логики состояний, но не переносится: у нас другая точка входа.