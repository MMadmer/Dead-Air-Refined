# Writes configs\text\{rus,eng}\dead_air_x64_colors.xml of the compatibility overlay: the strings of the
# interface colours window (ui_interface_colors*.xml, tools\ui\interface_colors_layout.py), of the templates
# it previews on, and of its row in Options -> Game -> Interface. The Russian file is windows-1251.
#
#   python tools\ui\interface_colors_strings.py
import os

ROOT = os.path.join(os.path.dirname(__file__), '..', '..', 'packaging', 'dead-air-x64', 'compatibility',
                    'gamedata', 'configs', 'text')

STRINGS = [
    # the row of the options
    ('ui_mm_interface_colors', 'Цвета интерфейса', 'Interface colours'),
    ('ui_mm_interface_colors_hint',
     'Основной и акцентный цвета окон игры: инвентаря, торговли, обыска, диалога и подсказок. Меню сохраняют свои.',
     'The main and the accent colour of the game\'s windows: the inventory, the trade, the search, the dialog and the hints. The menus keep their own.'),
    ('st_da_colors_open', 'Настроить', 'Set up'),
    # the window
    ('st_da_colors_title', 'Цвета интерфейса', 'Interface colours'),
    ('st_da_colors_subtitle',
     'Цвета окон игры: инвентаря, торговли, обыска, диалога и подсказок. Меню сохраняют свои.',
     'The colours of the game\'s windows: the inventory, the trade, the search, the dialog and the hints. The menus keep their own.'),
    ('st_da_colors_template', 'Шаблон', 'Template'),
    ('st_da_colors_main', 'Основной', 'Main'),
    ('st_da_colors_accent', 'Акцентный', 'Accent'),
    ('st_da_colors_old', 'Старый', 'Old'),
    ('st_da_colors_new', 'Новый', 'New'),
    ('st_da_colors_r', 'R', 'R'),
    ('st_da_colors_g', 'G', 'G'),
    ('st_da_colors_b', 'B', 'B'),
    ('st_da_colors_h', 'H', 'H'),
    ('st_da_colors_s', 'S', 'S'),
    ('st_da_colors_v', 'V', 'V'),
    ('st_da_colors_hex_linear', 'Hex Linear', 'Hex Linear'),
    ('st_da_colors_hex_srgb', 'Hex sRGB', 'Hex sRGB'),
    ('st_da_colors_reset', 'По умолчанию', 'Defaults'),
    ('st_da_colors_cancel', 'Отмена', 'Cancel'),
    ('st_da_colors_ok', 'Применить', 'Apply'),
    # the templates
    ('st_da_colors_tpl_inventory', 'Инвентарь', 'Inventory'),
    ('st_da_colors_tpl_trade', 'Торговля', 'Trade'),
    ('st_da_colors_tpl_dialog', 'Диалог', 'Dialog'),
    ('st_da_colors_tpl_stalker', 'Сталкер', 'Stalker'),
    ('st_da_colors_tpl_trader', 'Сидорович', 'Sidorovich'),
    ('st_da_colors_tpl_faction', 'Группировка', 'Faction'),
    ('st_da_colors_tpl_loners', 'Одиночки', 'Loners'),
    ('st_da_colors_tpl_traders', 'Торговцы', 'Traders'),
    ('st_da_colors_tpl_relation', 'Отношение', 'Attitude'),
    ('st_da_colors_tpl_neutral', 'нейтральное', 'neutral'),
    ('st_da_colors_tpl_money', '15000 RU', '15000 RU'),
    ('st_da_colors_tpl_trader_money', '48500 RU', '48500 RU'),
    ('st_da_colors_tpl_by_type', 'По типу', 'By type'),
    ('st_da_colors_tpl_weight', 'Вес: 23.4 кг (макс. 75 кг)', 'Weight: 23.4 kg (max 75 kg)'),
    ('st_da_colors_tpl_f1', 'F1', 'F1'),
    ('st_da_colors_tpl_f2', 'F2', 'F2'),
    ('st_da_colors_tpl_f3', 'F3', 'F3'),
    ('st_da_colors_tpl_f4', 'F4', 'F4'),
    ('st_da_colors_tpl_exit', 'Выход', 'Exit'),
    ('st_da_colors_tpl_ak', 'АК-74', 'AK-74'),
    ('st_da_colors_tpl_ak_weight', '3.60 кг', '3.60 kg'),
    ('st_da_colors_tpl_ak_text', 'Надёжный автомат под патрон 5,45x39. Прощает грязь и небрежный уход.',
     'A reliable rifle for the 5.45x39 round. It forgives dirt and careless upkeep.'),
    ('st_da_colors_tpl_accuracy', 'Точность', 'Accuracy'),
    ('st_da_colors_tpl_handling', 'Удобство', 'Handling'),
    ('st_da_colors_tpl_damage', 'Урон', 'Damage'),
    ('st_da_colors_tpl_rate', 'Темп огня', 'Rate of fire'),
    ('st_da_colors_tpl_ak_price', '6250 RU', '6250 RU'),
    ('st_da_colors_tpl_sum0', 'Продажа: 1200 RU', 'Selling: 1200 RU'),
    ('st_da_colors_tpl_sum1', 'Покупка: 3400 RU', 'Buying: 3400 RU'),
    ('st_da_colors_tpl_trade_button', 'Обменять', 'Trade'),
    ('st_da_colors_tpl_sell', 'Продать', 'Sell'),
    ('st_da_colors_tpl_describe', 'Описание', 'Description'),
    ('st_da_colors_tpl_attach', 'Присоединить', 'Attach'),
    ('st_da_colors_tpl_drop', 'Выбросить', 'Drop'),
    ('st_da_colors_tpl_line1', 'Ну, здравствуй, сталкер. С чем пожаловал?', 'Well, hello, stalker. What brings you here?'),
    ('st_da_colors_tpl_line2', 'Есть товар. Посмотришь?', 'I have goods. Will you take a look?'),
    ('st_da_colors_tpl_line3', 'Показывай, сговоримся.', 'Show me, we will come to terms.'),
    ('st_da_colors_tpl_ask1', 'Что нового в Зоне?', 'What is new in the Zone?'),
    ('st_da_colors_tpl_ask2', 'Давай поторгуем.', 'Let us trade.'),
    ('st_da_colors_tpl_ask3', 'Есть для меня работа?', 'Got any work for me?'),
    ('st_da_colors_tpl_ask4', 'Мне пора.', 'I have to go.'),
    ('st_da_colors_tpl_n1', '1.', '1.'),
    ('st_da_colors_tpl_n2', '2.', '2.'),
    ('st_da_colors_tpl_n3', '3.', '3.'),
    ('st_da_colors_tpl_n4', '4.', '4.'),
]


def write(lang, encoding, index):
    lines = ['<?xml version="1.0" encoding="%s"?>' % encoding,
             '<!-- The interface colours: written by tools\\ui\\interface_colors_strings.py. -->',
             '<string_table>']
    for row in STRINGS:
        lines.append('\t<string id="%s">' % row[0])
        lines.append('\t\t<text>%s</text>' % row[index].replace('&', '&amp;').replace('<', '&lt;'))
        lines.append('\t</string>')
    lines.append('</string_table>')
    path = os.path.join(ROOT, lang, 'dead_air_x64_colors.xml')
    with open(path, 'w', encoding=encoding, newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('wrote', path)


if __name__ == '__main__':
    write('rus', 'windows-1251', 1)
    write('eng', 'utf-8', 2)
