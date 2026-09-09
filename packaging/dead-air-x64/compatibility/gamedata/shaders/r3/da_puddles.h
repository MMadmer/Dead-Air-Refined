#ifndef DA_PUDDLES_H
#define DA_PUDDLES_H

// [DA_PORT] ---- Лужи в дождь --------------------------------------------------------------------
//
// Приём подсмотрен у Screen Space Shaders для Anomaly (Ascii1457), реализация своя. Суть в том, что
// лужа — это не объект, а изменение ТРЁХ свойств поверхности прямо в G-буфере: нормаль становится
// «вверх» (плюс рябь), глянец уходит в потолок, альбедо темнеет. Дальше отложенное освещение само
// нарисует блик от солнца и фонарей — отдельного прохода не нужно.
//
// Что приходит из движка (константа rain_params, см. Blender_Recorder_StandartBinding.cpp):
//   x — сколько льёт прямо сейчас,  y — насколько земля намокла (с задержкой),  z — размер луж.
//
// Почему НЕТ текстур. У SSS лужи стоят на трёх картах: шум Перлина, карта нормалей ряби и атлас
// всплесков. Каждая — это ещё и привязка сэмплера в C++-блендере, а блендер у деферред-геометрии
// встроенный, не скриптовый. Шум здесь считается на месте: два октава долины-шума по тем же UV,
// по которым лежит базовая текстура. Это дороже по арифметике и беднее на вид, зато не тянет за
// собой ни ассетов, ни правок блендера. Если вид не устроит — текстуры добавляются сюда же.

// ---- The fill map: the one texture this file does ask for ----------------------------------
//
// Value noise puts puddles in a pattern; it cannot put them where water actually goes. The
// answer is a Planchon-Darboux fill - flood every dip to the brim, then let it back down to the
// lowest sill it can escape over, and what is left standing IS the puddle. DESIGN2 section 6
// asks for that to replace the noise as the placement.
//
// It is computed ONCE, on the CPU, at level load, beside the water field and out of the same
// detail-slot heights (Level_load.cpp, bake_puddle_fill), and published as its own small texture
// (r4_water_field.cpp, "$user$puddle_fill"). Not out of the rain occlusion pass, which is where
// this started: rt_smap_rain is a depth-stencil target sampled through a comparison sampler, and
// the deferred terrain shader is built by a C++ blender with a fixed sampler set - the plumbing
// does not exist and a per-frame iterative fill over a shadow map would not be free either.
//
// Baking it also answers the constraint that decides everything here: this file runs TWICE, once
// in the terrain G-buffer shader and once in the reflection overlay, and the two are required to
// compute the same mask bit for bit or a puddle lays its mirror outside its own water. A map
// written once per level and read unchanged by both cannot drift between them by construction.
//
// C++ SIDE, landed: the texture is bound by uber_deffer.cpp for the G-buffer half and by
// da_puddle_refl.s for the reflection half, both to the same name; the mapping needs no constant
// of its own because the map shares the water field's footprint exactly, so da_water_map (bound
// for every shader already) addresses it and da_water_map2.w says whether it exists at all.
#include "da_water_field.h"

Texture2D s_puddle_fill;

uniform float4 rain_params;

// [DA_PORT] Вид воды, правится в игре консолью: x — зеркальность лужи (r__puddles_gloss),
// y — во сколько раз лужа темнее земли, z — глянец просто мокрой земли, w — сила ряби.
uniform float4 da_puddle_look;

// [DA_PORT] x — дальность, за которой луж нет (r__puddles_dist, метры). Отдельной константой, потому
// что в da_puddle_look все четыре слота заняты.
uniform float4 da_puddle_look2;

// [DA_PORT] x — ширина каймы вокруг воды (r__puddles_rim_width). Отдельной константой: в look2
// свободен был лишь один слот, а каймe нужно два — сила и ширина. Остальное свободно.
uniform float4 da_puddle_look3;

