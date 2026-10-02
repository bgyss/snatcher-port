-- MAME oracle: per-frame state of the Snatcher Sega CD engine, for diffing against engine/ (scd_headless).
-- Usage: mame segacd ... -autoboot_script tools/emu/trace_state.lua  (output: work/mame/state_trace.txt)
local out = io.open("work/mame/state_trace.txt", "w")
local main = manager.machine.devices[":maincpu"]
local mem = main.spaces["program"]
local sub = manager.machine.devices[":segacd:segacd_68k"]
local smem = sub and sub.spaces["program"]
local frame = 0
emu.register_frame_done(function()
  frame = frame + 1
  local function r8(a) return mem:read_u8(a) end
  local function r16(a) return mem:read_u16(a) end
  local s = string.format("%d e028=%02x e022=%04x e020=%04x flagsM=%02x flagsS=%02x st=%02x sw=%04x",
    frame, r8(0xffe028), r16(0xffe022), r16(0xffe020), r8(0xa1200e), r8(0xa1200f), r8(0xa1202f), r16(0xa12020))
  s = s .. string.format(" e04a=%04x e04c=%04x ef02=%04x pcM=%06x", r16(0xffe04a), r16(0xffe04c), r16(0xffef02), main.state["CURPC"].value)
  if smem then
    s = s .. string.format(" pcS=%06x", sub.state["CURPC"].value)
    s = s .. string.format(" s7840=%04x s78c8=%04x", smem:read_u16(0x7840), smem:read_u16(0x78c8))
  end
  out:write(s .. "\n")
  if frame >= 1300 then out:flush(); manager.machine:exit() end
end)
