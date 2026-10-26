#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <iostream>
#include <thread>

static void Send(const QJsonObject& object) {
	const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
	// A pipe read is not a message boundary.
	const qsizetype half = bytes.size() / 2;
	std::cout.write(bytes.constData(), half).flush();
	std::this_thread::sleep_for(std::chrono::milliseconds(2));
	std::cout.write(bytes.constData() + half, bytes.size() - half).flush();
}

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	QHash<QString, QJsonObject> databases;
	std::thread input([&app, &databases] {
		std::string line;
		while (std::getline(std::cin, line)) {
			const auto message = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
			QMetaObject::invokeMethod(&app, [message, &databases] {
				if (!message.contains("method")) { return; }
				const QString method = message.value("method").toString();
				if (!message.contains("id")) { return; }
				const auto id = message.value("id");
				if (method == "initialize") {
					Send({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{{"protocolVersion", "2025-03-26"},
						{"serverInfo", QJsonObject{{"name", "ida"}, {"version", "20261023.0.2"}}}, {"capabilities", QJsonObject{}}}}});
					return;
				}
				const auto params = message.value("params").toObject();
				const QString tool = params.value("name").toString();
				const auto args = params.value("arguments").toObject();
				const QString trace = qEnvironmentVariable("IDA_WORKBENCH_FAKE_TRACE");
				if (!trace.isEmpty()) { QFile file(trace); if (file.open(QIODevice::Append)) { file.write(tool.toUtf8() + '\n'); } }
				auto reply = [id](const QJsonObject& result, bool error = false) {
					Send({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{{"isError", error}, {"structuredContent", result},
						{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", error ? "fixture tool error" : QString::fromUtf8(QJsonDocument(result).toJson())}}}}}}});
				};
				if (tool == "fixture_error") { reply({}, true); return; }
				if (tool == "fixture_slow") { QTimer::singleShot(100, QCoreApplication::instance(), [reply] { reply({{"ok", true}}); }); return; }
				if (tool == "fixture_invalid") { std::cout << "broken protocol\n" << std::flush; return; }
				if (tool == "open_database") {
					// Exercise requests initiated by the server with a colliding id.
					Send({{"jsonrpc", "2.0"}, {"id", id}, {"method", "ping"}});
					const QString path = args.value("path").toString();
					const QString instance = QFileInfo(path).fileName();
					QJsonObject record{{"path", path}, {"idb_path", path}, {"instance_id", instance},
						{"backend", args.value("gui").toBool() ? "gui" : "idat"}, {"status", "attached"}};
					databases.insert(instance, record);
					if (path.contains("pending")) { QTimer::singleShot(120, QCoreApplication::instance(), [reply, record] { reply(record); }); }
					else { reply(record); }
				} else if (tool == "list_databases") {
					QJsonArray instances;
					for (const auto& database : databases) { instances.append(database); }
					reply({{"instances", instances}});
				} else if (tool == "close_database") {
					const QString instance = args.value("instance_id").toString();
					const bool final = !instance.contains("shared") && databases.value(instance).value("backend") != "gui";
					if (final) { databases.remove(instance); }
					reply({{"final_lease", final}});
				} else { reply({{"saved", true}}); } }, Qt::QueuedConnection);
		}
		QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
	});
	const int code = app.exec();
	input.join();
	return code;
}
