#include "manager.h"
#include "mcp_client.h"

#include "logging.h"
#include "paths.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <algorithm>

#ifdef Q_OS_WIN
	#define NOMINMAX
	#include <windows.h>
#endif

// The whole app shares ONE canonical log file (see logging.h): the in-app panel
// messages, verbose diagnostics, and the MCP server subprocess output all land in
// it, trimmed to the "maxLogSizeMB" cap. 0 disables the cap entirely.
static const int DEFAULT_MAX_LOG_MB = 10;
static const int MAX_LOG_MB_CEILING = 10240;

DepotLayout DepotLayoutFor(int appId, const QString& os) {
	const bool onWindows = os.compare("windows", Qt::CaseInsensitive) == 0;
	const bool onLinux = os.compare("linux", Qt::CaseInsensitive) == 0;
	if (appId == 730 && (onWindows || onLinux)) {
		return {onWindows ? 2347771 : 2347773, "game", "csgo/steam.inf", "https://raw.githubusercontent.com/SteamDatabase/GameTracking-CS2/master/game/csgo/steam.inf", false};
	}
	// Standalone CS:GO needs an account that owns it, even for Latest.
	if (appId == 4465480 && (onWindows || onLinux)) {
		return {onWindows ? 732 : 734, {}, "csgo/steam.inf", {}, true};
	}
	return {};
}

DepotLayout DepotLayoutFor(const DepotConfig& depot) {
	DepotLayout layout = DepotLayoutFor(depot.appId, depot.os);
	if (depot.depotId > 0) {
		layout = {depot.depotId, QDir::cleanPath(QDir::fromNativeSeparators(depot.pathPrefix.trimmed())), {}, {}, false};
		if (layout.pathPrefix == ".") {
			layout.pathPrefix.clear(); // cleanPath("") is "."
		}
	}
	layout.accountRequired = layout.accountRequired || depot.requiresLogin;
	return layout;
}

static QString DatabaseKey(QString path) {
	if (path.endsWith(".i64", Qt::CaseInsensitive) || path.endsWith(".idb", Qt::CaseInsensitive)) { path.chop(4); }
	const QFileInfo info(path);
	path = info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
#ifdef Q_OS_WIN
	return QDir::cleanPath(path).toCaseFolded();
#else
	return QDir::cleanPath(path);
#endif
}

QString DatabaseInput(const QString& binary) {
	return QFile::exists(binary + ".i64") ? binary + ".i64" : binary;
}

// Covers an interactive IDA that has not registered with Nexus yet, and incomplete
// databases from an interrupted session. Never sweep those files.
static bool UnpackedDatabaseExists(const QString& binary) {
	return QFile::exists(binary + ".id0") || QFile::exists(binary + ".id1");
}

// A live IDA keeps its unpacked pieces open; leftovers of a crashed session are held by
// nobody. Asking for exclusive access tells the two apart without trusting UI state.
// Where that cannot be proven the pieces count as held.
static bool UnpackedDatabaseHeld(const QString& binary) {
#ifdef Q_OS_WIN
	for (const char* ext : {".id0", ".id1", ".id2", ".nam", ".til"}) {
		const QString path = QDir::toNativeSeparators(binary + ext);
		if (!QFile::exists(path)) {
			continue;
		}
		const HANDLE handle = CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
										  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE) {
			return true;
		}
		CloseHandle(handle);
	}
	return false;
#else
	Q_UNUSED(binary);
	return true;
#endif
}

static QString GeneratedTagColor(const QString& tag) {
	static const char* colors[] = {"#EADDFF", "#FFD8E4", "#D3EFD5", "#D3E3FD", "#FFEFD2", "#CCE8E6", "#F0DBF5", "#E3E9C8"};
	quint32 hash = 2166136261u;
	for (const QChar c : tag) {
		hash ^= c.unicode();
		hash *= 16777619u;
	}
	return colors[hash % (sizeof(colors) / sizeof(colors[0]))];
}

static QString ExpandUserPath(const QString& p) {
	if (p == "~") {
		return QDir::homePath();
	}
	if (p.startsWith("~/") || p.startsWith("~\\")) {
		return QDir::homePath() + "/" + p.mid(2);
	}
	return p;
}

static QString ResolveExecutable(const QString& configured) {
	const QString value = QDir::fromNativeSeparators(configured.trimmed());
	if (value.isEmpty()) {
		return {};
	}
	if (QFileInfo(value).isFile()) {
		return QFileInfo(value).absoluteFilePath();
	}
	if (!QFileInfo(value).isAbsolute() && !value.contains('/')) {
		return QStandardPaths::findExecutable(value);
	}
	return {};
}

static bool DownloadUrl(const QUrl& url, int timeoutMs, QByteArray* data, QString* error) {
	QNetworkAccessManager network;
	QNetworkRequest request(url);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	// GitHub's API answers 403 to requests without a User-Agent, and naming ourselves is
	// the polite thing to do for the raw.githubusercontent fetch as well.
	request.setHeader(QNetworkRequest::UserAgentHeader, QString("ida-workbench/%1").arg(QCoreApplication::applicationVersion()));
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
		if (error) {
			*error = networkError.isEmpty() ? "empty response" : networkError;
		}
		return false;
	}
	if (data) {
		*data = body;
	}
	return true;
}

static bool BootstrapDepotDownloader(const QString& configPath, QString* executable, QString* error) {
#ifdef Q_OS_WIN
	const QString asset = "DepotDownloader-windows-x64.zip";
	const QString binaryName = "DepotDownloader.exe";
#elif defined(Q_OS_LINUX)
	const QString asset = QSysInfo::currentCpuArchitecture().contains("arm64") ?
							  "DepotDownloader-linux-arm64.zip" :
							  "DepotDownloader-linux-x64.zip";
	const QString binaryName = "DepotDownloader";
#else
	if (error) *error = "automatic DepotDownloader installation is supported on Windows and Linux only";
	return false;
#endif
	const QString tools = QDir(QFileInfo(configPath).absolutePath()).filePath("tools/depotdownloader");
	const QString binary = QDir(tools).filePath(binaryName);
	if (QFileInfo(binary).isFile()) {
		*executable = QFileInfo(binary).absoluteFilePath();
		return true;
	}
	if (!QDir().mkpath(tools)) {
		if (error) {
			*error = QString("cannot create tools directory: %1").arg(tools);
		}
		return false;
	}

	const QUrl url(QString("https://github.com/SteamRE/DepotDownloader/releases/download/DepotDownloader_3.4.0/%1").arg(asset));
	QByteArray archive;
	QString networkError;
	if (!DownloadUrl(url, 120000, &archive, &networkError)) {
		if (error) {
			*error = QString("download failed: %1").arg(networkError);
		}
		return false;
	}
	const QString zipPath = QDir(tools).filePath("DepotDownloader.zip");
	QSaveFile zip(zipPath);
	if (!zip.open(QIODevice::WriteOnly) || zip.write(archive) != archive.size() || !zip.commit()) {
		if (error) {
			*error = QString("cannot save downloaded archive: %1").arg(zip.errorString());
		}
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
		if (error) {
			*error = QString("archive extraction failed: %1").arg(QString::fromUtf8(extract.readAllStandardError()).trimmed());
		}
		QFile::remove(zipPath);
		return false;
	}
	QFile::remove(zipPath);
#ifndef Q_OS_WIN
	QFile::setPermissions(binary, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther);
#endif
	if (!QFileInfo(binary).isFile()) {
		if (error) {
			*error = QString("archive does not contain %1").arg(binaryName);
		}
		return false;
	}
	*executable = QFileInfo(binary).absoluteFilePath();
	return true;
}

// IDA 9.4 is the first release with the official MCP server and IDA Nexus.
static bool SupportedIda(const QString& executable) {
	if (!QFile::exists(executable)) { return false; }
#ifdef Q_OS_WIN
	const std::wstring file = QDir::toNativeSeparators(executable).toStdWString();
	const DWORD size = GetFileVersionInfoSizeW(file.c_str(), nullptr);
	if (!size) { return false; }
	QByteArray data(static_cast<qsizetype>(size), Qt::Uninitialized);
	if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) { return false; }
	VS_FIXEDFILEINFO* info = nullptr;
	UINT length = 0;
	if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) { return false; }
	const int major = HIWORD(info->dwFileVersionMS);
	const int minor = LOWORD(info->dwFileVersionMS);
#else
	QFile notes(QFileInfo(executable).absolutePath() + "/release-notes.html");
	if (!notes.open(QIODevice::ReadOnly)) { return false; }
	const auto version = QRegularExpression("(?:IDA|Version)[^0-9]{0,20}([0-9]+)\\.([0-9]+)", QRegularExpression::CaseInsensitiveOption)
							 .match(QString::fromUtf8(notes.read(65536)));
	if (!version.hasMatch()) { return false; }
	const int major = version.captured(1).toInt();
	const int minor = version.captured(2).toInt();
#endif
	return major > 9 || (major == 9 && minor >= 4);
}

