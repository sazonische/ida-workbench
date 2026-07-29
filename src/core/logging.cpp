#include "logging.h"
#include "singleton.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QSaveFile>
#include <cstdlib>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace {

	const int kLockWaitMs = 2000;

	class CrossProcessLock {
	public:
		explicit CrossProcessLock(const QString& path) {
#ifdef Q_OS_WIN
			_handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (_handle == INVALID_HANDLE_VALUE) {
				return;
			}
			for (int waitedMs = 0; waitedMs <= kLockWaitMs; waitedMs += 20) {
				if (LockFileEx(_handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &_overlapped)) {
					_locked = true;
					return;
				}
				Sleep(20);
			}
#else
			_fd = ::open(path.toLocal8Bit().constData(), O_CREAT | O_RDWR, 0666);
			if (_fd < 0) {
				return;
			}
			for (int waitedMs = 0; waitedMs <= kLockWaitMs; waitedMs += 20) {
				if (::flock(_fd, LOCK_EX | LOCK_NB) == 0) {
					_locked = true;
					return;
				}
				::usleep(20000);
			}
#endif
		}
		~CrossProcessLock() {
#ifdef Q_OS_WIN
			if (_locked) {
				UnlockFileEx(_handle, 0, 1, 0, &_overlapped);
			}
			if (_handle != INVALID_HANDLE_VALUE) {
				CloseHandle(_handle);
			}
#else
			if (_locked) {
				::flock(_fd, LOCK_UN);
			}
			if (_fd >= 0) {
				::close(_fd);
			}
#endif
		}
		bool IsLocked() const { return _locked; }

	private:
		bool _locked = false;
#ifdef Q_OS_WIN
		HANDLE _handle = INVALID_HANDLE_VALUE;
		OVERLAPPED _overlapped{};
#else
		int _fd = -1;
#endif
	};

	[[nodiscard]] const char* LevelTag(Log::Level level) {
		switch (level) {
			using enum Log::Level;
			case Debug: return "DEBUG";
			case Info: return "INFO ";
			case Warn: return "WARN ";
			case Error: return "ERROR";
		}
		return "INFO ";
	}

} // namespace

class LogManager final : public Singleton<LogManager> {
public:
	void Configure(const QString& path, int maxMB) {
		QMutexLocker lock(&_mutex);
		_path = path;
		_maxMB = maxMB;
		const QDir dir = QFileInfo(path).absoluteDir();
		dir.mkpath(".");

		EnforceCapLocked();
	}

	void EnforceCap() {
		QMutexLocker lock(&_mutex);
		EnforceCapLocked();
	}

	void Write(Log::Level level, const QString& message) {
		QMutexLocker lock(&_mutex);
		if (_path.isEmpty()) {
			return;
		}
		const QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
		QString line = timestamp + "  " + QLatin1String(LevelTag(level)) + "  " + message;
		if (!line.endsWith('\n')) {
			line += '\n';
		}
		AppendLocked(line.toUtf8());
	}

	void SessionBanner(const QString& appName, const QString& version) {
		QMutexLocker lock(&_mutex);
		if (_path.isEmpty()) {
			return;
		}
		const QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
		const QString bar = QString(72, QChar(0x2500)); // ─
		const QString title = version.isEmpty() ? appName : appName + " " + version;
		const QString banner = "\n" + bar + "\n"
							 + "  " + title + "  ·  session started " + timestamp + "\n"
							 + bar + "\n";
		AppendLocked(banner.toUtf8());
	}

	QString Path() const {
		QMutexLocker lock(&_mutex);
		return _path;
	}

private:
	void TrimFileLocked() {
		if (_maxMB <= 0 || _path.isEmpty()) {
			return;
		}
		const qint64 capBytes = qint64(_maxMB) * 1024 * 1024;
		QFile logFile(_path);
		if (!logFile.exists() || logFile.size() <= capBytes) {
			return;
		}
		if (!logFile.open(QIODevice::ReadOnly)) {
			return;
		}
		const qint64 keepBytes = capBytes * 9 / 10;
		logFile.seek(logFile.size() - keepBytes);
		QByteArray tail = logFile.readAll();
		logFile.close();
		const int newlineIndex = tail.indexOf('\n'); // resume at a clean line, not mid-record
		if (newlineIndex >= 0 && newlineIndex + 1 < tail.size()) {
			tail = tail.mid(newlineIndex + 1);
		}
		QSaveFile saveFile(_path);
		saveFile.setDirectWriteFallback(true);
		if (!saveFile.open(QIODevice::WriteOnly)) {
			return;
		}
		saveFile.write("[earlier lines trimmed to keep this log under the size cap]\n");
		saveFile.write(tail);
		saveFile.commit();
	}

	void EnforceCapLocked() {
		if (_path.isEmpty()) {
			return;
		}
		CrossProcessLock processLock(_path + ".lock");
		if (processLock.IsLocked()) {
			TrimFileLocked();
		}
	}

	void AppendLocked(const QByteArray& bytes) {
		if (_path.isEmpty()) {
			return;
		}
		CrossProcessLock processLock(_path + ".lock");
		if (!processLock.IsLocked()) {
			return;
		}
		QFile logFile(_path);
		if (!logFile.open(QIODevice::Append | QIODevice::WriteOnly)) {
			return;
		}
		logFile.write(bytes);
		logFile.close();
		TrimFileLocked();
	}

	mutable QMutex _mutex;
	QString _path;
	int _maxMB = 10;
};

namespace {
	void QtHandler(QtMsgType type, const QMessageLogContext&, const QString& message) {
		Log::Level level = Log::Level::Debug;
		switch (type) {
			case QtDebugMsg: level = Log::Level::Debug; break;
			case QtInfoMsg: level = Log::Level::Info; break;
			case QtWarningMsg: level = Log::Level::Warn; break;
			case QtCriticalMsg: level = Log::Level::Error; break;
			case QtFatalMsg: level = Log::Level::Error; break;
		}
		Log::Write(level, message);
		if (type == QtFatalMsg) {
			std::abort(); // preserve qFatal()'s hard-stop semantics
		}
	}
} // namespace

namespace Log {

	QString FileName() { return QStringLiteral("ida-workbench.log"); }

	void Configure(const QString& path, int maxMB) { LogManager::Instance().Configure(path, maxMB); }

	void EnforceCap() { LogManager::Instance().EnforceCap(); }

	void Write(Level level, const QString& message) { LogManager::Instance().Write(level, message); }

	void SessionBanner(const QString& appName, const QString& version) { LogManager::Instance().SessionBanner(appName, version); }

	QString Path() { return LogManager::Instance().Path(); }

	void InstallQtMessageHandler() { qInstallMessageHandler(QtHandler); }

} // namespace Log
