# Writes configs\ui\ui_interface_colors.xml (4:3) and ui_interface_colors_16.xml (16:9) of the compatibility
# overlay: the window of the interface colours (Options -> Game -> Interface; xrGame ui/UIInterfaceColorsWnd)
# and the templates it previews the colours on. Lengths are screen units: 768 to the screen's height, as many
# across as the aspect gives (1024 for 4:3, 1365.33 for 16:9), squeezed into 1024 on the way out like
# actor_menu_layout.py does.
#
#   python tools\ui\interface_colors_layout.py
#
# The window itself wears the menus' palette (sys_*): the colours it sets are the game windows'. The templates
# wear the game windows' styles and colours, and an <item section="..."> is the item's own icon.
import os

ROOT = os.path.join(os.path.dirname(__file__), '..', '..', 'packaging', 'dead-air-x64', 'compatibility',
                    'gamedata', 'configs', 'ui')

CELL = 41.0  # an item grid cell, screen units


def num(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


class Layout:
    def __init__(self, wide):
        self.kx = 0.75 if wide else 1.0
        self.W = 1024.0 / self.kx
        self.out = []
        self.frame_x = 40.0 if wide else 20.0
        self.picker_w = 400.0 if wide else 330.0
        self.picker_x = self.W - self.frame_x - 24.0 - self.picker_w
        self.preview = (self.frame_x + 24.0, 120.0, self.picker_x - 24.0 - (self.frame_x + 24.0), 584.0)

    def rect(self, x, y, w, h):
        return 'x="%s" y="%s" width="%s" height="%s"' % (num(x * self.kx), num(y), num(w * self.kx), num(h))

    def w(self, line='', depth=1):
        self.out.append('    ' * depth + line if line else '')

    # -- the pieces a template is made of, relative to the template -------------------------------------------

    def shape(self, x, y, w, h, style, depth=3, extra=''):
        self.w('<auto_static %s shape="%s"%s/>' % (self.rect(x, y, w, h), style, extra), depth)

    def shape_over(self, x, y, w, h, style, attrs, depth=3):
        self.w('<auto_static %s><shape style="%s" %s/></auto_static>' % (self.rect(x, y, w, h), style, attrs), depth)

    def glyph(self, x, y, size, kind, color, depth=3):
        self.w('<auto_static %s><shape kind="%s" fill="%s"/></auto_static>' % (self.rect(x, y, size, size), kind, color), depth)

    def text(self, x, y, w, h, string, font='letterica16', color='$text', align='l', depth=3, complex_mode=False):
        cm = ' complex_mode="1"' if complex_mode else ''
        self.w('<auto_static %s><text font="%s" align="%s" vert_align="c" color="%s"%s>%s</text></auto_static>'
               % (self.rect(x, y, w, h), font, align, color, cm, string), depth)

    def item(self, section, x, y, plate=None, count=None, depth=3, fit=None, turn=False):
        attrs = ' plate="%s"' % plate if plate else ''
        if count:
            attrs += ' count="%s"' % count
        if fit:
            attrs += ' fit_w="%s" fit_h="%s"' % (num(fit[0] * self.kx), num(fit[1]))
        if turn:
            attrs += ' turn="1"'
        self.w('<item section="%s" %s cell="%s"%s/>' % (section, self.rect(x, y, 0, 0).replace(' width="0" height="0"', ''),
                                                       num(CELL), attrs), depth)

    def over(self, start):
        # what floats over the items (a hint, a menu): the window builds it after them
        self.w('<over>' if start else '</over>', 3)

    def bar(self, x, y, w, h, fraction, color, thin=False, depth=3):
        track, fill = ('bar_thin_track', 'bar_thin_fill') if thin else ('bar_track', 'bar_fill')
        self.shape(x, y, w, h, track, depth)
        self.shape_over(x, y, w * fraction, h, fill, 'fill="%s"' % color, depth)

    def button(self, x, y, w, h, style, caption, font='graffiti22', depth=3):
        self.w('<auto_static %s shape="%s"><text font="%s" align="c" vert_align="c">%s</text></auto_static>'
               % (self.rect(x, y, w, h), style, font, caption), depth)

    def tool(self, x, y, size, kind, color, style='tool_button', depth=3):
        self.shape(x, y, size, size, style, depth)
        self.glyph(x + size * 0.18, y + size * 0.18, size * 0.64, kind, color, depth)

    def card(self, x, y, w, name, rows, money=None, depth=3):
        self.shape(x, y, w, 76, 'card', depth)
        self.text(x + 10, y + 6, w - 90, 20, name, 'letterica18', '$text', depth=depth)
        for i, (caption, value) in enumerate(rows):
            self.text(x + 10, y + 28 + 14 * i, w - 90, 14, caption, color='$text_dim', depth=depth)
            self.text(x + 10, y + 28 + 14 * i, w - 96, 14, value, color='$text', align='r', depth=depth)
        self.shape(x + w - 72, y + 9, 60, 58, 'portrait_frame', depth)
        self.glyph(x + w - 62, y + 18, 40, 'gasmask', '$text_muted', depth)
        if money:
            self.text(x + 10, y + 76 - 22, w - 90, 20, money, 'graffiti22', '$text', 'r', depth)

    def grid(self, x, y, cols, rows, items, depth=3):
        for r in range(rows):
            for c in range(cols):
                self.shape(x + c * CELL, y + r * CELL, CELL, CELL, 'cell', depth)
        for section, c, r, plate, count in items:
            self.item(section, x + c * CELL, y + r * CELL, plate, count, depth)

    # -- the templates -------------------------------------------------------------------------------------------

    def tpl_inventory(self, pw, ph):
        left = min(312.0, pw * 0.46)
        center = pw - left - 8.0
        tip_w = 236.0
        # the bag
        self.shape(0, 0, left, ph, 'panel')
        self.card(8, 8, left - 16, 'st_da_colors_tpl_stalker', [('st_da_colors_tpl_faction', 'st_da_colors_tpl_loners')],
                  'st_da_colors_tpl_money')
        y = 92.0
        self.tool(8, y, 22, 'sort', '$text_dim')
        # the drop-down of orders, closed
        self.shape(34, y, 96, 22, 'tool_button')
        self.text(40, y, 70, 22, 'st_da_colors_tpl_by_type', color='$text')
        self.w('<auto_static %s><shape kind="chevron" fill="$text" border="1.5"/></auto_static>'
               % self.rect(34 + 96 - 18, y + 7, 9, 9), 3)
        filters = [('grid', True), ('pistol', False), ('helmet', False), ('cutlery', False), ('medkit', False)]
        for i, (kind, on) in enumerate(filters):
            fx = 136 + i * 26
            if fx + 22 > left - 8:
                break
            if on:
                # the active filter: the checked look of a tab
                self.shape_over(fx, y, 22, 22, 'tab', 'fill="$accent_soft" line="$accent"')
                self.glyph(fx + 22 * 0.18, y + 22 * 0.18, 22 * 0.64, kind, '$accent')
            else:
                self.tool(fx, y, 22, kind, '$text_dim')
        cols = int((left - 16) // CELL)
        rows = int((ph - 124 - 52) // CELL)
        gx = (left - cols * CELL) / 2
        self.grid(gx, 122, cols, rows, [
            ('wpn_ak74', 0, 0, 'cell_selected', None),
            ('ammo_5.45x39_fmj', 0, 2, 'cell_related', '60'),
            ('detector_simple', 2, 2, 'cell_item', None),
            ('grenade_f1', 3, 2, 'cell_blocked', None),
            ('medkit', 0, 3, 'cell_item', None),
            ('bandage', 1, 3, 'cell_item', 'x3'),
            ('bread', 2, 3, 'cell_item', None),
            ('vodka', 3, 3, 'cell_marked', None),
            ('antirad', 4, 3, 'cell_item', None),
        ])
        # the load
        self.text(8, ph - 46, left - 16, 16, 'st_da_colors_tpl_weight', color='$text_dim', align='r')
        self.shape(12, ph - 22, left - 24, 4, 'load_track')
        self.shape_over(12, ph - 22, (left - 24) * 0.42, 4, 'load_fill', 'fill="$good"')
        self.shape(12 + (left - 24) * 0.8, ph - 26, 1.5, 12, 'load_mark')

        # the equipment
        cx = left + 8
        self.shape(cx, 0, center, ph, 'panel')
        sw = (center - 32) / 3
        self.shape(cx + 8, 8, sw, 44, 'slot')
        self.shape(cx + 16 + sw, 8, sw, 44, 'slot')
        self.glyph(cx + 16 + sw + sw / 2 - 14, 16, 28, 'trefoil', '$text#1F')
        self.shape(cx + 24 + 2 * sw, 8, sw, 44, 'slot')
        self.item('detector_simple', cx + 24 + 2 * sw + 4, 10, fit=(sw - 8, 40))
        tall = 214.0
        # the long ones stand upright in their slots, as the game shows them
        self.shape(cx + 8, 60, sw, tall, 'slot')
        self.item('wpn_toz34', cx + 8 + 6, 66, fit=(sw - 12, tall - 20), turn=True)
        self.shape(cx + 16 + sw, 60, sw, 140, 'slot')
        self.item('novice_outfit', cx + 16 + sw + 6, 66, fit=(sw - 12, 140 - 20))
        self.shape(cx + 24 + 2 * sw, 60, sw, tall, 'slot_highlight')
        self.item('wpn_ak74', cx + 24 + 2 * sw + 6, 66, fit=(sw - 12, tall - 20), turn=True)
        for i in range(3):
            self.shape(cx + 8 + i * (sw + 8), 60 + tall - 8 if i != 1 else 60 + 140 - 8, sw, 3, 'bar_thin_track')
            self.shape_over(cx + 8 + i * (sw + 8), 60 + tall - 8 if i != 1 else 60 + 140 - 8, sw * (0.9 - 0.2 * i), 3,
                            'bar_thin_fill', 'fill="$good"')
        # the quick slots
        qy = 60 + tall + 10
        qw = (center - 16 - 3 * 6) / 4
        for i, section in enumerate(['medkit', 'bandage', 'antirad', 'energy_drink']):
            qx = cx + 8 + i * (qw + 6)
            self.shape(qx, qy, qw, 50, 'inset')
            self.item(section, qx + 4, qy + 3, fit=(qw - 8, 44))
            self.text(qx + 4, qy + 34, 30, 14, 'st_da_colors_tpl_f%d' % (i + 1), color='$text_muted')
        # the state
        sy = qy + 60
        sh = ph - sy - 52
        self.shape(cx + 8, sy, center - 16, sh, 'card')
        self.glyph(cx + 16, sy + 9, 13, 'heart', '$health')
        self.bar(cx + 36, sy + 10, center - 60, 11, 0.92, '$health')
        self.glyph(cx + 16, sy + 28, 13, 'cutlery', '$satiety')
        self.bar(cx + 36, sy + 32, center - 60, 5, 0.7, '$satiety', thin=True)
        self.shape(cx + 16, sy + 46, center - 32, 1, 'rule')
        for i, (kind, value) in enumerate([('trefoil', 0.6), ('biohazard', 0.75), ('bolt', 0.4), ('flame', 0.55)]):
            ry = sy + 54 + i * 18
            if ry + 14 > sy + sh:
                break
            self.glyph(cx + 16, ry, 13, kind, '$text_dim')
            self.bar(cx + 36, ry + 3, center - 60, 7, value, '$protection')
        self.button(cx + 8, ph - 42, center - 16, 34, 'button', 'st_da_colors_tpl_exit')

        # the hint of the chosen item, over the bag's free cells
        self.over(True)
        tx = (left - tip_w) / 2
        ty = 122 + 4 * CELL + 8
        th = 226
        self.shape(tx, ty, tip_w, th, 'tooltip')
        self.text(tx + 10, ty + 8, tip_w - 20, 20, 'st_da_colors_tpl_ak', 'letterica18', '$text')
        self.text(tx + 10, ty + 8, tip_w - 20, 20, 'st_da_colors_tpl_ak_weight', color='$text_dim', align='r')
        self.shape(tx + 10, ty + 32, tip_w - 20, 1, 'tooltip_rule')
        self.text(tx + 10, ty + 38, tip_w - 20, 48, 'st_da_colors_tpl_ak_text', color='$text_dim', complex_mode=True)
        for i, caption in enumerate(['st_da_colors_tpl_accuracy', 'st_da_colors_tpl_handling', 'st_da_colors_tpl_damage',
                                     'st_da_colors_tpl_rate']):
            ry = ty + 96 + i * 22
            self.text(tx + 10, ry, 100, 16, caption, color='$text_dim')
            self.shape(tx + 112, ry + 4, tip_w - 122, 8, 'tip_bar_track')
            self.shape(tx + 112, ry + 4, (tip_w - 122) * (0.8 - 0.12 * i), 8, 'tip_bar_fill')
        self.shape(tx + 10, ty + th - 40, tip_w - 20, 1, 'tooltip_rule')
        self.text(tx + 10, ty + th - 32, tip_w - 20, 22, 'st_da_colors_tpl_ak_price', 'graffiti22', '$text', 'r')
        self.over(False)

    def tpl_trade(self, pw, ph):
        half = (pw - 8) / 2
        for side, (x, name, money) in enumerate([(0, 'st_da_colors_tpl_stalker', 'st_da_colors_tpl_money'),
                                                (half + 8, 'st_da_colors_tpl_trader', 'st_da_colors_tpl_trader_money')]):
            self.shape(x, 0, half, ph, 'panel')
            self.card(x + 8, 8, half - 16, name, [('st_da_colors_tpl_faction', 'st_da_colors_tpl_loners')], money)
            cols = int((half - 16) // CELL)
            gx = x + (half - cols * CELL) / 2
            # the table, then the bag
            self.shape(x + 8, 92, half - 16, 2 * CELL + 8, 'inset')
            self.grid(gx, 96, cols, 2, [('ammo_9x18_fmj', 0, 0, 'cell_item', '50')] if side == 0 else
                      [('wpn_pm', 0, 0, 'cell_item', None)])
            self.shape(x + 8, 96 + 2 * CELL + 10, half - 16, 30, 'inset')
            self.text(x + 16, 96 + 2 * CELL + 14, half - 32, 22, 'st_da_colors_tpl_sum%d' % side, 'letterica18', '$text', 'r')
            rows = int((ph - (96 + 2 * CELL + 48) - 56) // CELL)
            self.grid(gx, 96 + 2 * CELL + 48, cols, rows,
                      [('wpn_ak74', 0, 0, 'cell_item', None), ('medkit', 5 if cols > 5 else 0, 0 if cols > 5 else 2,
                                                                  'cell_selected', None),
                       ('kolbasa', 0, 2, 'cell_item', None), ('conserva', 1, 2, 'cell_item', None),
                       ('bandage', 2, 2, 'cell_item', 'x5')] if side == 0 else
                      [('wpn_mosin', 0, 0, 'cell_item', None), ('stalker_outfit', 0, 2 if cols > 5 else 3,
                                                                  'cell_item', None),
                       ('ammo_12x70_buck', 3, 2, 'cell_related', '20'), ('helm_respirator', 5 if cols > 6 else 3,
                                                                          0 if cols > 6 else 4, 'cell_item', None)])
        self.button(8, ph - 44, half - 16, 34, 'button_primary', 'st_da_colors_tpl_trade_button')
        self.button(half + 16, ph - 44, half - 16, 34, 'button', 'st_da_colors_tpl_exit')
        # a menu over an item
        self.over(True)
        mx, my, mw = 8 + CELL * 2, 96 + 2 * CELL + 48 + CELL, 150.0
        self.shape(mx, my, mw, 4 * 22 + 8, 'menu')
        for i, caption in enumerate(['st_da_colors_tpl_sell', 'st_da_colors_tpl_describe', 'st_da_colors_tpl_attach',
                                     'st_da_colors_tpl_drop']):
            ry = my + 4 + i * 22
            if i == 1:
                self.shape_over(mx + 4, ry, mw - 8, 22, 'menu_row', 'fill="$accent"')
                self.text(mx + 12, ry, mw - 20, 22, caption, color='$text_dark')
            else:
                self.text(mx + 12, ry, mw - 20, 22, caption, color='$text_dim')
        self.over(False)

    def tpl_dialog(self, pw, ph):
        w = min(465.0, pw - 16)
        x = (pw - w) / 2
        self.shape(x, 0, w, ph, 'talk_panel')
        self.card(x + 9, 10, w - 18, 'st_da_colors_tpl_trader', [('st_da_colors_tpl_faction', 'st_da_colors_tpl_traders'),
                                                               ('st_da_colors_tpl_relation', 'st_da_colors_tpl_neutral')])
        top = 94.0
        bottom_h = 150.0
        log_h = ph - top - bottom_h - 8 - 52
        self.shape(x + 9, top, w - 18, log_h, 'talk_inset')
        lines = [('st_da_colors_tpl_trader', '$accent', 'st_da_colors_tpl_line1', '$text'),
                 ('st_da_colors_tpl_stalker', '$text_dim', 'st_da_colors_tpl_line2', '$text_dim'),
                 ('st_da_colors_tpl_trader', '$accent', 'st_da_colors_tpl_line3', '$text')]
        y = top + 8
        for name, ncolor, line, lcolor in lines:
            if y + 58 > top + log_h:
                break
            self.text(x + 15, y, w - 30, 20, name, 'graffiti22', ncolor)
            self.text(x + 31, y + 22, w - 46, 32, line, 'letterica18', lcolor, complex_mode=True)
            y += 62
        by = top + log_h + 8
        self.shape(x + 9, by, w - 18, bottom_h, 'talk_inset')
        for i, (caption, color) in enumerate([('st_da_colors_tpl_ask1', '$text'), ('st_da_colors_tpl_ask2', '$accent'),
                                              ('st_da_colors_tpl_ask3', '$text'), ('st_da_colors_tpl_ask4', '$text_muted')]):
            ry = by + 8 + i * 24
            self.text(x + 15, ry, 14, 22, 'st_da_colors_tpl_n%d' % (i + 1), 'letterica18', '$text_muted')
            self.text(x + 31, ry, w - 46, 22, caption, 'letterica18', color)
        self.button(pw / 2 - 90, ph - 44, 180, 34, 'button_primary', 'st_da_colors_tpl_trade_button')

    # -- the window ----------------------------------------------------------------------------------------------

    def build(self):
        W = self.W
        self.w('<?xml version="1.0" encoding="utf-8"?>', 0)
        self.w('<!-- The interface colours (CUIInterfaceColorsWnd), written by tools\\ui\\interface_colors_layout.py: the', 0)
        self.w('     window in the menus\' palette, a picker after Unreal\'s, and the templates the game windows\' colours are', 0)
        self.w('     previewed on (their styles and colours are ui_theme.xml\'s; <item section> is the item\'s own icon,', 0)
        self.w('     plate= the look of its cells, count= the number on it). -->', 0)
        self.w('<w>', 0)
        self.w('<backdrop %s shape="sys_shade"/>' % self.rect(0, 0, W, 768))
        fx = self.frame_x
        self.w('<frame %s shape="sys_panel"/>' % self.rect(fx, 40, W - 2 * fx, 688))
        self.w('<title %s><text font="graffiti22" align="l" vert_align="c" color="$sys_text">st_da_colors_title</text></title>'
               % self.rect(fx + 24, 52, 600, 28))
        self.w('<subtitle %s><text font="letterica16" align="l" vert_align="c" color="$sys_muted">st_da_colors_subtitle</text></subtitle>'
               % self.rect(fx + 24, 80, W - 2 * fx - 48, 20))
        px, py, pw, ph = self.preview
        self.w('<template_caption %s><text font="letterica16" align="l" vert_align="c" color="$sys_muted">st_da_colors_template</text></template_caption>'
               % self.rect(self.picker_x, 640, 120, 22))
        self.w('<template %s list_length="6"><list_font font="letterica16" color="$sys_text"/></template>'
               % self.rect(self.picker_x + 124, 640, self.picker_w - 124, 22))
        self.w('<preview %s shape="sys_inset"/>' % self.rect(px - 6, py - 6, pw + 12, ph + 12))

        x, P = self.picker_x, self.picker_w
        half = (P - 8) / 2
        for i, (node, caption) in enumerate([('target_main', 'st_da_colors_main'), ('target_accent', 'st_da_colors_accent')]):
            bx = x + i * (half + 8)
            self.w('<%s %s shape="sys_button"><text font="letterica18" align="c" vert_align="c">%s</text></%s>'
                   % (node, self.rect(bx, 120, half, 30), caption, node))
            self.w('<%s_swatch %s/>' % (node, self.rect(bx + 8, 128, 14, 14)))

        S = 180.0 if self.kx < 1 else 150.0
        top = 162.0
        self.w('<picker %s>' % self.rect(x, top, P, 464))
        self.w('<wheel %s/>' % self.rect(0, 0, S, S), 2)
        self.w('<saturation %s/>' % self.rect(S + 12, 0, 16, S), 2)
        self.w('<value %s/>' % self.rect(S + 36, 0, 16, S), 2)
        sx = S + 66
        sw = P - sx
        self.w('<old_caption %s><text font="letterica16" align="l" vert_align="c" color="$sys_muted">st_da_colors_old</text></old_caption>'
               % self.rect(sx, 0, sw, 16), 2)
        self.w('<old %s/>' % self.rect(sx, 18, sw, 36), 2)
        self.w('<new_caption %s><text font="letterica16" align="l" vert_align="c" color="$sys_muted">st_da_colors_new</text></new_caption>'
               % self.rect(sx, 62, sw, 16), 2)
        self.w('<new %s/>' % self.rect(sx, 80, sw, 36), 2)
        ry = S + 18
        for i, label in enumerate(['R', 'G', 'B', 'H', 'S', 'V']):
            y = ry + i * 28
            self.w('<row_%s>' % label.lower(), 2)
            self.w('<caption %s><text font="letterica16" align="l" vert_align="c" color="$sys_text">st_da_colors_%s</text></caption>'
                   % (self.rect(0, y, 22, 22), label.lower()), 3)
            self.w('<track %s/>' % self.rect(28, y + 6, P - 28 - 96, 10), 3)
            self.w('<field %s max_symb_count="10"><shape style="sys_field"/><text x="4" font="letterica16" align="l" vert_align="c" color="$sys_text"/></field>'
                   % self.rect(P - 88, y, 88, 22), 3)
            self.w('</row_%s>' % label.lower(), 2)
        hy = ry + 6 * 28 + 8
        for i, (node, caption) in enumerate([('hex_linear', 'st_da_colors_hex_linear'), ('hex_srgb', 'st_da_colors_hex_srgb')]):
            y = hy + i * 28
            self.w('<%s>' % node, 2)
            self.w('<caption %s><text font="letterica16" align="l" vert_align="c" color="$sys_text">%s</text></caption>'
                   % (self.rect(0, y, 100, 22), caption), 3)
            self.w('<field %s max_symb_count="10"><shape style="sys_field"/><text x="4" font="letterica16" align="l" vert_align="c" color="$sys_text"/></field>'
                   % self.rect(104, y, P - 104, 22), 3)
            self.w('</%s>' % node, 2)
        self.w('</picker>')

        by = 672.0
        bw = (P - 16) / 3
        for i, (node, style, caption) in enumerate([('btn_reset', 'sys_button', 'st_da_colors_reset'),
                                                   ('btn_cancel', 'sys_button', 'st_da_colors_cancel'),
                                                   ('btn_ok', 'sys_button_primary', 'st_da_colors_ok')]):
            self.w('<%s %s shape="%s"><text font="letterica18" align="c" vert_align="c">%s</text></%s>'
                   % (node, self.rect(x + i * (bw + 8), by, bw, 32), style, caption, node))

        self.w('<templates %s>' % self.rect(px, py, pw, ph))
        for tid, caption, fn in [('inventory', 'st_da_colors_tpl_inventory', self.tpl_inventory),
                                 ('trade', 'st_da_colors_tpl_trade', self.tpl_trade),
                                 ('dialog', 'st_da_colors_tpl_dialog', self.tpl_dialog)]:
            self.w('<template id="%s" caption="%s" %s>' % (tid, caption, self.rect(0, 0, pw, ph)), 2)
            fn(pw, ph)
            self.w('</template>', 2)
        self.w('</templates>')
        self.w('</w>', 0)
        return '\n'.join(self.out) + '\n'


def main():
    for wide, name in ((False, 'ui_interface_colors.xml'), (True, 'ui_interface_colors_16.xml')):
        with open(os.path.join(ROOT, name), 'w', encoding='utf-8', newline='\n') as f:
            f.write(Layout(wide).build())
        print('wrote', name)


if __name__ == '__main__':
    main()
