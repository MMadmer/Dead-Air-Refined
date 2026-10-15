"""Check that every settings screen has a hover hint on every setting, and its texts, in both languages.

The options dialog (ui_mm_opt_main.script, the same dialog from the main menu and from the pause
menu) and the weather dialog of the main menu (atmosfear_options.script) show a hint when the cursor
rests on a row. A hint is a string id, translated when it is shown: an id in neither string table
shows nothing, one only in Russian shows the raw id to an English player. This walks every place the
two dialogs take a hint from and asks both tables:

- Video, Sound, Controls (dead_air_x64_option_list.script): every [rows] entry of the page's
  layout, configs\\dead_air_x64_<page>_options.ltx, has the hint st_da_x64_<page>_hint_<row>; the
  video page also has the variants ui_mm_opt_video.script names for greyed-out rows;
- the key list on the Controls page: every action of ui_keybinding.xml, and kb_map, which the
  engine adds when the file lacks it: <caption id>_hint;
- Game: every option the scripts put into gameplay_options (the effective copy of each script, the
  stock layers included) but the [hidden] ones of dead_air_x64_gameplay_options.ltx:
  ui_mm_<option>_hint, or the option's own `hint` field;
- the weather dialog: the rows of its events page (EVENTS of atmosfear_options.script, three
  settings each), the one hint of its per-level rows and the one row of its seasons page;
- every `hint` attribute in ui_mm_opt.xml and ui_mm_opt_16.xml.

What those screens show is checked in both languages as well: subsection headers, row captions (with
the layouts' [captions] and the Game options' own), the key list's groups and actions, the levels and
lists of the weather dialog, the texts the dialogs' nodes carry (tabs, buttons, column captions), the
questions the options dialog asks, and the entries of the video page's lists - an entry is the string
st_opt_<token>, else <token>, as the engine's combo box looks it up (a number shows as it is). The
seasons page is the overlay's alone, so its texts are checked even without the stock layers: the
entries ui_mm_season_<token> of the SEASONS table, which must also be the r__season tokens of
src/xrEngine/da_season.cpp in the same order, and the list must open to all of them in both layouts
(list_length). The
hints of Sound, Controls, the key list and the weather dialog are new: they must come from the
overlay (no later file overrides them) and stay within MAX_NEW characters. No hint may hold an em
dash: the hint popup's font on the 4:3 layout has none and leaves a gap.

    python tools\\qa\\Check-OptionHints.py [--stock <unpacked game archives>]

The string tables are folded like CStringTable::Init: the engine's own strings (res\\gamedata) first,
then the game's files by name, the overlay over the stock layers. The stock layers are the game's
archives unpacked (configs, xtra, dar2 - lowest first; by default ..\\_work\\da_unpack next to the
repository). Without them only the overlay is read: the Game rows the stock scripts add, the levels
of the weather dialog and the texts only the game has go unchecked. Prints every gap and exits 1;
exits 0 when there is none.
"""
import argparse
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

REPO = pathlib.Path(__file__).resolve().parents[2]
OVERLAY = REPO / 'packaging' / 'dead-air-x64' / 'compatibility' / 'gamedata'
ENGINE_GAMEDATA = REPO / 'res' / 'gamedata'
LANGUAGES = ('rus', 'eng')
ENGINE_TEXTS = ('grizzly.xml', 'xfined-ray.xml', 'openxray.xml')
MAX_NEW = 110
# The groups of checks that are hints, not captions.
HINT_GROUPS = ('video', 'sound', 'controls', 'keys', 'game', 'weather', 'xml')
UI_FILES = ('ui_mm_opt.xml', 'ui_mm_opt_16.xml')
WEATHER_UI_FILES = ('ui_mm_atmosfear_options.xml', 'ui_mm_atmosfear_options_16.xml')

