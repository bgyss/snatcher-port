trace work/mame/bram_trace.log,maincpu,noloop,{0}
bpset 70ee,1,{tracelog "MAIN BURAM d0=%x a0=%x a1=%x d1=%x\n",d0&ffff,a0,a1,d1;g}
bpset 5f16,1,{tracelog "SUB BURAM d0=%x a0=%x a1=%x d1=%x\n",d0&ffff,a0,a1,d1;g}
g
