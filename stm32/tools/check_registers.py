#!/usr/bin/env python3
"""Checks the safety MCU's hand-written register map against ST's SVD for the STM32G030.

Usage: python3 -I stm32/tools/check_registers.py --svd PATH/STM32G030.svd [--st-dir DIR] [-q]

It reads hal/stm32g030.h, every hal/*.c, hal/startup.c and the linker script, and checks against
the SVD: each peripheral base, each register macro's offset and name, each named bit or field
macro in the header (PERIPH_REG_FIELD, with optional _Pos/_Msk suffixes, or a field value such as
RCC_CFGR_SW_PLL), the IRQ numbers and the vector table, every bit constant written inline in the
driver (each one must have an entry in INLINE_CLAIMS below, saying which field it means), every
constant written whole into a register (no reserved bits), and the flash and RAM layout. The SVD
is ST's (Apache-2.0); get STM32G030.svd from ST's STM32G0 SVD package (st.com, "STM32G0 System
View Description") or the modm-io/cmsis-svd-stm32 or cmsis-svd/cmsis-svd-data collections on
GitHub. --st-dir optionally names a directory with ST's own headers, for the facts the SVD does
not hold and as a second opinion on the SVD (which covers the whole G0 family, so it lists a few
bits the G030 lacks): stm32g030xx.h (STMicroelectronics/cmsis-device-g0, Include/),
stm32g0xx_hal_flash.h, stm32g0xx_ll_dmamux.h and stm32g0xx_ll_adc.h
(STMicroelectronics/stm32g0xx-hal-driver, Inc/). Without them those checks print UNVERIFIED. The
files are only parsed as text, never run; the SHA-256 of each is printed beside the one this
checker was written against. Prints PASS / FAIL / UNVERIFIED / NOTE lines and exits 1 on any FAIL.
Standard library only.
"""

import argparse
import ast
import hashlib
import os
import re
import sys
import xml.etree.ElementTree as ET

# The files this checker was written and last run against (a different file only warns).
PINNED = {
    "STM32G030.svd": "034a3532e10e63b6c085ca9357050ea8b25f3d796ed4b1699cebf8a1d6742880",  # v1.6
    "stm32g030xx.h": "9af2b7c64ebe6cbe486f8a5672c7e6f2f08c23cd5d24962c138205b1a49b833b",
    "stm32g0xx_hal_flash.h": "b574ae06af84331eff7413d73f2326d5e4a1928277d9145ce52238312017e4a2",
    "stm32g0xx_ll_dmamux.h": "057659089a9dcc0923218871ede3b0d80520ae719aa3dcbf87c3d99119887ef1",
    "stm32g0xx_ll_adc.h": "d1760e7738f6759788e7af09ac8e5ea71ced97d81533e4f5330e659d88db1a85",
}

# Register names where the SVD differs from the reference manual (SVD name -> RM name).
REG_ALIASES = {("RCC", "PLLSYSCFGR"): "PLLCFGR"}
# Field names the SVD misspells (SVD name -> RM name).
FIELD_ALIASES = {("FLASH", "OPTR", "IDWG_SW"): "IWDG_SW"}
# Header base macros whose CMSIS name is not NAME_BASE.
CMSIS_BASE = {"ADC": "ADC1_BASE", "FLASH": "FLASH_R_BASE", "DMAMUX": "DMAMUX1_BASE"}
# The STM32G030K8: 64 KB of flash (the "8" in the part number; datasheet DS12991).
FLASH_SIZE_K8 = 64 * 1024
# RM0454 (option bytes); no ST header carries it, so it stays UNVERIFIED by this tool.
OPTION_BYTES = 0x1FFF7800

# Cortex-M0+ system registers (ARMv6-M Architecture Reference Manual): not in ST's SVD.
CORE = {
    0xE000E010: ("SYST", "CSR", {"ENABLE": (0, 1), "TICKINT": (1, 1), "CLKSOURCE": (2, 1),
                                 "COUNTFLAG": (16, 1)}),
    0xE000E014: ("SYST", "RVR", {"RELOAD": (0, 24)}),
    0xE000E018: ("SYST", "CVR", {"CURRENT": (0, 24)}),
    0xE000E01C: ("SYST", "CALIB", {"TENMS": (0, 24), "SKEW": (30, 1), "NOREF": (31, 1)}),
    0xE000E100: ("NVIC", "ISER", {"SETENA": (0, 32)}),
    0xE000E180: ("NVIC", "ICER", {"CLRENA": (0, 32)}),
    0xE000E200: ("NVIC", "ISPR", {"SETPEND": (0, 32)}),
    0xE000E280: ("NVIC", "ICPR", {"CLRPEND": (0, 32)}),
}
CORE_EXCEPTIONS = {2: "NMI", 3: "HardFault", 11: "SVCall", 14: "PendSV", 15: "SysTick"}

