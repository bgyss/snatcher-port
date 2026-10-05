-- MAME oracle for the intro skip: scripted_input.lua (button presses, per-frame state incl. the CDBSTAT area at Sub $5E80)
-- plus cdbios_cmd_log.lua (every command queued to the real BIOS, with BIOS mode/flag changes). Run from the repo root; env as
-- for scripted_input.lua (MAME_PRESSES, MAME_END). Outputs work/mame/input_state.txt and work/mame/cdbios_cmds.txt.
dofile("tools/emu/cdbios_cmd_log.lua")
dofile("tools/emu/scripted_input.lua")
