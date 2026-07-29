#pragma once
#include <QHash>
#include <QJsonObject>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QVector>
#include <atomic>
#include <optional>

struct DepotConfig {
	bool enabled = false;
	int appId = 730;
	QString os = "windows";
	QString manifest;             // last successfully installed manifest GID
	QString patchVersion;
	QString serverVersion;
};

struct DepotDownloaderConfig {
	QString executable = "DepotDownloader.exe";
	int timeoutMinutes = 30;
};

// A tagged source/output workspace with an explicit file manifest.
struct Workspace {
	QString tag;
	QString source;
	QString output;
	QStringList files;
	int portOffset = 0;
	QString color; // explicit tag pill colour (#RRGGBB); generated once when absent
	DepotConfig depot;
};

// A single binary added by explicit path (its own tag; name = file stem).
struct ExtraLib {
	QString tag;
	QString path;
	int port = 0;  // absolute MCP port; 0 = auto (basePort + 1000 + index)
	QString color; // explicit tag pill colour (#RRGGBB); generated once when absent
};

// A hand-picked port for one (tag, library) instance, overriding the auto value.
// Auto workspace port = basePort + fileIndex + tag portOffset; an override pins one
// instance to an exact port and is stored so it survives restarts.
struct PortOverride {
	QString tag;
	QString name;
	int port = 0;
};

// Auto (unedited) MCP ports. The single source of truth for the port formula,
// shared by Manager (RebuildInstances) and the UI (main-table port edits).
inline int AutoScanPort(int basePort, int fileIndex, int tagOffset) { return basePort + fileIndex + tagOffset; }
inline int AutoExtraPort(int basePort, int extraIndex) { return basePort + 1000 + extraIndex; }

struct ConfigView;
// The port a NEW extra library can take without colliding: the auto slot for the
// next extra index, bumped past every port the configuration already occupies.
// Quick-add paths ("Add binary", Settings "Add file…") pass the result as an
// explicit port so adding never trips the collision check merely because the
// auto slot is taken (SaveConfig drops the value again when it equals auto).
int FirstFreeExtraPort(const ConfigView& cfg);

// Best-effort scan of the usual install locations for an IDA install; fills the
// GUI (ida) and text (idat) paths and returns true when a matching pair exists.
// Used to seed a fresh config and by the Settings "Detect IDA" button.
bool AutoDetectIda(QString* guiPath, QString* textPath);

// Editable, OS-resolved snapshot of config.json for the settings dialog.
struct ConfigView {
	QString host;
	QString idaGui, idaText;
	QString logDir;
	QString analysisArgs;	   // optional extra idat switches; empty by default
	QString configPath;		   // where the loaded config.json lives (info only, not saved)
	int maxLogMB = 10;		   // trim the shared log's oldest lines; 0 = unlimited
	int basePort = 8500;	   // base for auto ports; per-library ports derive from it
	DepotDownloaderConfig depotDownloader;
	QVector<Workspace> workspaces;
	QVector<Workspace> steamWorkspaces;
	QVector<ExtraLib> extraLibs;
	QVector<PortOverride> portOverrides; // hand-edited (tag, name) ports for workspace libraries
};

inline QVector<Workspace> AllWorkspaces(const ConfigView& cfg) {
	QVector<Workspace> result = cfg.workspaces;
	result += cfg.steamWorkspaces;
	return result;
}

// A resolved instance: one binary of one library under one tag.
struct Instance {
	QString tag;
	QString name;				// library name (file stem)
	QString binary;				// full path to the active working or revision binary
	QString sourceBinary;		// configured source file, if this is a workspace instance
	QString relativeFile;
	QString outputRoot;
	QString storedVersion;
	bool hasSource = false;
	bool compareSource = false;
	int port = 0;
};

// An immutable action target. An empty revision means the workspace's Current;
// otherwise it is the timestamp/ManifestID selected when the action was issued.
struct Target {
	QString tag;
	QString name;
	QString revision;
};

// Status of one library under one tag (one matrix cell).
struct Cell {
	bool present = false; // an instance exists here at all
	bool up = false;	  // MCP server answering on the port
	bool hasDb = false;	  // .i64 present
	QString state;		  // human label: Ready | Update available | Not analyzed | Re-analyze | No binary | Source missing
	QString tip;		  // detailed multi-line tooltip for the status cell
	qint64 size = -1;	  // analyzed copy size
	qint64 srcSize = -1;  // live-source binary size (if a source is set)
	bool srcDiff = false; // analyzed copy differs (by hash) from its live source
	bool sourceConfigured = false; // Sync applies only to folder workspaces
	int port = 0;		  // effective MCP port for this instance
};

// One matrix row: a library name across all tags (cells aligned to tags order).
struct LibRow {
	QString name;
	QVector<Cell> cells;
};