# What each bit constant written inline in hal/*.c means: (register macro, the constant as the
# code writes it, the field(s), how). Fields: one name, several joined by "|", or NAME* for the
# per-pin fields (NAME0..NAME15) a shift by the pin number reaches. How: "bit" (a shift places
# the value in the field), "mask" (the constant is exactly the fields' mask), "value" (the
# constant is a value of the field, placed or compared elsewhere). "x" stands for any variable
# and "n" for the pin number. A new inline constant without an entry here FAILs as UNCHECKED:
# add its entry, or give it a named macro in stm32g030.h (checked by name).
INLINE_CLAIMS = [
    # clocks
    ("FLASH_ACR", "7u", "LATENCY", "mask"),
    ("FLASH_ACR", "2u", "LATENCY", "value"),
    ("FLASH_ACR", "1u << 8", "PRFTEN", "bit"),
    ("FLASH_ACR", "1u << 9", "ICEN", "bit"),
    ("RCC_PLLCFGR", "2u", "PLLSRC", "value"),
    ("RCC_PLLCFGR", "0u << 4", "PLLM", "bit"),
    ("RCC_PLLCFGR", "8u << 8", "PLLN", "bit"),
    ("RCC_PLLCFGR", "1u << 28", "PLLREN", "bit"),
    ("RCC_PLLCFGR", "1u << 29", "PLLR", "bit"),
    ("RCC_CR", "1u << 24", "PLLON", "bit"),
    ("RCC_CR", "1u << 25", "PLLRDY", "bit"),
    ("RCC_CFGR", "7u", "SW", "mask"),
    ("RCC_CFGR", "2u", "SW|SWS", "value"),
    ("RCC_CFGR", "(x >> 3) & 7u", "SWS", "bit"),
    ("SYST_CSR", "5u", "ENABLE|CLKSOURCE", "mask"),
    ("SYST_CSR", "1u << 16", "COUNTFLAG", "bit"),
    ("SYST_CVR", "0xFFFFFFu", "CURRENT", "mask"),
    # reset reason and option bytes
    ("RCC_CSR", "1u << 23", "RMVF", "bit"),
    ("RCC_CSR", "1u << 29", "IWDGRSTF", "bit"),
    ("RCC_CSR", "1u << 28", "SFTRSTF", "bit"),
    ("RCC_CSR", "1u << 27", "PWRRSTF", "bit"),
    ("RCC_CSR", "1u << 26", "PINRSTF", "bit"),
    ("FLASH_OPTR", "(x >> 16) & 1u", "IWDG_SW", "bit"),
    ("FLASH_OPTR", "(x >> 24) & 1u", "nBOOT_SEL", "bit"),
    ("FLASH_SR", "(x >> 15) & 1u", "OPTVERR", "bit"),
    # watchdog
    ("IWDG_SR", "7u", "PVU|RVU|WVU", "mask"),
    # pins
    ("GPIO_MODER", "3u << (2 * n)", "MODER*", "mask"),
    ("GPIO_MODER", "x << (2 * n)", "MODER*", "bit"),
    ("GPIO_AFRL", "15u << (4 * n)", "AFSEL*", "mask"),
    ("GPIO_AFRL", "x << (4 * n)", "AFSEL*", "bit"),
    ("GPIO_AFRH", "15u << (4 * (n - 8))", "AFSEL*", "mask"),
    ("GPIO_AFRH", "x << (4 * (n - 8))", "AFSEL*", "bit"),
    ("GPIO_PUPDR", "3u << (2 * n)", "PUPDR*", "mask"),
    ("GPIO_PUPDR", "1u << (2 * n)", "PUPDR*", "bit"),
    ("GPIO_BSRR", "1u << n", "BS*", "bit"),
    ("GPIO_BSRR", "1u << (n + 16)", "BR*", "bit"),
    ("GPIO_IDR", "(x >> n) & 1u", "IDR*", "bit"),
    ("GPIO_LCKR", "1u << 16", "LCKK", "bit"),
    ("GPIO_LCKR", "1u << 14", "LCK14", "bit"),
    # timers
    ("TIM_CCMR1", "6u << 4", "OC1M", "bit"),
    ("TIM_CCMR1", "1u << 3", "OC1PE", "bit"),
    ("TIM_CCMR1", "6u << 12", "OC2M", "bit"),
    ("TIM_CCMR1", "1u << 11", "OC2PE", "bit"),
    ("TIM_CCMR1", "7u << 4", "OC1M", "mask"),
    ("TIM_CCMR1", "x << 4", "OC1M", "bit"),
    ("TIM_CCMR1", "5u << 4", "OC1M", "bit"),
    ("TIM_CCMR1", "4u << 4", "OC1M", "bit"),
    ("TIM_CCMR1", "1u", "CC1S", "value"),
    ("TIM_CCER", "1u << 0", "CC1E", "bit"),
    ("TIM_CCER", "1u << 4", "CC2E", "bit"),
    ("TIM_CCER", "1u", "CC1E", "mask"),
    ("TIM_BDTR", "1u << 15", "MOE", "bit"),
    ("TIM_DIER", "1u << 8", "UDE", "bit"),
    ("TIM_DIER", "1u << 2", "CC2IE", "bit"),
    ("TIM_EGR", "1", "UG", "mask"),
    ("TIM_CR1", "1u", "CEN", "mask"),
    ("TIM_CR1", "1u << 7", "ARPE", "bit"),
    ("TIM_SR", "1u << 2", "CC2IF", "bit"),
    ("TIM_SR", "1u << 1", "CC1IF", "bit"),
    ("TIM_CCR1", "0xFFFFu", "CCR1", "mask"),
    ("DMA_CPAR1", "TIM1 + 0x34u", "CCR1", "address"),
    ("NVIC_ISER", "1u << *", "SETENA", "bit"),
    # DMA
    ("DMA_CCR1", "1u << 4", "DIR", "bit"),
    ("DMA_CCR1", "1u << 7", "MINC", "bit"),
    ("DMA_CCR1", "1u << 8", "PSIZE", "bit"),
    ("DMA_CCR1", "1u << 10", "MSIZE", "bit"),
    ("DMA_CCR1", "1u", "EN", "mask"),
    # ADC
    ("ADC_CR", "1u << 28", "ADVREGEN", "bit"),
    ("ADC_CR", "1u << 31", "ADCAL", "bit"),
    ("ADC_CR", "1u", "ADEN", "mask"),
    ("ADC_CR", "1u << 2", "ADSTART", "bit"),
    ("ADC_CFGR2", "2u << 30", "CKMODE", "bit"),
    ("ADC_SMPR", "4u", "SMP1", "value"),
    ("ADC_CCR", "1u << 22", "VREFEN", "bit"),
    ("ADC_ISR", "1u", "ADRDY", "mask"),
    ("ADC_ISR", "1u << 2", "EOC", "bit"),
    ("ADC_ISR", "1u << 3", "EOS", "bit"),
    # USART2
    ("USART_ISR", "1u << 3", "ORE", "bit"),
    ("USART_ISR", "1u << 5", "RXNE", "bit"),
    ("USART_ISR", "1u << 7", "TXE", "bit"),
    ("USART_ICR", "1u << 3", "ORECF", "bit"),
    ("USART_CR1", "1u << 7", "TXEIE", "bit"),
    ("USART_CR1", "1u << 5", "RXNEIE", "bit"),
    ("USART_CR1", "1u << 3", "TE", "bit"),
    ("USART_CR1", "1u << 2", "RE", "bit"),
    ("USART_CR1", "1u", "UE", "mask"),
    # flash programming
    ("FLASH_SR", "1u << 16", "BSY1", "bit"),
    ("FLASH_SR", "0x83FBu", "EOP|OPERR|PROGERR|WRPERR|PGAERR|SIZERR|PGSERR|MISERR|FASTERR|OPTVERR",
     "mask"),
    ("FLASH_SR", "0x83FAu", "OPERR|PROGERR|WRPERR|PGAERR|SIZERR|PGSERR|MISERR|FASTERR|OPTVERR",
     "mask"),
    ("FLASH_CR", "1u << 1", "PER", "bit"),
    ("FLASH_CR", "x << 3", "PNB", "bit"),
    ("FLASH_CR", "1u << 16", "STRT", "bit"),
    ("FLASH_CR", "1u", "PG", "mask"),
    ("FLASH_CR", "1u << 31", "LOCK", "bit"),
    # clock enables
    ("RCC_IOPENR", "7u", "GPIOAEN|GPIOBEN|GPIOCEN", "mask"),
    ("RCC_AHBENR", "1u", "DMA1EN", "mask"),
    ("RCC_APBENR1", "1u << 1", "TIM3EN", "bit"),
    ("RCC_APBENR1", "1u << 17", "USART2EN", "bit"),
    ("RCC_APBENR2", "1u", "SYSCFGEN", "mask"),
    ("RCC_APBENR2", "1u << 11", "TIM1EN", "bit"),
    ("RCC_APBENR2", "1u << 17", "TIM16EN", "bit"),
    ("RCC_APBENR2", "1u << 18", "TIM17EN", "bit"),
    ("RCC_APBENR2", "1u << 20", "ADCEN", "bit"),
]

# ------------------------------------------------------------------------------------ reporting

counts = {"PASS": 0, "FAIL": 0, "UNVERIFIED": 0, "NOTE": 0}
quiet = False


def report(status, msg):
    counts[status] += 1
    if quiet and status == "PASS":
        return
    print(f"{status:<10} {msg}")


# ------------------------------------------------------------------------------- C expressions

SUFFIX = re.compile(r"\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b")
CAST = re.compile(r"\(\s*(?:const\s+)?(?:volatile\s+)?(?:u?int\d+_t|unsigned|int|size_t)\s*\*?\s*\)")


class NotConst(Exception):
    pass


def ceval(text, names, free=None):
    """Evaluates a C integer expression. names: name -> value or callable; free: name -> value."""
    src = CAST.sub(" ", SUFFIX.sub(r"\1", text)).strip()
    src = src.replace("/", "//")
    try:
        tree = ast.parse(src, mode="eval")
    except SyntaxError as e:
        raise NotConst(text) from e

    def ev(n):
        if isinstance(n, ast.Expression):
            return ev(n.body)
        if isinstance(n, ast.Constant) and isinstance(n.value, int):
            return n.value
        if isinstance(n, ast.Name):
            if free and n.id in free:
                return free[n.id]
            v = names(n.id) if callable(names) else names.get(n.id)
            if v is None:
                raise NotConst(n.id)
            return v
        if isinstance(n, ast.UnaryOp):
            v = ev(n.operand)
            if isinstance(n.op, ast.Invert):
                return ~v & 0xFFFFFFFF
            if isinstance(n.op, ast.USub):
                return -v
            if isinstance(n.op, ast.UAdd):
                return v
        if isinstance(n, ast.BinOp):
            a, b = ev(n.left), ev(n.right)
            ops = {ast.Add: lambda: a + b, ast.Sub: lambda: a - b, ast.Mult: lambda: a * b,
                   ast.FloorDiv: lambda: a // b, ast.Mod: lambda: a % b,
                   ast.LShift: lambda: (a << b) & 0xFFFFFFFF, ast.RShift: lambda: a >> b,
                   ast.BitAnd: lambda: a & b, ast.BitOr: lambda: a | b, ast.BitXor: lambda: a ^ b}
            for k, f in ops.items():
                if isinstance(n.op, k):
                    return f()
        raise NotConst(text)

    return ev(tree)


