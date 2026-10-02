-- MAME oracle: snapshot at chosen frames. Frames come from env MAME_SNAP_FRAMES="2300,2500"; stops after the last.
local want, last = {}, 0
for f in string.gmatch(os.getenv("MAME_SNAP_FRAMES") or "", "%d+") do want[tonumber(f)] = true; last = math.max(last, tonumber(f)) end
local frame = 0
emu.register_frame_done(function()
  frame = frame + 1
  if want[frame] then manager.machine.video:snapshot() end
  if frame >= last then manager.machine:exit() end
end)
