#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Wires pad_xone_usb.h into nxnative's Mac front end.

Run from the repository root:   python3 apply_xone_patch.py
It edits dm/host/front_mac.m and dm/host/Makefile (backups: *.orig) and prints what it did.
It refuses to run twice, and stops without writing anything if an expected line is missing.
"""
import pathlib, shutil, sys

FRONT = pathlib.Path("dm/host/front_mac.m")
MAKE = pathlib.Path("dm/host/Makefile")
HDR = pathlib.Path("dm/host/pad_xone_usb.h")


def die(msg):
    sys.exit("STOP: " + msg)


def find(lines, pred, start=0, what=""):
    for i in range(start, len(lines)):
        if pred(lines[i]):
            return i
    die("could not find " + what)


def main():
    for p in (FRONT, MAKE):
        if not p.exists():
            die(f"{p} not found - run this from the repository root")
    if not HDR.exists():
        die(f"{HDR} not found - copy pad_xone_usb.h there first")

    src = FRONT.read_text().split("\n")
    if any("xone_start" in l for l in src):
        die("front_mac.m is already patched")

    # 1. include
    i_inc = find(src, lambda l: l.strip() == '#include "pad_slots.h"', what='#include "pad_slots.h"')
    # 2. input_release / input_resume
    i_rel = find(src, lambda l: l.startswith("static void input_release(void)"), what="input_release")
    i_rel_on = find(src, lambda l: "g_input_on = 0;" in l, i_rel, "g_input_on = 0; in input_release")
    i_res = find(src, lambda l: l.startswith("static void input_resume(void)"), what="input_resume")
    i_res_on = find(src, lambda l: "g_input_on = 1;" in l, i_res, "g_input_on = 1; in input_resume")
    # 3. pads_start
    i_ps = find(src, lambda l: l.startswith("static void pads_start(void)"), what="pads_start")
    i_kbd = find(src, lambda l: 'pad_slot_take(&g_slots, &kbd, "keyboard")' in l, i_ps, "the keyboard slot line in pads_start")

    if not (i_inc < i_rel < i_rel_on < i_res < i_res_on < i_ps < i_kbd):
        die("the functions are not in the expected order in front_mac.m; send me the file")

    # Insert from the bottom up so the indices stay valid.
    src.insert(i_kbd + 1, "    xone_start(&g_slots, &g_merge, xone_input_on);   // original Xbox One pad over USB (pad_xone_usb.h)")
    src.insert(i_ps, "static int xone_input_on(void) { return g_input_on; }")
    src.insert(i_res_on + 1, "    xone_resume();")
    src.insert(i_rel_on + 1, "    xone_release();")
    src.insert(i_inc + 1, '#include "pad_xone_usb.h"')

    mk = MAKE.read_text().split("\n")
    i_dep = find(mk, lambda l: l.startswith("dmhost:") and "pad_slots.h" in l, what="the dmhost: dependency line in the Makefile")
    if "pad_xone_usb.h" not in mk[i_dep]:
        mk[i_dep] = mk[i_dep].replace("pad_slots.h", "pad_slots.h pad_xone_usb.h", 1)
    # the frameworks: put them on a line that already names a framework
    i_fw = find(mk, lambda l: "-framework" in l and "GameController" in l, what="a Makefile line with -framework GameController")
    if "IOUSBHost" not in mk[i_fw]:
        extra = " -framework IOUSBHost -framework IOKit"
        line = mk[i_fw]
        if line.rstrip().endswith("\\"):
            body = line.rstrip()[:-1].rstrip()
            mk[i_fw] = body + extra + " \\"
        else:
            mk[i_fw] = line.rstrip() + extra

    for p in (FRONT, MAKE):
        shutil.copy(p, str(p) + ".orig")
    FRONT.write_text("\n".join(src))
    MAKE.write_text("\n".join(mk))
    print("front_mac.m: include, xone_input_on, xone_start in pads_start, xone_release / xone_resume added")
    print("Makefile:    pad_xone_usb.h added to the dmhost dependencies; -framework IOUSBHost -framework IOKit added to line", i_fw + 1)
    print("Check with:  git diff dm/host")


main()
