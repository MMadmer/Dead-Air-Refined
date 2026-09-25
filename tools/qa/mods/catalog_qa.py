"""The mod catalog's in-game QA (docs/dead-air/MOD_CATALOG.md 15): fixture, mock and checks.

    python catalog_qa.py fixture --qa-root <root> --assets <dir> --port <port>
    python catalog_qa.py serve --assets <dir> --port <port> [--log <file>]
    python catalog_qa.py inbox --assets <dir>          verifies the reviews the game posted

The fixture writes installed modules into <root>\\modules and everything the network serves into
<assets>: GitHub releases (signed descriptors with a [card], file indexes, packages), the catalog
signed with a fresh QA maintainer key (index, cards, thumbnails, VirusTotal results, ratings,
reviews), VirusTotal API answers, and variants of the index for the rollback and tamper phases.
It writes env.json with what the engine needs to trust it (DAR_QA_CATALOG_KEY and friends).

The mock serves /catalog/..., /<owner>/<repo>/releases/latest/download/<asset> with Range,
/vt/api/v3/files/<sha256> (the x-apikey of the fixture only) and a POST /inbox that keeps every
review the game sends.
"""
import argparse
import base64
import hashlib
import http.server
import json
import os
import shutil
import socketserver
import struct
import sys
import threading
import urllib.parse
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "catalog"))
import xms_catalog as xc  # noqa: E402

VT_KEY = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
TRUSTED = ("Microsoft, Kaspersky, ESET-NOD32, BitDefender, Avast, AVG, Sophos, Symantec, Malwarebytes, TrendMicro, "
           "McAfee, F-Secure, DrWeb, Emsisoft, GData, Panda, Avira, Fortinet, CrowdStrike, SentinelOne, Paloalto")