bool AutoDetectIda(QString* guiPath, QString* textPath) {
#ifdef Q_OS_WIN
	const QString guiName = "ida.exe";
	const QString textName = "idat.exe";
	QStringList roots;
	for (const char* env : {"ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"}) {
		const QString r = qEnvironmentVariable(env);
		if (!r.isEmpty()) {
			roots << r;
		}
	}
	const QString local = qEnvironmentVariable("LOCALAPPDATA");
	if (!local.isEmpty()) {
		roots << local + "/Programs";
	}
#else
	const QString guiName = "ida";
	const QString textName = "idat";
	QStringList roots = {QStringLiteral("/opt"), QDir::homePath(), QDir::homePath() + "/.local", QStringLiteral("/usr/local")};
#endif
	roots.removeDuplicates();

	// Collect every "IDA*" directory across the roots, then prefer the one that
	// sorts highest (roughly the newest version) and holds BOTH binaries.
	QStringList candidates;
	for (const QString& root : roots) {
		QDir d(root);
		if (!d.exists()) {
			continue;
		}
		for (const QString& sub : d.entryList({"IDA*", "ida*"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
			candidates << d.filePath(sub);
		}
	}
	candidates.removeDuplicates();
	std::sort(candidates.begin(), candidates.end(), [](const QString& a, const QString& b) {
		return a.compare(b, Qt::CaseInsensitive) < 0;
	});
	for (int i = candidates.size() - 1; i >= 0; --i) {
		const QString g = QDir(candidates[i]).filePath(guiName);
		const QString t = QDir(candidates[i]).filePath(textName);
		if (SupportedIda(g) && QFile::exists(t)) {
			if (guiPath) {
				*guiPath = QDir::toNativeSeparators(g);
			}
			if (textPath) {
				*textPath = QDir::toNativeSeparators(t);
			}
			return true;
		}
	}
	return false;
}

static QString ValidateConfig(const ConfigView& cfg) {
	// IDA paths and the library set are deliberately NOT required here: a brand
	// new install has neither. Missing IDA shows as a red readiness chip and
	// gates the actions; an empty library set is a valid "nothing added yet"
	// state. Only field formats that would corrupt the file are enforced.
	if (cfg.logDir.trimmed().isEmpty()) {
		return "Log directory must not be empty.";
	}
	if (cfg.maxLogMB < 0 || cfg.maxLogMB > MAX_LOG_MB_CEILING) {
		return QString("Max log size must be 0 (unlimited) to %1 MB.").arg(MAX_LOG_MB_CEILING);
	}

	QSet<QString> tags;
	QSet<QString> instances;
	for (const Workspace& workspace : AllWorkspaces(cfg)) {
		const QString tag = workspace.tag.trimmed();
		if (tag.isEmpty()) {
			return "Every workspace needs a tag.";
		}
		if (!QRegularExpression("^#[0-9A-Fa-f]{6}$").match(workspace.color.trimmed()).hasMatch()) {
			return QString("Workspace '%1' needs an explicit #RRGGBB color.").arg(tag);
		}
		if (workspace.source.trimmed().isEmpty()) {
			return workspace.depot.enabled ? QString("Steam workspace '%1' has no directory.").arg(tag) : QString("Workspace '%1' has no source.").arg(tag);
		}
		if (workspace.output.trimmed().isEmpty()) {
			return QString("Workspace '%1' has no output.").arg(tag);
		}
		if (workspace.depot.enabled) {
			if (workspace.depot.appId <= 0) {
				return QString("Steam workspace '%1' needs an AppID.").arg(tag);
			}
			if (!QStringList{"", "windows", "linux", "macos"}.contains(workspace.depot.os)) {
				return QString("Steam workspace '%1': OS must be windows, linux, macos or empty (any).").arg(tag);
			}
			const DepotLayout layout = DepotLayoutFor(workspace.depot);
			if (layout.depotId <= 0) {
				return QString("Steam workspace '%1': enter the DepotID that holds its files (AppID %2 has no built-in depot for OS '%3').")
					.arg(tag)
					.arg(workspace.depot.appId)
					.arg(workspace.depot.os.isEmpty() ? QStringLiteral("any") : workspace.depot.os);
			}
			if (QDir::isAbsolutePath(layout.pathPrefix) || layout.pathPrefix == ".." || layout.pathPrefix.startsWith("../")) {
				return QString("Steam workspace '%1': the depot folder must be relative to the depot root.").arg(tag);
			}
			if (QDir::cleanPath(workspace.source) != QDir::cleanPath(workspace.output)) {
				return QString("Steam workspace '%1' must use one directory for manifests and analysis.").arg(tag);
			}
		}
		const QString tagKey = tag.toCaseFolded();
		if (tags.contains(tagKey)) {
			return QString("Duplicate tag: %1").arg(tag);
		}
		tags.insert(tagKey);
		QSet<QString> tagNames; // duplicate check is per-tag: the same library across tags is fine
		for (int fileIndex = 0; fileIndex < workspace.files.size(); ++fileIndex) {
			const QString& raw = workspace.files[fileIndex];
			const QString relative = QDir::cleanPath(raw.trimmed());
			if (relative.isEmpty() || relative == "." || relative == ".." || QDir::isAbsolutePath(relative) || relative.startsWith("../")) {
				return QString("File must be relative to %1 (tag %2): %3")
					.arg(workspace.depot.enabled ? "the depot folder" : "Source", tag, raw);
			}
			const QString name = QFileInfo(relative).completeBaseName();
			if (name.isEmpty()) {
				return QString("File has no library name (tag %1): %2").arg(tag, raw);
			}
			const QString nk = name.toCaseFolded();
			if (tagNames.contains(nk)) {
				return QString("Duplicate library %1 in tag %2.").arg(name, tag);
			}
			tagNames.insert(nk);
			instances.insert(tagKey + '\t' + nk);
		}
	}
	for (int i = 0; i < cfg.extraLibs.size(); ++i) {
		const ExtraLib& extra = cfg.extraLibs[i];
		const QString tag = extra.tag.trimmed();
		const QString name = QFileInfo(extra.path.trimmed()).completeBaseName();
		if (tag.isEmpty() || extra.path.trimmed().isEmpty()) {
			return "Every library needs a tag and path.";
		}
		if (!QRegularExpression("^#[0-9A-Fa-f]{6}$").match(extra.color.trimmed()).hasMatch()) {
			return QString("Library '%1' needs an explicit #RRGGBB color.").arg(tag);
		}
		if (name.isEmpty()) {
			return QString("Library '%1' does not point to a file name.").arg(tag);
		}
		const QString instance = tag.toCaseFolded() + '\t' + name.toCaseFolded();
		if (instances.contains(instance)) {
			return QString("Duplicate library instance: %1@%2").arg(name, tag);
		}
		instances.insert(instance);
	}
	return {};
}

static QString PrepareWorkspaceDirectories(const ConfigView& cfg) {
	for (const Workspace& workspace : AllWorkspaces(cfg)) {
		const QString output = workspace.output.trimmed();
		if (!QDir().mkpath(output)) {
			return QString("Cannot create output directory for '%1': %2").arg(workspace.tag, output);
		}
		if (workspace.depot.enabled) {
			continue;
		}
		const QString revisions = QDir(output).filePath("revisions");
		if (!QDir().mkpath(revisions)) {
			return QString("Cannot create revisions directory for '%1': %2").arg(workspace.tag, revisions);
		}
		for (const QString& relative : workspace.files) {
			const QString destination = QDir(output).filePath(relative);
			if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
				return QString("Cannot create output path for '%1': %2").arg(workspace.tag, QFileInfo(destination).absolutePath());
			}
		}
	}
	return {};
}

static bool CopyFileAtomically(const QString& sourcePath, const QString& destinationPath, QString* error) {
	QFile source(sourcePath);
	if (!source.open(QIODevice::ReadOnly)) {
		if (error) {
			*error = source.errorString();
		}
		return false;
	}
	if (!QDir().mkpath(QFileInfo(destinationPath).absolutePath())) {
		if (error) {
			*error = "cannot create destination directory";
		}
		return false;
	}
	QSaveFile destination(destinationPath);
	destination.setDirectWriteFallback(false);
	if (!destination.open(QIODevice::WriteOnly)) {
		if (error) {
			*error = destination.errorString();
		}
		return false;
	}
	QByteArray block(1024 * 1024, Qt::Uninitialized);
	while (!source.atEnd()) {
		const qint64 count = source.read(block.data(), block.size());
		if (count < 0 || destination.write(block.constData(), count) != count) {
			if (error) {
				*error = count < 0 ? source.errorString() : destination.errorString();
			}
			destination.cancelWriting();
			return false;
		}
	}
	destination.setPermissions(source.permissions());
	if (!destination.commit()) {
		if (error) {
			*error = destination.errorString();
		}
		return false;
	}
	return true;
}

void ReadSteamVersions(const QString& steamInfPath, QString* patchVersion, QString* serverVersion) {
	QFile inf(steamInfPath);
	if (!inf.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return;
	}
	const QString text = QString::fromUtf8(inf.readAll());
	auto value = [&text](const QString& key) {
		return QRegularExpression(QString("(?mi)^\\s*%1\\s*=\\s*([^\\r\\n]+)").arg(QRegularExpression::escape(key))).match(text).captured(1).trimmed();
	};
	const QString patch = value("PatchVersion");
	const QString server = value("ServerVersion");
	if (patchVersion && !patch.isEmpty()) {
		*patchVersion = patch;
	}
	if (serverVersion && !server.isEmpty()) {
		*serverVersion = server;
	}
}

// Records which app version last materialised the helper scripts, next to them.
static const char* const kScriptStampFile = ".scripts-version";

// The standalone analyzer is embedded so a copied exe can create databases.
static void EnsureHelperScripts(const QString& destDir) {
	QDir().mkpath(destDir);
	const QString version = QCoreApplication::applicationVersion();
	QString stamp;
	QFile stampFile(QDir(destDir).filePath(kScriptStampFile));
	if (stampFile.open(QIODevice::ReadOnly)) {
		stamp = QString::fromUtf8(stampFile.readAll()).trimmed();
		stampFile.close();
	}
	const bool refresh = stamp != version;
	const QString dest = QDir(destDir).filePath("analyze_ida.py");
	const bool present = QFile::exists(dest);
	if (present && !refresh) {
		return; // this version already deployed its copy — keep any local edit
	}
	QFile bundled(":/scripts/analyze_ida.py");
	if (!bundled.open(QIODevice::ReadOnly)) {
		return; // resource absent (tests) — leave any on-disk copy
	}
	const QByteArray data = bundled.readAll();
	bool identical = false; // not worth a write or a log line
	if (present) {
		QFile existing(dest);
		identical = existing.open(QIODevice::ReadOnly) && existing.readAll() == data;
	}
	if (!identical) {
		QSaveFile out(dest);
		if (out.open(QIODevice::WriteOnly) && out.write(data) == data.size() && out.commit()) {
			Log::Write(Log::Level::Info, QString("[scripts] refreshed for %1: analyze_ida.py").arg(version));
		}
	}
	// Stamp only once the resource proved readable, so a build without it cannot claim
	// a version it never deployed and suppress the refresh for a build that has it.
	if (refresh) {
		QSaveFile out(QDir(destDir).filePath(kScriptStampFile));
		if (out.open(QIODevice::WriteOnly)) {
			out.write(version.toUtf8());
			out.commit();
		}
	}
}

// The app folder is for program files, so it is never also the log folder: a `logDir`
// pointing straight at it is redirected into logs/ underneath. That is where older
// installs logged — nobody picked the path, it was simply the default of the day — and
// keeping it as a standing rule rather than a one-shot migration means the two roles
// cannot quietly merge back together later. Any other logDir is the user's choice and is
// left exactly as written. The log file itself is carried over in main(), before this
// session writes its first line. Returns true when the caller must persist the change.
static bool RedirectLogDirOutOfAppHome(QString* logDir) {
	auto key = [](const QString& path) {
		return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower();
	};
	if (key(*logDir) != key(Paths::AppHome())) {
		return false;
	}
	if (!QDir().mkpath(Paths::DefaultLogDir())) {
		return false; // keep logging where it demonstrably works
	}
	*logDir = Paths::DefaultLogDir();
	return true;
}

// The schema 2 form of workspaces, shared by migration and Save.
static QJsonArray WorkspacesJson(const QVector<Workspace>& workspaces, bool steam) {
	QJsonArray result;
	for (const Workspace& workspace : workspaces) {
		if (workspace.tag.trimmed().isEmpty()) {
			continue;
		}
		QJsonObject object{{"tag", workspace.tag.trimmed()}};
		if (steam) {
			object["dir"] = workspace.output.trimmed();
		} else {
			object["source"] = workspace.source.trimmed();
			object["output"] = workspace.output.trimmed();
		}
		QJsonArray files;
		for (const QString& file : workspace.files) {
			if (!file.trimmed().isEmpty()) {
				files.append(QDir::cleanPath(file.trimmed()));
			}
		}
		object["files"] = files;
		object["color"] = workspace.color.trimmed().isEmpty() ? GeneratedTagColor(workspace.tag.trimmed()) : workspace.color.trimmed();
		if (steam) {
			object["appId"] = workspace.depot.appId;
			object["os"] = workspace.depot.os;
			// Built-in layouts leave these out, so known-game workspaces keep their old form.
			if (workspace.depot.depotId > 0) {
				object["depotId"] = workspace.depot.depotId;
			}
			if (!workspace.depot.pathPrefix.trimmed().isEmpty()) {
				object["pathPrefix"] = QDir::fromNativeSeparators(workspace.depot.pathPrefix.trimmed());
			}
			if (workspace.depot.requiresLogin) {
				object["requiresLogin"] = true;
			}
			if (!workspace.depot.manifest.isEmpty()) {
				object["current"] = workspace.depot.manifest;
			}
		}
		result.append(object);
	}
	return result;
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
	EnsureHelperScripts(configDir);
	_scriptDir = QFile::exists(QDir(appDir).filePath("analyze_ida.py")) ? appDir : configDir;
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		if (err) {
			*err = QString("cannot open %1: %2").arg(path, f.errorString());
		}
		return false;
	}
	QJsonParseError pe;
	const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
	f.close();
	if (doc.isNull() || !doc.isObject()) {
		if (err) {
			*err = QString("bad JSON in %1: %2").arg(path, pe.errorString());
		}
		return false;
	}
	QJsonObject c = doc.object();
	ConfigView next;
	bool generatedColors = false;
	const QJsonValue uvx = c.value("mcp").toObject().value("executable");
	next.uvx = ExpandUserPath(uvx.isString() ? uvx.toString() : McpClient::FindUvx());
	const QJsonObject ida = c.value("ida").toObject();
	next.idaGui = ExpandUserPath(ida.value("gui").toString());
	next.idaText = ExpandUserPath(ida.value("text").toString());
	next.logDir = ExpandUserPath(c.value("logDir").toString(Paths::DefaultLogDirSetting()));
	const bool logDirMigrated = RedirectLogDirOutOfAppHome(&next.logDir);
	next.analysisArgs = c.value("analysisArgs").toString().trimmed();
	const int ml = c.value("maxLogSizeMB").toInt(DEFAULT_MAX_LOG_MB);
	next.maxLogMB = (ml >= 0 && ml <= MAX_LOG_MB_CEILING) ? ml : DEFAULT_MAX_LOG_MB;
	const QJsonObject downloader = c.value("depotDownloader").toObject();
	next.depotDownloader.executable = ExpandUserPath(downloader.value("executable").toString("DepotDownloader.exe").trimmed());
	next.depotDownloader.timeoutMinutes = qBound(1, downloader.value("timeoutMinutes").toInt(30), 240);

	auto parseWorkspace = [&](const QJsonObject& object, bool steam) {
		Workspace workspace;
		workspace.tag = object.value("tag").toString().trimmed();
		if (steam) {
			const QString dir = ExpandUserPath(object.value("dir").toString().trimmed());
			workspace.source = dir;
			workspace.output = dir;
		} else {
			workspace.source = ExpandUserPath(object.value("source").toString().trimmed());
			workspace.output = ExpandUserPath(object.value("output").toString().trimmed());
		}
		workspace.color = object.value("color").toString().trimmed().toUpper();
		if (workspace.color.isEmpty()) {
			workspace.color = GeneratedTagColor(workspace.tag);
			generatedColors = true;
		}
		workspace.depot.enabled = steam;
		workspace.depot.appId = object.value("appId").toInt(730);
		workspace.depot.os = object.value("os").toString("windows").trimmed().toLower();
		workspace.depot.depotId = object.value("depotId").toInt(0);
		workspace.depot.pathPrefix = object.value("pathPrefix").toString().trimmed();
		workspace.depot.requiresLogin = object.value("requiresLogin").toBool(false);
		workspace.depot.manifest = object.value("current").toString().trimmed();
		for (const QJsonValue file : object.value("files").toArray()) {
			const QString relative = QDir::cleanPath(file.toString().trimmed());
			if (!relative.isEmpty() && !workspace.files.contains(relative)) {
				workspace.files << relative;
			}
		}
		if (!workspace.tag.isEmpty()) {
			(steam ? next.steamWorkspaces : next.workspaces) << workspace;
		}
	};
	for (const QJsonValue value : c.value("workspaces").toArray()) {
		const QJsonObject object = value.toObject();
		parseWorkspace(object, false);
	}
	for (const QJsonValue value : c.value("steamWorkspaces").toArray()) {
		parseWorkspace(value.toObject(), true);
	}
	for (const QJsonValue value : c.value("extraLibs").toArray()) {
		const QJsonObject object = value.toObject();
		ExtraLib extra;
		extra.tag = object.value("tag").toString().trimmed();
		extra.path = ExpandUserPath(object.value("path").toString().trimmed());
		extra.color = object.value("color").toString().trimmed().toUpper();
		if (extra.color.isEmpty()) {
			extra.color = GeneratedTagColor(extra.tag);
			generatedColors = true;
		}
		if (!extra.tag.isEmpty() && !extra.path.isEmpty()) {
			next.extraLibs << extra;
		}
	}

	// IDA paths may be empty on a fresh install — the app still comes up and
	// reports IDA as not-ready until they are set in Settings (or auto-detected).
	const QString validation = ValidateConfig(next);
	if (!validation.isEmpty()) {
		if (err) {
			*err = "config: " + validation;
		}
		return false;
	}
	const QString directoryError = PrepareWorkspaceDirectories(next);
	if (!directoryError.isEmpty()) {
		if (err) {
			*err = "config: " + directoryError;
		}
		return false;
	}
	for (Workspace& workspace : next.steamWorkspaces) {
		workspace.depot.patchVersion.clear();
		workspace.depot.serverVersion.clear();
		const QString versionFile = DepotLayoutFor(workspace.depot).versionFile;
		if (!workspace.depot.manifest.isEmpty() && !versionFile.isEmpty()) {
			ReadSteamVersions(QDir(workspace.output).filePath(workspace.depot.manifest + "/" + versionFile), &workspace.depot.patchVersion, &workspace.depot.serverVersion);
		}
	}
	const bool legacy = c.value("schemaVersion").toInt() < 2;
	if (legacy && (!SupportedIda(next.idaGui) || !QFile::exists(next.idaText))) { AutoDetectIda(&next.idaGui, &next.idaText); }
	const bool schemaNeedsWrite = legacy || generatedColors || logDirMigrated || !c.contains("analysisArgs") || !c.contains("depotDownloader") || !c.contains("steamWorkspaces");
	if (schemaNeedsWrite) {
		const QString backup = _configPath + ".pre-hexrays.bak";
		if (legacy && !QFile::exists(backup) && !QFile::copy(_configPath, backup)) {
			if (err) { *err = "Cannot back up the old configuration before migration: " + backup; }
			return false;
		}
		c["schemaVersion"] = 2;
		c.remove("host");
		c.remove("scanBasePort");
		c.remove("portOverrides");
		c["mcp"] = QJsonObject{{"executable", next.uvx}};
		c["ida"] = QJsonObject{{"gui", next.idaGui}, {"text", next.idaText}};
		QHash<QString, QString> extraColors;
		for (const ExtraLib& extra : next.extraLibs) {
			extraColors.insert(extra.tag + '\t' + extra.path, extra.color);
		}
		if (logDirMigrated) {
			// Tilde form, not the expanded path: config.json stays portable.
			c["logDir"] = Paths::DefaultLogDirSetting();
		}
		c["analysisArgs"] = next.analysisArgs;
		c["depotDownloader"] = QJsonObject{{"executable", next.depotDownloader.executable}, {"timeoutMinutes", next.depotDownloader.timeoutMinutes}};
		c["workspaces"] = WorkspacesJson(next.workspaces, false);
		c["steamWorkspaces"] = WorkspacesJson(next.steamWorkspaces, true);
		QJsonArray extraArray = c.value("extraLibs").toArray();
		for (int i = 0; i < extraArray.size(); ++i) {
			QJsonObject object = extraArray[i].toObject();
			object.remove("port");
			const QString key = object.value("tag").toString() + '\t' + object.value("path").toString();
			if (object.value("color").toString().trimmed().isEmpty()) {
				object["color"] = extraColors.value(key);
			}
			extraArray[i] = object;
		}
		c["extraLibs"] = extraArray;
		QSaveFile normalized(_configPath);
		const QByteArray migrated = QJsonDocument(c).toJson(QJsonDocument::Indented);
		if (!normalized.open(QIODevice::WriteOnly) || normalized.write(migrated) != migrated.size() || !normalized.commit()) {
			if (err) { *err = "Cannot save migrated configuration: " + normalized.errorString(); }
			return false;
		}
	}

	_uvx = next.uvx.trimmed();
	_idaGui = next.idaGui;
	_idaText = next.idaText;
	_logDir = next.logDir;
	_analysisArgs = next.analysisArgs;
	_maxLogMB = next.maxLogMB;
	_depotDownloader = next.depotDownloader;
	_workspaces = AllWorkspaces(next);
	_extraLibs = next.extraLibs;
	RebuildInstances();
	// Point the canonical log at the configured folder and apply the size cap.
	Log::Configure(QDir(_logDir).filePath(Log::FileName()), _maxLogMB);
	if (logDirMigrated) {
		// Written after the re-point, so the note lands in the folder it talks about.
		Log::Write(Log::Level::Info, QString("[config] logs moved out of the app folder into %1").arg(QDir::toNativeSeparators(_logDir)));
	}
	qDebug().noquote() << "[config] loaded" << path << "Hex-Rays MCP:" << _uvx;
	return true;
}

