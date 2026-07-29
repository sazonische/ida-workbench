#pragma once
#include <QString>

// Canonical application log: one human-readable file that captures everything the
// in-app LOG panel shows PLUS the diagnostics it does not — Qt runtime warnings,
// verbose internals emitted via qDebug()/qInfo(), and the MCP server subprocess
// output. Thread-safe; writers open-append-close per line so the file can be
// shared safely between the GUI and the detached IDA server processes.
namespace Log {

	enum class Level { Debug,
					   Info,
					   Warn,
					   Error };

	// Canonical file name kept inside the log directory.
	[[nodiscard]] QString FileName();

	// Point the log at `path`, capped at `maxMB` (0 = uncapped). Creates the folder
	// and trims to the cap. Safe to call again to re-point after a config edit.
	void Configure(const QString& path, int maxMB);
	// Re-check the configured cap without writing a line. Used periodically because
	// detached IDA processes share this file too.
	void EnforceCap();

	// Append one canonical line: "yyyy-MM-dd HH:mm:ss.zzz  LEVEL  message".
	void Write(Level level, const QString& message);

	// A visual separator + "session started" line, written once at startup.
	void SessionBanner(const QString& appName, const QString& version);

	// Absolute path of the active log file (empty until Configure()).
	[[nodiscard]] QString Path();

	// Route qDebug()/qInfo()/qWarning()/qCritical()/qFatal() into the log file.
	void InstallQtMessageHandler();

} // namespace Log
