#pragma once
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QProcess>
#include <functional>

// One official Hex-Rays MCP process per Workbench session. IDA Nexus owns the
// database processes; this client only owns its leases and never kills IDA.
class McpClient : public QObject {
	Q_OBJECT
public:
	// The server name exported to agents and quoted in the prompt that goes with it.
	static constexpr char SERVER_NAME[] = "ida94";
	static constexpr char UV_INSTALL_COMMAND[] = "winget install --id astral-sh.uv -e";
	static constexpr char PLUGIN_INSTALL_COMMAND[] = "uvx ida-hcli mcp install";

	using Reply = std::function<void(const QJsonObject&, const QString&)>;
	explicit McpClient(QObject* parent = nullptr);
	~McpClient() override;
	void Configure(const QString& executable, const QString& idaDirectory);
	void Call(const QString& tool, const QJsonObject& arguments, Reply reply, int timeoutMs = 180000);
	void Shutdown();
	static QString FindUvx();
	static QString ClaudeConfiguration(const QString& executable, const QString& idaDirectory);
	static QString CodexConfiguration(const QString& executable, const QString& idaDirectory);

signals:
	void Log(const QString& message);
	void Disconnected();

private:
	struct Pending {
		QJsonObject payload;
		Reply reply;
		bool sent = false;
	};
	QProcess* _process = nullptr;
	QString _executable;
	QString _idaDirectory;
	QByteArray _output;
	QByteArray _errors;
	QMap<int, Pending> _pending; // ordered by id, so queued calls go out in issue order
	int _nextId = 1;
	bool _ready = false;
	static QStringList ServerArguments(const QString& agent);
	static QJsonObject ClientConfiguration(const QString& executable, const QString& idaDirectory, const QString& agent);
	[[nodiscard]] bool Running() const;
	void Start();
	void Send(const QJsonObject& message);
	void ReadOutput();
	void Fail(const QString& message);
	void Dispatch();
	void FailPending(const QString& message);
};
