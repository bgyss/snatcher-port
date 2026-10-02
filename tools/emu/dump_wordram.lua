-- MAME oracle: dump Word RAM and Main RAM when the Main frame counter ($FFE020) reaches MAME_E020 (hex, default 600).
local target = tonumber(os.getenv("MAME_E020") or "600", 16)
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local dataram = manager.machine.memory.shares[":segacd:dataram"]
local done = false
emu.register_frame_done(function()
  if done then return end
  local c = mem:read_u16(0xffe020)
  local st = mem:read_u16(0xffe022)
  if c >= target and c < 0x4000 and st >= 3 and st < 0x100 then
    done = true
    local f = io.open("work/mame/wordram.bin", "wb")
    for a = 0, 0x3ffff do f:write(string.char(dataram:read_u8(a))) end
    f:close()
    f = io.open("work/mame/mainram.bin", "wb")
    for a = 0, 0xffff do f:write(string.char(mem:read_u8(0xff0000 + a))) end
    f:close()
    local scr = manager.machine.screens[":screen"] or manager.machine.screens:at(1)
    local w, h = scr.width, scr.height
    local px = scr:pixels()
    local o = io.open("work/mame/screen.ppm", "wb")
    o:write(string.format("P6\n%d %d\n255\n", w, h))
    local t = {}
    for i = 0, w * h - 1 do
      local a, b, c = string.byte(px, i * 4 + 1, i * 4 + 3)
      t[#t + 1] = string.char(c, b, a)
    end
    o:write(table.concat(t)); o:close()
    manager.machine:save("dump")
    manager.machine:exit()
  end
end)
