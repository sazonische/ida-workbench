#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	const QStringList args = app.arguments();
	auto getValue = [&args](const QString& key) {
		const int index = args.indexOf(key);
		return (index >= 0 && index + 1 < args.size()) ? args[index + 1] : QString();
	};
	const bool csgo = getValue("-app") == "4465480";
	// Any other Steam app: an explicit DepotID, files under "content/", any OS, login only.
	const bool generic = getValue("-app") == "480";
	const bool linux = getValue("-os") == "linux";
	const QString expectedDepot = generic ? "481" : csgo ? (linux ? "734" : "732") : "2347771";
	if (generic) {
		if (getValue("-depot") != expectedDepot || args.contains("-os") || getValue("-username") != "test-user" || getValue("-password") != "test-password") {
			return 13;
		}
	} else if ((!csgo && getValue("-app") != "730") || getValue("-depot") != expectedDepot ||
			   (!linux && getValue("-os") != "windows") || (!csgo && linux)) {
		return 5;
	}
	// CS:GO needs account access even for the initial Latest manifest query.
	if (csgo) {
		if (getValue("-username") != "test-user" || getValue("-password") != "test-password" ||
			!args.contains("-no-mobile") || !args.contains("-remember-password")) {
			return 10;
		}
		if (QTextStream(stdin).readLine().trimmed() != "12345") {
			return 11;
		}
	}
	const QStringList files = generic ? QStringList{"content/bin/game.dll", "content/bin/tools/editor.exe"}
		: csgo ? (linux ? QStringList{"csgo/bin/linux64/client_client.so", "csgo/bin/linux64/server_client.so", "bin/linux64/engine_client.so"}
		: QStringList{"csgo/bin/client.dll", "csgo/bin/server.dll", "bin/engine.dll"})
		: QStringList{"game/csgo/bin/win64/client.dll", "game/bin/win64/engine2.dll", "game/csgo/steam.inf"};
	if (args.contains("-manifest-only")) {
		const QString root = getValue("-dir");
		QDir().mkpath(root);
		QFile catalog(QDir(root).filePath(QString("manifest_%1_2222222222222222222.txt").arg(expectedDepot)));
		if (!catalog.open(QIODevice::WriteOnly | QIODevice::Text)) {
			return 7;
		}
		for (const QString& relative : files) {
			catalog.write("1 1 hash 0 " + QDir::toNativeSeparators(relative).toUtf8() + '\n');
		}
		QTextStream(stdout) << "Manifest 2222222222222222222\n";
		return 0;
	}
	const QString root = getValue("-dir");
	const QString listPath = getValue("-filelist");
	const QString manifest = getValue("-manifest");
	if (!manifest.isEmpty() && manifest != "1111111111111111111") {
		return 6;
	}
	const bool historical = (manifest == "1111111111111111111");
	if (historical && getValue("-username") == "test-user") {
		if (getValue("-password") != "test-password" || !args.contains("-no-mobile") || !args.contains("-remember-password")) {
			return 8;
		}
		if (QTextStream(stdin).readLine().trimmed() != "12345") {
			return 9;
		}
	}
	QFile fileList(listPath);
	if (root.isEmpty() || !fileList.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return 2;
	}
	while (!fileList.atEnd()) {
		const QString relative = QString::fromUtf8(fileList.readLine()).trimmed();
		if (relative.isEmpty()) {
			continue;
		}
		if (csgo && !files.contains(relative) && relative != "csgo/steam.inf") {
			return 12;
		}
		// A depot without a known game's build stamp must not be asked for one.
		if (generic && !files.contains(relative)) {
			return 14;
		}
		// The binary depots do not ship CS:GO's common-content steam.inf.
		if (csgo && relative == "csgo/steam.inf") {
			continue;
		}
		const QString path = QDir(root).filePath(relative);
		if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
			return 3;
		}
		QFile output(path);
		if (!output.open(QIODevice::WriteOnly)) {
			return 4;
		}
		if (relative.endsWith("csgo/steam.inf", Qt::CaseInsensitive)) {
			output.write(historical ? "PatchVersion=1.40.0.0\nServerVersion=1900000\n" : "PatchVersion=1.41.7.2\nServerVersion=2000877\n");
		} else {
			output.write((historical ? "old-depot:" : "depot:") + relative.toUtf8());
		}
	}
	return 0;
}
