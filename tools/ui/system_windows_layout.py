# Writes the layouts of the game's own windows over the menus - the update (ui_update.xml), the major
# release (ui_update_major.xml), the bug report (ui_bug_report.xml) and the content repair
# (ui_content.xml) - each for 4:3 and as its _16 twin, from one geometry. They wear the palette of
# the Mods window (the sys_* looks and colours of ui_theme.xml), not the in-game one. Lengths are
# screen units, as in actor_menu_layout.py: 768 to the screen's height, x squeezed by 0.75 into a
# 16:9 file.
#
#   python tools\ui\system_windows_layout.py
#
# The node names are the windows' own (UIUpdateWnd.cpp, UIMajorUpdateWnd.cpp, UIBugReportWnd.cpp,
# UIContentWnd.cpp); auto_static nodes under main are decorations a mod may add to or take away.
import os

ROOT = os.path.join(os.path.dirname(__file__), '..', '..', 'packaging', 'dead-air-x64', 'compatibility',
                    'gamedata', 'configs', 'ui')


def num(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


class Dialog:
    def __init__(self, wide, w, h, comment):
        self.kx = 0.75 if wide else 1.0
        self.screen_w = 1024.0 / self.kx
        self.w_, self.h_ = w, h
        self.x0 = (self.screen_w - w) / 2
        self.y0 = (768.0 - h) / 2
        self.out = ['<?xml version="1.0" encoding="utf-8"?>', '<!-- %s Written by tools\\ui\\system_windows_layout.py' % comment,
                    '     for 4:3 and 16:9 in the palette of the Mods window (sys_* of ui_theme.xml). -->', '<w>',
                    '    <main x="0" y="0" width="1024" height="768">']
        # the menu under it fades back, the window stands on a soft shadow
        self.auto(-self.x0, -self.y0, self.screen_w, 768, 'sys_shade')
        self.auto(-8, -6, w + 16, h + 16, 'sys_shadow')
        self.auto(0, 0, w, h, 'sys_panel')
        # the window's own background node stays empty: the panel is a decoration, so rules lie over it
        self.line('<background %s/>' % self.rect(0, 0, w, h))

    # x and width relative to the window, in screen units; squeezed on the way out
    def rect(self, x, y, w, h):
        return 'x="%s" y="%s" width="%s" height="%s"' % (num((self.x0 + x) * self.kx), num(self.y0 + y), num(w * self.kx), num(h))

    def line(self, text):
        self.out.append('        ' + text)

    def auto(self, x, y, w, h, shape):
        self.line('<auto_static %s shape="%s"/>' % (self.rect(x, y, w, h), shape))

    def text(self, node, x, y, w, h, font='letterica16', align='l', valign='c', color='$sys_text', caption='', complex_mode=False):
        cm = ' complex_mode="1"' if complex_mode else ''
        self.line('<%s %s>' % (node, self.rect(x, y, w, h)))
        self.line('    <text font="%s" align="%s" vert_align="%s" color="%s"%s>%s</text>' % (font, align, valign, color, cm, caption))
        self.line('</%s>' % node)

    def caption(self, text):
        w = self.w_
        self.text('caption', 24, 14, w - 48, 34, font='graffiti22', caption=text)
        self.auto(24, 54, w - 48, 1, 'sys_rule')

    def button(self, node, x, y, w, h, caption, style):
        self.line('<%s %s>' % (node, self.rect(x, y, w, h)))
        self.line('    <shape style="%s"/>' % style)
        self.line('    <text font="letterica16" align="c" vert_align="c">%s</text>' % caption)
        self.line('</%s>' % node)

    def buttons(self, primary, primary_caption, secondary, secondary_caption, primary_w=230, secondary_w=150):
        w, h = self.w_, self.h_
        self.auto(24, h - 70, w - 48, 1, 'sys_rule')
        self.button(secondary, w - 24 - secondary_w, h - 54, secondary_w, 32, secondary_caption, 'sys_button')
        self.button(primary, w - 24 - secondary_w - 12 - primary_w, h - 54, primary_w, 32, primary_caption, 'sys_button_primary')

    def check(self, node, x, y, side):
        self.line('<%s %s>' % (node, self.rect(x, y, side, side)))
        self.line('    <shape style="sys_check"/>')
        self.line('    <mark shape="sys_check_mark"/>')
        self.line('</%s>' % node)

    def edit(self, node, x, y, w, h, max_symbols, multiline=False):
        ml = ' multiline="1"' if multiline else ''
        self.line('<%s %s max_symb_count="%d"%s>' % (node, self.rect(x, y, w, h), max_symbols, ml))
        self.line('    <shape style="sys_field"/>')
        if multiline:
            self.line('    <text x="%s" y="6" width="%s" height="%s" font="letterica16" align="l" vert_align="t" complex_mode="1" color="$sys_text"/>'
                      % (num(8 * self.kx), num((w - 16) * self.kx), num(h - 12)))
        else:
            self.line('    <text x="%s" y="0" font="letterica16" align="l" vert_align="c" color="$sys_text"/>' % num(8 * self.kx))
        self.line('</%s>' % node)

    def scroll_list(self, node, text_node, y, h):
        """A list of text on an inset of its own: it hides with the list."""
        w = self.w_
        self.line('<%s %s shape="sys_inset" always_show_scroll="0" scroll_profile="sys" vert_interval="4"'
                  ' right_ident="8" left_ident="10" top_indent="8" bottom_indent="8"/>' % (node, self.rect(24, y, w - 48, h)))
        self.line('<%s x="0" y="0" width="%s" height="16">' % (text_node, num((w - 48 - 26) * self.kx)))
        self.line('    <text font="letterica16" align="l" vert_align="t" complex_mode="1" color="$sys_muted"/>')
        self.line('</%s>' % text_node)

    def progress(self, y):
        w = self.w_
        self.line('<progress %s horz="1" min="0" max="100" pos="0">' % self.rect(60, y, w - 120, 4))
        self.line('    <progress shape="sys_bar_fill"/>')
        self.line('    <background shape="sys_bar_track"/>')
        self.line('</progress>')

    def finish(self):
        self.out += ['    </main>', '</w>']
        return '\n'.join(self.out) + '\n'


def update(wide):
    d = Dialog(wide, 560, 480, 'The update of the game (UIUpdateWnd).')
    w = d.w_
    d.caption('st_update_available_caption')
    d.text('message', 24, 62, w - 48, 46, complex_mode=True)
    d.text('size', 24, 110, w - 48, 22, color='$sys_muted')
    d.text('theme', 24, 134, w - 48, 26, font='graffiti19', color='$sys_accent', complex_mode=True)
    d.scroll_list('changes', 'changes_text', 166, 230)
    # the download in place of the changes
    d.text('progress_text', 24, 222, w - 48, 24, align='c', color='$sys_muted')
    d.progress(256)
    d.text('error', 24, 272, w - 48, 70, align='c', color='$sys_bad', complex_mode=True)
    d.buttons('action', 'st_update_download', 'cancel', 'ui_mm_cancel')
    return d.finish()


def update_major(wide):
    d = Dialog(wide, 560, 500, 'The release that needs a new installer (UIMajorUpdateWnd).')
    w = d.w_
    d.caption('st_update_major_caption')
    d.text('message', 24, 62, w - 48, 44, complex_mode=True)
    d.text('folder_hint', 24, 108, w - 48, 88, valign='t', color='$sys_muted', complex_mode=True, caption='st_update_major_folder')
    d.text('theme', 24, 198, w - 48, 26, font='graffiti19', color='$sys_accent', complex_mode=True)
    d.scroll_list('changes', 'changes_text', 228, 150)
    d.check('disable_checks', 24, 392, 18)
    d.text('disable_checks_label', 52, 389, w - 76, 24, color='$sys_muted', caption='st_update_major_disable')
    d.buttons('action', 'st_update_major_open', 'cancel', 'st_update_major_close')
    return d.finish()


def bug_report(wide):
    d = Dialog(wide, 470, 480, 'The bug and crash report (UIBugReportWnd).')
    w = d.w_
    d.caption('st_bug_report_caption')
    d.text('title_label', 24, 66, w - 148, 20, color='$sys_muted', caption='st_bug_report_title')
    d.text('title_counter', w - 124, 66, 100, 20, align='r', color='$sys_dim', caption='0/200')
    d.edit('title', 24, 88, w - 48, 30, 200)
    d.text('description_label', 24, 130, w - 148, 20, color='$sys_muted', caption='st_bug_report_description')
    d.text('description_counter', w - 124, 130, 100, 20, align='r', color='$sys_dim', caption='0/10000')
    d.edit('description', 24, 152, w - 48, 160, 10000, multiline=True)
    d.check('attach_dump', 24, 327, 18)
    d.text('attach_label', 52, 324, w - 76, 24, color='$sys_muted', caption='st_bug_report_attach_dump')
    d.text('server_status_label', 24, 356, 120, 22, color='$sys_muted', caption='st_bug_report_server_status')
    d.text('server_status', 144, 356, w - 168, 22, caption='st_bug_report_server_checking')
    d.text('status', 24, 380, w - 48, 22, align='c')
    d.buttons('submit', 'st_bug_report_submit', 'cancel', 'ui_mm_cancel', primary_w=200, secondary_w=130)
    return d.finish()


def content(wide):
    d = Dialog(wide, 560, 470, 'The repair of the mod\'s content (UIContentWnd).')
    w = d.w_
    d.caption('st_content_caption')
    d.text('message', 24, 62, w - 48, 92, valign='t', complex_mode=True)
    d.scroll_list('problems', 'problems_text', 160, 150)
    d.text('progress_text', 24, 318, w - 48, 30, align='c', color='$sys_muted', complex_mode=True)
    d.progress(352)
    d.buttons('action', 'st_content_repair', 'exit', 'st_content_exit', primary_w=220, secondary_w=180)
    return d.finish()


def message_box(wide, kind):
    """message_box_sys_yes_no / _ok: the game's questions over these windows (the crash report's),
    each in a file of its own name - the engine looks there for a template message_box.xml lacks."""
    kx = 0.75 if wide else 1.0
    w, h = 440.0, 200.0
    x0, y0 = (1024.0 / kx - w) / 2, (768.0 - h) / 2

    def r(x, y, ww, hh):
        return 'x="%s" y="%s" width="%s" height="%s"' % (num(x * kx), num(y), num(ww * kx), num(hh))

    node = 'message_box_sys_' + kind
    out = ['<?xml version="1.0" encoding="utf-8"?>',
           '<!-- A question of the game\'s own windows over the menus, in the palette of the Mods window. Written by',
           '     tools\\ui\\system_windows_layout.py; CUIMessageBox reads a template message_box.xml lacks from the',
           '     file of its name. -->', '<w>',
           '    <%s type="%s" %s shape="sys_panel">' % (node, 'yes_no' if kind == 'yes_no' else 'ok', r(x0, y0, w, h)),
           '        <message_text %s complex_mode="1">' % r(20, 18, w - 40, 112),
           '            <text align="c" vert_align="c" font="letterica18" color="$sys_text">a</text>',
           '        </message_text>']
    if kind == 'yes_no':
        buttons = (('button_yes', 'Btn_Yes', w - 20 - 130 - 12 - 130, 130, 'sys_button_primary'),
                   ('button_no', 'Btn_No', w - 20 - 130, 130, 'sys_button'))
    else:
        buttons = (('button_ok', 'Btn_OK', (w - 140) / 2, 140, 'sys_button_primary'),)
    for name, caption, x, bw, style in buttons:
        out += ['        <%s %s check_mode="0">' % (name, r(x, h - 52, bw, 32)),
                '            <window_name>%s</window_name>' % name,
                '            <shape style="%s"/>' % style,
                '            <text font="letterica16" align="c" vert_align="c">%s</text>' % caption,
                '        </%s>' % name]
    out += ['    </%s>' % node, '</w>']
    return '\n'.join(out) + '\n'


def main():
    for build, base in ((update, 'ui_update'), (update_major, 'ui_update_major'), (bug_report, 'ui_bug_report'),
                        (content, 'ui_content')):
        for wide, suffix in ((False, ''), (True, '_16')):
            name = base + suffix + '.xml'
            with open(os.path.join(ROOT, name), 'w', encoding='utf-8', newline='\n') as f:
                f.write(build(wide))
            print('wrote', name)
    for kind in ('yes_no', 'ok'):
        for wide, suffix in ((False, ''), (True, '_16')):
            name = 'message_box_sys_' + kind + suffix + '.xml'
            with open(os.path.join(ROOT, name), 'w', encoding='utf-8', newline='\n') as f:
                f.write(message_box(wide, kind))
            print('wrote', name)


if __name__ == '__main__':
    main()