bool Manager::CreateDefaultConfig(const QString& path, QString* err) {
	QString gui;
	QString text;
	AutoDetectIda(&gui, &text); // best-effort; left blank when IDA is not found

	QJsonObject root;
	root["schemaVersion"] = 2;
	root["mcp"] = QJsonObject{{"executable", McpClient::FindUvx()}};
	root["ida"] = QJsonObject{{"gui", gui}, {"text", text}};
	root["logDir"] = Paths::DefaultLogDirSetting();
	root["maxLogSizeMB"] = DEFAULT_MAX_LOG_MB;
	root["analysisArgs"] = "";
	root["depotDownloader"] = QJsonObject{{"executable", "DepotDownloader.exe"}, {"timeoutMinutes", 30}};
	root["extraLibs"] = QJsonArray{};
	root["workspaces"] = QJsonArray{};
	root["steamWorkspaces"] = QJsonArray{};

	if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
		if (err) {
			*err = "cannot create " + QFileInfo(path).absolutePath();
		}
		return false;
	}
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly)) {
		if (err) {
			*err = f.errorString();
		}
		return false;
	}
	const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
	if (f.write(json) != json.size() || !f.commit()) {
		if (err) {
			*err = f.errorString();
		}
		return false;
	}
	return true;
}

// A Steam workspace keeps each ManifestID in its own folder; a folder workspace keeps
// Current in its output and archived revisions under revisions/<timestamp>.
static QString RevisionRoot(const Workspace& workspace, const QString& revision) {
	if (workspace.depot.enabled) {
		const QString manifest = revision.isEmpty() ? workspace.depot.manifest : revision;
		return QDir(workspace.output).filePath(manifest.isEmpty() ? ".no-current-manifest" : manifest);
	}
	return revision.isEmpty() ? workspace.output : QDir(workspace.output).filePath("revisions/" + revision);
}

static Instance WorkspaceInstance(const Workspace& workspace, const QString& relative, const QString& revision) {
	Instance inst;
	inst.tag = workspace.tag;
	inst.name = QFileInfo(relative).completeBaseName();
	inst.relativeFile = relative;
	inst.outputRoot = workspace.output;
	inst.storedVersion = revision;
	inst.binary = QDir(RevisionRoot(workspace, revision)).filePath(relative);
	inst.hasSource = !workspace.depot.enabled;
	inst.compareSource = inst.hasSource && revision.isEmpty();
	if (inst.hasSource) {
		inst.sourceBinary = QDir(workspace.source).filePath(relative);
	}
	return inst;
}

static Instance ExtraLibInstance(const ExtraLib& extra) {
	Instance inst;
	inst.tag = extra.tag;
	inst.name = QFileInfo(extra.path).completeBaseName();
	inst.binary = extra.path;
	return inst;
}

