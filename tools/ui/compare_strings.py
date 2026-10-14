# Writes configs\text\{rus,eng}\dead_air_x64_compare.xml of the compatibility overlay: the strings of the
# inventory hotkeys, their panel, the item comparison, the hint of an item as new and the hints of the
# actor menu's state panel. Russian goes out in windows-1251 like the rest of the game's text.
#
#   python tools\ui\compare_strings.py
import os
from xml.sax.saxutils import escape

ROOT = os.path.join(os.path.dirname(__file__), '..', '..', 'packaging', 'dead-air-x64', 'compatibility',
                    'gamedata', 'configs', 'text')

S = [
    # The player is addressed formally: impersonal or polite, never familiar.

    # the panel of hotkeys
    ('st_da_hk_title', 'Горячие клавиши', 'Hotkeys'),
    ('st_da_hk_ctrl', 'Ctrl', 'Ctrl'),
    ('st_da_hk_alt', 'Alt', 'Alt'),
    ('st_da_hk_shift', 'Shift', 'Shift'),
    ('st_da_hk_hover', 'наведение', 'hover'),
    ('st_da_hk_lmb', 'ЛКМ', 'LMB'),
    ('st_da_hk_rmb', 'ПКМ', 'RMB'),
    ('st_da_hk_mmb', 'СКМ', 'MMB'),
    ('st_da_hk_wheel_up', 'колесо вверх', 'wheel up'),
    ('st_da_hk_wheel_down', 'колесо вниз', 'wheel down'),
    ('st_da_hk_hold', 'удержание', 'hold'),
    ('st_da_hk_dbl', 'двойной щелчок', 'double click'),
    ('st_da_hk_drag', 'перетаскивание', 'drag'),
    ('st_da_hk_hover_move_trade', 'Переместить стопку целиком: из рюкзака на стол сделки и обратно; товар торговца - на его стол и обратно',
     'Move a whole stack: from the rucksack onto the table and back; the trader\'s goods onto his table and back'),
    ('st_da_hk_hover_move_loot', 'Переместить стопку целиком в рюкзак или обратно', 'Move a whole stack into the rucksack or back'),
    ('st_da_hk_equip', 'Экипировать предмет или убрать его в рюкзак', 'Equip an item or put it back in the rucksack'),
    ('st_da_hk_move_one_trade', 'Переместить один предмет из стопки на стол сделки или обратно',
     'Move one item of a stack onto the table or back'),
    ('st_da_hk_move_one_loot', 'Переместить один предмет из стопки в рюкзак или обратно', 'Move one item of a stack into the rucksack or back'),
    ('st_da_hk_split_inventory', 'Разделить пачку патронов или составной предмет на две части',
     'Split an ammo box or a stacked item in two'),
    ('st_da_hk_split_trade', 'Переместить половину стопки на стол сделки или обратно; пачку патронов - разделить на две части',
     'Move half of a stack onto the table or back; split an ammo box in two'),
    ('st_da_hk_split_loot', 'Переместить на другую сторону половину стопки или половину патронов пачки',
     'Move half of a stack, or half of an ammo box\'s rounds, to the other side'),
    ('st_da_hk_drop_stack', 'Выбросить стопку целиком', 'Drop a whole stack'),
    ('st_da_hk_pick', 'Отметить предмет для сравнения (сравниваются только предметы одного типа)',
     'Pick an item for a comparison (only items of one kind are compared)'),
    ('st_da_hk_compare', 'Сравнить отмеченные предметы', 'Compare the picked items'),
    ('st_da_hk_clear_picks', 'Снять все отметки', 'Clear all picks'),
    ('st_da_hk_pristine', 'Показать рядом характеристики предмета в идеальном состоянии (100%)',
     'Show beside it the item\'s characteristics in perfect condition (100%)'),
    ('st_da_hk_dbl_inventory', 'Использовать, экипировать или снять предмет', 'Use, equip or take off an item'),
    ('st_da_hk_dbl_trade', 'Переместить один предмет на стол сделки или обратно', 'Move one item onto the table or back'),
    ('st_da_hk_dbl_loot', 'Переместить один предмет в рюкзак или обратно', 'Move one item into the rucksack or back'),
    ('st_da_hk_drag_caption', 'Переместить один предмет в выбранное место', 'Move one item to the chosen place'),
    ('st_da_hk_context', 'Меню действий с предметом', 'Item actions menu'),
    ('st_da_hk_drop_key', 'Выбросить выбранный предмет', 'Drop the selected item'),
    ('st_da_hk_take_all_trade', 'Совершить сделку', 'Make the deal'),
    ('st_da_hk_take_all_loot', 'Забрать все предметы', 'Take all items'),
    ('st_da_hk_give_all_trade', 'Совершить продажу', 'Make the sale'),
    ('st_da_hk_give_all_loot', 'Переложить все предметы из рюкзака', 'Put all items from the rucksack'),
    ('st_da_hk_panel', 'Показать или скрыть панель горячих клавиш', 'Show or hide the hotkeys panel'),
    ('st_da_hk_button_hint', 'Горячие клавиши (H)', 'Hotkeys (H)'),
    ('st_da_cmp_button_hint', 'Сравнить отмеченные предметы (V). Отметка: СКМ или C + ЛКМ',
     'Compare the picked items (V). To pick: MMB or C + LMB'),
    ('st_da_pristine_suffix', '(100%)', '(100%)'),
    ('st_da_weight_hint', 'Текущий вес: %.1f кг\\nДо %.1f кг - без перегрузки.\\nДо %.1f кг - перегрузка: выносливость расходуется быстрее.\\nПри заполненной шкале - полная перегрузка: скорость передвижения снижается.',
     'Current weight: %.1f kg\\nUp to %.1f kg - no overload.\\nUp to %.1f kg - overload: stamina is spent faster.\\nA full bar is a full overload: movement slows down.'),

    # the state panel of the actor menu: what each bar and sign does in the game, briefly
    ('st_da_state_health', 'Здоровье\\nПри нуле персонаж погибает. Восстанавливается медленно, если персонаж сыт, и быстро - медикаментами.',
     'Health\\nAt zero the character dies. It comes back slowly while the character is fed, and quickly with medicine.'),
    ('st_da_state_bleeding', 'Кровотечение\\nОтнимает здоровье, пока не остановлено: чем сильнее рана, тем быстрее.',
     'Bleeding\\nTakes health until it is stopped, the faster the worse the wound.'),
    ('st_da_state_satiety', 'Сытость\\nЧем она ниже, тем хуже восстанавливаются здоровье и силы; на нуле здоровье убывает.',
     'Satiety\\nThe lower it is, the worse health and stamina recover; at zero, health drains.'),
    ('st_da_measured_dose_hint', 'Последняя измеренная доза: %s%d мЗв%s (%s)\\nРадиация постепенно отнимает здоровье, пока её не выведут антирадиационные средства.',
     'Last measured dose: %s%d mSv%s (%s)\\nRadiation slowly takes health until antiradiation means remove it.'),
    ('st_da_measured_dose_none', 'Доза ещё не измерялась\\nДозиметр показывает накопленную дозу; последняя измеренная остаётся здесь.',
     'The dose has not been measured yet\\nA dosimeter shows the dose taken; the last measured one stays here.'),
    ('st_da_state_radiation', 'Защита от радиации\\nСнижает дозу, получаемую в заражённых местах и от радиоактивных предметов.',
     'Radiation protection\\nLowers the dose taken in contaminated places and from radioactive items.'),
    ('st_da_state_psi', 'Пси-защита\\nСнижает урон от пси-излучения и пси-атак мутантов.',
     'Psi protection\\nLowers the damage of psi emissions and of mutants\' psi attacks.'),
    ('st_da_state_chemical', 'Защита от химических ожогов\\nСнижает урон кислотных аномалий и химических атак.',
     'Chemical protection\\nLowers the damage of acid anomalies and chemical attacks.'),
    ('st_da_state_wound', 'Защита от разрывов\\nСнижает урон когтей и зубов мутантов, ножей и разрывающих аномалий.',
     'Rupture protection\\nLowers the damage of mutants\' claws and teeth, of knives and of tearing anomalies.'),
    ('st_da_state_shock', 'Защита от электричества\\nСнижает урон электрических аномалий.',
     'Shock protection\\nLowers the damage of electric anomalies.'),
    ('st_da_state_fire_wound', 'Пулестойкость\\nСнижает урон от пуль и осколков.',
     'Bullet protection\\nLowers the damage of bullets and fragments.'),
    ('st_da_state_burn', 'Защита от огня\\nСнижает урон огненных аномалий и пламени.',
     'Burn protection\\nLowers the damage of fire anomalies and flames.'),
    ('st_da_state_power', 'Выносливость\\nВлияние экипировки на восстановление сил.',
     'Stamina\\nHow the equipment affects the recovery of stamina.'),

    # the comparison
    ('st_cmp_title', 'Сравнение: %s (%u)', 'Comparison: %s (%u)'),
    ('st_cmp_profile_use', 'Использование', 'To keep'),
    ('st_cmp_profile_sale', 'Продажа', 'To sell'),
    ('st_cmp_profile_use_hint', 'Выбор предмета для использования. Слева - доля параметра в итоговой оценке',
     'The item to keep. On the left: each parameter\'s share of the score'),
    ('st_cmp_profile_sale_hint', 'Выбор предмета для продажи. Слева - доля параметра в итоговой оценке',
     'The item to sell. On the left: each parameter\'s share of the score'),
    ('st_cmp_pairs', 'Попарное сравнение: разница оценок (строка минус столбец)', 'Pairwise: the difference of the scores (row minus column)'),
    ('st_cmp_winner', 'Рекомендуемый вариант: %u. %s', 'Recommended: %u. %s'),
    ('st_cmp_reasons', 'основное преимущество:', 'main advantage:'),
    ('st_cmp_no_weights', 'Для предметов этого типа в данном режиме оценка не ведётся', 'Items of this kind are not scored in this mode'),
    ('st_cmp_yes', 'есть', 'yes'),
    ('st_cmp_no', 'нет', 'no'),
    ('st_cmp_unit_kg', 'кг', 'kg'),
    ('st_cmp_unit_sec', 'с', 's'),
    ('st_cmp_unit_mps', 'м/с', 'm/s'),

    ('st_cmp_kind_none', 'предметы', 'items'),
    ('st_cmp_kind_firearm', 'оружие', 'firearms'),
    ('st_cmp_kind_melee', 'ножи', 'knives'),
    ('st_cmp_kind_outfit', 'костюмы', 'suits'),
    ('st_cmp_kind_helmet', 'шлемы', 'helmets'),
    ('st_cmp_kind_backpack', 'рюкзаки', 'rucksacks'),
    ('st_cmp_kind_artefact', 'артефакты', 'artefacts'),
    ('st_cmp_kind_ammo', 'боеприпасы', 'ammunition'),
    ('st_cmp_kind_grenade', 'гранаты', 'grenades'),
    ('st_cmp_kind_addon', 'навесное оборудование', 'weapon addons'),
    ('st_cmp_kind_detector', 'приборы', 'devices'),
    ('st_cmp_kind_food', 'еда и напитки', 'food and drink'),
    ('st_cmp_kind_medicine', 'медицина', 'medicine'),
    ('st_cmp_kind_misc', 'прочие предметы', 'other items'),

    # rows
    ('st_cmp_p_damage', 'Урон', 'Damage'),
    ('st_cmp_p_damage_heavy', 'Урон сильного удара', 'Heavy strike damage'),
    ('st_cmp_p_rpm', 'Скорострельность, выстр./мин', 'Rate of fire, rounds/min'),
    ('st_cmp_p_accuracy', 'Разброс', 'Dispersion'),
    ('st_cmp_p_recoil', 'Отдача', 'Recoil'),
    ('st_cmp_p_handling', 'Инерция', 'Inertia'),
    ('st_cmp_p_aim_time', 'Время прицеливания', 'Time to aim'),
    ('st_cmp_p_mag_size', 'Магазин', 'Magazine'),
    ('st_cmp_p_bullet_speed', 'Настильность (скорость пули)', 'Flatness (bullet speed)'),
    ('st_cmp_p_endurance', 'Ресурс, выстр. на 10% износа', 'Endurance, shots per 10% wear'),
    ('st_cmp_p_misfire', 'Вероятность осечки', 'Misfire chance'),
    ('st_cmp_p_fouled', 'Загрязнения', 'Fouled parts'),
    ('st_cmp_p_deformed', 'Износ и деформации', 'Worn parts'),
    ('st_cmp_p_broken', 'Поломки', 'Broken parts'),
    ('st_cmp_p_addons', 'Установленный обвес', 'Attached addons'),
    ('st_cmp_p_mounts', 'Крепления под обвес', 'Addon mounts'),
    ('st_cmp_p_upgrades', 'Улучшения', 'Upgrades'),
    ('st_cmp_p_ammo_cost', 'Цена патрона', 'Price of a round'),
    ('st_cmp_p_condition', 'Состояние', 'Condition'),
    ('st_cmp_p_weight', 'Вес', 'Weight'),
    ('st_cmp_p_price', 'Цена', 'Price'),
    ('st_cmp_p_price_per_kg', 'Цена за кг', 'Price per kg'),
    ('st_cmp_p_uses', 'Осталось порций', 'Portions left'),
    ('st_cmp_p_armor', 'Пулестойкость', 'Bullet protection'),
    ('st_cmp_p_prot_burn', 'Термозащита', 'Burn protection'),
    ('st_cmp_p_prot_shock', 'Электрозащита', 'Shock protection'),
    ('st_cmp_p_prot_chemical', 'Химзащита', 'Chemical protection'),
    ('st_cmp_p_prot_radiation', 'Радиозащита', 'Radiation protection'),
    ('st_cmp_p_prot_psi', 'Пси-защита', 'Psi protection'),
    ('st_cmp_p_prot_strike', 'Защита от ударов', 'Impact protection'),
    ('st_cmp_p_prot_wound', 'Защита от разрывов', 'Rupture protection'),
    ('st_cmp_p_prot_explosion', 'Защита от взрывов', 'Explosion protection'),
    ('st_cmp_p_artefact_slots', 'Слоты артефактов', 'Artefact slots'),
    ('st_cmp_p_carry', 'Переносимый вес', 'Carry weight'),
    ('st_cmp_p_power_loss', 'Расход выносливости', 'Stamina use'),
    ('st_cmp_p_night_vision', 'Ночное видение', 'Night vision'),
    ('st_cmp_p_restore_health', 'Здоровье', 'Health'),
    ('st_cmp_p_restore_power', 'Выносливость', 'Stamina'),
    ('st_cmp_p_restore_bleeding', 'Заживление ран', 'Wound healing'),
    ('st_cmp_p_restore_satiety', 'Сытость', 'Satiety'),
    ('st_cmp_p_restore_radiation', 'Радиация', 'Radiation'),
    ('st_cmp_p_imm_burn', 'Термозащита', 'Burn protection'),
    ('st_cmp_p_imm_shock', 'Электрозащита', 'Shock protection'),
    ('st_cmp_p_imm_chemical', 'Химзащита', 'Chemical protection'),
    ('st_cmp_p_imm_radiation', 'Радиозащита', 'Radiation protection'),
    ('st_cmp_p_imm_psi', 'Пси-защита', 'Psi protection'),
    ('st_cmp_p_imm_strike', 'Защита от ударов', 'Impact protection'),
    ('st_cmp_p_imm_wound', 'Защита от разрывов', 'Rupture protection'),
    ('st_cmp_p_imm_explosion', 'Защита от взрывов', 'Explosion protection'),
    ('st_cmp_p_imm_fire_wound', 'Пулестойкость', 'Bullet protection'),
    ('st_cmp_p_rounds', 'Число патронов', 'Rounds'),
    ('st_cmp_p_round_cost', 'Цена патрона', 'Price of a round'),
    ('st_cmp_p_k_hit', 'Урон', 'Damage'),
    ('st_cmp_p_k_ap', 'Бронебойность', 'Armour piercing'),
    ('st_cmp_p_k_disp', 'Разброс', 'Dispersion'),
    ('st_cmp_p_k_air_resistance', 'Сопротивление воздуха', 'Air drag'),
    ('st_cmp_p_impair', 'Износ оружия', 'Weapon wear'),
    ('st_cmp_p_blast', 'Урон взрыва', 'Blast damage'),
    ('st_cmp_p_blast_r', 'Радиус взрыва, м', 'Blast radius, m'),
    ('st_cmp_p_frags', 'Число осколков', 'Fragments'),
    ('st_cmp_p_frag_hit', 'Урон осколка', 'Fragment damage'),
    ('st_cmp_p_frags_r', 'Разлёт осколков, м', 'Fragment range, m'),
    ('st_cmp_p_zoom', 'Кратность', 'Magnification'),
    ('st_cmp_p_af_radius', 'Радиус обнаружения, м', 'Detection radius, m'),
    ('st_cmp_p_satiety', 'Сытость', 'Satiety'),
    ('st_cmp_p_thirst', 'Утоление жажды', 'Thirst'),
    ('st_cmp_p_health', 'Здоровье', 'Health'),
    ('st_cmp_p_power', 'Выносливость', 'Stamina'),
    ('st_cmp_p_radiation', 'Радиация', 'Radiation'),
    ('st_cmp_p_alcohol', 'Опьянение', 'Intoxication'),
    ('st_cmp_p_heal_rate', 'Лечение со временем', 'Healing over time'),
    ('st_cmp_p_bleeding', 'Остановка кровотечения', 'Stops bleeding'),
    ('st_cmp_p_bleeding_rate', 'Заживление со временем', 'Wound healing over time'),
    ('st_cmp_p_radiation_rate', 'Вывод радиации', 'Radiation purge'),
    ('st_cmp_p_rad_protection', 'Радиозащита (действие)', 'Radiation protection (effect)'),
    ('st_cmp_p_psi_protection', 'Пси-защита (действие)', 'Psi protection (effect)'),
    ('st_cmp_p_chem_protection', 'Химзащита (действие)', 'Chemical protection (effect)'),
    ('st_cmp_p_boost_time', 'Время действия, с', 'Effect time, s'),
]


def write(lang, index, encoding):
    lines = ['<?xml version="1.0" encoding="%s"?>' % encoding, '<string_table>']
    for row in S:
        lines.append('\t<string id="%s">' % row[0])
        lines.append('\t\t<text>%s</text>' % escape(row[index]))
        lines.append('\t</string>')
    lines.append('</string_table>')
    path = os.path.join(ROOT, lang, 'dead_air_x64_compare.xml')
    with open(path, 'w', encoding=encoding, newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('wrote', path)


def main():
    ids = [row[0] for row in S]
    assert len(ids) == len(set(ids)), 'a string twice'
    write('rus', 1, 'windows-1251')
    write('eng', 2, 'windows-1251')


if __name__ == '__main__':
    main()
