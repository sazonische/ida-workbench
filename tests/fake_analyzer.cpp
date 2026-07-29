#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QThread>

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	QString output;
	for (const QString& argument : app.arguments()) {
		if (argument.startsWith("-o")) {
			output = argument.mid(2);
		}
	}
	if (output.isEmpty()) {
		return 2;
	}
	QThread::msleep(600);
	QDir().mkpath(QFileInfo(output).absolutePath());
	QFile file(output);
	if (!file.open(QIODevice::WriteOnly) || file.write("analyzed-db") < 0) {
		return 3;
	}
	return 0;
}
