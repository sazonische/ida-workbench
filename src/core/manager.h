#pragma once
#include <QHash>
#include <QJsonObject>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <optional>

// Any Steam app works: a workspace names its depot explicitly. Known games also
// have a built-in layout, used when no DepotID is given.
struct DepotConfig {
	bool enabled = false;
	int appId = 730;
	QString os = "windows";		// DepotDownloader -os filter; empty = any
	int depotId = 0;			// 0 = the built-in depot for appId/os
	QString pathPrefix;			// depot folder the workspace's files are relative to
	bool requiresLogin = false; // even Latest needs a Steam account that owns the app
	QString manifest;			// last successfully installed manifest GID
	QString patchVersion;
	QString serverVersion;
};

struct DepotLayout {
	int depotId = 0;
	QString pathPrefix;
	QString versionFile;		// workspace-relative steam.inf carrying PatchVersion/ServerVersion
	QString versionFallbackUrl; // public copy of versionFile when the depot does not ship it
	bool accountRequired = false;
};

// Built-in layouts of known games; CS2 and standalone CS:GO keep distinct depots.
DepotLayout DepotLayoutFor(int appId, const QString& os);
// A workspace's effective layout, shared by validation, downloading and the UI.
DepotLayout DepotLayoutFor(const DepotConfig& depot);
// Reads PatchVersion/ServerVersion from a steam.inf; missing keys leave the outputs untouched.
void ReadSteamVersions(const QString& steamInfPath, QString* patchVersion, QString* serverVersion);

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
	QString color; // explicit tag pill colour (#RRGGBB); generated once when absent
	DepotConfig depot;
};

// A single binary added by explicit path (its own tag; name = file stem).
struct ExtraLib {
	QString tag;
	QString path;
	QString color; // explicit tag pill colour (#RRGGBB); generated once when absent
};

// Best-effort scan of the usual install locations for an IDA install; fills the
// GUI (ida) and text (idat) paths and returns true when a matching pair exists.
// Used to seed a fresh config and by the Settings "Detect IDA" button.
bool AutoDetectIda(QString* guiPath, QString* textPath);

// Editable, OS-resolved snapshot of config.json for the settings dialog.
struct ConfigView {
	QString uvx; // executable used to launch the official Hex-Rays MCP server
	QString idaGui, idaText;
	QString logDir;
	QString analysisArgs; // optional extra idat switches; empty by default
	QString configPath;	  // where the loaded config.json lives (info only, not saved)
	int maxLogMB = 10;	  // trim the shared log's oldest lines; 0 = unlimited
	DepotDownloaderConfig depotDownloader;
	QVector<Workspace> workspaces;
	QVector<Workspace> steamWorkspaces;
	QVector<ExtraLib> extraLibs;
};

inline QVector<Workspace> AllWorkspaces(const ConfigView& cfg) {
	QVector<Workspace> result = cfg.workspaces;
	result += cfg.steamWorkspaces;
	return result;
}

// A resolved instance: one binary of one library under one tag.
struct Instance {
	QString tag;
	QString name;		  // library name (file stem)
	QString binary;		  // full path to the active working or revision binary
	QString sourceBinary; // configured source file, if this is a workspace instance
	QString relativeFile;
	QString outputRoot;
	QString storedVersion;
	bool hasSource = false;
	bool compareSource = false;
};

// An immutable action target. An empty revision means the workspace's Current;
// otherwise it is the timestamp/ManifestID selected when the action was issued.
struct Target {
	QString tag;
	QString name;
	QString revision;
};

// The one target -> binary mapping, shared by every operation and Connect's export.
std::optional<Instance> ResolveInstance(const QVector<Workspace>& workspaces, const QVector<ExtraLib>& extraLibs, const Target& target);

// The file IDA opens for a binary: its .i64 when analyzed, otherwise the binary itself.
QString DatabaseInput(const QString& binary);

// Status of one library under one tag (one matrix cell).
struct Cell {
	bool present = false; // an instance exists here at all
	bool up = false;	  // visible through IDA Nexus
	bool owned = false;	  // Workbench holds a lease
	bool opening = false;
	QString backend;
	QString instanceId;
	bool hasDb = false;			   // .i64 present
	QString state;				   // human label: Ready | Update available | Not analyzed | Re-analyze | No binary | Source missing
	QString tip;				   // detailed multi-line tooltip for the status cell
	qint64 size = -1;			   // analyzed copy size
	qint64 srcSize = -1;		   // live-source binary size (if a source is set)
	bool srcDiff = false;		   // analyzed copy differs (by hash) from its live source
	bool sourceConfigured = false; // Sync applies only to folder workspaces
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
	bool mcp = false;	 // official Hex-Rays GUI plugin and uvx are present
	QString idaMsg, pythonMsg, mcpMsg;
	QString pythonLabel; // chip text, e.g. "Python 3.14" (empty -> "IDAPython")
	[[nodiscard]] bool Ready() const { return ida && python && mcp; }
};

class McpClient;

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
	void UpdateDepot(const QStringList& tags, const QString& manifest, const QString& username = {}, const QString& password = {}, const QString& authCode = {}, bool rememberSession = false);
	void SetStoredVersion(const QString& tag, const QString& version);
	// Ask GitHub for the newest release. Emits UpdateAvailable only when it is newer than
	// the running build; everything else (offline, rate limit, malformed answer) is a log
	// line, because a background check must never look like a problem with the app.
	void CheckForUpdate();
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
	void UpdateAvailable(const QString& version, const QString& releaseUrl);
	void ReadinessChanged(const Readiness& r);

private:
	Readiness ComputeReadiness() const;
	void FinishUpdateCheck(const QByteArray& releaseJson); // parse + compare, once the reply is in

	QString _uvx;
	QString _idaGui, _idaText;
	QString _logDir;
	QString _analysisArgs;
	int _maxLogMB = 10;
	DepotDownloaderConfig _depotDownloader;
	QString _scriptDir;

	QVector<Workspace> _workspaces;
	QVector<ExtraLib> _extraLibs;

	QVector<Instance> _instances; // source of truth for all operations
	QStringList _tags;			  // ordered distinct tags
	QStringList _names;			  // ordered distinct names
	QHash<QString, QString> _selectedVersions;
	struct ActiveAnalyze;
	QHash<QString, ActiveAnalyze*> _analyzeJobs;
	QSet<QString> _depotTags;

	std::atomic<bool> _stopDepotRequested{false}; // polled while DepotDownloader is running
	bool _updateCheckInFlight = false;			  // one release query at a time

	QString _configPath;

	void RebuildInstances();
	const Instance* InstanceAt(const QString& tag, const QString& name) const;
	std::optional<Instance> ResolveTarget(const Target& target) const;
	QString TargetKey(const Target& target) const;
	bool TargetBusy(const Target& target) const;
	bool AnalyzingDatabase(const QString& key) const; // key = DatabaseKey of the binary
	bool DatabaseOpen(const QString& key) const;	  // listed by Nexus or leased by Workbench
	void FinalizeAnalyze(const QString& key);
	void PublishStatus();
	void PollDatabases();
	void EnsureMcp();
	void OpenDatabase(const Target& target, bool gui);
	void ReleaseDatabase(const QString& path);
	QString AnalyzePy() const;
	McpClient* _mcp = nullptr;
	QHash<QString, QJsonObject> _databases;
	QHash<QString, QString> _leases;
	QSet<QString> _opening;
	QSet<QString> _releaseRequested;
	bool _listing = false;
	quint64 _databaseEpoch = 0;
	bool _shuttingDown = false;

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