PROBE = r'''-- The protected mode probe (MOD_CATALOG 9): every escape the sandbox has to stop. run() goes
-- twice: in an ordinary game every "yes" shows the capability is real, under protection every one
-- must be "no" (write_appdata_text stays "yes": that is what a script may do). console() goes under
-- protection only: each way a script could pass a command off as the player's. PROTECTQA lines are
-- read by Run-ModsQa.ps1 -Scenario Catalog.
local function say(name, ok, detail)
	printf("PROTECTQA: %s %s %s", name, ok and "yes" or "no", tostring(detail or ""))
end

local function root(alias)
	return getFS():update_path(alias, "")
end

local function exists(path)
	local f = io.open(path, "rb")
	if f then f:close() return true end
	return false
end

local function slurp(path)
	local f = io.open(path, "rb")
	if not f then return nil end
	local data = f:read("*a")
	f:close()
	return data
end

function run()
	local fs_root = root("$fs_root$")
	local appdata = root("$app_data_root$")

	say("ffi_available", pcall(require, "ffi"))
	local shell = os.execute and os.execute()
	say("os_execute_available", shell ~= nil and shell ~= false and shell ~= 0)
	local pipe = io.popen and io.popen("cd")
	say("io_popen_available", pipe ~= nil)
	if pipe then pipe:close() end
	local lib = package.loadlib and package.loadlib("kernel32.dll", "*")
	say("loadlib_available", lib ~= nil and lib ~= false)

	-- bytecode: made where it can be made, loaded where it must not be
	local dumped = string.dump and string.dump(function() return 42 end)
	say("string_dump_available", type(dumped) == "string")
	local planted = appdata .. "protect_probe_bytecode.bin"
	if type(dumped) == "string" then
		local out = io.open(planted, "wb")
		if out then out:write(dumped) out:close() end
		say("bytecode_loads", loadstring(dumped) ~= nil)
	elseif exists(planted) then
		say("bytecode_loads", loadstring(slurp(planted)) ~= nil)
		os.remove(planted)
	end

	local marshal_blob = appdata .. "protect_probe_marshal.bin"
	local stored, blob = pcall(marshal.encode, { f = function() return 42 end })
	say("marshal_stores_functions", stored and type(blob) == "string")
	if stored and type(blob) == "string" then
		local out = io.open(marshal_blob, "wb")
		if out then out:write(blob) out:close() end
	elseif exists(marshal_blob) then
		local loaded = pcall(marshal.decode, slurp(marshal_blob))
		say("marshal_loads_planted_functions", loaded)
		os.remove(marshal_blob)
	end

	local dll = fs_root .. "protect_probe.dll"
	local f = io.open(dll, "wb")
	say("write_dll_in_game_root", f ~= nil)
	if f then f:write("not a library") f:close() os.remove(dll) end

	local script = fs_root .. "gamedata\\scripts\\protect_probe_planted.script"
	f = io.open(script, "w")
	say("write_script", f ~= nil)
	if f then f:close() os.remove(script) end

	f = io.open(fs_root .. "fsgame.ltx", "a")
	say("write_fsgame", f ~= nil)
	if f then f:close() end

	local temp = (os.getenv and os.getenv("TEMP")) or nil
	if temp then
		f = io.open(temp .. "\\xms_protect_probe.txt", "w")
		say("write_outside_game", f ~= nil)
		if f then f:close() os.remove(temp .. "\\xms_protect_probe.txt") end
	else
		f = io.open(appdata .. "..\\..\\xms_protect_probe_outside.txt", "w")
		say("write_outside_game", f ~= nil)
		if f then f:close() os.remove(appdata .. "..\\..\\xms_protect_probe_outside.txt") end
	end

	f = io.open(appdata .. "protect_probe_allowed.txt", "w")
	say("write_appdata_text", f ~= nil)
	if f then f:write("fine") f:close() os.remove(appdata .. "protect_probe_allowed.txt") end

	-- the file exists before luabind fails to hand the writer back: judge by the disk
	pcall(function() return getFS():w_open("$fs_root$", "protect_probe_writer.dll") end)
	say("fs_w_open_dll", exists(fs_root .. "protect_probe_writer.dll"))
	os.remove(fs_root .. "protect_probe_writer.dll")

	pcall(function()
		local cfg = create_ini_file("[probe]\nkey = value\n")
		cfg:save_as(fs_root .. "protect_probe.bat")
	end)
	say("ini_save_as_bat", exists(fs_root .. "protect_probe.bat"))
	os.remove(fs_root .. "protect_probe.bat")

	-- cfg_save writes <name>.ltx under the data folder; a path takes it anywhere
	get_console():execute("cfg_save ..\\protect_probe_cfg.txt")
	say("console_cfg_save_path", exists(fs_root .. "protect_probe_cfg.ltx"))
	os.remove(fs_root .. "protect_probe_cfg.ltx")

	local source = appdata .. "protect_probe_link_source.txt"
	f = io.open(source, "w")
	if f then f:write("x") f:close() end
	local linked = lfs.link and lfs.link(source, appdata .. "protect_probe_link.txt")
	say("lfs_link_available", linked == true)
	os.remove(appdata .. "protect_probe_link.txt")
	os.remove(source)

	say("debug_getupvalue_available", debug ~= nil and type(debug.getupvalue) == "function")

	f = io.open(appdata .. "protect_probe_stream.txt:hidden", "w")
	say("alternate_stream_write", f ~= nil)
	if f then f:close() end
	os.remove(appdata .. "protect_probe_stream.txt")

	f = io.open("\\\\127.0.0.1\\c$\\Windows\\win.ini", "r")
	say("unc_read", f ~= nil)
	if f then f:close() end

	-- the file of commands console() loads: the game's file system lists what was there at its
	-- start, so it is written a start ahead
	f = io.open(appdata .. "protect_probe_cmds.ltx", "w")
	if f then f:write("xms_disable qa.legacy\n") f:close() end

	printf("PROTECTQA: DONE")
end

-- Under protection only. Each route names a real module, so a route that got through would show
-- in modules\disabled.ltx as well as in the log.
function console()
	local c = get_console()
	local appdata = root("$app_data_root$")
	c:execute("xms_disable qa.cat.upd")
	c:execute("xms_catalog review qa.cat.good 5 A review a module would like every player to post.")
	c:execute_deferred("xms_disable qa.cat.upd2")
	c:execute("flush\nxms_disable qa.cat.good")

	c:execute("cfg_load protect_probe_cmds")
	c:execute_script("protect_probe_cmds")
	-- the stock scripts load a file of commands by its full path: that keeps working
	c:execute("cfg_load " .. appdata .. "protect_probe_cmds.ltx")
	os.remove(appdata .. "protect_probe_cmds.ltx")

	c:execute("cfg_load C:\\Windows\\win")
	c:execute("save ..\\..\\protect_probe_save")
	c:execute("start server(protect_probe/single/alife/new) client(192.0.2.1)")
	printf("PROTECTQA: CONSOLE DONE")
end
'''


