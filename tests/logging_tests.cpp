#include "logging.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>

namespace {
bool Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
	}
	return condition;
}
}

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	QTemporaryDir temp;
	if (!Check(temp.isValid(), "temporary directory creation")) {
		return 1;
	}
	const QString path = temp.filePath("ida-workbench.log");
	Log::Configure(path, 1);
	for (int i = 0; i < 14000; ++i) {
		Log::Write(Log::Level::Info, QString("record-%1 %2").arg(i).arg(QString(80, QLatin1Char('x'))));
	}
	Log::Write(Log::Level::Info, "LATEST-RECORD-MUST-SURVIVE");
	Log::EnforceCap();

	QFile file(path);
	if (!Check(file.open(QIODevice::ReadOnly), "trimmed log opens")) {
		return 1;
	}
	const QByteArray contents = file.readAll();
	if (!Check(contents.size() <= 1024 * 1024, "trimmed log stays under cap")) {
		return 1;
	}
	if (!Check(contents.contains("LATEST-RECORD-MUST-SURVIVE"), "latest record survives")) {
		return 1;
	}
	if (!Check(contents.contains("earlier lines trimmed"), "trim marker is present")) {
		return 1;
	}
	if (!Check(!contents.contains("record-0 "), "oldest records are removed")) {
		return 1;
	}
	return 0;
}
