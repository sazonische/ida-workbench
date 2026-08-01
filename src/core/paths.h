#pragma once
#include <QDir>
#include <QString>

// Every location the app owns, named once instead of spelled out as a string literal at
// each call site — the folder is referenced from main(), the manager and the window, and
// three copies of the same path is how they drift apart.
//
// Two roles live here and are deliberately kept apart. AppHome() holds what the program
// *is*: config.json, ui-state.json, the unpacked helper scripts and their version stamp —
// files it needs to start, and losing one changes behaviour. Logs are what the program
// *emits*: they grow, get trimmed behind your back, are shared with every IDA subprocess
// and are safe to delete at any moment. Mixing the two made the app folder unreadable and
// made "clear the logs" a risky operation, so logs get their own subfolder.
//
// DefaultLogDir() is only where a fresh install starts: `logDir` in config.json can point
// anywhere, and the manager uses whatever it says.
namespace Paths {

	[[nodiscard]] inline QString AppHome() { return QDir::homePath() + "/.ida-workbench"; }

	[[nodiscard]] inline QString ConfigFile() { return QDir(AppHome()).filePath("config.json"); }

	[[nodiscard]] inline QString UiStateFile() { return QDir(AppHome()).filePath("ui-state.json"); }

	[[nodiscard]] inline QString DefaultLogDir() { return QDir(AppHome()).filePath("logs"); }

	// The same folder as DefaultLogDir(), in the tilde form written into config.json so the
	// file stays portable between machines and user names.
	[[nodiscard]] inline QString DefaultLogDirSetting() { return QStringLiteral("~/.ida-workbench/logs"); }

} // namespace Paths
