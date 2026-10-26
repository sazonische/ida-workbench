# IDA Workbench

A Windows app that prepares binaries and IDA databases for AI agents such as Codex and Claude Code. It organizes libraries into workspaces, analyzes them with IDA, keeps older builds and gives agents access through the [official Hex-Rays IDA MCP server](https://github.com/HexRaysSA/ida-mcp).

> **Version 2 requires IDA 9.4 or newer** and works only with the official Hex-Rays MCP. The old `ida-pro-mcp` server and IDA 9.2 are no longer supported; see [Upgrading from version 1](#upgrading-from-version-1).

[Installation](#installation) · [Upgrade from v1](#upgrading-from-version-1) · [Troubleshooting](#troubleshooting) · [Build](#building-from-source)

Workbench is an independent project. It does not include IDA or an IDA license, and the MCP server is developed by Hex-Rays.

## Requirements

| Component | Requirement |
|---|---|
| OS | Windows x64 |
| IDA | 9.4 or newer with idalib |
| IDAPython | Python 3.11+, selected in IDA. A Python installed only for the terminal is not enough. |
| Tools | [Git](https://git-scm.com/downloads/win) and [uv](https://docs.astral.sh/uv/getting-started/installation/) on PATH |
| Agent | Codex, Claude Code or another MCP client running on the same PC |

See also the [Hex-Rays requirements](https://github.com/HexRaysSA/ida-mcp#requirements).

## Installation

### 1. Install IDA, Git and uv

Install IDA 9.4 or newer and start it once to check that its Python console works. Install [Git](https://git-scm.com/downloads/win) if you don't have it, then uv:

```powershell
winget install --id astral-sh.uv -e
```

uv includes `uvx`. Other installation methods are in the [uv documentation](https://docs.astral.sh/uv/getting-started/installation/). Open a new PowerShell window and check that `uvx --version` works.

### 2. Install the Hex-Rays MCP plugin

```powershell
uvx ida-hcli mcp install
```

Restart IDA afterwards. Workbench enables **Open MCP**, **Open IDA** and **Analyze** only once this plugin is installed.

The installer also offers to set up your agent. You can accept, or skip it and use Workbench's export in step 4; just keep only one entry for the server.

If it reports a Python problem, run `uvx ida-hcli ida python doctor` and see the Hex-Rays [IDAPython environment guide](https://docs.hex-rays.com/getting-started/idapython-environment). For a non-standard IDA installation, follow the [official setup guide](https://github.com/HexRaysSA/ida-mcp#installation).

### 3. Run Workbench

Download `ida-workbench-<version>-windows-x64.exe` from [Releases](https://github.com/sazonische/ida-workbench/releases) and run it from any folder. It is a single file.

The exe is not code-signed, so Windows shows *"Windows protected your PC"* on the first launch: click **More info → Run anyway**, or run `Unblock-File` on the download first. To check that the file was built by this repository's workflow:

```powershell
gh attestation verify .\ida-workbench-<version>-windows-x64.exe --repo sazonische/ida-workbench
```

Workbench finds IDA and uvx on its own. If it doesn't, open **Settings**, select `ida.exe` and `idat.exe` from the same IDA installation and `uvx.exe` under **Official Hex-Rays MCP** (`Get-Command uvx` shows where it is), and save. If you installed uv while Workbench was running, exit it from the tray and start it again.

On startup Workbench checks GitHub for a newer release and offers to open its page.

### 4. Connect your agent

Open **Connect**. The first two tabs are configurations for Codex and Claude Code with your uvx and IDA paths filled in. Copy one, merge it into the agent's configuration without removing other servers, and restart the agent. This is needed only once: adding libraries or switching revisions does not change it.

**Codex**: `%USERPROFILE%\.codex\config.toml`

```toml
[mcp_servers.ida94]
command = "C:/Users/you/.local/bin/uvx.exe"
args = ["--exclude-newer=1s", "ida-mcp", "stdio", "--agent=codex"]

[mcp_servers.ida94.env]
IDADIR = "C:\\Program Files\\IDA Professional 9.4"
```

Check it with `codex mcp list`, or `/mcp` inside a session. See the [Codex MCP documentation](https://learn.chatgpt.com/docs/extend/mcp?surface=cli).

**Claude Code**: `.mcp.json` in the root of your project

```json
{
  "mcpServers": {
    "ida94": {
      "command": "C:/Users/you/.local/bin/uvx.exe",
      "args": ["--exclude-newer=1s", "ida-mcp", "stdio", "--agent=claude-code"],
      "env": {
        "IDADIR": "C:\\Program Files\\IDA Professional 9.4"
      }
    }
  }
}
```

Approve the server when Claude Code asks, and check it with `/mcp`. Other configuration scopes are in the [Claude Code MCP documentation](https://code.claude.com/docs/en/mcp).

Other MCP clients take the same command, arguments and `IDADIR`.

### 5. Open a library

1. In **Settings**, add a single binary, or a folder or Steam workspace (see [Libraries and versions](#libraries-and-versions)).
2. In the main table, check the rows you need and pick a revision or manifest.
3. Press **Analyze** to build the databases in advance, or **Open MCP** to open them in the background. An existing `.i64` is used as is; otherwise IDA imports the binary.
4. Copy the last tab of **Connect**, **Selected libraries**, and paste it into the agent. It lists the exact paths, tags and revisions; with nothing checked it covers all visible rows.

The agent knows only what you paste; it does not see Workbench's table. It can open the listed paths itself, without Open MCP.

## How it works

Each agent starts its own Hex-Rays server from the configuration, and Workbench runs one more in the background. IDA Nexus, part of the Hex-Rays server, shares open databases between them.

| Button | What it does |
|---|---|
| **Open MCP** | Opens the database in the background and keeps it open for Workbench. Useful when several agent sessions work on it. |
| **Open IDA** | Opens one library in the IDA GUI. A database open in the background must first be released by everyone who holds it. |
| **Release** | Saves the database and releases Workbench's hold. Other agents and IDA windows keep it open. Also cancels selected analyses and downloads. |
| **Analyze** | Runs `idat` on the selected binaries in parallel. Existing databases are backed up and restored if analysis fails or is canceled. |
| **Replace** | Archives the current binary and database of a folder workspace and copies the new binary from its source. |
| **Update Depot** | Downloads a Steam build; see [Steam depots](#steam-depots). |
| **Connect** | Agent configurations and the list of selected libraries. |

Analyze and Replace refuse a database that is open or being analyzed.

The **Session** column shows **ATTACHED** when Workbench holds the database open, **SHARED** when it is open only in another agent or IDA window, and **closed** otherwise. Release never kills IDA.

Closing the window hides Workbench in the tray. **Exit** in the tray menu closes its server and cancels its analyses.

## Libraries and versions

- **Single library:** any file, used where it is.
- **Folder workspace:** a Source folder, an Output folder and a list of files relative to them. **Sync** shows when the source copy differs. **Replace** moves the old binary and database to `<output>/revisions/<timestamp>/` and copies the new binary, together with the source's database if it has one.
- **Steam workspace:** one depot of any Steam app, with each build in its own folder; see [Steam depots](#steam-depots).

Pick a revision or manifest to work with an older build. Older builds are read-only: Analyze and Replace are disabled for them, but they can be opened and exported next to the current one.

## Steam depots

Steam is optional. Steam workspaces are added under **Steam games** in Settings, and the **Update Depot** button appears once one exists. [DepotDownloader](https://github.com/SteamRE/DepotDownloader/releases) is downloaded automatically on first use; to use your own copy, select it in Settings.

A Steam workspace holds one depot of one app. Each build is stored in its own `<ManifestID>` folder, and `current` names the active one. Enter the AppID and the DepotID that holds the files; both are on the app's SteamDB *Depots* page, linked from the depot editor. Files are relative to the depot root, or to `pathPrefix` if it is set.

```json
{
  "tag": "mygame-windows",
  "color": "#D3E3FD",
  "dir": "D:/reverse/mygame",
  "files": ["bin/game.dll", "bin/engine.dll"],
  "appId": 123450,
  "depotId": 123451,
  "pathPrefix": "game",
  "os": "windows",
  "requiresLogin": true,
  "current": "1756429801267405217"
}
```

| Field | Meaning |
|---|---|
| `appId` | The Steam application. |
| `depotId` | The depot that holds the files. Optional for the games below. |
| `pathPrefix` | Optional folder inside the depot that `files` are relative to. |
| `os` | `windows`, `linux`, `macos`, or empty for any. |
| `requiresLogin` | Always ask for a Steam account. Most paid apps need it; free and dedicated-server depots download anonymously. |

Two games have built-in depot settings, used when `depotId` is omitted. Their builds are also labeled with `PatchVersion`/`ServerVersion` from `csgo/steam.inf`; other apps show the ManifestID only.

| Game | AppID | Windows depot | Linux depot | Files relative to | Login |
|---|---|---|---|---|---|
| Counter-Strike 2 | `730` | `2347771` | `2347773` | `game/` | anonymous |
| Counter-Strike: Global Offensive | `4465480` | `732` | `734` | depot root | required |

For CS:GO, list `csgo/bin/client.dll`, `csgo/bin/server.dll` and `bin/engine.dll` on Windows, or `csgo/bin/linux64/client_client.so`, `csgo/bin/linux64/server_client.so` and `bin/linux64/engine_client.so` on Linux.

**Update Depot** always downloads the workspace's whole file list:

- **Latest** downloads the current build and makes it `current`.
- **A ManifestID** downloads that build next to the others without changing `current`.
- When a depot needs an account (CS:GO, or `requiresLogin`), login fields appear. The username, password and Steam Guard code are used for that download only and are never saved.
- A binary that has not changed since the previous build gets that build's database, so it does not need analyzing again.
- An interrupted download can be finished later: unchanged files stay as they are, and a changed file that is in use is skipped and named in the log.
- Databases that are already open keep using the previous build until they are released and opened again from the new path.
- Files a manifest does not contain are skipped and logged.

## Configuration

Settings edits `%USERPROFILE%\.ida-workbench\config.json`, so there is no need to change the file by hand. For reference:

```json
{
  "schemaVersion": 2,
  "ida": {
    "gui": "C:/Program Files/IDA Professional 9.4/ida.exe",
    "text": "C:/Program Files/IDA Professional 9.4/idat.exe"
  },
  "mcp": { "executable": "C:/Users/you/.local/bin/uvx.exe" },
  "analysisArgs": "",
  "logDir": "~/.ida-workbench/logs",
  "maxLogSizeMB": 10,
  "extraLibs": [],
  "workspaces": [
    {
      "tag": "app-current",
      "color": "#BFE3FF",
      "source": "C:/Program Files/Vendor/App",
      "output": "D:/reverse/app",
      "files": ["bin/app.exe", "bin/render.dll"]
    }
  ],
  "steamWorkspaces": [],
  "depotDownloader": { "executable": "DepotDownloader.exe", "timeoutMinutes": 30 }
}
```

Logs are shown in the log panel and written to `%USERPROFILE%\.ida-workbench\logs`.

## Upgrading from version 1

Version 2 replaces the per-library HTTP servers and ports with one server entry per agent.

1. Exit the old Workbench and back up `%USERPROFILE%\.ida-workbench`.
2. Uninstall the old `ida-pro-mcp` plugin ([instructions](https://github.com/mrexodia/ida-pro-mcp/blob/main/CLAUDE.md#install--uninstall)) and remove its entries and per-library URLs from your agents' configurations.
3. Install IDA 9.4+ and the Hex-Rays plugin as described in [Installation](#installation).
4. Start Workbench 2. It saves the old configuration as `config.json.pre-hexrays.bak` and converts it: libraries, tags, colors, workspaces, Steam settings and stored revisions are kept, port settings are removed. Check the IDA and uvx paths in Settings.
5. Set up each agent through **Connect**, as in [step 4](#4-connect-your-agent).

Workbench does not edit agent configurations or uninstall old Python packages for you.

## Troubleshooting

| Problem | What to do |
|---|---|
| Open MCP, Open IDA and Analyze are disabled | The main window lists what is missing: IDA 9.4+, IDAPython, uvx or the Hex-Rays plugin. |
| `uvx` is not found | Install uv (step 1), then exit Workbench from the tray and start it again, or select `uvx.exe` in Settings. |
| The first connection times out | uvx is probably still downloading the server. Run `uvx --exclude-newer=1s ida-mcp --help` once, then try again. Codex also accepts `startup_timeout_sec` in the server entry. |
| The plugin installer reports a Python problem | Run `uvx ida-hcli ida python doctor`. |
| Open IDA fails, or IDA GUI databases are not listed | Check that the plugin is installed and IDA was restarted, and that `IDADIR` in the agent configuration points to the IDA selected in Workbench. |
| A database does not open in the GUI | Release it in every agent that has it open, then press **Open IDA** again. |
| The agent still shows old tools or per-library servers | Remove the old entries from its configuration, keep one `ida94` entry and restart the agent. |
| The agent works on the wrong build | Copy **Connect → Selected libraries** again after selecting the right rows and revision. |

The Hex-Rays server has a session dashboard: `uvx ida-mcp dashboard --open`. Server documentation is at [docs.hex-rays.com](https://docs.hex-rays.com/core/mcp).

## Building from source

Each script sets its Visual Studio, Qt and vcpkg paths in the first lines; change them to match your machine.

- `build.cmd` builds against an installed Qt 6.8.3 and puts the exe with its Qt DLLs in `dist\`.
- `build-static.cmd` builds the single self-contained exe, as in the releases, in `dist-static\`. It needs static Qt from vcpkg; the install command is at the top of the script.

Run the tests from a Developer PowerShell. They use fake executables and need neither IDA nor Steam:

```powershell
ctest --test-dir build --output-on-failure   # build-static for the static build
```

Created by [@sazonische](https://github.com/sazonische). Not affiliated with Hex-Rays or Valve.