def strip_c(text):
    """Comments and strings out (newlines kept, for line numbers)."""
    out = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    out = re.sub(r"//[^\n]*", "", out)
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', out)


def parse_defines(path):
    """#defines of a C file: name -> (params or None, body, line)."""
    text = strip_c(open(path, encoding="utf-8", errors="replace").read())
    defs = {}
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        line, start = lines[i], i + 1
        while line.endswith("\\") and i + 1 < len(lines):
            i += 1
            line = line[:-1] + " " + lines[i]
        m = re.match(r"\s*#\s*define\s+(\w+)(\(([^)]*)\))?\s*(.*)$", line)
        if m:
            params = [p.strip() for p in m.group(3).split(",")] if m.group(2) else None
            defs[m.group(1)] = (params, m.group(4).strip(), start)
        i += 1
    return defs


class Names:
    """Lazy constant lookup over #defines."""

    def __init__(self, *defsets):
        self.defs = {}
        for d in defsets:
            self.defs.update(d)
        self.cache = {}

    def __call__(self, name):
        if name in self.cache:
            return self.cache[name]
        d = self.defs.get(name)
        if d is None or d[0] is not None:
            return None
        self.cache[name] = None  # no recursion loops
        try:
            v = ceval(d[1], self)
        except (NotConst, ZeroDivisionError):
            v = None
        self.cache[name] = v
        return v


# ----------------------------------------------------------------------------------------- SVD

class Svd:
    def __init__(self, path):
        raw = open(path, "rb").read()
        if b"<!DOCTYPE" in raw or b"<!ENTITY" in raw:
            sys.exit(f"{path}: has a DOCTYPE or ENTITY declaration; refusing to parse it")
        root = ET.fromstring(raw)
        self.name = root.findtext("name")
        self.version = root.findtext("version")
        self.periphs = {}
        self.irqs = {}
        elems = {p.findtext("name"): p for p in root.find("peripherals")}
        for name, p in elems.items():
            src = p
            if p.find("registers") is None and p.get("derivedFrom"):
                src = elems[p.get("derivedFrom")]
            group = (p.findtext("groupName") or src.findtext("groupName") or name).upper()
            regs = []
            for r in src.find("registers") if src.find("registers") is not None else []:
                if r.tag != "register":
                    continue
                if r.find("dim") is not None:
                    report("NOTE", f"SVD {name}.{r.findtext('name')}: dim arrays not expanded")
                fields = {}
                for f in r.find("fields") if r.find("fields") is not None else []:
                    off, width = field_range(f)
                    fname = f.findtext("name")
                    fname = FIELD_ALIASES.get((name_root(name), norm_reg(r.findtext("name"), group, name), fname), fname)
                    fields[fname] = (off, width)
                regs.append({"name": r.findtext("name"), "norm": norm_reg(r.findtext("name"), group, name),
                             "offset": int(r.findtext("addressOffset"), 0), "fields": fields})
            self.periphs[name] = {"base": int(p.findtext("baseAddress"), 0), "group": group,
                                  "regs": regs}
            for irq in p.findall("interrupt"):
                self.irqs[irq.findtext("name")] = int(irq.findtext("value"), 0)

    def regs_at(self, periph, offset):
        return [r for r in self.periphs[periph]["regs"] if r["offset"] == offset]

    def find_periph(self, addr):
        best = None
        for n, p in self.periphs.items():
            if p["base"] <= addr < p["base"] + 0x400 and (best is None or p["base"] > self.periphs[best]["base"]):
                best = n
        return best


def name_root(periph):
    return re.sub(r"\d+$", "", periph)


def field_range(f):
    if f.find("bitOffset") is not None:
        return int(f.findtext("bitOffset"), 0), int(f.findtext("bitWidth") or "1", 0)
    if f.find("lsb") is not None:
        lsb, msb = int(f.findtext("lsb"), 0), int(f.findtext("msb"), 0)
        return lsb, msb - lsb + 1
    m = re.match(r"\[(\d+):(\d+)\]", f.findtext("bitRange"))
    return int(m.group(2)), int(m.group(1)) - int(m.group(2)) + 1


def norm_reg(name, group, periph):
    n = name.upper()
    for pre in (group + "_", name_root(periph).upper() + "_"):
        if n.startswith(pre):
            n = n[len(pre):]
            break
    n = re.sub(r"_(OUTPUT|INPUT|\d+)$", "", n)
    return REG_ALIASES.get((name_root(periph), n), n)


def merged_fields(regs):
    out = {}
    for r in regs:
        for k, v in r["fields"].items():
            out.setdefault(k, v)
    return out


def defined_mask(fields):
    m = 0
    for off, w in fields.values():
        m |= ((1 << w) - 1) << off
    return m


# ---------------------------------------------------------------------------- ST's own headers

class St:
    def __init__(self, d):
        self.defs = {}
        self.guards = {}
        self.files = []
        if not d:
            return
        for fn in ("stm32g030xx.h", "stm32g0xx_hal_flash.h", "stm32g0xx_ll_dmamux.h", "stm32g0xx_ll_adc.h"):
            p = os.path.join(d, fn)
            if os.path.isfile(p):
                self.files.append(p)
                self.defs[fn] = parse_defines(p)
        p = os.path.join(d, "stm32g0xx_ll_dmamux.h")
        if os.path.isfile(p):
            guard = None
            for line in strip_c(open(p, encoding="utf-8", errors="replace").read()).split("\n"):
                m = re.match(r"\s*#\s*if\s+defined\s*\(\s*(\w+)\s*\)", line)
                if m:
                    guard = m.group(1)
                elif re.match(r"\s*#\s*endif", line):
                    guard = None
                m = re.match(r"\s*#\s*define\s+(LL_DMAMUX_REQ_\w+)", line)
                if m:
                    self.guards[m.group(1)] = guard
        self.cmsis = Names(self.defs.get("stm32g030xx.h", {}))
        self.all = Names(*self.defs.values())

    def has(self, fn):
        return fn in self.defs

    def value(self, name):
        if not self.defs:
            return None
        v = self.all(name)
        if v is None:  # pointer-cast addresses, e.g. ((uint16_t*) (0x1FFF75AAUL))
            for d in self.defs.values():
                if name in d and d[name][0] is None:
                    m = re.search(r"0[xX][0-9a-fA-F]+", d[name][1])
                    if m:
                        return int(m.group(0), 16)
        return v

    def cmsis_field(self, reg_macro, field):
        """(offset, width) of the field's run from its _Pos in stm32g030xx.h, or None. (A split
        field such as TIM_CCMR1_OC1M, bits 6:4 and 16, gives its low run.)"""
        if "stm32g030xx.h" not in self.defs:
            return None
        for r in (reg_macro, re.sub(r"\d+$", "", reg_macro)):
            pos, msk = self.cmsis(f"{r}_{field}_Pos"), self.cmsis(f"{r}_{field}_Msk")
            if pos is not None and msk is not None:
                run = msk >> pos
                return pos, (run & ~(run + 1)).bit_length()
        return None


def cmsis_vs_svd(st, reg, f, off, w, label):
    """(width to check against, [(ok, text)]): ST's CMSIS header is G030-specific, the SVD covers
    the whole G0 family; a width they disagree on is a NOTE, and the narrower one is used."""
    cf = st.cmsis_field(reg, f)
    if not cf or cf == (off, w):
        return w, []
    if cf[0] != off:
        return w, [(False, f"ST's CMSIS {reg}_{f} is at bit {cf[0]}, the SVD says bit {off}")]
    report("NOTE", f"{label}.{f}: the SVD says {w} bits, ST's CMSIS {reg}_{f} {cf[1]}; "
                   f"checked against {min(w, cf[1])}")
    return min(w, cf[1]), []


