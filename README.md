# IDA Workbench

A native C++ / Qt 6 workspace manager for Windows that connects your binaries and IDA databases to Codex, Claude Code and other local MCP clients. Prepare analysis, organise libraries into workspaces, retain older builds and share databases through the **official IDA MCP server from Hex-Rays, the developer of IDA**.

> **Version 2 is a breaking change.** IDA Workbench 2.x supports only the [official Hex-Rays IDA MCP](https://github.com/HexRaysSA/ida-mcp) and requires **IDA 9.4 or newer**. The previous `ida-pro-mcp` server and its forks are no longer supported. IDA 9.2 and earlier are no longer supported. Existing version 1 MCP configurations must be replaced using the setup below.

[Installation](#installation-on-windows) · [Agent setup](#4-connect-your-agent-once) · [Upgrade from v1](#upgrading-from-version-1) · [Troubleshooting](#troubleshooting) · [Build](#build-and-validation) · [Publish a release](#publishing-a-release)

## What changes in version 2

- The official Hex-Rays server replaces the legacy `ida-pro-mcp` integration.
- One stdio MCP entry per agent replaces separate HTTP connections for each library. Host, port, base-port and port-offset settings have been removed.
- **Open MCP** opens databases in the background. **Open IDA** opens a selected library in the GUI. **Release** saves and releases Workbench's database lease.
- **Connect** exports the agent configuration and a message containing the selected libraries' exact paths, tags and revisions.
- Existing folder and Steam workspaces, library lists, tags, colours and revision directories are retained during configuration migration.

Workbench is an independent application. The MCP server is developed and maintained by Hex-Rays; Workbench does not bundle IDA or provide an IDA license.

## Requirements

| Component | Requirement |
|---|---|
| Operating system | Windows x64, the platform validated for Workbench. |
| IDA | A working IDA **9.4+** installation with idalib and the processor/decompiler support needed for your files. |
| IDAPython | Python **3.11+**, configured for IDA. Installing Python for your terminal alone does not configure IDAPython. |
| Launch tools | [Git](https://git-scm.com/downloads/win) and [uv](https://docs.astral.sh/uv/getting-started/installation/), including `uvx`. |
| Agent | A local MCP client such as Codex or Claude Code, running on the PC that holds the IDA installation and binaries. |
| Optional Steam downloads | [DepotDownloader](https://github.com/SteamRE/DepotDownloader/releases), configured separately in Settings. |

See the [Hex-Rays installation requirements](https://github.com/HexRaysSA/ida-mcp#requirements) for the server. IDA edition and license capabilities still apply.

## Installation on Windows

### 1. Install IDA and the launch tools

Install IDA 9.4 or newer with IDAPython and idalib. Start IDA once and confirm that its Python console works. Keep the GUI and text executables from the same installation, for example:

```text
C:\Program Files\IDA Professional 9.4\ida.exe
C:\Program Files\IDA Professional 9.4\idat.exe
```

Your installation directory may have a different name.

Install Git from its [Windows download page](https://git-scm.com/downloads/win). Install uv using the [official instructions](https://docs.astral.sh/uv/getting-started/installation/); with WinGet:

```powershell
winget install --id astral-sh.uv -e
```

**uvx is included with uv**; there is no separate uvx installation step. Workbench needs it to launch the official MCP server. In **Settings → Official Hex-Rays MCP**, **Copy uv install command** copies the command above, and **uv installation guide** opens the official instructions if WinGet is unavailable or you prefer another installation method.

Open a new PowerShell window after installation and check that the tools are available:

```powershell
git --version
uv --version
uvx --version
```

### 2. Install the official Hex-Rays IDA MCP

Run the official installer:

```powershell
uvx ida-hcli mcp install
```

It installs the GUI integration and offers agent setup. You can choose **Other agent (mcp.json)** and configure your agent through Workbench's **Connect** dialog in step 4. If the installer already configured your agent, use that entry or replace it with the Workbench export; avoid adding a second entry for the same server.

Restart any open IDA windows after installing the plugin. The GUI plugin enables access to GUI databases and the **Open IDA** action. Follow the [official setup guide](https://github.com/HexRaysSA/ida-mcp#installation) if you use a custom IDA installation.

If the installer reports a Python environment problem, inspect the diagnostics:

```powershell
uvx ida-hcli ida python doctor
```

Use those diagnostics to fix the Python environment selected by IDA before troubleshooting Workbench. Hex-Rays also documents the recommended [IDAPython environment setup](https://docs.hex-rays.com/getting-started/idapython-environment).

### 3. Download and configure Workbench 2

Download the Windows x64 EXE from a **2.x release** on the [Releases page](https://github.com/sazonische/ida-workbench/releases). For version 2.0.0, the asset is named `ida-workbench-2.0.0-windows-x64.exe`.

Put the EXE in a folder of your choice and run it. The release is a single static executable with its analyzer embedded; building Qt or copying Python helpers beside it is not required.

Open **Settings** and check the detected paths:

| Setting | Select |
|---|---|
| IDA GUI | `ida.exe` from your supported IDA installation. |
| IDA text/analyzer | `idat.exe` from that same installation. |
| uvx executable, under Official Hex-Rays MCP | `uvx.exe`. This launches the Python MCP package; do not select an old `ida-pro-mcp.exe` launcher. |

Workbench searches for uvx on PATH and in `%USERPROFILE%\.local\bin`. If it is not detected, find its installed path with `Get-Command uvx` and select that file in Settings. Save the settings before exporting the agent configuration.

If uvx is missing, the main window displays the installation command and the steps to select its path. Workbench checks whether the executable can be found; it does not automatically run `uvx --version` or install uv for you. Use the version check in step 1 to verify your installation. After installing uv, fully exit Workbench from the tray and reopen it to refresh PATH, or choose `uvx.exe` explicitly in Settings.

### 4. Connect your agent once

Open **Connect** and copy the tab for your agent. The exported configuration uses your selected executable paths and sets `IDADIR` to the selected IDA directory. Merge it into the existing configuration, keeping unrelated servers and settings.

The examples below use placeholder paths. Prefer the Workbench export, or replace both the uvx and IDA paths with your actual installation paths.

#### Codex

Add the **Codex · config.toml** export to `%USERPROFILE%\.codex\config.toml`:

```toml
[mcp_servers.ida94]
command = "C:/Users/you/.local/bin/uvx.exe"
args = ["--exclude-newer=1s", "ida-mcp", "stdio", "--agent=codex"]

[mcp_servers.ida94.env]
IDADIR = "C:/Program Files/IDA Professional 9.4"
```

Restart Codex to load the configuration. In Codex CLI, `codex mcp list` lists configured servers and `/mcp` in the interactive session shows their status. See the [official Codex MCP documentation](https://learn.chatgpt.com/docs/extend/mcp?surface=cli) for configuration details.

#### Claude Code

Merge the **Claude Code · .mcp.json** export into `.mcp.json` at the root of the project where you run Claude Code:

```json
{
  "mcpServers": {
    "ida94": {
      "command": "C:/Users/you/.local/bin/uvx.exe",
      "args": ["--exclude-newer=1s", "ida-mcp", "stdio", "--agent=claude-code"],
      "env": {
        "IDADIR": "C:/Program Files/IDA Professional 9.4"
      }
    }
  }
}
```

Restart Claude Code, approve the project server when prompted, and use `/mcp` to inspect the connection. This example is for **Claude Code**; Claude Desktop has its own configuration. See the [Claude Code MCP documentation](https://code.claude.com/docs/en/mcp) for other configuration scopes.

Other local MCP clients can use the same command, arguments and environment in their own stdio configuration format. **No library ports or manually started HTTP server are needed for Workbench's setup.**

### 5. Open your first library

1. Add an individual binary or create a folder/Steam workspace in Settings. For folder workspaces, select Source and Output folders and list the relative files to track.
2. Select the required libraries and revision/manifest in the main table.
3. Use **Open MCP** to hold the selected databases open in the background. An existing `.i64` is preferred; otherwise IDA imports the binary. **Analyze** can prepare databases first, and **Open IDA** opens one selected library in the GUI.
4. Open **Connect → Selected libraries**, copy the generated message, and paste it into your agent. Ask it to open and inspect the listed paths.

The message contains exact paths, tags, revisions and local-file availability. With no checked rows, the tab is labelled **Visible libraries** and includes all libraries in the visible workspace/filter. The agent does not automatically read Workbench's catalogue or selection.

Adding a library or changing a revision does **not** require another MCP entry. The agent can open the exported path with `open_database` or inspect open databases with `list_databases`. It can also open a binary directly without Workbench holding it open first.

## How the new connection works

Workbench launches `uvx --exclude-newer=1s ida-mcp stdio --agent=ida-workbench` as a hidden child process. A native asynchronous JSON-RPC client initializes MCP and calls its official tools. No manually configured network listener is involved.

Each agent starts its own MCP process from the generated configuration. **IDA Nexus** discovers and shares the actual databases between those clients. Workbench exports `IDADIR` alongside the launch command so the selected IDA installation is used consistently.

| Action | Behaviour |
|---|---|
| **Open MCP** | Opens an existing `.i64`, or imports a local binary through `open_database`; acquires Workbench's own database lease. |
| **Open IDA** | Opens one selected library with `gui=true`. A headless database must first be released by its clients before it can move to the GUI. |
| **Release** | Saves and releases Workbench's lease. Another client's lease and a user-owned GUI window remain open. Also cancels selected analysis/download tasks. A release requested during opening is completed after the open reply. |
| **Analyze** | Runs supported `idat` processes in parallel, preserving normal analysis defaults. Previous databases are backed up and restored on failure or cancellation. |
| **Replace** | Archives the current binary and database, then copies the newer file from a folder workspace's source. Refuses files held by an analysis or open database. |
| **Update Depot** | Downloads a Steam workspace's full file list; keeps each build in its manifest directory. Shown only once a Steam workspace exists. |
| **Connect** | One Codex configuration, one Claude Code configuration, and an agent message for the current selection. |

The **Session** column distinguishes **ATTACHED** (Workbench owns a lease), **SHARED** (the database is open elsewhere) and **closed**. These states come from Nexus database listings and successful tool replies, rather than port probes or PID guesses. Release never force-kills IDA and never sweeps another process's unpacked database files.

An agent can also open a library directly from its exported path without clicking Open MCP in Workbench first. Keeping it open in Workbench is useful when several agent sessions should share it. Closing the Workbench window hides it in the tray; Exit releases its MCP connection and cancels its analysis jobs.

## Binaries, folders and versions

- **Individual libraries:** choose a file and give it a tag. The file stays at its original absolute path.
- **Folder workspaces:** choose separate Source and Output folders and list relative files. Sync compares their contents. Replace archives the old binary and database under `output/revisions/<timestamp>/` before copying the update. A source-side database is carried over when present.
- **Steam workspaces (optional):** choose any Steam app by AppID and DepotID, plus a file list. Each manifest has its own directory and database set. See [Steam depots](#steam-depots).
- **Stored versions:** select a revision/manifest to inspect it or export its exact paths. Analyze and Replace are disabled for historical versions. Current and historical databases can remain open simultaneously because they have distinct paths.

## Upgrading from version 1

1. Exit the old Workbench and stop its legacy MCP sessions.
2. Back up `%USERPROFILE%\.ida-workbench` and retain your binary/database workspace folders.
3. Disable or uninstall the old `ida-pro-mcp` plugin using its original installer's instructions. For the upstream project, see its [install/uninstall documentation](https://github.com/mrexodia/ida-pro-mcp/blob/main/CLAUDE.md#install--uninstall). Remove the old server entries and per-library HTTP URLs from your agents' MCP configurations.
4. Install IDA 9.4+ and the official Hex-Rays MCP as described above. Version 2 cannot connect to IDA 9.2 through the old server.
5. Run Workbench 2. The first load backs up the version 1 configuration to `config.json.pre-hexrays.bak` and migrates its workspace settings. Check the selected IDA and uvx paths in Settings.
6. Configure one official server entry per agent through **Connect**, restart the agent, and export the libraries you want it to inspect.

Migration retains standalone libraries, tags, colours, folder Source/Output paths, file lists, Steam application IDs and current manifests. Old revision and database directories remain in place. Obsolete host, port, base-port, per-library override and offset fields are removed from the configuration.

Workbench migrates **its own configuration**. It does not automatically uninstall third-party Python packages/plugins or rewrite Codex/Claude configuration files. The old `start_mcp.py` and `disable_autostart.py` helpers are no longer used. Remove legacy launch configurations instead of pointing them at the new server, and keep the newly installed official `ida-mcp` plugin.

## Configuration

Workbench stores its configuration in `%USERPROFILE%\.ida-workbench\config.json`; edit it through Settings. On a fresh installation it attempts to detect IDA and uvx automatically. During migration, a missing or unsupported IDA path is replaced by a detected 9.4+ installation when available; see [Upgrading from version 1](#upgrading-from-version-1) for the backup and migration steps.

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

The standalone analyzer is embedded in the exe and unpacked beside the configuration once per app version. Logs have their own `logs/` directory with a size cap. `IDA_WORKBENCH_HOME` selects an isolated app/config directory for development and validation.

## Steam depots

Steam downloads are optional and live in their own **Steam games** section of Settings. Folder workspaces and individual libraries never need them, and the **Update Depot** button appears only once a Steam workspace exists. [DepotDownloader](https://github.com/SteamRE/DepotDownloader/releases) is installed automatically on the first download while its Settings path is left as `DepotDownloader.exe`; select your own copy there to use a specific build.

A Steam workspace replaces the source folder with one depot of one app: one directory holds every build, each under its own `ManifestID`, and `current` names the active one. **Any Steam app works.** Enter its AppID and the DepotID that holds the files; both are listed on the app's SteamDB *Depots* page, which the depot editor links to. Files are listed relative to the depot root, or to the optional depot folder (`pathPrefix`).

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
| `depotId` | The depot that holds the files. Optional for the built-in games below. |
| `pathPrefix` | Optional folder inside the depot that `files` are relative to. |
| `os` | DepotDownloader's `-os` filter: `windows`, `linux`, `macos`, or empty for any. |
| `requiresLogin` | Ask for a Steam account even for **Latest**. Needed by most paid apps; free and dedicated-server depots download anonymously. |

Two games have built-in layouts, used when `depotId` is omitted. These also label each manifest with `PatchVersion`/`ServerVersion` from `csgo/steam.inf`; for other apps manifests are shown by ID.

| Game | AppID | Windows depot | Linux depot | Files relative to | Login |
|---|---|---|---|---|---|
| Counter-Strike 2 | `730` | `2347771` | `2347773` | `game/` | anonymous |
| Counter-Strike: Global Offensive | `4465480` | `732` | `734` | depot root | required |

For CS:GO, list `csgo/bin/client.dll`, `csgo/bin/server.dll`, and `bin/engine.dll` on Windows, or `csgo/bin/linux64/client_client.so`, `csgo/bin/linux64/server_client.so`, and `bin/linux64/engine_client.so` on Linux.

Depot update is a workspace-level operation: the checked row only names the workspace, and DepotDownloader always fetches that workspace's whole `files` list.

- **Latest** asks Steam what the current manifest is, stores it under `dir/<ManifestID>/` and writes that ID to `current`.
- A depot that requires login (CS:GO, or any workspace with `requiresLogin`) shows Steam login fields in both modes. Credentials are used for both the manifest query and the file download and are never written to `config.json`.
- **A numeric ManifestID** stores a historical build alongside the others without touching `current`, so you can keep old builds around and switch to them read-only. Old manifests may need Steam credentials; username, password and Guard code are passed to DepotDownloader for that one run and never written to `config.json`.
- Paths a manifest does not contain are logged and simply left out of that build's folder.
- `PatchVersion` and `ServerVersion` are display data, not config.

A new `<ManifestID>` folder is assembled aside and moved into place in one step. An **existing** folder — an interrupted download — is *completed file by file* instead: a module whose hash already matches is left completely untouched, along with its database and any live session, so you can finish downloading the remaining modules while one of them is being served over MCP. A module that changed but is currently in use (analyzing, open in IDA, or serving) is skipped and named in the log. After `current` moves, open databases keep using the previous build until their clients release them and open the new paths — the log says so, with the list of modules.

## Troubleshooting

| Problem | What to check |
|---|---|
| Workbench reports IDA is not ready | Select both `ida.exe` and `idat.exe` from one supported 9.4+ installation. An old 9.2 path cannot be used. |
| `uvx` is not found | Use **Settings → Official Hex-Rays MCP → Copy uv install command** and run it in PowerShell. Then fully exit and reopen Workbench, or use `Get-Command uvx` to find its installed path and select that executable in Settings. Save and export the updated agent configuration. |
| The first connection times out | uvx may still be downloading the server and its dependencies. Run `uvx --exclude-newer=1s ida-mcp --help` in PowerShell to resolve the package first, then retry. For Codex, its MCP configuration also supports `startup_timeout_sec` if a longer startup allowance is needed. |
| The installer reports a Python environment issue | Run `uvx ida-hcli ida python doctor` and follow its diagnostics for the selected IDA installation. |
| GUI databases are missing, or Open IDA fails | Install the official GUI plugin and restart IDA. Confirm that `IDADIR` in the agent configuration matches Workbench's selected installation. |
| A background database cannot be opened in the GUI | Save and release it from every client currently holding a lease before reopening it with **Open IDA**. |
| The agent still shows old tools or per-library connections | Remove the legacy MCP entries, keep one official server entry, and restart the agent. Updating Workbench alone does not change the agent's configuration. |
| The agent inspects the wrong library or build | Copy **Connect → Selected libraries** again after choosing the required rows and revision/manifest. Ask the agent to use those exact paths. |

Workbench logs are available in its log panel and, by default, `%USERPROFILE%\.ida-workbench\logs`. The official server also provides a session dashboard:

```powershell
uvx ida-mcp dashboard --open
```

For server-side setup and diagnostics, see the [Hex-Rays IDA MCP documentation](https://docs.hex-rays.com/core/mcp).

## Build and validation

Windows builds use Qt 6.8.3 and Visual Studio with CMake/Ninja. The local build scripts contain machine-specific Visual Studio, Qt and vcpkg paths; adjust them for your installation. Use a developer shell with CMake/CTest on PATH:

```powershell
.\build.cmd
ctest --test-dir build --output-on-failure

# Or, with static Qt installed through vcpkg:
.\build-static.cmd
ctest --test-dir build-static --output-on-failure
```

The static build produces one self-contained exe with its analyzer embedded. Windows is the validated platform; Linux paths are retained but do not establish Linux application support.

Three CTest suites cover workspace/revision and Steam behaviour (built-in CS layouts and an arbitrary app with an explicit depot and login), shared-log trimming, and the official MCP client. MCP coverage includes initialization, fragmented messages, server-initiated requests, tool errors, timeouts and late responses, save-before-release, sharing with another client, and release while an open is pending. Tests use fake executables and do not require Steam credentials or an IDA license.

An optional live check uses a disposable test binary and a real IDA installation:

```powershell
build/mcp-tests.exe --live /path/to/uvx /path/to/IDA /path/to/disposable-sample.exe
```

It opens the sample, queries the API reference, waits for analysis, counts functions through IDAPython, saves the database, and releases it. For a dynamic Qt build, run with Qt's bin directory on PATH.

## Publishing a release

Commit the changes, then create and push a version tag from your local console. For example, for the first version 2 release:

```powershell
# Run in your ida-workbench checkout.
git add -A
git commit -m "Use the official Hex-Rays MCP and publish on tag push"
git push origin main
git tag -a v2.0.0 -m "IDA Workbench 2.0.0"
git push origin v2.0.0
```

Use a new version for each release. Tags accept `vMAJOR.MINOR.PATCH` (recommended) or `MAJOR.MINOR.PATCH`. The tag must include the workflow changes; pushing an older commit uses its older workflow. Uncommitted changes are never included in a tag.

Pushing the tag starts **Windows** in GitHub Actions: build the static Qt/Windows x64 EXE, run CTest, check DLL dependencies, attest the binary and upload the artifact. Only after these steps succeed does the publish job create a draft, attach `ida-workbench-2.0.0-windows-x64.exe`, and automatically publish the release with generated notes. No release needs to be created on the website. A cold Qt cache can take hours; subsequent builds reuse cached packages.

The tag supplies the version shown inside the app, in Windows file properties and in the asset filename. Each numeric component must be between 0 and 65535 to fit Windows version metadata. Updating `project(VERSION)` is optional for publishing; it remains the default version for local builds.

**Only pushing a version tag starts the GitHub Actions build.** Ordinary file/commit pushes to `main` or other branches, including uploads through GitHub's website, do not start it. Pull requests and manual workflow dispatches do not start it either. You can commit and push work freely, then push a version tag when it is ready to release. This applies once the updated workflow has been committed and pushed.

A failed build or test prevents release creation; a failed upload leaves an unpublished draft. Use **Re-run failed jobs** on that tag's run to retry. A rerun can finish its draft or skip an already published asset; published releases are never overwritten. Before publishing, the workflow checks that the tag still points at the commit used for the build. For a code fix, commit it and push a new version tag instead of moving the old one.

Created by [@sazonische](https://github.com/sazonische). Not affiliated with Hex-Rays or Valve.
