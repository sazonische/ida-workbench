"""Run once per IDB during analysis (idat -c -B -S) to disable ida-pro-mcp autostart.

Why: the ida-pro-mcp plugin autostarts its server on the default port 13337 when a
database is opened in the GUI. Our launcher instead runs start_mcp.py, which binds the
per-module IDA_MCP_PORT. If both fire, IDA ends up with two servers (13337 + our port)
and pollutes the discovery dir. Disabling autostart here makes start_mcp.py the only
thing that binds a port. The -B batch save persists this netnode into the .i64.
"""
import ida_netnode

NODE = "$ ida_mcp.autostart"
n = ida_netnode.netnode(NODE, 0, True)
n.altset(0, 1)  # 0 = unset, 1 = off, 2 = on  (see ida_mcp.py:_get_autostart)
print("[disable_autostart] autostart disabled for this IDB")