# ----------------------------------------------------------------------------- the register map

class Map:
    """The header's bases and register macros, resolved to SVD registers."""

    def __init__(self, svd, hdr, names):
        self.svd, self.hdr, self.names = svd, hdr, names
        self.bases = {}  # macro -> address, for macros naming a peripheral
        self.regs = {}  # macro -> {"param": bool, "offset", "fixed": (periph, addr) | None}
        for name, (params, body, line) in hdr.items():
            if params is None and name in svd.periphs:
                v = names(name)
                if v is not None:
                    self.bases[name] = v
        for name, (params, body, line) in hdr.items():
            m = re.fullmatch(r"REG\s*\((.*)\)", body)
            if not m:
                continue
            expr = m.group(1)
            if params:
                free = {p: 0 for p in params}
                try:
                    off = ceval(expr, names, free)
                except NotConst:
                    report("FAIL", f"stm32g030.h:{line} {name}: cannot evaluate {expr}")
                    continue
                self.regs[name] = {"param": True, "offset": off, "fixed": None, "line": line}
            else:
                try:
                    addr = ceval(expr, names)
                except NotConst:
                    report("FAIL", f"stm32g030.h:{line} {name}: cannot evaluate {expr}")
                    continue
                syms = [t for t in re.findall(r"[A-Za-z_]\w*", expr) if t in self.bases]
                if addr in CORE:
                    periph = "CORE"
                elif syms:
                    periph = syms[0]
                else:
                    periph = svd.find_periph(addr)
                pbase = self.bases.get(periph, svd.periphs[periph]["base"] if periph in svd.periphs else 0)
                self.regs[name] = {"param": False, "offset": addr - pbase,
                                   "fixed": (periph, addr), "line": line}

    def family(self, macro):
        pre = macro.split("_")[0]
        return [(b, a) for b, a in sorted(self.bases.items()) if b.startswith(pre)]

    def instances(self, macro, inst=None):
        """[(periph, address)] the macro reaches: one, or every family member for a parameter."""
        r = self.regs[macro]
        if r["fixed"]:
            return [r["fixed"]]
        if inst:
            if inst in self.bases:
                return [(inst, self.bases[inst] + r["offset"])]
            return []
        return [(b, a + r["offset"]) for b, a in self.family(macro)]

    def fields(self, periph, addr):
        """(register label, fields) at an address, or (None, None)."""
        if periph == "CORE":
            c = CORE.get(addr)
            return (f"{c[0]}_{c[1]}", c[2]) if c else (None, None)
        if periph not in self.svd.periphs:
            return None, None
        regs = self.svd.regs_at(periph, addr - self.svd.periphs[periph]["base"])
        if not regs:
            return None, None
        return f"{periph}.{regs[0]['norm']}", merged_fields(regs)


def check_header(svd, m, hdr, names, st):
    print("-- peripheral bases")
    for name, addr in sorted(m.bases.items()):
        want = svd.periphs[name]["base"]
        extra = ""
        cn = CMSIS_BASE.get(name, name + "_BASE")
        cv = st.cmsis(cn) if st.has("stm32g030xx.h") else None
        if cv is not None:
            if cv != addr:
                report("FAIL", f"{name} = {addr:#010x}, ST's CMSIS {cn} = {cv:#010x}")
            extra = f" [CMSIS {cn} agrees]" if cv == addr else ""
        if addr == want:
            report("PASS", f"{name} = {addr:#010x}{extra}")
        else:
            report("FAIL", f"{name} = {addr:#010x}, SVD says {want:#010x}")

    print("-- register macros")
    for macro, r in sorted(m.regs.items(), key=lambda kv: kv[1]["line"]):
        regname = macro.split("_", 1)[1] if "_" in macro else macro
        insts = m.instances(macro)
        if r["fixed"] and r["fixed"][0] == "CORE":
            c = CORE.get(r["fixed"][1])
            if c and f"{c[0]}_{c[1]}" == macro:
                report("PASS", f"{macro} at {r['fixed'][1]:#010x} (ARMv6-M, built-in table)")
            else:
                report("FAIL", f"{macro} at {r['fixed'][1]:#010x}: the ARMv6-M map has "
                               f"{c[0] + '_' + c[1] if c else 'nothing'} there")
            continue
        found, wrong = [], []
        for periph, addr in insts:
            if periph not in svd.periphs:
                wrong.append(f"{periph}: no such peripheral in the SVD")
                continue
            off = addr - svd.periphs[periph]["base"]
            regs = svd.regs_at(periph, off)
            if any(x["norm"] == regname for x in regs):
                found.append(periph)
                continue
            elsewhere = [x for x in svd.periphs[periph]["regs"] if x["norm"] == regname]
            if regs:
                wrong.append(f"{periph}+{off:#x} is {regs[0]['name']}")
            elif elsewhere:
                wrong.append(f"{periph}+{off:#x} is reserved; {regname} is at "
                             f"{periph}+{elsewhere[0]['offset']:#x}")
            elif r["param"]:
                pass  # this family member lacks the register: only its uses are checked
            else:
                wrong.append(f"{periph}+{off:#x} is reserved and {periph} has no {regname}")
        where = f"stm32g030.h:{r['line']} {macro} (+{r['offset']:#x})"
        if wrong or not found:
            report("FAIL", f"{where}: " + "; ".join(wrong or ["no SVD register matches"]))
        else:
            report("PASS", f"{where} in {', '.join(found)}")

    print("-- bit and field macros, IRQs, DMAMUX requests, system memory")
    regnames = sorted(m.regs, key=len, reverse=True)
    for name, (params, body, line) in sorted(hdr.items(), key=lambda kv: kv[1][2]):
        if name in m.regs or name in m.bases or name == "REG" or not body:
            continue
        where = f"stm32g030.h:{line} {name}"
        if name.startswith("IRQ_"):
            check_irq(svd, st, where, name[4:], names(name))
            continue
        if name.startswith("DMAMUX_REQ_"):
            check_dmamux(st, where, name[len("DMAMUX_REQ_"):], names(name))
            continue
        mm = re.fullmatch(r"\(\*\s*\(\s*const\s+volatile\s+uint\d+_t\s*\*\s*\)\s*(0[xX][0-9a-fA-F]+)[uU]?\s*\)", body)
        if mm:
            check_sysmem(st, where, name, int(mm.group(1), 16))
            continue
        reg = next((r for r in regnames if name.startswith(r + "_")), None)
        if reg is None:
            report("NOTE", f"{where}: not a register, base, field, IRQ or DMAMUX name; not checked")
            continue
        check_field_macro(m, st, where, reg, name[len(reg) + 1:], params, body, names)


def check_irq(svd, st, where, irq, value):
    want = svd.irqs.get(irq)
    cm = st.cmsis(f"{irq}_IRQn") if st.has("stm32g030xx.h") else None
    if want is None:
        report("FAIL", f"{where} = {value}: no interrupt {irq} in the SVD")
    elif want != value:
        report("FAIL", f"{where} = {value}, SVD says {want}")
    elif cm is not None and cm != value:
        report("FAIL", f"{where} = {value}, ST's CMSIS {irq}_IRQn = {cm}")
    else:
        report("PASS", f"{where} = {value}" + (" [CMSIS agrees]" if cm == value else ""))


def check_dmamux(st, where, req, value):
    ll = st.value(f"LL_DMAMUX_REQ_{req}") if st.has("stm32g0xx_ll_dmamux.h") else None
    if ll is None:
        report("UNVERIFIED", f"{where} = {value}: the DMAMUX request table is in RM0454, not "
                             "the SVD; no ST LL header given")
        return
    guard = st.guards.get(f"LL_DMAMUX_REQ_{req}")
    if guard and st.has("stm32g030xx.h") and guard not in st.defs["stm32g030xx.h"]:
        report("FAIL", f"{where}: ST's LL driver has it only with {guard}, which the G030 lacks")
    elif ll != value:
        report("FAIL", f"{where} = {value}, ST's LL driver says {ll}")
    else:
        report("UNVERIFIED", f"{where} = {value}: matches ST's LL driver (LL_DMAMUX_REQ_{req} = "
                             f"{ll}); RM0454's DMAMUX table itself not machine-read")