# The video page's lists and the engine's token arrays behind them: (source, array) or tokens.
VIDEO_TOKENS = {
    'renderer': ('src/xrEngine/Engine.cpp', None),
    'vid_window_mode': ('src/xrEngine/xr_ioc_cmd.cpp', 'vid_window_mode_token'),
    'vid_fps_limit': ('src/xrEngine/xr_ioc_cmd.cpp', 'fpsLimitTokens'),
    '_preset': ('src/Layers/xrRender/xrRender_console.cpp', 'qpreset_token'),
    'r__aa': ('src/Layers/xrRender/xrRender_console.cpp', 'qaa_token'),
    'r__fsr': ('src/Layers/xrRender/xrRender_console.cpp', 'qfsr_token'),
    'r__dlss': ('src/Layers/xrRender/xrRender_console.cpp', 'qdlss_token'),
    'r__framegen': ('src/Layers/xrRender/xrRender_console.cpp', 'qframegen_token'),
    'r__async_compute': ('src/Layers/xrRender/xrRender_console.cpp', 'qasync_compute_token'),
    'r__rt_quality': ('src/Layers/xrRender/xrRender_console.cpp', 'qrt_quality_token'),
}


def layers(stock, kind):
    """Folders of one kind ('configs' or 'scripts'), lowest priority first."""
    found = []
    if stock:
        found += [stock / 'configs' / 'configs' if kind == 'configs' else stock / 'configs' / kind,
                  stock / 'xtra' / kind, stock / 'dar2' / kind]
    found.append(OVERLAY / kind)
    return [f for f in found if f.is_dir()]


def effective_files(folders, pattern):
    """name -> path of the highest layer that has it, as the game's file system resolves it."""
    files = {}
    for folder in folders:
        for path in folder.glob(pattern):
            files[path.name.lower()] = path
    return files


def read_strings(path, strict):
    data = path.read_bytes()
    try:
        root = ET.fromstring(data)
    except ET.ParseError as error:
        if strict:
            raise SystemExit(f'{path}: {error}')
        # Stock files are not always well-formed; the engine's parser forgives what this does.
        text = data.decode('cp1251', 'replace')
        return {m.group(1): m.group(2) for m in
                re.finditer(r'<string\s+id="([^"]+)"\s*>\s*<text>(.*?)</text>', text, re.S)}
    return {s.get('id'): (s.findtext('text') or '') for s in root.iter('string')}


def string_table(stock, language):
    """id -> (text, file), folded like CStringTable::Init: the engine's own file first, then by name."""
    folders = [ENGINE_GAMEDATA / 'configs'] + layers(stock, 'configs')
    files = effective_files([f / 'text' / language for f in folders], '*.xml')
    names = sorted(files, key=lambda n: (n not in ENGINE_TEXTS, n))
    table = {}
    for name in names:
        path = files[name]
        for key, text in read_strings(path, OVERLAY in path.parents).items():
            table[key] = (text, path)
    return table


def ini_section(path, section):
    """Lines of an ltx section as (key, value), comments dropped."""
    lines, inside = [], False
    for raw in path.read_text('cp1251', errors='replace').splitlines():
        line = raw.split(';', 1)[0].strip()
        if not line:
            continue
        if line.startswith('['):
            inside = line[1:line.index(']')] == section
            continue
        if inside:
            key, _, value = line.partition('=')
            lines.append((key.strip(), value.strip()))
    return lines


def ini_list(path, section, key):
    return [item for k, value in ini_section(path, section) if k == key for item in re.findall(r'[^,\s]+', value)]


def lua_text(path):
    # Comments out, strings kept: a commented-out option is not a row.
    text = path.read_text('cp1251', errors='replace')
    text = re.sub(r'--\[(=*)\[.*?\]\1\]', '', text, flags=re.S)
    return re.sub(r'--[^\n]*', '', text)


def table_body(text, start):
    """The text of the Lua table constructor opening at text[start] == '{'."""
    depth, i, quote = 0, start, None
    while i < len(text):
        c = text[i]
        if quote:
            if c == '\\':
                i += 1
            elif c == quote:
                quote = None
        elif c in '"\'':
            quote = c
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1
    return text[start:]


