#include "mcp_client.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>
#include <utility>

#ifdef Q_OS_WIN
	#define NOMINMAX
	#include <windows.h>
#endif

McpClient::McpClient(QObject* parent) : QObject(parent) {}
McpClient::~McpClient() { Shutdown(); }

QString McpClient::FindUvx() {
	// uv's installer puts uvx in ~/.local/bin, which an already running session may not have on PATH.
	const QString found = QStandardPaths::findExecutable("uvx");
	return found.isEmpty() ? QStandardPaths::findExecutable("uvx", {QDir::homePath() + "/.local/bin"}) : found;
}

QStringList McpClient::ServerArguments(const QString& agent) {
	return {"--exclude-newer=1s", "ida-mcp", "stdio", "--agent=" + agent};
}

QJsonObject McpClient::ClientConfiguration(const QString& executable, const QString& idaDirectory, const QString& agent) {
	return {{"command", executable}, {"args", QJsonArray::fromStringList(ServerArguments(agent))}, {"env", QJsonObject{{"IDADIR", QDir::toNativeSeparators(idaDirectory)}}}};
}

QString McpClient::ClaudeConfiguration(const QString& executable, const QString& idaDirectory) {
	const QJsonObject servers{{SERVER_NAME, ClientConfiguration(executable, idaDirectory, "claude-code")}};
	return QString::fromUtf8(QJsonDocument(QJsonObject{{"mcpServers", servers}}).toJson(QJsonDocument::Indented));
}

QString McpClient::CodexConfiguration(const QString& executable, const QString& idaDirectory) {
	auto quote = [](const QString& text) {
		const QByteArray json = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
		return QString::fromUtf8(json.mid(1, json.size() - 2));
	};
	const QString table = QString("mcp_servers.") + SERVER_NAME;
	return "[" + table + "]\ncommand = " + quote(executable) + "\nargs = " + QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(ServerArguments("codex"))).toJson(QJsonDocument::Compact)) + "\n\n[" + table + ".env]\nIDADIR = " + quote(QDir::toNativeSeparators(idaDirectory)) + "\n";
}

void McpClient::Configure(const QString& executable, const QString& idaDirectory) {
	if (_executable == executable && _idaDirectory == idaDirectory) { return; }
	Shutdown();
	_executable = executable;
	_idaDirectory = idaDirectory;
}

bool McpClient::Running() const { return _process && _process->state() != QProcess::NotRunning; }

void McpClient::Send(const QJsonObject& message) {
	if (_process) { _process->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n'); }
}

void McpClient::Start() {
	if (Running()) { return; }
	_ready = false;
	_output.clear();
	_errors.clear();
	if (_process) {
		QObject::disconnect(_process, nullptr, this, nullptr);
		_process->deleteLater();
	}
	_process = new QProcess(this);
	_process->setProgram(_executable);
	_process->setArguments(ServerArguments("ida-workbench"));
	QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
	environment.insert("IDADIR", _idaDirectory);
	_process->setProcessEnvironment(environment);
#ifdef Q_OS_WIN
	_process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
#endif
	connect(_process, &QProcess::started, this, [this] {
		Send({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", QJsonObject{{"protocolVersion", "2025-03-26"}, {"capabilities", QJsonObject{}}, {"clientInfo", QJsonObject{{"name", "IDA Workbench"}, {"version", QCoreApplication::applicationVersion()}}}}}});
	});
	connect(_process, &QProcess::readyReadStandardOutput, this, &McpClient::ReadOutput);
	connect(_process, &QProcess::readyReadStandardError, this, [this] {
		_errors += _process->readAllStandardError();
		qsizetype end = 0;
		while ((end = _errors.indexOf('\n')) >= 0) {
			const QString line = QString::fromUtf8(_errors.left(end)).trimmed();
			_errors.remove(0, end + 1);
			if (!line.isEmpty()) { emit Log("[hex-rays] " + line.left(2000)); }
		}
		_errors = _errors.right(8192);
	});
	connect(_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart) { Fail("Cannot start Hex-Rays MCP: " + _process->errorString()); }
	});
	// Fail and Shutdown disconnect first, so this only sees an exit nobody asked for.
	connect(_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus) {
		Fail(QString("Hex-Rays MCP exited (%1). %2").arg(code).arg(QString::fromUtf8(_errors).trimmed()));
	});
	_process->start();
	QProcess* process = _process;
	QTimer::singleShot(60000, this, [this, process] {
		if (_process == process && !_ready && Running()) { Fail("Hex-Rays MCP initialization timed out. Check uvx, network and IDA settings."); }
	});
}