SYSMEM = {"VREFINT_CAL": "VREFINT_CAL_ADDR", "TS_CAL1": "TEMPSENSOR_CAL1_ADDR",
          "TS_CAL2": "TEMPSENSOR_CAL2_ADDR"}


def check_sysmem(st, where, name, addr):
    ref = SYSMEM.get(name)
    v = st.value(ref) if ref and st.has("stm32g0xx_ll_adc.h") else None
    if v is None:
        report("UNVERIFIED", f"{where} at {addr:#010x}: system memory, not in the SVD")
    elif v == addr:
        report("PASS", f"{where} at {addr:#010x} [ST's LL {ref}]")
    else:
        report("FAIL", f"{where} at {addr:#010x}, ST's LL {ref} = {v:#010x}")


def find_field(fields, want):
    for k in fields:
        if k.upper() == want.upper():
            return k
    return None


def check_field_macro(m, st, where, reg, rest, params, body, names):
    kind = "mask"
    for suf, k in (("_Pos", "pos"), ("_POS", "pos"), ("_SHIFT", "pos"), ("_Msk", "mask"),
                   ("_MSK", "mask"), ("_MASK", "mask")):
        if rest.endswith(suf):
            rest, kind = rest[: -len(suf)], k
            break
    try:
        if params:
            free0 = {p: 0 for p in params}
            free1 = {p: 1 for p in params}
            v0, v1 = ceval(body, names, free0), ceval(body, names, free1)
            step = v1 - v0
            if step <= 0 or step & (step - 1):
                raise NotConst(body)
            kind, value = "fn", step.bit_length() - 1
        else:
            value = ceval(body, names)
    except NotConst:
        report("FAIL", f"{where}: cannot evaluate {body}")
        return
    results = []
    for periph, addr in m.instances(reg):
        label, fields = m.fields(periph, addr)
        if not fields:
            continue
        f = find_field(fields, rest)
        fval = None
        if f is None and kind == "mask":
            parts = rest.split("_")
            for i in range(len(parts) - 1, 0, -1):
                f = find_field(fields, "_".join(parts[:i]))
                if f:
                    fval = "_".join(parts[i:])
                    break
        if f is None:
            results.append((False, f"{label} has no field {rest}"))
            continue
        off, w = fields[f]
        w, extra = cmsis_vs_svd(st, reg, f, off, w, label)
        results.extend(extra)
        fmask = ((1 << w) - 1) << off
        if kind == "pos" or kind == "fn":
            ok = value == off
            results.append((ok, f"{label}.{f} at bit {off}" + ("" if ok else f", macro says {value}")))
        elif fval is not None:
            ok = value != 0 and value & ~fmask == 0
            results.append((ok, f"value {fval} {value:#x} {'inside' if ok else 'outside'} "
                                f"{label}.{f} (bits {off}..{off + w - 1})"))
        else:
            ok = value == fmask
            results.append((ok, f"{label}.{f} mask {fmask:#x}" + ("" if ok else f", macro says {value:#x}")))
    if not results:
        report("FAIL", f"{where}: {reg} has no SVD register to check against")
    elif all(ok for ok, _ in results):
        report("PASS", f"{where}: " + "; ".join(sorted({t for _, t in results})))
    else:
        report("FAIL", f"{where}: " + "; ".join(sorted({t for ok, t in results if not ok})))


# ----------------------------------------------------------------- inline constants in hal/*.c

TOK = re.compile(r"\s*(0[xX][0-9a-fA-F]+[uUlL]*|\d+[uUlL]*|[A-Za-z_]\w*|<<=|>>=|<<|>>|<=|>=|==|!="
                 r"|&&|\|\||[-+*/%&|^]=|\S)")
ARITH = {"+", "-", "*", "/", "%", "<", ">", "<=", ">=", "?", ":"}


def tokenize(text):
    toks, line, pos = [], 1, 0
    while True:
        m = TOK.match(text, pos)
        if not m:
            break
        line += text.count("\n", pos, m.start(1))
        toks.append((m.group(1), line))
        pos = m.end()
    return toks


def isnum(t):
    return re.fullmatch(r"0[xX][0-9a-fA-F]+[uUlL]*|\d+[uUlL]*", t) is not None


def num(t):
    return int(re.sub(r"[uUlL]+$", "", t), 0)


def group_end(toks, i):
    """Index of the ')' closing the '(' at i."""
    depth = 0
    for j in range(i, len(toks)):
        if toks[j] == "(":
            depth += 1
        elif toks[j] == ")":
            depth -= 1
            if depth == 0:
                return j
    return len(toks) - 1


def group_start(toks, i):
    depth = 0
    for j in range(i, -1, -1):
        if toks[j] == ")":
            depth += 1
        elif toks[j] == "(":
            depth -= 1
            if depth == 0:
                return j
    return 0


def shift_amount(toks, j, names):
    """The right operand of a shift at toks[j]: (value, end index), value an int, ("lin", s, o)
    for a linear function of one variable, or None."""
    t = toks[j] if j < len(toks) else ""
    if isnum(t):
        return num(t), j
    if re.fullmatch(r"[A-Za-z_]\w*", t):
        v = names(t)
        return (v if v is not None else ("lin", 1, 0)), j
    if t == "(":
        e = group_end(toks, j)
        text = " ".join(toks[j:e + 1])
        try:
            return ceval(text, names), e
        except NotConst:
            pass
        free = [x for x in toks[j:e + 1] if re.fullmatch(r"[A-Za-z_]\w*", x) and names(x) is None]
        if len(set(free)) == 1:
            try:
                a = ceval(text, names, {free[0]: 0})
                b = ceval(text, names, {free[0]: 1})
                return ("lin", b - a, a), e
            except NotConst:
                pass
        return None, e
    return None, j


def items(toks, names):
    """The constants of one statement: [(item, token index)]. Items: ("shl", value, amount),
    ("ext", amount, mask), ("lit", value), ("arith", value), ("addr", base, offset)."""
    out, used = [], set()
    for j, t in enumerate(toks):
        if t == "<<":
            left = None
            p = toks[j - 1] if j else ""
            if isnum(p):
                left = num(p)
                used.add(j - 1)
            elif p == ")":
                s = group_start(toks, j - 1)
                try:
                    left = ceval(" ".join(toks[s:j]), names)
                    used.update(range(s, j))
                except NotConst:
                    left = None
                    # (cond ? 5u : 4u) << k: each choice is a value placed at k
                    amt, _ = shift_amount(toks, j + 1, names)
                    for q in range(s + 1, j - 1):
                        if isnum(toks[q]) and toks[q - 1] in ("?", ":") and toks[q + 1] in (":", ")"):
                            used.add(q)
                            out.append((("shl", num(toks[q]), amt), q))
                    used.update(q for q in range(s, j) if isnum(toks[q]) and q not in used
                                and (toks[q - 1] in ARITH or toks[q + 1] in ARITH))
            elif re.fullmatch(r"[A-Za-z_]\w*", p):
                left = names(p)
            amt, e = shift_amount(toks, j + 1, names)
            used.update(range(j + 1, e + 1))
            out.append((("shl", left, amt), j))
        elif t == ">>":
            amt, e = shift_amount(toks, j + 1, names)
            used.update(range(j + 1, e + 1))
            k = e + 1
            while k < len(toks) and toks[k] == ")":
                k += 1
            if k + 1 < len(toks) and toks[k] == "&" and isnum(toks[k + 1]):
                used.add(k + 1)
                out.append((("ext", amt, num(toks[k + 1])), j))
            else:
                out.append((("shr", amt), j))
    for j, t in enumerate(toks):
        if j in used or not isnum(t):
            continue
        prev = toks[j - 1] if j else ""
        nxt = toks[j + 1] if j + 1 < len(toks) else ""
        if prev == "+" and j >= 2 and re.fullmatch(r"[A-Za-z_]\w*", toks[j - 2]):
            out.append((("addr", toks[j - 2], num(t)), j))
        elif prev in ARITH or nxt in ARITH:
            out.append((("arith", num(t)), j))
        else:
            out.append((("lit", num(t)), j))
    return out