// Ветер и пробег ряби: xy — мировое XZ-направление ветра, умноженное на силу (eff_wind_norm),
// z — накопленный ВЕТРОВОЙ пробег узора ряби, w — накопленный ДОЖДЕВОЙ пробег (оба в единицах
// q-пространства, копятся на CPU — Environment::UpdateEffectiveWind). Биндер cl_da_puddle_wind.
uniform float4 da_puddle_wind;

// Водяные импакты (Environment::water_hit): константы и сами кольца живут в da_water_rings.h,
// общем с открытой водой (water.ps) — кольцо есть кольцо на любой воде. Здесь остаётся только
// осушение: лужу взрыв выплёскивает, озеро — нет.
#include "da_water_rings.h"

// The split wetness, the Saunderson darkening and the analytic rain rings, shared verbatim with
// the fullscreen wet-surface pass so the two never drift apart.
#include "da_wetness.h"

float da_hash21(float2 p)
{
	p = frac(p * float2(127.1f, 311.7f));
	p += dot(p, p + 34.23f);
	return frac(p.x * p.y);
}

// Периодический хэш для БЕГУЩЕГО узора ряби: обычный da_hash21 теряет точность, когда к
// координате прибавлен большой накопленный пробег (frac от p*127.1 при p ~ 10^4 уже мусор).
// Решётка замкнута по 64 ячейкам — аргументы остаются малыми при любом стаже сессии.
float da_hash21_p(float2 i)
{
	i = fmod(i + 4096.0f, 64.0f);
	return da_hash21(i);
}

