#include "manager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

namespace {
bool Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
	}
	return condition;
}

bool WriteFile(const QString& path, const QByteArray& bytes) {
	if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
		return false;
	}
	QFile file(path);
	return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray ReadFile(const QString& path) {
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
} // namespace

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	if (!Check(app.arguments().size() >= 3, "fake helper paths supplied")) {
		return 1;
	}
	const QString fakeDepot = app.arguments().at(1);
	const QString fakeAnalyzer = app.arguments().at(2);
	QTemporaryDir temp;
	if (!Check(temp.isValid(), "temporary directory creation")) {
		return 1;
	}

	const QString source = temp.filePath("game");
	const QString output = temp.filePath("ida");
	const QString relative = "csgo/bin/win64/client.dll";
	const QString sourceBinary = QDir(source).filePath(relative);
	const QString outputBinary = QDir(output).filePath(relative);
	const QString logDir = temp.filePath("logs");
	if (!Check(WriteFile(sourceBinary, "new-binary"), "source fixture")) {
		return 1;
	}
	if (!Check(WriteFile(outputBinary, "old-binary"), "working binary fixture")) {
		return 1;
	}
	if (!Check(WriteFile(outputBinary + ".i64", "old-database"), "working database fixture")) {
		return 1;
	}

	QJsonObject root{
		{"host", "127.0.0.1"},
		{"ida", QJsonObject{{"gui", ""}, {"text", ""}}},
		{"logDir", logDir},
		{"scanBasePort", 59000},
		{"maxLogSizeMB", 10},
		{"workspaces", QJsonArray{QJsonObject{
			{"tag", "current"}, {"source", source}, {"output", output},
			{"files", QJsonArray{relative}}
		}}},
		{"extraLibs", QJsonArray{}}
	};
	const QString configPath = temp.filePath("config.json");
	if (!Check(WriteFile(configPath, QJsonDocument(root).toJson()), "config fixture")) {
		return 1;
	}

	Manager manager;
	QString error;
	if (!Check(manager.LoadConfig(configPath, &error), qPrintable(error))) {
		return 1;
	}
	if (!Check(manager.View().workspaces.size() == 1, "workspace parsed")) {
		return 1;
	}
	if (!Check(manager.View().workspaces.first().files == QStringList{relative}, "relative files parsed")) {
		return 1;
	}
	if (!Check(!manager.View().workspaces.first().color.isEmpty(), "missing tag color generated")) {
		return 1;
	}
	if (!Check(!QJsonDocument::fromJson(ReadFile(configPath)).object().value("workspaces").toArray().first().toObject().value("color").toString().isEmpty(), "generated color persisted during load")) {
		return 1;
	}
	if (!Check(QDir(QDir(output).filePath("revisions")).exists(), "revisions root created on load")) {
		return 1;
	}
	if (!Check(QFile::exists(outputBinary) && QFile::exists(outputBinary + ".i64"), "working files preserve relative paths")) {
		return 1;
	}

	bool saveOk = false;
	QObject::connect(&manager, &Manager::ConfigSaveFinished, &app, [&](bool ok, const QString&) { saveOk = ok; });
	ConfigView edited = manager.View();
	edited.host = "localhost";
	edited.depotDownloader.executable = fakeDepot;
	manager.SaveConfig(edited);
	if (!Check(saveOk, "new config saves")) {
		return 1;
	}
	const QJsonObject saved = QJsonDocument::fromJson(ReadFile(configPath)).object();
	if (!Check(saved.value("workspaces").toArray().size() == 1, "workspaces saved")) {
		return 1;
	}
	if (!Check(saved.value("ida").toObject().value("win").isUndefined(), "platform branches removed")) {
		return 1;
	}
	const QJsonObject savedWorkspace = saved.value("workspaces").toArray().first().toObject();
	if (!Check(!savedWorkspace.value("color").toString().isEmpty(), "tag color is always explicit")) {
		return 1;
	}
	if (!Check(!savedWorkspace.contains("depot") && saved.value("steamWorkspaces").toArray().isEmpty(), "folder and Steam mechanisms are separate")) {
		return 1;
	}
	if (!Check(saved.value("depotDownloader").toObject().value("executable").toString() == fakeDepot, "downloader settings saved")) {
		return 1;
	}

	QVector<LibRow> rows;
	QObject::connect(&manager, &Manager::StatusReady, &app, [&](const QVector<LibRow>& value) { rows = value; });
	manager.Refresh();
	if (!Check(rows.size() == 1 && rows.first().cells.first().srcDiff, "source path resolves and differs")) {
		return 1;
	}

	manager.Replace(QVector<Target>{{"current", "client"}});
	if (!Check(ReadFile(outputBinary) == QByteArray("new-binary"), "replace copied source binary")) {
		return 1;
	}
	if (!Check(!QFile::exists(outputBinary + ".i64"), "stale database removed")) {
		return 1;
	}

	const QStringList revisions = manager.StoredVersions("current");
	if (!Check(revisions.size() == 1, "one timestamped revision created")) {
		return 1;
	}
	const QString revisionRoot = QDir(output).filePath("revisions/" + revisions.first());
	if (!Check(ReadFile(QDir(revisionRoot).filePath(relative)) == QByteArray("old-binary"), "old binary archived with relative path")) {
		return 1;
	}
	if (!Check(ReadFile(QDir(revisionRoot).filePath(relative + ".i64")) == QByteArray("old-database"), "old database archived with relative path")) {
		return 1;
	}

	manager.SetStoredVersion("current", revisions.first());
	manager.Refresh();
	if (!Check(rows.first().cells.first().size == QByteArray("old-binary").size(), "selected revision is active")) {
		return 1;
	}
	manager.SetStoredVersion("current", QString());

	ConfigView withSteam = manager.View();
	Workspace steam = withSteam.workspaces.takeFirst();
	const QString steamDir = temp.filePath("steam-manifests");
	steam.source = steamDir;
	steam.output = steamDir;
	steam.source = steam.output;
	const QString unchangedRelative = "bin/win64/engine2.dll";
	const QString absentRelative = "csgo/bin/win64/modtools.dll";
	const QByteArray unchangedDepotBytes = QByteArray("depot:game/") + unchangedRelative.toUtf8();
	steam.files << unchangedRelative;
	steam.files << absentRelative;
	steam.depot.enabled = true;
	steam.depot.appId = 730;
	steam.depot.os = "windows";
	steam.depot.manifest = "3333333333333333333";
	steam.depot.patchVersion = "1.41.7.1";
	steam.depot.serverVersion = "2000800";
	const QString previousRoot = QDir(steamDir).filePath(steam.depot.manifest);
	if (!Check(WriteFile(QDir(previousRoot).filePath(relative), "previous-client"), "previous Steam client fixture")) {
		return 1;
	}
	if (!Check(WriteFile(QDir(previousRoot).filePath(unchangedRelative), unchangedDepotBytes), "unchanged Steam binary fixture")) {
		return 1;
	}
	if (!Check(WriteFile(QDir(previousRoot).filePath(unchangedRelative + ".i64"), "unchanged-db"), "unchanged Steam database fixture")) {
		return 1;
	}
	if (!Check(WriteFile(QDir(previousRoot).filePath("csgo/steam.inf"), "PatchVersion=1.41.7.1\nServerVersion=2000800\n"), "previous Steam version fixture")) {
		return 1;
	}
	withSteam.steamWorkspaces << steam;
	manager.SaveConfig(withSteam);
	if (!Check(saveOk, "Steam workspace saves")) {
		return 1;
	}
	const QJsonObject steamSaved = QJsonDocument::fromJson(ReadFile(configPath)).object();
	if (!Check(steamSaved.value("workspaces").toArray().isEmpty(), "folder workspace removed independently")) {
		return 1;
	}
	const QJsonObject savedSteam = steamSaved.value("steamWorkspaces").toArray().first().toObject();
	if (!Check(savedSteam.value("dir").toString() == steamDir && savedSteam.value("appId").toInt() == 730, "single-directory Steam settings saved")) {
		return 1;
	}
	if (!Check(savedSteam.value("current").isString() && savedSteam.value("current").toString() == "3333333333333333333", "Steam Current is stored as one ManifestID")) {
		return 1;
	}

	manager.UpdateDepot({"current"}, "latest");
	const QByteArray depotBytes = QByteArray("depot:game/") + relative.toUtf8();
	const QString currentRoot = QDir(steamDir).filePath("2222222222222222222");
	const QString steamOutputBinary = QDir(currentRoot).filePath(relative);
	if (!Check(ReadFile(steamOutputBinary) == depotBytes, "depot update installed configured file under ManifestID")) {
		return 1;
	}
	if (!Check(!QFile::exists(QDir(currentRoot).filePath(absentRelative)), "path absent from depot does not block Current manifest")) {
		return 1;
	}
	if (!Check(ReadFile(QDir(currentRoot).filePath("csgo/steam.inf")).contains("PatchVersion=1.41.7.2"), "steam.inf stored inside ManifestID")) {
		return 1;
	}
	const ConfigView afterDepot = manager.View();
	if (!Check(afterDepot.steamWorkspaces.first().depot.manifest == "2222222222222222222", "installed manifest persisted")) {
		return 1;
	}
	if (!Check(!rows.first().cells.first().sourceConfigured, "Steam workspace does not expose a folder Source status")) {
		return 1;
	}
	if (!Check(afterDepot.steamWorkspaces.first().depot.patchVersion == "1.41.7.2", "PatchVersion derived")) {
		return 1;
	}
	if (!Check(afterDepot.steamWorkspaces.first().depot.serverVersion == "2000877", "ServerVersion derived")) {
		return 1;
	}
	const QJsonObject updatedSteam = QJsonDocument::fromJson(ReadFile(configPath)).object().value("steamWorkspaces").toArray().first().toObject();
	if (!Check(updatedSteam.value("current").toString() == "2222222222222222222", "updated config stores only the Current ManifestID")) {
		return 1;
	}
	if (!Check(ReadFile(QDir(previousRoot).filePath(unchangedRelative + ".i64")) == QByteArray("unchanged-db"), "IDA database remains attached to previous ManifestID")) {
		return 1;
	}
	if (!Check(manager.StoredVersions("current") == QStringList{"3333333333333333333"}, "previous ManifestID is exposed as a stored version")) {
		return 1;
	}
	manager.UpdateDepot({"current"}, "latest");
	if (!Check(manager.StoredVersions("current").size() == 1, "same manifest creates no duplicate directory")) {
		return 1;
	}

	// An interrupted download leaves the Current directory incomplete. Completing it must
	// add the missing modules *in place*: everything already there keeps its database and
	// its identity, which is what lets one module stay served (or open in IDA) while the
	// rest of the workspace is downloaded.
	const QString keptDatabase = steamOutputBinary + ".i64";
	const QString unrelatedFile = QDir(currentRoot).filePath("bin/win64/notes.txt");
	if (!Check(WriteFile(keptDatabase, "current-db") && WriteFile(unrelatedFile, "keep-me"), "installed manifest fixtures")) {
		return 1;
	}
	if (!Check(QFile::remove(QDir(currentRoot).filePath(unchangedRelative)), "interrupted download fixture")) {
		return 1;
	}
	manager.UpdateDepot({"current"}, "latest");
	if (!Check(ReadFile(QDir(currentRoot).filePath(unchangedRelative)) == unchangedDepotBytes, "incomplete manifest is completed")) {
		return 1;
	}
	if (!Check(ReadFile(keptDatabase) == QByteArray("current-db"), "completion keeps the database of an already installed module")) {
		return 1;
	}
	if (!Check(ReadFile(unrelatedFile) == QByteArray("keep-me"), "completion adds files instead of rebuilding the directory")) {
		return 1;
	}
	if (!Check(manager.StoredVersions("current").size() == 1 &&
				  manager.View().steamWorkspaces.first().depot.manifest == "2222222222222222222",
			"completion neither duplicates nor moves Current")) {
		return 1;
	}
	manager.UpdateDepot({"current"}, "1111111111111111111", "test-user", "test-password", "12345", true);
	if (!Check(ReadFile(steamOutputBinary) == depotBytes, "historical import does not replace Current")) {
		return 1;
	}
	if (!Check(manager.View().steamWorkspaces.first().depot.manifest == "2222222222222222222", "historical import leaves Current manifest unchanged")) {
		return 1;
	}
	if (!Check(manager.StoredVersions("current").size() == 2, "historical manifest creates one directory")) {
		return 1;
	}
	const QString historicalRoot = QDir(steamDir).filePath("1111111111111111111");
	if (!Check(ReadFile(QDir(historicalRoot).filePath(relative)) == QByteArray("old-depot:game/") + relative.toUtf8(), "historical ManifestID directory stores requested files")) {
		return 1;
	}
	for (const QString& entry : QDir(steamDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
		if (!Check(QRegularExpression("^[0-9]+$").match(entry).hasMatch(), "Steam root contains only ManifestID directories")) {
			return 1;
		}
	}
	manager.SetStoredVersion("current", "1111111111111111111");
	manager.Refresh();
	if (!Check(rows.first().cells.first().size == QByteArray("old-depot:game/").size() + relative.toUtf8().size(), "selected ManifestID is the active analysis root")) {
		return 1;
	}
	manager.SetStoredVersion("current", QString());
	manager.UpdateDepot({"current"}, "1111111111111111111");
	if (!Check(manager.StoredVersions("current").size() == 2, "historical manifest is not imported twice")) {
		return 1;
	}
	manager.Replace(QVector<Target>{{"current", "client"}});
	if (!Check(ReadFile(steamOutputBinary) == depotBytes, "manual Replace cannot mutate a Steam workspace")) {
		return 1;
	}

	ConfigView withAnalyzer = manager.View();
	withAnalyzer.idaText = fakeAnalyzer;
	manager.SaveConfig(withAnalyzer);
	if (!Check(saveOk, "fake analyzer path saves")) {
		return 1;
	}
	int started = 0;
	int finished = 0;
	QHash<QString, bool> outcomes;
	QEventLoop analysisLoop;
	QObject::connect(&manager, &Manager::AnalyzeStarted, &app, [&](const Target&) { ++started; });
	QObject::connect(&manager, &Manager::AnalyzeFinished, &app, [&](const Target& target, bool ok) {
		outcomes[target.name] = ok;
		if (++finished >= 2) {
			analysisLoop.quit();
		}
	});
	QElapsedTimer dispatchTime;
	dispatchTime.start();
	manager.Analyze({{"current", "client"}, {"current", "engine2"}}, true);
	if (!Check(dispatchTime.elapsed() < 1500, "analyze dispatch is non-blocking")) {
		return 1;
	}
	if (!Check(started == 2, "independent analyses launch together")) {
		return 1;
	}
	QTimer::singleShot(5000, &analysisLoop, &QEventLoop::quit);
	analysisLoop.exec();
	if (!Check(finished == 2 && outcomes.value("client") && outcomes.value("engine2"), "parallel analyses finish independently")) {
		return 1;
	}

	started = 0;
	finished = 0;
	outcomes.clear();
	manager.Analyze({{"current", "client"}, {"current", "engine2"}}, true);
	manager.StopOperations({{"current", "client"}});
	QTimer::singleShot(5000, &analysisLoop, &QEventLoop::quit);
	analysisLoop.exec();
	if (!Check(finished == 2 && !outcomes.value("client") && outcomes.value("engine2"), "stop affects only the addressed analysis")) {
		return 1;
	}

	ConfigView invalid = manager.View();
	invalid.steamWorkspaces << invalid.steamWorkspaces.first();
	saveOk = true;
	manager.SaveConfig(invalid);
	if (!Check(!saveOk, "duplicate tag rejected")) {
		return 1;
	}
	ConfigView unsupported = manager.View();
	unsupported.steamWorkspaces.first().depot.appId = 999;
	saveOk = true;
	manager.SaveConfig(unsupported);
	if (!Check(!saveOk, "unsupported AppID/OS mapping rejected")) {
		return 1;
	}

	Manager fresh;
	const QString freshPath = temp.filePath("fresh/config.json");
	if (!Check(fresh.CreateDefaultConfig(freshPath, &error), qPrintable(error))) {
		return 1;
	}
	const QJsonObject freshJson = QJsonDocument::fromJson(ReadFile(freshPath)).object();
	if (!Check(freshJson.value("workspaces").toArray().isEmpty(), "default has empty workspaces")) {
		return 1;
	}
	if (!Check(freshJson.value("steamWorkspaces").toArray().isEmpty(), "default has empty Steam workspaces")) {
		return 1;
	}

	qInfo() << "manager config tests passed";
	return 0;
}
