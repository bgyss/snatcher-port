-- MAME oracle: dump the screen at chosen frames as PPM (works with -video none).
-- Env MAME_SNAP_FRAMES="2300,2500" (counted from MAME start; the Sega CD BIOS takes ~1160 frames); output work/mame/snap2/f<N>.ppm
local want, last = {}, 0
for f in string.gmatch(os.getenv("MAME_SNAP_FRAMES") or "", "%d+") do want[tonumber(f)] = true; last = math.max(last, tonumber(f)) end
local frame = 0
local function dump(n)
  local scr = manager.machine.screens[":screen"] or manager.machine.screens:at(1)
  local w, h = scr.width, scr.height
  local px = scr:pixels()
  local f = io.open(string.format("work/mame/snap2/f%05d.ppm", n), "wb")
  f:write(string.format("P6\n%d %d\n255\n", w, h))
  local t = {}
  for i = 0, w * h - 1 do
    local a, b, c, d = string.byte(px, i * 4 + 1, i * 4 + 4)  -- little endian BGRA
    t[#t + 1] = string.char(c, b, a)
  end
  f:write(table.concat(t)); f:close()
end
emu.register_frame_done(function()
  frame = frame + 1
  if want[frame] then dump(frame) end
  if frame >= last then manager.machine:exit() end
end)
