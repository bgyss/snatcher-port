// Pre-analysis setup for Snatcher (Sega CD) programs: memory map, hardware
// register labels, extra code blocks and entry points.
//
// Script args: main                      Main CPU program (IP imported at 0xFF0000)
//              sub <path/to/subcode.bin> Sub CPU program (SP imported at 0x6000)
//
// Addresses come from docs/DISC_LAYOUT.md and the public Sega CD hardware
// manuals. abs.w operands sign-extend to 0xFFFFxxxx in Ghidra's 32-bit space,
// so high RAM / Gate Array are mirrored there (the real bus is 24-bit).
//@category Snatcher

import java.io.FileInputStream;
import java.io.File;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class SnatcherSetup extends GhidraScript {

    private Memory mem;

    private Address a(long off) {
        return toAddr(off);
    }

    private void io(String name, long start, long len) throws Exception {
        MemoryBlock b = mem.createUninitializedBlock(name, a(start), len, false);
        b.setVolatile(true);
        b.setRead(true);
        b.setWrite(true);
        b.setExecute(false);
    }

    private void ram(String name, long start, long len) throws Exception {
        MemoryBlock b = mem.createUninitializedBlock(name, a(start), len, false);
        b.setRead(true);
        b.setWrite(true);
        b.setExecute(true);
    }

    private void mirror(String name, long start, long target, long len) throws Exception {
        MemoryBlock b = mem.createByteMappedBlock(name, a(start), a(target), len, false);
        b.setRead(true);
        b.setWrite(true);
        b.setExecute(true);
    }

    private void label(long addr, String name) throws Exception {
        createLabel(a(addr), name, true, SourceType.IMPORTED);
    }

    private void entry(long addr, String name) throws Exception {
        Address ad = a(addr);
        currentProgram.getSymbolTable().addExternalEntryPoint(ad);
        disassemble(ad);
        createFunction(ad, name);
    }

    @Override
    protected void run() throws Exception {
        mem = currentProgram.getMemory();
        String[] args = getScriptArgs();
        String mode = args.length > 0 ? args[0] : "main";
        mem.getBlocks()[0].setName(mode.equals("main") ? "ip" : "sp");
        if (mode.equals("main")) {
            setupMain();
        } else {
            setupSub(args[1]);
        }
    }

    private void setupMain() throws Exception {
        // Main CPU map in CD-boot mode (BIOS ROM at 0x000000)
        ram("bios_main", 0x000000, 0x20000);
        ram("prg_ram_window", 0x020000, 0x20000);
        ram("word_ram", 0x200000, 0x40000);
        io("z80", 0xA00000, 0x10000);
        io("io", 0xA10000, 0x20);
        io("z80ctl", 0xA11100, 0x200);
        io("gate_array", 0xA12000, 0x30);
        io("vdp", 0xC00000, 0x20);
        long ipEnd = 0xFF0000 + mem.getBlock("ip").getSize();
        ram("work_ram", ipEnd, 0x1000000 - ipEnd);
        mirror("work_ram_absw", 0xFFFF0000L, 0xFF0000, 0x10000);

        label(0xC00000, "VDP_DATA");
        label(0xC00004, "VDP_CTRL");
        label(0xC00008, "VDP_HV");
        label(0xC00011, "PSG");
        label(0xA10001, "IO_VERSION");
        label(0xA10003, "IO_DATA1");
        label(0xA10005, "IO_DATA2");
        label(0xA10009, "IO_CTRL1");
        label(0xA1000B, "IO_CTRL2");
        label(0xA11100, "Z80_BUSREQ");
        label(0xA11200, "Z80_RESET");
        label(0xA12000, "GA_RESET");
        label(0xA12002, "GA_MEMMODE");
        label(0xA12006, "GA_HINT_VECTOR");
        label(0xA1200E, "GA_COMFLAGS_MAIN");
        label(0xA1200F, "GA_COMFLAGS_SUB");
        for (int i = 0; i < 8; i++) {
            label(0xA12010 + i * 2, "GA_COMCMD" + i);
            label(0xA12020 + i * 2, "GA_COMSTAT" + i);
        }

        entry(0xFF0000, "ip_security_start");
        entry(0xFF0584, "ip_main");  // target of `bra` at 0xFF0008 (US security block length)
    }

    private void setupSub(String subcodePath) throws Exception {
        ram("bios_sub", 0x000000, 0x6000);
        long spEnd = 0x6000 + mem.getBlock("sp").getSize();
        ram("prg_ram_lo", spEnd, 0xD400 - spEnd);
        File f = new File(subcodePath);
        try (FileInputStream in = new FileInputStream(f)) {
            MemoryBlock b = mem.createInitializedBlock("subcode", a(0xD400), in, f.length(), monitor, false);
            b.setRead(true);
            b.setWrite(true);
            b.setExecute(true);
        }
        long subEnd = 0xD400 + f.length();
        ram("prg_ram_hi", subEnd, 0x80000 - subEnd);
        ram("word_ram", 0x080000, 0x40000);
        io("pcm", 0xFF0000, 0x4000);
        io("gate_array", 0xFF8000, 0x200);
        mirror("gate_array_absw", 0xFFFF8000L, 0xFF8000, 0x200);

        // Sub BIOS jump points (Sega CD BIOS manual)
        label(0x5F16, "_BURAM");
        label(0x5F22, "_CDBIOS");
        label(0x5F28, "_USERCALL0");
        label(0x5F2E, "_USERCALL1");
        label(0x5F34, "_USERCALL2");
        label(0x5F3A, "_USERCALL3");

        String[][] ga = {
            {"0", "GA_RESET"}, {"2", "GA_MEMMODE"}, {"4", "GA_CDC_MODE"}, {"5", "GA_CDC_RS0"},
            {"7", "GA_CDC_RS1"}, {"8", "GA_CDC_HOSTDATA"}, {"A", "GA_CDC_DMAADDR"},
            {"C", "GA_STOPWATCH"}, {"E", "GA_COMFLAGS_MAIN"}, {"F", "GA_COMFLAGS_SUB"},
            {"30", "GA_TIMER"}, {"32", "GA_INTMASK"}, {"34", "GA_CD_FADER"},
            {"36", "GA_CDD_CTRL"}, {"38", "GA_CDD_STATUS"}, {"42", "GA_CDD_CMD"},
            {"4C", "GA_FONT_COLOR"}, {"4E", "GA_FONT_BITS"}, {"58", "GA_STAMP_SIZE"},
            {"5A", "GA_STAMP_MAP"}, {"5C", "GA_IMG_VCELL"}, {"5E", "GA_IMG_START"},
            {"60", "GA_IMG_OFFSET"}, {"62", "GA_IMG_HDOT"}, {"64", "GA_IMG_VDOT"},
            {"66", "GA_TRACE_VECTOR"},
        };
        for (String[] r : ga) {
            label(0xFF8000 + Long.parseLong(r[0], 16), r[1]);
        }
        for (int i = 0; i < 8; i++) {
            label(0xFF8010 + i * 2, "GA_COMCMD" + i);
            label(0xFF8020 + i * 2, "GA_COMSTAT" + i);
        }
        label(0xFF0001, "PCM_ENV");
        label(0xFF0003, "PCM_PAN");
        label(0xFF0005, "PCM_FDL");
        label(0xFF0007, "PCM_FDH");
        label(0xFF0009, "PCM_LSL");
        label(0xFF000B, "PCM_LSH");
        label(0xFF000D, "PCM_ST");
        label(0xFF000F, "PCM_CTRL");
        label(0xFF0011, "PCM_ONOFF");
        label(0xFF2001, "PCM_WAVE_RAM");

        // SP module "MAIN A014": header at 0x6000, jump table at 0x6020 (offsets rel. to table)
        String[] spNames = {"sp_init", "sp_main", "sp_int2", "sp_user"};
        for (int i = 0; i < spNames.length; i++) {
            short off = getShort(a(0x6020 + i * 2));
            if (off != 0) {
                entry(0x6020 + off, spNames[i]);
            }
        }
        // SUBCODE.BIN: 2x NOP then a JMP vector table
        entry(0xD400, "subcode_start");
        long p = 0xD404;
        int n = 0;
        while (getShort(a(p)) == (short) 0x4EF9) {
            disassemble(a(p));
            long target = getInt(a(p + 2)) & 0xFFFFFFFFL;
            entry(target, String.format("subcode_vec%02d", n));
            label(p, String.format("subcode_jt%02d", n));
            p += 6;
            n++;
        }
        println("subcode vectors: " + n);
    }
}
