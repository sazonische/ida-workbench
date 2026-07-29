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
	if (getValue("-app") != "730" || getValue("-depot") != "2347771" || getValue("-os") != "windows") {
		return 5;
	}
	if (args.contains("-manifest-only")) {
		const QString root = getValue("-dir");
		QDir().mkpath(root);
		QFile catalog(QDir(root).filePath("manifest_2347771_2222222222222222222.txt"));
		if (!catalog.open(QIODevice::WriteOnly | QIODevice::Text)) {
			return 7;
		}
		catalog.write("1 1 hash 0 game\\csgo\\bin\\win64\\client.dll\n"
			"1 1 hash 0 game\\bin\\win64\\engine2.dll\n"
			"1 1 hash 0 game\\csgo\\steam.inf\n");
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
