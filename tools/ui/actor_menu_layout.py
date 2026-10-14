# Writes configs\ui\actor_menu.xml (4:3) and actor_menu_16.xml (16:9) of the compatibility overlay from
# one geometry. Lengths here are screen units: 768 of them to the screen's height, as many across as
# the aspect gives (1024 for 4:3, 1365.33 for 16:9). A 16:9 layout file squeezes its x lengths into
# 1024 (x * 0.75), which this script does on the way out.
#
#   python tools\ui\actor_menu_layout.py
#
# The XML it writes is the file a mod edits: everything in it is a plain node of the actor menu.
import os

ROOT = os.path.join(os.path.dirname(__file__), '..', '..', 'packaging', 'dead-air-x64', 'compatibility',
                    'gamedata', 'configs', 'ui')


def num(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


class Layout:
    def __init__(self, wide):
        self.wide = wide
        self.kx = 0.75 if wide else 1.0
        self.W = 1024.0 / self.kx
        self.out = []
        self.margin = 10.0 if wide else 4.0
        self.gap = 8.0 if wide else 4.0
        self.center_w = 346.0 if wide else 340.0
        self.side_w = (self.W - 2 * self.margin - 2 * self.gap - self.center_w) / 2
        self.pad = 12.0 if wide else 8.0
        self.top = self.margin
        self.height = 768.0 - 2 * self.margin
        # cells: square on screen, whole x units
        self.cell_x = 31 if wide else 41
        self.cell_y = 41
        self.cell_w = self.cell_x / self.kx
        self.left = self.margin
        self.center = self.margin + self.side_w + self.gap
        self.right = self.center + self.center_w + self.gap
        self.portrait = (114.0, 76.0) if wide else (96.0, 64.0)

    # a rect: x and width in screen units, squeezed on the way out
    def rect(self, x, y, w, h):
        return 'x="%s" y="%s" width="%s" height="%s"' % (num(x * self.kx), num(y), num(w * self.kx), num(h))

    def w(self, line=''):
        self.out.append(line)

    # ---------------------------------------------------------------------------------------------

    def card_texts(self, node, x, partner):
        """CUICharacterInfo: name, the rows, the portrait. Children are relative to the node."""
        pw, ph = self.portrait
        inner = self.side_w - 2 * self.pad
        text_w = inner - pw - 30
        self.w('    <%s %s>' % (node, self.rect(x + self.pad, self.top + 8, inner, 100)))
        self.w('        <name_static %s>' % self.rect(10, 6, text_w, 22))
        self.w('            <text align="l" vert_align="c" font="letterica18" color="$text"/>')
        self.w('        </name_static>')
        rows = [('community', 'ui_st_community'), ('rank', 'ui_st_rank'), ('reputation', 'ui_st_reputation')]
        if partner:
            rows.append(('relation', 'ui_st_relation'))
        for i, (name, caption) in enumerate(rows):
            y = 30 + 14 * i
            self.w('        <%s_caption %s>' % (name, self.rect(10, y, text_w, 14)))
            self.w('            <text align="l" font="letterica16" color="$text_dim">%s</text>' % caption)
            self.w('        </%s_caption>' % name)
            self.w('        <%s_static %s>' % (name, self.rect(10, y, text_w, 14)))
            self.w('            <text align="r" font="letterica16" color="$text"/>')
            self.w('        </%s_static>' % name)
        self.w('        <icon %s stretch="1">' % self.rect(inner - pw - 10, 12, pw, ph))
        self.w('            <deadbody r="255" g="150" b="140" a="200"/>')
        self.w('        </icon>')
        self.w('    </%s>' % node)

    def card_background(self, x, partner):
        pw, ph = self.portrait
        inner = self.side_w - 2 * self.pad
        self.w('        <auto_static %s shape="card"/>' % self.rect(x + self.pad, self.top + 8, inner, 100))
        # the portrait's frame, a hair outside the picture
        self.w('        <auto_static %s shape="portrait_frame"/>' % self.rect(x + self.pad + inner - pw - 11, self.top + 19, pw + 2, ph + 2))

    def money(self, node, x):
        pw, _ = self.portrait
        inner = self.side_w - 2 * self.pad
        text_w = inner - pw - 30
        self.w('    <%s %s>' % (node, self.rect(x + self.pad + 10, self.top + 8 + 82, text_w, 18)))
        self.w('        <text align="r" vert_align="c" font="graffiti22" color="$text"/>')
        self.w('    </%s>' % node)

    def grid(self, node, x, y, rows, extra=''):
        """A bag grid centred in a side panel, its thin scroll bar at its right."""
        inner = self.side_w - 2 * self.pad
        scroll = 5.0
        cols = int((inner - 4 - scroll) // self.cell_w)
        gw = cols * self.cell_w + scroll
        gx = x + (self.side_w - gw) / 2
        self.w('    <%s %s cell_width="%d" cell_height="%d" rows_num="%d" cols_num="%d" a="255" unlimited="1"'
               ' group_similar="1" always_show_scroll="0" condition_progress_bar="1" scroll_profile="thin"%s>'
               % (node, self.rect(gx, self.top + y, gw, rows * self.cell_y), self.cell_x, self.cell_y, rows, cols, extra))
        self.cells()
        self.w('    </%s>' % node)
        return gx, gw

    def cells(self):
        self.w('        <cells style="cell">')
        for look, style in (('item', 'cell_item'), ('selected', 'cell_selected'), ('marked', 'cell_marked'),
                            ('related', 'cell_related'), ('blocked', 'cell_blocked'), ('drop', 'cell_drop'),
                            ('picked', 'cell_picked'), ('dim', 'cell_dim')):
            self.w('            <%s style="%s"/>' % (look, style))
        self.w('        </cells>')

    def state_cells(self, node):
        """A list over plates of its own (a slot, the belt, the quick slots): no cells drawn, only the item's
        state - under the cursor, related to the one under it, marked, blocked."""
        self.w('        <cells>')
        for look, style in (('selected', 'cell_selected'), ('marked', 'cell_marked'), ('related', 'cell_related'),
                            ('blocked', 'cell_blocked')):
            self.w('            <%s style="%s"/>' % (look, style))
        self.w('        </cells>')
        self.w('    </%s>' % node)

    def button(self, node, x, y, w, h, caption, hint, style='button', window_name=None):
        self.w('    <%s %s hint="%s">' % (node, self.rect(x, y, w, h), hint))
        self.w('        <window_name>%s</window_name>' % (window_name or node))
        self.w('        <shape style="%s"/>' % style)
        self.w('        <text font="graffiti22" align="c" vert_align="c">%s</text>' % caption)
        self.w('    </%s>' % node)

    def trade_bar(self, node, x, y, align_left):
        """CUITradeBar: the sum on the table. The price and the caption lay out leftwards from the weight."""
        inner = self.side_w - 2 * self.pad
        self.w('    <%s %s shape="inset">' % (node, self.rect(x + self.pad, self.top + y, inner, 34)))
        self.w('        <trade_caption %s>' % self.rect(inner - 150, 9, 60, 16))
        self.w('            <text align="l" font="letterica16" color="$text_dim">ui_total_price</text>')
        self.w('        </trade_caption>')
        self.w('        <trade_price %s>' % self.rect(inner - 120, 9, 60, 16))
        self.w('            <text align="l" font="letterica18" color="$text"/>')
        self.w('        </trade_price>')
        self.w('        <trade_weight_max %s>' % self.rect(inner - 70, 9, 64, 16))
        self.w('            <text align="l" font="letterica16" color="$text_dim"/>')
        self.w('        </trade_weight_max>')
        self.w('    </%s>' % node)

    def bar(self, node, x, y, w, h, thin=False, extra_children=''):
        track = 'bar_thin_track' if thin else 'bar_track'
        fill = 'bar_thin_fill' if thin else 'bar_fill'
        self.w('    <%s %s horz="1" min="0" max="1" pos="0">' % (node, self.rect(x, y, w, h)))
        self.w('        <progress shape="%s"/>' % fill)
        self.w('        <background shape="%s"/>' % track)
        self.w('        <min_color color="$bad"/>')
        self.w('        <middle_color color="$warn"/>')
        self.w('        <max_color color="$good"/>')
        self.w('    </%s>' % node)

    # ---------------------------------------------------------------------------------------------

    def build(self):
        L, C, R, top = self.left, self.center, self.right, self.top
        inner = self.side_w - 2 * self.pad
        self.w('<?xml version="1.0" encoding="utf-8"?>')
        self.w('<!-- The actor menu (CUIActorMenu): the inventory, the trade, the loot of a body or a box. Written by')
        self.w('     tools\\ui\\actor_menu_layout.py from one geometry for actor_menu.xml (4:3) and actor_menu_16.xml (16:9);')
        self.w('     the files are plain layouts, a mod edits them as it would any other. Every surface, button, bar')
        self.w('     and cell is drawn by the xms_ui_shape shader: shape="name" takes a look of ui_theme.xml, colours')
        self.w('     written "$name" come from it too. Item icons and portraits are the only textures. -->')
        self.w('<w>')

        # the context menu over an item
        self.w('    <properties_box shape="menu">')
        self.w('        <list x="0" y="0" width="10" height="10" item_height="20" always_show_scroll="0" can_select="1"'
               ' bottom_indent="4" right_ident="4" left_ident="12">')
        self.w('            <font font="letterica16" color="$text"/>')
        self.w('            <item shape="menu_row"/>')
        self.w('        </list>')
        self.w('    </properties_box>')
        self.w()

        # Always shown: the player's panel and the equipment column.
        self.w('    <main x="0" y="0" width="1024" height="768">')
        self.w('        <auto_static %s shape="panel"/>' % self.rect(L, top, self.side_w, self.height))
        self.w('        <auto_static %s shape="panel"/>' % self.rect(C, top, self.center_w, self.height))
        self.card_background(L, False)
        self.equipment_plates()
        self.w('    </main>')
        self.w()

        # Shown for a trade and a search: the partner's panel.
        self.w('    <left_background %s shape="panel">' % self.rect(R, top, self.side_w, self.height))
        pw, ph = self.portrait
        self.w('        <auto_static %s shape="card"/>' % self.rect(self.pad, 8, inner, 100))
        self.w('        <auto_static %s shape="portrait_frame"/>' % self.rect(self.pad + inner - pw - 11, 19, pw + 2, ph + 2))
        self.w('    </left_background>')
        self.w()

        self.card_texts('actor_ch_info', L, False)
        self.card_texts('partner_ch_info', R, True)
        self.money('actor_money_static', L)
        self.money('partner_money_static', R)
        self.w()

        # the bag of the inventory window and of the search: the toolbar takes its first row
        self.grid('dragdrop_bag', L, 120, 14)
        self.grid('dragdrop_deadbody_bag', R, 120, 13)
        # the trade: the offer on the table, the sum, the rest of the bag
        self.grid('dragdrop_actor_trade', L, 120, 4)
        self.grid('dragdrop_partner_trade', R, 120, 4)
        self.trade_bar('right_delimiter', L, 290, True)
        self.trade_bar('left_delimiter', R, 290, False)
        self.grid('dragdrop_actor_trade_bag', L, 330, 9)
        self.grid('dragdrop_partner_bag', R, 330, 9)
        # in the inventory window, an item dropped over the world goes to the ground
        self.w('    <dragdrop_trash %s cell_width="%d" cell_height="768" rows_num="1" cols_num="1" a="0"/>'
               % (self.rect(R, 0, self.W - R, 768), int(round((self.W - R) * self.kx))))
        self.w()

        bw = min(170.0, inner * 0.42)
        self.button('trade_barter_button', L + self.pad + 3, top + 293, bw, 28, 'ui_st_barter', 'ui_st_barter_hint', 'button_primary')
        self.button('trade_sell_button', L + self.pad + 3, top + 293, bw, 28, 'ui_st_sell', 'ui_st_sell_hint', 'button_primary')
        self.button('trade_buy_button', R + self.pad + 3, top + 293, bw, 28, 'ui_st_buy', 'ui_st_buy_hint', 'button_primary')
        self.button('takeall_button', R + self.pad, top + self.height - 44, inner, 34, 'ui_st_take_all', 'ui_st_take_all_hint', 'button_primary')
        self.button('exit_button', C + 10, top + self.height - 44, self.center_w - 20, 34, 'ui_inv_exit', 'ui_st_exit_hint')
        self.w()

        # the weight: the player's with its load, the partner's alone
        foot = top + self.height - 44
        self.weight('actor', L, foot, True)
        self.weight('partner', R, top + self.height - 66, False)
        self.w()

        self.equipment_lists()
        self.actor_state()
        self.hint()
        self.hotkeys_panel()
        self.compare_window()

        self.w('    <action_sounds>')
        for tag, snd in (('snd_open', 'inv_open'), ('snd_close', 'inv_close'), ('snd_item_to_slot', 'inv_slot'),
                         ('snd_item_to_belt', 'inv_belt'), ('snd_item_to_ruck', 'inv_ruck'), ('snd_properties', 'inv_properties'),
                         ('snd_drop_item', 'inv_drop'), ('snd_attach_addon', 'inv_attach_addon'),
                         ('snd_detach_addon', 'inv_detach_addon'), ('snd_item_use', 'inv_slot')):
            self.w('        <%s>interface\\%s</%s>' % (tag, snd, tag))
        self.w('    </action_sounds>')
        self.w('</w>')
        return '\n'.join(self.out) + '\n'

    def weight(self, who, x, y, load):
        inner = self.side_w - 2 * self.pad
        x0 = x + self.pad
        # CUIWeightBar lays the caption and the weight out leftwards from the max
        self.w('    <%s_weight_caption %s>' % (who, self.rect(x0 + 10, y + 4, 60, 16)))
        self.w('        <text align="l" font="letterica16" color="$text_dim">ui_total_weight</text>')
        self.w('    </%s_weight_caption>' % who)
        self.w('    <%s_weight %s>' % (who, self.rect(x0 + 70, y + 4, 40, 16)))
        self.w('        <text align="l" font="letterica18" color="$text"/>')
        self.w('    </%s_weight>' % who)
        if load:
            self.w('    <%s_weight_max %s>' % (who, self.rect(x0 + inner - 120, y + 4, 110, 16)))
            self.w('        <text align="l" font="letterica16" color="$text_dim"/>')
            self.w('    </%s_weight_max>' % who)
            # the load against the limits of movement: the overload's start marked, a full bar the full overload
            self.w('    <%s_weight_bar %s hint_delay="500" zone_tint="0.22">' % (who, self.rect(x0 + 10, y + 26, inner - 20, 5)))
            self.w('        <track shape="load_track"/>')
            self.w('        <fill shape="load_fill"/>')
            self.w('        <mark shape="load_mark"/>')
            self.w('        <normal color="$good"/>')
            self.w('        <overload color="$warn"/>')
            self.w('        <slow color="$bad"/>')
            self.w('    </%s_weight_bar>' % who)
        else:
            # without a max the weight ends where the max would start
            self.w('    <%s_weight_max %s>' % (who, self.rect(x0 + inner - 10, y + 4, 2, 16)))
            self.w('        <text align="l" font="letterica16" color="$text_dim"/>')
            self.w('    </%s_weight_max>' % who)

    # ---------------------------------------------------------------------------------------------
    # the equipment column

    def slots(self):
        """(name, x, y, cols, rows, condition bar, vertical) of every slot, x/y relative to the column."""
        cw = self.cell_w
        plate_w = 2 * cw + 16
        lx, cx, rx = 10.0, (self.center_w - plate_w) / 2, self.center_w - 10 - plate_w
        return [
            ('binocular', lx, 8, 2, 1, False, False),
            ('helmet', cx, 8, 2, 2, True, False),
            ('detector', rx, 8, 2, 1, False, False),
            ('pistol', lx, 73, 2, 6, True, True),
            ('outfit', cx, 124, 2, 3, True, False),
            ('automatic', rx, 73, 2, 6, True, True),
            ('backpack', cx, 281, 2, 2, True, False),
            ('sidearm', lx, 353, 2, 1, True, False),
            ('knife', rx, 353, 2, 1, True, False),
        ]

    def plate_size(self, cols, rows, bar):
        return cols * self.cell_w + 16, rows * self.cell_y + 16 + (10 if bar else 0)

    def equipment_plates(self):
        C, top = self.center, self.top
        for name, x, y, cols, rows, bar, vertical in self.slots():
            w, h = self.plate_size(cols, rows, bar)
            self.w('        <auto_static %s shape="slot"/>' % self.rect(C + x, top + y, w, h))
            if name in ('helmet', 'backpack'):
                # Dead Air's sign of an empty helmet or rucksack slot, faint under the item
                s = min(w, h - (10 if bar else 0)) * 0.62
                self.w('        <auto_static %s shape="slot_glyph"/>' % self.rect(C + x + (w - s) / 2, top + y + (h - (10 if bar else 0) - s) / 2, s, s))
        qx, qy = self.quick()
        for i in range(4):
            self.w('        <auto_static %s shape="slot"/>' % self.rect(C + qx[i], top + qy, 74, 62))
        bx, by, bw = self.belt()
        self.w('        <auto_static %s shape="slot"/>' % self.rect(C + bx, top + by, bw, 57))
        self.w('        <auto_static %s shape="card"/>' % self.rect(C + 10, top + 565, self.center_w - 20, 135))

    def quick(self):
        step = (self.center_w - 20 - 74) / 3
        return [10 + step * i for i in range(4)], 430.0

    def belt(self):
        bw = 7 * self.cell_w + 16
        return (self.center_w - bw) / 2, 500.0, bw

    def equipment_lists(self):
        C, top = self.center, self.top
        names = {'binocular': ('dragdrop_binocular', None, 'binocular_slot_highlight', None),
                 'helmet': ('dragdrop_helmet', 'progess_bar_helmet', 'helmet_slot_highlight', 'helmet_over'),
                 'detector': ('dragdrop_detector', None, 'detector_slot_highlight', None),
                 'pistol': ('dragdrop_pistol', 'progess_bar_weapon1', 'inv_slot2_highlight', None),
                 'outfit': ('dragdrop_outfit', 'progess_bar_outfit', 'outfit_slot_highlight', None),
                 'automatic': ('dragdrop_automatic', 'progess_bar_weapon2', 'inv_slot3_highlight', None),
                 'backpack': ('dragdrop_backpack', 'progess_bar_backpack', 'backpack_slot_highlight', 'backpack_over'),
                 'sidearm': ('dragdrop_sidearm', 'progess_bar_sidearm', 'sidearm_highlight', None),
                 'knife': ('dragdrop_knife', 'progess_bar_knife', 'knife_highlight', None)}
        for name, x, y, cols, rows, bar, vertical in self.slots():
            node, progress, highlight, blocker = names[name]
            w, h = self.plate_size(cols, rows, bar)
            lx, ly = C + x + 8, top + y + 8
            self.w('    <%s %s cell_width="%d" cell_height="%d" rows_num="%d" cols_num="%d" custom_placement="0"%s'
                   ' a="0" virtual_cells="1" vc_vert_align="c" vc_horiz_align="c">'
                   % (node, self.rect(lx, ly, cols * self.cell_w, rows * self.cell_y), self.cell_x, self.cell_y, rows, cols,
                      ' vertical_placement="1"' if vertical else ''))
            self.state_cells(node)
            if progress:
                self.bar(progress, C + x + 8, top + y + h - 12, w - 16, 4, thin=True)
            self.w('    <%s %s shape="slot_highlight"/>' % (highlight, self.rect(C + x, top + y, w, h)))
            if blocker:
                self.w('    <%s %s shape="blocker"/>' % (blocker, self.rect(C + x, top + y, w, h)))
        self.w()

        # the quick slots: one cell over each plate, the key under it
        qx, qy = self.quick()
        spacing = (qx[1] - qx[0]) - self.cell_w
        self.w('    <dragdrop_quick_slots %s cell_width="%d" cell_height="%d" a="0" rows_num="1" cols_num="4"'
               ' cell_sp_x="%d" cell_sp_y="0" custom_placement="1">'
               % (self.rect(C + qx[0] + (74 - self.cell_w) / 2, top + qy + 6, 3 * (qx[1] - qx[0]) + self.cell_w, self.cell_y),
                  self.cell_x, self.cell_y, int(round(spacing * self.kx))))
        self.state_cells('dragdrop_quick_slots')
        self.w('    <quick_slot_highlight %s dx="%s" shape="slot_highlight"/>'
               % (self.rect(C + qx[0], top + qy, 74, 62), num((qx[1] - qx[0]) * self.kx)))
        for i in range(4):
            self.w('    <quick_slot%d_text %s>' % (i + 1, self.rect(C + qx[i] + 4, top + qy + 46, 30, 14)))
            self.w('        <text align="l" font="letterica16" color="$text_muted">quick_use_str_%d</text>' % (i + 1))
            self.w('    </quick_slot%d_text>' % (i + 1))
        self.w()

        # the belt of artefacts; a slot the outfit does not give is covered
        bx, by, bw = self.belt()
        self.w('    <dragdrop_belt %s cell_width="%d" cell_height="%d" rows_num="1" cols_num="7" a="0">'
               % (self.rect(C + bx + 8, top + by + 8, 7 * self.cell_w, self.cell_y), self.cell_x, self.cell_y))
        self.state_cells('dragdrop_belt')
        self.w('    <artefact_slot_highlight %s dx="%s" shape="slot_highlight"/>'
               % (self.rect(C + bx + 8, top + by + 8, self.cell_w, self.cell_y), num(self.cell_x)))
        self.w('    <belt_list_over %s dx="%s" shape="blocker"/>'
               % (self.rect(C + bx + 8, top + by + 8, self.cell_w, self.cell_y), num(self.cell_x)))
        self.w()

    def actor_state(self):
        C, top = self.center, self.top
        w = self.center_w - 20
        self.w('    <actor_state_info %s>' % self.rect(C + 10, top + 565, w, 135))
        self.hint(indent='        ')
        # health: the heart, the bar, bleeding and radiation lighting up at its end
        self.w('        <auto_static %s><shape kind="heart" fill="$health"/></auto_static>' % self.rect(10, 9, 14, 14))
        self.w('        <health_state %s>' % self.rect(30, 10, w - 80, 12))
        self.w('            <hint_text delay="800">st_da_state_health</hint_text>')
        self.w('            <state_progress %s horz="1" min="0" max="1" pos="0">' % self.rect(0, 0, w - 80, 12))
        self.w('                <progress shape="bar_fill"><shape fill="$health"/></progress>')
        self.w('                <background shape="bar_track"/>')
        self.w('            </state_progress>')
        self.w('        </health_state>')
        # Bleeding blinks while it lasts; the radiation sign is the dose a dosimeter measured last,
        # its hint written by the engine (st_da_measured_dose_hint). Both stay on the panel muted
        # while there is nothing to show, the lit sign drawn over the muted one.
        for node, kind, hint, x, blink in (('bleeding_state', 'drop', 'st_da_state_bleeding', w - 44, True),
                                           ('radiation_state', 'trefoil', 'st_da_measured_dose_hint', w - 24, False)):
            self.w('        <auto_static %s><shape kind="%s" fill="$text_muted"/></auto_static>'
                   % (self.rect(x, 8, 16, 16), kind))
            self.w('        <%s %s>' % (node, self.rect(x, 8, 16, 16)))
            self.w('            <hint_text delay="800">%s</hint_text>' % hint)
            anim = ' light_anim="ui_slow_blinking" la_cyclic="1" la_texture="1" la_text="0" la_alpha="1"' if blink else ''
            for icon, color in (('icon', '$good'), ('icon2', '$warn'), ('icon3', '$bad')):
                self.w('            <%s x="0" y="0" width="%s" height="16"%s><shape kind="%s" fill="%s"/></%s>'
                       % (icon, num(16 * self.kx), anim, kind, color, icon))
            self.w('        </%s>' % node)
        # what the second bar shows is satiety
        self.w('        <auto_static %s><shape kind="cutlery" fill="$satiety"/></auto_static>' % self.rect(10, 29, 14, 14))
        self.w('        <stamina_state %s>' % self.rect(30, 33, w - 80, 6))
        self.w('            <hint_text delay="800">st_da_state_satiety</hint_text>')
        self.w('            <state_progress %s horz="1" min="0" max="1" pos="0">' % self.rect(0, 0, w - 80, 6))
        self.w('                <progress shape="bar_thin_fill"><shape fill="$satiety"/></progress>')
        self.w('                <background shape="bar_thin_track"/>')
        self.w('            </state_progress>')
        self.w('        </stamina_state>')
        self.w('        <auto_static %s shape="rule"/>' % self.rect(10, 48, w - 20, 1))
        # the protection of what is worn: four rows of two
        # short hints of what each protection does in the game (st_da_state_*)
        sensors = [('radia_sensor', 'trefoil', 'st_da_state_radiation'),
                   ('psi_sensor', 'psi', 'st_da_state_psi'),
                   ('acid_sensor', 'biohazard', 'st_da_state_chemical'),
                   ('wound_sensor', 'claw', 'st_da_state_wound'),
                   ('shock_sensor', 'bolt', 'st_da_state_shock'),
                   ('fire_wound_sensor', 'bullet', 'st_da_state_fire_wound'),
                   ('fire_sensor', 'flame', 'st_da_state_burn'),
                   ('power_sensor', 'runner', 'st_da_state_power')]
        col_w = (w - 30) / 2
        for i, (node, kind, hint) in enumerate(sensors):
            x = 10 + (i % 2) * (col_w + 10)
            y = 58 + (i // 2) * 19
            self.w('        <%s %s>' % (node, self.rect(x, y, col_w, 14)))
            self.w('            <hint_text delay="800">%s</hint_text>' % hint)
            self.w('            <icon x="0" y="0" width="%s" height="14"><shape kind="%s" fill="$text_dim"/></icon>' % (num(14 * self.kx), kind))
            self.w('            <state_progress %s horz="1" min="0" max="1" pos="0">' % self.rect(20, 3, col_w - 20, 8))
            self.w('                <progress shape="bar_fill"><shape fill="$protection"/></progress>')
            self.w('                <background shape="bar_track"/>')
            self.w('            </state_progress>')
            self.w('        </%s>' % node)
        self.w('    </actor_state_info>')
        self.w()

    def hotkeys_panel(self):
        """CUIInventoryHotkeysPanel: over the lower half of the equipment column, where the quick slots,
        the belt and the state are; the grids the hotkeys work on stay in sight. keys_width, chip_gap
        and chip_padding are 4:3 lengths the engine squeezes itself."""
        C, top = self.center, self.top
        # under the pistol's and the knife's plates, above the exit button
        y = top + 426
        w, h = self.center_w - 20, (top + self.height - 50) - y
        self.w('    <!-- the panel of hotkeys (the keyboard button of the toolbar, or H); its rows are inventory_hotkeys.xml -->')
        self.w('    <hotkeys_panel %s shape="panel" row_height="22" keys_width="128" chip_gap="3" chip_padding="5">'
               % self.rect(C + 10, y, w, h))
        self.w('        <title %s>' % self.rect(12, 6, w - 54, 24))
        self.w('            <text font="letterica18" align="l" vert_align="c" color="$accent">st_da_hk_title</text>')
        self.w('        </title>')
        self.w('        <close %s shape="tool_button" cross="$text_dim"/>' % self.rect(w - 34, 7, 22, 22))
        self.w('        <auto_static %s shape="rule"/>' % self.rect(10, 34, w - 20, 1))
        self.w('        <list %s always_show_scroll="0" scroll_profile="thin" right_ident="4" left_ident="0"'
               ' top_indent="0" bottom_indent="2" vert_interval="0"/>' % self.rect(10, 38, w - 16, h - 46))
        self.w('        <chip shape="key_chip"><text font="letterica16" color="$text"/></chip>')
        self.w('        <plus><text font="letterica16" color="$text_muted"/></plus>')
        self.w('        <caption><text font="letterica16" color="$text_dim"/></caption>')
        self.w('        <rule shape="rule"/>')
        self.w('    </hotkeys_panel>')
        self.w()

    def compare_window(self):
        """CUIItemCompareWnd: centred over the menu, the menu shaded under it. label_width and
        column_width are 4:3 lengths the engine squeezes itself."""
        w = 980.0 if not self.wide else 1000.0
        h = 640.0
        x = (self.W - w) / 2
        y = (768.0 - h) / 2
        self.w('    <!-- the comparison of picked items (the scales button of the toolbar, or V); the weights are')
        self.w('         configs\\dead_air_x64_item_compare.ltx -->')
        self.w('    <compare_window %s shape="panel" row_height="20" label_width="210" column_width="360"'
               ' icon_height="56" padding="6">' % self.rect(x, y, w, h))
        self.w('        <shade shape="compare_shade"/>')
        self.w('        <title %s>' % self.rect(16, 10, w - 300, 28))
        self.w('            <text font="graffiti22" align="l" vert_align="c" color="$text"/>')
        self.w('        </title>')
        for i, (node, caption) in enumerate((('tab_use', 'st_cmp_profile_use'), ('tab_sale', 'st_cmp_profile_sale'))):
            self.w('        <%s %s>' % (node, self.rect(w - 278 + i * 120, 12, 114, 26)))
            self.w('            <shape style="tab"/>')
            self.w('            <text font="letterica16" align="c" vert_align="c">%s</text>' % caption)
            self.w('        </%s>' % node)
        self.w('        <close %s shape="tool_button" cross="$text_dim"/>' % self.rect(w - 40, 13, 24, 24))
        self.w('        <auto_static %s shape="rule"/>' % self.rect(12, 46, w - 24, 1))
        self.w('        <columns %s/>' % self.rect(12, 52, w - 30, 156))
        self.w('        <list %s always_show_scroll="0" scroll_profile="thin" right_ident="0" left_ident="0"'
               ' top_indent="2" bottom_indent="2" vert_interval="0"/>' % self.rect(12, 212, w - 24, h - 212 - 46))
        self.w('        <auto_static %s shape="rule"/>' % self.rect(12, h - 42, w - 24, 1))
        self.w('        <footer %s>' % self.rect(16, h - 38, w - 32, 30))
        self.w('            <text font="letterica16" align="l" vert_align="c" color="$text_dim"/>')
        self.w('        </footer>')
        self.w('        <name><text font="letterica16" color="$text"/></name>')
        self.w('        <score><text font="graffiti22" color="$text"/></score>')
        self.w('        <label><text font="letterica16" color="$text_dim"/></label>')
        self.w('        <weight><text font="letterica16" color="$text_muted"/></weight>')
        self.w('        <value><text font="letterica16" color="$text"/></value>')
        self.w('        <section><text font="letterica18" color="$accent"/></section>')
        self.w('        <best color="$good"/>')
        self.w('        <worst color="$bad"/>')
        self.w('        <column shape="compare_column"/>')
        self.w('        <column_winner shape="compare_column_winner"/>')
        self.w('        <row_alt shape="compare_row"/>')
        self.w('        <score_bar shape="bar_thin_fill"><shape fill="$accent"/></score_bar>')
        self.w('        <score_track shape="bar_thin_track"/>')
        self.w('        <badge shape="compare_badge"/>')
        self.w('        <rule shape="rule"/>')
        self.w('    </compare_window>')
        self.w()

    def hint(self, indent='    '):
        self.w('%s<hint_wnd x="0" y="0" width="%s" height="120">' % (indent, num(320 * self.kx)))
        self.w('%s    <background x="0" y="0" width="%s" height="120" border="10" shape="tooltip"/>' % (indent, num(320 * self.kx)))
        self.w('%s    <text x="%s" y="10" width="%s" height="100" always_show_scroll="0" fit_to_height="1" complex_mode="1">'
               % (indent, num(12 * self.kx), num(296 * self.kx)))
        self.w('%s        <font font="letterica16" color="$text_dim" align="l" vert_align="t"/>' % indent)
        self.w('%s    </text>' % indent)
        self.w('%s</hint_wnd>' % indent)


def standalone(wide, part, comment):
    """A file of one node: what a menu layout without that node (an older one, a UI mod's) gets."""
    layout = Layout(wide)
    layout.w('<?xml version="1.0" encoding="utf-8"?>')
    layout.w('<!-- %s Written by tools\\ui\\actor_menu_layout.py with actor_menu.xml; the' % comment)
    layout.w('     engine reads this file only when the actor_menu layout has no node of this name. -->')
    layout.w('<w>')
    getattr(layout, part)()
    layout.w('</w>')
    return '\n'.join(layout.out) + '\n'


def main():
    for wide, name in ((False, 'actor_menu.xml'), (True, 'actor_menu_16.xml')):
        text = Layout(wide).build()
        with open(os.path.join(ROOT, name), 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
        print('wrote', name)
    for part, base, comment in (('hotkeys_panel', 'actor_menu_hotkeys', 'The panel of inventory hotkeys.'),
                                ('compare_window', 'actor_menu_compare', 'The comparison of items.')):
        for wide, suffix in ((False, ''), (True, '_16')):
            name = base + suffix + '.xml'
            with open(os.path.join(ROOT, name), 'w', encoding='utf-8', newline='\n') as f:
                f.write(standalone(wide, part, comment))
            print('wrote', name)


if __name__ == '__main__':
    main()