def sig_text(item):
    def amt(a):
        if a is None:
            return "?"
        if isinstance(a, tuple):
            s = "n" if a[1] == 1 else f"{a[1]}n"
            return s + (f"+{a[2]}" if a[2] > 0 else f"{a[2]}" if a[2] < 0 else "")
        return str(a)

    k = item[0]
    if k == "shl":
        return f"{'x' if item[1] is None else hex(item[1]) if item[1] > 9 else item[1]} << {amt(item[2])}"
    if k == "ext":
        return f"(x >> {amt(item[1])}) & {item[2]:#x}"
    if k == "shr":
        return f"x >> {amt(item[1])}"
    if k == "addr":
        return f"{item[1]} + {item[2]:#x}"
    return f"{item[1]:#x}" if item[1] > 9 else str(item[1])


def statements(toks):
    """Splits on ; { }, top-level commas and after if/while conditions: [(tokens, lines)]."""
    plain = [t for t, _ in toks]
    cut = set()
    for i, t in enumerate(plain):
        if t in ("if", "while") and i + 1 < len(plain) and plain[i + 1] == "(":
            cut.add(group_end(plain, i + 1))
    out, cur, lines, depth = [], [], [], 0
    for i, (t, ln) in enumerate(toks):
        if i in cut:
            cur.append(t)
            lines.append(ln)
            out.append((cur, lines))
            cur, lines, depth = [], [], 0
            continue
        if t in (";", "{", "}") or (t == "," and depth == 0):
            if cur:
                out.append((cur, lines))
            cur, lines = [], []
            if t in ("{", "}"):
                depth = 0
            continue
        if t == "(":
            depth += 1
        elif t == ")":
            depth -= 1
        cur.append(t)
        lines.append(ln)
    if cur:
        out.append((cur, lines))
    return out


def build_claims(m, names):
    claims = {}
    for i, (reg, text, fields, how) in enumerate(INLINE_CLAIMS):
        if reg not in m.regs:
            report("NOTE", f"INLINE_CLAIMS: {reg} is not a register macro in stm32g030.h")
            continue
        if "*" in text and "<<" in text and not fields.endswith("*"):
            item = ("shl", num(text.split("<<")[0].strip()), "*")
        else:
            toks = [t for t, _ in tokenize(text)]
            its = [x for x, _ in items(toks, lambda n: None if n in ("x", "n") else names(n))]
            its = [x if x[0] != "arith" else ("lit", x[1]) for x in its]
            if len(its) != 1:
                report("FAIL", f"INLINE_CLAIMS: cannot read {text!r} for {reg}")
                continue
            item = its[0]
        claims.setdefault(reg, []).append({"id": i, "item": item, "fields": fields, "how": how,
                                           "text": text, "used": []})
    return claims


def item_matches(claim_item, item):
    if claim_item == item:
        return True
    if claim_item[0] == "shl" and item[0] == "shl":
        if claim_item[2] == "*" and claim_item[1] == item[1] and isinstance(item[2], int):
            return True
        if claim_item[1] is None and claim_item[2] == item[2] and item[1] is None:
            return True
    return False


def check_claim(m, st, claim, periph, addr):
    """[(ok, text)] for one claim on one register instance."""
    label, fields = m.fields(periph, addr)
    if fields is None:
        return [(False, f"{periph} has no register here")]
    item, how, spec = claim["item"], claim["how"], claim["fields"]
    if how == "address":
        regs = m.svd.regs_at(item[1], item[2]) if item[1] in m.svd.periphs else []
        ok = any(r["norm"] == spec for r in regs)
        return [(ok, f"{item[1]} + {item[2]:#x} is {item[1]}." + (regs[0]["norm"] if regs else "reserved"))]
    if spec.endswith("*"):
        pre = spec[:-1]
        pat = {int(k[len(pre):]): v for k, v in fields.items()
               if k.startswith(pre) and k[len(pre):].isdigit()}
        if not pat:
            return [(False, f"{label} has no {spec} fields")]
        amt = item[2] if item[0] == "shl" else item[1]
        widths = {w for _, w in pat.values()}
        if len(widths) != 1:
            return [(False, f"{label}.{spec}: fields of different widths")]
        w = widths.pop()
        res = []
        if isinstance(amt, tuple):
            bad = [n for n, (off, _) in pat.items() if off != amt[1] * n + amt[2]]
            pos = ("n" if amt[1] == 1 else f"{amt[1]}n") + (f"{amt[2]:+d}" if amt[2] else "")
            res.append((not bad, f"{label}.{pre}n at bit {pos}"
                        + (f" (not for n = {bad})" if bad else "")))
        elif amt is not None:
            hit = [n for n, (off, _) in pat.items() if off == amt]
            res.append((bool(hit), f"{label}.{pre}{hit[0] if hit else '?'} at bit {amt}"))
        val = item[1] if item[0] == "shl" else item[2]
        if val is not None:
            ok = val == (1 << w) - 1 if how == "mask" or item[0] == "ext" else val < (1 << w)
            res.append((ok, f"{val:#x} {'is the' if ok else 'does not fit the'} {w}-bit field"))
        return res
    names_ = spec.split("|")
    fs, res = [], []
    for n in names_:
        f = find_field(fields, n)
        if f is None:
            return [(False, f"{label} has no field {n}")]
        off, w = fields[f]
        w, extra = cmsis_vs_svd(st, claim["reg"], f, off, w, label)
        fs.append((f, (off, w)))
        res.extend(extra)
    if item[0] == "shl" and item[2] == "*":
        f, (off, w) = fs[0]
        return [(True, f"{label}.{f} holds any bit 0..{w - 1} (IRQ numbers checked separately)")]
    if item[0] == "shl":
        f, (off, w) = fs[0]
        if isinstance(item[2], int):
            res.append((item[2] == off, f"{label}.{f} at bit {off}"))
        if item[1] is not None:
            ok = item[1] == (1 << w) - 1 if how == "mask" else item[1] < (1 << w)
            res.append((ok, f"{item[1]} fits {w} bit{'s' if w > 1 else ''}" if ok
                        else f"{item[1]:#x} does not fit {label}.{f} ({w} bits)"))
    elif item[0] == "ext":
        f, (off, w) = fs[0]
        res.append((item[1] == off, f"{label}.{f} at bit {off}"))
        res.append((item[2] == (1 << w) - 1, f"mask {item[2]:#x} for {w} bit{'s' if w > 1 else ''}"))
    elif item[0] in ("lit", "arith"):
        v = item[1]
        if how == "mask":
            want = 0
            for _, (off, w) in fs:
                want |= ((1 << w) - 1) << off
            res.append((v == want, f"{v:#x} = mask of {label}.{'|'.join(f for f, _ in fs)}"
                        + ("" if v == want else f" ({want:#x})")))
        else:
            for f, (off, w) in fs:
                res.append((v < (1 << w), f"{v} a value of {label}.{f} ({w} bits)"))
    else:
        res.append((False, f"cannot check a {item[0]} item"))
    return res


