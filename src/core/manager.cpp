#include "manager.h"

#include "logging.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QStandardPaths>
#include <algorithm>
#include <future>
#include <memory>
#include <vector>

#ifndef Q_OS_WIN
	#include <csignal>
#endif

#ifdef Q_OS_WIN
	#define NOMINMAX
	#include <windows.h>
	#include <iphlpapi.h>
#endif

// Base for scan-library ports: port = scanBasePort + nameIndex + tag portOffset.
// Extra libraries use scanBasePort + 1000 + index. Editable via "scanBasePort".
static const int DEFAULT_BASE_PORT = 8500;

// The whole app shares ONE canonical log file (see logging.h): the in-app panel
// messages, verbose diagnostics, and the MCP server subprocess output all land in
// it, trimmed to the "maxLogSizeMB" cap. 0 disables the cap entirely.
static const int DEFAULT_MAX_LOG_MB = 10;
static const int MAX_LOG_MB_CEILING = 10240;

struct DepotLayout {
	int depotId = 0;
	QString pathPrefix;
};

static DepotLayout depotLayout(int appId, const QString& os) {
	if (appId == 730) {
		if (os.compare("windows", Qt::CaseInsensitive) == 0) return {2347771, "game"};
		if (os.compare("linux", Qt::CaseInsensitive) == 0) return {2347773, "game"};
	}
	return {};
}

static QString generatedTagColor(const QString& tag) {
	static const char* colors[] = {"#EADDFF", "#FFD8E4", "#D3EFD5", "#D3E3FD", "#FFEFD2", "#CCE8E6", "#F0DBF5", "#E3E9C8"};
	quint32 hash = 2166136261u;
	for (const QChar c : tag) { hash ^= c.unicode(); hash *= 16777619u; }
	return colors[hash % (sizeof(colors) / sizeof(colors[0]))];
}

static QString expand(const QString& p) {
	if (p == "~") return QDir::homePath();
	if (p.startsWith("~/") || p.startsWith("~\\")) return QDir::homePath() + "/" + p.mid(2);
	return p;
}

static QString resolveExecutable(const QString& configured) {
	const QString value = QDir::fromNativeSeparators(configured.trimmed());
	if (value.isEmpty()) return {};
	if (QFileInfo(value).isFile()) return QFileInfo(value).absoluteFilePath();
	if (!QFileInfo(value).isAbsolute() && !value.contains('/')) return QStandardPaths::findExecutable(value);
	return {};
}

static bool downloadUrl(const QUrl& url, int timeoutMs, QByteArray* data, QString* error) {
	QNetworkAccessManager network;
	QNetworkRequest request(url);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	QNetworkReply* reply = network.get(request);
	QEventLoop loop;
	QTimer timer;
	timer.setSingleShot(true);
	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	QObject::connect(&timer, &QTimer::timeout, reply, &QNetworkReply::abort);
	timer.start(timeoutMs);
	loop.exec();
	const QByteArray body = reply->readAll();
	const QString networkError = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
	reply->deleteLater();
	if (!networkError.isEmpty() || body.isEmpty()) {
		if (error) *error = networkError.isEmpty() ? "empty response" : networkError;
		return false;
	}
	if (data) *data = body;
	return true;
}

static bool bootstrapDepotDownloader(const QString& configPath, QString* executable, QString* error) {
#ifdef Q_OS_WIN
	const QString asset = "DepotDownloader-windows-x64.zip";
	const QString binaryName = "DepotDownloader.exe";
#elif defined(Q_OS_LINUX)
	const QString asset = QSysInfo::currentCpuArchitecture().contains("arm64") ?
		"DepotDownloader-linux-arm64.zip" : "DepotDownloader-linux-x64.zip";
	const QString binaryName = "DepotDownloader";
#else
	if (error) *error = "automatic DepotDownloader installation is supported on Windows and Linux only";
	return false;
#endif
	const QString tools = QDir(QFileInfo(configPath).absolutePath()).filePath("tools/depotdownloader");
	const QString binary = QDir(tools).filePath(binaryName);
	if (QFileInfo(binary).isFile()) { *executable = QFileInfo(binary).absoluteFilePath(); return true; }
	if (!QDir().mkpath(tools)) {
		if (error) *error = QString("cannot create tools directory: %1").arg(tools);
		return false;
	}

	const QUrl url(QString("https://github.com/SteamRE/DepotDownloader/releases/download/DepotDownloader_3.4.0/%1").arg(asset));
	QByteArray archive;
	QString networkError;
	if (!downloadUrl(url, 120000, &archive, &networkError)) {
		if (error) *error = QString("download failed: %1").arg(networkError);
		return false;
	}
	const QString zipPath = QDir(tools).filePath("DepotDownloader.zip");
	QSaveFile zip(zipPath);
	if (!zip.open(QIODevice::WriteOnly) || zip.write(archive) != archive.size() || !zip.commit()) {
		if (error) *error = QString("cannot save downloaded archive: %1").arg(zip.errorString());
		return false;
	}

	QProcess extract;
#ifdef Q_OS_WIN
	auto psQuote = [](QString value) { return value.replace("'", "''"); };
	const QString command = QString("$ErrorActionPreference='Stop'; Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force")
		.arg(psQuote(zipPath), psQuote(tools));
	extract.start("powershell.exe", {"-NoProfile", "-NonInteractive", "-Command", command});
#else
	extract.start("unzip", {"-o", zipPath, "-d", tools});
#endif
	bool extractionFinished = extract.waitForStarted(10000);
	QElapsedTimer extractionClock;
	extractionClock.start();
	while (extractionFinished && !extract.waitForFinished(250)) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		if (extractionClock.hasExpired(120000)) {
			extract.kill();
			extract.waitForFinished(3000);
			extractionFinished = false;
		}
	}
	if (!extractionFinished || extract.exitStatus() != QProcess::NormalExit || extract.exitCode() != 0) {
		if (error) *error = QString("archive extraction failed: %1").arg(QString::fromUtf8(extract.readAllStandardError()).trimmed());
		QFile::remove(zipPath);
		return false;
	}
	QFile::remove(zipPath);
#ifndef Q_OS_WIN
	QFile::setPermissions(binary, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
		QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther);
#endif
	if (!QFileInfo(binary).isFile()) {
		if (error) *error = QString("archive does not contain %1").arg(binaryName);
		return false;
	}
	*executable = QFileInfo(binary).absoluteFilePath();
	return true;
}

bool AutoDetectIda(QString* guiPath, QString* textPath) {
#ifdef Q_OS_WIN
	const QString guiName = "ida.exe", textName = "idat.exe";
	QStringList roots;
	for (const char* env : {"ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"}) {
		const QString r = qEnvironmentVariable(env);
		if (!r.isEmpty()) roots << r;
	}
	const QString local = qEnvironmentVariable("LOCALAPPDATA");
	if (!local.isEmpty()) roots << local + "/Programs";
#else
	const QString guiName = "ida", textName = "idat";
	QStringList roots = {QStringLiteral("/opt"), QDir::homePath(), QDir::homePath() + "/.local", QStringLiteral("/usr/local")};
#endif
	roots.removeDuplicates();

	// Collect every "IDA*" directory across the roots, then prefer the one that
	// sorts highest (roughly the newest version) and holds BOTH binaries.
	QStringList candidates;
	for (const QString& root : roots) {
		QDir d(root);
		if (!d.exists()) continue;
		for (const QString& sub : d.entryList({"IDA*", "ida*"}, QDir::Dirs | QDir::NoDotAndDotDot))
			candidates << d.filePath(sub);
	}
	candidates.removeDuplicates();
	std::sort(candidates.begin(), candidates.end(), [](const QString& a, const QString& b) {
		return a.compare(b, Qt::CaseInsensitive) < 0;
	});
	for (int i = candidates.size() - 1; i >= 0; --i) {
		const QString g = QDir(candidates[i]).filePath(guiName);
		const QString t = QDir(candidates[i]).filePath(textName);
		if (QFile::exists(g) && QFile::exists(t)) {
			if (guiPath) *guiPath = QDir::toNativeSeparators(g);
			if (textPath) *textPath = QDir::toNativeSeparators(t);
			return true;
		}
	}
	return false;
}

static QString validateConfig(const ConfigView& cfg) {
	// IDA paths and the library set are deliberately NOT required here: a brand
	// new install has neither. Missing IDA shows as a red readiness chip and
	// gates the actions; an empty library set is a valid "nothing added yet"
	// state. Only field formats that would corrupt the file are enforced.
	if (cfg.host.trimmed().isEmpty()) return "Host must not be empty.";
	if (cfg.logDir.trimmed().isEmpty()) return "Log directory must not be empty.";
	if (cfg.maxLogMB < 0 || cfg.maxLogMB > MAX_LOG_MB_CEILING)
		return QString("Max log size must be 0 (unlimited) to %1 MB.").arg(MAX_LOG_MB_CEILING);

	QSet<QString> tags, instances;
	QSet<int> ports;
	auto overrideFor = [&cfg](const QString& tag, const QString& name) -> int {
		for (const PortOverride& po : cfg.portOverrides)
			if (po.tag == tag && po.name == name) return po.port;
		return 0;
	};
	for (const Workspace& workspace : AllWorkspaces(cfg)) {
		const QString tag = workspace.tag.trimmed();
		if (tag.isEmpty()) return "Every workspace needs a tag.";
		if (!QRegularExpression("^#[0-9A-Fa-f]{6}$").match(workspace.color.trimmed()).hasMatch())
			return QString("Workspace '%1' needs an explicit #RRGGBB color.").arg(tag);
		if (workspace.source.trimmed().isEmpty())
			return workspace.depot.enabled ? QString("Steam workspace '%1' has no directory.").arg(tag) : QString("Workspace '%1' has no source.").arg(tag);
		if (workspace.output.trimmed().isEmpty()) return QString("Workspace '%1' has no output.").arg(tag);
		if (workspace.depot.enabled) {
			if (depotLayout(workspace.depot.appId, workspace.depot.os).depotId <= 0)
				return QString("Steam workspace '%1': AppID %2 / OS '%3' is not supported.").arg(tag).arg(workspace.depot.appId).arg(workspace.depot.os);
			if (QDir::cleanPath(workspace.source) != QDir::cleanPath(workspace.output))
				return QString("Steam workspace '%1' must use one directory for manifests and analysis.").arg(tag);
		}
		const QString tagKey = tag.toCaseFolded();
		if (tags.contains(tagKey)) return QString("Duplicate tag: %1").arg(tag);
		tags.insert(tagKey);
		QSet<QString> tagNames; // duplicate check is per-tag: the same library across tags is fine
		for (int fileIndex = 0; fileIndex < workspace.files.size(); ++fileIndex) {
			const QString& raw = workspace.files[fileIndex];
			const QString relative = QDir::cleanPath(raw.trimmed());
			if (relative.isEmpty() || relative == "." || relative == ".." || QDir::isAbsolutePath(relative) || relative.startsWith("../"))
				return QString("File must be relative to %1 (tag %2): %3")
					.arg(workspace.depot.enabled ? "the depot game directory" : "Source", tag, raw);
			const QString name = QFileInfo(relative).completeBaseName();
			if (name.isEmpty()) return QString("File has no library name (tag %1): %2").arg(tag, raw);
			const QString nk = name.toCaseFolded();
			if (tagNames.contains(nk)) return QString("Duplicate library %1 in tag %2.").arg(name, tag);
			tagNames.insert(nk);
			const int ov = overrideFor(tag, name);
			const int port = ov > 0 ? ov : AutoScanPort(cfg.basePort, fileIndex, workspace.portOffset);
			if (port < 1 || port > 65535) return QString("Port for %1@%2 is outside 1..65535.").arg(name, tag);
			if (ports.contains(port)) return QString("Port collision at %1. Change a port on the main page or a tag's offset.").arg(port);
			ports.insert(port);
			instances.insert(tagKey + '\t' + nk);
		}
	}
	for (int i = 0; i < cfg.extraLibs.size(); ++i) {
		const ExtraLib& extra = cfg.extraLibs[i];
		const QString tag = extra.tag.trimmed();
		const QString name = QFileInfo(extra.path.trimmed()).completeBaseName();
		if (tag.isEmpty() || extra.path.trimmed().isEmpty()) return "Every library needs a tag and path.";
		if (!QRegularExpression("^#[0-9A-Fa-f]{6}$").match(extra.color.trimmed()).hasMatch())
			return QString("Library '%1' needs an explicit #RRGGBB color.").arg(tag);
		if (name.isEmpty()) return QString("Library '%1' does not point to a file name.").arg(tag);
		const QString instance = tag.toCaseFolded() + '\t' + name.toCaseFolded();
		if (instances.contains(instance)) return QString("Duplicate library instance: %1@%2").arg(name, tag);
		instances.insert(instance);
		const int port = extra.port > 0 ? extra.port : AutoExtraPort(cfg.basePort, i);
		if (port < 1 || port > 65535) return QString("Port for library %1 is outside 1..65535.").arg(name);
		if (ports.contains(port)) return QString("Port collision at %1 (library %2).").arg(port).arg(name);
		ports.insert(port);
	}
	return {};
}

