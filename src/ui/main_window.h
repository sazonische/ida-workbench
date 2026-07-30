#pragma once
#include "manager.h"
#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QMap>
#include <QPair>
#include <QStringList>
#include <QSet>
#include <QVector>
#include <Qt>

class QTableWidget;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QThread;
class QTimer;
class QTabBar;
class QStackedWidget;
class QWidget;
class QSplitter;
class SettingsPanel;
class QCloseEvent;
class QComboBox;
class QGroupBox;
class QPoint;
class QSystemTrayIcon;

class MainWindow : public QMainWindow {
	Q_OBJECT
public:
	explicit MainWindow(QWidget* parent = nullptr);
	~MainWindow() override;

public slots:
	void ShowFromTray();

protected:
	void closeEvent(QCloseEvent* event) override;

signals:
	void RequestRefresh();
	void RequestOpenIda(const Target& target);
	void RequestStart(const QVector<Target>& targets);
	void RequestStop(const QVector<Target>& targets);
	void RequestAnalyze(const QVector<Target>& targets, bool force);
	void RequestStopOperations(const QVector<Target>& targets);
	void RequestReplace(const QVector<Target>& targets);
	void RequestDepotUpdate(const QStringList& tags, const QString& manifest, const QString& username,
		const QString& password, const QString& authCode, bool rememberSession);
	void RequestSaveConfig(const ConfigView& v);
	void RequestImportConfig(const QString& sourcePath);
	void RequestStoredVersion(const QString& tag, const QString& version);
	void RequestUpdateCheck();

private slots:
	void OnStatus(const QVector<LibRow>& rows);
	void OnLog(const QString& msg);
	void OnBusy(bool busy);
	void OnAnalyzeStarted(const Target& target);
	void OnAnalyzeFinished(const Target& target, bool ok);
	void OnOperationChanged(const Target& target, const QString& operation, bool active);
	void OnWorkspaceOperationChanged(const QString& tag, const QString& operation, bool active);
	void OnConfigLoaded(const ConfigView& v);
	void OnReadiness(const Readiness& r);
	void OnUpdateAvailable(const QString& version, const QString& releaseUrl);
	void DoStart();
	void DoOpenIda();
	void DoStop();
	void DoAnalyze();
	void DoReplace();
	void DoDepotUpdate();
	void DoDelete(); // remove the selected libraries from the workbench (config only)
	void OpenSettings();
	void CloseSettings();
	void ToggleAll();
	void AddBinary();		// quick-add a tag-scoped library by file path (an extra library)
	void ShowMcpData();		// dialog with ready-to-paste Claude / Codex MCP client config
	void SetView(int mode); // 0 = flat list, 1 = tabs by tag
	void ApplyFilter();
	void UpdateVersionSelector();
	void UpdateSegmented(int active); // 0=list, 1=tabs, 2=settings
	void ExitFromTray();

private:
	// The toolbar commands, for the shared eligibility rules below.
	enum class Action { Start, OpenIda, Analyze, Replace, Depot, Delete, Stop };

	[[nodiscard]] QVector<Target> SelectedTargets() const;
	// Why `action` cannot touch this target right now; empty string when it can. The
	// single source of truth behind a button's enabled state, its tooltip and the
	// batch the command actually runs on — they can never drift apart.
	[[nodiscard]] QString ActionBlocker(Action action, const Target& target) const;
	// The part of `selection` that `action` applies to. Every rejected target is
	// appended to `skipped` as "name — reason", so a caller can always say what it
	// left out instead of quietly narrowing the user's selection.
	[[nodiscard]] QVector<Target> EligibleTargets(Action action, const QVector<Target>& selection, QStringList* skipped = nullptr) const;
	void LogSkipped(const QString& action, const QStringList& skipped); // audit trail for a partial run
	[[nodiscard]] QString ActiveRevision(const QString& tag) const;
	void RecomputeModel();
	void BuildTable();
	void RebuildRowOf();		// rebuild the (name,tag)->row map after a sort reorders rows
	void CycleSort(int column); // header click: off -> ascending -> descending -> off
	void ApplySort();			// reorder rows by the active (combinable) sort keys
	void UpdateSortHeaders();	// tint each header by its sort state (no arrows)
	void UpdateRowSelection(int row);
	void ShowLibraryContextMenu(const QPoint& position);
	void UpdateActionButtons();				   // enable/disable actions to fit the current selection
	void EditPort(int row);					   // prompt for a new MCP port (double-click on the Port cell)
	void ApplyPortEdit(int row, int newPort);  // persist an in-table port change
	[[nodiscard]] QColor TagColor(const QString& tag) const; // configured tag colour, with a deterministic fallback
	void SaveUiState();						   // persist the log-splitter layout to ui-state.json
	void QueueRefresh();
	void SetupTray();
	[[nodiscard]] int RefreshIntervalMs() const; // brisk while a server is up (or booting), relaxed when idle
	[[nodiscard]] bool InstanceExists(const QString& name, const QString& tag) const;
	void UpdateSpinTimer(); // run the spinner iff any cell is Analyzing… or Starting…