std::optional<Instance> ResolveInstance(const QVector<Workspace>& workspaces, const QVector<ExtraLib>& extraLibs, const Target& target) {
	for (const Workspace& workspace : workspaces) {
		if (workspace.tag != target.tag) {
			continue;
		}
		for (const QString& relative : workspace.files) {
			if (QFileInfo(relative).completeBaseName() == target.name) {
				return WorkspaceInstance(workspace, relative, target.revision);
			}
		}
	}
	if (!target.revision.isEmpty()) {
		return std::nullopt; // extra libs have no stored revisions
	}
	for (const ExtraLib& extra : extraLibs) {
		if (extra.tag == target.tag && QFileInfo(extra.path).completeBaseName() == target.name) {
			return ExtraLibInstance(extra);
		}
	}
	return std::nullopt;
}

void Manager::RebuildInstances() {
	_instances.clear();
	_tags.clear();
	_names.clear();

	for (const Workspace& sd : _workspaces) {
		if (!_tags.contains(sd.tag)) {
			_tags << sd.tag;
		}
	}
	for (const ExtraLib& e : _extraLibs) {
		if (!_tags.contains(e.tag)) {
			_tags << e.tag;
		}
	}
	for (const Workspace& sd : _workspaces) {
		for (const QString& file : sd.files) {
			const QString n = QFileInfo(file).completeBaseName();
			if (!n.isEmpty() && !_names.contains(n)) {
				_names << n;
			}
		}
	}
	for (const ExtraLib& e : _extraLibs) {
		const QString n = QFileInfo(e.path).completeBaseName();
		if (!_names.contains(n)) {
			_names << n;
		}
	}

	for (const Workspace& sd : _workspaces) {
		const QString selectedVersion = _selectedVersions.value(sd.tag);
		for (const QString& relative : sd.files) {
			if (!QFileInfo(relative).completeBaseName().isEmpty()) {
				_instances.push_back(WorkspaceInstance(sd, relative, selectedVersion));
			}
		}
	}
	for (const ExtraLib& e : _extraLibs) {
		_instances.push_back(ExtraLibInstance(e));
	}
}

QStringList Manager::StoredVersions(const QString& tag) const {
	for (const Workspace& workspace : _workspaces) {
		if (workspace.tag != tag) {
			continue;
		}
		if (workspace.depot.enabled) {
			QStringList manifests = QDir(workspace.output).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
			manifests.erase(std::remove_if(manifests.begin(), manifests.end(), [&workspace](const QString& name) {
								return name == workspace.depot.manifest || !QRegularExpression("^[0-9]+$").match(name).hasMatch();
							}),
							manifests.end());
			return manifests;
		}
		QDir revisionsDir(QDir(workspace.output).filePath("revisions"));
		return revisionsDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
	}
	return {};
}

void Manager::SetStoredVersion(const QString& tag, const QString& version) {
	if (!version.isEmpty() && !StoredVersions(tag).contains(version)) {
		return;
	}
	if (version.isEmpty()) {
		_selectedVersions.remove(tag);
	} else {
		_selectedVersions.insert(tag, version);
	}
	RebuildInstances();
	Refresh();
}

// Compares dotted versions component by component and numerically, so 1.3.10 ranks above
// 1.3.9 where a plain string compare would not. Returns <0, 0 or >0 like strcmp; a missing
// component counts as 0, which makes "1.3" and "1.3.0" equal.
static int CompareVersions(const QString& left, const QString& right) {
	const QStringList leftParts = left.split('.');
	const QStringList rightParts = right.split('.');
	const qsizetype components = qMax(leftParts.size(), rightParts.size());
	for (qsizetype i = 0; i < components; ++i) {
		const int leftValue = leftParts.value(i).toInt();
		const int rightValue = rightParts.value(i).toInt();
		if (leftValue != rightValue) {
			return leftValue < rightValue ? -1 : 1;
		}
	}
	return 0;
}

void Manager::CheckForUpdate() {
	// Asynchronous on purpose. The blocking DownloadUrl() helper parks the worker inside a
	// nested event loop for as long as the request takes, and queued commands (Start, Stop,
	// Analyze) would then run reentrantly in the middle of a background check nobody asked
	// to wait for. The depot downloads accept that trade because the user is waiting on them;
	// this one must not.
	if (_updateCheckInFlight) {
		return;
	}
	// /releases/latest skips drafts and prereleases, so whatever it returns is something a
	// user is meant to install.
	static const QString releasesApi = "https://api.github.com/repos/sazonische/ida-workbench/releases/latest";
	auto* network = new QNetworkAccessManager(this);
	QNetworkRequest request{QUrl(releasesApi)}; // braces: parentheses here declare a function
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setHeader(QNetworkRequest::UserAgentHeader, QString("ida-workbench/%1").arg(QCoreApplication::applicationVersion()));
	request.setTransferTimeout(10000);
	QNetworkReply* reply = network->get(request);
	_updateCheckInFlight = true;
	connect(reply, &QNetworkReply::finished, this, [this, reply, network] {
		_updateCheckInFlight = false;
		const QByteArray body = reply->readAll();
		const QNetworkReply::NetworkError status = reply->error();
		const QString networkError = reply->errorString();
		reply->deleteLater();
		network->deleteLater();
		if (status != QNetworkReply::NoError || body.isEmpty()) {
			emit Log(QString("[update] check skipped: %1")
						 .arg(status != QNetworkReply::NoError ? networkError : QStringLiteral("empty response")));
			return;
		}
		FinishUpdateCheck(body);
	});
}

void Manager::FinishUpdateCheck(const QByteArray& body) {
	const QString current = QCoreApplication::applicationVersion();
	const QJsonObject release = QJsonDocument::fromJson(body).object();
	const QString tag = release.value("tag_name").toString().trimmed();
	const QString latest = tag.startsWith('v', Qt::CaseInsensitive) ? tag.mid(1) : tag;
	if (latest.isEmpty()) {
		emit Log("[update] check skipped: the latest release carries no tag name");
		return;
	}
	if (CompareVersions(latest, current) <= 0) {
		emit Log(QString("[update] %1 is the newest release — nothing to do").arg(latest));
		return;
	}
	const QString url = release.value("html_url").toString().isEmpty() ? QStringLiteral("https://github.com/sazonische/ida-workbench/releases/latest") : release.value("html_url").toString();
	emit Log(QString("[update] %1 is available (running %2)").arg(latest, current));
	emit UpdateAvailable(latest, url);
}

const Instance* Manager::InstanceAt(const QString& tag, const QString& name) const {
	for (const Instance& i : _instances) {
		if (i.tag == tag && i.name == name) {
			return &i;
		}
	}
	return nullptr;
}

QString Manager::TargetKey(const Target& target) const {
	return target.tag + "\t" + target.name + "\t" + target.revision;
}

std::optional<Instance> Manager::ResolveTarget(const Target& target) const {
	return ResolveInstance(_workspaces, _extraLibs, target);
}

bool Manager::TargetBusy(const Target& target) const {
	return !TargetBusyReason(target).isEmpty();
}

// The skip log must name the real holder: unpacked leftovers of a crashed session look
// "closed" in the Session column, and a generic "in use" sends the user to the wrong place.
QString Manager::TargetBusyReason(const Target& target, bool allowCrashLeftovers) const {
	if (_depotTags.contains(target.tag)) {
		return QStringLiteral("a Depot update is running for this workspace");
	}
	if (_analyzeJobs.contains(TargetKey(target))) {
		return QStringLiteral("analysis is running");
	}
	const auto requested = ResolveTarget(target);
	if (!requested) {
		return QString();
	}
	const QString path = DatabaseKey(requested->binary);
	if (AnalyzingDatabase(path)) {
		return QStringLiteral("analysis is running");
	}
	if (_opening.contains(path)) {
		return QStringLiteral("database is being opened or released");
	}
	if (DatabaseOpen(path)) {
		return QStringLiteral("database is open in IDA");
	}
	if (UnpackedDatabaseExists(requested->binary)) {
		if (UnpackedDatabaseHeld(requested->binary)) {
			return QStringLiteral("unpacked database files (.id0/.id1) next to the binary are held by a running IDA — "
								  "close it with save first");
		}
		if (!allowCrashLeftovers) {
			return QStringLiteral("unpacked database files (.id0/.id1) of a crashed IDA session lie next to the binary — "
								  "open it in IDA and close with save, or move .id0 .id1 .id2 .nam .til aside");
		}
	}
	return QString();
}

bool Manager::DatabaseOpen(const QString& key) const {
	return _databases.contains(key) || _leases.contains(key);
}

QString Manager::AnalyzePy() const { return QDir(_scriptDir).filePath("analyze_ida.py"); }

void Manager::RequestStopDepot() {
	_stopDepotRequested = true;
}

void Manager::EnsureMcp() {
	if (!_mcp) {
		_mcp = new McpClient(this);
		connect(_mcp, &McpClient::Log, this, &Manager::Log);
		connect(_mcp, &McpClient::Disconnected, this, [this] {
			++_databaseEpoch;
			_databases.clear();
			_leases.clear();
			if (!_shuttingDown) { PublishStatus(); }
		});
	}
	_mcp->Configure(_uvx, QFileInfo(_idaGui).absolutePath());
}

void Manager::PollDatabases() {
	if (_listing || _shuttingDown || (!_mcp && !ComputeReadiness().Ready())) { return; }
	EnsureMcp();
	_listing = true;
	const quint64 epoch = _databaseEpoch;
	_mcp->Call("list_databases", {}, [this, epoch](const QJsonObject& result, const QString& error) {
		_listing = false;
		if (_shuttingDown) { return; }
		if (!error.isEmpty()) { emit Log("[mcp] Cannot list databases: " + error); return; }
		// A listing begun before an open/release cannot revoke the lease that just
		// completed. Ask again rather than publishing a stale registry snapshot.
		if (epoch != _databaseEpoch) { PollDatabases(); return; }
		QHash<QString, QJsonObject> databases;
		for (const auto value : result.value("instances").toArray()) {
			const QJsonObject instance = value.toObject();
			const QString path = instance.value("path").toString();
			if (!path.isEmpty() && instance.value("status").toString() != "unavailable") {
				databases.insert(DatabaseKey(path), instance);
			}
		}
		_databases = databases;
		for (auto it = _leases.begin(); it != _leases.end();) {
			if (!_databases.contains(it.key()) && !_opening.contains(it.key())) { it = _leases.erase(it); }
			else { ++it; }
		}
		PublishStatus(); }, 65000);
}

void Manager::OpenDatabase(const Target& target, bool gui) {
	const auto instance = ResolveTarget(target);
	if (!instance) {
		emit Log("[skip] Library is no longer configured: " + target.name);
		return;
	}
	const QString path = DatabaseKey(instance->binary);
	const QString label = instance->name + "@" + instance->tag;
	if (AnalyzingDatabase(path)) {
		emit Log("[skip] " + label + ": this file is being analyzed");
		return;
	}
	if (_opening.contains(path) || _depotTags.contains(target.tag)) {
		emit Log("[skip] " + label + ": another operation is in progress");
		return;
	}
	const QString input = DatabaseInput(instance->binary);
	if (!QFile::exists(input)) {
		emit Log("[skip] " + label + ": no local binary or database; Replace or download it first");
		return;
	}
	if (gui && _databases.contains(path) && _databases.value(path).value("backend").toString() != "gui") {
		emit Log("[skip] " + label + ": release the headless database in all clients before opening it in the GUI");
		return;
	}
	if (_leases.contains(path)) {
		emit Log("[skip] " + label + ": already attached in Workbench");
		return;
	}
	EnsureMcp();
	++_databaseEpoch;
	_opening.insert(path);
	emit OperationChanged(target, "opening", true);
	emit Log("[open] " + label + (gui ? " in IDA GUI" : " through Hex-Rays MCP"));
	PublishStatus();
	_mcp->Call("open_database", {{"path", input}, {"set_current", false}, {"gui", gui}}, [this, path, label, target](const QJsonObject& result, const QString& error) {
			_opening.remove(path);
			if (_shuttingDown) { return; }
			emit OperationChanged(target, "opening", false);
			const QString id = result.value("instance_id").toString();
			++_databaseEpoch;
			if (!error.isEmpty() || id.isEmpty()) {
				_releaseRequested.remove(path);
				emit Log("[fail] " + label + ": " + (error.isEmpty() ? "MCP returned no database identifier" : error));
			} else {
				_leases.insert(path, id);
				QJsonObject record = result;
				record["status"] = "attached";
				_databases.insert(path, record);
				emit Log("[open] " + label + " attached · " + id);
				if (_releaseRequested.remove(path)) { ReleaseDatabase(path); }
			}
			Refresh(); }, 300000);
}