static QString prepareWorkspaceDirectories(const ConfigView& cfg) {
	for (const Workspace& workspace : AllWorkspaces(cfg)) {
		const QString output = workspace.output.trimmed();
		if (!QDir().mkpath(output)) return QString("Cannot create output directory for '%1': %2").arg(workspace.tag, output);
		if (workspace.depot.enabled) continue;
		const QString revisions = QDir(output).filePath("revisions");
		if (!QDir().mkpath(revisions)) return QString("Cannot create revisions directory for '%1': %2").arg(workspace.tag, revisions);
		for (const QString& relative : workspace.files) {
			const QString destination = QDir(output).filePath(relative);
			if (!QDir().mkpath(QFileInfo(destination).absolutePath()))
				return QString("Cannot create output path for '%1': %2").arg(workspace.tag, QFileInfo(destination).absolutePath());
		}
	}
	return {};
}

int FirstFreeExtraPort(const ConfigView& cfg) {
	QSet<int> used;
	auto overrideFor = [&cfg](const QString& tag, const QString& name) -> int {
		for (const PortOverride& po : cfg.portOverrides)
			if (po.tag == tag && po.name == name) return po.port;
		return 0;
	};
	for (const Workspace& workspace : AllWorkspaces(cfg))
		for (int fileIndex = 0; fileIndex < workspace.files.size(); ++fileIndex) {
			const QString& raw = workspace.files[fileIndex];
			const QString name = QFileInfo(raw.trimmed()).completeBaseName();
			if (name.isEmpty()) continue;
			const int ov = overrideFor(workspace.tag.trimmed(), name);
			used.insert(ov > 0 ? ov : AutoScanPort(cfg.basePort, fileIndex, workspace.portOffset));
		}
	for (int i = 0; i < cfg.extraLibs.size(); ++i)
		used.insert(cfg.extraLibs[i].port > 0 ? cfg.extraLibs[i].port : AutoExtraPort(cfg.basePort, i));
	int port = AutoExtraPort(cfg.basePort, cfg.extraLibs.size());
	while (used.contains(port) && port < 65535) ++port;
	return port;
}

static bool copyFileAtomically(const QString& sourcePath, const QString& destinationPath, QString* error) {
	QFile source(sourcePath);
	if (!source.open(QIODevice::ReadOnly)) {
		if (error) *error = source.errorString();
		return false;
	}
	if (!QDir().mkpath(QFileInfo(destinationPath).absolutePath())) {
		if (error) *error = "cannot create destination directory";
		return false;
	}
	QSaveFile destination(destinationPath);
	destination.setDirectWriteFallback(false);
	if (!destination.open(QIODevice::WriteOnly)) {
		if (error) *error = destination.errorString();
		return false;
	}
	QByteArray block(1024 * 1024, Qt::Uninitialized);
	while (!source.atEnd()) {
		const qint64 count = source.read(block.data(), block.size());
		if (count < 0 || destination.write(block.constData(), count) != count) {
			if (error) *error = count < 0 ? source.errorString() : destination.errorString();
			destination.cancelWriting();
			return false;
		}
	}
	destination.setPermissions(source.permissions());
	if (!destination.commit()) {
		if (error) *error = destination.errorString();
		return false;
	}
	return true;
}

static void readSteamVersions(const QString& steamInfPath, QString* patchVersion, QString* serverVersion) {
	QFile inf(steamInfPath);
	if (!inf.open(QIODevice::ReadOnly | QIODevice::Text)) return;
	const QString text = QString::fromUtf8(inf.readAll());
	auto value = [&text](const QString& key) {
		return QRegularExpression(QString("(?mi)^\\s*%1\\s*=\\s*([^\\r\\n]+)").arg(QRegularExpression::escape(key))).match(text).captured(1).trimmed();
	};
	const QString patch = value("PatchVersion"), server = value("ServerVersion");
	if (patchVersion && !patch.isEmpty()) *patchVersion = patch;
	if (serverVersion && !server.isEmpty()) *serverVersion = server;
}

// The static build is a single exe, but the Python glue it drives (start_mcp.py /
// analyze_ida.py / disable_autostart.py) are sidecar files. They are embedded as
// resources and re-materialised next to config.json — an always-writable folder —
// so copying just the exe to another machine still works. Only rewrites when the
// bundled copy differs, so it stays in lockstep with the exe without clobbering on
// every launch. Silently no-ops in builds without the resource (e.g. unit tests).
static void ensureHelperScripts(const QString& destDir) {
	QDir().mkpath(destDir);
	for (const char* name : {"start_mcp.py", "analyze_ida.py", "disable_autostart.py"}) {
		QFile bundled(QString(":/scripts/%1").arg(name));
		if (!bundled.open(QIODevice::ReadOnly)) continue; // resource absent (tests) — leave any on-disk copy
		const QByteArray data = bundled.readAll();
		const QString dest = QDir(destDir).filePath(name);
		QFile existing(dest);
		if (existing.exists() && existing.open(QIODevice::ReadOnly)) {
			const bool current = existing.readAll() == data;
			existing.close();
			if (current) continue; // already up to date
		}
		QSaveFile out(dest);
		if (out.open(QIODevice::WriteOnly)) {
			out.write(data);
			out.commit();
		}
	}
}

Manager::Manager(QObject* parent) :
	QObject(parent) {
}

bool Manager::LoadConfig(const QString& path, QString* err) {
	_configPath = QFileInfo(path).absoluteFilePath();
	const QString configDir = QFileInfo(path).absolutePath();
	const QString appDir = QCoreApplication::applicationDirPath();
	// Self-heal the sidecar scripts in the (writable) config dir before resolving
	// where to run them from, so a bare copied exe finds its helpers there.
	ensureHelperScripts(configDir);
	_scriptDir = QFile::exists(QDir(appDir).filePath("start_mcp.py")) ? appDir : configDir;
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		if (err) *err = QString("cannot open %1: %2").arg(path, f.errorString());
		return false;
	}
	QJsonParseError pe;
	const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
	f.close();
	if (doc.isNull() || !doc.isObject()) {
		if (err) *err = QString("bad JSON in %1: %2").arg(path, pe.errorString());
		return false;
	}
	QJsonObject c = doc.object();
	ConfigView next;
	bool generatedColors = false;
	next.host = c.value("host").toString("127.0.0.1");
	const QJsonObject ida = c.value("ida").toObject();
	next.idaGui = expand(ida.value("gui").toString());
	next.idaText = expand(ida.value("text").toString());
	next.logDir = expand(c.value("logDir").toString("~/.ida-workbench"));
	next.analysisArgs = c.value("analysisArgs").toString().trimmed();
	const int bp = c.value("scanBasePort").toInt(DEFAULT_BASE_PORT);
	next.basePort = (bp >= 1024 && bp <= 60000) ? bp : DEFAULT_BASE_PORT;
	const int ml = c.value("maxLogSizeMB").toInt(DEFAULT_MAX_LOG_MB);
	next.maxLogMB = (ml >= 0 && ml <= MAX_LOG_MB_CEILING) ? ml : DEFAULT_MAX_LOG_MB;
	const QJsonObject downloader = c.value("depotDownloader").toObject();
	next.depotDownloader.executable = expand(downloader.value("executable").toString("DepotDownloader.exe").trimmed());
	next.depotDownloader.timeoutMinutes = qBound(1, downloader.value("timeoutMinutes").toInt(30), 240);

	auto parseWorkspace = [&](const QJsonObject& object, bool steam) {
		Workspace workspace;
		workspace.tag = object.value("tag").toString().trimmed();
		if (steam) {
			const QString dir = expand(object.value("dir").toString().trimmed());
			workspace.source = dir;
			workspace.output = dir;
		} else {
			workspace.source = expand(object.value("source").toString().trimmed());
			workspace.output = expand(object.value("output").toString().trimmed());
		}
		workspace.portOffset = object.value("portOffset").toInt();
		workspace.color = object.value("color").toString().trimmed().toUpper();
		if (workspace.color.isEmpty()) { workspace.color = generatedTagColor(workspace.tag); generatedColors = true; }
		workspace.depot.enabled = steam;
		workspace.depot.appId = object.value("appId").toInt(730);
		workspace.depot.os = object.value("os").toString("windows").trimmed().toLower();
		workspace.depot.manifest = object.value("current").toString().trimmed();
		for (const QJsonValue file : object.value("files").toArray()) {
			const QString relative = QDir::cleanPath(file.toString().trimmed());
			if (!relative.isEmpty() && !workspace.files.contains(relative)) workspace.files << relative;
		}
		if (!workspace.tag.isEmpty()) (steam ? next.steamWorkspaces : next.workspaces) << workspace;
	};
	for (const QJsonValue value : c.value("workspaces").toArray()) {
		const QJsonObject object = value.toObject();
		parseWorkspace(object, false);
	}
	for (const QJsonValue value : c.value("steamWorkspaces").toArray()) parseWorkspace(value.toObject(), true);
	for (const QJsonValue value : c.value("portOverrides").toArray()) {
		const QJsonObject object = value.toObject();
		const QString tag = object.value("tag").toString().trimmed();
		const QString name = object.value("name").toString().trimmed();
		const int port = object.value("port").toInt();
		if (!tag.isEmpty() && !name.isEmpty() && port > 0) next.portOverrides.push_back({tag, name, port});
	}
	for (const QJsonValue value : c.value("extraLibs").toArray()) {
		const QJsonObject object = value.toObject();
		ExtraLib extra;
		extra.tag = object.value("tag").toString().trimmed();
		extra.path = expand(object.value("path").toString().trimmed());
		extra.port = object.value("port").toInt();
		extra.color = object.value("color").toString().trimmed().toUpper();
		if (extra.color.isEmpty()) { extra.color = generatedTagColor(extra.tag); generatedColors = true; }
		if (!extra.tag.isEmpty() && !extra.path.isEmpty()) next.extraLibs << extra;
	}

	// IDA paths may be empty on a fresh install — the app still comes up and
	// reports IDA as not-ready until they are set in Settings (or auto-detected).
	const QString validation = validateConfig(next);
	if (!validation.isEmpty()) {
		if (err) *err = "config: " + validation;
		return false;
	}
	const QString directoryError = prepareWorkspaceDirectories(next);
	if (!directoryError.isEmpty()) {
		if (err) *err = "config: " + directoryError;
		return false;
	}
	for (Workspace& workspace : next.steamWorkspaces) {
		workspace.depot.patchVersion.clear();
		workspace.depot.serverVersion.clear();
		if (!workspace.depot.manifest.isEmpty())
			readSteamVersions(QDir(workspace.output).filePath(workspace.depot.manifest + "/csgo/steam.inf"),
				&workspace.depot.patchVersion, &workspace.depot.serverVersion);
	}
	bool schemaNeedsWrite = generatedColors || !c.contains("analysisArgs") || !c.contains("depotDownloader") || !c.contains("steamWorkspaces");
	for (const QJsonValue value : c.value("workspaces").toArray()) {
		const QJsonObject object = value.toObject();
		if (!object.contains("color") || !object.contains("portOffset")) schemaNeedsWrite = true;
	}
	if (schemaNeedsWrite) {
		QHash<QString, QString> extraColors;
		for (const ExtraLib& extra : next.extraLibs) extraColors.insert(extra.tag + '\t' + extra.path, extra.color);
		c["analysisArgs"] = next.analysisArgs;
		c["depotDownloader"] = QJsonObject{{"executable", next.depotDownloader.executable}, {"timeoutMinutes", next.depotDownloader.timeoutMinutes}};
		auto workspaceObject = [](const Workspace& workspace, bool steam) {
			QJsonObject object{{"tag", workspace.tag}, {"files", QJsonArray::fromStringList(workspace.files)},
				{"portOffset", workspace.portOffset}, {"color", workspace.color}};
			if (steam) {
				object["dir"] = workspace.output;
				object["appId"] = workspace.depot.appId;
				object["os"] = workspace.depot.os;
				if (!workspace.depot.manifest.isEmpty()) object["current"] = workspace.depot.manifest;
			} else {
				object["source"] = workspace.source;
				object["output"] = workspace.output;
			}
			return object;
		};
		QJsonArray workspaceArray;
		for (const Workspace& workspace : next.workspaces) workspaceArray.append(workspaceObject(workspace, false));
		c["workspaces"] = workspaceArray;
		QJsonArray steamArray;
		for (const Workspace& workspace : next.steamWorkspaces) steamArray.append(workspaceObject(workspace, true));
		c["steamWorkspaces"] = steamArray;
		QJsonArray extraArray = c.value("extraLibs").toArray();
		for (int i = 0; i < extraArray.size(); ++i) {
			QJsonObject object = extraArray[i].toObject();
			const QString key = object.value("tag").toString() + '\t' + object.value("path").toString();
			if (object.value("color").toString().trimmed().isEmpty()) object["color"] = extraColors.value(key);
			extraArray[i] = object;
		}
		c["extraLibs"] = extraArray;
		QSaveFile normalized(_configPath);
		if (normalized.open(QIODevice::WriteOnly)) {
			normalized.write(QJsonDocument(c).toJson(QJsonDocument::Indented));
			normalized.commit();
		}
	}

	_host = next.host.trimmed();
	_idaGui = next.idaGui;
	_idaText = next.idaText;
	_logDir = next.logDir;
	_analysisArgs = next.analysisArgs;
	_maxLogMB = next.maxLogMB;
	_basePort = next.basePort;
	_depotDownloader = next.depotDownloader;
	_workspaces = AllWorkspaces(next);
	_extraLibs = next.extraLibs;
	_portOverrides = next.portOverrides;
	RebuildInstances();
	// Point the canonical log at the configured folder and apply the size cap.
	Log::Configure(QDir(_logDir).filePath(Log::FileName()), _maxLogMB);
	qDebug().noquote() << QString("[config] loaded %1 · ida=%2 · idat=%3 · basePort=%4 · workspaces=%5")
							  .arg(QFileInfo(path).absoluteFilePath(), _idaGui, _idaText)
							  .arg(_basePort)
							  .arg(_workspaces.size());
	return true;
}