float da_vnoise_p(float2 p)
{
	const float2 i = floor(p);
	float2 f = p - i;
	f = f * f * (3.0f - 2.0f * f);

	const float a = da_hash21_p(i);
	const float b = da_hash21_p(i + float2(1.0f, 0.0f));
	const float c = da_hash21_p(i + float2(0.0f, 1.0f));
	const float d = da_hash21_p(i + float2(1.0f, 1.0f));

	return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// Долинный шум: значения в узлах решётки, между ними сглаженная интерполяция.
float da_vnoise(float2 p)
{
	float2 i = floor(p);
	float2 f = frac(p);
	f = f * f * (3.0f - 2.0f * f);

	float a = da_hash21(i);
	float b = da_hash21(i + float2(1.0f, 0.0f));
	float c = da_hash21(i + float2(0.0f, 1.0f));
	float d = da_hash21(i + float2(1.0f, 1.0f));

	return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// Where water actually stands. ONE bilinear tap of the baked map - the nine-tap single-step
// approximation this replaces was trying to do the fill in the shader, and a fill is not a local
// question: a dip drains over the lowest sill anywhere on its rim, which nine taps at a fixed
// radius cannot see. The bake iterates until it does (Level_load.cpp).
//
// Bilinear and not point: the fill is a smooth scalar, and point-sampling it would print the
// map's 1024-texel lattice on the ground as square puddles. The coverage mask in the same
// footprint IS point-sampled, and for the opposite reason - see da_water_field.h.
//
// The map shares the water field's footprint exactly, so its mapping is da_water_map and its
// "does this level have one" flag is da_water_map2.w. No constant of its own, and nothing to
// keep in step with a second binder.
//
// Returns x = the stored depth, 0..1 of DA_PUDDLE_FILL_MAX; y = whether there is an answer here
// at all, so the caller can fall back to the noise where there is not. No rim fade: the bake
// seeds the map's own border as an outlet, so it is already zero where it ends.
#define DA_PUDDLE_FILL_MAX	0.25f	// metres of standing water that read as full (CEnvironment::puddle_fill_depth)
// How far the value noise may move that level, in the noise's own 0..1 domain. This is the whole
// of what the noise does now: the map says WHERE the water is, the noise gives the edge its
// shape so a puddle is not a contour line of a 1.4 m lattice. Mirrored on the CPU as
// da_puddle_fill_edge (Environment.cpp) - the two masks are one mask.
#define DA_PUDDLE_FILL_EDGE	0.30f

float2 da_puddle_fill_level(float2 wp)
{
	[branch] if (da_water_map2.w < 0.5f)
		return float2(0.0f, 0.0f);

	const float2 uv = da_wf_uv(wp);
	[branch] if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
		return float2(0.0f, 0.0f);

	return float2(saturate(s_puddle_fill.SampleLevel(smp_rtlinear, uv, 0).x), 1.0f);
}

// [DA_PORT] ⚠️ БЕЗ inout. Функция ничего не меняет на месте — она возвращает структуру, а вызывающий
// сам раскладывает поля. Причина не в стиле: предыдущая версия отдавала цвет, глянец и нормаль через
// изменяемые параметры, и цвет НЕ доезжал до кадра. Проба показала это однозначно — вода не темнела
// даже когда цвет задавался абсолютным числом прямо в шейдере, при живой маске и живых константах.
// Нормаль при этом применялась, поэтому в кадре оставался только контур лужи — «разметка».
//
// Три захода ушло на то, чтобы поверить не рассуждению («по стандарту копирование обратно обязано
// сработать»), а измерению. Здесь копировать нечего: одно присваивание структуры на выходе.
struct da_puddle_result
{
	float3 N;		// нормаль в пространстве вида
	float  hemi;	// открытость небу; в луже поднимается — это и есть отражение неба
	float  mask;	// маска луж, 0..1 — мягкая, для цвета и отражений
	float  mask_bin;	// она же «по пикселям»: 0 или 1, для нормали и глянца (см. ниже)
	float  rim;		// тёмная кайма промокшей земли ВОКРУГ воды, 0..1
	float  ripple_amp;	// сила ряби, посчитанная по дождю и дальности; рябь применяет наложение
	float2 ripple;	// the ripple gradient in world XZ (drift, rain, impact rings); water normal = (x, 1, y)
	float  damp;	// общая мокрота (шире луж), 0..1
	float3 dbg_color;	// цвет для отладочных режимов; применяется вызывающим
	bool   dbg_paint;	// красить ли dbg_color
};

// pos_v — положение точки В ПРОСТРАНСТВЕ ВИДА (I.position.xyz в deffer_*).
da_puddle_result da_puddles(float3 pos_v, float hemi_in, float3 N_in)
{
	da_puddle_result R;
	R.N = N_in;
	R.hemi = hemi_in;
	R.mask = 0.0f;
	R.mask_bin = 0.0f;
	R.rim = 0.0f;
	R.ripple_amp = 0.0f;
	R.ripple = 0.0f;
	R.damp = 0.0f;
	R.dbg_color = 0.0f;
	R.dbg_paint = false;

	// rain_params.y is now specifically the STANDING WATER accumulator - the mask, the rim and
	// the threshold ladder below are unchanged and still read it, so the fp32 replica of this
	// mask in Environment.cpp still agrees with the picture. What used to be one number for
	// everything is split in da_wetness; damp takes the film and the porous term instead.
	const float wet = rain_params.y;
	const float damp_w = saturate(da_wetness.x * 0.6f + da_wetness.y);
	const bool dbg      = rain_params.w > 0.5f;	// r__puddles_debug 1 — три величины по каналам
	const bool dbg_fill = rain_params.w > 1.5f;	// 2 — заливка чёрным/белым
	const bool dbg_hard = rain_params.w > 2.5f;	// 3 — вода абсолютно тёмного цвета (проба)

	// Сухо — выходим ДО мировой позиции и производных: в сухую погоду это чистая экономия на
	// каждом пикселе земли. Условие равномерное (константа на весь вызов), поэтому обычный if
	// без [branch]: производные ниже остаются в равномерном потоке и компилятор не ругается.
	// Both clocks have to be dry: three seconds into a shower the ground is already dark while
	// there is not a puddle anywhere yet, and the old single test threw that away.
	if (wet < 0.01f && damp_w < 0.01f && !dbg)
		return R;

	// Мировое положение точки.
	const float3x3 V_rot = float3x3(m_V[0].xyz, m_V[1].xyz, m_V[2].xyz);
	const float3 V_ofs = float3(m_V[0].w, m_V[1].w, m_V[2].w);
	const float3 pos_w = mul(transpose(V_rot), pos_v - V_ofs);

	// Геометрическая нормаль из производных мировой позиции — без матричных допущений.
	const float3 gn_w = normalize(cross(ddy(pos_w), ddx(pos_w)));

	// [DA_PORT] Геометрической нормали в пространстве вида здесь БОЛЬШЕ НЕТ.
	//
	// Она бралась из производных экрана и служила основой зеркала в луже. Но производные считаются на
	// квадрат 2x2 пикселя, и у самой границы в один квадрат попадают и вода, и земля: нормаль там
	// выходила ни та ни другая, причём по-разному в зависимости от того, как квадрат лёг на границу.
	// Под сильным светом это и рисовало контур по краю лужи.
	//
	// Нормаль воды теперь константа «вверх», как у Screen Space Shaders, — см. ниже.

	// Под крышей луж быть не должно. Раньше признаком служила полусферическая освещённость с
	// множителем 1.6 — то есть почти всё, что не в глухом подвале, считалось «под небом», и лужи
	// заводились в помещениях. Порог теперь с запасом: нужно не «хоть немного света сверху», а
	// заметная открытость. Вызывающий передаёт сюда доступ солнца из лайтмапы там, где он есть, —
	// это честный признак «сюда попадает дождь».
	const float sky = saturate((hemi_in - 0.30f) * 2.2f);

	// Только горизонтальное. ⚠️ Источник наклона обязан быть ГЕОМЕТРИЧЕСКИМ (ddx/ddy): маску
	// считают ДВА прохода - G-буфер и отражение (da_puddle_refl), и они должны сойтись
	// бит-в-бит. Попытка взять «гладкую» нормаль из G-буфера развалила согласованность:
	// рефл-проход видел уже ПОДМЕНЕННУЮ водяную нормаль, маски разошлись, и отражения легли
	// мимо луж - «вода чёрная как нефть». Фасетки же лечит НЕ смена источника, а мягкая
	// кривая: старая (порог 0.8, крутизна 6) превращала межтреугольную разницу нормали в
	// разы по маске; широкий пологий диапазон размазывает её до незаметности.
	const float3 Nn = normalize(N_in);
	const float slope = saturate((abs(gn_w.y) - 0.62f) * 2.2f);

	// Общая мокрота: шире луж и слабее их. Держать слабой — когда блестит всё, солнце отражается одним
	// пятном на пол-экрана, и земля читается ледяной, а не мокрой.
	//
	// Damp is the FILM plus the porous saturation (computed above, next to the early-out), not
	// the puddle accumulator: bare soil goes dark within seconds of the first drops and stays
	// dark for minutes after the shower, while standing water needs the whole storm to gather
	// and the best part of ten minutes to drain. Terrain is soil, so its porosity is one - no
	// per-material lookup is possible here anyway, because the reflection pass would read a
	// different one and the two halves of a puddle are required to agree bit for bit.
	const float damp = damp_w * slope * sky;
	R.damp = damp;

	// Рисунок луж по МИРОВЫМ координатам: развёртка ландшафта растянута на сотни метров, по ней пятна
	// выходили размером с локацию. Два масштаба — разливы метра по три и рваная кромка около метра.
	const float2 wp = pos_w.xz;
	float n = da_vnoise(wp * 0.33f) * 0.62f + da_vnoise(wp * 1.10f) * 0.38f;

	// Порог: влажность двигает его сама, поэтому лужи растут по мере дождя, а не появляются готовыми.
	// Нулевой размер — это ступень «Низкое»: остаётся только влажная земля, самих луж нет. Проверяем
	// явно, а не через порог: при size=0 порог 0.86 всё равно оставлял бы редкие пятна на пиках шума.
	[branch] if (rain_params.z < 0.005f)
		return R;

	// The fill map, where the ladder bought it and where the map has something to say. This is
	// the difference between blobs on flat ground and water in the dips it would really run to.
	//
	// The map sets the LEVEL and the noise keeps its job as the EDGE: the perturbation is a
	// third of the threshold band, so the contour of a puddle follows the fill isoline while
	// wandering by roughly the width of one noise feature. Without it the edge would trace the
	// bake's lattice and every puddle would have the same soft rectangular shoulder.
	//
	// fill.y is 0 where there is no answer - a level with no bake, or a point off its footprint -
	// and the lerp then leaves the noise placement exactly as it was, which is also what the
	// whole branch does when the rung is off (Minimum and Low). da_wet_params.x is
	// r__puddle_fill, straight off the preset ladder. Below the size early-out so the tap is
	// never spent on a rung that draws no puddle at all.
	[branch] if (da_wet_params.x > 0.5f)
	{
		const float2 fill = da_puddle_fill_level(wp);
		n = lerp(n, saturate(fill.x + (n - 0.5f) * DA_PUDDLE_FILL_EDGE), fill.y);
	}

	const float thr = lerp(0.86f, 0.30f, saturate(rain_params.z)) + (1.0f - wet) * 0.15f;

	// Дальность. Гасим не резко, а на последней четверти: жёсткая граница читается кольцом вокруг
	// игрока, которое едет вместе с ним, и это заметнее самих луж.
	const float d_max = max(da_puddle_look2.x, 1.0f);
	const float dist_fade = saturate((d_max - pos_v.z) / (d_max * 0.25f));

	float puddles = smoothstep(0.0f, 0.10f, n - thr) * wet * slope * sky * dist_fade;

	// ---- Осушение от взрывов: вода выплеснута, слой воды исчезает и медленно возвращается.
	// Гасится ТОЛЬКО маска воды: damp (тёмная мокрая земля) намеренно не трогается — мокрое
	// место от бывшей лужи остаётся, как и в жизни. Оба прохода (G-буфер и отражение) читают
	// этот же код — маски сходятся бит-в-бит.
	const int wh_count = int(da_wh_info.x);
	[loop]
	for (int wi = 0; wi < wh_count; ++wi)
	{
		const int wlo = min(wi, 3);
		const int whi = max(wi - 4, 0);
		const float4 WP = (wi < 4) ? da_wh_pos0[wlo] : da_wh_pos1[whi];
		const float4 WA = (wi < 4) ? da_wh_par0[wlo] : da_wh_par1[whi];
		[branch]
		if (WP.w <= 0.0f || WA.z < 0.5f || abs(WA.x) <= 0.001f)
			continue;
		const float wd = length(pos_w.xz - WP.xz);
		[branch]
		if (wd > WP.w || abs(pos_w.y - WP.y) > 2.5f)
			continue;
		const float dry = WA.x * saturate((WP.w - wd) / max(WP.w * 0.35f, 0.2f));
		puddles *= 1.0f - dry;
	}
	R.mask = puddles;

	// ---- Тёмная кайма вокруг воды ----------------------------------------------------------------
	// Грунт у кромки напитан водой и темнее сухого, а к краю сходит на нет. Считается по тому же шуму:
	// это полоса, где значение шума ЧУТЬ НЕ ДОТЯНУЛО до порога лужи. Ширина полосы — в единицах шума,
	// поэтому кайма сама подстраивается под форму пятна и нигде не идёт ровной линией.
	//
	// ⚠️ Её однажды сняли целиком, решив, что «обводка» — это она. Оказалось нет: обводку давал
	// дизеринг границы (см. ниже, где считается m_bin). Кайма вернулась как была.
	const float rim_w = max(da_puddle_look3.x, 0.01f);
	R.rim = saturate((n - (thr - rim_w)) / rim_w) * (1.0f - puddles) * wet * slope * sky * dist_fade;

	// Отладка 2: заливка чёрным по белому. Свет может сделать белое ярче или тусклее, но чёрное
	// останется чёрным — по такой картинке видно, сплошная маска или только кайма.
	[branch] if (dbg_fill && !dbg_hard)
	{
		R.dbg_color = (puddles > 0.5f) ? float3(0.0f, 0.0f, 0.0f) : float3(1.0f, 1.0f, 1.0f);
		R.dbg_paint = true;
		return R;
	}

	// Отладка 1: три НЕЗАВИСИМЫЕ величины по каналам (шум, горизонтальность, влажность) плюс клетка по
	// метру из мировых координат. Независимые — потому что произведение при нуле не называет виновника.
	[branch] if (dbg && !dbg_fill && !dbg_hard)
	{
		const float grid = step(0.5f, frac(pos_w.x)) * 0.15f + step(0.5f, frac(pos_w.z)) * 0.15f;
		R.dbg_color = float3(n, slope, wet) + grid;
		R.dbg_paint = true;
		return R;
	}

	[branch] if (puddles < 0.004f)
		return R;

	// Рябь от ВЕТРА.
	//
	// ⚠️ The rain half of this used to live here too, and it was the same animated noise: two
	// octaves advected downhill and downwind. That is wind chop by construction - a continuous
	// travelling field with no ring in it anywhere - and it is why rain on a puddle never read
	// as rain. The drops are a separate, analytic term now (da_rain_rings, below); what stays
	// here is the wind, which this field always actually depicted.
	//
	// ⚠️ Было суммой синусов: ripple.x = sin(x+t) + sin(y*1.31-t*1.13) и так же по y. Сумма синусов —
	// это параллельные волновые фронты, то есть ПОЛОСЫ, и они ещё и ползут вместе со временем. На
	// зеркальной воде это читалось как «полоски текут», и списать их на ray-march было легко: узор
	// действительно похож на бандинг отражений. Здесь же анимированный шум — рисунок нерегулярный, и
	// собрать его в полосы глазу не на чем.
	// Ветровое состояние: длина xy = сила ветра (eff_wind_norm уже содержит изменчивость).
	const float wind_len = length(da_puddle_wind.xy);
	const float wind_k = saturate(wind_len * 1.15f);
	const float2 wind_u = da_puddle_wind.xy / max(wind_len, 0.001f);
	// Нижняя граница волнения ЖИВАЯ: в настоящий штиль без дождя вода почти зеркало (0.12),
	// ветер поднимает волнение сам. Старый жёсткий floor 0.35 делал стоячую лужу вечно неспокойной.
	// A puddle is sheltered water: a breeze barely stirs it (quadratic in the wind), only a
	// gale ripples it. The linear 0.5 term kept every puddle churning in ordinary weather.
	// No rain term in it any more - the drops are rings, not chop.
	const float chop_now = 0.12f + 0.25f * wind_k * wind_k;
	// The ripple field used to stop dead at 40 m while the top presets draw puddles out to 60,
	// so the far end of the puddle field was a band of frictionless glass reflecting perfectly.
	// It fades with the same distance the puddles themselves do.
	const float near_f = saturate(1.0f - pos_v.z / max(da_puddle_look2.x, 1.0f));
	float2 ripple = 0.0f;
	[branch] if (chop_now * near_f > 0.01f)
	{
		// ⚠️ История: (1) сдвиг по синусам времени — узор ездил по эллипсу туда-обратно;
		// (2) двухслойный flowmap-кроссфейд — на слабом дрейфе глаз читал переливание слоёв
		// A→B→A как то же самое «качание всей текстуры». Слоёв больше НЕТ: узор один и он
		// НЕПРЕРЫВНО едет по потоку. Точность держит периодический шум (da_vnoise_p, решётка
		// замкнута по 64) — накопленный пробег не разрушает хэш, а сами фазы копятся на CPU
		// и приходят готовыми (da_puddle_wind.z/w).
		//
		// Куда плыть — из физики, раздельными членами:
		//   * стекание по склону, пока идёт дождь: горизонтальная компонента геометрической
		//     нормали указывает в сторону спуска, её ДЛИНА (крутизна) сама масштабирует
		//     дождевой пробег — на плоском дне склоновый член исчезает;
		//   * снос ветром по направлению погоды, взвешенный прокси-глубиной: мелкая кромка
		//     ползёт вчетверо медленнее середины, крупная лужа живёт заметнее мелкой.
		const float depth_k = saturate(0.25f + puddles * 1.5f);
		const float2 q = wp * 12.0f
		               - gn_w.xz * (da_puddle_wind.w * 3.0f)
		               - wind_u * (da_puddle_wind.z * depth_k);

		// Наклон поверхности берём как разность шума по двум осям: это градиент, то есть именно
		// то, на что отклоняется нормаль воды. Две октавы с РАЗНОЙ скоростью (вторая едет на 27%
		// быстрее и мельче вдвое) — параллакс слоёв воды без всякого кроссфейда.
		const float2 q2 = q * 1.9f + 31.4f - wind_u * (da_puddle_wind.z * depth_k * 0.27f);
		const float h1 = da_vnoise_p(q);
		const float2 g1 = float2(da_vnoise_p(q + float2(0.35f, 0.0f)) - h1,
		                         da_vnoise_p(q + float2(0.0f, 0.35f)) - h1);
		const float h2 = da_vnoise_p(q2);
		const float2 g2 = float2(da_vnoise_p(q2 + float2(0.45f, 0.0f)) - h2,
		                         da_vnoise_p(q2 + float2(0.0f, 0.45f)) - h2);
		ripple = (g1 * 0.62f + g2 * 0.38f) * (0.18f * chop_now * near_f * da_puddle_look.w);
	}

	// ---- Rain, as rings ------------------------------------------------------------------
	// The actual drops: expanding gravity-capillary packets, front 0.35 m/s, dead by 0.25 m,
	// one lattice cell per ring and a layer more for every quarter of rain intensity. Analytic
	// (da_wetness.h) because a deferred-geometry shader cannot be given a sampler, and the
	// same call runs in the reflection pass, so both halves tilt the water identically.
	const float2 rain_ripple = da_rain_rings(wp, da_wetness.w, da_wet_params.y) * near_f;
	ripple += rain_ripple;
	R.ripple_amp = 0.18f * chop_now * near_f * da_puddle_look.w + length(rain_ripple);

	// ---- Кольца от попаданий -------------------------------------------------------------
	// Лужа — тоже вода, и её кольца живут в том же симулируемом поле ряби, что и кольца на
	// озере: внутри окна поля градиент берётся из него, за окном — и для колец, рождённых за
	// окном, — из аналитических восьми слотов. Та же передача, что и в water.ps, на тех же
	// метрах. Сим гасит края своего окна сам, поэтому стыка нет по построению.
	float wfield = 0.0f;
	[branch] if (da_water_rip.z > 0.0001f)
	{
		const float2 rd = abs(wp - da_water_rip.xy);
		wfield = saturate((da_water_rip.z * 0.5f - max(rd.x, rd.y)) * (1.0f / DA_WF_RIM_M));
	}
	[branch] if (wfield > 0.001f)
		ripple += da_wf_ripple_slope(wp) * (near_f * wfield);
	[branch] if (da_wh_info.x > 0.5f)
		ripple += da_water_rings(pos_w, wfield);
	// Published for the reflection pass: its mirror must wobble with THIS gradient, not with a
	// noise of its own, or the two halves of one puddle disagree about where the water tilts.
	R.ripple = ripple;

	// Нормаль воды: плоская геометрическая нормаль поверхности плюс рябь. Именно она делает лужу
	// лужей — освещение начинает считать поверхность ровной.
	// [DA_PORT] Нормаль воды — константа «ВВЕРХ» в мировых координатах, как у Screen Space Shaders:
	//     MirrorUp = mul(m_V, float3(ripplesNormal.x, 1.0, ripplesNormal.y));
	//
	// Раньше здесь стояла ГЕОМЕТРИЧЕСКАЯ нормаль поверхности из производных, и вода послушно повторяла
	// бугры дороги. Это неверно по существу — вода стоит ровно, а не по склону, — и вдобавок давало
	// внутри лужи собственную структуру, которой там быть не должно, а на границе усиливало разницу
	// с землёй. Под сильным светом эта разница и читалась контуром.
	//
	// Рябь уже посчитана как градиент шума по мировым XZ, то есть ровно в тех осях, в которых её ждёт
	// эта формула.
	const float3 water_n = normalize(mul(V_rot, float3(ripple.x, 1.0f, ripple.y)));

	// [DA_PORT] Граница: СЖАТИЕ перехода, а не жребий по пикселям.
	//
	// Раньше здесь стоял дизеринг: у каждого пикселя свой случайный порог, вода или земля целиком.
	// Промежуточных нормалей он действительно не давал — но взамен по всему контуру лужи появлялась
	// полоса, где пиксели случайно делятся пополам. На экране это зернистая кайма, обводка. Один шов
	// заменился другим, менее объяснимым.
	//
	// Как у Screen Space Shaders: маска пропускается через smoothstep со СЖАТОЙ верхней границей, и
	// жёсткость задаётся ручкой. При жёсткости 1 верхняя граница схлопывается в ноль — получается
	// чистая ступень: ни смешивания, ни зерна, край идёт по линии шума и потому органичен.
	const float edge_hard = saturate(da_puddle_look2.y);
	const float m_soft = smoothstep(0.0f, max(saturate(0.30f - edge_hard * 0.30f), 0.001f), puddles);
	const float m_bin = step(0.5f, m_soft);
	R.mask_bin = m_bin;
	// [DA_PORT] Нормаль смешивается по МЯГКОЙ маске, а ручка жёсткости решает, насколько узок переход.
	//
	// Раньше здесь стояла чистая ступень, и ручка на нормаль не влияла вовсе — а именно ступень и
	// рисует контур под сильным светом: с одной стороны пикселя поверхность плоская, с другой
	// бугристая, отклик на костёр разный, граница читается линией.
	//
	// При edge = 1 переход по-прежнему ступень (m_soft схлопывается), при меньших значениях он
	// расширяется. normalize обязателен: без него смешанная нормаль короче единичной и темнит.
	R.N = normalize(lerp(Nn, water_n, m_soft));

	// ⛔ ОТРАЖЕНИЕ НЕБА УБРАНО. Было:
	//     fres = pow(saturate(1 - dot(water_n, V)), 4);
	//     R.hemi = max(R.hemi, puddles * (0.06 + 0.94 * fres));
	//
	// Замысел был честный: лужа смотрит в небо всей поверхностью, значит её полусферическая
	// освещённость выше окружающей земли, и отложенное освещение домножит это на цвет неба. Но вся
	// земля в кадре видна под СКОЛЬЗЯЩИМ углом, там френель близок к единице, освещённость задиралась
	// к максимуму — и рост света съедал потемнение цвета почти ровно во столько же раз. Лужа была, но
	// по яркости совпадала с сухой землёй; единственным местом, где баланс ломался, оставалась
	// граница с переходной нормалью. Это и была «разметка»: контур без заливки, четыре захода подряд.
	//
	// Урок общий: осветляющий и затемняющий члены, заведённые независимо, могут взаимно погаситься —
	// и тогда эффект «не работает», хотя работают обе его половины. Возвращать небо можно только
	// отдельной ручкой, чтобы видеть его вклад отдельно от потемнения.

	return R;
}

// The albedo wet ground should have, for the caller that owns the G-buffer write.
//
// The three flat lerps it replaces (0.88 for damp, r__puddles_rim for the rim, r__puddles_dark
// for the water) darken bright gravel exactly as hard as black asphalt. Saunderson does not:
// one curve, no authored parameters, half off a dark albedo and a fifth off a bright one -
// which is the entire observable difference between wet stone and wet tar.
//
// The knobs stay knobs and stay SEPARATE. r__puddles_dark becomes how far the water goes
// towards the wet curve, the rim keeps its own multiplier, and nothing here lightens anything:
// the note at the foot of this file is four attempts lost to a lightening term and a darkening
// term sharing one control and cancelling each other out.
float3 da_puddle_albedo(float3 D, da_puddle_result P)
{
	D = lerp(D, da_saunderson(D), saturate(P.damp) * 0.55f);
	D *= lerp(1.0f, saturate(da_puddle_look2.z), P.rim);
	return lerp(D, da_saunderson(D), saturate(P.mask) * saturate(1.0f - da_puddle_look.y));
}

#endif // DA_PUDDLES_H