// Whether the pieces needed to run MCP servers are present.
struct Readiness {
	bool ida = false;	 // ida.exe + idat.exe exist
	bool python = false; // IDA is bound to an existing Python (idapyswitch record)
	bool mcp = false;	 // ida-pro-mcp plugin installed in IDA's plugins dir
	QString idaMsg, pythonMsg, mcpMsg;
	QString pythonLabel; // chip text, e.g. "Python 3.14" (empty -> "IDAPython")
	[[nodiscard]] bool Ready() const { return ida && python && mcp; }
};

class Manager : public QObject {
	Q_OBJECT
public:
	explicit Manager(QObject* parent = nullptr);
	~Manager() override;

	bool LoadConfig(const QString& path, QString* err);
	// Write a clean, loadable default config.json at `path` (auto-detecting IDA),
	// so the app can start in a sane, editable state when no config exists yet.
	bool CreateDefaultConfig(const QString& path, QString* err);
	// Thread-safe stop request for the blocking DepotDownloader operation.
	void RequestStopDepot();
	[[nodiscard]] ConfigView View() const;
	[[nodiscard]] Readiness CurrentReadiness() const { return ComputeReadiness(); } // synchronous, for startup
	[[nodiscard]] QStringList StoredVersions(const QString& tag) const;

public slots:
	void Refresh();
	void OpenIda(const Target& target);
	void Start(const QVector<Target>& targets);
	void Stop(const QVector<Target>& targets);
	void Analyze(const QVector<Target>& targets, bool force);
	void StopOperations(const QVector<Target>& targets);
	void Replace(const QVector<Target>& targets); // pull fresh binary from live source
	void UpdateDepot(const QStringList& tags, const QString& manifest, const QString& username = {},
		const QString& password = {}, const QString& authCode = {}, bool rememberSession = false);
	void SetStoredVersion(const QString& tag, const QString& version);
	void SaveConfig(const ConfigView& v);
	void ImportConfig(const QString& sourcePath); // replace config.json with an external file

signals:
	void Log(const QString& msg);
	void StatusReady(const QVector<LibRow>& rows);
	void AnalyzeStarted(const Target& target);
	void AnalyzeFinished(const Target& target, bool ok); // analyzer exited (ok = fresh DB saved)
	void OperationChanged(const Target& target, const QString& operation, bool active);
	void WorkspaceOperationChanged(const QString& tag, const QString& operation, bool active);
	void ConfigLoaded(const ConfigView& v);
	void ConfigSaveFinished(bool ok, const QString& message);
	void ReadinessChanged(const Readiness& r);

private:
	Readiness ComputeReadiness() const;

	QString _host;
	QString _idaGui, _idaText;
	QString _logDir;
	QString _analysisArgs;
	int _maxLogMB = 10;
	int _basePort = 8500;
	DepotDownloaderConfig _depotDownloader;
	QString _scriptDir;

	QVector<Workspace> _workspaces;
	QVector<ExtraLib> _extraLibs;
	QVector<PortOverride> _portOverrides; // hand-edited (tag, name) scan ports

	QVector<Instance> _instances; // source of truth for all operations
	QStringList _tags;			  // ordered distinct tags
	QStringList _names;			  // ordered distinct names
	QHash<QString, QString> _selectedVersions;
	struct ActiveAnalyze;
	QHash<QString, ActiveAnalyze*> _analyzeJobs;
	QHash<QString, qint64> _idaPids;
	QSet<QString> _depotTags;

	std::atomic<bool> _stopDepotRequested{false}; // polled while DepotDownloader is running

	QString _configPath;

	void RebuildInstances();
	const Instance* InstanceAt(const QString& tag, const QString& name) const;
	std::optional<Instance> ResolveTarget(const Target& target) const;
	QString TargetKey(const Target& target) const;
	bool TargetBusy(const Target& target) const;
	void FinalizeAnalyze(const QString& key);
	void PollIdaProcesses();
	QString StartPy() const;
	QString AnalyzePy() const;

	bool PortUp(int port, int timeoutMs = 800) const;
	qint64 PidOnPort(int port) const;
	bool IsExpectedServerProcess(qint64 pid) const;
	bool KillPid(qint64 pid) const;
	bool LaunchServer(const Instance& inst);

	// Cached SHA-256: re-hashes a file only when its size/mtime changed.
	struct ShaEntry {
		qint64 size = -1;
		qint64 mtime = -1;
		QString hash;
	};
	QHash<QString, ShaEntry> _shaCache;
	QString Sha256(const QString& path);
};

Q_DECLARE_METATYPE(Target)
Q_DECLARE_METATYPE(QVector<Target>)
Q_DECLARE_METATYPE(LibRow)
Q_DECLARE_METATYPE(QVector<LibRow>)
Q_DECLARE_METATYPE(ConfigView)
Q_DECLARE_METATYPE(Readiness)
