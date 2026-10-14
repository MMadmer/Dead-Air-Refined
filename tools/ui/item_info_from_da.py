# Writes the item hint of the actor menu - actor_menu_item(_16).xml, af_params(_16).xml and
# booster_params(_16).xml - for the compatibility overlay from Dead Air's own files: every node stays
# where Dead Air put it, every texture becomes a look the xms_ui_shape shader draws (ui_theme.xml) and
# every property icon a pictogram of the same meaning.
#
#   python tools\ui\item_info_from_da.py <unpacked Dead Air root> [<unpacked root> ...]
#
# Each root is an unpacked archive with configs\ui under it; the first one that has a file wins, so
# list Revolution II before the base game the way the archives mount.
import os
import re
import sys

OUT = os.path.join(os.path.dirname(__file__), '..', '..', 'packaging', 'dead-air-x64', 'compatibility',
                   'gamedata', 'configs', 'ui')
FILES = ['actor_menu_item.xml', 'actor_menu_item_16.xml', 'af_params.xml', 'af_params_16.xml',
         'booster_params.xml', 'booster_params_16.xml']

# a property's pictogram by the node it captions; Dead Air swapped the bullet and the blast icons
BY_NODE = {
    'fire_wound_immunity': 'bullet', 'explosion_immunity': 'burst', 'strike_immunity': 'impact',
    'wound_immunity': 'claw', 'burn_immunity': 'flame', 'shock_immunity': 'bolt',
    'chemical_burn_immunity': 'biohazard', 'radiation_immunity': 'trefoil', 'telepatic_immunity': 'psi',
    'health_restore_speed': 'heart', 'radiation_restore_speed': 'trefoil', 'satiety_restore_speed': 'cutlery',
    'power_restore_speed': 'runner', 'bleeding_restore_speed': 'drop', 'additional_weight': 'weight',
    'condition': 'wrench', 'boost_health_restore': 'heart', 'boost_radiation_restore': 'trefoil',
    'boost_satiety': 'cutlery', 'boost_bleeding': 'drop', 'boost_radiation': 'trefoil',
    'boost_anabiotic': 'moon', 'boost_power_restore': 'runner', 'boost_bleeding_restore': 'drop',
    'boost_radiation_protection': 'trefoil', 'boost_telepat_protection': 'psi',
    'boost_chemburn_protection': 'biohazard', 'boost_burn_immunity': 'flame', 'boost_shock_immunity': 'bolt',
    'boost_radiation_immunity': 'trefoil', 'boost_telepat_immunity': 'psi', 'boost_chemburn_immunity': 'biohazard',
    'boost_max_weight': 'weight', 'boost_time': 'clock',
    'static_condition': 'wrench', 'static_accuracy': 'crosshair', 'static_handling': 'hand',
    'static_damage': 'burst', 'static_rpm': 'gauge', 'static_reliability': 'shield', 'static_ammo': 'cartridge',
}
# the rest by the texture it replaces
BY_TEXTURE = {
    'ui_am_propery_09': 'trefoil', 'ui_am_propery_11': 'psi', 'ui_am_prop_chem': 'biohazard',
    'ui_am_prop_thermo': 'flame', 'ui_am_prop_electro': 'bolt', 'ui_am_prop_restore_bleeding': 'drop',
    'ui_am_propery_08': 'weight', 'ui_am_propery_07': 'runner', 'ui_am_propery_05': 'heart',
    'ui_am_prop_satiety_restore_speed': 'cutlery', 'ui_am_prop_strike': 'impact', 'ui_am_prop_explo': 'burst',
    'ui_am_prop_wound': 'claw', 'ui_am_prop_damage': 'claw', 'ui_am_propery_01': 'bullet',
    'ui_am_prop_time_period': 'clock', 'ui_am_prop_vibros': 'moon', 'ui_am_condition': 'wrench',
    'ui_am_prop_condition': 'wrench', 'ui_wp_prop_condition': 'wrench', 'ui_wp_propery_07': 'cartridge',
    'ui_wp_prop_tochnost': 'crosshair', 'ui_wp_prop_skorostrelnost': 'gauge', 'ui_wp_prop_ergonomics': 'hand',
    'ui_wp_prop_damage': 'burst', 'ui_wp_prop_nadezhnost': 'shield', 'ui_upgrade_arrow2': 'upgrade',
    'ui_icons_newpda_perssign_h': 'pin',
}
# textures that are surfaces, by the look of the theme that replaces them
SURFACES = {
    'ui_ingame2_hint_wnd_main_window': 'tooltip',
    'ui_ingame2_hint_wnd_properties': 'tooltip_rule',
    'ui_ingame2_hint_wnd_information': 'tooltip_rule',
    'ui_ingame2_hint_wnd_upgrades': 'tooltip_rule',
    'ui_item_count_back': 'badge',
}
BARS = {
    'ui_ingame2_hint_wnd_bar': 'tip_bar_track', 'ui_ingame2_hint_wnd_bar_16': 'tip_bar_track',
    'ui_ingame2_hint_wnd_bar_alfa_line': 'tip_bar_fill', 'ui_ingame2_hint_wnd_bar_alfa_line_16': 'tip_bar_fill',
    'ui_buymenu_progbar': 'tip_bar_fill',
}
CELL_BAR = {'ui_ingame2_inventory_item_status_bar', 'ui_ingame2_inventory_item_status_bar_16'}

