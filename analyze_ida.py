"""Headless IDA analysis that preserves the normal interactive defaults.

Unlike IDA's -B/analysis.idc shortcut, this script does not enable AF_DODATA,
does not generate a potentially huge .asm listing, and does not save the hidden
analysis window's desktop layout into the database.
"""
import ida_auto
import ida_pro
import idautils


print("[ida-workbench] waiting for standard auto-analysis...")
ida_auto.auto_wait()

# Keep string discovery in the prepared database, including any follow-up analysis.
strings = list(idautils.Strings())
print(f"[ida-workbench] indexed {len(strings)} strings; waiting for follow-up analysis...")
ida_auto.auto_wait()

print("[ida-workbench] analysis complete; saving database without GUI desktop")
ida_pro.qexit(0)