bool Manager::CreateDefaultConfig(const QString& path, QString* err) {
	QString gui, text;
	AutoDetectIda(&gui, &text); // best-effort; left blank when IDA is not found

	QJsonObject root;
	root["host"] = "127.0.0.1";
	root["ida"] = QJsonObject{{"gui", gui}, {"text", text}};
	root["logDir"] = "~/.ida-workbench";
	root["scanBasePort"] = DEFAULT_BASE_PORT;
	root["maxLogSizeMB"] = DEFAULT_MAX_LOG_MB;
	root["analysisArgs"] = "";
	root["depotDownloader"] = QJsonObject{{"executable", "DepotDownloader.exe"}, {"timeoutMinutes", 30}};
	root["extraLibs"] = QJsonArray{};
	root["workspaces"] = QJsonArray{};
	root["steamWorkspaces"] = QJsonArray{};

	if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
		if (err) *err = "cannot create " + QFileInfo(path).absolutePath();
		return false;
	}
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly)) {
		if (err) *err = f.errorString();
		return false;
	}
	const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
	if (f.write(json) != json.size() || !f.commit()) {
		if (err) *err = f.errorString();
		return false;
	}
	return true;
}

void Manager::RebuildInstances() {
	_instances.clear();
	_tags.clear();
	_names.clear();

	for (const Workspace& sd : _workspaces)
		if (!_tags.contains(sd.tag)) _tags << sd.tag;
	for (const ExtraLib& e : _extraLibs)
		if (!_tags.contains(e.tag)) _tags << e.tag;
	for (const Workspace& sd : _workspaces)
		for (const QString& file : sd.files) {
			const QString n = QFileInfo(file).completeBaseName();
			if (!n.isEmpty() && !_names.contains(n)) _names << n;
		}
	for (const ExtraLib& e : _extraLibs) {
		const QString n = QFileInfo(e.path).completeBaseName();
		if (!_names.contains(n)) _names << n;
	}

	auto overrideFor = [this](const QString& tag, const QString& name) -> int {
		for (const PortOverride& po : _portOverrides)
			if (po.tag == tag && po.name == name) return po.port;
		return 0;
	};
	for (const Workspace& sd : _workspaces) {
		const QString selectedVersion = _selectedVersions.value(sd.tag);
		QString root;
		if (sd.depot.enabled) {
			const QString manifest = selectedVersion.isEmpty() ? sd.depot.manifest : selectedVersion;
			root = QDir(sd.output).filePath(manifest.isEmpty() ? ".no-current-manifest" : manifest);
		} else {
			root = selectedVersion.isEmpty() ? sd.output : QDir(sd.output).filePath("revisions/" + selectedVersion);
		}
		for (int fileIndex = 0; fileIndex < sd.files.size(); ++fileIndex) {
			const QString& relative = sd.files[fileIndex];
			const QString name = QFileInfo(relative).completeBaseName();
			if (name.isEmpty()) continue;
			Instance inst;
			inst.tag = sd.tag;
			inst.name = name;
			inst.relativeFile = relative;
			inst.outputRoot = sd.output;
			inst.storedVersion = selectedVersion;
			inst.binary = QDir(root).filePath(relative);
			inst.hasSource = !sd.depot.enabled;
			inst.compareSource = !sd.depot.enabled && selectedVersion.isEmpty();
			if (!sd.depot.enabled) inst.sourceBinary = QDir(sd.source).filePath(relative);
			const int ov = overrideFor(sd.tag, name);	  // hand-edited port, else auto
			inst.port = ov > 0 ? ov : AutoScanPort(_basePort, fileIndex, sd.portOffset);
			_instances.push_back(inst);
		}
	}
	for (int ei = 0; ei < _extraLibs.size(); ++ei) {
		const ExtraLib& e = _extraLibs[ei];
		Instance inst;
		inst.tag = e.tag;
		inst.name = QFileInfo(e.path).completeBaseName();
		inst.binary = e.path;
		inst.port = e.port > 0 ? e.port : AutoExtraPort(_basePort, ei);
		_instances.push_back(inst);
	}
}

QStringList Manager::StoredVersions(const QString& tag) const {
	for (const Workspace& workspace : _workspaces) {
		if (workspace.tag != tag) continue;
		if (workspace.depot.enabled) {
			QStringList manifests = QDir(workspace.output).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
			manifests.erase(std::remove_if(manifests.begin(), manifests.end(), [&workspace](const QString& name) {
				return name == workspace.depot.manifest || !QRegularExpression("^[0-9]+$").match(name).hasMatch();
			}), manifests.end());
			return manifests;
		}
		QDir revisionsDir(QDir(workspace.output).filePath("revisions"));
		return revisionsDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
	}
	return {};
}

void Manager::SetStoredVersion(const QString& tag, const QString& version) {
	if (!version.isEmpty() && !StoredVersions(tag).contains(version)) return;
	if (version.isEmpty())
		_selectedVersions.remove(tag);
	else
		_selectedVersions.insert(tag, version);
	RebuildInstances();
	Refresh();
}

const Instance* Manager::InstanceAt(const QString& tag, const QString& name) const {
	for (const Instance& i : _instances)
		if (i.tag == tag && i.name == name) return &i;
	return nullptr;
}

QString Manager::TargetKey(const Target& target) const {
	return target.tag + "\t" + target.name + "\t" + target.revision;
}

std::optional<Instance> Manager::ResolveTarget(const Target& target) const {
	auto overrideFor = [this](const QString& tag, const QString& name) -> int {
		for (const PortOverride& po : _portOverrides)
			if (po.tag == tag && po.name == name) return po.port;
		return 0;
	};
	for (const Workspace& workspace : _workspaces) {
		if (workspace.tag != target.tag) continue;
		for (int fileIndex = 0; fileIndex < workspace.files.size(); ++fileIndex) {
			const QString relative = workspace.files[fileIndex];
			if (QFileInfo(relative).completeBaseName() != target.name) continue;
			QString root;
			if (workspace.depot.enabled) {
				const QString manifest = target.revision.isEmpty() ? workspace.depot.manifest : target.revision;
				root = QDir(workspace.output).filePath(manifest.isEmpty() ? ".no-current-manifest" : manifest);
			} else {
				root = target.revision.isEmpty() ? workspace.output :
					QDir(workspace.output).filePath("revisions/" + target.revision);
			}
			Instance inst;
			inst.tag = workspace.tag;
			inst.name = target.name;
			inst.relativeFile = relative;
			inst.outputRoot = workspace.output;
			inst.storedVersion = target.revision;
			inst.binary = QDir(root).filePath(relative);
			inst.hasSource = !workspace.depot.enabled;
			inst.compareSource = !workspace.depot.enabled && target.revision.isEmpty();
			if (inst.hasSource) inst.sourceBinary = QDir(workspace.source).filePath(relative);
			const int overridePort = overrideFor(workspace.tag, target.name);
			inst.port = overridePort > 0 ? overridePort : AutoScanPort(_basePort, fileIndex, workspace.portOffset);
			return inst;
		}
	}
	if (!target.revision.isEmpty()) return std::nullopt;
	for (int index = 0; index < _extraLibs.size(); ++index) {
		const ExtraLib& extra = _extraLibs[index];
		if (extra.tag != target.tag || QFileInfo(extra.path).completeBaseName() != target.name) continue;
		Instance inst;
		inst.tag = extra.tag;
		inst.name = target.name;
		inst.binary = extra.path;
		inst.port = extra.port > 0 ? extra.port : AutoExtraPort(_basePort, index);
		return inst;
	}
	return std::nullopt;
}

bool Manager::TargetBusy(const Target& target) const {
	if (_depotTags.contains(target.tag)) return true;
	const QString key = TargetKey(target);
	if (_analyzeJobs.contains(key) || _idaPids.contains(key)) return true;
	const auto requested = ResolveTarget(target);
	if (!requested) return false;
	const QString path = QDir::cleanPath(QFileInfo(requested->binary).absoluteFilePath()).toLower();
	for (auto it = _analyzeJobs.constBegin(); it != _analyzeJobs.constEnd(); ++it) {
		const QStringList parts = it.key().split('\t');
		const auto activeTarget = ResolveTarget({parts.value(0), parts.value(1), parts.value(2)});
		if (activeTarget && QDir::cleanPath(QFileInfo(activeTarget->binary).absoluteFilePath()).toLower() == path) return true;
	}
	for (auto it = _idaPids.constBegin(); it != _idaPids.constEnd(); ++it) {
		const QStringList parts = it.key().split('\t');
		const auto openTarget = ResolveTarget({parts.value(0), parts.value(1), parts.value(2)});
		if (openTarget && QDir::cleanPath(QFileInfo(openTarget->binary).absoluteFilePath()).toLower() == path) return true;
	}
	return false;
}

QString Manager::StartPy() const { return QDir(_scriptDir).filePath("start_mcp.py"); }
QString Manager::AnalyzePy() const { return QDir(_scriptDir).filePath("analyze_ida.py"); }

// --- os helpers -------------------------------------------------------------

bool Manager::PortUp(int port, int timeoutMs) const {
	QTcpSocket s;
	s.connectToHost(_host, static_cast<quint16>(port));
	const bool ok = s.waitForConnected(timeoutMs);
	s.abort();
	return ok;
}

