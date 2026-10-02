-- MAME oracle: log every VDP port word written by the Main CPU once the game is running (taps must be installed
-- after the BIOS hands over, which remaps the Main CPU space). Output work/mame/vdp_ctrl.txt: "C xxxx" control, "D xxxx" data.
local limit = tonumber(os.getenv("MAME_VDPLOG_FRAMES") or "60")
local mem = manager.machine.devices[":maincpu"].spaces["program"]
local log = {}
local installed, gframes = false, 0
local tap
emu.register_frame_done(function()
  local e20 = mem:read_u16(0xffe020)
  if not installed and e20 >= 1 and e20 < 0x100 then
    installed = true
    tap = mem:install_write_tap(0xc00000, 0xc00007, "vdpctrl", function(offset, data, mask)
      local kind = offset >= 0xc00004 and "C" or "D"
      if mask == 0xffffffff then
        log[#log + 1] = string.format("%s %04x", kind, (data >> 16) & 0xffff)
        log[#log + 1] = string.format("%s %04x", kind, data & 0xffff)
      else
        log[#log + 1] = string.format("%s %04x", kind, data & 0xffff)
      end
    end)
  end
  if installed then gframes = gframes + 1 end
  if gframes >= limit then
    local out = io.open("work/mame/vdp_ctrl.txt", "w")
    out:write(table.concat(log, "\n"), "\n"); out:close()
    manager.machine:exit()
  end
end)
