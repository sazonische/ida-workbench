"""Headless IDA analysis that preserves the normal interactive defaults.

Unlike IDA's -B/analysis.idc shortcut, this script does not enable AF_DODATA,
does not generate a potentially huge .asm listing, and does not save the hidden
analysis window's desktop layout into the database.
"""
import ida_auto
import ida_netnode
import ida_pro
import idautils


print("[ida-workbench] waiting for standard auto-analysis...")
ida_auto.auto_wait()

# The ida-pro-mcp plugin builds this cache when a database is opened manually.
# Enumerating strings can schedule additional x86 analysis (notably separating
# some jump targets from function tails), so repeat the same pass headlessly and
# wait again. This makes the function set match an interactive MCP-enabled load.
strings = list(idautils.Strings())
print(f"[ida-workbench] indexed {len(strings)} strings; waiting for follow-up analysis...")
ida_auto.auto_wait()

# Workbench starts ida-pro-mcp itself on a per-library port. Persistently disable
# the plugin's separate default-port autostart in the newly-created database.
node = ida_netnode.netnode("$ ida_mcp.autostart", 0, True)
node.altset(0, 1)

print("[ida-workbench] analysis complete; saving database without GUI desktop")
ida_pro.qexit(0)