class Page:
    """A page that is one list: its layout ltx, rows, captions, groups."""

    def __init__(self, name):
        self.name = name
        self.ltx = OVERLAY / 'configs' / f'dead_air_x64_{name}_options.ltx'
        self.rows = [key for key, _ in ini_section(self.ltx, 'rows')]
        self.captions = dict(ini_section(self.ltx, 'captions'))
        self.groups = ini_list(self.ltx, 'groups', 'order')

    def hints(self):
        return [f'st_da_x64_{self.name}_hint_{row}' for row in self.rows]

    def texts(self):
        return ([self.captions.get(row, f'ui_mm_{row}') for row in self.rows]
                + [f'ui_mm_grp_{group}' for group in self.groups])


def keybinding(stock):
    """(groups, [(caption id, action)]) of the effective ui_keybinding.xml, kb_map included."""
    path = effective_files(layers(stock, 'configs'), 'ui/ui_keybinding.xml').get('ui_keybinding.xml')
    if not path:
        raise SystemExit('no ui_keybinding.xml in any layer - is --stock right?')
    root = ET.fromstring(path.read_bytes())
    groups = [group.get('name') for group in root.iter('group')]
    commands = [(c.get('id'), c.get('exe')) for c in root.iter('command')]
    # FillUpList and key_binding_actions() add the PDA map row when the file has none.
    if 'map' not in {exe for _, exe in commands}:
        commands.append(('kb_map', 'map'))
    return groups, commands


def game_rows(stock):
    """[(hint id, caption id)] of the Game tab's rows."""
    ltx = OVERLAY / 'configs' / 'dead_air_x64_gameplay_options.ltx'
    hidden = {key for key, _ in ini_section(ltx, 'hidden')}
    captions = dict(ini_section(ltx, 'captions'))
    rows = {}
    for path in effective_files(layers(stock, 'scripts'), '*.script').values():
        text = lua_text(path)
        for m in re.finditer(r'gameplay_options\s*\[\s*"([^"]+)"\s*\]\s*=\s*\{', text):
            key = m.group(1)
            if key in hidden:
                continue
            body = table_body(text, m.end() - 1)
            hint = re.search(r'\bhint\s*=\s*"([^"]+)"', body)
            caption = re.search(r'\bcaption\s*=\s*"([^"]+)"', body)
            rows[key] = (hint.group(1) if hint else f'ui_mm_{key}_hint',
                         caption.group(1) if caption else captions.get(key, f'ui_mm_{key}'))
    return [rows[key] for key in sorted(rows)]


def xml_roots(names):
    return [ET.fromstring((OVERLAY / 'configs' / 'ui' / name).read_bytes()) for name in names]


def xml_hints():
    return sorted({node.get('hint') for root in xml_roots(UI_FILES) for node in root.iter() if node.get('hint')})


def node_texts(names, *paths):
    """Text ids of the nodes under these paths in the given xml files."""
    ids = set()
    for root in xml_roots(names):
        for path in paths:
            for node in root.findall(path):
                ids |= {t.text.strip() for t in node.iter('text') if t.text and t.text.strip()}
    return sorted(ids)


