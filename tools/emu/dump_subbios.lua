-- Dump the (decompressed) Sega CD Sub BIOS area 0x0000-0x5FFF and Main BIOS ROM from a running MAME to work/mame/ (local, never committed).
local frame = 0
emu.register_frame_done(function()
  frame = frame + 1
  if frame == 1000 then
    local sub = manager.machine.devices[":segacd:segacd_68k"].spaces["program"]
    local f = io.open("work/mame/subbios.bin", "wb")
    for a = 0, 0x5fff do f:write(string.char(sub:read_u8(a))) end
    f:close()
    manager.machine:exit()
  end
end)