qint64 Manager::PidOnPort(int port) const {
#ifdef Q_OS_WIN
	DWORD bytes = 0;
	GetExtendedTcpTable(nullptr, &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
	QByteArray storage(static_cast<int>(bytes), Qt::Uninitialized);
	if (GetExtendedTcpTable(storage.data(), &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
		const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(storage.constData());
		for (DWORD index = 0; index < table->dwNumEntries; ++index) {
			const quint16 networkPort = static_cast<quint16>(table->table[index].dwLocalPort);
			const int hostPort = ((networkPort & 0xff) << 8) | (networkPort >> 8);
			if (hostPort == port) return static_cast<qint64>(table->table[index].dwOwningPid);
		}
	}
#else
	QProcess p;
	p.start("ss", {"-ltnpH"});
	p.waitForFinished(4000);
	const QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
	for (const QString& line : out.split('\n'))
		if (line.contains(QRegularExpression(QString("[:.]%1\\s").arg(port)))) {
			const auto m = QRegularExpression("pid=(\\d+)").match(line);
			if (m.hasMatch()) return m.captured(1).toLongLong();
		}
#endif
	return -1;
}

bool Manager::IsExpectedServerProcess(qint64 pid) const {
	if (pid <= 0) return false;
	const QString expected = QFileInfo(_idaGui).fileName();
#ifdef Q_OS_WIN
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
	if (!process) return false;
	wchar_t path[32768] = {};
	DWORD length = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
	const bool queried = QueryFullProcessImageNameW(process, 0, path, &length);
	CloseHandle(process);
	return queried && QFileInfo(QString::fromWCharArray(path, static_cast<int>(length))).fileName()
		.compare(expected, Qt::CaseInsensitive) == 0;
#else
	const QString executable = QFileInfo(QFileInfo(QString("/proc/%1/exe").arg(pid)).symLinkTarget()).fileName();
	return !executable.isEmpty() && executable == expected;
#endif
}

bool Manager::KillPid(qint64 pid) const {
	if (pid <= 0) return false;
#ifdef Q_OS_WIN
	HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
	if (!process) return false;
	const bool killed = TerminateProcess(process, 1) != FALSE;
	if (killed) WaitForSingleObject(process, 3000);
	CloseHandle(process);
	return killed;
#else
	return ::kill(static_cast<pid_t>(pid), SIGTERM) == 0;
#endif
}

static bool processRunning(qint64 pid) {
	if (pid <= 0) return false;
#ifdef Q_OS_WIN
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
	if (!process) return false;
	DWORD code = 0;
	const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
	CloseHandle(process);
	return running;
#else
	return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

void Manager::PollIdaProcesses() {
	for (auto it = _idaPids.begin(); it != _idaPids.end();) {
		if (processRunning(it.value())) { ++it; continue; }
		const QStringList parts = it.key().split('\t');
		Target target{parts.value(0), parts.value(1), parts.value(2)};
		it = _idaPids.erase(it);
		emit OperationChanged(target, "ida", false);
	}
}

void Manager::RequestStopDepot() {
	_stopDepotRequested = true;
}

bool Manager::LaunchServer(const Instance& inst) {
	if (!QDir().mkpath(_logDir)) return false;
	// The server subprocess appends to the SAME canonical file as the GUI, so its
	// output joins the app log instead of a separate servers.log.
	const QString logPath = QDir(_logDir).filePath(Log::FileName());
	QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	env.insert("IDA_MCP_PORT", QString::number(inst.port));
	env.insert("IDA_MCP_HOST", _host);
	env.insert("IDA_MCP_LOG", logPath);
	env.insert("IDA_MCP_LOG_MAX_MB", QString::number(_maxLogMB));

	const QString db = inst.binary + ".i64";
	QProcess proc;
	proc.setProcessEnvironment(env);
#ifdef Q_OS_WIN
	proc.setProgram("cmd.exe");
	proc.setNativeArguments(QString(R"(/c start "" /min "%1" -A "-S%2" "%3")")
								.arg(_idaGui, StartPy(), db));
	qDebug().noquote() << QString("[mcp] launch %1@%2 port=%3 : %4 -A -S%5 %6")
							  .arg(inst.name, inst.tag)
							  .arg(inst.port)
							  .arg(QDir::toNativeSeparators(_idaGui), QDir::toNativeSeparators(StartPy()), QDir::toNativeSeparators(db));
#else
	proc.setProgram(_idaGui);
	proc.setArguments({"-A", "-S" + StartPy(), db});
	qDebug().noquote() << QString("[mcp] launch %1@%2 port=%3 : %4 -A -S%5 %6")
							  .arg(inst.name, inst.tag)
							  .arg(inst.port)
							  .arg(_idaGui, StartPy(), db);
#endif
	return proc.startDetached();
}

QString Manager::Sha256(const QString& path) {
	const QFileInfo fi(path);
	if (!fi.exists()) return {};
	const qint64 sz = fi.size();
	const qint64 mt = fi.lastModified().toMSecsSinceEpoch();

	auto it = _shaCache.constFind(path);
	if (it != _shaCache.constEnd() && it->size == sz && it->mtime == mt)
		return it->hash; // unchanged -> reuse

	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) return {};
	QCryptographicHash h(QCryptographicHash::Sha256);
	if (!h.addData(&f)) return {};
	const QString hex = QString::fromLatin1(h.result().toHex());
	_shaCache.insert(path, {sz, mt, hex});
	return hex;
}

// --- commands ---------------------------------------------------------------

void Manager::OpenIda(const Target& target) {
	PollIdaProcesses();
	const auto resolved = ResolveTarget(target);
	if (!resolved) {
		emit Log(QString("[fail] %1@%2: library is no longer configured").arg(target.name, target.tag));
		return;
	}
	const Instance* inst = &*resolved;
	const QString label = QString("%1@%2").arg(inst->name, inst->tag);
	if (TargetBusy(target)) {
		emit Log(QString("[skip] %1: this database is already in use").arg(label));
		return;
	}
	if (!QFile::exists(_idaGui)) {
		emit Log(QString("[fail] %1: IDA GUI executable is missing; check Settings").arg(label));
		return;
	}
	if (PortUp(inst->port)) {
		emit Log(QString("[skip] %1: MCP server is using this database; stop it before opening IDA").arg(label));
		return;
	}

	const QString database = inst->binary + ".i64";
	const bool hasDatabase = QFile::exists(database);
	const QString input = hasDatabase ? database : inst->binary;
	if (!QFile::exists(input)) {
		emit Log(QString("[fail] %1: neither an IDA database nor the binary exists").arg(label));
		return;
	}

	qint64 pid = 0;
	const bool started = QProcess::startDetached(_idaGui, {input}, QFileInfo(input).absolutePath(), &pid);
	if (started) {
		_idaPids.insert(TargetKey(target), pid);
		emit OperationChanged(target, "ida", true);
		emit Log(QString("[ida] %1: %2 %3 (pid %4)")
			.arg(label, hasDatabase ? QStringLiteral("opened database") : QStringLiteral("importing binary"),
				 QDir::toNativeSeparators(input))
			.arg(pid));
	} else
		emit Log(QString("[fail] %1: could not launch %2").arg(label, QDir::toNativeSeparators(_idaGui)));
}

// Derive a "major.minor" Python version from a linked DLL path, e.g.
// ".../python314.dll" -> "3.14", or ".../pythoncore-3.14-64/python3.dll" -> "3.14".
static QString pythonVerFromPath(const QString& path) {
	static const QRegularExpression reDll(R"(python(\d)(\d+)\.)", QRegularExpression::CaseInsensitiveOption);
	const auto m = reDll.match(QFileInfo(path).fileName());
	if (m.hasMatch()) return m.captured(1) + "." + m.captured(2);
	static const QRegularExpression reAny(R"((\d+)\.(\d+))");
	const auto m2 = reAny.match(path);
	return m2.hasMatch() ? (m2.captured(1) + "." + m2.captured(2)) : QString();
}

Readiness Manager::ComputeReadiness() const {
	Readiness r;

	const bool gui = QFile::exists(_idaGui), txt = QFile::exists(_idaText);
	r.ida = gui && txt;
	r.idaMsg = r.ida ? ("IDA: " + _idaGui) : ("Not found: " + (gui ? _idaText : _idaGui) + " — set IDA paths in Settings");

	const QString idaDir = QFileInfo(_idaGui).absolutePath();
	QString pyPlugin;
	for (const QString& f : {"idapython3.dll", "idapython3.so", "idapython3.dylib", "idapython3_64.so"}) {
		const QString p = idaDir + "/plugins/" + f;
		if (QFile::exists(p)) {
			pyPlugin = p;
			break;
		}
	}
	const bool pluginPresent = !pyPlugin.isEmpty();

	// Report the interpreter IDA is actually bound to (recorded by idapyswitch),
	// not just that the idapython plugin file exists. On Windows that binding is
	// the registry value Python3TargetDLL; reading it + checking the DLL still
	// exists is a real link check. Elsewhere we fall back to plugin presence.
	QString linkedDll;
#ifdef Q_OS_WIN
	linkedDll = QSettings(R"(HKEY_CURRENT_USER\Software\Hex-Rays\IDA)", QSettings::NativeFormat)
					.value("Python3TargetDLL")
					.toString();
#endif
	if (!linkedDll.isEmpty()) {
		const bool dllExists = QFile::exists(linkedDll);
		const QString ver = dllExists ? pythonVerFromPath(linkedDll) : QString();
		r.python = pluginPresent && dllExists;
		r.pythonLabel = ver.isEmpty() ? QStringLiteral("Python") : ("Python " + ver);
		if (!dllExists)
			r.pythonMsg = "IDA is bound to a Python that is missing:\n" + linkedDll + "\nInstall it or run idapyswitch to relink.";
		else if (!pluginPresent)
			r.pythonMsg = "idapython3 plugin missing from the IDA install\n(bound Python " + ver + ": " + linkedDll + ")";
		else
			r.pythonMsg = "IDAPython bound to Python " + ver + "\n" + linkedDll;
	} else {
		r.python = pluginPresent;
		r.pythonLabel = QStringLiteral("IDAPython");
		r.pythonMsg = pluginPresent ? ("IDAPython: " + pyPlugin) : "IDAPython plugin not found in the IDA install";
	}

	QString pluginsDir;
#ifdef Q_OS_WIN
	pluginsDir = QDir(qEnvironmentVariable("APPDATA")).filePath("Hex-Rays/IDA Pro/plugins");
#else
	pluginsDir = QDir::homePath() + "/.idapro/plugins";
#endif
	const QString mcpPlugin = QDir(pluginsDir).filePath("ida_mcp.py");
	const bool pluginExists = QFile::exists(mcpPlugin);
	const bool scriptsExist = QFile::exists(StartPy()) && QFile::exists(AnalyzePy());
	r.mcp = pluginExists && scriptsExist;
	if (!pluginExists)
		r.mcpMsg = "ida-pro-mcp plugin missing in " + pluginsDir + " — run: ida-pro-mcp --install";
	else if (!scriptsExist)
		r.mcpMsg = "Workbench helper scripts (start_mcp.py / analyze_ida.py) could not be written to the config folder — check it is writable";
	else
		r.mcpMsg = "ida-pro-mcp: " + mcpPlugin;
	return r;
}

static QString mbStr(qint64 n) {
	return n < 0 ? QStringLiteral("—") : QString::number(n / 1048576.0, 'f', 2) + " MB";
}

// One-line meaning for each status, used in the hover tooltip.
static QString statusExplain(const QString& s) {
	if (s == "Ready") return "Analyzed and current — ready to Start.";
	if (s == "Update available") return "The live game shipped a newer binary — Replace, then Analyze.";
	if (s == "Not analyzed") return "Binary is present but has no .i64 database — press Analyze.";
	if (s == "Re-analyze") return "The database is older than the binary — press Analyze.";
	if (s == "No binary") return "No binary in this folder — drop one in, or Replace from the live source.";
	return s;
}

static QString buildStatusTip(const Instance& inst, const Cell& c, bool binEx, bool liveEx, bool i64stale) {
	QStringList lines;
	lines << QString("%1 @ %2").arg(inst.name, inst.tag);
	lines << "Status: " + c.state + " — " + statusExplain(c.state);
	lines << "Binary: " + inst.binary + (binEx ? "  (" + mbStr(c.size) + ")" : "  (missing)");
	lines << "Database: " + (c.hasDb ? QString(".i64 present") + (i64stale ? " — older than binary" : "") : QString("none"));
	lines << "Port: " + QString::number(inst.port);
	if (inst.hasSource) {
		const QString src = liveEx ? inst.sourceBinary + "  (" + mbStr(c.srcSize) + ")" : QString("not found");
		lines << "Source: " + src;
	}
	return lines.join("\n");
}

void Manager::Refresh() {
	PollIdaProcesses();
	emit ReadinessChanged(ComputeReadiness());
	QVector<int> ports;
	ports.reserve(_instances.size());
	for (const Instance& instance : _instances)
		if (!ports.contains(instance.port)) ports << instance.port;
	const QString host = _host;
	std::vector<std::future<QPair<int, bool>>> portFutures;
	portFutures.reserve(static_cast<size_t>(ports.size()));
	for (const int port : ports) {
		portFutures.emplace_back(std::async(std::launch::async, [host, port] {
			QTcpSocket socket;
			socket.connectToHost(host, static_cast<quint16>(port));
			const bool up = socket.waitForConnected(500);
			socket.abort();
			return qMakePair(port, up);
		}));
	}
	QHash<int, bool> portStates;
	for (auto& future : portFutures) {
		const auto result = future.get();
		portStates.insert(result.first, result.second);
	}

	QVector<LibRow> rows;
	for (const QString& name : _names) {
		LibRow r;
		r.name = name;
		for (const QString& tag : _tags) {
			Cell c;
			const Instance* inst = InstanceAt(tag, name);
			if (inst) {
				c.present = true;
				c.sourceConfigured = inst->hasSource;
				c.port = inst->port;
				c.up = portStates.value(inst->port, false);
				const QString bin = inst->binary, db = bin + ".i64";
				const bool binEx = QFile::exists(bin);
				c.hasDb = QFile::exists(db);
				if (binEx) c.size = QFileInfo(bin).size();

				// Compare the analyzed copy against its configured Live source (by
				// content hash). This drives BOTH the "Update available" status
				// (for live-tracking tags) and the "vs source" Δ column.
				const bool liveEx = !inst->sourceBinary.isEmpty() && QFile::exists(inst->sourceBinary);
				if (liveEx) c.srcSize = QFileInfo(inst->sourceBinary).size();
				const bool i64stale = c.hasDb && binEx && QFileInfo(db).lastModified() < QFileInfo(bin).lastModified();
				c.srcDiff = liveEx && binEx && Sha256(bin) != Sha256(inst->sourceBinary);

				if (!binEx)
					c.state = "No binary";
				else if (inst->compareSource && c.srcDiff)
					c.state = "Update available";
				else if (!c.hasDb)
					c.state = "Not analyzed";
				else if (i64stale)
					c.state = "Re-analyze";
				else
					c.state = "Ready";

				c.tip = buildStatusTip(*inst, c, binEx, liveEx, i64stale);
			}
			r.cells << c;
		}
		rows << r;
	}
	emit StatusReady(rows);
}

void Manager::Start(const QVector<Target>& targets) {
	PollIdaProcesses();
	for (const Target& t : targets) {
		const auto resolved = ResolveTarget(t);
		if (!resolved) continue;
		const Instance* inst = &*resolved;
		const QString tag = QString("%1@%2").arg(inst->name, inst->tag);
		if (TargetBusy(t)) {
			emit Log(QString("[skip] %1: database is in use by another operation").arg(tag));
			continue;
		}
		if (PortUp(inst->port)) {
			emit Log(QString("[skip] %1 already up on %2").arg(tag).arg(inst->port));
			continue;
		}
		if (!QFile::exists(inst->binary + ".i64")) {
			emit Log(QString("[warn] %1: no DB — Analyze first").arg(tag));
			continue;
		}
		if (!LaunchServer(*inst)) {
			emit Log(QString("[fail] %1: could not launch IDA").arg(tag));
			continue;
		}
		emit Log(QString("[start] %1 -> %2").arg(tag).arg(inst->port));
	}
	emit Log("start: done (heavy DBs bind after ~1 min).");
	Refresh();
}

// The database files IDA writes/opens. .i64/.idb is the packed database; the rest are
// the unpacked pieces that exist only while a database is open — IDA folds them back
// into the packed file and deletes them on a clean exit, but a force-killed process
// leaves them behind (hundreds of megabytes that also shadow the packed copy).
static const QStringList kUnpackedDbExts = {".id0", ".id1", ".id2", ".nam", ".til"};
static const QStringList kDbExts = QStringList{".i64", ".idb"} + kUnpackedDbExts;

// How long one Stop batch waits for IDA to close its databases on its own before
// force-killing: long enough for a normal pack, short enough that a wedged IDA does
// not make Stop feel broken. The window is shared by the whole batch, so stopping
// eleven servers is no slower than stopping one.
static const int kStopGraceMs = 8000;

#ifdef Q_OS_WIN
namespace {
	struct CloseRequest {
		DWORD pid = 0;
		int posted = 0;
	};

	BOOL CALLBACK postCloseToWindow(HWND window, LPARAM parameter) {
		auto* request = reinterpret_cast<CloseRequest*>(parameter);
		DWORD owner = 0;
		GetWindowThreadProcessId(window, &owner);
		// Only real windows (IDA is launched minimized, which still counts as visible):
		// hidden top-level windows are internal helpers that should not be poked, and
		// finding none simply falls back to the kill.
		if (owner != request->pid || !IsWindowVisible(window)) return TRUE;
		if (PostMessageW(window, WM_CLOSE, 0, 0)) ++request->posted;
		return TRUE;
	}
} // namespace
#endif

// Ask a process to close itself instead of killing it, so IDA gets to save and pack
// its database. Returns false when nothing could be asked (no window of ours, signal
// refused) and the caller should go straight to the kill.
static bool requestProcessClose(qint64 pid) {
	if (pid <= 0) return false;
#ifdef Q_OS_WIN
	CloseRequest request{static_cast<DWORD>(pid), 0};
	EnumWindows(&postCloseToWindow, reinterpret_cast<LPARAM>(&request));
	return request.posted > 0;
#else
	return ::kill(static_cast<pid_t>(pid), SIGTERM) == 0;
#endif
}

// Sweep the unpacked database left next to `bin`, returning the bytes freed and the
// extensions removed. Refuses when there is no packed database to fall back on: those
// files are then the only copy of the work (an aborted import), and IDA can still pack
// them itself on the next open.
static qint64 removeUnpackedDatabase(const QString& bin, QStringList* removed, bool* keptUnpacked) {
	bool present = false;
	for (const QString& extension : kUnpackedDbExts)
		if (QFile::exists(bin + extension)) { present = true; break; }
	if (!present) return 0;
	if (!QFile::exists(bin + ".i64") && !QFile::exists(bin + ".idb")) {
		if (keptUnpacked) *keptUnpacked = true;
		return 0;
	}
	qint64 freed = 0;
	for (const QString& extension : kUnpackedDbExts) {
		const QString part = bin + extension;
		if (!QFile::exists(part)) continue;
		const qint64 size = QFileInfo(part).size();
		if (QFile::remove(part)) {
			freed += size;
			if (removed) *removed << extension;
		}
	}
	return freed;
}

void Manager::Stop(const QVector<Target>& targets) {
	struct Stopping {
		QString label, binary;
		int port = 0;
		qint64 pid = 0;
		bool asked = false; // a close request was delivered, so its exit is worth waiting for
	};
	QVector<Stopping> stopping;
	for (const Target& t : targets) {
		const auto resolved = ResolveTarget(t);
		if (!resolved) continue;
		const Instance* inst = &*resolved;
		const qint64 pid = PidOnPort(inst->port);
		if (pid <= 0) continue;
		if (!IsExpectedServerProcess(pid)) {
			emit Log(QString("[refused] port %1 belongs to an unexpected process (pid %2); not killed")
						 .arg(inst->port)
						 .arg(pid));
			continue;
		}
		stopping.push_back({QString("%1@%2").arg(inst->name, inst->tag), inst->binary, inst->port, pid, false});
	}
	// Ask everything to close first, then wait once. A clean IDA exit packs the database
	// back into the .i64 (keeping whatever the MCP session changed) and removes the
	// unpacked pieces; a kill leaves both the work and those files behind.
	for (Stopping& target : stopping) target.asked = requestProcessClose(target.pid);
	if (std::any_of(stopping.cbegin(), stopping.cend(), [](const Stopping& s) { return s.asked; })) {
		QElapsedTimer grace;
		grace.start();
		while (grace.elapsed() < kStopGraceMs) {
			bool anyAlive = false;
			for (const Stopping& target : stopping)
				if (target.asked && processRunning(target.pid)) { anyAlive = true; break; }
			if (!anyAlive) break;
			QThread::msleep(100);
		}
	}
	int n = 0;
	for (const Stopping& target : stopping) {
		if (!processRunning(target.pid)) {
			emit Log(QString("stopped %1 (port %2, pid %3) — IDA closed its database itself")
						 .arg(target.label)
						 .arg(target.port)
						 .arg(target.pid));
			++n;
		} else if (!IsExpectedServerProcess(target.pid)) {
			// The grace window widens the gap between looking the pid up and killing it,
			// so confirm once more that it is still our IDA and not a recycled pid.
			emit Log(QString("[refused] %1: pid %2 is no longer our IDA process; not killed").arg(target.label).arg(target.pid));
			continue;
		} else if (KillPid(target.pid)) {
			emit Log(QString("stopped %1 (port %2, pid %3)").arg(target.label).arg(target.port).arg(target.pid));
			++n;
		} else {
			emit Log(QString("[fail] could not stop %1 (pid %2)").arg(target.label).arg(target.pid));
			continue;
		}
		QStringList removed;
		bool keptUnpacked = false;
		const qint64 freed = removeUnpackedDatabase(target.binary, &removed, &keptUnpacked);
		if (!removed.isEmpty())
			emit Log(QString("[cleanup] %1: removed the unpacked database IDA left behind (%2): %3")
						 .arg(target.label, mbStr(freed), removed.join(' ')));
		else if (keptUnpacked)
			emit Log(QString("[warn] %1: an unpacked database is present but there is no packed .i64 — kept it; "
							 "open it in IDA to pack it")
						 .arg(target.label));
	}
	emit Log(QString("stop: %1 instance(s).").arg(n));
	Refresh();
}

// Move a target's database aside (<part>.ida-workbench.bak), first recovering any
// leftover backup from an earlier crash. Returns the extensions backed up, or
// reports failure via *err (and rolls its own partial work back).
static bool backupDatabase(const QString& bin, QStringList* backedUp, QString* err) {
	for (const QString& e : kDbExts) {
		const QString dbPart = bin + e;
		const QString backup = dbPart + ".ida-workbench.bak";
		if (QFile::exists(backup)) { // recover a known-good DB left by an earlier crash
			QFile::remove(dbPart);
			if (!QFile::rename(backup, dbPart)) {
				*err = "cannot recover previous backup";
				break;
			}
		}
		if (QFile::exists(dbPart)) {
			if (QFile::rename(dbPart, backup))
				*backedUp << e;
			else {
				*err = "cannot back up the existing database (is it open in IDA?)";
				break;
			}
		}
	}
	if (err->isEmpty()) return true;
	for (const QString& e : *backedUp) QFile::rename(bin + e + ".ida-workbench.bak", bin + e);
	backedUp->clear();
	return false;
}

// Put backed-up database parts back and (optionally) restore the original binary,
// preserving its timestamp so the restored DB is not seen as stale.
static void restoreDatabase(const QString& bin, const QStringList& backedUp, const QString& binaryBackup, const QDateTime& binMtime) {
	for (const QString& e : kDbExts) QFile::remove(bin + e);
	for (const QString& e : backedUp) QFile::rename(bin + e + ".ida-workbench.bak", bin + e);
	if (QFile::exists(binaryBackup)) {
		QString err;
		if (copyFileAtomically(binaryBackup, bin, &err)) {
			QFile::remove(binaryBackup);
			if (binMtime.isValid()) {
				QFile f(bin);
				if (f.open(QIODevice::ReadWrite)) {
					f.setFileTime(binMtime, QFileDevice::FileModificationTime);
					f.close();
				}
			}
		}
	}
}

// Human-friendly elapsed time: "42s" under a minute, "2m 05s" beyond.
static QString fmtDuration(qint64 ms) {
	const qint64 s = (ms + 500) / 1000;
	return s < 60 ? QString("%1s").arg(s) : QString("%1m %2s").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}


struct Manager::ActiveAnalyze {
	Target target;
	QString label, bin, db, binaryBackup;
	QDateTime savedBinMtime;
	QStringList backups;
	QProcess* process = nullptr;
	QTimer* timeout = nullptr;
	QElapsedTimer clock;
	QByteArray tail;
	bool stopped = false;
	bool timedOut = false;
};

void Manager::Analyze(const QVector<Target>& targets, bool force) {
	PollIdaProcesses();
	int launched = 0, skipped = 0;
	for (const Target& target : targets) {
		if (!target.revision.isEmpty()) {
			emit Log(QString("[skip] %1@%2: stored revisions are read-only").arg(target.name, target.tag));
			++skipped;
			continue;
		}
		const QString key = TargetKey(target);
		const auto resolved = ResolveTarget(target);
		if (!resolved) { ++skipped; continue; }
		const Instance inst = *resolved;
		const QString label = QString("%1@%2").arg(inst.name, inst.tag);
		if (TargetBusy(target)) {
			emit Log(QString("[skip] %1: database is already in use").arg(label));
			++skipped;
			continue;
		}
		if (PortUp(inst.port, 100)) {
			emit Log(QString("[skip] %1: server running - stop it first").arg(label));
			++skipped;
			continue;
		}

		const QString bin = inst.binary;
		const QString db = bin + ".i64";
		const QString binaryBackup = bin + ".ida-workbench.binary.bak";
		QDateTime savedBinMtime;
		if (QFile::exists(binaryBackup)) {
			QString error;
			if (!copyFileAtomically(binaryBackup, bin, &error)) {
				emit Log(QString("[fail] %1: cannot recover binary backup: %2").arg(label, error));
				++skipped;
				continue;
			}
			QFile::remove(binaryBackup);
		}
		if (QFile::exists(db) && !force) {
			emit Log(QString("[skip] %1: DB exists - use force").arg(label));
			++skipped;
			continue;
		}
		const bool pullSource = inst.hasSource && !QFile::exists(bin);
		if (pullSource && !QFile::exists(inst.sourceBinary)) {
			emit Log(QString("[skip] %1: source file is missing: %2").arg(label, inst.sourceBinary));
			++skipped;
			continue;
		}
		if (!inst.hasSource && !QFile::exists(bin)) {
			emit Log(QString("[skip] %1: no binary at %2").arg(label, bin));
			++skipped;
			continue;
		}

		QStringList backups;
		QString backupError;
		if (!backupDatabase(bin, &backups, &backupError)) {
			emit Log(QString("[fail] %1: %2").arg(label, backupError));
			++skipped;
			continue;
		}
		if (pullSource) {
			QString error;
			if (QFile::exists(bin)) savedBinMtime = QFileInfo(bin).lastModified();
			if (QFile::exists(bin) && !copyFileAtomically(bin, binaryBackup, &error)) {
				restoreDatabase(bin, backups, QString(), QDateTime());
				emit Log(QString("[fail] %1: cannot back up binary: %2").arg(label, error));
				++skipped;
				continue;
			}
			if (!copyFileAtomically(inst.sourceBinary, bin, &error)) {
				restoreDatabase(bin, backups, binaryBackup, savedBinMtime);
				emit Log(QString("[fail] %1: copy failed: %2").arg(label, error));
				++skipped;
				continue;
			}
		}

		QStringList args = _analysisArgs.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
		args.removeAll("-B");
		if (!args.contains("-A")) args.prepend("-A");
		args << ("-S" + AnalyzePy()) << ("-o" + db) << bin;

		auto* job = new ActiveAnalyze;
		job->target = target;
		job->label = label;
		job->bin = bin;
		job->db = db;
		job->binaryBackup = binaryBackup;
		job->savedBinMtime = savedBinMtime;
		job->backups = backups;
		job->process = new QProcess(this);
		job->timeout = new QTimer(this);
		job->timeout->setSingleShot(true);
#ifdef Q_OS_WIN
		job->process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
#endif
		connect(job->process, &QProcess::readyReadStandardOutput, this, [job] {
			job->tail += job->process->readAllStandardOutput();
			if (job->tail.size() > 4096) job->tail = job->tail.right(4096);
		});
		connect(job->process, &QProcess::readyReadStandardError, this, [job] {
			job->tail += job->process->readAllStandardError();
			if (job->tail.size() > 4096) job->tail = job->tail.right(4096);
		});
		connect(job->process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
			[this, key](int, QProcess::ExitStatus) { FinalizeAnalyze(key); });
		connect(job->process, &QProcess::errorOccurred, this, [this, key](QProcess::ProcessError error) {
			if (error == QProcess::FailedToStart) QTimer::singleShot(0, this, [this, key] { FinalizeAnalyze(key); });
		});
		connect(job->timeout, &QTimer::timeout, this, [this, key] {
			ActiveAnalyze* active = _analyzeJobs.value(key, nullptr);
			if (!active) return;
			active->timedOut = true;
			active->process->kill();
		});
		_analyzeJobs.insert(key, job);
		job->clock.start();
		emit AnalyzeStarted(target);
		emit OperationChanged(target, "analyze", true);
		emit Log(QString("[analyze] %1: launching %2").arg(label, QDir::toNativeSeparators(_idaText)));
		job->process->start(_idaText, args);
		job->timeout->start(2 * 60 * 60 * 1000);
		++launched;
	}
	emit Log(launched ? QString("analyze: %1 started in parallel%2.").arg(launched)
		.arg(skipped ? QString(", %1 skipped").arg(skipped) : QString()) :
		QString("analyze: nothing to do (%1 skipped).").arg(skipped));
}

void Manager::StopOperations(const QVector<Target>& targets) {
	int count = 0;
	for (const Target& target : targets) {
		ActiveAnalyze* job = _analyzeJobs.value(TargetKey(target), nullptr);
		if (!job) continue;
		job->stopped = true;
		job->process->kill();
		++count;
	}
	if (count) emit Log(QString("stop: stopping %1 analysis task(s)...").arg(count));
}

void Manager::FinalizeAnalyze(const QString& key) {
	ActiveAnalyze* job = _analyzeJobs.take(key);
	if (!job) return;
	job->timeout->stop();
	job->tail += job->process->readAllStandardError() + job->process->readAllStandardOutput();
	QFile::remove(job->bin + ".asm");
	const bool success = !job->stopped && !job->timedOut &&
		job->process->exitStatus() == QProcess::NormalExit && job->process->exitCode() == 0 && QFile::exists(job->db);
	if (success) {
		for (const QString& extension : job->backups) QFile::remove(job->bin + extension + ".ida-workbench.bak");
		QFile::remove(job->binaryBackup);
		emit Log(QString("[ok] %1 (%2) analyzed in %3")
			.arg(job->label, mbStr(QFileInfo(job->bin).size()), fmtDuration(job->clock.elapsed())));
	} else {
		restoreDatabase(job->bin, job->backups, job->binaryBackup, job->savedBinMtime);
		if (job->stopped)
			emit Log(QString("[stopped] %1: analysis stopped; previous database restored").arg(job->label));
		else if (job->timedOut)
			emit Log(QString("[fail] %1: analysis timed out; previous database restored").arg(job->label));
		else
			emit Log(QString("[fail] %1 (idat rc=%2) %3; previous database restored")
				.arg(job->label).arg(job->process->exitCode()).arg(QString::fromLocal8Bit(job->tail).right(300).trimmed()));
	}
	emit AnalyzeFinished(job->target, success);
	emit OperationChanged(job->target, "analyze", false);
	QObject::disconnect(job->process, nullptr, this, nullptr);
	QObject::disconnect(job->timeout, nullptr, this, nullptr);
	job->process->deleteLater();
	job->timeout->deleteLater();
	delete job;
	Refresh();
}

Manager::~Manager() {
	for (ActiveAnalyze* job : _analyzeJobs) {
		job->stopped = true;
		QObject::disconnect(job->process, nullptr, this, nullptr);
		QObject::disconnect(job->timeout, nullptr, this, nullptr);
		if (job->process->state() != QProcess::NotRunning) {
			job->process->kill();
			job->process->waitForFinished(5000);
		}
		restoreDatabase(job->bin, job->backups, job->binaryBackup, job->savedBinMtime);
		delete job->process;
		delete job->timeout;
		delete job;
	}
	_analyzeJobs.clear();
}

void Manager::Replace(const QVector<Target>& targets) {
	PollIdaProcesses();
	int n = 0;
	const QString revision = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz");
	for (const Target& t : targets) {
		const auto resolved = ResolveTarget(t);
		if (!resolved) continue;
		const Instance* inst = &*resolved;
		const QString tag = QString("%1@%2").arg(inst->name, inst->tag);
		if (TargetBusy(t)) {
			emit Log(QString("[skip] %1: database is in use by another operation").arg(tag));
			continue;
		}
		bool steamWorkspace = false;
		for (const Workspace& workspace : _workspaces) if (workspace.tag == inst->tag) { steamWorkspace = workspace.depot.enabled; break; }
		if (steamWorkspace) {
			emit Log(QString("[skip] %1: Steam workspace files are changed only by Depot update").arg(tag));
			continue;
		}
		if (!inst->storedVersion.isEmpty()) {
			emit Log(QString("[skip] %1: archived revisions are read-only; select Current to replace").arg(tag));
			continue;
		}
		if (!inst->hasSource || inst->sourceBinary.isEmpty()) {
			emit Log(QString("[skip] %1: no live source").arg(tag));
			continue;
		}
		if (PortUp(inst->port, 100)) {
			emit Log(QString("[skip] %1: server running — stop it first").arg(tag));
			continue;
		}

		const QString local = inst->binary, src = inst->sourceBinary;
		const QString revisionsRoot = QDir(inst->outputRoot).filePath("revisions");
		if (!QDir().mkpath(revisionsRoot)) {
			emit Log(QString("[fail] %1: cannot create revisions directory: %2").arg(tag, revisionsRoot));
			continue;
		}
		// Every IDA database file: .i64/.idb is the packed DB; the rest are the
		// unpacked pieces present only while a DB is open.
		static const QStringList DB_EXTS = {".i64", ".idb", ".id0", ".id1", ".id2", ".nam", ".til"};

		// Whether the SOURCE folder already carries an IDA database next to the
		// binary decides the mode:
		//   source has DB  -> full replace: copy binary + its DB (ready immediately).
		//   source has none-> fresh binary only: wipe the destination's now-stale
		//                     DB so the cell drops to "Not analyzed".
		bool sourceHasDb = false;
		for (const QString& e : DB_EXTS)
			if (QFile::exists(src + e)) {
				sourceHasDb = true;
				break;
			}

		const bool binaryIdentical = QFile::exists(local) && Sha256(local) == Sha256(src);

		// Nothing to do only when the binary already matches AND the source has no
		// database to hand over.
		if (binaryIdentical && !sourceHasDb) {
			emit Log(QString("[skip] %1: already identical to live source (no database to copy)").arg(tag));
			continue;
		}

		// Preserve the exact working state before changing it. One Replace batch
		// shares one timestamp, so all changed files form a coherent revision.
		const QString revisionDir = QDir(revisionsRoot).filePath(revision);
		const QString revisionBinary = QDir(revisionDir).filePath(inst->relativeFile);
		bool archivedAny = false;
		bool archiveOk = true;
		auto archive = [&](const QString& path, const QString& destination) {
			if (!QFile::exists(path)) return true;
			QString archiveError;
			if (!copyFileAtomically(path, destination, &archiveError)) {
				emit Log(QString("[fail] %1: cannot create revision %2: %3").arg(tag, revision, archiveError));
				return false;
			}
			archivedAny = true;
			return true;
		};
		archiveOk = archive(local, revisionBinary);
		for (const QString& ext : DB_EXTS)
			if (archiveOk) archiveOk = archive(local + ext, revisionBinary + ext);
		if (!archiveOk) continue;
		if (archivedAny) emit Log(QString("[revision] %1 -> %2").arg(tag, revision));

		// 1) Refresh the binary when it differs (QSaveFile keeps it atomic).
		if (!binaryIdentical) {
			QString copyError;
			if (!copyFileAtomically(src, local, &copyError)) {
				emit Log(QString("[fail] %1: copy failed: %2").arg(tag, copyError));
				continue;
			}
		}

		// 2) Clear the destination database, then mirror the source's if it has one.
		for (const QString& e : DB_EXTS) QFile::remove(local + e);
		QStringList carried;
		if (sourceHasDb)
			for (const QString& e : DB_EXTS)
				if (QFile::exists(src + e)) {
					QString dbCopyError;
					if (copyFileAtomically(src + e, local + e, &dbCopyError))
						carried << e;
					else
						emit Log(QString("[warn] %1: could not copy %2: %3").arg(tag, e, dbCopyError));
				}

		if (sourceHasDb && !binaryIdentical)
			emit Log(QString("[replaced] %1 <- source binary + database (%2) — ready, no re-analysis needed")
						 .arg(tag, carried.join(" ")));
		else if (sourceHasDb) // binary already matched, only the DB was synced
			emit Log(QString("[synced] %1 <- source database (%2), binary already identical — ready")
						 .arg(tag, carried.join(" ")));
		else // source had no DB -> stale destination DB removed
			emit Log(QString("[replaced] %1 <- source binary; stale database removed — press Analyze to rebuild")
						 .arg(tag));
		++n;
	}
	emit Log(QString("replace: %1 replaced.").arg(n));
	Refresh();
}

void Manager::UpdateDepot(const QStringList& requestedTags, const QString& requestedManifest,
	const QString& requestedUsername, const QString& requestedPassword, const QString& requestedAuthCode, bool rememberSession) {
	// Refusing here is the one exit that reports no depot lifecycle at all, so it must
	// stay unreachable from the UI (DoDepotUpdate holds the busy lock for the whole
	// run): a caller that got past that guard would be left waiting for a signal that
	// never comes. Every other exit below goes through finishDepot().
	if (!_depotTags.isEmpty()) {
		emit Log("[skip] a Depot update is already running");
		return;
	}
	_stopDepotRequested = false;
	QStringList activeTags = requestedTags;
	activeTags.removeDuplicates();
	if (activeTags.isEmpty())
		for (const Workspace& workspace : _workspaces)
			if (workspace.depot.enabled) activeTags << workspace.tag;
	// Announce the operation before anything can fail: the UI locks its toolbar when
	// the update is requested and this signal pair is what releases it again, so every
	// exit path below must go through finishDepot() — including the argument check.
	for (const QString& tag : activeTags) {
		_depotTags.insert(tag);
		emit WorkspaceOperationChanged(tag, "depot", true);
	}
	auto finishDepot = [this, activeTags] {
		for (const QString& tag : activeTags) {
			_depotTags.remove(tag);
			emit WorkspaceOperationChanged(tag, "depot", false);
		}
	};
	const QString manifestSelection = requestedManifest.trimmed();
	if (manifestSelection.compare("latest", Qt::CaseInsensitive) != 0 &&
		!QRegularExpression("^[0-9]+$").match(manifestSelection).hasMatch()) {
		emit Log("[fail] Manifest selection must be Latest or a numeric ManifestID");
		finishDepot();
		return;
	}
	QString depotExecutable = resolveExecutable(_depotDownloader.executable);
	bool downloaderConfigChanged = false;
	if (depotExecutable.isEmpty()) {
		const QString configured = _depotDownloader.executable.trimmed();
		const bool automatic = configured.isEmpty() || configured.compare("DepotDownloader.exe", Qt::CaseInsensitive) == 0 ||
			configured.compare("DepotDownloader", Qt::CaseInsensitive) == 0;
		if (!automatic) {
			emit Log(QString("[fail] DepotDownloader executable does not exist: %1 (change it in Settings)").arg(configured));
			finishDepot();
			return;
		}
		emit Log("[depot] DepotDownloader was not found; installing DepotDownloader 3.4.0...");
		QString error;
		if (!bootstrapDepotDownloader(_configPath, &depotExecutable, &error)) {
			emit Log(QString("[fail] DepotDownloader installation failed: %1").arg(error));
			finishDepot();
			return;
		}
		_depotDownloader.executable = QDir::toNativeSeparators(depotExecutable);
		downloaderConfigChanged = true;
		emit Log(QString("[depot] DepotDownloader installed: %1").arg(_depotDownloader.executable));
	}

	auto run = [this, depotExecutable](const QStringList& args, QString* output, const QByteArray& input = QByteArray()) {
		QProcess process;
		QByteArray captured;
		process.setProcessChannelMode(QProcess::MergedChannels);
		process.start(depotExecutable, args);
		if (!process.waitForStarted(10000)) {
			*output = process.errorString();
			return false;
		}
		if (!input.isEmpty()) {
			process.write(input);
			process.waitForBytesWritten(3000);
			process.closeWriteChannel();
		}
		QElapsedTimer timer;
		timer.start();
		while (!process.waitForFinished(250)) {
			captured += process.readAll();
			QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
			if (captured.size() > 16384) captured = captured.right(16384);
			if (_stopDepotRequested.load() || timer.elapsed() > qint64(_depotDownloader.timeoutMinutes) * 60000) {
				process.kill();
				process.waitForFinished(3000);
				*output = _stopDepotRequested.load() ? "stopped" : "timed out";
				return false;
			}
		}
		captured += process.readAll();
		*output = QString::fromUtf8(captured);
		return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
	};
	QStringList tags = requestedTags;
	tags.removeDuplicates();
	int updated = 0;
	int imported = 0;
	bool configChanged = downloaderConfigChanged;
	for (Workspace& workspace : _workspaces) {
		if (!workspace.depot.enabled || (!tags.isEmpty() && !tags.contains(workspace.tag))) continue;
		if (_stopDepotRequested.load()) break;
		const QString label = workspace.tag;
		const DepotLayout layout = depotLayout(workspace.depot.appId, workspace.depot.os);
		auto depotPath = [&layout](const QString& relative) {
			return layout.pathPrefix.isEmpty() ? QDir::fromNativeSeparators(relative) :
				QDir::fromNativeSeparators(layout.pathPrefix + "/" + relative);
		};
		// Which binaries of this workspace are held right now, by absolute path. A depot
		// update installs a brand-new <ManifestID> directory, so a module held under some
		// other manifest is no reason to refuse the whole workspace: the paths matter only
		// against the directory this update would replace, checked once that is known.
		auto heldBinaries = [this, &label](const auto& jobs) {
			QSet<QString> paths;
			for (auto it = jobs.constBegin(); it != jobs.constEnd(); ++it) {
				const QStringList parts = it.key().split('\t');
				if (parts.value(0) != label) continue;
				const auto held = ResolveTarget({parts.value(0), parts.value(1), parts.value(2)});
				if (held) paths.insert(QDir::cleanPath(QFileInfo(held->binary).absoluteFilePath()).toLower());
			}
			return paths;
		};
		const QSet<QString> analyzing = heldBinaries(_analyzeJobs);
		const QSet<QString> openInIda = heldBinaries(_idaPids);
		// What holds this exact binary right now, or an empty string. Overwriting a file
		// under a live session would pull the ground from under it; adding the modules
		// beside it is what a completion is for, so the check is per file, never per tag.
		auto heldBy = [this, &label, &analyzing, &openInIda](const QString& path) {
			const QString key = QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower();
			if (analyzing.contains(key)) return QStringLiteral("analyzing");
			if (openInIda.contains(key)) return QStringLiteral("open in IDA");
			for (const Instance& instance : _instances)
				if (instance.tag == label &&
					QDir::cleanPath(QFileInfo(instance.binary).absoluteFilePath()).toLower() == key &&
					PidOnPort(instance.port) > 0)
					return QStringLiteral("MCP server running");
			return QString();
		};
		// A server keeps the database it was started with, so a new Current does not move
		// it: say so, instead of leaving a row that reads UP next to a fresh binary.
		auto warnStaleServers = [this, &label](const QString& selectedManifest) {
			QStringList serving;
			for (const Instance& instance : _instances)
				if (instance.tag == label && PidOnPort(instance.port) > 0) serving << instance.name;
			if (!serving.isEmpty())
				emit Log(QString("[warn] %1: %2 still serve the previous manifest — restart those MCP servers to pick up %3")
					.arg(label, serving.join(", "), selectedManifest));
		};
		emit Log(QString("[depot] %1: checking app %2 (%3), depot %4").arg(label).arg(workspace.depot.appId).arg(workspace.depot.os).arg(layout.depotId));

		QTemporaryDir manifestDir(QDir(workspace.output).filePath(".depot-manifest-XXXXXX"));
		QStringList common{"-app", QString::number(workspace.depot.appId), "-depot", QString::number(layout.depotId)};
		if (!workspace.depot.os.isEmpty()) common << "-os" << workspace.depot.os;
		const QString username = requestedUsername.trimmed().isEmpty() ? qEnvironmentVariable("STEAM_USERNAME").trimmed() : requestedUsername.trimmed();
		const QString password = requestedPassword.isEmpty() ? qEnvironmentVariable("STEAM_PASSWORD") : requestedPassword;
		const QString authCode = requestedAuthCode.trimmed().isEmpty() ? qEnvironmentVariable("STEAM_GUARD_CODE").trimmed() : requestedAuthCode.trimmed();
		const QByteArray authInput = authCode.isEmpty() ? QByteArray() : authCode.toUtf8() + '\n';
		QString manifestOutput;
		QStringList manifestArgs = common;
		manifestArgs << "-manifest-only" << "-dir" << manifestDir.path();
		if (!run(manifestArgs, &manifestOutput, authInput)) {
			emit Log(QString("[fail] %1: DepotDownloader manifest query failed: %2").arg(label, manifestOutput.trimmed()));
			continue;
		}
		const QRegularExpressionMatch manifestMatch = QRegularExpression("\\bManifest\\s+([0-9]+)", QRegularExpression::CaseInsensitiveOption).match(manifestOutput);
		if (!manifestMatch.hasMatch()) {
			emit Log(QString("[fail] %1: DepotDownloader did not report a manifest GID").arg(label));
			continue;
		}
		const QString latestManifest = manifestMatch.captured(1);
		const QString manifest = manifestSelection.isEmpty() || manifestSelection.compare("latest", Qt::CaseInsensitive) == 0
			? latestManifest : manifestSelection;
		const bool historicalImport = manifest != latestManifest;
		QStringList downloadFiles = workspace.files;
		if (!historicalImport) {
			QFile catalog;
			const QStringList catalogs = QDir(manifestDir.path()).entryList({QString("manifest_%1_%2.txt").arg(layout.depotId).arg(latestManifest)}, QDir::Files);
			if (!catalogs.isEmpty()) catalog.setFileName(QDir(manifestDir.path()).filePath(catalogs.first()));
			if (catalog.open(QIODevice::ReadOnly | QIODevice::Text)) {
				const QString manifestText = QString::fromUtf8(catalog.readAll()).replace('\\', '/');
				QStringList absent;
				for (const QString& relative : workspace.files) {
					const QString path = depotPath(relative).replace('\\', '/');
					if (!QRegularExpression(QString("(?mi)\\s%1\\s*$").arg(QRegularExpression::escape(path))).match(manifestText).hasMatch()) absent << relative;
				}
				for (const QString& relative : absent) downloadFiles.removeAll(relative);
				if (!absent.isEmpty())
					emit Log(QString("[depot] %1: not present in manifest %2: %3").arg(label, manifest, absent.join(", ")));
			}
		}
		const QString manifestRoot = QDir(workspace.output).filePath(manifest);
		bool complete = QDir(manifestRoot).exists();
		for (const QString& relative : historicalImport ? QStringList{} : downloadFiles)
			if (!QFile::exists(QDir(manifestRoot).filePath(relative))) { complete = false; break; }
		if (complete) {
			QString patchVersion, serverVersion;
			readSteamVersions(QDir(manifestRoot).filePath("csgo/steam.inf"), &patchVersion, &serverVersion);
			if (historicalImport) {
				emit Log(QString("[depot] %1: manifest %2 is already stored").arg(label, manifest));
				continue;
			}
			const bool manifestChanged = workspace.depot.manifest != manifest;
			const bool changed = manifestChanged || workspace.depot.patchVersion != patchVersion || workspace.depot.serverVersion != serverVersion;
			workspace.depot.manifest = manifest;
			workspace.depot.patchVersion = patchVersion;
			workspace.depot.serverVersion = serverVersion;
			configChanged = configChanged || changed;
			if (changed) ++updated;
			emit Log(QString("[depot] %1: manifest %2 selected as Current").arg(label, manifest));
			if (manifestChanged) warnStaleServers(manifest);
			continue;
		}

		QTemporaryDir stage(QDir(workspace.output).filePath(".depot-stage-XXXXXX"));
		if (!stage.isValid()) {
			emit Log(QString("[fail] %1: cannot create download staging directory").arg(label));
			continue;
		}
		const QString fileListPath = QDir(stage.path()).filePath("files.txt");
		QFile fileList(fileListPath);
		if (!fileList.open(QIODevice::WriteOnly | QIODevice::Text)) {
			emit Log(QString("[fail] %1: cannot create DepotDownloader file list").arg(label));
			continue;
		}
		QTextStream stream(&fileList);
		for (const QString& relative : downloadFiles) stream << depotPath(relative) << '\n';
		const QString versionRelative = "csgo/steam.inf";
		if (!workspace.files.contains(versionRelative)) stream << depotPath(versionRelative) << '\n';
		fileList.close();

		QStringList downloadArgs = common;
		if (!username.isEmpty()) {
			downloadArgs << "-username" << username;
			if (!password.isEmpty()) downloadArgs << "-password" << password;
			if (rememberSession) downloadArgs << "-remember-password";
			if (!authCode.isEmpty()) downloadArgs << "-no-mobile";
		}
		// DepotDownloader's normal anonymous path resolves the current manifest itself.
		// Pinning even the latest GID with -manifest unnecessarily requires a manifest request code.
		if (historicalImport) downloadArgs << "-manifest" << manifest;
		downloadArgs << "-filelist" << fileListPath << "-dir" << stage.path() << "-validate";
		QString downloadOutput;
		emit Log(QString("[depot] %1: downloading manifest %2 (%3 files)").arg(label, manifest).arg(downloadFiles.size()));
		if (!run(downloadArgs, &downloadOutput, authInput)) {
			const bool authenticationRequired = historicalImport &&
				(downloadOutput.contains("401", Qt::CaseInsensitive) || downloadOutput.contains("No manifest request code", Qt::CaseInsensitive));
			const bool loginSucceeded = !username.isEmpty() &&
				(downloadOutput.contains("Got session token", Qt::CaseInsensitive) || downloadOutput.contains("Got AppInfo", Qt::CaseInsensitive));
			if (authenticationRequired && loginSucceeded)
				emit Log(QString("[fail] %1: Steam login succeeded, but ManifestID %2 was not resolved for %3 depot %4. Verify that the ID belongs to this depot; otherwise the publisher has disabled that old manifest.")
					.arg(label, manifest, workspace.depot.os, QString::number(layout.depotId)));
			else if (authenticationRequired && username.isEmpty())
				emit Log(QString("[fail] %1: ManifestID %2 was not resolved anonymously for %3 depot %4. Verify the depot/OS; a valid old ID may require Steam account access.")
					.arg(label, manifest, workspace.depot.os, QString::number(layout.depotId)));
			else if (authenticationRequired)
				emit Log(QString("[fail] %1: Steam authentication for '%2' did not complete, or ManifestID %3 does not belong to %4 depot %5. Check the credentials, Guard code and depot/OS.")
					.arg(label, username, manifest, workspace.depot.os, QString::number(layout.depotId)));
			else
				emit Log(QString("[fail] %1: DepotDownloader failed: %2").arg(label, downloadOutput.right(1000).trimmed()));
			continue;
		}
		QStringList missing;
		for (const QString& relative : downloadFiles)
			if (!QFileInfo::exists(QDir(stage.path()).filePath(depotPath(relative)))) missing << relative;
		if (!missing.isEmpty()) {
			if (!historicalImport) {
				emit Log(QString("[fail] %1: incomplete depot download; missing: %2").arg(label, missing.join(", ")));
				continue;
			}
			emit Log(QString("[depot] %1: paths absent from historical manifest %2: %3").arg(label, manifest, missing.join(", ")));
			for (const QString& relative : missing) downloadFiles.removeAll(relative);
		}

		const QString stagedSteamInf = QDir(stage.path()).filePath(depotPath(versionRelative));
		QString patchVersion, serverVersion;
		readSteamVersions(stagedSteamInf, &patchVersion, &serverVersion);
		if (!historicalImport && workspace.depot.appId == 730 && (patchVersion.isEmpty() || serverVersion.isEmpty())) {
			QByteArray steamInf;
			QString steamInfError;
			if (downloadUrl(QUrl("https://raw.githubusercontent.com/SteamDatabase/GameTracking-CS2/master/game/csgo/steam.inf"), 30000, &steamInf, &steamInfError)) {
				QDir().mkpath(QFileInfo(stagedSteamInf).absolutePath());
				QSaveFile file(stagedSteamInf);
				if (!file.open(QIODevice::WriteOnly) || file.write(steamInf) != steamInf.size() || !file.commit())
					emit Log(QString("[warn] %1: could not store GameTracking steam.inf: %2").arg(label, file.errorString()));
			} else {
				emit Log(QString("[warn] %1: GameTracking steam.inf is unavailable: %2").arg(label, steamInfError));
			}
			patchVersion.clear();
			serverVersion.clear();
			readSteamVersions(stagedSteamInf, &patchVersion, &serverVersion);
		}
		// A <ManifestID> directory that already exists is *completed*, file by file, rather
		// than swapped as a whole: the point of completing an interrupted download is to add
		// the modules that are missing next to the ones already analyzed, and a directory
		// swap would demand exclusive access to files a live MCP session or IDA legitimately
		// holds. A fresh directory has no such tenants, so it is still built aside and
		// renamed into place in one step.
		const bool completeInPlace = QDir(manifestRoot).exists();
		QTemporaryDir installStage(QDir(workspace.output).filePath(".manifest-install-XXXXXX"));
		const QString installRoot = completeInPlace ? manifestRoot : installStage.path();
		bool ok = completeInPlace || installStage.isValid();
		QString error;
		QStringList unchanged, inUse;
		for (const QString& relative : downloadFiles) {
			if (!ok) break;
			const QString incoming = QDir(stage.path()).filePath(depotPath(relative));
			const QString destination = QDir(installRoot).filePath(relative);
			if (completeInPlace && QFile::exists(destination)) {
				// Identical file: leave it — and its database, and whoever is using it — alone.
				if (Sha256(destination) == Sha256(incoming)) {
					unchanged << QFileInfo(relative).completeBaseName();
					continue;
				}
				const QString holder = heldBy(destination);
				if (!holder.isEmpty()) {
					inUse << QString("%1 (%2)").arg(QFileInfo(relative).completeBaseName(), holder);
					continue;
				}
			}
			ok = copyFileAtomically(incoming, destination, &error);
			if (!ok) break;

			// Re-analysis remains attached to the exact manifest only when its binary is unchanged.
			// Carrying the database over is an optimisation — having it saves an Analyze,
			// losing it costs one — so nothing below fails the update: with servers allowed
			// to run through an update, a live session legitimately holds the previous .i64.
			QStringList sidecarSources;
			if (!completeInPlace) sidecarSources << QDir(manifestRoot).filePath(relative); // now the destination itself
			if (!workspace.depot.manifest.isEmpty() && workspace.depot.manifest != manifest)
				sidecarSources << QDir(workspace.output).filePath(workspace.depot.manifest + "/" + relative);
			sidecarSources << QDir(workspace.output).filePath(relative);
			const QString moduleName = QFileInfo(relative).completeBaseName();
			for (const QString& previous : sidecarSources) {
				if (!QFile::exists(previous) || Sha256(previous) != Sha256(incoming)) continue;
				// An analyzer has that database moved aside and half-rewritten; copying it now
				// would plant a corrupt .i64 under the new manifest.
				if (analyzing.contains(QDir::cleanPath(QFileInfo(previous).absoluteFilePath()).toLower())) {
					emit Log(QString("[warn] %1: %2 is being analyzed — its database is not carried into manifest %3")
						.arg(label, moduleName, manifest));
					continue;
				}
				// Unpacked parts mean the database is open (or a kill left them): only the
				// packed file is a consistent snapshot, the session's own work is not ours.
				bool unpacked = false;
				for (const QString& ext : kUnpackedDbExts)
					if (QFile::exists(previous + ext)) { unpacked = true; break; }
				for (const QString& ext : unpacked ? QStringList{".i64", ".idb"} : kDbExts) {
					QString sidecarError;
					if (QFile::exists(previous + ext) && !QFile::exists(destination + ext) &&
						!copyFileAtomically(previous + ext, destination + ext, &sidecarError))
						emit Log(QString("[warn] %1: could not carry %2 into manifest %3: %4 — press Analyze there")
							.arg(label, QFileInfo(previous + ext).fileName(), manifest, sidecarError));
				}
				if (unpacked)
					emit Log(QString("[warn] %1: %2 has an unpacked database beside it (open in IDA, or left by a kill) — "
									 "manifest %3 gets only the packed copy")
						.arg(label, moduleName, manifest));
			}
		}
		if (ok && QFile::exists(stagedSteamInf) && !downloadFiles.contains(versionRelative))
			ok = copyFileAtomically(stagedSteamInf, QDir(installRoot).filePath(versionRelative), &error);
		if (!ok) {
			// In-place completion writes every file atomically on its own, so a failure here
			// leaves a directory that is merely still incomplete — the next run continues it.
			emit Log(QString("[fail] %1: cannot %2 manifest %3: %4")
				.arg(label, completeInPlace ? QStringLiteral("complete") : QStringLiteral("prepare"), manifest, error));
			continue;
		}
		if (!unchanged.isEmpty())
			emit Log(QString("[depot] %1: manifest %2 already had %3 — left untouched").arg(label, manifest, unchanged.join(", ")));
		if (!inUse.isEmpty())
			emit Log(QString("[skip] %1: %2 changed under manifest %3 but is in use — not replaced; stop it and update again")
				.arg(label, inUse.join(", "), manifest));

		if (!completeInPlace) {
			installStage.setAutoRemove(false);
			QDir outputDir(workspace.output);
			const QString stagedName = QFileInfo(installStage.path()).fileName();
			if (!outputDir.rename(stagedName, manifest)) {
				QDir(installStage.path()).removeRecursively();
				emit Log(QString("[fail] %1: cannot finalize manifest directory %2").arg(label, manifest));
				continue;
			}
		}

		if (historicalImport) {
			++imported;
			QStringList versionDetails;
			if (!patchVersion.isEmpty()) versionDetails << "PatchVersion " + patchVersion;
			if (!serverVersion.isEmpty()) versionDetails << "ServerVersion " + serverVersion;
			emit Log(QString("[manifest] %1 stored %2%3").arg(label, manifest,
				versionDetails.isEmpty() ? QString() : ": " + versionDetails.join(", ")));
		} else {
			const bool manifestChanged = workspace.depot.manifest != manifest;
			workspace.depot.manifest = manifest;
			workspace.depot.patchVersion = patchVersion;
			workspace.depot.serverVersion = serverVersion;
			configChanged = true;
			++updated;
			QStringList versionDetails;
			if (!patchVersion.isEmpty()) versionDetails << "PatchVersion " + patchVersion;
			if (!serverVersion.isEmpty()) versionDetails << "ServerVersion " + serverVersion;
			emit Log(QString("[depot] %1 Current is %2%3").arg(label, manifest,
				versionDetails.isEmpty() ? QString() : ": " + versionDetails.join(", ")));
			if (manifestChanged) warnStaleServers(manifest);
		}
	}

	if (configChanged) SaveConfig(View());
	emit Log(QString("depot update: %1 Current selected, %2 historical manifest(s) stored.").arg(updated).arg(imported));
	finishDepot();
	Refresh();
}

// --- view / save ------------------------------------------------------------

ConfigView Manager::View() const {
	ConfigView v;
	v.host = _host;
	v.idaGui = _idaGui;
	v.idaText = _idaText;
	v.logDir = _logDir;
	v.analysisArgs = _analysisArgs;
	v.configPath = _configPath;
	v.maxLogMB = _maxLogMB;
	v.basePort = _basePort;
	v.depotDownloader = _depotDownloader;
	for (const Workspace& workspace : _workspaces)
		(workspace.depot.enabled ? v.steamWorkspaces : v.workspaces) << workspace;
	v.extraLibs = _extraLibs;
	v.portOverrides = _portOverrides;
	return v;
}

void Manager::SaveConfig(const ConfigView& cfgIn) {
	ConfigView cfg = cfgIn;

	const QString validation = validateConfig(cfg);
	if (!validation.isEmpty()) {
		emit Log("save: " + validation);
		emit ConfigSaveFinished(false, validation);
		return;
	}
	const QString directoryError = prepareWorkspaceDirectories(cfg);
	if (!directoryError.isEmpty()) {
		emit Log("save: " + directoryError);
		emit ConfigSaveFinished(false, directoryError);
		return;
	}

	QJsonObject root;
	root["host"] = cfg.host.trimmed();
	root["logDir"] = cfg.logDir.trimmed();
	root["maxLogSizeMB"] = (cfg.maxLogMB >= 0 && cfg.maxLogMB <= MAX_LOG_MB_CEILING) ? cfg.maxLogMB : DEFAULT_MAX_LOG_MB;
	root["analysisArgs"] = cfg.analysisArgs.trimmed();
	root["scanBasePort"] = (cfg.basePort >= 1024 && cfg.basePort <= 60000) ? cfg.basePort : DEFAULT_BASE_PORT;
	root["ida"] = QJsonObject{{"gui", cfg.idaGui.trimmed()}, {"text", cfg.idaText.trimmed()}};
	root["depotDownloader"] = QJsonObject{
		{"executable", cfg.depotDownloader.executable.trimmed().isEmpty() ? "DepotDownloader.exe" : cfg.depotDownloader.executable.trimmed()},
		{"timeoutMinutes", qBound(1, cfg.depotDownloader.timeoutMinutes, 240)}};

	auto saveWorkspaces = [](const QVector<Workspace>& source, bool steam) {
		QJsonArray result;
		for (const Workspace& workspace : source) {
		if (workspace.tag.trimmed().isEmpty()) continue;
		QJsonObject object{{"tag", workspace.tag.trimmed()}};
		if (steam)
			object["dir"] = workspace.output.trimmed();
		else {
			object["source"] = workspace.source.trimmed();
			object["output"] = workspace.output.trimmed();
		}
		QJsonArray files;
		for (const QString& file : workspace.files)
			if (!file.trimmed().isEmpty()) files.append(QDir::cleanPath(file.trimmed()));
		object["files"] = files;
		object["portOffset"] = workspace.portOffset;
		object["color"] = workspace.color.trimmed().isEmpty() ? generatedTagColor(workspace.tag.trimmed()) : workspace.color.trimmed();
			if (steam) {
				object["appId"] = workspace.depot.appId;
				object["os"] = workspace.depot.os;
			if (!workspace.depot.manifest.isEmpty()) object["current"] = workspace.depot.manifest;
		}
		result.append(object);
		}
		return result;
	};
	root["workspaces"] = saveWorkspaces(cfg.workspaces, false);
	root["steamWorkspaces"] = saveWorkspaces(cfg.steamWorkspaces, true);

	// Hand-edited (tag, name) ports: write only genuine overrides (a port that
	// differs from the auto value) for a library the tag still tracks. Sorted so
	// the file diff is stable; the key is dropped entirely when none remain.
	auto tracks = [&cfg](const QString& tag, const QString& name) {
		for (const Workspace& workspace : AllWorkspaces(cfg))
			if (workspace.tag.trimmed() == tag)
				for (const QString& file : workspace.files)
					if (QFileInfo(file).completeBaseName() == name) return true;
		return false;
	};
	QVector<PortOverride> keep;
	for (const PortOverride& po : cfg.portOverrides) {
		const QString tg = po.tag.trimmed(), nm = po.name.trimmed();
		if (po.port <= 0 || !tracks(tg, nm)) continue; // drop orphaned overrides
		int autoPort = -1;
		for (const Workspace& workspace : AllWorkspaces(cfg))
			if (workspace.tag.trimmed() == tg) {
				for (int fileIndex = 0; fileIndex < workspace.files.size(); ++fileIndex)
					if (QFileInfo(workspace.files[fileIndex]).completeBaseName() == nm) {
						autoPort = AutoScanPort(cfg.basePort, fileIndex, workspace.portOffset);
						break;
					}
				break;
			}
		if (po.port == autoPort) continue;
		keep.push_back({tg, nm, po.port});
	}
	std::sort(keep.begin(), keep.end(), [](const PortOverride& a, const PortOverride& b) {
		return a.tag == b.tag ? a.name < b.name : a.tag < b.tag;
	});
	if (keep.isEmpty())
		root.remove("portOverrides");
	else {
		QJsonArray ov;
		for (const PortOverride& po : keep)
			ov.append(QJsonObject{{"tag", po.tag}, {"name", po.name}, {"port", po.port}});
		root["portOverrides"] = ov;
	}

	QJsonArray extra;
	for (int index = 0; index < cfg.extraLibs.size(); ++index) {
		const ExtraLib& e = cfg.extraLibs[index];
		if (e.tag.isEmpty() || e.path.isEmpty()) continue;
		QJsonObject o{{"tag", e.tag.trimmed()}, {"path", e.path.trimmed()}};
		if (e.port > 0 && e.port != AutoExtraPort(cfg.basePort, index))
			o["port"] = e.port;
		o["color"] = e.color.trimmed().isEmpty() ? generatedTagColor(e.tag.trimmed()) : e.color.trimmed();
		extra.append(o);
	}
	root["extraLibs"] = extra;

	QSaveFile f(_configPath);
	f.setDirectWriteFallback(false);
	if (!f.open(QIODevice::WriteOnly)) {
		emit Log(QString("save: cannot write %1: %2").arg(_configPath, f.errorString()));
		emit ConfigSaveFinished(false, QString("Cannot write %1: %2").arg(_configPath, f.errorString()));
		return;
	}
	const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
	if (f.write(json) != json.size() || !f.commit()) {
		emit Log(QString("save: cannot commit %1: %2").arg(_configPath, f.errorString()));
		emit ConfigSaveFinished(false, QString("Cannot save %1: %2").arg(_configPath, f.errorString()));
		return;
	}
	emit Log("config saved: " + QDir::toNativeSeparators(_configPath));

	QString err;
	if (!LoadConfig(_configPath, &err)) {
		emit Log("reload failed: " + err);
		emit ConfigSaveFinished(false, "Saved, but reload failed: " + err);
		return;
	}
	emit ConfigLoaded(View());
	emit ConfigSaveFinished(true, "Configuration saved.");
	Refresh();
}

// Replace config.json with an external file (Settings → Import…). The current
// file's bytes are held in memory first; an import that fails to parse or
// validate is rolled back from that snapshot, so a bad file can never brick the
// app — no on-disk backup copy is needed.
void Manager::ImportConfig(const QString& sourcePath) {
	const QString src = QFileInfo(sourcePath).absoluteFilePath();
	if (!QFile::exists(src)) {
		emit ConfigSaveFinished(false, "File not found: " + sourcePath);
		return;
	}
	if (src == _configPath) {
		emit ConfigSaveFinished(false, "That is already the active config file.");
		return;
	}
	QByteArray previous; // in-memory snapshot to roll back to
	if (QFile prev(_configPath); prev.open(QIODevice::ReadOnly)) previous = prev.readAll();

	QString copyError;
	if (!copyFileAtomically(src, _configPath, &copyError)) {
		emit ConfigSaveFinished(false, QString("Cannot import %1: %2").arg(sourcePath, copyError));
		return;
	}
	QString err;
	if (LoadConfig(_configPath, &err)) {
		emit Log("config imported: " + src);
		emit ConfigLoaded(View());
		emit ConfigSaveFinished(true, "Configuration imported.");
		Refresh();
		return;
	}
	// Import rejected — restore the previous config from the in-memory snapshot.
	bool rolledBack = false;
	if (!previous.isEmpty()) {
		QSaveFile restore(_configPath);
		QString rollbackError;
		if (restore.open(QIODevice::WriteOnly) && restore.write(previous) == previous.size() && restore.commit())
			rolledBack = LoadConfig(_configPath, &rollbackError);
	}
	emit Log("import rejected: " + err);
	emit ConfigSaveFinished(false, QString("Import rejected: %1\n\n%2").arg(err, rolledBack ? "The previous configuration was restored." : "WARNING: the previous configuration could not be restored — check " + _configPath));
}