def weather_dialog(stock):
    """(hints, texts) of the weather dialog: its rows, headers, lists, levels and nodes."""
    script = lua_text(OVERLAY / 'scripts' / 'atmosfear_options.script')
    prefix = re.search(r'local\s+HINTS\s*=\s*"([^"]+)"', script).group(1)
    events = re.findall(r'\{\s*id\s*=\s*"(\w+)"\s*,\s*title\s*=\s*"(\w+)"\s*\}', script)
    if not events:
        raise SystemExit('atmosfear_options.script: no EVENTS table - has the page changed?')
    rows = [row for event, _ in events for row in (f'enable_{event}', f'{event}_freq', f'{event}_task')]
    hints = [prefix + row for row in rows] + [prefix + 'period']
    texts = [title for _, title in events] + re.findall(r'"(ui_mm_(?:grp_)?af_\w+)"', script)

    # The entries of its lists: st_<key> of the list_<row> sections, and of list_periods.
    settings = effective_files(layers(stock, 'configs'), 'atmosfear_default_settings.ltx').get('atmosfear_default_settings.ltx')
    if settings:
        for section in [f'list_{event}_{s}' for event, _ in events for s in ('freq', 'task')] + ['list_periods']:
            texts += [f'st_{key}' for key, _ in ini_section(settings, section)]

    # The levels it lists: game_maps_single.ltx's single-player levels whose weather is AtmosFear.
    maps = effective_files(layers(stock, 'configs'), 'game_maps_single.ltx').get('game_maps_single.ltx')
    if maps:
        for level, _ in ini_section(maps, 'level_maps_single'):
            if dict(ini_section(maps, level)).get('weathers') == 'atmosfear':
                texts.append(level)

    texts += node_texts(WEATHER_UI_FILES, 'main_dialog/btn_accept', 'main_dialog/btn_cancel',
                        'main_dialog/btn_weather_options', 'main_dialog/btn_event_options',
                        'event_page', 'weather_page')
    return hints, texts


def season_page():
    """(hint, texts, gaps) of the weather dialog's seasons page: one row, the time of year (r__season)."""
    script = lua_text(OVERLAY / 'scripts' / 'atmosfear_options.script')
    prefix = re.search(r'local\s+HINTS\s*=\s*"([^"]+)"', script).group(1)
    seasons = re.search(r'local\s+SEASONS\s*=\s*\{([^}]*)\}', script)
    if not seasons:
        raise SystemExit('atmosfear_options.script: no SEASONS table - has the page changed?')
    tokens = re.findall(r'"(\w+)"', seasons.group(1))
    texts = ([f'ui_mm_season_{token}' for token in tokens] + ['st_cap_af_season_options']
             + node_texts(WEATHER_UI_FILES, 'main_dialog/btn_season_options', 'season_page'))
    gaps = []
    # The menu sends its tokens to r__season: the engine must know each, in the list's order.
    engine = (REPO / 'src' / 'xrEngine' / 'da_season.cpp').read_text('utf-8', errors='replace')
    array = re.search(r'season_token\[\]\s*=\s*\{(.*?)\};', engine, re.S)
    engine_tokens = re.findall(r'\{\s*"(\w+)"', array.group(1)) if array else []
    if engine_tokens != tokens:
        gaps.append(f'seasons: SEASONS {tokens} differs from the r__season tokens {engine_tokens} of da_season.cpp')
    # The open list must show every entry: the scroll view around the page clips a longer one.
    for root, name in zip(xml_roots(WEATHER_UI_FILES), WEATHER_UI_FILES):
        node = root.find('season_page/list_season')
        length = int(node.get('list_length', '4')) if node is not None else 0
        if length != len(tokens):
            gaps.append(f'seasons: {name} season_page/list_season has list_length {length}, the list has {len(tokens)}')
    return prefix + 'season', texts, gaps