void Manager::ReleaseDatabase(const QString& path) {
	if (!_leases.contains(path) || !_mcp) {
		emit Log("[release] No Workbench lease for " + path + "; other clients and GUI windows stay open");
		return;
	}
	if (_opening.contains(path)) { return; }
	_opening.insert(path);
	++_databaseEpoch;
	const QString id = _leases.value(path);
	PublishStatus();
	_mcp->Call("save_database", {{"instance_id", id}}, [this, path, id](const QJsonObject&, const QString& error) {
		if (_shuttingDown) { return; }
		if (!error.isEmpty()) {
			_opening.remove(path);
			emit Log("[fail] Save before release failed: " + error + ". The Workbench lease was kept.");
			Refresh();
			return;
		}
		_mcp->Call("close_database", {{"instance_id", id}}, [this, path](const QJsonObject& result, const QString& closeError) {
			++_databaseEpoch;
			_opening.remove(path);
			if (_shuttingDown) { return; }
			if (closeError.isEmpty()) {
				_leases.remove(path);
				if (result.value("final_lease").toBool()) { _databases.remove(path); }
				emit Log("[release] Workbench detached from " + path + "; GUI and other clients retain their databases");
			} else { emit Log("[fail] Cannot release database: " + closeError); }
			Refresh(); }, 65000);
	});
}

QString Manager::Sha256(const QString& path) {
	const QFileInfo fi(path);
	if (!fi.exists()) {
		return {};
	}
	const qint64 sz = fi.size();
	const qint64 mt = fi.lastModified().toMSecsSinceEpoch();

	auto it = _shaCache.constFind(path);
	if (it != _shaCache.constEnd() && it->size == sz && it->mtime == mt) {
		return it->hash; // unchanged -> reuse
	}

	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		return {};
	}
	QCryptographicHash h(QCryptographicHash::Sha256);
	if (!h.addData(&f)) {
		return {};
	}
	const QString hex = QString::fromLatin1(h.result().toHex());
	_shaCache.insert(path, {sz, mt, hex});
	return hex;
}

// --- commands ---------------------------------------------------------------

void Manager::OpenIda(const Target& target) {
	OpenDatabase(target, true);
}

// Derive a "major.minor" Python version from a linked DLL path, e.g.
// ".../python314.dll" -> "3.14", or ".../pythoncore-3.14-64/python3.dll" -> "3.14".
static QString PythonVersionFromPath(const QString& path) {
	static const QRegularExpression reDll(R"(python(\d)(\d+)\.)", QRegularExpression::CaseInsensitiveOption);
	const auto m = reDll.match(QFileInfo(path).fileName());
	if (m.hasMatch()) {
		return m.captured(1) + "." + m.captured(2);
	}
	static const QRegularExpression reAny(R"((\d+)\.(\d+))");
	const auto m2 = reAny.match(path);
	return m2.hasMatch() ? (m2.captured(1) + "." + m2.captured(2)) : QString();
}

Readiness Manager::ComputeReadiness() const {
	Readiness r;

	const bool gui = SupportedIda(_idaGui);
	const bool txt = QFile::exists(_idaText);
	r.ida = gui && txt;
	r.idaMsg = r.ida ? ("IDA: " + _idaGui) : ("Not found: " + (gui ? _idaText : _idaGui) + " — select IDA 9.4 or newer in Settings");

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
		const QString ver = dllExists ? PythonVersionFromPath(linkedDll) : QString();
		r.python = pluginPresent && dllExists;
		r.pythonLabel = ver.isEmpty() ? QStringLiteral("Python") : ("Python " + ver);
		if (!dllExists) {
			r.pythonMsg = "IDA is bound to a Python that is missing:\n" + linkedDll + "\nInstall it or run idapyswitch to relink.";
		} else if (!pluginPresent) {
			r.pythonMsg = "idapython3 plugin missing from the IDA install\n(bound Python " + ver + ": " + linkedDll + ")";
		} else {
			r.pythonMsg = "IDAPython bound to Python " + ver + "\n" + linkedDll;
		}
	} else {
		r.python = pluginPresent;
		r.pythonLabel = QStringLiteral("IDAPython");
		r.pythonMsg = pluginPresent ? ("IDAPython: " + pyPlugin) : "IDAPython plugin not found in the IDA install";
	}

	QString pluginsDir;
#ifdef Q_OS_WIN
	pluginsDir = QDir(qEnvironmentVariable("IDAUSR", QDir(qEnvironmentVariable("APPDATA")).filePath("Hex-Rays/IDA Pro"))).filePath("plugins");
#else
	pluginsDir = QDir::homePath() + "/.idapro/plugins";
#endif
	const QString mcpPlugin = QDir(pluginsDir).filePath("ida-mcp/ida-plugin.json");
	const QString executable = ResolveExecutable(_uvx);
	r.mcp = QFile::exists(mcpPlugin) && !executable.isEmpty();
	QStringList mcpSetup;
	if (executable.isEmpty()) {
		mcpSetup << QString("uvx was not found. It is included with uv. Install it in PowerShell:\n%1\n"
							"Then restart Workbench, or select uvx.exe in Settings > Official Hex-Rays MCP and save.")
						.arg(McpClient::UV_INSTALL_COMMAND);
	}
	if (!QFile::exists(mcpPlugin)) {
		mcpSetup << QString("Install the official IDA MCP plugin after uvx is available:\n%1\n"
							"Restart IDA after installing the plugin.")
						.arg(McpClient::PLUGIN_INSTALL_COMMAND);
	}
	r.mcpMsg = mcpSetup.isEmpty() ? ("Official Hex-Rays MCP · " + executable) : mcpSetup.join("\n\n");
	return r;
}

static QString FormatMegabytes(qint64 n) {
	return n < 0 ? QStringLiteral("—") : QString::number(n / 1048576.0, 'f', 2) + " MB";
}

// One-line meaning for each status, used in the hover tooltip.
static QString ExplainStatus(const QString& s) {
	if (s == "Ready") {
		return "Analyzed and current — ready to open.";
	}
	if (s == "Update available") {
		return "The live game shipped a newer binary — Replace, then Analyze.";
	}
	if (s == "Not analyzed") {
		return "Binary is present but has no .i64 database — press Analyze.";
	}
	if (s == "Re-analyze") {
		return "The database is older than the binary — press Analyze.";
	}
	if (s == "No binary") {
		return "No binary in this folder — drop one in, or Replace from the live source.";
	}
	return s;
}

static QString BuildStatusTip(const Instance& inst, const Cell& c, bool binEx, bool liveEx, bool i64stale) {
	QStringList lines;
	lines << QString("%1 @ %2").arg(inst.name, inst.tag);
	lines << "Status: " + c.state + " — " + ExplainStatus(c.state);
	lines << "Binary: " + inst.binary + (binEx ? "  (" + FormatMegabytes(c.size) + ")" : "  (missing)");
	lines << "Database: " + (c.hasDb ? QString(".i64 present") + (i64stale ? " — older than binary" : "") : QString("none"));
	if (c.up) { lines << "IDA Nexus: " + c.backend + " · " + c.instanceId; }
	if (inst.hasSource) {
		const QString src = liveEx ? inst.sourceBinary + "  (" + FormatMegabytes(c.srcSize) + ")" : QString("not found");
		lines << "Source: " + src;
	}
	return lines.join("\n");
}

void Manager::Refresh() {
	emit ReadinessChanged(ComputeReadiness());
	PollDatabases();
	PublishStatus();
}

void Manager::PublishStatus() {
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
				const QString path = DatabaseKey(inst->binary);
				c.up = DatabaseOpen(path);
				c.owned = _leases.contains(path);
				c.opening = _opening.contains(path);
				c.backend = _databases.value(path).value("backend").toString();
				c.instanceId = _leases.value(path, _databases.value(path).value("instance_id").toString());
				const QString bin = inst->binary;
				const QString db = bin + ".i64";
				const bool binEx = QFile::exists(bin);
				c.hasDb = QFile::exists(db);
				if (binEx) {
					c.size = QFileInfo(bin).size();
				}

				// Compare the analyzed copy against its configured Live source (by
				// content hash). This drives BOTH the "Update available" status
				// (for live-tracking tags) and the "vs source" Δ column.
				const bool liveEx = !inst->sourceBinary.isEmpty() && QFile::exists(inst->sourceBinary);
				if (liveEx) {
					c.srcSize = QFileInfo(inst->sourceBinary).size();
				}
				const bool i64stale = c.hasDb && binEx && QFileInfo(db).lastModified() < QFileInfo(bin).lastModified();
				c.srcDiff = liveEx && binEx && Sha256(bin) != Sha256(inst->sourceBinary);

				if (!binEx) {
					c.state = "No binary";
				} else if (inst->compareSource && c.srcDiff) {
					c.state = "Update available";
				} else if (!c.hasDb) {
					c.state = "Not analyzed";
				} else if (i64stale) {
					c.state = "Re-analyze";
				} else {
					c.state = "Ready";
				}

				c.tip = BuildStatusTip(*inst, c, binEx, liveEx, i64stale);
			}
			r.cells << c;
		}
		rows << r;
	}
	emit StatusReady(rows);
}

void Manager::Start(const QVector<Target>& targets) {
	for (const Target& target : targets) { OpenDatabase(target, false); }
}

// The database files IDA writes/opens. .i64/.idb is the packed database; the rest are
// the unpacked pieces that exist only while a database is open — IDA folds them back
// into the packed file and deletes them on a clean exit, but a force-killed process
// leaves them behind (hundreds of megabytes that also shadow the packed copy).
static const QStringList kUnpackedDbExts = {".id0", ".id1", ".id2", ".nam", ".til"};
static const QStringList kDbExts = QStringList{".i64", ".idb"} + kUnpackedDbExts;

void Manager::Stop(const QVector<Target>& targets) {
	for (const Target& target : targets) {
		const auto instance = ResolveTarget(target);
		if (!instance) { continue; }
		const QString path = DatabaseKey(instance->binary);
		// A pending operation without a lease is an open; one with a lease is already releasing.
		if (_opening.contains(path) && !_leases.contains(path)) {
			_releaseRequested.insert(path);
			emit Log("[release] Waiting for the pending open, then releasing " + instance->name);
		} else {
			ReleaseDatabase(path);
		}
	}
}

// Move a target's database aside (<part>.ida-workbench.bak), first recovering any
// leftover backup from an earlier crash. Returns the extensions backed up, or
// reports failure via *err (and rolls its own partial work back).
static bool BackupDatabase(const QString& bin, QStringList* backedUp, QString* err) {
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
			if (QFile::rename(dbPart, backup)) {
				*backedUp << e;
			} else {
				*err = "cannot back up the existing database (is it open in IDA?)";
				break;
			}
		}
	}
	if (err->isEmpty()) {
		return true;
	}
	for (const QString& e : *backedUp) {
		QFile::rename(bin + e + ".ida-workbench.bak", bin + e);
	}
	backedUp->clear();
	return false;
}

