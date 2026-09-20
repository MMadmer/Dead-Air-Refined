-- Prints how pairs() walks a set of tables. Two LuaJIT builds that print the same text walk the
-- script state of a save in the same order, which is what X-Ray scripts rely on when they write
-- with one pairs() loop and read with another. Deterministic: no clock, no addresses, no math.random.

local lines = {}

local function emit(name, walk)
    lines[#lines + 1] = name .. ": " .. table.concat(walk, " ")
end

local function walk_keys(t)
    local walk = {}
    for key, value in pairs(t) do
        -- A nested table prints as its address, which is the one thing that may differ.
        local shown = type(value) == "table" and "table" or tostring(value)
        walk[#walk + 1] = tostring(key) .. "=" .. shown
    end
    return walk
end

-- A cheap stand-in for the text itself once the walks get long.
local function digest(walk)
    local a, b = 1, 0
    for _, item in ipairs(walk) do
        for i = 1, #item do
            a = (a + item:byte(i)) % 65521
            b = (b + a) % 65521
        end
        a = (a + 32) % 65521
        b = (b + a) % 65521
    end
    return string.format("%d entries, adler %04x%04x", #walk, b, a)
end

-- 1. Names the game scripts use as keys, inserted in source order.
local game = {}
for _, key in ipairs({
    "id", "name", "section", "position", "level_vertex_id", "game_vertex_id", "story_id",
    "squad_id", "smart_terrain", "community", "rank", "reputation", "money", "health", "power",
    "radiation", "bleeding", "satiety", "thirst", "sleep", "psy_health", "task", "stage",
    "state", "timer", "x", "y", "z", "h", "p", "b", "a", "", "_", "1", "01", "one",
}) do
    game[key] = #key
end
emit("game keys", walk_keys(game))

-- 2. Keys of every length class the hash samples differently (1-3, 4-7, 8+ bytes), generated.
local generated = {}
for i = 1, 4096 do
    generated[string.rep(string.char(97 + i % 26), 1 + i % 23) .. i] = i
end
emit("generated", { digest(walk_keys(generated)) })

-- 3. Array part, hash part and the border between them.
local mixed = {}
for i = 1, 40 do mixed[i] = i end
for i = 100, 60, -7 do mixed[i] = -i end
for i = 1, 40 do mixed["k" .. i] = i end
mixed[2.5] = "float"
mixed[-1] = "negative"
mixed[0] = "zero"
mixed[true] = "true"
mixed[false] = "false"
emit("mixed", { digest(walk_keys(mixed)) })

-- 4. Removal and reinsertion: tombstones and the free pointer decide where a key returns to.
local churn = {}
for i = 1, 300 do churn["slot_" .. i] = i end
for i = 1, 300, 3 do churn["slot_" .. i] = nil end
for i = 301, 420 do churn["slot_" .. i] = i end
for i = 1, 300, 6 do churn["slot_" .. i] = -i end
emit("churn", { digest(walk_keys(churn)) })

-- 5. Growth: every rehash reorders, so the walk is sampled on the way up.
local grow, samples = {}, {}
for i = 1, 2000 do
    grow["g" .. i * 7919 % 10007] = i
    if i == 3 or i == 17 or i == 129 or i == 1025 or i == 2000 then
        samples[#samples + 1] = digest(walk_keys(grow))
    end
end
emit("growth", samples)

-- 6. Table constructors: the template table of a prototype, duplicated at run time.
local function record(n)
    return { id = n, name = "n" .. n, pos = { x = n, y = -n, z = 0 }, flags = { "a", "b", n }, alive = true }
end
local records = {}
for i = 1, 3 do
    local r = record(i)
    records[#records + 1] = table.concat(walk_keys(r), ",") .. "|" .. table.concat(walk_keys(r.pos), ",")
end
emit("constructors", records)

-- 7. next() after a partial walk, the way scripts resume iteration.
local resume, key = {}, nil
for i = 1, 5 do
    key = next(game, key)
    resume[#resume + 1] = tostring(key)
end
emit("next", resume)

for _, line in ipairs(lines) do
    print(line)
end