def check_sources(m, st, src_files, names):
    claims = build_claims(m, names)
    for reg, cl in claims.items():
        for c in cl:
            c["reg"] = reg
    reg_names = set(m.regs)
    results = {}  # claim id -> list of (ok, text, where)
    for path in src_files:
        rel = os.path.basename(path)
        text = strip_c(open(path, encoding="utf-8").read())
        text = re.sub(r"^\s*#[^\n]*", "", text, flags=re.M)
        aliases = {}
        for stoks, lines in statements(tokenize(text)):
            toks = list(stoks)
            occ = []  # (macro, instance or None)
            j = 0
            while j < len(toks):
                t = toks[j]
                if t in reg_names:
                    inst = None
                    if m.regs[t]["param"] and j + 1 < len(toks) and toks[j + 1] == "(":
                        e = group_end(toks, j + 1)
                        arg = toks[j + 2:e]
                        inst = arg[0] if len(arg) == 1 and arg[0] in m.bases else None
                        if inst:
                            occ.append((t, inst, lines[j]))
                        else:
                            occ.append((t, None, lines[j]))
                        toks[j + 1:e + 1] = ["@"] * (e - j)
                    else:
                        occ.append((t, None, lines[j]))
                elif t in aliases:
                    occ.append(aliases[t] + (lines[j],))
                j += 1
            if not occ:
                continue
            # `x = REG...` makes x an alias of the register within this file.
            k = 0
            while k < len(toks) and toks[k] in ("const", "volatile", "static") or (
                    k < len(toks) and re.fullmatch(r"u?int\d+_t|int|unsigned", toks[k])):
                k += 1
            if k + 2 < len(toks) and toks[k + 1] == "=" and toks[k + 2] in reg_names \
                    and toks[k] not in reg_names:
                aliases[toks[k]] = (toks[k + 2], occ[0][1])
            # Each register instance's use: the register must exist there.
            for macro, inst, ln in occ:
                if inst:
                    label, fields = m.fields(*m.instances(macro, inst)[0])
                    if label is None:
                        report("FAIL", f"{rel}:{ln} {macro}({inst}): {inst} has no such register")
            clean = [t for t in toks if t != "@"]
            for item, idx in items(clean, names):
                if item[0] == "arith" or item in (("lit", 0),):
                    continue
                if item[0] == "addr" and item[1] not in m.bases:
                    continue
                ln = lines[min(idx, len(lines) - 1)]
                matched = None
                for macro, inst, _ in occ:
                    for c in claims.get(macro, []):
                        if item_matches(c["item"], item):
                            matched = (c, macro, inst)
                            break
                    if matched:
                        break
                if not matched:
                    if item[0] == "lit" and value_reg_fits(m, occ, item[1]):
                        continue
                    regs = ", ".join(sorted({o[0] for o in occ}))
                    report("FAIL", f"{rel}:{ln} UNCHECKED {sig_text(item)} on {regs}: add it to "
                                   "INLINE_CLAIMS or use a named macro")
                    continue
                c, macro, inst = matched
                c["used"].append(f"{rel}:{ln}")
                for periph, addr in m.instances(macro, inst):
                    if m.fields(periph, addr)[0] is None and m.regs[macro]["param"] and not inst:
                        continue  # a family member without this register
                    for ok, txt in check_claim(m, st, c, periph, addr):
                        results.setdefault(c["id"], set()).add((ok, txt))
            check_whole_write(m, rel, clean, occ, lines, names)
    print("-- inline constants (INLINE_CLAIMS)")
    for reg, cl in claims.items():
        for c in cl:
            where = f"{','.join(sorted(set(c['used']))) or '(unused)'} {reg} {c['text']} = {c['fields']}"
            if not c["used"]:
                report("NOTE", f"INLINE_CLAIMS entry not used by the code: {reg} {c['text']}")
                continue
            res = results.get(c["id"], set())
            bad = sorted(t for ok, t in res if not ok)
            if bad:
                report("FAIL", f"{where}: " + "; ".join(bad))
            else:
                report("PASS", f"{where}: " + "; ".join(sorted(t for _, t in res)))


def value_reg_fits(m, occ, v):
    """A bare value written to a register that holds one number from bit 0 (ARR, CCR, RLR...;
    also when the SVD splits it, as ARR_L/ARR_H or BRR_0_3/BRR_4_15)."""
    for macro, inst, _ in occ:
        for periph, addr in m.instances(macro, inst):
            label, fields = m.fields(periph, addr)
            if not fields:
                continue
            regname = label.split(".")[-1].split("_")[-1]
            if len(fields) > 1 and not all(k.startswith(regname) for k in fields):
                continue
            mask = defined_mask(fields)
            if mask & (mask + 1) == 0 and v <= mask:  # contiguous from bit 0
                return True
    return False


def check_whole_write(m, rel, toks, occ, lines, names):
    """REG = constant: no reserved bits, and a one-field register's value fits."""
    if len(occ) != 1 or not toks or toks[0] != occ[0][0]:
        return
    k = 1
    if k < len(toks) and toks[k] == "(":
        k = group_end(toks, k) + 1
    if k >= len(toks) or toks[k] not in ("=", "|="):
        return
    rhs = toks[k + 1:]
    if not rhs or rhs[0] == "~":
        return  # rc_w0 clears (TIMx_SR = ~flag) write 1 to reserved bits, as ST's own drivers do
    try:
        v = ceval(" ".join(rhs), names)
    except (NotConst, ZeroDivisionError):
        return
    macro, inst, ln = occ[0]
    for periph, addr in m.instances(macro, inst):
        label, fields = m.fields(periph, addr)
        if not fields:
            continue
        bad = v & ~defined_mask(fields) & 0xFFFFFFFF
        what = f"{rel}:{ln} {macro}{'(' + inst + ')' if inst else ''} {toks[k]} {v:#x}"
        if bad:
            report("FAIL", f"{what}: sets reserved bits {bad:#x} of {label}")
        else:
            report("PASS", f"{what}: only defined bits of {label}")


# ------------------------------------------------------------------------- vectors and layout

def check_vectors(svd, st, path, hdr_names, c_texts):
    print("-- vector table (startup.c)")
    text = strip_c(open(path, encoding="utf-8").read())
    rel = os.path.basename(path)
    size = re.search(r"vectors\s*\[\s*16\s*\+\s*(\d+)\s*\]", text)
    need = max(svd.irqs.values()) + 1
    if not size:
        report("FAIL", f"{rel}: no vectors[16 + N] array found")
    else:
        n = int(size.group(1))
        ok = need <= n <= 32
        report("PASS" if ok else "FAIL", f"{rel}: 16 + {n} vectors (the SVD's highest IRQ is "
                                         f"{need - 1}; a Cortex-M0+ has at most 32)")
    raw = open(path, encoding="utf-8").read()
    for mm in re.finditer(r"\[\s*(\d+)\s*\]\s*=\s*V\((\w+)\)\s*,?\s*/\*\s*(\w+)", raw):
        idx, label = int(mm.group(1)), mm.group(3)
        want = CORE_EXCEPTIONS.get(idx)
        report("PASS" if want == label else "FAIL",
               f"{rel}: vector {idx} labelled {label}" + ("" if want == label else f", ARMv6-M has {want} there"))
    handlers = {}
    for mm in re.finditer(r"\[\s*16\s*\+\s*(\w+)\s*\]\s*=\s*V\((\w+)\)", text):
        n = int(mm.group(1)) if mm.group(1).isdigit() else hdr_names(mm.group(1))
        h = mm.group(2)
        irq = re.sub(r"_IRQHandler$", "", h)
        handlers[irq] = n
        want = svd.irqs.get(irq)
        cm = st.cmsis(f"{irq}_IRQn") if st.has("stm32g030xx.h") else None
        if want is None and cm is None:
            report("FAIL", f"{rel}: vector 16 + {n} = {h}: no interrupt {irq} in the SVD")
        elif n not in (want, cm):
            report("FAIL", f"{rel}: vector 16 + {n} = {h}, SVD says {irq} = {want}")
        else:
            report("PASS", f"{rel}: vector 16 + {n} = {h} (SVD {irq} = {want})"
                   + (" [CMSIS agrees]" if cm == n else ""))
    for t in c_texts:
        for mm in re.finditer(r"NVIC_ISER\s*=\s*1u?\s*<<\s*IRQ_(\w+)", t):
            irq = mm.group(1)
            ok = handlers.get(irq) == hdr_names("IRQ_" + irq)
            report("PASS" if ok else "FAIL", f"NVIC_ISER enables IRQ_{irq}: "
                   + ("its handler is in the vector table" if ok else "no matching vector-table entry"))