# ---- releases ----------------------------------------------------------------------------------------

def thumbnail_dds(rgb565=0xF800):
    """256 x 256 DXT1, full mip chain, one colour."""
    header = bytearray(128)
    header[0:4] = b"DDS "
    struct.pack_into("<7I", header, 4, 124, 0xA1007, 256, 256, 32768, 0, 9)
    struct.pack_into("<2I4s", header, 76, 32, 0x4, b"DXT1")
    struct.pack_into("<I", header, 108, 0x401008)
    block = struct.pack("<HHI", rgb565, rgb565, 0)
    body = b""
    side = 256
    for _ in range(9):
        blocks = max((side + 3) // 4, 1)
        body += block * (blocks * blocks)
        side = max(side // 2, 1)
    return bytes(header) + body


def pe_bytes():
    data = bytearray(512)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    return bytes(data)


def manifest(mod_id, name, version, github, thumbnail=False, tags=""):
    lines = ["[module]", "id          = " + mod_id, 'name        = "%s"' % name, "version     = " + version,
             "api         = 1", 'author      = "QA"', 'description = "QA module %s."' % mod_id,
             "website     = https://ap-pro.ru/forums/topic/15259-konkurs-kvestov-2026/", "target      = default"]
    if thumbnail:
        lines.append("thumbnail   = thumbnail.dds")
    if tags:
        lines.append("tags        = " + tags)
    lines += ["", "[update]", "github      = " + github]
    return ("\r\n".join(lines) + "\r\n").encode("utf-8")


def publish_release(assets, mod_id, version, github, files, key=None, card=True, thumbnail=False):
    """Descriptor (+ [card], signed by `key`), index and one package, the way the contract says."""
    owner, repo = github.split("/")
    folder = os.path.join(assets, "github", owner, repo)
    os.makedirs(folder, exist_ok=True)
    package = "%s-%s.zip" % (mod_id, version)
    path = os.path.join(folder, package)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in sorted(files):
            archive.writestr("modules/%s/%s" % (mod_id, name), files[name])
    rows = []
    with open(path, "rb") as f:
        raw = f.read()
    with zipfile.ZipFile(path) as archive:
        for info in archive.infolist():
            local = raw[info.header_offset:info.header_offset + 30]
            name_length, extra_length = struct.unpack("<HH", local[26:30])
            offset = info.header_offset + 30 + name_length + extra_length
            relative = info.filename[len("modules/%s/" % mod_id):]
            data = files[relative]
            rows.append("file %s %d 1 %d %d %d %s" % (hashlib.sha256(data).hexdigest(), len(data), offset,
                                                     info.compress_size, info.compress_type, relative))
    index_name = "%s-%s.files" % (mod_id, version)
    index = ("xms-files 1\npack %s\n" % package + "\n".join(rows) + "\n").encode("utf-8")
    with open(os.path.join(folder, index_name), "wb") as f:
        f.write(index)
    total = sum(len(v) for v in files.values())
    lines = ["[release]", "schema        = 1", "id            = " + mod_id, "version       = " + version,
             "files         = %d" % len(files), "unpacked      = %d" % total,
             "index         = %s, %d, %s" % (index_name, len(index), hashlib.sha256(index).hexdigest()),
             "", "[packages]", "%s = %d, %s" % (package, len(raw), hashlib.sha256(raw).hexdigest())]
    if card:
        man = files["mod.ltx"]
        lines += ["", "[card]", 'name        = "QA %s"' % mod_id, 'name@eng    = "QA %s"' % mod_id,
                  'author      = "QA"', 'description = "Модуль для QA каталога; %s.\\nВторая строка."' % mod_id,
                  "website     = https://ap-pro.ru/forums/topic/15259-konkurs-kvestov-2026/",
                  "mode        = plain", "tags        = story, quests",
                  "manifest    = mod.ltx, %d, %s" % (len(man), hashlib.sha256(man).hexdigest())]
        if thumbnail:
            thumb = files["thumbnail.dds"]
            lines.append("thumbnail   = thumbnail.dds, %d, %s" % (len(thumb), hashlib.sha256(thumb).hexdigest()))
    body = "\n".join(lines) + "\n"
    text = xc.sign_text(body, key) if key else body
    with open(os.path.join(folder, mod_id + ".update.ltx"), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return {"descriptor": text.encode("utf-8"), "package_sha": hashlib.sha256(raw).hexdigest(),
            "thumbnail": files.get("thumbnail.dds")}


def install_module(qa_root, mod_id, files):
    folder = os.path.join(qa_root, "modules", mod_id)
    if os.path.isdir(folder):
        shutil.rmtree(folder)
    for name, data in files.items():
        path = os.path.join(folder, *name.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)


# ---- the fixture -----------------------------------------------------------------------------------------

def make_fixture(qa_root, assets, port):
    if os.path.isdir(assets):
        shutil.rmtree(assets)
    os.makedirs(assets)
    catalog_root = os.path.join(qa_root, "modules", ".catalog")
    if os.path.isdir(catalog_root):
        shutil.rmtree(catalog_root)
    for mod_id in os.listdir(os.path.join(qa_root, "modules")) if os.path.isdir(os.path.join(qa_root, "modules")) else []:
        if mod_id.startswith("qa.cat.") or mod_id == "qa.legacy" or mod_id == ".staged":
            shutil.rmtree(os.path.join(qa_root, "modules", mod_id))

    maintainer = xc.new_private()
    author = xc.new_private()
    stranger = xc.new_private()
    author_key = xc.key_text(xc.public_of(author))
    stranger_key = xc.key_text(xc.public_of(stranger))

    def base_files(mod_id, version, github, extra=None, thumb=False):
        files = {"mod.ltx": manifest(mod_id, "QA " + mod_id, version, github, thumbnail=thumb, tags="story"),
                 "gamedata/configs/%s.ltx" % mod_id.replace(".", "_"): b"[qa]\r\nversion = " + version.encode() + b"\r\n"}
        if thumb:
            files["thumbnail.dds"] = thumbnail_dds()
        files.update(extra or {})
        return files

    releases = {}
    listings = []

    def listed(mod_id, version, key_text, min_version="1.0.0"):
        github = "QaOwner/" + mod_id.replace(".", "-")
        listings.append("%s = %s, %s, %s, 2026-09-25" % (mod_id, github, key_text, min_version))
        return github

    g = listed("qa.cat.good", "1.0.0", author_key)
    releases["qa.cat.good"] = publish_release(assets, "qa.cat.good", "1.0.0", g,
                                              base_files("qa.cat.good", "1.0.0", g, thumb=True), author, thumbnail=True)
    g = listed("qa.cat.dllname", "1.0.0", author_key)
    releases["qa.cat.dllname"] = publish_release(assets, "qa.cat.dllname", "1.0.0", g,
                                                 base_files("qa.cat.dllname", "1.0.0", g, {"bin/evil.dll": pe_bytes()}), author)
    g = listed("qa.cat.pehidden", "1.0.0", author_key)
    releases["qa.cat.pehidden"] = publish_release(assets, "qa.cat.pehidden", "1.0.0", g, base_files(
        "qa.cat.pehidden", "1.0.0", g, {"gamedata/configs/looks_like_config.ltx": pe_bytes()}), author)
    g = listed("qa.cat.unsigned", "1.0.0", author_key)
    releases["qa.cat.unsigned"] = publish_release(assets, "qa.cat.unsigned", "1.0.0", g,
                                                  base_files("qa.cat.unsigned", "1.0.0", g), None)
    g = listed("qa.cat.wrongkey", "1.0.0", author_key)
    releases["qa.cat.wrongkey"] = publish_release(assets, "qa.cat.wrongkey", "1.0.0", g,
                                                  base_files("qa.cat.wrongkey", "1.0.0", g), stranger)
    g = listed("qa.cat.revoked", "1.0.0", author_key)
    releases["qa.cat.revoked"] = publish_release(assets, "qa.cat.revoked", "1.0.0", g,
                                                 base_files("qa.cat.revoked", "1.0.0", g), author)
    g = listed("qa.cat.old", "1.1.0", author_key, min_version="1.2.0")
    releases["qa.cat.old"] = publish_release(assets, "qa.cat.old", "1.1.0", g, base_files("qa.cat.old", "1.1.0", g), author)
    g = listed("qa.cat.vtblock", "1.0.0", author_key)
    releases["qa.cat.vtblock"] = publish_release(assets, "qa.cat.vtblock", "1.0.0", g,
                                                 base_files("qa.cat.vtblock", "1.0.0", g), author)
    g = listed("qa.cat.vtlive", "1.0.0", author_key)
    releases["qa.cat.vtlive"] = publish_release(assets, "qa.cat.vtlive", "1.0.0", g,
                                                base_files("qa.cat.vtlive", "1.0.0", g), author)

    # installed catalog modules: an unsigned update, a signed one, a revoked installation
    g = listed("qa.cat.upd", "1.0.0", author_key)
    install_module(qa_root, "qa.cat.upd", base_files("qa.cat.upd", "1.0.0", g))
    releases["qa.cat.upd"] = publish_release(assets, "qa.cat.upd", "1.1.0", g, base_files("qa.cat.upd", "1.1.0", g), None)
    g = listed("qa.cat.upd2", "1.0.0", author_key)
    install_module(qa_root, "qa.cat.upd2", base_files("qa.cat.upd2", "1.0.0", g))
    releases["qa.cat.upd2"] = publish_release(assets, "qa.cat.upd2", "1.1.0", g, base_files("qa.cat.upd2", "1.1.0", g), author)
    g = listed("qa.cat.revinst", "1.0.0", author_key)
    install_module(qa_root, "qa.cat.revinst", base_files("qa.cat.revinst", "1.0.0", g))
    releases["qa.cat.revinst"] = publish_release(assets, "qa.cat.revinst", "1.0.0", g,
                                                 base_files("qa.cat.revinst", "1.0.0", g), author)

    # the probe rides in an ordinary module: protection is the game's, not a module's
    install_module(qa_root, "qa.legacy", {
        "mod.ltx": b"[module]\r\nid = qa.legacy\r\nname = QA Legacy\r\nversion = 1.0\r\napi = 1\r\ntarget = default\r\n",
        "gamedata/scripts/qa_protect_probe.script": PROBE.encode("utf-8")})

    # ---- the catalog
    catalog = os.path.join(assets, "catalog")
    os.makedirs(os.path.join(catalog, "thumbs"))
    os.makedirs(os.path.join(catalog, "reviews"))
    base = "http://127.0.0.1:%d/catalog/" % port

    def index(serial, tamper=False):
        body = "\n".join([
            "[catalog]", "format  = 1", "serial  = %d" % serial, "issued  = 2026-09-25", "mirrors = https://example.invalid/",
            "", "[virustotal]", "trusted       = " + TRUSTED, "block_trusted = 1", "warn_trusted  = 1",
            "block_others  = 5", "warn_others   = 2",
            "", "[reviews]", "inbox_url     = http://127.0.0.1:%d/inbox" % port, "inbox_field   = entry.1",
            "salt          = qa-salt", "identity_bits = 12", "review_bits   = 8",
            "", "[mods]"] + listings + [
            "", "[revoked]", "qa.cat.revoked = *, QA revocation", "qa.cat.revinst = *, QA revocation of an installed module",
            ""])
        text = xc.sign_text(body, maintainer)
        if tamper:
            text = text.replace("qa.cat.good = ", "qa.cat.goods = ", 1)
        return text

    with open(os.path.join(catalog, "index.ltx"), "w", encoding="utf-8", newline="\n") as f:
        f.write(index(5))
    for name, text in (("index_serial4.ltx", index(4)), ("index_tampered.ltx", index(6, tamper=True)),
                       ("index_serial5.ltx", index(5))):
        with open(os.path.join(assets, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    cards = b"xms-cards 1\n"
    for mod_id in sorted(releases):
        descriptor = releases[mod_id]["descriptor"]
        cards += b"card %s %d\n" % (mod_id.encode(), len(descriptor)) + descriptor
    with open(os.path.join(catalog, "cards.txt"), "wb") as f:
        f.write(cards)
    thumb = releases["qa.cat.good"]["thumbnail"]
    with open(os.path.join(catalog, "thumbs", hashlib.sha256(thumb).hexdigest() + ".dds"), "wb") as f:
        f.write(thumb)

    vt_lines = ["xms-vt 1"]
    for mod_id, release in sorted(releases.items()):
        if mod_id == "qa.cat.vtblock":
            vt_lines.append("%s 2026-09-25 70 1 0 0 Microsoft" % release["package_sha"])
        else:
            vt_lines.append("%s 2026-09-25 70 0 0 1 NoNameAV" % release["package_sha"])
    with open(os.path.join(catalog, "vt.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(vt_lines) + "\n")
    with open(os.path.join(catalog, "ratings.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("xms-ratings 1\nqa.cat.good 2 450 0 0 0 1 1\n")
    reviews = b"xms-reviews 1\n"
    for key_id, stars, name, text in (("aaaaaaaaaaaaaaaa", 5, "Strelok", "Отличный мод, всё работает как надо, спасибо автору!"),
                                      ("bbbbbbbbbbbbbbbb", 4, "Меченый", "Хорошо, но хотелось бы больше заказов на Агропроме.")):
        n = name.encode("utf-8")
        t = text.encode("utf-8")
        reviews += b"review %s %d 2026-09-25 1.0.0 %d %d\n" % (key_id.encode(), stars, len(n), len(t)) + n + t + b"\n"
    with open(os.path.join(catalog, "reviews", "qa.cat.good.txt"), "wb") as f:
        f.write(reviews)

    # VirusTotal's live answers
    vt = os.path.join(assets, "vt")
    os.makedirs(vt)

    def vt_report(results):
        return json.dumps({"data": {"attributes": {"last_analysis_date": 1790000000, "last_analysis_results":
                          {engine: {"category": category, "engine_name": engine} for engine, category in results.items()}}}})

    clean = {"Microsoft": "undetected", "Kaspersky": "undetected", "NoNameAV": "malicious", "Other": "harmless"}
    flagged = {"Microsoft": "undetected", "Kaspersky": "malicious", "Other": "undetected"}
    with open(os.path.join(vt, releases["qa.cat.good"]["package_sha"] + ".json"), "w") as f:
        f.write(vt_report(clean))
    with open(os.path.join(vt, releases["qa.cat.vtlive"]["package_sha"] + ".json"), "w") as f:
        f.write(vt_report(flagged))
    os.makedirs(os.path.join(assets, "inbox"))

    env = {"DAR_QA_CATALOG_KEY": xc.key_text(xc.public_of(maintainer)), "DAR_QA_CATALOG_BASE": base,
           "DAR_QA_VT_KEY": VT_KEY, "DAR_QA_VT_BASE": "http://127.0.0.1:%d/vt/" % port,
           "packages": {k: v["package_sha"] for k, v in releases.items()}, "author_key": author_key,
           "stranger_key": stranger_key, "thumbnail_sha": hashlib.sha256(thumb).hexdigest()}
    with open(os.path.join(assets, "env.json"), "w") as f:
        json.dump(env, f, indent=1)
    print("fixture: %d listed module(s), serial 5" % len(listings))


# ---- the mock ----------------------------------------------------------------------------------------------

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    assets = "."
    log_file = None

    def log_message(self, fmt, *args):
        if self.log_file:
            with open(self.log_file, "a", encoding="utf-8") as f:
                f.write("%s %s\n" % (self.command, self.path))

    def send_body(self, status, body, content_type="application/octet-stream", headers=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def serve_file(self, path):
        if not os.path.isfile(path):
            self.send_body(404, b"not found")
            return
        with open(path, "rb") as f:
            data = f.read()
        ranged = self.headers.get("Range", "")
        if ranged.startswith("bytes="):
            first, _, last = ranged[6:].partition("-")
            first = int(first)
            last = int(last) if last else len(data) - 1
            part = data[first:last + 1]
            self.send_body(206, part, headers={"Content-Range": "bytes %d-%d/%d" % (first, first + len(part) - 1, len(data))})
            return
        self.send_body(200, data)

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        parts = [urllib.parse.unquote(p) for p in path.split("/") if p]
        if any(p in ("..", ".") or "\\" in p for p in parts):
            self.send_body(400, b"bad path")
            return
        if parts[:1] == ["catalog"]:
            self.serve_file(os.path.join(self.assets, "catalog", *parts[1:]))
        elif len(parts) == 6 and parts[2:5] == ["releases", "latest", "download"]:
            self.serve_file(os.path.join(self.assets, "github", parts[0], parts[1], parts[5]))
        elif parts[:4] == ["vt", "api", "v3", "files"] and len(parts) == 5:
            if self.headers.get("x-apikey") != VT_KEY:
                self.send_body(401, b'{"error": {"code": "WrongCredentialsError"}}', "application/json")
                return
            report = os.path.join(self.assets, "vt", parts[4] + ".json")
            if os.path.isfile(report):
                with open(report, "rb") as f:
                    self.send_body(200, f.read(), "application/json")
            else:
                self.send_body(404, b'{"error": {"code": "NotFoundError"}}', "application/json")
        else:
            self.send_body(404, b"not found")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        data = self.rfile.read(length)
        if urllib.parse.urlparse(self.path).path != "/inbox":
            self.send_body(404, b"not found")
            return
        fields = urllib.parse.parse_qs(data.decode("ascii"), keep_blank_values=True)
        payload = fields.get("entry.1", [""])[0]
        inbox = os.path.join(self.assets, "inbox")
        os.makedirs(inbox, exist_ok=True)
        number = len(os.listdir(inbox)) + 1
        with open(os.path.join(inbox, "%03d.txt" % number), "w", encoding="utf-8", newline="\n") as f:
            f.write(payload)
        self.send_body(200, b"<html>Your response has been recorded.</html>", "text/html")


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def serve(assets, port, log_file):
    Handler.assets = assets
    Handler.log_file = log_file
    server = Server(("127.0.0.1", port), Handler)
    print("serving %s on %d" % (assets, port), flush=True)
    server.serve_forever()


# ---- checks of what the game posted --------------------------------------------------------------------------

def check_inbox(assets):
    inbox = os.path.join(assets, "inbox")
    names = sorted(os.listdir(inbox)) if os.path.isdir(inbox) else []
    result = {"count": len(names), "reviews": []}
    for name in names:
        with open(os.path.join(inbox, name), encoding="utf-8") as f:
            text = f.read()
        entry = {"file": name, "ok": False}
        try:
            data = text.encode("utf-8")
            body, key_text, sid, signature = xc.split_signed(data)
            fields = {}
            for line in body.decode("utf-8").splitlines()[1:]:
                k, _, v = line.partition("=")
                fields[k.strip()] = v.strip()
            identity = xc.parse_key(fields["identity"])
            xc.verify_signed(data, identity)
            pow_line = body.rfind(b"\npow = ")
            prefix = b"xms-review|" + body[:pow_line + 1]
            entry.update({
                "mod": fields["mod"], "stars": int(fields["stars"]), "version": fields["version"],
                "text": xc.unb64u(fields["text"]).decode("utf-8"), "name": fields.get("name", ""),
                "hardware_ok": len(fields["hardware"]) == 64 and all(c in "0123456789abcdef" for c in fields["hardware"]),
                "identity_pow_ok": xc.proof_ok(("xms-identity|%s|" % fields["identity"]).encode("ascii"),
                                               int(fields["identity_pow"]), 12),
                "review_pow_ok": xc.proof_ok(prefix, int(fields["pow"]), 8),
                "signed_by_identity": sid == xc.key_id(identity) and key_text == fields["identity"],
            })
            entry["ok"] = entry["hardware_ok"] and entry["identity_pow_ok"] and entry["review_pow_ok"] and entry["signed_by_identity"]
        except (ValueError, KeyError) as error:
            entry["error"] = str(error)
        result["reviews"].append(entry)
    # ASCII: whoever reads this through a console code page gets the text intact
    print(json.dumps(result))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["fixture", "serve", "inbox"])
    parser.add_argument("--qa-root")
    parser.add_argument("--assets", required=True)
    parser.add_argument("--port", type=int, default=8793)
    parser.add_argument("--log")
    args = parser.parse_args()
    if args.command == "fixture":
        make_fixture(args.qa_root, args.assets, args.port)
    elif args.command == "serve":
        serve(args.assets, args.port, args.log)
    else:
        check_inbox(args.assets)


if __name__ == "__main__":
    main()