TEXTURE = re.compile(r'<texture(\s[^>]*)?>\s*([^<]*?)\s*</texture>')
OPEN = re.compile(r'<([A-Za-z_][\w]*)\b[^>]*?(/?)>')


def enclosing(text, pos):
    """The names of the elements open at pos, innermost last."""
    stack = []
    for m in re.finditer(r'<(/?)([A-Za-z_][\w]*)\b[^>]*?(/?)>', text[:pos]):
        closing, name, selfclose = m.group(1), m.group(2), m.group(3)
        if closing:
            while stack and stack.pop() != name:
                pass
        elif not selfclose:
            stack.append(name)
    return stack


def convert(text):
    out = []
    last = 0
    for m in TEXTURE.finditer(text):
        name = m.group(2).strip()
        key = name.lower()
        stack = enclosing(text, m.start())
        owner = stack[-1] if stack else ''
        parent = stack[-2] if len(stack) > 1 else ''
        shape = None
        if key in SURFACES:
            shape = '<shape style="%s"/>' % SURFACES[key]
        elif key in BARS:
            shape = '<shape style="%s"/>' % BARS[key]
        elif key in CELL_BAR:
            shape = '<shape style="%s"/>' % ('bar_thin_fill' if owner == 'progress' else 'bar_thin_track')
        else:
            # a caption's icon names its property by the node above it
            node = parent if owner == 'caption' else owner
            kind = BY_NODE.get(node) or BY_TEXTURE.get(key)
            if kind:
                fill = '$accent' if kind == 'upgrade' else '$text_dim'
                shape = '<shape kind="%s" fill="%s"/>' % (kind, fill)
        if shape is None:
            continue
        out.append(text[last:m.start()])
        out.append(shape)
        last = m.end()
    out.append(text[last:])
    text = ''.join(out)
    # A rule was a line drawn inside a taller texture: the shape is the line itself, one unit
    # high across the middle of where the texture was.
    def thin(m):
        attrs = m.group(2)
        y = re.search(r'(?<![\w])y="([-\d.]+)"', attrs)
        h = re.search(r'(?<![\w])height="([-\d.]+)"', attrs)
        if y and h:
            mid = float(y.group(1)) + max(float(h.group(1)) - 1.0, 0.0) / 2.0
            attrs = attrs.replace(y.group(0), 'y="%s"' % ('%g' % mid)).replace(h.group(0), 'height="1"')
        return '<%s%s>%s' % (m.group(1), attrs, m.group(3))
    text = re.sub(r'<(\w+)(\s[^>]*)>(\s*<shape style="tooltip_rule"/>)', thin, text)
    # Dead Air's greys, by the colours of the theme
    text = re.sub(r'r="170"\s+g="170"\s+b="170"', 'color="$text_dim"', text)
    text = re.sub(r'r="210"\s+g="210"\s+b="210"', 'color="$text"', text)
    text = re.sub(r'r="220"\s+g="220"\s+b="220"', 'color="$text"', text)
    return text


def main(roots):
    for name in FILES:
        source = next((os.path.join(r, 'configs', 'ui', name) for r in roots
                       if os.path.isfile(os.path.join(r, 'configs', 'ui', name))), None)
        if not source:
            print('missing', name)
            continue
        raw = open(source, 'rb').read()
        text = raw.decode('cp1251')
        header = ('<!-- Dead Air\'s %s with every texture drawn by the xms_ui_shape shader (ui_theme.xml):\n'
                  '     written by tools\\ui\\item_info_from_da.py; the nodes are where Dead Air put them. -->\n' % name)
        result = header + convert(text)
        with open(os.path.join(OUT, name), 'wb') as f:
            f.write(result.encode('cp1251'))
        print('wrote', name, 'from', source)


if __name__ == '__main__':
    main(sys.argv[1:])