	// Per-instance facts used to decide which actions apply. Keyed by (name,tag)
	// so a column sort (which reorders rows) never invalidates it.
	struct RowState {
		bool present = false;  // an instance exists here
		bool up = false;	   // MCP server answering
		bool hasDb = false;	   // .i64 present (can be served)
		bool localBin = false; // binary present in output or the active revision
		bool srcBin = false;   // binary present in the live source (can Replace/pull)
		bool starting = false; // just launched, IDA still booting (port not up yet)
		bool analyzing = false;
		bool idaOpen = false;
		bool depot = false;
	};
	QHash<QString, RowState> _rowState;

	Manager* _mgr = nullptr;
	QThread* _thread = nullptr;
	ConfigView _cfg;
	QStringList _tags;
	QStringList _names;

	QTableWidget* _table = nullptr;
	QPlainTextEdit* _log = nullptr;
	QLabel* _statServers = nullptr; // header stat pills
	QLabel* _statNeed = nullptr;
	QLabel* _statDiffer = nullptr;
	QTimer* _refreshTimer = nullptr;
	QTimer* _spinTimer = nullptr; // drives the Analyzing… spinner
	int _spinAngle = 0;
	int _spinSec = -1;							  // last elapsed second painted into Analyzing… / Starting… cells
	QHash<QString, QElapsedTimer> _analyzeClocks; // RowKey -> per-library analysis start (live elapsed)
	QHash<QString, QElapsedTimer> _startingClocks; // RowKey -> Start requested; keeps Stop live while IDA boots
	QHash<QString, QString> _activeOperations; // revision-aware TargetKey -> analyze / ida
	QSet<QString> _activeDepotTags;
	QTimer* _refreshSpinTimer = nullptr;		  // spins the Refresh icon during a manual Refresh
	int _refreshAngle = 0;
	QPushButton* _startBtn = nullptr;
	QPushButton* _openIdaBtn = nullptr;
	QPushButton* _stopBtn = nullptr;
	QPushButton* _analyzeBtn = nullptr;
	QPushButton* _replaceBtn = nullptr;
	QPushButton* _depotBtn = nullptr;
	QPushButton* _refreshBtn = nullptr; // icon-only utilities (tooltips carry the names)
	QPushButton* _addBinBtn = nullptr;
	QPushButton* _deleteBtn = nullptr;
	QPushButton* _mcpBtn = nullptr; // "MCP data" — connection info, right-aligned like a Connect button

	QLabel* _chipPython = nullptr; // readiness indicators (text)
	QLabel* _chipIda = nullptr;
	QLabel* _chipMcp = nullptr;
	QLabel* _chipPythonIc = nullptr; // readiness icons
	QLabel* _chipIdaIc = nullptr;
	QLabel* _chipMcpIc = nullptr;
	QLabel* _readyMsg = nullptr;
	bool _ready = false; // gates the action buttons
	bool _busy = false;
	bool _refreshPending = false;
	bool _anyServerUp = false; // drives the adaptive auto-Refresh cadence

	QPushButton* _listBtn = nullptr; // segmented view switcher (top-right)
	QPushButton* _tabsBtn = nullptr;
	QPushButton* _settingsBtn = nullptr;
	QComboBox* _tagCombo = nullptr; // active workspace in Tabs view
	QWidget* _tabsRow = nullptr;
	QComboBox* _versionCombo = nullptr;
	QGroupBox* _versionField = nullptr;
	QHash<QString, QString> _versionByTag;
	int _view = 1;				 // 0 = list, 1 = tabs (HTML reference default)

	QStackedWidget* _stack = nullptr; // main page vs settings page
	QWidget* _mainPage = nullptr;
	QSplitter* _logSplitter = nullptr; // resizable divider between the table and the log
	SettingsPanel* _settingsPanel = nullptr;
	QSystemTrayIcon* _trayIcon = nullptr;
	bool _quitting = false;

	QMap<QString, int> _rowOf; // "name\ttag" -> table row

	// Active sort keys, most-significant first. A header click adds/cycles/removes
	// its column here; empty means the original insertion order.
	QVector<QPair<int, Qt::SortOrder>> _sortKeys;
};