void McpClient::Call(const QString& tool, const QJsonObject& arguments, Reply reply, int timeoutMs) {
	const int id = _nextId++;
	_pending.insert(id, {{{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", QJsonObject{{"name", tool}, {"arguments", arguments}}}}, std::move(reply), false});
	Start();
	Dispatch();
	QTimer::singleShot(timeoutMs, this, [this, id] {
		if (!_pending.contains(id)) { return; }
		const Pending pending = _pending.take(id);
		if (pending.sent) {
			Send({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", QJsonObject{{"requestId", id}, {"reason", "Workbench request timed out"}}}});
		}
		pending.reply({}, "Hex-Rays MCP request timed out. Check the session in the MCP dashboard.");
	});
}

void McpClient::Dispatch() {
	if (!_ready) { return; }
	for (Pending& pending : _pending) {
		if (!pending.sent) {
			pending.sent = true;
			Send(pending.payload);
		}
	}
}

void McpClient::ReadOutput() {
	_output += _process->readAllStandardOutput();
	if (_output.size() > 16 * 1024 * 1024) {
		Fail("Hex-Rays MCP sent an oversized response.");
		return;
	}
	qsizetype end = 0;
	while ((end = _output.indexOf('\n')) >= 0) {
		const QByteArray line = _output.left(end);
		_output.remove(0, end + 1);
		QJsonParseError parse;
		const QJsonDocument document = QJsonDocument::fromJson(line, &parse);
		if (!document.isObject()) {
			Fail("Invalid JSON from Hex-Rays MCP: " + parse.errorString());
			return;
		}
		const QJsonObject message = document.object();
		if (!message.contains("id")) { continue; }
		// No client-side capabilities were advertised. Answer server requests instead
		// of accidentally treating their ids as replies to our own tool calls.
		if (message.contains("method")) {
			const bool ping = message.value("method").toString() == "ping";
			QJsonObject response{{"jsonrpc", "2.0"}, {"id", message.value("id")}};
			response[ping ? "result" : "error"] = ping ? QJsonObject{} : QJsonObject{{"code", -32601}, {"message", "Unsupported client method"}};
			Send(response);
			continue;
		}
		const int id = message.value("id").toInt(-1);
		if (id == 0) {
			if (message.contains("error")) {
				Fail("MCP initialization failed: " + message.value("error").toObject().value("message").toString());
				return;
			}
			const auto info = message.value("result").toObject().value("serverInfo").toObject();
			if (info.value("name").toString() != "ida") {
				Fail("The configured executable is not the official Hex-Rays IDA MCP server.");
				return;
			}
			Send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
			_ready = true;
			emit Log("[mcp] Connected to Hex-Rays IDA MCP " + info.value("version").toString());
			Dispatch();
			continue;
		}
		if (!_pending.contains(id)) { continue; }
		const Pending pending = _pending.take(id);
		QString error = message.value("error").toObject().value("message").toString();
		const QJsonObject result = message.value("result").toObject();
		QJsonObject structured = result.value("structuredContent").toObject();
		QStringList content;
		for (const auto block : result.value("content").toArray()) {
			if (block.toObject().value("type") == "text") { content << block.toObject().value("text").toString(); }
		}
		if (result.value("isError").toBool()) {
			error = content.join('\n');
			if (error.isEmpty()) { error = "Hex-Rays MCP tool failed"; }
		}
		if (structured.isEmpty() && !content.isEmpty()) { structured = QJsonDocument::fromJson(content.join('\n').toUtf8()).object(); }
		pending.reply(structured, error);
	}
}

// Replies run on a detached copy: a callback may queue the next call into a fresh map.
void McpClient::FailPending(const QString& message) {
	for (const Pending& request : std::exchange(_pending, {})) { request.reply({}, message); }
}

void McpClient::Fail(const QString& message) {
	_ready = false;
	QProcess* failed = std::exchange(_process, nullptr);
	if (failed) {
		QObject::disconnect(failed, nullptr, this, nullptr);
		if (failed->state() != QProcess::NotRunning) { failed->kill(); }
		failed->deleteLater();
	}
	emit Log("[mcp] " + message);
	emit Disconnected();
	FailPending(message);
}

void McpClient::Shutdown() {
	_ready = false;
	if (_process) {
		QObject::disconnect(_process, nullptr, this, nullptr);
		if (Running()) {
			// EOF lets Nexus release leases and save its workers. Never terminate an
			// IDA process or delete unpacked database files owned by another client.
			_process->closeWriteChannel();
			if (!_process->waitForFinished(8000)) {
				_process->kill();
				_process->waitForFinished(1000);
			}
		}
		delete _process;
		_process = nullptr;
	}
	FailPending("MCP connection closed");
}
