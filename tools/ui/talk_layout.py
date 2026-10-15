# Writes configs\ui\talk.xml and talk_character.xml (4:3) and their _16 twins (16:9) of the compatibility overlay:
# the dialog window (CUITalkDialogWnd) in the look of the inventory, the trade and the carcass - one panel, the
# partner's card on top as the trade shows it, the conversation and the player's phrases in insets, the trade
# button under them. Lengths are screen units, as in actor_menu_layout.py: 768 to the screen's height, x squeezed
# by 0.75 into a 16:9 file.
#
# The panel is laid out in the middle of the screen; dar_dialog_position moves the whole window to the left or
# the right edge at show time (CUITalkDialogWnd::Show), so a mod's own talk.xml moves the same way.
#
#   python tools\ui\talk_layout.py
import os

ROOT = os.path.join(os.path.dirname(__file__), '..', '..', 'packaging', 'dead-air-x64', 'compatibility',
                    'gamedata', 'configs', 'ui')


def num(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


class Layout:
    def __init__(self, wide):
        self.kx = 0.75 if wide else 1.0
        self.screen_w = 1024.0 / self.kx
        self.W = 620.0 if wide else 560.0
        self.H = 748.0
        self.x0 = (self.screen_w - self.W) / 2
        self.y0 = (768.0 - self.H) / 2
        self.pad = 12.0
        self.gap = 8.0
        # the partner's card: the trade's (actor_menu_layout.py card_texts), its portrait at the right
        self.card_h = 100.0
        self.portrait = (114.0, 76.0) if wide else (96.0, 64.0)
        self.button_h = 32.0
        self.questions_h = 176.0
        self.inner = self.W - 2 * self.pad
        # the conversation takes what the rest leaves
        self.answers_h = self.H - 2 * self.pad - self.card_h - self.questions_h - self.button_h - 3 * self.gap - 2.0
        self.list_pad = 8.0
        self.out = []

    def rect(self, x, y, w, h):
        return 'x="%s" y="%s" width="%s" height="%s"' % (num(x * self.kx), num(y), num(w * self.kx), num(h))

    def w(self, line=''):
        self.out.append(line)

    def talk(self):
        L = self.x0 + self.pad
        card_y = self.y0 + self.pad
        answers_y = card_y + self.card_h + self.gap
        questions_y = answers_y + self.answers_h + self.gap
        button_y = questions_y + self.questions_h + self.gap + 2.0
        list_w = self.inner - 2 * self.list_pad
        w = self.w
        w('<?xml version="1.0" encoding="utf-8"?>')
        w('<!-- The dialog window (CUITalkDialogWnd). Written by tools\\ui\\talk_layout.py for 4:3 and 16:9 in the look of')
        w('     the inventory and the trade: every surface and button is drawn by the xms_ui_shape shader (ui_theme.xml).')
        w('     The window is laid out in the middle; the option "Dialog window" (dar_dialog_position) moves all of it')
        w('     to the left or the right edge of the screen. -->')
        w('<w>')
        w('    <main x="0" y="0" width="1024" height="768">')
        w('        <auto_static %s shape="shadow"/>' % self.rect(self.x0 - 6, self.y0 - 4, self.W + 12, self.H + 12))
        w('        <auto_static %s shape="panel"/>' % self.rect(self.x0, self.y0, self.W, self.H))
        w('        <auto_static %s shape="card"/>' % self.rect(L, card_y, self.inner, self.card_h))
        # the portrait's frame, a hair outside the picture
        w('        <auto_static %s shape="portrait_frame"/>' % self.rect(
            L + self.inner - self.portrait[0] - 11, card_y + 11, self.portrait[0] + 2, self.portrait[1] + 2))
        w('    </main>')
        w()
        # the partner: CUICharacterInfo of talk_character.xml over the card
        w('    <left_character_icon %s/>' % self.rect(L, card_y, self.inner, self.card_h))
        w()
        w('    <frame_top %s shape="inset"/>' % self.rect(L, answers_y, self.inner, self.answers_h))
        w('    <frame_bottom %s shape="inset"/>' % self.rect(L, questions_y, self.inner, self.questions_h))
        w('    <!-- relative to their frames -->')
        w('    <answers_list %s always_show_scroll="0" scroll_profile="thin" vert_interval="2"/>' % self.rect(
            self.list_pad, self.list_pad, list_w, self.answers_h - 2 * self.list_pad))
        w('    <questions_list %s always_show_scroll="0" scroll_profile="thin" vert_interval="2"/>' % self.rect(
            self.list_pad, self.list_pad, list_w, self.questions_h - 2 * self.list_pad))
        w()
        bw = 240.0
        w('    <button %s>' % self.rect(self.x0 + (self.W - bw) / 2, button_y, bw, self.button_h))
        w('        <shape style="button_primary"/>')
        w('        <text font="graffiti22" align="c" vert_align="c">ui_st_trade</text>')
        w('    </button>')
        w()
        item_w = list_w - 10
        text_x = 22.0
        w('    <question_item %s min_height="22">' % self.rect(0, 0, item_w, 22))
        w('        <num_text %s>' % self.rect(2, 2, 18, 20))
        w('            <text font="letterica18" color="$text_muted"/>')
        w('        </num_text>')
        w('        <content_text %s>' % self.rect(text_x, 2, item_w - text_x - 4, 20))
        w('            <text font="letterica18" color="$text" align="l" complex_mode="1"/>')
        w('            <text_color>')
        w('                <e color="$text"/>')
        w('                <t color="$accent"/>')
        w('                <h color="$accent"/>')
        w('                <d color="$text_muted"/>')
        w('            </text_color>')
        w('        </content_text>')
        w('    </question_item>')
        w()
        for node, name_color, text_color in (('other_answer_item', '$accent', '$text'),
                                             ('actor_answer_item', '$text_dim', '$text_dim')):
            w('    <%s %s min_height="14" bottom_footer="8">' % (node, self.rect(0, 0, item_w, 0)))
            w('        <name_caption %s>' % self.rect(0, 2, item_w - 8, 18))
            w('            <text font="graffiti22" color="%s" complex_mode="0"/>' % name_color)
            w('        </name_caption>')
            w('        <content_text %s complex_mode="1">' % self.rect(text_x, 24, item_w - text_x - 8, 10))
            w('            <text font="letterica18" color="%s"/>' % text_color)
            w('        </content_text>')
            w('    </%s>' % node)
            w()
        w('    <iconed_answer_item %s min_height="32" bottom_footer="6">' % self.rect(0, 0, item_w, 0))
        w('        <name_caption x="0" y="0" width="0" height="0"/>')
        w('        <content_text %s complex_mode="1">' % self.rect(text_x + 46, 0, item_w - text_x - 54, 30))
        w('            <text font="letterica18" color="$text"/>')
        w('        </content_text>')
        w('        <msg_icon %s stretch="1"/>' % self.rect(text_x, 2, 41, 29))
        w('    </iconed_answer_item>')
        w()
        w('    <simple_answer_item %s min_height="14" bottom_footer="6">' % self.rect(0, 0, item_w, 0))
        w('        <name_caption x="0" y="0" width="0" height="0"/>')
        w('        <content_text %s complex_mode="1">' % self.rect(text_x, 0, item_w - text_x - 8, 30))
        w('            <text font="letterica18" color="$text"/>')
        w('        </content_text>')
        w('        <msg_icon x="0" y="0" width="0" height="0"/>')
        w('    </simple_answer_item>')
        w()
        w('    <iconed_trade_info %s min_height="22" bottom_footer="6">' % self.rect(0, 0, item_w, 0))
        w('        <name_caption x="0" y="0" width="0" height="0"/>')
        w('        <content_text %s complex_mode="1">' % self.rect(text_x + 36, 2, item_w - text_x - 44, 20))
        w('            <text font="letterica18" color="$text_dim"/>')
        w('        </content_text>')
        w('        <msg_icon %s stretch="1"/>' % self.rect(text_x, 0, 28, 20))
        w('    </iconed_trade_info>')
        w('</w>')
        return '\n'.join(self.out) + '\n'

    def character(self):
        """CUICharacterInfo of the card above: the trade card's rows, relative to the card."""
        self.out = []
        w = self.w
        pw, ph = self.portrait
        text_w = self.inner - pw - 30
        w('<?xml version="1.0" encoding="utf-8"?>')
        w('<!-- The partner\'s card of the dialog window (talk.xml left_character_icon): the trade\'s card. Written by')
        w('     tools\\ui\\talk_layout.py; the lengths are relative to the card. -->')
        w('<w>')
        w('    <name_static %s>' % self.rect(10, 6, text_w, 22))
        w('        <text align="l" vert_align="c" font="letterica18" color="$text"/>')
        w('    </name_static>')
        for i, (name, caption) in enumerate((('community', 'ui_st_community'), ('rank', 'ui_st_rank'),
                                             ('reputation', 'ui_st_reputation'), ('relation', 'ui_st_relation'))):
            y = 30 + 14 * i
            w('    <%s_caption %s>' % (name, self.rect(10, y, text_w, 14)))
            w('        <text align="l" font="letterica16" color="$text_dim">%s</text>' % caption)
            w('    </%s_caption>' % name)
            w('    <%s_static %s>' % (name, self.rect(10, y, text_w, 14)))
            w('        <text align="r" font="letterica16" color="$text"/>')
            w('    </%s_static>' % name)
        w('    <icon %s stretch="1">' % self.rect(self.inner - pw - 10, 12, pw, ph))
        w('        <deadbody r="255" g="150" b="140" a="200"/>')
        w('    </icon>')
        w('</w>')
        return '\n'.join(self.out) + '\n'


def main():
    for wide, suffix in ((False, ''), (True, '_16')):
        L = Layout(wide)
        for name, text in (('talk%s.xml' % suffix, L.talk()), ('talk_character%s.xml' % suffix, L.character())):
            with open(os.path.join(ROOT, name), 'w', encoding='utf-8', newline='\n') as f:
                f.write(text)
            print('wrote', name)


if __name__ == '__main__':
    main()
