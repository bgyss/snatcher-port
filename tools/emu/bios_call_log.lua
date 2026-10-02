-- MAME oracle: log _CDBIOS ($5F22) and _BURAM ($5F16) calls of the Sub CPU with d0/d1/a0/a1 and game-frame stamps.
-- Output work/mame/bios_calls.txt. Taps are installed once the game runs ($FFE020 >= 1).
local sub = manager.machine.devices[":segacd:segacd_68k"]
local smem = sub.spaces["program"]
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local log = {}
local gf, installed = 0, false
local limit = tonumber(os.getenv("MAME_END") or "3000")
local function rec(tag)
  return function(offset, data, mask)
    local st = sub.state
    log[#log + 1] = string.format("%d %s d0=%04x d1=%08x a0=%06x a1=%06x", gf, tag, st["D0"].value & 0xffff, st["D1"].value, st["A0"].value & 0xffffff, st["A1"].value & 0xffffff)
  end
end
emu.register_frame_done(function()
  local e20 = mem:read_u16(0xffe020)
  if not installed and e20 >= 1 and e20 < 0x100 then
    installed = true
    smem:install_read_tap(0x5f22, 0x5f23, "cdbios", rec("CDBIOS"))
    smem:install_read_tap(0x5f16, 0x5f17, "buram", rec("BURAM"))
    smem:install_read_tap(0x5f10, 0x5f11, "vsync", rec("VSYNC"))
  end
  if installed then gf = gf + 1 end
  if gf >= limit then
    local f = io.open("work/mame/bios_calls.txt", "w"); f:write(table.concat(log, "\n"), "\n"); f:close()
    manager.machine:exit()
  end
end)
