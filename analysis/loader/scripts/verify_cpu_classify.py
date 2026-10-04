"""verify_cpu_classify.py - shared instruction classifier for verify_cpu_priv2.py / verify_cpu_rdgap.py."""

PRIV = {"in", "out", "insb", "insw", "insd", "outsb", "outsw", "outsd", "cli", "sti", "hlt", "lidt", "lgdt",
        "lldt", "ltr", "lmsw", "clts", "invd", "wbinvd", "invlpg", "rdmsr", "wrmsr", "rdpmc", "sysenter",
        "sysexit", "syscall", "sysret", "iret", "iretd", "int", "into", "int1", "icebp", "ud2", "bound", "arpl",
        "sidt", "sgdt", "sldt", "str", "smsw", "lar", "lsl", "verr", "verw", "lds", "les", "lss", "lfs", "lgs",
        "pushfd", "popfd", "pushf", "popf", "rdtsc", "rdtscp", "cpuid", "cmpxchg", "cmpxchg8b", "xadd",
        "fxsave", "fxrstor", "ldmxcsr", "stmxcsr", "pushal", "popal", "pusha", "popa"}
SREG = {"cs", "ds", "es", "ss", "fs", "gs"}


def special(i):
    m = i.mnemonic
    if m in PRIV:
        return m if m != "int" else "int " + i.op_str
    if m.startswith("lock") or (i.bytes[0] == 0xF0):
        return "LOCK " + m
    if m in ("push", "pop") and i.op_str in SREG:
        return m + " " + i.op_str
    if m == "mov" and (i.op_str.split(",")[0].strip() in SREG or i.op_str.split(",")[-1].strip() in SREG):
        return "mov sreg"
    if m == "mov" and ("cr" in i.op_str.split(",")[0] or i.op_str.startswith("dr") or ", cr" in i.op_str or ", dr" in i.op_str):
        if any(r in i.op_str for r in ("cr0", "cr2", "cr3", "cr4", "dr0", "dr1", "dr2", "dr3", "dr6", "dr7")):
            return "mov crN/drN"
    if m == "xchg" and "ptr" in i.op_str:
        return "xchg mem"
    return None