def video_list_entries():
    """(label ids, tokens) of the video page's lists: st_opt_<token> first, then the token itself."""
    entries = []
    for command, (source, array) in VIDEO_TOKENS.items():
        text = (REPO / source).read_text('utf-8', errors='replace')
        if array is None:
            tokens = sorted(set(re.findall(r'"(renderer_r\d)"', text)))
        else:
            m = re.search(re.escape(array) + r'\s*\[\s*\]\s*=\s*\{(.*?)\n\s*\};', text, re.S)
            if not m:
                raise SystemExit(f'{source}: no token array {array}')
            tokens = re.findall(r'\{\s*"([^"]+)"', m.group(1))
        entries += [(command, token) for token in tokens if not token.isdigit()]
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--stock', type=pathlib.Path, default=REPO.parent / '_work' / 'da_unpack')
    args = parser.parse_args()
    stock = args.stock if args.stock.is_dir() else None
    if not stock:
        print(f'note: no stock layers at {args.stock}; the stock Game rows, the levels and the stock texts are skipped')

    tables = {language: string_table(stock, language) for language in LANGUAGES}
    video, sound, controls = Page('video'), Page('sound'), Page('controls')
    key_groups, key_actions = keybinding(stock)
    video_script = lua_text(OVERLAY / 'scripts' / 'ui_mm_opt_video.script')
    main_script = lua_text(OVERLAY / 'scripts' / 'ui_mm_opt_main.script')
    game = game_rows(stock)
    weather_hints, weather_texts = weather_dialog(stock)
    season_hint, season_texts, season_gaps = season_page()

    # (group, ids, new): a new hint must come from the overlay and stay short. Every hint shows in the
    # popup's font, which on the 4:3 layout draws no em dash.
    checks = [
        ('video', video.hints() + sorted(set(re.findall(r'"(st_da_x64_video_hint_\w+)"', video_script))), False),
        ('sound', sound.hints(), True),
        ('controls', controls.hints(), True),
        ('keys', [f'{caption}_hint' for caption, _ in key_actions], True),
        ('game', [hint for hint, _ in game], False),
        ('weather', weather_hints + [season_hint], True),
        ('season-texts', sorted(set(season_texts)), True),
        ('xml', xml_hints(), False),
    ]
    if stock:
        frame = (node_texts(UI_FILES, 'main_dialog/tab', 'main_dialog/btn_accept', 'main_dialog/btn_cancel',
                            'sound_page', 'controls_page', 'tab_sound/tab_captioning')
                 + re.findall(r'"(ui_mm_confirm_changes|mp_si_sec)"', main_script))
        # The two questions the dialog asks, in the message box the game draws them in.
        # The stock files are not all well-formed: read the two nodes as text.
        boxes = effective_files(layers(stock, 'configs'), 'ui/message_box*.xml')
        for path in boxes.values():
            text = path.read_bytes().decode('cp1251', 'replace')
            for name in ('message_box_yes_no', 'message_box_restart_game'):
                for node in re.findall(rf'<{name}\b.*?</{name}>', text, re.S):
                    frame += [t.strip() for t in re.findall(r'<text[^>]*>([^<]*)</text>', node) if len(t.strip()) > 1]
        checks += [
            ('texts', sorted(set(
                video.texts() + sound.texts() + controls.texts() + key_groups
                + [controls.captions.get(caption, caption) for caption, _ in key_actions])), False),
            ('game-captions', sorted({caption for _, caption in game}), False),
            ('weather-texts', sorted(set(weather_texts)), False),
            ('frame', sorted(set(frame)), False),
        ]

    gaps = list(season_gaps)
    for group, ids, new in checks:
        for hint in ids:
            for language in LANGUAGES:
                text, path = tables[language].get(hint, ('', None))
                text = ' '.join(text.split())
                if not text:
                    gaps.append(f'{group}: {hint} has no {language} text')
                elif new and OVERLAY not in path.parents:
                    gaps.append(f'{group}: {hint} ({language}) is overridden by {path}')
                elif new and len(text) > MAX_NEW:
                    gaps.append(f'{group}: {hint} ({language}) is {len(text)} characters, over {MAX_NEW}')
                elif group in HINT_GROUPS and '—' in text:
                    gaps.append(f'{group}: {hint} ({language}) has an em dash, which the 4:3 hint font does not draw')

    entries = video_list_entries()
    for command, token in entries:
        for language in LANGUAGES:
            if not any(tables[language].get(key, ('', None))[0].strip() for key in (f'st_opt_{token}', token)):
                gaps.append(f'video-lists: {command} entry {token} has no {language} text')

    for gap in gaps:
        print(gap)
    print('ids checked: ' + ', '.join(f'{group} {len(ids)}' for group, ids, _ in checks)
          + f', video-lists {len(entries)}; gaps: {len(gaps)}')
    return 1 if gaps else 0


if __name__ == '__main__':
    sys.exit(main())