// Put backed-up database parts back and (optionally) restore the original binary,
// preserving its timestamp so the restored DB is not seen as stale.
static void RestoreDatabase(const QString& bin, const QStringList& backedUp, const QString& binaryBackup, const QDateTime& binMtime) {
	for (const QString& e : kDbExts) {
		QFile::remove(bin + e);
	}
	for (const QString& e : backedUp) {
		QFile::rename(bin + e + ".ida-workbench.bak", bin + e);
	}
	if (QFile::exists(binaryBackup)) {
		QString err;
		if (CopyFileAtomically(binaryBackup, bin, &err)) {
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
static QString FormatDuration(qint64 ms) {
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

// Compares the path each job captured when it was issued, not a fresh resolution.
bool Manager::AnalyzingDatabase(const QString& key) const {
	return std::any_of(_analyzeJobs.cbegin(), _analyzeJobs.cend(), [&key](const ActiveAnalyze* job) { return DatabaseKey(job->bin) == key; });
}

void Manager::Analyze(const QVector<Target>& targets, bool force) {
	int launched = 0;
	int skipped = 0;
	for (const Target& target : targets) {
		if (!target.revision.isEmpty()) {
			emit Log(QString("[skip] %1@%2: stored revisions are read-only").arg(target.name, target.tag));
			++skipped;
			continue;
		}
		const QString key = TargetKey(target);
		const auto resolved = ResolveTarget(target);
		if (!resolved) {
			++skipped;
			continue;
		}
		const Instance inst = *resolved;
		const QString label = QString("%1@%2").arg(inst.name, inst.tag);
		if (const QString busy = TargetBusyReason(target); !busy.isEmpty()) {
			emit Log(QString("[skip] %1: %2").arg(label, busy));
			++skipped;
			continue;
		}

		const QString bin = inst.binary;
		const QString db = bin + ".i64";
		const QString binaryBackup = bin + ".ida-workbench.binary.bak";
		QDateTime savedBinMtime;
		if (QFile::exists(binaryBackup)) {
			QString error;
			if (!CopyFileAtomically(binaryBackup, bin, &error)) {
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
		if (!BackupDatabase(bin, &backups, &backupError)) {
			emit Log(QString("[fail] %1: %2").arg(label, backupError));
			++skipped;
			continue;
		}
		if (pullSource) {
			QString error;
			if (QFile::exists(bin)) {
				savedBinMtime = QFileInfo(bin).lastModified();
			}
			if (QFile::exists(bin) && !CopyFileAtomically(bin, binaryBackup, &error)) {
				RestoreDatabase(bin, backups, QString(), QDateTime());
				emit Log(QString("[fail] %1: cannot back up binary: %2").arg(label, error));
				++skipped;
				continue;
			}
			if (!CopyFileAtomically(inst.sourceBinary, bin, &error)) {
				RestoreDatabase(bin, backups, binaryBackup, savedBinMtime);
				emit Log(QString("[fail] %1: copy failed: %2").arg(label, error));
				++skipped;
				continue;
			}
		}

		QStringList args = _analysisArgs.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
		args.removeAll("-B");
		if (!args.contains("-A")) {
			args.prepend("-A");
		}
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
			if (job->tail.size() > 4096) {
				job->tail = job->tail.right(4096);
			}
		});
		connect(job->process, &QProcess::readyReadStandardError, this, [job] {
			job->tail += job->process->readAllStandardError();
			if (job->tail.size() > 4096) {
				job->tail = job->tail.right(4096);
			}
		});
		connect(job->process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, key](int, QProcess::ExitStatus) { FinalizeAnalyze(key); });
		connect(job->process, &QProcess::errorOccurred, this, [this, key](QProcess::ProcessError error) {
			if (error == QProcess::FailedToStart) {
				QTimer::singleShot(0, this, [this, key] { FinalizeAnalyze(key); });
			}
		});
		connect(job->timeout, &QTimer::timeout, this, [this, key] {
			ActiveAnalyze* active = _analyzeJobs.value(key, nullptr);
			if (!active) {
				return;
			}
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
	emit Log(launched ? QString("analyze: %1 started in parallel%2.").arg(launched).arg(skipped ? QString(", %1 skipped").arg(skipped) : QString()) : QString("analyze: nothing to do (%1 skipped).").arg(skipped));
}

void Manager::StopOperations(const QVector<Target>& targets) {
	int count = 0;
	for (const Target& target : targets) {
		ActiveAnalyze* job = _analyzeJobs.value(TargetKey(target), nullptr);
		if (!job) {
			continue;
		}
		job->stopped = true;
		job->process->kill();
		++count;
	}
	if (count) {
		emit Log(QString("stop: stopping %1 analysis task(s)...").arg(count));
	}
}

void Manager::FinalizeAnalyze(const QString& key) {
	ActiveAnalyze* job = _analyzeJobs.take(key);
	if (!job) {
		return;
	}
	job->timeout->stop();
	job->tail += job->process->readAllStandardError() + job->process->readAllStandardOutput();
	QFile::remove(job->bin + ".asm");
	const bool success = !job->stopped && !job->timedOut && job->process->exitStatus() == QProcess::NormalExit && job->process->exitCode() == 0 && QFile::exists(job->db);
	if (success) {
		for (const QString& extension : job->backups) {
			QFile::remove(job->bin + extension + ".ida-workbench.bak");
		}
		QFile::remove(job->binaryBackup);
		emit Log(QString("[ok] %1 (%2) analyzed in %3")
					 .arg(job->label, FormatMegabytes(QFileInfo(job->bin).size()), FormatDuration(job->clock.elapsed())));
	} else {
		RestoreDatabase(job->bin, job->backups, job->binaryBackup, job->savedBinMtime);
		if (job->stopped) {
			emit Log(QString("[stopped] %1: analysis stopped; previous database restored").arg(job->label));
		} else if (job->timedOut) {
			emit Log(QString("[fail] %1: analysis timed out; previous database restored").arg(job->label));
		} else {
			emit Log(QString("[fail] %1 (idat rc=%2) %3; previous database restored")
						 .arg(job->label)
						 .arg(job->process->exitCode())
						 .arg(QString::fromLocal8Bit(job->tail).right(300).trimmed()));
		}
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
	_shuttingDown = true;
	delete _mcp; // ~McpClient sends EOF so Nexus releases this session's leases
	_mcp = nullptr;
	for (ActiveAnalyze* job : _analyzeJobs) {
		job->stopped = true;
		QObject::disconnect(job->process, nullptr, this, nullptr);
		QObject::disconnect(job->timeout, nullptr, this, nullptr);
		if (job->process->state() != QProcess::NotRunning) {
			job->process->kill();
			job->process->waitForFinished(5000);
		}
		RestoreDatabase(job->bin, job->backups, job->binaryBackup, job->savedBinMtime);
		delete job->process;
		delete job->timeout;
		delete job;
	}
	_analyzeJobs.clear();
}

void Manager::Replace(const QVector<Target>& targets) {
	int n = 0;
	const QString revision = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz");
	for (const Target& t : targets) {
		const auto resolved = ResolveTarget(t);
		if (!resolved) {
			continue;
		}
		const Instance* inst = &*resolved;
		const QString tag = QString("%1@%2").arg(inst->name, inst->tag);
		// Crash leftovers nobody holds do not block Replace: they travel into the
		// revision with the old binary, so the unsaved work stays recoverable.
		if (const QString busy = TargetBusyReason(t, true); !busy.isEmpty()) {
			emit Log(QString("[skip] %1: %2").arg(tag, busy));
			continue;
		}
		bool steamWorkspace = false;
		for (const Workspace& workspace : _workspaces) {
			if (workspace.tag == inst->tag) {
				steamWorkspace = workspace.depot.enabled;
				break;
			}
		}
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

		const QString local = inst->binary;
		const QString src = inst->sourceBinary;
		const QString revisionsRoot = QDir(inst->outputRoot).filePath("revisions");
		if (!QDir().mkpath(revisionsRoot)) {
			emit Log(QString("[fail] %1: cannot create revisions directory: %2").arg(tag, revisionsRoot));
			continue;
		}
		// Every IDA database file: .i64/.idb is the packed DB; the rest are the
		// unpacked pieces present only while a DB is open.
		static const QStringList DB_EXTS = {".i64", ".idb", ".id0", ".id1", ".id2", ".nam", ".til"};

		// Only a packed database is something to hand over. Unpacked pieces in the
		// source mean IDA is open there or crashed: the .i64 beside them (if any) is
		// not the latest state, and copying the pieces alone produces a database
		// without .i64 that every later operation sees as open.
		if (UnpackedDatabaseExists(src)) {
			emit Log(QString("[skip] %1: the source has unpacked database files (.id0/.id1) next to %2 — IDA is still "
							 "open there or crashed; close it with save, or move .id0 .id1 .id2 .nam .til aside")
						 .arg(tag, QDir::toNativeSeparators(src)));
			continue;
		}

		// Whether the SOURCE folder already carries a packed IDA database next to
		// the binary decides the mode:
		//   source has DB  -> full replace: copy binary + its DB (ready immediately).
		//   source has none-> fresh binary only: wipe the destination's now-stale
		//                     DB so the cell drops to "Not analyzed".
		static const QStringList PACKED_EXTS = {".i64", ".idb"};
		bool sourceHasDb = false;
		for (const QString& e : PACKED_EXTS) {
			if (QFile::exists(src + e)) {
				sourceHasDb = true;
				break;
			}
		}
		// A database older than its binary was built for a previous build (a game
		// update rewrote the binary and left the old .i64 behind). Carrying it would
		// report "ready" for a database that does not describe this binary.
		if (sourceHasDb) {
			const QDateTime srcBinTime = QFileInfo(src).lastModified();
			for (const QString& e : PACKED_EXTS) {
				if (QFile::exists(src + e) && QFileInfo(src + e).lastModified() < srcBinTime) {
					emit Log(QString("[warn] %1: source database %2 is older than the source binary — not carried over")
								 .arg(tag, QFileInfo(src + e).fileName()));
					sourceHasDb = false;
					break;
				}
			}
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
		const bool crashLeftovers = UnpackedDatabaseExists(local);
		bool archivedAny = false;
		bool archiveOk = true;
		auto archive = [&](const QString& path, const QString& destination) {
			if (!QFile::exists(path)) {
				return true;
			}
			QString archiveError;
			if (!CopyFileAtomically(path, destination, &archiveError)) {
				emit Log(QString("[fail] %1: cannot create revision %2: %3").arg(tag, revision, archiveError));
				return false;
			}
			archivedAny = true;
			return true;
		};
		archiveOk = archive(local, revisionBinary);
		for (const QString& ext : DB_EXTS) {
			if (archiveOk) {
				archiveOk = archive(local + ext, revisionBinary + ext);
			}
		}
		if (!archiveOk) {
			continue;
		}
		if (archivedAny) {
			emit Log(QString("[revision] %1 -> %2").arg(tag, revision));
		}
		if (crashLeftovers) {
			emit Log(QString("[revision] %1: unpacked files of a crashed IDA session were archived too — open %2 in IDA "
							 "to recover unsaved work")
						 .arg(tag, QDir::toNativeSeparators(revisionBinary)));
		}

		// 1) Refresh the binary when it differs (QSaveFile keeps it atomic).
		if (!binaryIdentical) {
			QString copyError;
			if (!CopyFileAtomically(src, local, &copyError)) {
				emit Log(QString("[fail] %1: copy failed: %2").arg(tag, copyError));
				continue;
			}
		}

		// 2) Clear the destination database, then mirror the source's if it has one.
		QStringList notRemoved;
		for (const QString& e : DB_EXTS) {
			if (QFile::exists(local + e) && !QFile::remove(local + e)) {
				notRemoved << e;
			}
		}
		if (!notRemoved.isEmpty()) {
			emit Log(QString("[warn] %1: could not remove old database files (%2) — remove them by hand")
						 .arg(tag, notRemoved.join(" ")));
		}
		QStringList carried;
		if (sourceHasDb) {
			for (const QString& e : PACKED_EXTS) {
				if (QFile::exists(src + e)) {
					QString dbCopyError;
					if (CopyFileAtomically(src + e, local + e, &dbCopyError)) {
						carried << e;
					} else {
						emit Log(QString("[warn] %1: could not copy %2: %3").arg(tag, e, dbCopyError));
					}
				}
			}
		}

		if (sourceHasDb && !binaryIdentical) {
			emit Log(QString("[replaced] %1 <- source binary + database (%2) — ready, no re-analysis needed")
						 .arg(tag, carried.join(" ")));
		} else if (sourceHasDb) { // binary already matched, only the DB was synced
			emit Log(QString("[synced] %1 <- source database (%2), binary already identical — ready")
						 .arg(tag, carried.join(" ")));
		} else { // source had no DB -> stale destination DB removed
			emit Log(QString("[replaced] %1 <- source binary; stale database removed — press Analyze to rebuild")
						 .arg(tag));
		}
		++n;
	}
	emit Log(QString("replace: %1 replaced.").arg(n));
	Refresh();
}

void Manager::UpdateDepot(const QStringList& requestedTags, const QString& requestedManifest, const QString& requestedUsername, const QString& requestedPassword, const QString& requestedAuthCode, bool rememberSession) {
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
	if (activeTags.isEmpty()) {
		for (const Workspace& workspace : _workspaces) {
			if (workspace.depot.enabled) {
				activeTags << workspace.tag;
			}
		}
	}
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
	if (manifestSelection.compare("latest", Qt::CaseInsensitive) != 0 && !QRegularExpression("^[0-9]+$").match(manifestSelection).hasMatch()) {
		emit Log("[fail] Manifest selection must be Latest or a numeric ManifestID");
		finishDepot();
		return;
	}
	QString depotExecutable = ResolveExecutable(_depotDownloader.executable);
	bool downloaderConfigChanged = false;
	if (depotExecutable.isEmpty()) {
		const QString configured = _depotDownloader.executable.trimmed();
		const bool automatic = configured.isEmpty() || configured.compare("DepotDownloader.exe", Qt::CaseInsensitive) == 0 || configured.compare("DepotDownloader", Qt::CaseInsensitive) == 0;
		if (!automatic) {
			emit Log(QString("[fail] DepotDownloader executable does not exist: %1 (change it in Settings)").arg(configured));
			finishDepot();
			return;
		}
		emit Log("[depot] DepotDownloader was not found; installing DepotDownloader 3.4.0...");
		QString error;
		if (!BootstrapDepotDownloader(_configPath, &depotExecutable, &error)) {
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
			if (captured.size() > 16384) {
				captured = captured.right(16384);
			}
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
		if (!workspace.depot.enabled || (!tags.isEmpty() && !tags.contains(workspace.tag))) {
			continue;
		}
		if (_stopDepotRequested.load()) {
			break;
		}
		const QString label = workspace.tag;
		const DepotLayout layout = DepotLayoutFor(workspace.depot);
		const QString depotOs = workspace.depot.os.isEmpty() ? QStringLiteral("any OS") : workspace.depot.os;
		auto depotPath = [&layout](const QString& relative) {
			return layout.pathPrefix.isEmpty() ? QDir::fromNativeSeparators(relative) :
												 QDir::fromNativeSeparators(layout.pathPrefix + "/" + relative);
		};
		// What holds this exact binary right now, or an empty string. A depot update
		// installs a brand-new <ManifestID> directory, so a module held under some other
		// manifest is no reason to refuse the whole workspace. Overwriting a file under a
		// live session would pull the ground from under it; adding the modules beside it
		// is what a completion is for, so the check is per file, never per tag.
		auto heldBy = [this](const QString& path) {
			const QString key = DatabaseKey(path);
			if (AnalyzingDatabase(key)) { return QStringLiteral("analyzing"); }
			if (_opening.contains(key) || DatabaseOpen(key) || UnpackedDatabaseExists(path)) { return QStringLiteral("database is open"); }
			return QString();
		};
		// An open database keeps the file it was opened from, so a new Current does not
		// move it: say so, instead of leaving a session next to a fresh binary.
		auto warnStaleServers = [this, &label](const QString& selectedManifest) {
			QStringList serving;
			for (const Instance& instance : _instances) {
				if (instance.tag == label && DatabaseOpen(DatabaseKey(instance.binary))) {
					serving << instance.name;
				}
			}
			if (!serving.isEmpty()) {
				emit Log(QString("[warn] %1: %2 still use the previous manifest — release those databases and open the new paths for %3")
							 .arg(label, serving.join(", "), selectedManifest));
			}
		};
		emit Log(QString("[depot] %1: checking app %2 (%3), depot %4").arg(label).arg(workspace.depot.appId).arg(depotOs).arg(layout.depotId));

		QTemporaryDir manifestDir(QDir(workspace.output).filePath(".depot-manifest-XXXXXX"));
		QStringList common{"-app", QString::number(workspace.depot.appId), "-depot", QString::number(layout.depotId)};
		if (!workspace.depot.os.isEmpty()) {
			common << "-os" << workspace.depot.os;
		}
		const QString username = requestedUsername.trimmed().isEmpty() ? qEnvironmentVariable("STEAM_USERNAME").trimmed() : requestedUsername.trimmed();
		const QString password = requestedPassword.isEmpty() ? qEnvironmentVariable("STEAM_PASSWORD") : requestedPassword;
		const QString authCode = requestedAuthCode.trimmed().isEmpty() ? qEnvironmentVariable("STEAM_GUARD_CODE").trimmed() : requestedAuthCode.trimmed();
		const QByteArray authInput = authCode.isEmpty() ? QByteArray() : authCode.toUtf8() + '\n';
		if (layout.accountRequired && username.isEmpty()) {
			emit Log(QString("[fail] %1: depot %2 needs a Steam account that owns app %3 — enter it in the download dialog").arg(label).arg(layout.depotId).arg(workspace.depot.appId));
			continue;
		}
		// Account-only apps need the same login for the manifest query and the files.
		if (!username.isEmpty()) {
			common << "-username" << username;
			if (!password.isEmpty()) {
				common << "-password" << password;
			}
			if (rememberSession) {
				common << "-remember-password";
			}
			if (!authCode.isEmpty()) {
				common << "-no-mobile";
			}
		}
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
		const QString manifest = manifestSelection.isEmpty() || manifestSelection.compare("latest", Qt::CaseInsensitive) == 0 ? latestManifest : manifestSelection;
		const bool historicalImport = manifest != latestManifest;
		QStringList downloadFiles = workspace.files;
		if (!historicalImport) {
			QFile catalog;
			const QStringList catalogs = QDir(manifestDir.path()).entryList({QString("manifest_%1_%2.txt").arg(layout.depotId).arg(latestManifest)}, QDir::Files);
			if (!catalogs.isEmpty()) {
				catalog.setFileName(QDir(manifestDir.path()).filePath(catalogs.first()));
			}
			if (catalog.open(QIODevice::ReadOnly | QIODevice::Text)) {
				const QString manifestText = QString::fromUtf8(catalog.readAll()).replace('\\', '/');
				QStringList absent;
				for (const QString& relative : workspace.files) {
					const QString path = depotPath(relative).replace('\\', '/');
					if (!QRegularExpression(QString("(?mi)\\s%1\\s*$").arg(QRegularExpression::escape(path))).match(manifestText).hasMatch()) {
						absent << relative;
					}
				}
				for (const QString& relative : absent) {
					downloadFiles.removeAll(relative);
				}
				if (!absent.isEmpty()) {
					emit Log(QString("[depot] %1: not present in manifest %2: %3").arg(label, manifest, absent.join(", ")));
				}
			}
		}
		const QString manifestRoot = QDir(workspace.output).filePath(manifest);
		bool complete = QDir(manifestRoot).exists();
		for (const QString& relative : historicalImport ? QStringList{} : downloadFiles) {
			if (!QFile::exists(QDir(manifestRoot).filePath(relative))) {
				complete = false;
				break;
			}
		}
		if (complete) {
			QString patchVersion;
			QString serverVersion;
			if (!layout.versionFile.isEmpty()) {
				ReadSteamVersions(QDir(manifestRoot).filePath(layout.versionFile), &patchVersion, &serverVersion);
			}
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
			if (changed) {
				++updated;
			}
			emit Log(QString("[depot] %1: manifest %2 selected as Current").arg(label, manifest));
			if (manifestChanged) {
				warnStaleServers(manifest);
			}
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
		for (const QString& relative : downloadFiles) {
			stream << depotPath(relative) << '\n';
		}
		// Known games ship a build stamp that labels each manifest; other depots have none.
		const QString versionRelative = layout.versionFile;
		if (!versionRelative.isEmpty() && !workspace.files.contains(versionRelative)) {
			stream << depotPath(versionRelative) << '\n';
		}
		fileList.close();

		QStringList downloadArgs = common;
		// DepotDownloader's normal anonymous path resolves the current manifest itself.
		// Pinning even the latest GID with -manifest unnecessarily requires a manifest request code.
		if (historicalImport) {
			downloadArgs << "-manifest" << manifest;
		}
		downloadArgs << "-filelist" << fileListPath << "-dir" << stage.path() << "-validate";
		QString downloadOutput;
		emit Log(QString("[depot] %1: downloading manifest %2 (%3 files)").arg(label, manifest).arg(downloadFiles.size()));
		if (!run(downloadArgs, &downloadOutput, authInput)) {
			const bool authenticationRequired = historicalImport && (downloadOutput.contains("401", Qt::CaseInsensitive) || downloadOutput.contains("No manifest request code", Qt::CaseInsensitive));
			const bool loginSucceeded = !username.isEmpty() && (downloadOutput.contains("Got session token", Qt::CaseInsensitive) || downloadOutput.contains("Got AppInfo", Qt::CaseInsensitive));
			if (authenticationRequired && loginSucceeded) {
				emit Log(QString("[fail] %1: Steam login succeeded, but ManifestID %2 was not resolved for %3 depot %4. Verify that the ID belongs to this depot; otherwise the publisher has disabled that old manifest.")
							 .arg(label, manifest, depotOs, QString::number(layout.depotId)));
			} else if (authenticationRequired && username.isEmpty()) {
				emit Log(QString("[fail] %1: ManifestID %2 was not resolved anonymously for %3 depot %4. Verify the depot/OS; a valid old ID may require Steam account access.")
							 .arg(label, manifest, depotOs, QString::number(layout.depotId)));
			} else if (authenticationRequired) {
				emit Log(QString("[fail] %1: Steam authentication for '%2' did not complete, or ManifestID %3 does not belong to %4 depot %5. Check the credentials, Guard code and depot/OS.")
							 .arg(label, username, manifest, depotOs, QString::number(layout.depotId)));
			} else {
				emit Log(QString("[fail] %1: DepotDownloader failed: %2").arg(label, downloadOutput.right(1000).trimmed()));
			}
			continue;
		}
		QStringList missing;
		for (const QString& relative : downloadFiles) {
			if (!QFileInfo::exists(QDir(stage.path()).filePath(depotPath(relative)))) {
				missing << relative;
			}
		}
		if (!missing.isEmpty()) {
			if (!historicalImport) {
				emit Log(QString("[fail] %1: incomplete depot download; missing: %2").arg(label, missing.join(", ")));
				continue;
			}
			emit Log(QString("[depot] %1: paths absent from historical manifest %2: %3").arg(label, manifest, missing.join(", ")));
			for (const QString& relative : missing) {
				downloadFiles.removeAll(relative);
			}
		}

		const QString stagedSteamInf = versionRelative.isEmpty() ? QString() : QDir(stage.path()).filePath(depotPath(versionRelative));
		QString patchVersion;
		QString serverVersion;
		if (!stagedSteamInf.isEmpty()) {
			ReadSteamVersions(stagedSteamInf, &patchVersion, &serverVersion);
		}
		if (!historicalImport && !layout.versionFallbackUrl.isEmpty() && (patchVersion.isEmpty() || serverVersion.isEmpty())) {
			QByteArray steamInf;
			QString steamInfError;
			if (DownloadUrl(QUrl(layout.versionFallbackUrl), 30000, &steamInf, &steamInfError)) {
				QDir().mkpath(QFileInfo(stagedSteamInf).absolutePath());
				QSaveFile file(stagedSteamInf);
				if (!file.open(QIODevice::WriteOnly) || file.write(steamInf) != steamInf.size() || !file.commit()) {
					emit Log(QString("[warn] %1: could not store GameTracking steam.inf: %2").arg(label, file.errorString()));
				}
			} else {
				emit Log(QString("[warn] %1: GameTracking steam.inf is unavailable: %2").arg(label, steamInfError));
			}
			patchVersion.clear();
			serverVersion.clear();
			ReadSteamVersions(stagedSteamInf, &patchVersion, &serverVersion);
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
		QStringList unchanged;
		QStringList inUse;
		for (const QString& relative : downloadFiles) {
			if (!ok) {
				break;
			}
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
			ok = CopyFileAtomically(incoming, destination, &error);
			if (!ok) {
				break;
			}

			// Re-analysis remains attached to the exact manifest only when its binary is unchanged.
			// Carrying the database over is an optimisation — having it saves an Analyze,
			// losing it costs one — so nothing below fails the update: with servers allowed
			// to run through an update, a live session legitimately holds the previous .i64.
			QStringList sidecarSources;
			if (!completeInPlace) {
				sidecarSources << QDir(manifestRoot).filePath(relative); // now the destination itself
			}
			if (!workspace.depot.manifest.isEmpty() && workspace.depot.manifest != manifest) {
				sidecarSources << QDir(workspace.output).filePath(workspace.depot.manifest + "/" + relative);
			}
			sidecarSources << QDir(workspace.output).filePath(relative);
			const QString moduleName = QFileInfo(relative).completeBaseName();
			for (const QString& previous : sidecarSources) {
				if (!QFile::exists(previous) || Sha256(previous) != Sha256(incoming)) {
					continue;
				}
				// An analyzer has that database moved aside and half-rewritten; copying it now
				// would plant a corrupt .i64 under the new manifest.
				if (AnalyzingDatabase(DatabaseKey(previous))) {
					emit Log(QString("[warn] %1: %2 is being analyzed — its database is not carried into manifest %3")
								 .arg(label, moduleName, manifest));
					continue;
				}
				// Unpacked parts mean the database is open (or a kill left them): only the
				// packed file is a consistent snapshot, the session's own work is not ours.
				bool unpacked = false;
				for (const QString& ext : kUnpackedDbExts) {
					if (QFile::exists(previous + ext)) {
						unpacked = true;
						break;
					}
				}
				for (const QString& ext : unpacked ? QStringList{".i64", ".idb"} : kDbExts) {
					QString sidecarError;
					if (QFile::exists(previous + ext) && !QFile::exists(destination + ext) && !CopyFileAtomically(previous + ext, destination + ext, &sidecarError)) {
						emit Log(QString("[warn] %1: could not carry %2 into manifest %3: %4 — press Analyze there")
									 .arg(label, QFileInfo(previous + ext).fileName(), manifest, sidecarError));
					}
				}
				if (unpacked) {
					emit Log(QString("[warn] %1: %2 has an unpacked database beside it (open in IDA, or left by a kill) — "
									 "manifest %3 gets only the packed copy")
								 .arg(label, moduleName, manifest));
				}
			}
		}
		if (ok && !stagedSteamInf.isEmpty() && QFile::exists(stagedSteamInf) && !downloadFiles.contains(versionRelative)) {
			ok = CopyFileAtomically(stagedSteamInf, QDir(installRoot).filePath(versionRelative), &error);
		}
		if (!ok) {
			// In-place completion writes every file atomically on its own, so a failure here
			// leaves a directory that is merely still incomplete — the next run continues it.
			emit Log(QString("[fail] %1: cannot %2 manifest %3: %4")
						 .arg(label, completeInPlace ? QStringLiteral("complete") : QStringLiteral("prepare"), manifest, error));
			continue;
		}
		if (!unchanged.isEmpty()) {
			emit Log(QString("[depot] %1: manifest %2 already had %3 — left untouched").arg(label, manifest, unchanged.join(", ")));
		}
		if (!inUse.isEmpty()) {
			emit Log(QString("[skip] %1: %2 changed under manifest %3 but is in use — not replaced; stop it and update again")
						 .arg(label, inUse.join(", "), manifest));
		}

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
			if (!patchVersion.isEmpty()) {
				versionDetails << "PatchVersion " + patchVersion;
			}
			if (!serverVersion.isEmpty()) {
				versionDetails << "ServerVersion " + serverVersion;
			}
			emit Log(QString("[manifest] %1 stored %2%3").arg(label, manifest, versionDetails.isEmpty() ? QString() : ": " + versionDetails.join(", ")));
		} else {
			const bool manifestChanged = workspace.depot.manifest != manifest;
			workspace.depot.manifest = manifest;
			workspace.depot.patchVersion = patchVersion;
			workspace.depot.serverVersion = serverVersion;
			configChanged = true;
			++updated;
			QStringList versionDetails;
			if (!patchVersion.isEmpty()) {
				versionDetails << "PatchVersion " + patchVersion;
			}
			if (!serverVersion.isEmpty()) {
				versionDetails << "ServerVersion " + serverVersion;
			}
			emit Log(QString("[depot] %1 Current is %2%3").arg(label, manifest, versionDetails.isEmpty() ? QString() : ": " + versionDetails.join(", ")));
			if (manifestChanged) {
				warnStaleServers(manifest);
			}
		}
	}

	emit Log(QString("depot update: %1 Current selected, %2 historical manifest(s) stored.").arg(updated).arg(imported));
	// End the operation before persisting its result: SaveConfig refuses to write while a
	// depot is live (it would reallocate the _workspaces this loop held a reference into),
	// and by here that reference is gone. Nothing can slip in between — the worker is
	// single-threaded and neither call spins an event loop before the write.
	finishDepot();
	if (configChanged) {
		SaveConfig(View());
	}
	Refresh();
}

// --- view / save ------------------------------------------------------------

ConfigView Manager::View() const {
	ConfigView v;
	v.uvx = _uvx;
	v.idaGui = _idaGui;
	v.idaText = _idaText;
	v.logDir = _logDir;
	v.analysisArgs = _analysisArgs;
	v.configPath = _configPath;
	v.maxLogMB = _maxLogMB;
	v.depotDownloader = _depotDownloader;
	for (const Workspace& workspace : _workspaces) {
		(workspace.depot.enabled ? v.steamWorkspaces : v.workspaces) << workspace;
	}
	v.extraLibs = _extraLibs;
	return v;
}

void Manager::SaveConfig(const ConfigView& cfgIn) {
	// UpdateDepot holds a reference into _workspaces across its downloads, and those spin
	// event loops — a config write landing in between would reallocate the vector under it.
	// The UI already locks its editors while a depot runs; this is the worker refusing to
	// depend on that for memory safety.
	if (!_depotTags.isEmpty()) {
		emit Log("[skip] save: a depot update is running — the configuration is locked until it finishes");
		emit ConfigSaveFinished(false, "Finish active operations before changing configuration.");
		return;
	}
	if ((!_leases.isEmpty() || !_opening.isEmpty() || !_analyzeJobs.isEmpty()) && (cfgIn.uvx != _uvx || cfgIn.idaGui != _idaGui || cfgIn.idaText != _idaText)) {
		emit ConfigSaveFinished(false, "Release databases and finish analyses before changing IDA or MCP paths.");
		return;
	}
	ConfigView cfg = cfgIn;

	const QString validation = ValidateConfig(cfg);
	if (!validation.isEmpty()) {
		emit Log("save: " + validation);
		emit ConfigSaveFinished(false, validation);
		return;
	}
	const QString directoryError = PrepareWorkspaceDirectories(cfg);
	if (!directoryError.isEmpty()) {
		emit Log("save: " + directoryError);
		emit ConfigSaveFinished(false, directoryError);
		return;
	}

	QJsonObject root;
	root["schemaVersion"] = 2;
	root["mcp"] = QJsonObject{{"executable", cfg.uvx.trimmed()}};
	root["logDir"] = cfg.logDir.trimmed();
	root["maxLogSizeMB"] = (cfg.maxLogMB >= 0 && cfg.maxLogMB <= MAX_LOG_MB_CEILING) ? cfg.maxLogMB : DEFAULT_MAX_LOG_MB;
	root["analysisArgs"] = cfg.analysisArgs.trimmed();
	root["ida"] = QJsonObject{{"gui", cfg.idaGui.trimmed()}, {"text", cfg.idaText.trimmed()}};
	root["depotDownloader"] = QJsonObject{
		{"executable", cfg.depotDownloader.executable.trimmed().isEmpty() ? "DepotDownloader.exe" : cfg.depotDownloader.executable.trimmed()},
		{"timeoutMinutes", qBound(1, cfg.depotDownloader.timeoutMinutes, 240)}
	};

	root["workspaces"] = WorkspacesJson(cfg.workspaces, false);
	root["steamWorkspaces"] = WorkspacesJson(cfg.steamWorkspaces, true);

	QJsonArray extra;
	for (int index = 0; index < cfg.extraLibs.size(); ++index) {
		const ExtraLib& e = cfg.extraLibs[index];
		if (e.tag.isEmpty() || e.path.isEmpty()) {
			continue;
		}
		QJsonObject o{{"tag", e.tag.trimmed()}, {"path", e.path.trimmed()}};
		o["color"] = e.color.trimmed().isEmpty() ? GeneratedTagColor(e.tag.trimmed()) : e.color.trimmed();
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
	if (!_depotTags.isEmpty()) { // same reason as SaveConfig: a live depot holds _workspaces
		emit Log("[skip] import: a depot update is running — the configuration is locked until it finishes");
		emit ConfigSaveFinished(false, "Finish active operations before changing configuration.");
		return;
	}
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
	if (QFile prev(_configPath); prev.open(QIODevice::ReadOnly)) {
		previous = prev.readAll();
	}

	QString copyError;
	if (!CopyFileAtomically(src, _configPath, &copyError)) {
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
		if (restore.open(QIODevice::WriteOnly) && restore.write(previous) == previous.size() && restore.commit()) {
			rolledBack = LoadConfig(_configPath, &rollbackError);
		}
	}
	emit Log("import rejected: " + err);
	emit ConfigSaveFinished(false, QString("Import rejected: %1\n\n%2").arg(err, rolledBack ? "The previous configuration was restored." : "WARNING: the previous configuration could not be restored — check " + _configPath));
}
