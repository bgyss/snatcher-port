-- MAME oracle: log _BURAM calls (Main $70EE, Sub $5F16) with arguments to work/mame/bram_trace.log
local dbg = manager.machine.debugger
local frame = 0
local started = false
emu.register_frame_done(function()
  frame = frame + 1
  if frame == 5 and dbg then
    dbg:command("trace work/mame/bram_trace.log,maincpu,noloop,{0}")
    dbg:command('bpset 70ee,1,{tracelog "MAIN BURAM d0=%x a0=%x a1=%x d1=%x\\n",d0&ffff,a0,a1,d1;g}')
    started = true
  end
  if frame >= 3500 then manager.machine:exit() end
end)
