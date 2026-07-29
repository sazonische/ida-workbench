#pragma once
#include "manager.h"
#include <QWidget>

class QLineEdit;
class QTableWidget;
class QListWidget;

// Inline settings view (embedded in the main window's stacked area, not a dialog).
class SettingsPanel : public QWidget {
	Q_OBJECT
public:
	explicit SettingsPanel(const ConfigView& v, QWidget* parent = nullptr);

signals:
	void Saved(const ConfigView& v);
	void Cancelled();
	void ImportRequested(const QString& file); // user picked a config file to import

private:
	ConfigView Values() const;
	void ImportConfigFile(); // pick + confirm a config file, then emit ImportRequested
	void ExportConfigFile(); // copy the saved config.json to a picked location
	void AddWorkspaceRow(const Workspace& workspace);
	void AddSteamWorkspaceRow(const Workspace& workspace);
	void AddExtraRow(const ExtraLib& e);
	void AddWorkspace();
	void AddSteamWorkspace();
	void EditFiles(QTableWidget* table, int row);
	void EditDepot(int row);
	void PickTagColor(int row);
	void PickExtraColor(int row);
	void AddExtraFile();

	QLineEdit* _host = nullptr;
	QLineEdit* _gui = nullptr;
	QLineEdit* _text = nullptr;
	QLineEdit* _logDir = nullptr;
	QLineEdit* _analysisArgs = nullptr;
	QLineEdit* _basePort = nullptr;
	QLineEdit* _maxLog = nullptr; // shared log size cap in MB; 0 = unlimited
	QLineEdit* _depotExecutable = nullptr;
	QLineEdit* _depotTimeout = nullptr;
	QTableWidget* _workspaces = nullptr;
	QTableWidget* _steamWorkspaces = nullptr;
	QTableWidget* _extra = nullptr;
	QString _configPath; // where config.json lives (info only, passed through)

	// Carried through to Values() unchanged; edited on the main page.
	QVector<PortOverride> _portOverrides;
};
