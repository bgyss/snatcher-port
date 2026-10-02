-- MAME oracle: press pad buttons at given *game* frames and dump screenshots/state.
-- Env: MAME_PRESSES="1700:S:6,2500:D:3" (frame:buttons:hold, buttons from U D L R B C A S), MAME_SHOTS="2400,3000",
--      MAME_END=last game frame. Game frame 0 = first frame with $FFE020 >= 1 (matches scd_headless frame numbering).
-- Output: work/mame/shot_<frame>.ppm and work/mame/input_state.txt (one line per game frame).
local presses = {}
for f, b, h in string.gmatch(os.getenv("MAME_PRESSES") or "", "(%d+):(%a+):(%d+)") do
  presses[#presses + 1] = {frame = tonumber(f), buttons = b, hold = tonumber(h)}
end
local shots = {}
for f in string.gmatch(os.getenv("MAME_SHOTS") or "", "%d+") do shots[tonumber(f)] = true end
local dumps = {}
for f in string.gmatch(os.getenv("MAME_DUMPS") or "", "%d+") do dumps[tonumber(f)] = true end
local last = tonumber(os.getenv("MAME_END") or "3000")
local names = {U = "P1 Up", D = "P1 Down", L = "P1 Left", R = "P1 Right", B = "P1 B", C = "P1 C", A = "P1 A", S = "P1 Start"}
local port = manager.machine.ioport.ports[":ctrl1:mdpad:PAD"]
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local smem = manager.machine.devices[":segacd:segacd_68k"].spaces["program"]
local out = io.open("work/mame/input_state.txt", "w")
local started, gf = false, -1
local function shot(n)
  local scr = manager.machine.screens[":screen"] or manager.machine.screens:at(1)
  local w, h = scr.width, scr.height
  local px = scr:pixels()
  local f = io.open(string.format("work/mame/shot_%05d.ppm", n), "wb")
  f:write(string.format("P6\n%d %d\n255\n", w, h))
  local t = {}
  for i = 0, w * h - 1 do
    local a, b, c = string.byte(px, i * 4 + 1, i * 4 + 3)
    t[#t + 1] = string.char(c, b, a)
  end
  f:write(table.concat(t)); f:close()
end
emu.register_frame_done(function()
  local e20 = mem:read_u16(0xffe020)
  if not started and e20 >= 1 and e20 < 0x100 then started = true; gf = 0 end
  if started then
    gf = gf + 1
    local down = {}
    for _, p in ipairs(presses) do
      if gf >= p.frame and gf < p.frame + p.hold then
        for c in p.buttons:gmatch(".") do down[names[c]] = true end
      end
    end
    for _, n in pairs(names) do port.fields[n]:set_value(down[n] and 1 or 0) end
    local comm = ""
    for a = 0xa12010, 0xa1202e, 2 do comm = comm .. string.format("%04x ", mem:read_u16(a)) end
    local bs = string.format("%04x %04x %08x %08x", smem:read_u16(0x5e80), smem:read_u16(0x5e82), (smem:read_u16(0x5e88) << 16) | smem:read_u16(0x5e8a), (smem:read_u16(0x5e8c) << 16) | smem:read_u16(0x5e8e))
    out:write(string.format("%d e022=%04x e06c=%04x e020=%04x f=%02x%02x bstat=%s comm=%s\n", gf, mem:read_u16(0xffe022), mem:read_u16(0xffe06c), mem:read_u16(0xffe020), mem:read_u8(0xa1200e), mem:read_u8(0xa1200f), bs, comm))
    if shots[gf] then shot(gf) end
    if dumps[gf] then
      local d = io.open(string.format("work/mame/mainram_%05d.bin", gf), "wb")
      for a = 0, 0xffff do d:write(string.char(mem:read_u8(0xff0000 + a))) end
      d:close()
      d = io.open(string.format("work/mame/subram_%05d.bin", gf), "wb")
      for a = 0x7000, 0xcfff do d:write(string.char(smem:read_u8(a))) end
      d:close()
    end
    if gf >= last then out:close(); manager.machine:exit() end
  end
end)
