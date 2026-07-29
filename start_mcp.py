"""IDA -S startup: bind the ida_mcp HTTP server on $IDA_MCP_PORT.

Launched by IDA Workbench as:  ida[.exe] -A -S"start_mcp.py" <database>
Env: IDA_MCP_PORT (required), IDA_MCP_HOST (default 127.0.0.1), IDA_MCP_LOG (optional).
This is IDA's own scripting layer (IDA runs Python for -S) -- not part of the Node tool.
"""
import os
import sys
import traceback
from contextlib import contextmanager
from datetime import datetime

LOG = os.environ.get("IDA_MCP_LOG")
LOG_MAX_MB = int(os.environ.get("IDA_MCP_LOG_MAX_MB", "10") or 0)
PORT = os.environ.get("IDA_MCP_PORT", "?")


@contextmanager
def _locked_log():
    """Use the same one-byte lock as Workbench while appending or trimming."""
    if not LOG:
        yield
        return
    lock = open(LOG + ".lock", "a+b")
    try:
        lock.seek(0, os.SEEK_END)
        if lock.tell() == 0:
            lock.write(b"\0")
            lock.flush()
        lock.seek(0)
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(lock.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            lock.seek(0)
            if os.name == "nt":
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
    finally:
        lock.close()


def _trim_log_locked():
    if not LOG or LOG_MAX_MB <= 0:
        return
    cap = LOG_MAX_MB * 1024 * 1024
    try:
        size = os.path.getsize(LOG)
        if size <= cap:
            return
        keep = cap * 9 // 10
        with open(LOG, "rb") as source:
            source.seek(max(0, size - keep))
            tail = source.read()
        newline = tail.find(b"\n")
        if 0 <= newline + 1 < len(tail):
            tail = tail[newline + 1:]
        temporary = f"{LOG}.trim-{os.getpid()}"
        with open(temporary, "wb") as output:
            output.write(b"[earlier lines trimmed to keep this log under the size cap]\n")
            output.write(tail)
        os.replace(temporary, LOG)
    except Exception:
        pass


def _log(m, level="INFO"):
    # Share the app's canonical log file, matching its line format so the GUI, the
    # verbose diagnostics and every server interleave cleanly:
    #   "yyyy-MM-dd HH:mm:ss.mmm  LEVEL  [mcp:PORT] message"
    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S.") + f"{datetime.now().microsecond // 1000:03d}"
    line = f"{ts}  {level:<5}  [mcp:{PORT}] {m}\n"
    try:
        sys.stdout.write(line)
        sys.stdout.flush()
    except Exception:
        pass
    if LOG:
        try:
            with _locked_log():
                # Open-append-close per line so writes stay whole across processes.
                with open(LOG, "a", encoding="utf-8") as f:
                    f.write(line)
                _trim_log_locked()
        except Exception:
            pass


def _main():
    import ida_nalt

    host = os.environ.get("IDA_MCP_HOST", "127.0.0.1")
    port = int(os.environ.get("IDA_MCP_PORT", "13337"))
    plug = os.path.join(os.environ["APPDATA"], "Hex-Rays", "IDA Pro", "plugins") \
        if sys.platform == "win32" else os.path.join(os.path.expanduser("~"), ".idapro", "plugins")
    if plug not in sys.path:
        sys.path.insert(0, plug)

    from ida_mcp import (MCP_SERVER, IdaMcpHttpRequestHandler,
                         init_caches, set_local_instance)
    try:
        init_caches()
    except Exception as e:
        _log(f"cache init failed: {e}", "WARN")

    MCP_SERVER.serve(host, port, request_handler=IdaMcpHttpRequestHandler)
    set_local_instance(host, port)
    try:
        import idc
        from ida_mcp.discovery import register_instance

        register_instance(host=host, port=port, pid=os.getpid(),
                          binary=ida_nalt.get_root_filename() or "",
                          idb_path=idc.get_idb_path() or "")
    except Exception as e:
        _log(f"register failed: {e}", "WARN")

    _log(f"SERVING {ida_nalt.get_root_filename()} on http://{host}:{port}/ pid={os.getpid()}")


try:
    _main()
except Exception:
    _log("FAILED:\n" + traceback.format_exc(), "ERROR")