def check_layout(svd, st, ld_path, c_files, names):
    print("-- flash and RAM layout")
    ld = strip_c(open(ld_path, encoding="utf-8").read())
    regions = {}
    for mm in re.finditer(r"(\w+)\s*\([rwx]+\)\s*:\s*ORIGIN\s*=\s*(0x[0-9A-Fa-f]+)\s*,\s*LENGTH\s*=\s*(\d+)([KM]?)", ld):
        mult = {"": 1, "K": 1024, "M": 1 << 20}[mm.group(4)]
        regions[mm.group(1)] = (int(mm.group(2), 16), int(mm.group(3)) * mult)
    rel = os.path.basename(ld_path)
    flash_base = st.value("FLASH_BASE") if st.has("stm32g030xx.h") else None
    sram_base = st.value("SRAM_BASE") if st.has("stm32g030xx.h") else None
    sram_max = st.value("SRAM_SIZE_MAX") if st.has("stm32g030xx.h") else None
    page = st.value("FLASH_PAGE_SIZE") if st.has("stm32g0xx_hal_flash.h") else None

    def against(what, have, ref, refname):
        if ref is None:
            report("UNVERIFIED", f"{what} = {have:#x}: {refname} not given")
        elif have == ref:
            report("PASS", f"{what} = {have:#x} [{refname}]")
        else:
            report("FAIL", f"{what} = {have:#x}, {refname} = {ref:#x}")

    if "FLASH" not in regions or "RAM" not in regions:
        report("FAIL", f"{rel}: FLASH and RAM regions not found")
        return
    fo, fl = regions["FLASH"]
    ro, rl = regions["RAM"]
    against(f"{rel} FLASH origin", fo, flash_base, "ST's CMSIS FLASH_BASE")
    against(f"{rel} RAM origin", ro, sram_base, "ST's CMSIS SRAM_BASE")
    if sram_max is None:
        report("UNVERIFIED", f"{rel} RAM length {rl}: ST's CMSIS SRAM_SIZE_MAX not given")
    else:
        report("PASS" if rl <= sram_max else "FAIL",
               f"{rel} RAM length {rl} <= {sram_max} [ST's CMSIS SRAM_SIZE_MAX]")
    saved = regions.get("SAVED")
    end = fo + fl + (saved[1] if saved else 0)
    report("PASS" if end - fo == FLASH_SIZE_K8 else "FAIL",
           f"{rel} FLASH + SAVED = {(end - fo) // 1024} KB, the STM32G030K8's 64 KB (DS12991, built in)")
    if saved:
        report("PASS" if saved[0] == fo + fl else "FAIL",
               f"{rel} SAVED at {saved[0]:#x} follows FLASH (ends {fo + fl:#x})")
    # The driver's page address formula and its flash erase page number.
    for path in c_files:
        t = strip_c(open(path, encoding="utf-8").read())
        mm = re.search(r"(0x[0-9A-Fa-f]+)u?\s*\+\s*\(\s*(\d+)u?\s*\+\s*(\w+)\s*\)\s*\*\s*(\d+)u?", t)
        if not mm:
            continue
        cname = os.path.basename(path)
        base, first, var, psize = int(mm.group(1), 16), int(mm.group(2)), mm.group(3), int(mm.group(4))
        against(f"{cname} flash base", base, flash_base, "ST's CMSIS FLASH_BASE")
        against(f"{cname} flash page size", psize, page, "ST's HAL FLASH_PAGE_SIZE")
        if saved:
            ok = saved[0] == base + first * psize and saved[0] % psize == 0 and saved[1] % psize == 0
            report("PASS" if ok else "FAIL",
                   f"{cname} pages {first}.. at {base + first * psize:#x}, {rel} SAVED at "
                   f"{saved[0]:#x}, {saved[1] // psize} page(s) of {psize}")
        pnb = re.search(r"\(\s*(\d+)u?\s*\+\s*" + var + r"\s*\)\s*<<\s*(\d+)", t)
        fields = merged_fields(svd.regs_at("FLASH", 0x14))
        if pnb:
            off, w = fields["PNB"]
            last = int(pnb.group(1)) + (saved[1] // psize - 1 if saved else 0)
            ok = int(pnb.group(1)) == first and int(pnb.group(2)) == off and last < (1 << w)
            report("PASS" if ok else "FAIL",
                   f"{cname} FLASH_CR page number {pnb.group(1)}+{var} << {pnb.group(2)}: PNB at bit "
                   f"{off}, last page {last} fits {w} bits, same first page as the address")
        wsize = re.search(r"i\s*\+=\s*(\d+)", t[mm.end():])
        if wsize:
            report("PASS" if wsize.group(1) == "8" else "FAIL",
                   f"{cname} programs {wsize.group(1)} bytes at a time (the G0 programs double words)")
    ob = []
    for path in c_files:
        for mm in re.finditer(r"0[xX]1[fF]{3}78[0-9A-Fa-f]{2}", open(path, encoding="utf-8").read()):
            ob.append((os.path.basename(path), int(mm.group(0), 16)))
    if not ob:
        report("UNVERIFIED", f"option bytes at {OPTION_BYTES:#x} (RM0454): the driver does not use "
                             "the address (it reads FLASH_OPTR)")
    for f, v in ob:
        report("PASS" if v == OPTION_BYTES else "FAIL",
               f"{f}: option-byte address {v:#x} (RM0454: {OPTION_BYTES:#x}, built in; UNVERIFIED by a file)")
    if st.has("stm32g0xx_hal_flash.h"):
        for key, want in (("FLASH_KEY1", 0x45670123), ("FLASH_KEY2", 0xCDEF89AB)):
            v = st.value(key)
            found = any(re.search(f"{want:#010X}".replace("0X", "0x"), open(p, encoding="utf-8").read(), re.I)
                        for p in c_files)
            report("PASS" if v == want and found else "FAIL",
                   f"flash unlock key {want:#010x} in the driver [ST's HAL {key} = {v:#x}]")


# ------------------------------------------------------------------------------------------ main

def sha256(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def main():
    global quiet
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--svd", required=True, help="ST's STM32G030.svd")
    ap.add_argument("--st-dir", help="directory with ST's stm32g030xx.h and HAL/LL headers")
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."),
                    help="the stm32/ directory (default: this script's parent)")
    ap.add_argument("-q", "--quiet", action="store_true", help="print only what is not PASS")
    a = ap.parse_args()
    quiet = a.quiet
    hal = os.path.join(a.root, "hal")
    header = os.path.join(hal, "stm32g030.h")
    c_files = sorted(os.path.join(hal, f) for f in os.listdir(hal) if f.endswith(".c"))
    lds = sorted(os.path.join(hal, f) for f in os.listdir(hal) if f.endswith(".ld"))

    print("-- sources")
    svd = Svd(a.svd)
    st = St(a.st_dir)
    for p in [a.svd] + st.files:
        h, pin = sha256(p), PINNED.get(os.path.basename(p))
        tag = "pinned" if h == pin else "NOT the pinned file (" + (pin or "none") + ")"
        print(f"   {os.path.basename(p)} sha256 {h} {tag}")
        if h != pin:
            report("NOTE", f"{os.path.basename(p)} differs from the file this checker was written against")
    print(f"   SVD device {svd.name} version {svd.version}")
    if not st.files:
        print("   no --st-dir: DMAMUX, system memory and layout facts print UNVERIFIED")

    hdr = parse_defines(header)
    cdefs = {}
    for p in c_files:
        cdefs.update(parse_defines(p))
    names = Names(hdr, cdefs)
    m = Map(svd, hdr, names)
    check_header(svd, m, hdr, names, st)
    srcs = [p for p in c_files if os.path.basename(p) != "startup.c"]
    check_sources(m, st, srcs, names)
    startup = os.path.join(hal, "startup.c")
    if os.path.isfile(startup):
        check_vectors(svd, st, startup, names, [open(p, encoding="utf-8").read() for p in srcs])
    for ld in lds:
        check_layout(svd, st, ld, srcs, names)
    print(f"-- {counts['PASS']} PASS, {counts['FAIL']} FAIL, {counts['UNVERIFIED']} UNVERIFIED, "
          f"{counts['NOTE']} NOTE")
    return 1 if counts["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main())
