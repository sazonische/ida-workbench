#include "logging.h"
#include "main_window.h"
#include "manager.h"
#include "paths.h"
#include "theme.h"
#include "ui_util.h"
#include "version.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>

namespace {
	QString InstanceServerName() {
		const QByteArray userKey = QCryptographicHash::hash(QDir::homePath().toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
		return "ida-workbench-" + QString::fromLatin1(userKey);
	}

	// The log used to sit directly in the app folder, next to config.json. Carry it into
	// logs/ before this session writes its first line — a moment later the destination
	// exists and the old history would have to be spliced into it instead of moved.
	// Layout only: config.json has not been read yet, and an install with a custom logDir
	// simply has nothing here to move. A failed rename (a server from a previous session
	// still holding the file) leaves the destination absent, so the next launch retries.
	void MigrateLegacyLogFile() {
		const QString legacy = QDir(Paths::AppHome()).filePath(Log::FileName());
		const QString target = QDir(Paths::DefaultLogDir()).filePath(Log::FileName());
		if (!QFile::exists(legacy) || QFile::exists(target) || !QDir().mkpath(Paths::DefaultLogDir())) {
			return;
		}
		if (QFile::rename(legacy, target)) {
			QFile::remove(legacy + ".lock"); // one-byte sentinel, recreated on demand
		}
	}

	bool ActivateRunningInstance(const QString& serverName, int timeoutMs = 500) {
		QLocalSocket socket;
		socket.connectToServer(serverName, QIODevice::WriteOnly);
		if (!socket.waitForConnected(timeoutMs)) {
			return false;
		}
		socket.write("activate");
		socket.waitForBytesWritten(timeoutMs);
		socket.disconnectFromServer();
		return true;
	}
}

int main(int argc, char* argv[]) {
	QApplication app(argc, argv);
	app.setApplicationName("IDA Workbench");
	app.setApplicationVersion(APP_VERSION);
	app.setWindowIcon(QIcon(":/icons/app_nobg.png"));

	const QString serverName = InstanceServerName();
	if (ActivateRunningInstance(serverName)) {
		return 0;
	}

	// A crashed process can leave a Unix-domain socket behind. Removing it is
	// harmless after the connection attempt above failed; on Windows this simply
	// clears any stale local-server registration.
	QLocalServer::removeServer(serverName);
	QLocalServer instanceServer;
	if (!instanceServer.listen(serverName)) {
		// Cover two processes racing through startup: the winner owns the endpoint,
		// while the loser activates it and exits without constructing a second UI.
		if (ActivateRunningInstance(serverName, 1000)) {
			return 0;
		}
		QMessageBox::critical(nullptr, "IDA Workbench", "IDA Workbench is already running or its instance endpoint is unavailable.");
		return 1;
	}

	// Canonical log first: everything the in-app LOG panel shows plus the Qt
	// diagnostics it does not are appended to one file. Starts at the default location
	// because config.json has not been read yet; the manager re-points this to the
	// configured logDir once it loads, which for a default install is the same folder.
	MigrateLegacyLogFile();
	Log::Configure(QDir(Paths::DefaultLogDir()).filePath(Log::FileName()), 10);
	Log::InstallQtMessageHandler();
	Log::SessionBanner(app.applicationName(), app.applicationVersion());

	// Custom types crossing the worker/UI thread boundary via queued signals.
	qRegisterMetaType<Target>("Target");
	qRegisterMetaType<QVector<Target>>("QVector<Target>");
	qRegisterMetaType<LibRow>("LibRow");
	qRegisterMetaType<QVector<LibRow>>("QVector<LibRow>");
	qRegisterMetaType<ConfigView>("ConfigView");
	qRegisterMetaType<Readiness>("Readiness");

	Ui::LoadIcons();
	QFont appFont("Roboto Flex");
	appFont.setPixelSize(14);
	app.setFont(appFont);
	ApplyMaterial3(app);

	MainWindow mainWindow;
	QObject::connect(&instanceServer, &QLocalServer::newConnection, &mainWindow, [&instanceServer, &mainWindow] {
		while (QLocalSocket* socket = instanceServer.nextPendingConnection()) {
			socket->disconnectFromServer();
			socket->deleteLater();
		}
		mainWindow.ShowFromTray();
	});
	mainWindow.show();
	return app.exec();
}
