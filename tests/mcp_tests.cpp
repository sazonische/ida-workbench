#include "manager.h"
#include "mcp_client.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>

static bool Check(bool ok, const char* message) {
	if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); }
	return ok;
}

static bool Wait(const std::function<bool()>& predicate, int timeout = 5000) {
	QElapsedTimer timer;
	timer.start();
	while (!predicate() && timer.elapsed() < timeout) {
		QCoreApplication::processEvents();
		QThread::msleep(5);
	}
	return predicate();
}

static void Write(const QString& path, const QByteArray& data) {
	QFile file(path);
	if (file.open(QIODevice::WriteOnly)) { file.write(data); }
}

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	if (app.arguments().value(1) == "--live" && app.arguments().size() == 5) {
		McpClient live;
		live.Configure(app.arguments()[2], app.arguments()[3]);
		QObject::connect(&live, &McpClient::Log, &app, [](const QString& message) { std::fprintf(stderr, "%s\n", qPrintable(message)); });
		auto call = [&](const QString& name, const QJsonObject& args, QJsonObject* output) {
			bool received = false;
			QString failure;
			live.Call(name, args, [&](const QJsonObject& result, const QString& error) {
				*output = result; failure = error; received = true; }, 300000);
			const bool complete = Wait([&] { return received; }, 310000);
			std::fprintf(stderr, "%s: %s\n", qPrintable(name), complete && failure.isEmpty() ? "OK" : qPrintable(failure));
			return complete && failure.isEmpty();
		};
		QJsonObject opened;
		if (!call("open_database", {{"path", app.arguments()[4]}, {"set_current", false}}, &opened)) { return 1; }
		const QString id = opened.value("instance_id").toString();
		QJsonObject result;
		if (!call("reference", {{"query", "functions"}}, &result)) { return 1; }
		if (!call("execute_python", {{"instance_id", id}, {"code", "import ida_funcs\nida_funcs.get_func_qty()"}}, &result)) { return 1; }
		std::fprintf(stderr, "analysis result: %s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
		if (!call("save_database", {{"instance_id", id}}, &result)) { return 1; }
		std::fprintf(stderr, "saved database: %s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
		if (!call("close_database", {{"instance_id", id}}, &result)) { return 1; }
		return 0;
	}
	if (app.arguments().size() < 2) { return 1; }
	const QString executable = app.arguments()[1];
	QTemporaryDir directory;
	const QString trace = directory.filePath("trace.txt");
	qputenv("IDA_WORKBENCH_FAKE_TRACE", trace.toUtf8());
	McpClient client;
	client.Configure(executable, directory.path());
	bool done = false;
	QString error;
	client.Call("open_database", {{"path", directory.filePath("client.dll.i64")}}, [&](const QJsonObject& value, const QString& e) {
		error = e;
		done = value.value("instance_id").toString() == "client.dll.i64";
	});
	if (!Check(Wait([&] { return done; }) && error.isEmpty(), "stdio handshake, fragmented reply and server-initiated ping")) { return 1; }
	done = false;
	client.Call("fixture_error", {}, [&](const QJsonObject&, const QString& e) { error = e; done = true; });
	if (!Check(Wait([&] { return done; }) && error == "fixture tool error", "MCP tool errors propagated")) { return 1; }
	int callbacks = 0;
	client.Call("fixture_slow", {}, [&](const QJsonObject&, const QString& e) { error = e; ++callbacks; }, 20);
	if (!Check(Wait([&] { return callbacks == 1; }) && error.contains("timed out"), "tool timeout reported")) { return 1; }
	done = false;
	QElapsedTimer settling;
	settling.start();
	Wait([&] { return settling.elapsed() >= 200; });
	client.Call("list_databases", {}, [&](const QJsonObject& r, const QString& e) { done = e.isEmpty() && r.value("instances").toArray().size() == 1; });
	if (!Check(Wait([&] { return done; }) && callbacks == 1, "late replies cannot complete a timed-out request twice")) { return 1; }
	client.Shutdown();
	done = false;
	client.Call("fixture_invalid", {}, [&](const QJsonObject&, const QString& e) { error = e; done = true; });
	if (!Check(Wait([&] { return done; }) && error.contains("Invalid JSON"), "malformed protocol fails pending calls")) { return 1; }
	done = false;
	client.Call("list_databases", {}, [&](const QJsonObject& r, const QString& e) { done = e.isEmpty() && r.value("instances").toArray().isEmpty(); });
	if (!Check(Wait([&] { return done; }), "client reconnects after a protocol failure")) { return 1; }
	client.Shutdown();
	const QString config = McpClient::CodexConfiguration("C:\\Tools With Spaces\\uvx.exe", "C:\\IDA 9.4");
	if (!Check(config.contains("[mcp_servers.ida94.env]") && config.contains("C:\\\\IDA 9.4") && !config.contains("url ="), "one stdio config pins IDA and escapes Windows paths")) { return 1; }

	const QString binary = directory.filePath("shared.dll");
	const QString pending = directory.filePath("pending.dll");
	Write(binary, "fixture");
	Write(binary + ".i64", "database");
	Write(pending, "fixture");
	Write(pending + ".i64", "database");
	QJsonObject root{{"schemaVersion", 2}, {"ida", QJsonObject{{"gui", executable}, {"text", executable}}}, {"mcp", QJsonObject{{"executable", executable}}}, {"logDir", directory.path()}, {"extraLibs", QJsonArray{QJsonObject{{"tag", "shared"}, {"path", binary}, {"color", "#FFFFFF"}}, QJsonObject{{"tag", "pending"}, {"path", pending}, {"color", "#FFFFFF"}}}}};
	const QString path = directory.filePath("config.json");
	Write(path, QJsonDocument(root).toJson());
	Manager manager;
	QString loadError;
	if (!Check(manager.LoadConfig(path, &loadError), qPrintable(loadError))) { return 1; }
	QVector<LibRow> rows;
	QObject::connect(&manager, &Manager::StatusReady, &app, [&](const QVector<LibRow>& result) { rows = result; });
	manager.Start({{"shared", "shared"}});
	auto attached = [&] {
		for (const auto& row : rows) {
			if (row.name == "shared") { return row.cells[0].owned; }
		}
		return false;
	};
	if (!Check(Wait(attached), "Workbench acquires a database lease")) { return 1; }
	manager.Stop({{"shared", "shared"}});
	if (!Check(Wait([&] { return !attached(); }), "Workbench releases its own lease")) { return 1; }
	if (!Check(rows[0].cells[0].up, "shared database stays open for other clients")) { return 1; }
	QFile log(trace);
	if (!Check(log.open(QIODevice::ReadOnly), "MCP trace written")) { return 1; }
	const QByteArray operations = log.readAll();
	const qsizetype savePosition = operations.lastIndexOf("save_database\n");
	const qsizetype closePosition = operations.lastIndexOf("close_database\n");
	if (!Check(savePosition >= 0 && closePosition > savePosition, "database saved before its lease is released")) { return 1; }
	manager.Start({{"pending", "pending"}});
	manager.Stop({{"pending", "pending"}});
	if (!Check(Wait([&] { return rows.size() == 2 && !rows[1].cells[1].opening && !rows[1].cells[1].owned; }), "release during a pending open does not orphan a lease")) { return 1; }
	std::puts("MCP transport and database lease tests passed");
	return 0;
}
