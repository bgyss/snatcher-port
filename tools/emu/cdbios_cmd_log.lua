-- MAME oracle: log every command the game queues to the real Sega CD BIOS (_CDBIOS d0 < $80 is stored at Sub $5AF2 by the
-- entry code at $2F2C), with game-frame stamps, plus the BIOS drive-mode word ($5802) and flag byte ($5B42) when they change.
-- Output work/mame/cdbios_cmds.txt. Combine with scripted_input.lua through skip_probe.lua. (A read tap on $5F22 only
-- catches the first calls: MAME caches opcode fetches, write taps on data are reliable.)
local sub = manager.machine.devices[":segacd:segacd_68k"]
local smem = sub.spaces["program"]
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local out = io.open("work/mame/cdbios_cmds.txt", "w")
out:setvbuf("line")
local gf, installed = 0, false
local last5802, last5b42
emu.register_frame_done(function()
  local e20 = mem:read_u16(0xffe020)
  if not installed and e20 >= 1 and e20 < 0x100 then
    installed = true
    smem:install_write_tap(0x5af2, 0x5af3, "cmd", function(offset, data, mask)
      if offset == 0x5af2 or offset == 0 then
        out:write(string.format("%d cmd %04x a0=%06x\n", gf, data & 0xffff, sub.state["A0"].value & 0xffffff))
      end
    end)
  end
  if installed then
    gf = gf + 1
    local m, f = smem:read_u16(0x5802), smem:read_u8(0x5b42)
    if m ~= last5802 or f ~= last5b42 then
      out:write(string.format("%d state mode5802=%04x flags5b42=%02x status=%04x\n", gf, m, f, smem:read_u16(0x5e80)))
      last5802, last5b42 = m, f
    end
  end
end)
