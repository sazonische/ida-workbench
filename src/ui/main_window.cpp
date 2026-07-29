#include "main_window.h"
#include "delegates.h"
#include "logging.h"
#include "palette.h"
#include "settings_panel.h"
#include "ui_util.h"
#include "version.h"

#include <QApplication>
#include <QAbstractButton>
#include <QClipboard>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QFocusEvent>
#include <QFrame>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegion>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStackedWidget>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QSyntaxHighlighter>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextCharFormat>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVersionNumber>
#include <QWidget>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <numeric>

// COL_ORDER is a hidden scratch column: ApplySort() writes each row's target rank
// into it and lets QTableWidget::sortItems reorder the rows to that permutation.
enum { COL_SEL = 0,
	   COL_MODULE,
	   COL_TAG,
	   COL_SERVER,
	   COL_PORT,
	   COL_STATUS,
	   COL_SIZE,
	   COL_DELTA,
	   COL_ORDER,
	   COL_COUNT };

namespace {

	// A cell that sorts by a numeric key stored in Qt::UserRole (so "16.51 MB" sorts
	// above "8.86 MB", and ports sort as numbers). Falls back to text when unset.
	// A cell that sorts by a numeric key stored in Qt::UserRole (so "16.51 MB" sorts
	// above "8.86 MB", and ports sort as numbers). Falls back to text when unset.
	class NumericItem final : public QTableWidgetItem {
	public:
		bool operator<(const QTableWidgetItem& other) const override {
			const QVariant a = data(Qt::UserRole), b = other.data(Qt::UserRole);
			if (a.isValid() && b.isValid()) return a.toLongLong() < b.toLongLong();
			return QTableWidgetItem::operator<(other);
		}
	};

	class HeaderCheckBox final : public QAbstractButton {
	public:
		explicit HeaderCheckBox(QWidget* parent = nullptr) : QAbstractButton(parent) {
			setCursor(Qt::PointingHandCursor);
			setToolTip("Select all visible modules");
			setFixedSize(24, 24);
			setAttribute(Qt::WA_TranslucentBackground);
		}

		void setCheckState(Qt::CheckState state) {
			if (_state == state) return;
			_state = state;
			update();
		}

	protected:
		void paintEvent(QPaintEvent*) override {
			QPainter painter(this);
			painter.setRenderHint(QPainter::Antialiasing);
			const QRectF box((width() - 18) / 2.0, (height() - 18) / 2.0, 18, 18);
			if (_state == Qt::Unchecked) {
				painter.setPen(QPen(isEnabled() ? Pal::OUTLINE : QColor("#A9A3AD"), 2));
				painter.setBrush(Qt::NoBrush);
				painter.drawRoundedRect(box.adjusted(1, 1, -1, -1), 4, 4);
			} else {
				painter.setPen(Qt::NoPen);
				painter.setBrush(isEnabled() ? Pal::PRIMARY : QColor("#A9A3AD"));
				painter.drawRoundedRect(box, 4, 4);
				if (_state == Qt::Checked) {
					Ui::DrawSpinningGlyph(&painter, "check", 16, Pal::ON_PRIMARY, box.center(), 0, true);
				} else {
					painter.setBrush(Pal::ON_PRIMARY);
					painter.drawRoundedRect(QRectF(box.center().x() - 5, box.center().y() - 1, 10, 2), 1, 1);
				}
			}
		}

	private:
		Qt::CheckState _state = Qt::Unchecked;
	};

	class SelectHeader final : public TrailingInsetHeader {
	public:
		explicit SelectHeader(Qt::Orientation orientation, QWidget* parent = nullptr) :
			TrailingInsetHeader(orientation, parent), _checkBox(new HeaderCheckBox(viewport())) {
			connect(this, &QHeaderView::geometriesChanged, this, [this] { positionCheckBox(); });
			connect(this, &QHeaderView::sectionResized, this, [this](int, int, int) { positionCheckBox(); });
			_checkBox->raise();
		}

		void setCheckState(Qt::CheckState state) { _checkBox->setCheckState(state); }
		void setSelectionEnabled(bool enabled) { _checkBox->setEnabled(enabled); }
		void setToggleHandler(std::function<void()> handler) {
			connect(_checkBox, &QAbstractButton::clicked, this, [handler = std::move(handler)] { handler(); });
		}

	protected:
		void resizeEvent(QResizeEvent* event) override {
			QHeaderView::resizeEvent(event);
			positionCheckBox();
		}

	private:
		void positionCheckBox() {
			// QTableWidget indicators sit 6 px left of the geometric section centre
			// because the item delegate reserves leading content padding. Match that
			// visual axis so the header and every row form one straight column.
			const int x = sectionViewportPosition(COL_SEL) + (sectionSize(COL_SEL) - _checkBox->width()) / 2 - 6;
			const int y = (height() - _checkBox->height()) / 2;
			_checkBox->move(x, y);
			_checkBox->setVisible(!isSectionHidden(COL_SEL));
			_checkBox->raise();
		}
		HeaderCheckBox* _checkBox;
	};

	// Main table behavior; the shared Ui::OverlayVerticalScrollBar supplies the
	// scrollbar and its content inset, as it does for every settings table.
	class OverlayTableWidget final : public QTableWidget {
	public:
		explicit OverlayTableWidget(int rows, int columns, QWidget* parent = nullptr) :
			QTableWidget(rows, columns, parent) {
			setMouseTracking(true);
			viewport()->setMouseTracking(true);
			_outline = Ui::RoundedOutline(this, 16.0, Pal::OUTLINE_VARIANT);
		}

	protected:
		void resizeEvent(QResizeEvent* event) override {
			QTableWidget::resizeEvent(event);
			// QTableWidget's header and viewport are native child widgets.  They
			// otherwise paint square white corners over the rounded QSS frame.
			// The HTML card clips all of its contents to the same 16 px radius.
			QPainterPath clipPath;
			clipPath.addRoundedRect(QRectF(rect()), 16.0, 16.0);
			setMask(QRegion(clipPath.toFillPolygon().toPolygon()));
			_outline->raise();
		}

		void mouseMoveEvent(QMouseEvent* event) override {
			const int row = indexAt(event->position().toPoint()).row();
			if (property("hoveredRow").toInt() != row) {
				setProperty("hoveredRow", row);
				viewport()->update();
			}
			QTableWidget::mouseMoveEvent(event);
		}

		void leaveEvent(QEvent* event) override {
			setProperty("hoveredRow", -1);
			viewport()->update();
			QTableWidget::leaveEvent(event);
		}

	private:
		QWidget* _outline = nullptr;
	};

	// Lightweight regex highlighter for the MCP config previews (JSON / TOML). Keys,
	// string values and URLs each get their own colour so the config reads clearly.
	class CodeHighlighter final : public QSyntaxHighlighter {
	public:
		enum Mode { Json,
					Toml };
		CodeHighlighter(QTextDocument* doc, Mode mode) :
			QSyntaxHighlighter(doc), _mode(mode) {}

	protected:
		void highlightBlock(const QString& text) override {
			auto apply = [this, &text](const QString& pattern, const QColor& color) {
				const QRegularExpression re(pattern);
				auto it = re.globalMatch(text);
				while (it.hasNext()) {
					const QRegularExpressionMatch m = it.next();
					QTextCharFormat fmt;
					fmt.setForeground(color);
					setFormat(int(m.capturedStart()), int(m.capturedLength()), fmt);
				}
			};
			const QColor punct("#79747E"), key("#8250DF"), str("#0B57D0"), url("#0B7B83");
			if (_mode == Json) {
				apply("[{}\\[\\],:]", punct);
				apply("\"[^\"]*\"", str);		   // strings (values first)
				apply("\"[^\"]*\"(?=\\s*:)", key); // quoted keys (before a colon) win over values
				apply("\"https?://[^\"]*\"", url); // URLs win over plain strings
			} else {
				apply("=", punct);
				apply("\\[[^\\]]*\\]", key);			   // [table.header]
				apply("^\\s*[A-Za-z0-9_]+(?=\\s*=)", key); // bare key before '='
				apply("\"[^\"]*\"", str);
				apply("\"https?://[^\"]*\"", url);
			}
		}

	private:
		Mode _mode;
	};

	// The grip between the table and the log. Dragging it resizes the log the way
	// the VSCode terminal / browser devtools panels resize; a double-click toggles
	// the bottom pane collapsed, remembering its height so the next one restores it.
	class LogSplitterHandle final : public QSplitterHandle {
	public:
		using QSplitterHandle::QSplitterHandle;

	protected:
		void mouseDoubleClickEvent(QMouseEvent*) override {
			auto* sp = splitter();
			if (!sp || sp->count() < 2) return;
			const int lo = sp->count() - 2, hi = sp->count() - 1; // table, log
			QList<int> sizes = sp->sizes();
			const int total = sizes.value(lo) + sizes.value(hi);
			int want;
			if (sizes.value(hi) > 0) { // collapse the log, remembering its height
				sp->setProperty("restoreSize", sizes.at(hi));
				want = 0;
			} else { // restore it to its remembered height (or a sensible default)
				want = sp->property("restoreSize").toInt();
				if (want <= 0) want = 130;
				want = qBound(60, want, qMax(60, total - 120));
			}
			sizes[hi] = want;
			sizes[lo] = total - want;
			sp->setSizes(sizes);
		}

		void enterEvent(QEnterEvent*) override {
			_hot = true;
			update();
		}
		void leaveEvent(QEvent*) override {
			_hot = false;
			update();
		}

		void paintEvent(QPaintEvent*) override {
			QPainter p(this);
			p.fillRect(rect(), Pal::SURFACE);
			p.setRenderHint(QPainter::Antialiasing);
			p.setPen(Qt::NoPen);
			p.setBrush(_hot ? Pal::PRIMARY : Pal::DIVIDER);
			const qreal w = 36, h = 4;
			p.drawRoundedRect(QRectF((width() - w) / 2.0, (height() - h) / 2.0, w, h), h / 2, h / 2);
		}

	private:
		bool _hot = false;
	};

	class LogSplitter final : public QSplitter {
	public:
		using QSplitter::QSplitter;

	protected:
		QSplitterHandle* createHandle() override { return new LogSplitterHandle(orientation(), this); }
	};

} // namespace

static QString mb(qint64 n) {
	return n < 0 ? QStringLiteral("—") : QString::number(n / 1048576.0, 'f', 2) + " MB";
}
static QString RowKey(const QString& name, const QString& tag) { return name + "\t" + tag; }
static QString TargetKey(const Target& target) { return target.tag + "\t" + target.name + "\t" + target.revision; }
// A revision directory is stamped "yyyy-MM-dd_HH-mm-ss-zzz" (see Manager::Replace).
// Show it as a readable local timestamp in the dropdown while the item DATA keeps
// the raw directory name (the on-disk key). Unparseable names fall back to raw.
static QString PrettyRevision(const QString& revision) {
	const QDateTime dt = QDateTime::fromString(revision, "yyyy-MM-dd_HH-mm-ss-zzz");
	return dt.isValid() ? dt.toString("d MMM yyyy · HH:mm:ss") : revision;
}
static QString PrettyManifest(const QString& output, const QString& manifest) {
	QFile inf(QDir(output).filePath(manifest + "/csgo/steam.inf"));
	if (!inf.open(QIODevice::ReadOnly | QIODevice::Text)) return manifest;
	const QString text = QString::fromUtf8(inf.readAll());
	auto value = [&text](const QString& key) {
		return QRegularExpression(QString("(?mi)^\\s*%1\\s*=\\s*([^\\r\\n]+)").arg(QRegularExpression::escape(key))).match(text).captured(1).trimmed();
	};
	const QString patch = value("PatchVersion"), server = value("ServerVersion");
	QStringList parts{manifest};
	if (!patch.isEmpty()) parts << "Patch " + patch;
	if (!server.isEmpty()) parts << "Server " + server;
	return parts.join(QStringLiteral(" \u00b7 "));
}
// Live elapsed time for the Analyzing… / Starting… cell: m:ss (mm:ss past ten minutes).
static QString fmtElapsed(qint64 ms) {
	const qint64 s = ms / 1000;
	return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}
// How long a just-started server keeps its "Starting…" state (and a live Stop
// button) before we give up waiting for it to bind its port. A big .i64 can take
// ~1 min to load; this grace is generous so slow binds are never cut off early.
static const qint64 kStartGraceMs = 240000;

// The Refresh glyph rotated by `angle`, spun in place via the shared helper.
static QIcon SpunRefreshIcon(int angle) { return Ui::SpinningIcon("refresh", 20, Pal::ON_SURFACE_VARIANT, angle); }

// config.json always lives in the user's data folder so it survives rebuilds and
// app updates and there is exactly one place to look.
static QString userConfigHome() { return QDir::homePath() + "/.ida-workbench"; }
static QString configPath() { return QDir(userConfigHome()).filePath("config.json"); }

// Pure-UI layout preferences (splitter position, …) live in their own small file
// next to config.json, so resizing a panel never rewrites the app configuration.
static QString uiStatePath() { return QDir(userConfigHome()).filePath("ui-state.json"); }
static QJsonObject readUiState() {
	QFile f(uiStatePath());
	if (!f.open(QIODevice::ReadOnly)) return {};
	return QJsonDocument::fromJson(f.readAll()).object();
}

MainWindow::MainWindow(QWidget* parent) :
	QMainWindow(parent) {
	setWindowTitle("IDA Workbench");
	// This is the smallest layout that keeps the toolbar, table and log usable.
	setMinimumSize(1100, 920);
	resize(minimumSize());

	auto* appBar = new QFrame;
	appBar->setObjectName("appBar");
	auto* barLay = new QHBoxLayout(appBar);
	barLay->setContentsMargins(24, 18, 24, 14);
	barLay->setSpacing(16);

	// app icon (hub) in a rounded container
	auto* iconChip = new QFrame;
	iconChip->setObjectName("iconChip");
	iconChip->setFixedSize(52, 52);
	auto* icL = new QVBoxLayout(iconChip);
	icL->setContentsMargins(0, 0, 0, 0);
	// Our own app icon, rendered monochrome in the primary tone (replaces the
	// stock Material "hub" glyph).
	auto* icImg = new QLabel;
	icImg->setPixmap(Ui::TintedPixmap(":/icons/app_nobg.png", Pal::ON_PRIMARY_CONTAINER, 30));
	icImg->setFixedSize(52, 52);
	icImg->setAlignment(Qt::AlignCenter);
	icL->addWidget(icImg);

	auto* title = new QLabel("IDA Workbench");
	title->setObjectName("appTitle");

	// header stat pills (icon + text); returns the text label for later updates
	auto statPill = [](const QString& iconName, const QColor& fg, const QColor& bg, QLabel** outText) {
		auto* f = new QFrame;
		f->setStyleSheet(QString("background:%1; border-radius:9px;").arg(bg.name()));
		auto* h = new QHBoxLayout(f);
		h->setContentsMargins(9, 3, 11, 3);
		h->setSpacing(5);
		h->addWidget(Ui::IconLabel(iconName, 14, fg));
		auto* t = new QLabel;
		t->setStyleSheet(QString("color:%1; font-size:12px; font-weight:600; background:transparent;").arg(fg.name()));
		h->addWidget(t);
		*outText = t;
		return f;
	};
	auto* pServers = statPill("dns", Pal::ON_SURFACE_VARIANT, Pal::PILL_SERVER_BG, &_statServers);
	auto* pNeed = statPill("manage_search", Pal::CHIP_AMBER_FG, Pal::CHIP_AMBER_BG, &_statNeed);
	auto* pDiffer = statPill("difference", Pal::ON_TERTIARY_CONTAINER, Pal::TERTIARY_CONTAINER, &_statDiffer);

	// readiness chips (icon + text), updated in OnReadiness
	auto readyPill = [](QLabel** outIcon, QLabel** outText) {
		auto* f = new QFrame;
		f->setStyleSheet("background:transparent;");
		auto* h = new QHBoxLayout(f);
		h->setContentsMargins(0, 0, 0, 0);
		h->setSpacing(4);
		auto* ic = Ui::IconLabel("check_circle", 15, Pal::OK_GREEN, true);
		auto* t = new QLabel;
		t->setStyleSheet("font-size:12px; background:transparent;");
		h->addWidget(ic);
		h->addWidget(t);
		*outIcon = ic;
		*outText = t;
		return f;
	};
	auto* pPython = readyPill(&_chipPythonIc, &_chipPython);
	auto* pIda = readyPill(&_chipIdaIc, &_chipIda);
	auto* pMcp = readyPill(&_chipMcpIc, &_chipMcp);

	auto* chipsRow = new QHBoxLayout;
	chipsRow->setSpacing(8);
	chipsRow->addWidget(pServers);
	chipsRow->addWidget(pNeed);
	chipsRow->addWidget(pDiffer);
	chipsRow->addSpacing(8);
	chipsRow->addWidget(pPython);
	chipsRow->addWidget(pIda);
	chipsRow->addWidget(pMcp);
	chipsRow->addStretch();

	auto* titleBox = new QVBoxLayout;
	titleBox->setSpacing(6);
	titleBox->addWidget(title);
	titleBox->addLayout(chipsRow);

	// segmented view switcher (top-right): List | Tabs | Settings
	_listBtn = Ui::IconButton("list", "List", "text");
	_tabsBtn = Ui::IconButton("tab", "Tabs", "text");
	_settingsBtn = Ui::IconButton("settings", "Settings", "text");
	auto* seg = Ui::SegmentedControl({_listBtn, _tabsBtn, _settingsBtn}, 286);
	barLay->addWidget(iconChip);
	barLay->addLayout(titleBox);
	barLay->addStretch();
	barLay->addWidget(seg);

	_startBtn = Ui::IconButton("play_arrow", "MCP Start", nullptr, true); // filled primary
	_openIdaBtn = Ui::IconButton("open_in_new", "Open IDA", "tonal");
	_stopBtn = Ui::IconButton("stop", "Stop", "tonal", true);
	_analyzeBtn = Ui::IconButton("autorenew", "Analyze", "tonal");
	_replaceBtn = Ui::IconButton("swap_horiz", "Replace", "tonal");
	_depotBtn = Ui::IconButton("download", "Update Depot…", "tonal");
	_startBtn->setObjectName("startButton");
	_openIdaBtn->setObjectName("openIdaBtn");
	_analyzeBtn->setObjectName("analyzeBtn");
	_replaceBtn->setObjectName("replaceBtn");
	_depotBtn->setObjectName("depotBtn");
	_stopBtn->setObjectName("stopBtn");
	// Icon tints matched to each button's text colour (see material3.qss).
	_startBtn->setIcon(Ui::Icon("play_arrow", Pal::START_FG, 18, true));
	_openIdaBtn->setIcon(Ui::Icon("open_in_new", Pal::PRIMARY, 18));
	_analyzeBtn->setIcon(Ui::Icon("autorenew", Pal::ANALYZE_FG, 18));
	_replaceBtn->setIcon(Ui::Icon("swap_horiz", Pal::REPLACE_FG, 18));
	_depotBtn->setIcon(Ui::Icon("download", Pal::PRIMARY, 18));
	_stopBtn->setIcon(Ui::Icon("stop", Pal::STOP_FG, 18, true));
	for (QPushButton* button : {_openIdaBtn, _stopBtn, _analyzeBtn, _replaceBtn, _depotBtn})
		button->setProperty("actionPill", true);
	// Action tooltips are selection-dependent and owned by UpdateActionButtons().
	_startBtn->setAutoDefault(false);
	_startBtn->setDefault(false);
	_refreshBtn = Ui::RoundIconButton("refresh", "Refresh server status", Pal::PRIMARY, Pal::PRIMARY);
	_addBinBtn = Ui::RoundIconButton("note_add", "Add binary… (quick-add a standalone file)", Pal::PRIMARY, Pal::PRIMARY);
	_deleteBtn = Ui::RoundIconButton("delete", "Delete from workbench (files on disk are kept)", Pal::ERROR_COLOR, Pal::ERROR_COLOR);
	_deleteBtn->setEnabled(false); // enabled only when a stopped row is selected (UpdateActionButtons)
	// "MCP data" is connection info, not a table action — it sits apart on the
	// right, the way database dashboards place their Connect button.
	_mcpBtn = Ui::IconButton("dns", "MCP data", "outlined");

	auto* actions = new QHBoxLayout;
	actions->setSpacing(8);
	actions->addWidget(_startBtn);
	actions->addWidget(_openIdaBtn);
	actions->addWidget(_analyzeBtn);
	actions->addWidget(_replaceBtn);
	actions->addWidget(_depotBtn);
	actions->addWidget(_stopBtn);
	actions->addSpacing(8);
	actions->addWidget(Ui::VerticalDivider(24));
	actions->addSpacing(8);
	actions->addWidget(_refreshBtn);
	actions->addWidget(_addBinBtn);
	actions->addWidget(_deleteBtn);
	actions->addStretch();
	actions->addWidget(_mcpBtn);

	_table = new OverlayTableWidget(0, COL_COUNT);
	auto* selectHeader = new SelectHeader(Qt::Horizontal, _table);
	selectHeader->setToggleHandler([this] { ToggleAll(); });
	_table->setHorizontalHeader(selectHeader);
	_table->setHorizontalHeaderLabels({"", "Module", "Tag", "Server", "Port", "Status", "Size", "Sync", ""});
	_table->horizontalHeaderItem(COL_SEL)->setToolTip("Select all visible modules");
	_table->setProperty("hoveredRow", -1);
	_table->setProperty("spinRow", -1);
	_table->verticalHeader()->setVisible(false);
	_table->horizontalHeader()->setFixedHeight(48);
	Ui::OverlayVerticalScrollBar(_table, 50);
	_table->verticalHeader()->setDefaultSectionSize(46);
	_table->setShowGrid(false);
	_table->setSelectionMode(QAbstractItemView::NoSelection);
	_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	_table->setFocusPolicy(Qt::NoFocus);
	_table->setContextMenuPolicy(Qt::CustomContextMenu);
	_table->setIconSize(QSize(18, 18));
	_table->setItemDelegate(new RowDelegate(_table));
	_table->setItemDelegateForColumn(COL_SEL, new CheckDelegate(this));
	_table->setItemDelegateForColumn(COL_TAG, new TagDelegate(this));
	_table->setItemDelegateForColumn(COL_SERVER, new ChipDelegate(this));
	_table->setItemDelegateForColumn(COL_STATUS, new ChipDelegate(this));
	_table->setItemDelegateForColumn(COL_DELTA, new ChipDelegate(this));
	_table->horizontalHeaderItem(COL_MODULE)->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	_table->horizontalHeaderItem(COL_TAG)->setTextAlignment(Qt::AlignCenter);
	_table->horizontalHeaderItem(COL_SERVER)->setTextAlignment(Qt::AlignCenter);
	_table->horizontalHeaderItem(COL_PORT)->setTextAlignment(Qt::AlignCenter);
	_table->horizontalHeaderItem(COL_STATUS)->setTextAlignment(Qt::AlignCenter);
	_table->horizontalHeaderItem(COL_SIZE)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
	_table->horizontalHeaderItem(COL_DELTA)->setTextAlignment(Qt::AlignCenter);
	connect(_table, &QTableWidget::cellClicked, this, [this](int row, int col) {
		if (col == COL_SEL || col == COL_PORT) return; // Port: double-click to edit
		auto* it = _table->item(row, COL_SEL);
		if (it) {
			it->setCheckState(it->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
			UpdateRowSelection(row);
			UpdateActionButtons();
		}
	});
	connect(_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int col) {
		if (col == COL_PORT) EditPort(row);
	});
	connect(_table, &QWidget::customContextMenuRequested, this, &MainWindow::ShowLibraryContextMenu);
	// Custom sorting: no arrow indicator; a header click cycles that column
	// off -> ascending -> descending, and several columns can be combined.
	_table->setSortingEnabled(false);
	_table->horizontalHeader()->setSortIndicatorShown(false);
	_table->horizontalHeader()->setSectionsClickable(true);
	connect(_table->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int column) {
		if (column == COL_SEL)
			ToggleAll();
		else
			CycleSort(column);
	});
	connect(_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
		if (item && item->column() == COL_SEL) {
			UpdateRowSelection(item->row());
			UpdateActionButtons();
		}
	});

	// log wrapped in a labelled card
	_log = new QPlainTextEdit;
	_log->setReadOnly(true);
	_log->setMaximumBlockCount(2000);
	_log->setPlaceholderText("log output…");
	auto* logCard = new QFrame;
	logCard->setObjectName("logCard");
	logCard->setMinimumHeight(70); // a drag keeps the log usable; a double-click can still hide it
	auto* logLay = new QVBoxLayout(logCard);
	logLay->setContentsMargins(16, 10, 16, 12);
	logLay->setSpacing(6);
	auto* logHead = new QHBoxLayout;
	logHead->setSpacing(6);
	logHead->addWidget(Ui::IconLabel("terminal", 16, Pal::PRIMARY));
	auto* logTitle = new QLabel("LOG");
	logTitle->setObjectName("logTitle");
	logHead->addWidget(logTitle);
	logHead->addStretch();
	// Jump straight to the on-disk log (the panel is a live tail of the same file).
	auto* openLogBtn = Ui::RoundIconButton("folder_open", "Open the log folder", Pal::ON_SURFACE_VARIANT, Pal::ON_SURFACE_VARIANT, 28, 16);
	connect(openLogBtn, &QPushButton::clicked, this, [] {
		const QString p = Log::Path();
		if (!p.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(p).absolutePath()));
	});
	logHead->addWidget(openLogBtn);
	logLay->addLayout(logHead);
	logLay->addWidget(_log);

	_readyMsg = Ui::Hint(QString()); // "not ready" hint, shown only when needed
	_readyMsg->setVisible(false);

	_tagCombo = Ui::ComboBox();
	_tagCombo->setIconSize(QSize(18, 18));
	_tagCombo->setToolTip("Workspace shown in the table");
	auto* tagsField = Ui::ComboField("Workspace", _tagCombo);
	tagsField->setFixedWidth(250);
	auto* versionCombo = Ui::ComboBox();
	_versionCombo = versionCombo;
	_versionCombo->setIconSize(QSize(18, 18));
	_versionCombo->setToolTip("Current or stored version opened by the active tag");
	auto* versionField = Ui::ComboField("Revision", versionCombo);
	_versionField = versionField;
	versionField->setFixedWidth(250);
	_tabsRow = new QWidget;
	auto* tabsLayout = new QHBoxLayout(_tabsRow);
	tabsLayout->setContentsMargins(0, 0, 0, 0);
	tabsLayout->setSpacing(10);
	tabsLayout->addWidget(tagsField);
	tabsLayout->addStretch();
	tabsLayout->addWidget(versionField);
	_tabsRow->setVisible(false);

	_mainPage = new QWidget;
	_mainPage->setObjectName("appSurface");
	auto* content = new QVBoxLayout(_mainPage);
	content->setContentsMargins(24, 6, 24, 20);
	content->setSpacing(14);
	// The table and the log share a draggable divider (like the VSCode terminal):
	// drag the handle to resize the log, double-click it to hide / show the log.
	_logSplitter = new LogSplitter(Qt::Vertical);
	_logSplitter->setObjectName("logSplitter");
	_logSplitter->setHandleWidth(11);
	_logSplitter->addWidget(_table);
	_logSplitter->addWidget(logCard);
	_logSplitter->setStretchFactor(0, 1); // the table takes the extra space on resize
	_logSplitter->setStretchFactor(1, 0); // the log keeps its dragged height
	_logSplitter->setCollapsible(0, false); // the table never fully collapses
	_logSplitter->setCollapsible(1, true);	// the log can be double-clicked shut

	content->addWidget(_readyMsg);
	content->addLayout(actions);
	content->addWidget(_tabsRow);
	content->addWidget(_logSplitter, 1);

	// Restore the last dragged layout (falls back to a comfortable default height).
	const QByteArray splitterState = QByteArray::fromBase64(readUiState().value("logSplitter").toString().toLatin1());
	if (!splitterState.isEmpty())
		_logSplitter->restoreState(splitterState);
	else
		_logSplitter->setSizes({640, 130});

	_stack = new QStackedWidget;
	_stack->addWidget(_mainPage); // page 0 = main (list/tabs)

	// Thin footer pinned to the bottom: the CMake-recorded version + author credit.
	auto* footer = new QWidget;
	footer->setObjectName("appFooter");
	auto* footerLay = new QHBoxLayout(footer);
	footerLay->setContentsMargins(24, 5, 24, 6);
	footerLay->setSpacing(0);
	auto* credit = new QLabel("Created by <a href=\"https://t.me/sazonische\">@sazonische</a>");
	credit->setObjectName("footerText");
	credit->setTextFormat(Qt::RichText);
	credit->setOpenExternalLinks(true);
	credit->setTextInteractionFlags(Qt::TextBrowserInteraction);
	auto* version = new QLabel(QString("IDA Workbench v%1").arg(APP_VERSION));
	version->setObjectName("footerText");
	footerLay->addWidget(credit);
	footerLay->addStretch();
	footerLay->addWidget(version);

	auto* central = new QWidget;
	central->setObjectName("appSurface");
	auto* root = new QVBoxLayout(central);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);
	root->addWidget(appBar);
	root->addWidget(_stack, 1);
	root->addWidget(footer);
	setCentralWidget(central);

	_mgr = new Manager;
	QString err;
	const QString cfg = configPath();
	// First run (or the file was deleted): write a clean, loadable default so the
	// app opens ready to edit instead of failing. IDA is auto-detected if present.
	if (!QFile::exists(cfg)) {
		QString createErr;
		if (_mgr->CreateDefaultConfig(cfg, &createErr))
			OnLog("created a clean config: " + QDir::toNativeSeparators(cfg));
		else
			OnLog("could not create a clean config: " + createErr);
	}
	if (!_mgr->LoadConfig(cfg, &err))
		QMessageBox::critical(this, "Config error", QString("Failed to load %1\n\n%2\n\nOpen Settings to fix the paths.").arg(cfg, err));
	else
		OnLog("config: " + QDir::toNativeSeparators(cfg));
	_cfg = _mgr->View();
	RecomputeModel();
	BuildTable();
	// Prime the header synchronously so nothing pops in / shifts after the first Refresh.
	_statServers->setText("0 servers up");
	_statNeed->setText("0 need re-Analyze");
	_statDiffer->setText("0 differ from source");
	OnReadiness(_mgr->CurrentReadiness());
	SetView(1);

	_thread = new QThread(this);
	_mgr->moveToThread(_thread);
	connect(_thread, &QThread::finished, _mgr, &QObject::deleteLater);

	connect(_mgr, &Manager::Log, this, &MainWindow::OnLog);
	connect(_mgr, &Manager::StatusReady, this, &MainWindow::OnStatus);
	connect(_mgr, &Manager::ConfigLoaded, this, &MainWindow::OnConfigLoaded);
	connect(_mgr, &Manager::ConfigSaveFinished, this, [this](bool ok, const QString& message) {
		OnBusy(false);
		if (ok) {
			if (_settingsPanel) CloseSettings();
			// else: an in-table port edit — ConfigLoaded already rebuilt the table.
		} else {
			QMessageBox::warning(this, "Could not save", message);
			if (_settingsPanel)
				_settingsPanel->setEnabled(true);
			else
				QueueRefresh(); // revert the edited port cell to its stored value
		}
	});
	connect(_mgr, &Manager::ReadinessChanged, this, &MainWindow::OnReadiness);
	connect(_mgr, &Manager::AnalyzeStarted, this, &MainWindow::OnAnalyzeStarted);
	connect(_mgr, &Manager::AnalyzeFinished, this, &MainWindow::OnAnalyzeFinished);
	connect(_mgr, &Manager::OperationChanged, this, &MainWindow::OnOperationChanged);
	connect(_mgr, &Manager::WorkspaceOperationChanged, this, &MainWindow::OnWorkspaceOperationChanged);

	// Rotates the Analyzing… chip icon while idat runs on the worker thread. Several
	// libraries can analyze at once, so repaint the whole viewport to spin them all.
	_spinTimer = new QTimer(this);
	_spinTimer->setInterval(60);
	connect(_spinTimer, &QTimer::timeout, this, [this]() {
		_spinAngle = (_spinAngle + 30) % 360;
		_table->setProperty("spinAngle", _spinAngle);
		// Once per second, refresh the live "Analyzing… m:ss" / "Starting… m:ss"
		// elapsed counter in each active cell (a real progress signal; headless idat
		// and a booting server expose no %).
		qint64 anyElapsed = -1;
		if (!_analyzeClocks.isEmpty())
			anyElapsed = _analyzeClocks.constBegin()->elapsed();
		else if (!_startingClocks.isEmpty())
			anyElapsed = _startingClocks.constBegin()->elapsed();
		if (anyElapsed >= 0 && int(anyElapsed / 1000) != _spinSec) {
			_spinSec = int(anyElapsed / 1000);
			auto paintRows = [this](const QHash<QString, QElapsedTimer>& clocks, const QString& prefix) {
				for (auto it = clocks.constBegin(); it != clocks.constEnd(); ++it) {
					const int row = _rowOf.value(it.key(), -1);
					if (row < 0) continue;
					if (auto* cell = _table->item(row, COL_STATUS))
						cell->setText(prefix + fmtElapsed(it.value().elapsed()));
				}
			};
			for (auto it = _analyzeClocks.constBegin(); it != _analyzeClocks.constEnd(); ++it) {
				const QStringList parts = it.key().split('\t');
				if (parts.size() < 3 || ActiveRevision(parts[0]) != parts[2]) continue;
				const int row = _rowOf.value(RowKey(parts[1], parts[0]), -1);
				if (row >= 0) _table->item(row, COL_STATUS)->setText("Analyzing… " + fmtElapsed(it.value().elapsed()));
			}
			paintRows(_startingClocks, "Starting… ");
		}
		_table->viewport()->update();
	});

	// Spins the Refresh button icon while a manual Refresh is in flight.
	_refreshSpinTimer = new QTimer(this);
	_refreshSpinTimer->setInterval(60);
	connect(_refreshSpinTimer, &QTimer::timeout, this, [this]() {
		_refreshAngle = (_refreshAngle + 30) % 360;
		_refreshBtn->setIcon(SpunRefreshIcon(_refreshAngle));
	});

	connect(this, &MainWindow::RequestRefresh, _mgr, &Manager::Refresh);
	connect(this, &MainWindow::RequestOpenIda, _mgr, &Manager::OpenIda);
	connect(this, &MainWindow::RequestStart, _mgr, &Manager::Start);
	connect(this, &MainWindow::RequestStop, _mgr, &Manager::Stop);
	connect(this, &MainWindow::RequestAnalyze, _mgr, &Manager::Analyze);
	connect(this, &MainWindow::RequestStopOperations, _mgr, &Manager::StopOperations);
	connect(this, &MainWindow::RequestReplace, _mgr, &Manager::Replace);
	connect(this, &MainWindow::RequestDepotUpdate, _mgr, &Manager::UpdateDepot);
	connect(this, &MainWindow::RequestSaveConfig, _mgr, &Manager::SaveConfig);
	connect(this, &MainWindow::RequestImportConfig, _mgr, &Manager::ImportConfig);
	connect(this, &MainWindow::RequestStoredVersion, _mgr, &Manager::SetStoredVersion);

	connect(_refreshBtn, &QPushButton::clicked, this, [this]() {
		if (_busy || _refreshPending) return; // mirror QueueRefresh's guard
		_refreshAngle = 0;
		_refreshSpinTimer->start(); // stopped again in OnStatus()
		QueueRefresh();
	});
	connect(_startBtn, &QPushButton::clicked, this, &MainWindow::DoStart);
	connect(_openIdaBtn, &QPushButton::clicked, this, &MainWindow::DoOpenIda);
	connect(_stopBtn, &QPushButton::clicked, this, &MainWindow::DoStop);
	connect(_analyzeBtn, &QPushButton::clicked, this, &MainWindow::DoAnalyze);
	connect(_replaceBtn, &QPushButton::clicked, this, &MainWindow::DoReplace);
	connect(_depotBtn, &QPushButton::clicked, this, &MainWindow::DoDepotUpdate);
	connect(_addBinBtn, &QPushButton::clicked, this, &MainWindow::AddBinary);
	connect(_mcpBtn, &QPushButton::clicked, this, &MainWindow::ShowMcpData);
	connect(_deleteBtn, &QPushButton::clicked, this, &MainWindow::DoDelete);
	connect(_settingsBtn, &QPushButton::clicked, this, &MainWindow::OpenSettings);
	connect(_listBtn, &QPushButton::clicked, this, [this] { SetView(0); });
	connect(_tabsBtn, &QPushButton::clicked, this, [this] { SetView(1); });
	connect(_tagCombo, &QComboBox::currentIndexChanged, this, [this](int) { ApplyFilter(); });
	connect(_versionCombo, &QComboBox::currentIndexChanged, this, [this](int) {
		if (_view != 1 || _versionCombo->signalsBlocked() || !_tagCombo->count()) return;
		const QString tag = _tagCombo->currentData().toString();
		const QString version = _versionCombo->currentData().toString();
		bool serverActive = false;
		for (auto it = _rowState.constBegin(); it != _rowState.constEnd(); ++it) {
			const QStringList parts = it.key().split('\t');
			if (parts.value(1) == tag && (it->up || it->starting)) { serverActive = true; break; }
		}
		if (serverActive && version != _versionByTag.value(tag)) {
			OnLog(QString("[skip] %1: stop its MCP servers before changing revision").arg(tag));
			QSignalBlocker blocker(_versionCombo);
			const int previous = _versionCombo->findData(_versionByTag.value(tag));
			_versionCombo->setCurrentIndex(previous >= 0 ? previous : 0);
			return;
		}
		_versionByTag[tag] = version;
		emit RequestStoredVersion(tag, version);
		UpdateActionButtons();
	});

	_thread->start();
	QueueRefresh();

	_refreshTimer = new QTimer(this);
	connect(_refreshTimer, &QTimer::timeout, this, &MainWindow::QueueRefresh);
	_refreshTimer->start(15000);
	auto* logCapTimer = new QTimer(this);
	logCapTimer->setInterval(5000);
	connect(logCapTimer, &QTimer::timeout, this, [] { Log::EnforceCap(); });
	logCapTimer->start();

	SetupTray();
}

MainWindow::~MainWindow() {
	if (_trayIcon) _trayIcon->hide();
	_mgr->RequestStopDepot();
	QVector<Target> analyses;
	for (auto it = _activeOperations.constBegin(); it != _activeOperations.constEnd(); ++it) {
		if (it.value() != "analyze") continue;
		const QStringList parts = it.key().split('\t');
		analyses.push_back({parts.value(0), parts.value(1), parts.value(2)});
	}
	if (!analyses.isEmpty() && _thread->isRunning())
		QMetaObject::invokeMethod(_mgr, [this, analyses] { _mgr->StopOperations(analyses); }, Qt::BlockingQueuedConnection);
	_thread->quit();
	// An unbounded wait() here is what leaves the process in Task Manager after Exit: a
	// worker inside a blocking call (a Stop grace window, DepotDownloader, a network
	// fetch) does not see quit() until it returns. Cut it short instead — every
	// persistent write goes through QSaveFile, so no half-written config can survive
	// this, and a zombie is the worse outcome: the single-instance guard would make the
	// next launch silently activate the dead process instead of starting a new one.
	// Not QThread::terminate(): the worker could hold the log mutex, and then the very
	// next log line (including Qt's own during teardown) would deadlock us here.
	if (!_thread->wait(15000)) std::_Exit(0);
}

void MainWindow::closeEvent(QCloseEvent* event) {
	bool sessionEnding = false;
#if QT_CONFIG(sessionmanager)
	sessionEnding = qGuiApp->isSavingSession();
#endif
	if (!_quitting && !sessionEnding && _trayIcon && _trayIcon->isVisible()) {
		SaveUiState();
		hide();
		event->ignore();
		return;
	}
	if (_busy && !_quitting && !sessionEnding) {
		QMessageBox::information(this, "Saving configuration", "Wait for configuration saving to finish before closing the manager.");
		event->ignore();
		return;
	}
	SaveUiState();
	QMainWindow::closeEvent(event);
}

void MainWindow::SetupTray() {
	if (!QSystemTrayIcon::isSystemTrayAvailable()) return;

	_trayIcon = new QSystemTrayIcon(windowIcon(), this);
	_trayIcon->setToolTip(QString("IDA Workbench %1").arg(QCoreApplication::applicationVersion()));

	auto* menu = Ui::Menu(this);
	auto* open = menu->addAction(windowIcon(), "Open IDA Workbench");
	menu->addSeparator();
	auto* exit = menu->addAction(Ui::Icon("logout", Pal::ERROR_COLOR, 18), "Exit");
	open->setIconVisibleInMenu(true);
	exit->setIconVisibleInMenu(true);
	connect(open, &QAction::triggered, this, &MainWindow::ShowFromTray);
	connect(exit, &QAction::triggered, this, &MainWindow::ExitFromTray);
	connect(_trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
		if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
			ShowFromTray();
	});
	_trayIcon->setContextMenu(menu);
	_trayIcon->show();
}

void MainWindow::ShowFromTray() {
	showNormal();
	raise();
	activateWindow();
}

void MainWindow::ExitFromTray() {
	if (_busy) {
		ShowFromTray();
		if (!_activeDepotTags.isEmpty())
			QMessageBox::information(this, "Depot update in progress",
				"A depot update is running. Press Stop to cancel it, then exit.");
		else
			QMessageBox::information(this, "Saving configuration", "Wait for configuration saving to finish before exiting.");
		return;
	}
	_quitting = true;
	if (_trayIcon) _trayIcon->hide();
	close();
	// Do not lean on quitOnLastWindowClosed: one stray visible dialog would leave the
	// process running with no window left to close it from.
	QCoreApplication::quit();
}

// Persist the log-splitter layout so the log keeps its dragged height next launch.
void MainWindow::SaveUiState() {
	if (!_logSplitter) return;
	QJsonObject o = readUiState(); // preserve any keys added later
	o["logSplitter"] = QString::fromLatin1(_logSplitter->saveState().toBase64());
	QSaveFile f(uiStatePath());
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
		f.commit();
	}
}

void MainWindow::RecomputeModel() {
	_tags.clear();
	_names.clear();
	for (const Workspace& d : AllWorkspaces(_cfg))
		if (!_tags.contains(d.tag)) _tags << d.tag;
	for (const ExtraLib& e : _cfg.extraLibs)
		if (!_tags.contains(e.tag)) _tags << e.tag;
	for (const Workspace& d : AllWorkspaces(_cfg))
		for (const QString& file : d.files) {
			const QString n = QFileInfo(file).completeBaseName();
			if (!n.isEmpty() && !_names.contains(n)) _names << n;
		}
	for (const ExtraLib& e : _cfg.extraLibs) {
		const QString n = QFileInfo(e.path).completeBaseName();
		if (!_names.contains(n)) _names << n;
	}
}

QColor MainWindow::TagColor(const QString& tag) const {
	for (const Workspace& sd : AllWorkspaces(_cfg))
		if (sd.tag == tag && !sd.color.trimmed().isEmpty()) return QColor(sd.color.trimmed());
	for (const ExtraLib& e : _cfg.extraLibs)
		if (e.tag == tag && !e.color.trimmed().isEmpty()) return QColor(e.color.trimmed());
	return Pal::AutoTagBg(tag); // stable colour derived from the tag name
}

bool MainWindow::InstanceExists(const QString& name, const QString& tag) const {
	for (const Workspace& d : AllWorkspaces(_cfg))
		if (d.tag == tag)
			for (const QString& file : d.files)
				if (QFileInfo(file).completeBaseName() == name) return true;
	for (const ExtraLib& e : _cfg.extraLibs)
		if (e.tag == tag && QFileInfo(e.path).completeBaseName() == name) return true;
	return false;
}

void MainWindow::BuildTable() {
	// One row per (name, tag) instance, grouped by name so tags sit together.
	_rowOf.clear();
	_table->setRowCount(0); // sorting is handled manually by ApplySort(), never by Qt
	int row = 0;
	for (const QString& name : _names) {
		for (const QString& tag : _tags) {
			if (!InstanceExists(name, tag)) continue;
			_table->insertRow(row);
			auto* sel = new QTableWidgetItem;
			sel->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
			sel->setTextAlignment(Qt::AlignCenter);
			sel->setCheckState(Qt::Unchecked);
			_table->setItem(row, COL_SEL, sel);
			auto* mod = new QTableWidgetItem(name);
			mod->setIcon(Ui::Icon("deployed_code", Pal::ICON_MUTED, 18));
			mod->setToolTip("Binary: " + name);
			mod->setData(Qt::UserRole, row); // immutable insertion index (sort tiebreak / "off" order)
			_table->setItem(row, COL_MODULE, mod);
			auto* tagIt = new QTableWidgetItem(tag);
			tagIt->setData(Qt::UserRole, TagColor(tag)); // per-tag pill colour
			_table->setItem(row, COL_TAG, tagIt);
			_table->setItem(row, COL_SERVER, new QTableWidgetItem("—"));
			auto* portIt = new NumericItem;
			portIt->setText("—");
			portIt->setTextAlignment(Qt::AlignCenter);
			portIt->setToolTip("Double-click to change this instance's MCP port");
			_table->setItem(row, COL_PORT, portIt);
			_table->setItem(row, COL_STATUS, new QTableWidgetItem("—"));
			auto* sz = new NumericItem;
			sz->setText("—");
			sz->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
			_table->setItem(row, COL_SIZE, sz);
			auto* sync = new QTableWidgetItem("—");
			sync->setTextAlignment(Qt::AlignCenter);
			sync->setForeground(Pal::ICON_MUTED);
			_table->setItem(row, COL_DELTA, sync);
			auto* ord = new NumericItem; // hidden scratch column for ApplySort()'s rank
			ord->setData(Qt::UserRole, row);
			_table->setItem(row, COL_ORDER, ord);
			_rowOf.insert(RowKey(name, tag), row);
			++row;
		}
	}

	auto* hh = _table->horizontalHeader();
	hh->setSectionResizeMode(COL_SEL, QHeaderView::Fixed);
	hh->setSectionResizeMode(COL_MODULE, QHeaderView::Stretch);
	hh->setSectionResizeMode(COL_TAG, QHeaderView::Fixed);
	hh->setSectionResizeMode(COL_SERVER, QHeaderView::Fixed);
	hh->setSectionResizeMode(COL_PORT, QHeaderView::Fixed);
	hh->setSectionResizeMode(COL_STATUS, QHeaderView::Fixed);
	hh->setSectionResizeMode(COL_SIZE, QHeaderView::Fixed);
	hh->setSectionResizeMode(COL_DELTA, QHeaderView::Fixed);
	_table->setColumnWidth(COL_SEL, 56);
	_table->setColumnWidth(COL_TAG, 110);
	_table->setColumnWidth(COL_SERVER, 100);
	_table->setColumnWidth(COL_PORT, 74);
	_table->setColumnWidth(COL_STATUS, 160);
	_table->setColumnWidth(COL_SIZE, 100);
	_table->setColumnWidth(COL_DELTA, 118); // 104 px content + 14 px overlay scrollbar gutter
	// "Sync" compares each tag's analyzed copy against its own Live source (the same
	// content check that drives "Update available"): in sync / changed / source missing.
	if (auto* dh = _table->horizontalHeaderItem(COL_DELTA))
		dh->setText("Sync");

	// Rebuild the bounded workspace dropdown without losing the active tag.
	const QString activeTag = _tagCombo->currentData().toString();
	_tagCombo->blockSignals(true);
	_tagCombo->clear();
	for (const QString& tag : _tags)
		_tagCombo->addItem(Ui::SwatchIcon(TagColor(tag), 16), tag, tag);
	const int activeIndex = _tagCombo->findData(activeTag);
	_tagCombo->setCurrentIndex(activeIndex >= 0 ? activeIndex : 0);
	_tagCombo->blockSignals(false);
	_table->setColumnHidden(COL_TAG, _view == 1);
	_table->setColumnHidden(COL_ORDER, true); // internal sort-rank scratch column
	_rowState.clear();						  // refilled by the next Refresh (keyed by name/tag)
	ApplySort();							  // re-apply the active column sort to the new rows
	RebuildRowOf();							  // capture positions after the reorder
	UpdateSortHeaders();
	ApplyFilter();
	_table->scrollToTop();
}

// Rebuild the (name,tag) -> visual-row map. Called after a sort (or table rebuild)
// reorders rows so status updates and port edits still target the right row.
void MainWindow::RebuildRowOf() {
	_rowOf.clear();
	for (int r = 0; r < _table->rowCount(); ++r) {
		auto* mod = _table->item(r, COL_MODULE);
		auto* tag = _table->item(r, COL_TAG);
		if (mod && tag) _rowOf.insert(RowKey(mod->text(), tag->text()), r);
	}
}

// Header click: cycle one column through off -> ascending -> descending -> off.
// Columns already active stay active, so several can be combined into one sort;
// removing the last one restores the original (insertion) order.
void MainWindow::CycleSort(int column) {
	if (column == COL_SEL || column == COL_ORDER) return; // checkbox / hidden scratch aren't sortable
	int at = -1;
	for (int i = 0; i < _sortKeys.size(); ++i)
		if (_sortKeys[i].first == column) {
			at = i;
			break;
		}
	if (at < 0)
		_sortKeys.append({column, Qt::AscendingOrder}); // new key: least-significant (first click stays primary)
	else if (_sortKeys[at].second == Qt::AscendingOrder)
		_sortKeys[at].second = Qt::DescendingOrder;
	else
		_sortKeys.remove(at); // third click drops this column from the sort
	ApplySort();
	RebuildRowOf();
	UpdateSortHeaders();
	ApplyFilter();
	UpdateActionButtons();
}

// Reorder the rows by the active sort keys (earlier keys win; ties fall back to
// insertion order). The multi-key comparison is done here, then the resulting
// rank is written into the hidden COL_ORDER column and sortItems does the physical
// move so whole rows (checkstate, colours, tooltips) travel together.
void MainWindow::ApplySort() {
	const int n = _table->rowCount();
	if (n == 0) return;
	auto insIdx = [this](int row) {
		const auto* it = _table->item(row, COL_MODULE);
		return it ? it->data(Qt::UserRole).toInt() : row;
	};
	auto cellCmp = [this](int a, int b, int col) -> int {
		const auto* ia = _table->item(a, col);
		const auto* ib = _table->item(b, col);
		if (!ia || !ib) return 0;
		if (col == COL_PORT || col == COL_SIZE) { // numeric columns sort by their UserRole key
			const qlonglong va = ia->data(Qt::UserRole).toLongLong();
			const qlonglong vb = ib->data(Qt::UserRole).toLongLong();
			return va < vb ? -1 : (va > vb ? 1 : 0);
		}
		return QString::compare(ia->text(), ib->text(), Qt::CaseInsensitive);
	};
	QVector<int> order(n);
	std::iota(order.begin(), order.end(), 0);
	std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
		for (const auto& k : _sortKeys) {
			const int c = cellCmp(a, b, k.first);
			if (c != 0) return k.second == Qt::AscendingOrder ? c < 0 : c > 0;
		}
		return insIdx(a) < insIdx(b); // stable fallback: original insertion order
	});
	for (int rank = 0; rank < n; ++rank)
		_table->item(order[rank], COL_ORDER)->setData(Qt::UserRole, rank);
	_table->sortItems(COL_ORDER, Qt::AscendingOrder);
}

// No sort arrow: the header text tone alone signals each column's state.
void MainWindow::UpdateSortHeaders() {
	for (int col = 0; col < _table->columnCount(); ++col) {
		auto* h = _table->horizontalHeaderItem(col);
		if (!h) continue;
		QColor tone = Pal::SORT_INACTIVE;
		for (const auto& k : _sortKeys)
			if (k.first == col) {
				tone = (k.second == Qt::AscendingOrder) ? Pal::SORT_ASC : Pal::SORT_DESC;
				break;
			}
		h->setForeground(tone);
	}
}

void MainWindow::UpdateRowSelection(int row) {
	if (row < 0 || row >= _table->rowCount()) return;
	const auto* selector = _table->item(row, COL_SEL);
	const QColor background = selector && selector->checkState() == Qt::Checked ? Pal::ROW_SELECTED : QColor(Qt::transparent);
	for (int col = 0; col < _table->columnCount(); ++col)
		if (auto* item = _table->item(row, col)) item->setBackground(background);
}

void MainWindow::ShowLibraryContextMenu(const QPoint& position) {
	const QModelIndex index = _table->indexAt(position);
	if (!index.isValid()) return;

	const bool clickedAlreadySelected = _table->item(index.row(), COL_SEL)->checkState() == Qt::Checked;
	if (!clickedAlreadySelected) {
		QSignalBlocker blocker(_table);
		for (int row = 0; row < _table->rowCount(); ++row) {
			if (auto* selector = _table->item(row, COL_SEL)) selector->setCheckState(row == index.row() ? Qt::Checked : Qt::Unchecked);
			UpdateRowSelection(row);
		}
	}
	UpdateActionButtons();

	QMenu* menu = Ui::Menu(this);
	menu->setToolTipsVisible(true); // a greyed command must still be able to say why
	auto addCommand = [menu](const QString& icon, const QString& text, QPushButton* source, auto handler) {
		QAction* action = menu->addAction(Ui::Icon(icon, source->isEnabled() ? Pal::PRIMARY : Pal::ICON_MUTED, 18), text);
		action->setEnabled(source->isEnabled());
		action->setToolTip(source->toolTip());
		QObject::connect(action, &QAction::triggered, menu, handler);
	};
	addCommand("play_arrow", "MCP Start", _startBtn, [this] { DoStart(); });
	addCommand("open_in_new", "Open IDA", _openIdaBtn, [this] { DoOpenIda(); });
	addCommand("autorenew", "Analyze", _analyzeBtn, [this] { DoAnalyze(); });
	addCommand("swap_horiz", "Replace", _replaceBtn, [this] { DoReplace(); });
	addCommand("download", "Depot update", _depotBtn, [this] { DoDepotUpdate(); });
	addCommand("stop", _stopBtn->text(), _stopBtn, [this] { DoStop(); });
	menu->addSeparator();
	addCommand("delete", "Delete from workbench", _deleteBtn, [this] { DoDelete(); });
	menu->exec(_table->viewport()->mapToGlobal(position));
	menu->deleteLater();
}

void MainWindow::QueueRefresh() {
	if (_busy || _refreshPending) return;
	_refreshPending = true;
	emit RequestRefresh();
}

int MainWindow::RefreshIntervalMs() const {
	// Brisk while servers are up or one is still booting (catch the bind quickly),
	// relaxed when idle.
	return (_anyServerUp || !_startingClocks.isEmpty()) ? 4000 : 15000;
}

// One 60 ms timer both spins the arrow and ticks the elapsed counters; run it iff
// something is animating (Analyzing…, Starting…, or Downloading…), stop it otherwise.
void MainWindow::UpdateSpinTimer() {
	const bool active = !_analyzeClocks.isEmpty() || !_startingClocks.isEmpty() || !_activeDepotTags.isEmpty();
	if (active && !_spinTimer->isActive()) {
		_spinSec = -1; // force the counters to repaint on the next tick
		_spinTimer->start();
	} else if (!active && _spinTimer->isActive()) {
		_spinTimer->stop();
		_table->viewport()->update();
	}
}

void MainWindow::SetView(int mode) {
	if (mode == 0) {
		for (auto version = _versionByTag.constBegin(); version != _versionByTag.constEnd(); ++version) {
			if (version.value().isEmpty()) continue;
			for (auto state = _rowState.constBegin(); state != _rowState.constEnd(); ++state) {
				const QStringList parts = state.key().split('\t');
				if (parts.value(1) == version.key() && (state->up || state->starting)) {
					OnLog(QString("[skip] %1: stop its MCP servers before leaving the selected revision").arg(version.key()));
					return;
				}
			}
		}
		for (auto it = _versionByTag.begin(); it != _versionByTag.end(); ++it)
			if (!it.value().isEmpty()) emit RequestStoredVersion(it.key(), QString());
		_versionByTag.clear();
	}
	_view = mode;
	_stack->setCurrentWidget(_mainPage);
	UpdateSegmented(mode);
	_tabsRow->setVisible(mode == 1);
	_table->setColumnHidden(COL_TAG, mode == 1); // tag is redundant inside a per-tag tab
	ApplyFilter();
}

void MainWindow::UpdateSegmented(int active) {
	QPushButton* segs[3] = {_listBtn, _tabsBtn, _settingsBtn};
	const char* names[3] = {"list", "tab", "settings"};
	for (int i = 0; i < 3; ++i) {
		const bool on = (i == active);
		segs[i]->setProperty("variant", on ? "tonal" : "text");
		// active segment shows a check mark (like the M3 mock); others show their own icon
		segs[i]->setIcon(Ui::Icon(on ? "check" : names[i], on ? Pal::SEG_ACTIVE : Pal::SEG_INACTIVE, 18));
		segs[i]->style()->unpolish(segs[i]);
		segs[i]->style()->polish(segs[i]);
	}
}

void MainWindow::ApplyFilter() {
	if (_view == 0) { // list: show everything
		for (int i = 0; i < _table->rowCount(); ++i) _table->setRowHidden(i, false);
		UpdateActionButtons();
		return;
	}
	const QString tag = _tagCombo->currentData().toString();
	for (int i = 0; i < _table->rowCount(); ++i)
		_table->setRowHidden(i, _table->item(i, COL_TAG)->text() != tag);
	_table->scrollToTop();
	UpdateVersionSelector();
	UpdateActionButtons();
}

void MainWindow::UpdateVersionSelector() {
	if (!_versionCombo) return;
	if (!_tagCombo->count()) {
		_versionCombo->blockSignals(true);
		_versionCombo->clear();
		_versionCombo->addItem("No workspace", QString());
		_versionCombo->setEnabled(false);
		_versionCombo->blockSignals(false);
		return;
	}
	_versionCombo->setEnabled(!_busy);
	const QString tag = _tagCombo->currentData().toString();
	QString output;
	bool steam = false;
	QString currentManifest;
	for (const Workspace& workspace : AllWorkspaces(_cfg))
		if (workspace.tag == tag) {
			output = workspace.output;
			steam = workspace.depot.enabled;
			currentManifest = workspace.depot.manifest;
			break;
		}
	if (_versionField) _versionField->setTitle(steam ? "Manifest" : "Revision");
	const QString selected = _versionByTag.value(tag);
	// Desired items: Current plus every stored revision or manifest.
	QStringList wanted{QString()};
	if (!output.isEmpty() && steam) {
		QStringList manifests = QDir(output).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
		manifests.erase(std::remove_if(manifests.begin(), manifests.end(), [&currentManifest](const QString& name) {
			return name == currentManifest || !QRegularExpression("^[0-9]+$").match(name).hasMatch();
		}), manifests.end());
		struct ManifestOrder { QVersionNumber patch; qlonglong server = -1; };
		QHash<QString, ManifestOrder> order;
		for (const QString& manifest : manifests) {
			QFile inf(QDir(output).filePath(manifest + "/csgo/steam.inf"));
			if (!inf.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
			const QString text = QString::fromUtf8(inf.readAll());
			auto value = [&text](const QString& key) {
				return QRegularExpression(QString("(?mi)^\\s*%1\\s*=\\s*([^\\r\\n]+)").arg(QRegularExpression::escape(key))).match(text).captured(1).trimmed();
			};
			bool ok = false;
			const qlonglong server = value("ServerVersion").toLongLong(&ok);
			order.insert(manifest, {QVersionNumber::fromString(value("PatchVersion")), ok ? server : -1});
		}
		std::sort(manifests.begin(), manifests.end(), [&order](const QString& a, const QString& b) {
			const ManifestOrder left = order.value(a), right = order.value(b);
			const int patch = QVersionNumber::compare(left.patch, right.patch);
			if (patch != 0) return patch > 0;
			if (left.server != right.server) return left.server > right.server;
			return a > b;
		});
		wanted += manifests;
	} else if (!output.isEmpty()) {
		QDir dir(QDir(output).filePath("revisions"));
		QStringList revisions = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
		wanted += revisions;
	}
	// OnStatus polls this every refresh; only rebuild when the revision set actually
	// changed, so an open dropdown is not torn down (and Replace's new revision shows).
	QStringList current;
	for (int i = 0; i < _versionCombo->count(); ++i) current << _versionCombo->itemData(i).toString();
	QString currentLabel = "Current";
	for (const Workspace& workspace : _cfg.steamWorkspaces)
		if (workspace.tag == tag && (!workspace.depot.manifest.isEmpty() || !workspace.depot.patchVersion.isEmpty() || !workspace.depot.serverVersion.isEmpty())) {
			QStringList parts{workspace.depot.manifest.isEmpty() ? "Current" : workspace.depot.manifest};
			if (!workspace.depot.patchVersion.isEmpty()) parts << "Patch " + workspace.depot.patchVersion;
			if (!workspace.depot.serverVersion.isEmpty()) parts << "Server " + workspace.depot.serverVersion;
			currentLabel = parts.join(QStringLiteral(" \u00b7 "));
			break;
		}
	if (current != wanted || _versionCombo->itemText(0) != currentLabel) {
		_versionCombo->blockSignals(true);
		_versionCombo->clear();
		_versionCombo->addItem(currentLabel, QString());
		for (int i = 1; i < wanted.size(); ++i)
			_versionCombo->addItem(steam ? PrettyManifest(output, wanted[i]) : PrettyRevision(wanted[i]), wanted[i]);
		_versionCombo->blockSignals(false);
	}
	const int index = _versionCombo->findData(selected);
	if (index != _versionCombo->currentIndex()) {
		_versionCombo->blockSignals(true);
		_versionCombo->setCurrentIndex(index >= 0 ? index : 0);
		_versionCombo->blockSignals(false);
	}
}

void MainWindow::OnStatus(const QVector<LibRow>& rows) {
	_refreshPending = false;
	if (_refreshSpinTimer && _refreshSpinTimer->isActive()) { // Refresh finished
		_refreshSpinTimer->stop();
		_refreshBtn->setIcon(Ui::Icon("refresh", Pal::ON_SURFACE_VARIANT, 20));
	}
	int up = 0, need = 0, diff = 0;
	for (const LibRow& r : rows) {
		for (int t = 0; t < r.cells.size() && t < _tags.size(); ++t) {
			const QString rk = RowKey(r.name, _tags[t]);
			const int i = _rowOf.value(rk, -1);
			if (i < 0) continue;
			const Cell& c = r.cells[t];
			if (c.up) ++up;
			if (c.srcDiff) ++diff;
			if (c.state == "Not analyzed" || c.state == "Re-analyze" || c.state == "Update available") ++need;

			// A just-started server stays "Starting…" until its port answers (it is
			// now up) or the grace window lapses — then it hands back to real status.
			bool starting = false;
			if (_startingClocks.contains(rk)) {
				if (c.up || _startingClocks[rk].elapsed() >= kStartGraceMs)
					_startingClocks.remove(rk);
				else
					starting = true;
			}
			const Target visibleTarget{_tags[t], r.name, ActiveRevision(_tags[t])};
			const QString operation = _activeOperations.value(TargetKey(visibleTarget));
			const bool analyzing = operation == "analyze" || _analyzeClocks.contains(TargetKey(visibleTarget));
			const bool depotUpdating = _activeDepotTags.contains(_tags[t]);

			_table->item(i, COL_SERVER)->setText(c.present ? (c.up ? "UP" : "down") : "—");
			auto* portItem = _table->item(i, COL_PORT);
			portItem->setText(c.present && c.port > 0 ? QString::number(c.port) : "—");
			portItem->setData(Qt::UserRole, c.present ? c.port : 0); // numeric sort key
			auto* stItem = _table->item(i, COL_STATUS);
			if (depotUpdating) {
				stItem->setText("Downloading…");
				stItem->setToolTip(QString("%1\nStatus: DepotDownloader is updating this workspace.").arg(_tags[t]));
			} else if (operation == "ida") {
				stItem->setText("Open in IDA");
				stItem->setToolTip(QString("%1 @ %2\nStatus: Open in IDA - close that window before modifying this database.")
					.arg(r.name, _tags[t]));
			} else if (analyzing) {
				const qint64 elapsed = _analyzeClocks.contains(TargetKey(visibleTarget)) ?
					_analyzeClocks[TargetKey(visibleTarget)].elapsed() : 0;
				stItem->setText("Analyzing… " + fmtElapsed(elapsed));
				stItem->setToolTip(QString("%1 @ %2\nStatus: Analyzing…").arg(r.name, _tags[t]));
			} else if (starting) {
				stItem->setText("Starting… " + fmtElapsed(_startingClocks[rk].elapsed()));
				stItem->setToolTip(QString("%1 @ %2\nStatus: Starting… — IDA is loading the database and binding the MCP "
										   "port. Large databases can take up to a minute; Stop cancels it.")
									   .arg(r.name, _tags[t]));
			} else {
				stItem->setText(c.present ? c.state : "—");
				stItem->setToolTip(c.present ? c.tip : QString());
			}
			const qint64 eff = (c.size >= 0) ? c.size : c.srcSize;
			auto* szItem = _table->item(i, COL_SIZE);
			szItem->setText(eff >= 0 ? mb(eff) : "—");
			szItem->setData(Qt::UserRole, eff >= 0 ? eff : 0); // numeric sort key
			// Sync column: needs both a local copy and a live source to compare.
			const bool comparable = c.size >= 0 && c.srcSize >= 0;
			_table->item(i, COL_DELTA)->setText(!c.sourceConfigured ? QStringLiteral("—") :
				!comparable ? QStringLiteral("source missing") : c.srcDiff ? QStringLiteral("changed") : QStringLiteral("in sync"));

			RowState rs;
			rs.present = c.present;
			rs.up = c.up;
			rs.hasDb = c.hasDb;
			rs.localBin = (c.size >= 0);
			rs.srcBin = (c.srcSize >= 0);
			rs.starting = starting;
			rs.analyzing = analyzing;
			rs.idaOpen = operation == "ida";
			rs.depot = depotUpdating;
			_rowState[rk] = rs;
		}
	}
	_statServers->setText(QString("%1 servers up").arg(up));
	_statNeed->setText(QString("%1 need re-Analyze").arg(need));
	_statDiffer->setText(QString("%1 differ from source").arg(diff));
	ApplySort();	// re-apply the active sort with the new data
	RebuildRowOf(); // rows may have moved
	// setRowHidden flags are bound to row indices, not items: a sort that moved
	// rows leaves them on the wrong rows, which could hide a selected running row
	// and wrongly disable its actions. Re-apply the per-tag filter (no scroll).
	if (_view == 1) {
		const QString tag = _tagCombo->currentData().toString();
		for (int i = 0; i < _table->rowCount(); ++i)
			_table->setRowHidden(i, _table->item(i, COL_TAG)->text() != tag);
	}
	UpdateActionButtons();
	if (_view == 1) UpdateVersionSelector(); // reflect newly stored revisions or manifests
	UpdateSpinTimer(); // a Starting… row may have just resolved (up / timed out)

	// While a server is up — or one is still booting — we poll briskly so a change
	// (bind, or a user-closed IDA) shows within a few seconds instead of up to 15.
	_anyServerUp = (up > 0);
	if (_refreshTimer && !_busy && _stack && _stack->currentWidget() == _mainPage) {
		const int want = RefreshIntervalMs();
		if (_refreshTimer->interval() != want) _refreshTimer->start(want);
	}
}

void MainWindow::OnAnalyzeStarted(const Target& target) {
	const QString key = TargetKey(target);
	_analyzeClocks[key].start();
	const int row = ActiveRevision(target.tag) == target.revision ? _rowOf.value(RowKey(target.name, target.tag), -1) : -1;
	if (row < 0) { UpdateSpinTimer(); return; }
	if (auto* it = _table->item(row, COL_STATUS)) {
		it->setText("Analyzing… 0:00");
		it->setToolTip(QString("%1 @ %2\nStatus: Analyzing… — idat is disassembling the binary. "
							   "Large DLLs can take a few minutes; the server stays down until it finishes.")
						   .arg(target.name, target.tag));
	}
	UpdateSpinTimer(); // one timer spins + times every Analyzing… / Starting… cell
	_table->viewport()->update();
}

void MainWindow::OnAnalyzeFinished(const Target& target, bool ok) {
	// Reflect this library immediately; its own Refresh refines the filesystem state.
	_analyzeClocks.remove(TargetKey(target));
	const int row = ActiveRevision(target.tag) == target.revision ? _rowOf.value(RowKey(target.name, target.tag), -1) : -1;
	if (row < 0) return;
	if (auto* it = _table->item(row, COL_STATUS)) {
		it->setText(ok ? "Ready" : "Re-analyze");
		it->setToolTip(QString("%1 @ %2\nStatus: %3").arg(target.name, target.tag, ok ? "Ready — freshly analyzed" : "Analysis failed — see the log"));
	}
	_table->viewport()->update();
}

void MainWindow::OnOperationChanged(const Target& target, const QString& operation, bool active) {
	const QString key = TargetKey(target);
	if (active) _activeOperations.insert(key, operation);
	else _activeOperations.remove(key);
	if (ActiveRevision(target.tag) == target.revision) {
		RowState& state = _rowState[RowKey(target.name, target.tag)];
		if (operation == "analyze") state.analyzing = active;
		if (operation == "ida") {
			state.idaOpen = active;
			if (active) {
				const int row = _rowOf.value(RowKey(target.name, target.tag), -1);
				if (row >= 0) _table->item(row, COL_STATUS)->setText("Open in IDA");
			}
		}
	}
	UpdateActionButtons();
}

void MainWindow::OnWorkspaceOperationChanged(const QString& tag, const QString&, bool active) {
	if (active) _activeDepotTags.insert(tag);
	else _activeDepotTags.remove(tag);
	for (auto it = _rowState.begin(); it != _rowState.end(); ++it)
		if (it.key().section('\t', 1, 1) == tag) it->depot = active;
	if (active)
		for (int row = 0; row < _table->rowCount(); ++row)
			if (_table->item(row, COL_TAG)->text() == tag)
				_table->item(row, COL_STATUS)->setText("Downloading…");
	UpdateSpinTimer();
	// DoDepotUpdate takes the busy lock when it issues the request, and the worker
	// saves the config only when a manifest actually changed — so ConfigSaveFinished
	// cannot be relied on to release it. This signal is the one guaranteed end of the
	// operation: an update that changed nothing must still hand the toolbar back.
	const bool depotFinished = !active && _activeDepotTags.isEmpty();
	OnBusy(depotFinished ? false : _busy); // re-applies the locks; a live depot keeps Stop enabled
}

// Infer a canonical log level from the worker's message prefix so the file reads
// as a proper log (the panel keeps the raw text either way).
static Log::Level InferLogLevel(const QString& m) {
	if (m.startsWith("[fail]") || m.startsWith("[refused]") || m.contains("failed") || m.contains("cannot ") || m.contains("rejected"))
		return Log::Level::Error;
	if (m.startsWith("[warn]") || m.startsWith("[skip]") || m.startsWith("[stopped]"))
		return Log::Level::Warn;
	return Log::Level::Info;
}

void MainWindow::OnLog(const QString& msg) {
	Log::Write(InferLogLevel(msg), msg); // everything the panel shows is persisted to the file
	_log->appendPlainText(msg);
}

void MainWindow::OnBusy(bool busy) {
	_busy = busy;
	if (!busy && _spinTimer) UpdateSpinTimer();
	if (busy && _refreshSpinTimer && _refreshSpinTimer->isActive()) { // op preempts a Refresh
		_refreshSpinTimer->stop();
		_refreshBtn->setIcon(Ui::Icon("refresh", Pal::ON_SURFACE_VARIANT, 20));
	}
	if (busy) {
		// Config save/import is transactional and briefly locks the whole editor.
		_startBtn->setEnabled(false);
		_openIdaBtn->setEnabled(false);
		_analyzeBtn->setEnabled(false);
		_replaceBtn->setEnabled(false);
		_depotBtn->setEnabled(false);
		if (_deleteBtn) _deleteBtn->setEnabled(false); // UpdateActionButtons skips while busy
		_stopBtn->setText("Stop");
		_stopBtn->setIcon(Ui::Icon("stop", Pal::STOP_FG, 18, true));
		// A depot update holds the lock for its whole run (minutes), so Stop stays live
		// there — cancelling DepotDownloader is the only way out of a stuck download.
		_stopBtn->setEnabled(!_activeDepotTags.isEmpty());
	} else {
		_stopBtn->setText("Stop");
		_stopBtn->setIcon(Ui::Icon("stop", Pal::STOP_FG, 18, true));
	}
	if (_refreshBtn) _refreshBtn->setEnabled(!busy);
	if (_addBinBtn) _addBinBtn->setEnabled(!busy);
	if (_versionCombo) _versionCombo->setEnabled(!busy && _tagCombo && _tagCombo->count() > 0);
	if (_tagCombo) _tagCombo->setEnabled(!busy);
	if (_table) static_cast<SelectHeader*>(_table->horizontalHeader())->setSelectionEnabled(!busy);
	if (_mcpBtn) _mcpBtn->setEnabled(!busy);
	if (_settingsBtn) _settingsBtn->setEnabled(!busy);
	if (_listBtn) _listBtn->setEnabled(!busy);
	if (_tabsBtn) _tabsBtn->setEnabled(!busy);
	if (_refreshTimer) {
		if (busy)
			_refreshTimer->stop();
		else if (_stack && _stack->currentWidget() == _mainPage)
			_refreshTimer->start(RefreshIntervalMs());
	}
	// Last, so it keeps the final word on the widgets it owns (Add binary, Settings):
	// the blanket !busy assignments above would otherwise re-enable them while IDA is
	// still open on a row, leaving buttons that look live but do nothing when clicked.
	if (!busy) UpdateActionButtons(); // selection-aware enable
}

// The configured file list is a wish list, not a promise: a depot may not ship every
// module, a source folder may not hold every binary. So a checked row that an action
// cannot touch is skipped, never a veto over the rest of the selection.
QString MainWindow::ActionBlocker(Action action, const Target& target) const {
	const RowState state = _rowState.value(RowKey(target.name, target.tag));
	const bool depotActive = _activeDepotTags.contains(target.tag);
	// Stop is the mirror image of the others: it wants a row that IS busy.
	if (action == Action::Stop)
		return (state.up || state.starting || state.analyzing || depotActive) ? QString() : "nothing running";

	// Depot update is workspace-scoped: the checked row only names the workspace, so what
	// that particular module is doing is beside the point — and a served or opened module
	// anywhere in the workspace is no obstacle either, because the download lands in a
	// fresh <ManifestID> directory. The one case that is refused (replacing a directory
	// that is in use) depends on which manifest Steam answers with, so it cannot be known
	// before the click: the worker reports it as a [skip].
	if (action == Action::Depot) {
		if (depotActive) return "depot update running";
		for (const Workspace& workspace : _cfg.steamWorkspaces)
			if (workspace.tag == target.tag)
				return workspace.depot.enabled ? QString() : "depot download is off for this workspace";
		return "not a Steam workspace";
	}

	// Transient conflicts: someone else holds the files. Listed before the permanent
	// properties below because these are the ones the user can clear and retry.
	if (depotActive) return "depot update running";
	if (state.idaOpen) return "open in IDA";
	if (state.analyzing) return "analyzing";
	// Already serving is not a fault for Start — it just has nothing left to do there.
	if (state.up || state.starting)
		return action == Action::Start ? "already served" : "MCP server running — stop it first";

	switch (action) {
		using enum Action;
		case Start:
			return state.hasDb ? QString() : "no database — Analyze it first";
		case OpenIda:
			return (state.hasDb || state.localBin) ? QString() : "no database and no binary";
		case Analyze:
			return (state.localBin || state.srcBin) ? QString() : "no binary here";
		case Replace:
			if (!state.srcBin) return "no binary in the source folder";
			for (const Workspace& workspace : _cfg.workspaces)
				if (workspace.tag == target.tag) return {};
			return "not a source-folder workspace";
		// Delete only edits config, but the transient checks above still apply: a
		// running server must be stopped first so we never orphan a process whose
		// row we are about to drop.
		case Delete:
		case Depot: // both answered above
		case Stop:
			return {};
	}
	return {};
}

QVector<Target> MainWindow::EligibleTargets(Action action, const QVector<Target>& selection, QStringList* skipped) const {
	QVector<Target> eligible;
	for (const Target& target : selection) {
		const QString blocker = ActionBlocker(action, target);
		if (blocker.isEmpty())
			eligible.push_back(target);
		else if (skipped)
			*skipped << QString("%1 — %2").arg(target.name, blocker);
	}
	return eligible;
}

void MainWindow::LogSkipped(const QString& action, const QStringList& skipped) {
	if (skipped.isEmpty()) return;
	OnLog(QString("%1: skipping %2 of the checked modules (%3)").arg(action).arg(skipped.size()).arg(skipped.join("; ")));
}

// Bulleted "name — reason" list. Capped so a 200-row selection cannot grow a tooltip
// past the screen; the tail is counted, never silently dropped.
static QString SkipList(const QStringList& skipped, int cap = 6) {
	QStringList lines;
	for (const QString& entry : skipped.mid(0, cap)) lines << QString("  • ") + entry;
	if (skipped.size() > cap) lines << QString("  • …and %1 more").arg(skipped.size() - cap);
	return lines.join('\n');
}

static QString Modules(int count) { return count == 1 ? QStringLiteral("1 module") : QString("%1 modules").arg(count); }

// Standard multi-select semantics: the checked rows are the scope an action may work
// in, not a promise that every one of them fits it. A button lights up when the
// action fits at least one checked row and then runs on exactly that subset. What it
// skips — or why it is dead — is always in the tooltip (and repeated in the
// confirmation dialog and the log), so a grey button is never a mystery.
void MainWindow::UpdateActionButtons() {
	if (_busy) return; // busy state is owned by OnBusy()
	int visible = 0, checked = 0;
	for (int i = 0; i < _table->rowCount(); ++i)
		if (!_table->isRowHidden(i)) {
			++visible;
			if (_table->item(i, COL_SEL)->checkState() == Qt::Checked) ++checked;
		}
	const Qt::CheckState headerState = checked == 0 ? Qt::Unchecked : checked == visible ? Qt::Checked : Qt::PartiallyChecked;
	static_cast<SelectHeader*>(_table->horizontalHeader())->setCheckState(headerState);

	const QVector<Target> selection = SelectedTargets();
	const bool archived = _view == 1 && _versionCombo && !_versionCombo->currentData().toString().isEmpty();
	const bool depotRunning = !_activeDepotTags.isEmpty();
	const QString archivedGate = "A stored version is selected — switch the Manifest/Revision back to Current.";
	const QString readyGate = "IDA, IDAPython and ida-pro-mcp must all be ready (see the chips above).";

	// `gate` is a precondition that has nothing to do with the individual rows: while
	// it holds the button is dead no matter what is checked, and it says why.
	auto apply = [&](QPushButton* button, Action action, const QString& title, const QString& runs, const QString& gate) {
		if (!button) return;
		QStringList skipped;
		const int count = gate.isEmpty() ? EligibleTargets(action, selection, &skipped).size() : 0;
		button->setEnabled(count > 0);
		button->setToolTip(title + "\n" + (!gate.isEmpty() ? gate :
			selection.isEmpty() ? QStringLiteral("Check at least one module.") :
			count == 0 ? QString("Fits none of the %1 checked:\n%2").arg(Modules(selection.size()), SkipList(skipped)) :
			skipped.isEmpty() ? QString("%1 %2.").arg(runs, Modules(count)) :
			QString("%1 %2 of the %3 checked — skipping:\n%4").arg(runs).arg(count).arg(Modules(selection.size()), SkipList(skipped))));
	};
	apply(_startBtn, Action::Start, "MCP Start", "Serves", _ready ? QString() : readyGate);
	apply(_openIdaBtn, Action::OpenIda, "Open the .i64 database in IDA, or import the binary when there is none", "Opens",
		!QFile::exists(_cfg.idaGui) ? QString("ida.exe was not found at %1 — set it in Settings.").arg(_cfg.idaGui) :
		selection.size() > 1 ? QStringLiteral("IDA opens one module at a time — check a single row.") : QString());
	apply(_analyzeBtn, Action::Analyze, "Analyze", "Rebuilds the database of", archived ? archivedGate : _ready ? QString() : readyGate);
	apply(_replaceBtn, Action::Replace, "Replace", "Pulls from source for", archived ? archivedGate : QString());
	apply(_deleteBtn, Action::Delete, "Delete from workbench (files on disk are kept)", "Removes",
		depotRunning ? QStringLiteral("Wait for the depot update to finish.") : archived ? archivedGate : QString());
	_stopBtn->setText("Stop");
	_stopBtn->setIcon(Ui::Icon("stop", Pal::STOP_FG, 18, true));
	apply(_stopBtn, Action::Stop, "Stop", "Stops", QString());

	// A depot update is workspace-scoped: the checked rows only pick which workspace,
	// and the download always covers that workspace's whole file list.
	if (_depotBtn) {
		QStringList depotTags, blockers;
		for (const Target& target : selection) {
			const QString blocker = ActionBlocker(Action::Depot, target);
			if (blocker.isEmpty()) {
				if (!depotTags.contains(target.tag)) depotTags << target.tag;
			} else if (!blockers.contains(blocker)) {
				blockers << blocker;
			}
		}
		const QString gate = depotRunning ? QStringLiteral("A depot update is already running.") : archived ? archivedGate : QString();
		_depotBtn->setEnabled(gate.isEmpty() && depotTags.size() == 1);
		_depotBtn->setToolTip("Depot update\n" + (!gate.isEmpty() ? gate :
			depotTags.size() > 1 ? QString("One Steam workspace at a time — %1 are checked.").arg(depotTags.join(", ")) :
			!depotTags.isEmpty() ? QString("Downloads the whole file list of %1, not only the checked modules.").arg(depotTags.first()) :
			selection.isEmpty() ? QStringLiteral("Check a module of a Steam workspace.") :
			QString("Not available here: %1.").arg(blockers.join("; "))));
	}
	if (_addBinBtn) _addBinBtn->setEnabled(!depotRunning);
	if (_settingsBtn) {
		const bool configSafe = _activeOperations.isEmpty() && _activeDepotTags.isEmpty();
		_settingsBtn->setEnabled(configSafe);
		_settingsBtn->setToolTip(configSafe ? "Edit configuration" :
			"Close IDA and finish active analyses before editing paths");
	}
}

void MainWindow::OnReadiness(const Readiness& r) {
	auto setChip = [](QLabel* ic, QLabel* t, const QString& name, bool ok, const QString& tip) {
		const QString col = (ok ? Pal::OK_GREEN : Pal::ERROR_COLOR).name();
		ic->setText(Ui::Sym(ok ? "check_circle" : "close"));
		ic->setStyleSheet(QString("color:%1; background:transparent;").arg(col));
		ic->setToolTip(tip);
		t->setText(name);
		t->setStyleSheet(QString("color:%1; font-size:12px; font-weight:500; background:transparent;").arg(col));
		t->setToolTip(tip);
	};
	setChip(_chipPythonIc, _chipPython, r.pythonLabel.isEmpty() ? "IDAPython" : r.pythonLabel, r.python, r.pythonMsg);
	setChip(_chipIdaIc, _chipIda, "IDA", r.ida, r.idaMsg);
	setChip(_chipMcpIc, _chipMcp, "ida-pro-mcp", r.mcp, r.mcpMsg);

	_ready = r.Ready();
	_readyMsg->setVisible(!_ready);
	_readyMsg->setText(_ready ? QString() : "Start and Analyze are unavailable until IDA, IDAPython and ida-pro-mcp "
											"are present. Stop and Replace remain available (fix paths in Settings).");
	OnBusy(_busy);
}

void MainWindow::OnConfigLoaded(const ConfigView& v) {
	_cfg = v;
	RecomputeModel();
	BuildTable();
	OnLog("config reloaded");
}

// Double-clicking a Port cell prompts for a new port. Uses a plain line-edit
// dialog (no spinbox arrows) and only persists when the value actually changes.
void MainWindow::EditPort(int row) {
	if (_busy || !_activeDepotTags.isEmpty()) return;
	auto* portItem = _table->item(row, COL_PORT);
	if (!portItem) return;
	const QString name = _table->item(row, COL_MODULE)->text();
	const QString tag = _table->item(row, COL_TAG)->text();
	const int cur = portItem->text().toInt();
	bool ok = false;
	const QString txt = QInputDialog::getText(
		this, "MCP port", QString("Port for %1 @ %2:").arg(name, tag),
		QLineEdit::Normal, cur > 0 ? QString::number(cur) : QString(), &ok
	);
	if (!ok) return;
	const int p = txt.trimmed().toInt();
	if (p < 1 || p > 65535) {
		OnLog(QString("(port must be 1..65535)"));
		return;
	}
	if (p == cur) return;
	ApplyPortEdit(row, p);
}

// A port typed into the main table's Port cell. Any instance's port is editable
// here: a scan library stores a per-(tag,name) override (dropped when set back to
// the auto value), an extra library stores its absolute port. Then we save.
void MainWindow::ApplyPortEdit(int row, int newPort) {
	if (_busy || !_activeDepotTags.isEmpty()) {
		QueueRefresh();
		return;
	} // don't edit mid-operation; revert cell
	const QString name = _table->item(row, COL_MODULE)->text();
	const QString tag = _table->item(row, COL_TAG)->text();
	ConfigView cfg = _cfg; // edit a copy; _cfg only changes via ConfigLoaded

	bool scanHere = false;
	int fileIndex = -1;
	int tagOffset = 0; // does this tag track this scan library?
	for (const Workspace& sd : AllWorkspaces(cfg))
		if (sd.tag == tag) {
			tagOffset = sd.portOffset;
			for (int i = 0; i < sd.files.size(); ++i)
				if (QFileInfo(sd.files[i]).completeBaseName() == name) {
					scanHere = true;
					fileIndex = i;
					break;
				}
			break;
		}

	bool applied = false;
	if (scanHere) { // scan library: (tag, name) override
		for (int i = cfg.portOverrides.size() - 1; i >= 0; --i)
			if (cfg.portOverrides[i].tag == tag && cfg.portOverrides[i].name == name)
				cfg.portOverrides.remove(i);
		if (newPort != AutoScanPort(cfg.basePort, fileIndex, tagOffset))
			cfg.portOverrides.push_back({tag, name, newPort});
		applied = true;
	} else { // extra library: absolute port
		for (int i = 0; i < cfg.extraLibs.size(); ++i)
			if (cfg.extraLibs[i].tag == tag && QFileInfo(cfg.extraLibs[i].path).completeBaseName() == name) {
				cfg.extraLibs[i].port = (newPort == AutoExtraPort(cfg.basePort, i)) ? 0 : newPort;
				applied = true;
				break;
			}
	}
	if (!applied) {
		QueueRefresh();
		return;
	}

	OnBusy(true);
	emit RequestSaveConfig(cfg);
}

QVector<Target> MainWindow::SelectedTargets() const {
	QVector<Target> targets;
	for (int i = 0; i < _table->rowCount(); ++i) {
		if (_table->isRowHidden(i)) continue; // only act on what is visible + checked
		if (_table->item(i, COL_SEL)->checkState() != Qt::Checked) continue;
		const QString tag = _table->item(i, COL_TAG)->text();
		targets.push_back({tag, _table->item(i, COL_MODULE)->text(), ActiveRevision(tag)});
	}
	return targets;
}

QString MainWindow::ActiveRevision(const QString& tag) const {
	if (_view != 1 || !_tagCombo || _tagCombo->currentData().toString() != tag || !_versionCombo) return {};
	return _versionCombo->currentData().toString();
}

void MainWindow::DoOpenIda() {
	if (_busy) return;
	const QVector<Target> selection = SelectedTargets();
	if (selection.size() != 1) {
		OnLog("(check exactly one module to open in IDA)");
		return;
	}
	const QString blocker = ActionBlocker(Action::OpenIda, selection.first());
	if (!blocker.isEmpty()) {
		OnLog(QString("(cannot open %1: %2)").arg(selection.first().name, blocker));
		return;
	}
	emit RequestOpenIda(selection.first());
}

void MainWindow::DoStart() {
	if (_busy) return;
	QStringList skipped;
	const QVector<Target> targets = EligibleTargets(Action::Start, SelectedTargets(), &skipped);
	if (targets.isEmpty()) {
		OnLog(skipped.isEmpty() ? "(check at least one module)" : QString("(nothing to start — %1)").arg(skipped.join("; ")));
		return;
	}
	LogSkipped("start", skipped);
	// Mark each target "Starting…" now: IDA takes seconds-to-a-minute to bind its
	// port, and until it does the row is "down". Tracking the launch keeps the Stop
	// button live (and the cell honest) through that window instead of greying out.
	for (const Target& tg : targets)
		if (!_rowState.value(RowKey(tg.name, tg.tag)).up) _startingClocks[RowKey(tg.name, tg.tag)].start();
	UpdateSpinTimer();
	if (_refreshTimer) _refreshTimer->start(RefreshIntervalMs()); // poll briskly to catch the bind
	emit RequestStart(targets);
}
void MainWindow::DoStop() {
	if (_busy && _activeDepotTags.isEmpty()) return; // busy for a config save: nothing to stop
	const QVector<Target> selected = SelectedTargets();
	if (selected.isEmpty()) {
		OnLog("(select at least one row)");
		return;
	}
	QVector<Target> analyses;
	QVector<Target> servers;
	bool stopDepot = false;
	for (const Target& target : selected) {
		// While the depot holds the busy lock only its own cancel is safe to act on.
		if (_activeDepotTags.contains(target.tag)) {
			stopDepot = true;
		} else if (_busy) {
			continue;
		} else if (_activeOperations.value(TargetKey(target)) == "analyze") {
			analyses.push_back(target);
		} else {
			const RowState state = _rowState.value(RowKey(target.name, target.tag));
			if (state.up || state.starting) servers.push_back(target);
		}
	}
	if (!analyses.isEmpty()) emit RequestStopOperations(analyses);
	if (stopDepot) {
		OnLog("stopping DepotDownloader...");
		_mgr->RequestStopDepot();
	}
	// Dropping the "Starting…" mark hands the row back to real port status: the
	// worker kills whatever is on the port, and anything still booting shows plain
	// "down" (then UP once it binds, where a second Stop takes effect).
	for (const Target& target : servers) _startingClocks.remove(RowKey(target.name, target.tag));
	if (!servers.isEmpty()) {
		UpdateSpinTimer();
		emit RequestStop(servers);
	}
}
// Confirmation dialogs spell out the batch the command really runs on and list what
// it leaves behind: partial application is only safe when it is visible before the OK.
void MainWindow::DoAnalyze() {
	if (_busy) return;
	const QVector<Target> selection = SelectedTargets();
	QStringList skipped;
	const QVector<Target> targets = EligibleTargets(Action::Analyze, selection, &skipped);
	if (targets.isEmpty()) {
		OnLog(skipped.isEmpty() ? "(check at least one module)" : QString("(nothing to analyze — %1)").arg(skipped.join("; ")));
		return;
	}
	QString text = QString("Analyze %1?\nThis overwrites their .i64 (annotations lost) and can take minutes.").arg(Modules(targets.size()));
	if (!skipped.isEmpty())
		text += QString("\n\nSkipping %1 of the %2 checked:\n%3").arg(skipped.size()).arg(Modules(selection.size()), SkipList(skipped, 12));
	if (QMessageBox::warning(this, "Analyze", text, QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Ok) {
		LogSkipped("analyze", skipped);
		emit RequestAnalyze(targets, true);
	}
}

void MainWindow::DoReplace() {
	if (_busy) return;
	const QVector<Target> selection = SelectedTargets();
	QStringList skipped;
	const QVector<Target> targets = EligibleTargets(Action::Replace, selection, &skipped);
	if (targets.isEmpty()) {
		OnLog(skipped.isEmpty() ? "(check at least one module)" : QString("(nothing to replace — %1)").arg(skipped.join("; ")));
		return;
	}
	QString text = QString("Replace %1 from their source folder?\n\n"
						   "• If the source has an IDA database next to the binary, the binary "
						   "and its .i64 are copied over — ready without re-analysis.\n"
						   "• If it doesn't, only the fresh binary is copied and the stale "
						   "database here is removed — press Analyze afterwards.")
					   .arg(Modules(targets.size()));
	if (!skipped.isEmpty())
		text += QString("\n\nSkipping %1 of the %2 checked:\n%3").arg(skipped.size()).arg(Modules(selection.size()), SkipList(skipped, 12));
	if (QMessageBox::warning(this, "Replace binaries", text, QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Ok) {
		LogSkipped("replace", skipped);
		emit RequestReplace(targets);
	}
}

void MainWindow::DoDepotUpdate() {
	if (_busy || !_activeDepotTags.isEmpty()) return;
	// The checked rows only pick the workspace here: DepotDownloader then fetches that
	// workspace's whole file list, so nothing is "skipped" by narrowing the selection.
	QStringList blockers;
	QStringList tags;
	for (const Target& target : SelectedTargets()) {
		const QString blocker = ActionBlocker(Action::Depot, target);
		if (blocker.isEmpty()) {
			if (!tags.contains(target.tag)) tags << target.tag;
		} else if (!blockers.contains(blocker)) {
			blockers << blocker;
		}
	}
	if (tags.isEmpty()) {
		OnLog(blockers.isEmpty() ? "(check at least one module of a Steam workspace)"
								 : QString("(no workspace to update — %1)").arg(blockers.join("; ")));
		return;
	}
	if (tags.size() != 1) {
		QMessageBox::information(this, "Download depot", "Select files from one Steam workspace at a time.");
		return;
	}
	const Workspace* workspace = nullptr;
	for (const Workspace& candidate : _cfg.steamWorkspaces)
		if (candidate.tag == tags.first()) { workspace = &candidate; break; }
	if (!workspace) return;

	QDialog dialog(this);
	dialog.setWindowTitle("Download depot");
	dialog.setMinimumWidth(520);
	auto* layout = new QVBoxLayout(&dialog);
	// The ManifestID mode adds credential fields. Let the top-level dialog follow
	// the layout's size hint in both directions when those fields are shown/hidden.
	layout->setSizeConstraint(QLayout::SetFixedSize);
	bool customMode = false;
	auto* latest = Ui::Button("Latest", "tonal");
	auto* specific = Ui::Button("ManifestID", "text");
	auto* mode = Ui::SegmentedControl({latest, specific}, 240);
	layout->addWidget(mode, 0, Qt::AlignLeft);
	auto* customId = new QLineEdit;
	customId->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]+"), customId));
	auto* customField = new QGroupBox("ManifestID");
	customField->setObjectName("fieldBox");
	auto* customLayout = new QVBoxLayout(customField);
	customLayout->setContentsMargins(10, 2, 10, 2);
	customLayout->addWidget(customId);
	layout->addWidget(customField);
	auto makeField = [&dialog, layout](const QString& title, QLineEdit::EchoMode echo = QLineEdit::Normal) {
		auto* input = new QLineEdit(&dialog);
		input->setEchoMode(echo);
		auto* field = new QGroupBox(title, &dialog);
		field->setObjectName("fieldBox");
		auto* fieldLayout = new QVBoxLayout(field);
		fieldLayout->setContentsMargins(10, 2, 10, 2);
		fieldLayout->addWidget(input);
		layout->addWidget(field);
		return qMakePair(input, field);
	};
	const auto usernameField = makeField("Steam username");
	const auto passwordField = makeField("Steam password", QLineEdit::Password);
	const auto authCodeField = makeField("Steam Guard code (optional)", QLineEdit::Password);
	usernameField.first->setText(qEnvironmentVariable("STEAM_USERNAME"));
	passwordField.first->setText(qEnvironmentVariable("STEAM_PASSWORD"));
	authCodeField.first->setText(qEnvironmentVariable("STEAM_GUARD_CODE"));
	auto* rememberSession = new QCheckBox("Remember Steam session", &dialog);
	layout->addWidget(rememberSession);
	auto* footer = new QHBoxLayout;
	const QString depotOs = workspace->depot.os.compare("linux", Qt::CaseInsensitive) == 0 ? "Linux" : "Windows";
	auto* steamDb = Ui::IconButton("manage_search", QString("SteamDB %1 manifests").arg(depotOs), "text");
	footer->addWidget(steamDb);
	footer->addStretch();
	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	buttons->button(QDialogButtonBox::Ok)->setText("Download");
	buttons->button(QDialogButtonBox::Ok)->setIcon(Ui::Icon("download", QColor("#FFFFFF"), 19, true));
	footer->addWidget(buttons);
	layout->addLayout(footer);
	const int depotId = workspace->depot.os.compare("linux", Qt::CaseInsensitive) == 0 ? 2347773 : 2347771;
	connect(steamDb, &QPushButton::clicked, &dialog, [depotId] {
		QDesktopServices::openUrl(QUrl(QString("https://steamdb.info/depot/%1/manifests/").arg(depotId)));
	});
	auto applyMode = [&customMode, latest, specific, customId, customField, usernameField, passwordField, authCodeField, rememberSession, buttons] {
		latest->setProperty("variant", customMode ? "text" : "tonal");
		specific->setProperty("variant", customMode ? "tonal" : "text");
		for (QPushButton* button : {latest, specific}) {
			button->style()->unpolish(button);
			button->style()->polish(button);
		}
		customField->setVisible(customMode);
		customId->setEnabled(customMode);
		for (QGroupBox* field : {usernameField.second, passwordField.second, authCodeField.second}) field->setVisible(customMode);
		rememberSession->setVisible(customMode);
		const bool codeHasCredentials = authCodeField.first->text().trimmed().isEmpty() ||
			(!usernameField.first->text().trimmed().isEmpty() && !passwordField.first->text().isEmpty());
		const bool credentialsHaveUser = (passwordField.first->text().isEmpty() && authCodeField.first->text().trimmed().isEmpty()) ||
			!usernameField.first->text().trimmed().isEmpty();
		buttons->button(QDialogButtonBox::Ok)->setEnabled(!customMode ||
			(!customId->text().trimmed().isEmpty() && codeHasCredentials && credentialsHaveUser));
	};
	connect(latest, &QPushButton::clicked, &dialog, [&customMode, applyMode] { customMode = false; applyMode(); });
	connect(specific, &QPushButton::clicked, &dialog, [&customMode, applyMode, customId] { customMode = true; applyMode(); customId->setFocus(); });
	connect(customId, &QLineEdit::textChanged, &dialog, [applyMode](const QString&) { applyMode(); });
	for (QLineEdit* input : {usernameField.first, passwordField.first, authCodeField.first})
		connect(input, &QLineEdit::textChanged, &dialog, [applyMode](const QString&) { applyMode(); });
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	applyMode();
	if (dialog.exec() != QDialog::Accepted) return;
	const QString selected = customMode ? customId->text().trimmed() : "latest";
	OnBusy(true);
	emit RequestDepotUpdate(tags, selected,
		customMode ? usernameField.first->text().trimmed() : QString(),
		customMode ? passwordField.first->text() : QString(),
		customMode ? authCodeField.first->text().trimmed() : QString(),
		customMode && rememberSession->isChecked());
}

// Remove the selected libraries from the workbench. This edits config.json only —
// the analyzed .i64 and the binaries on disk are left untouched. Each selected
// (tag, library) entry is removed from its own workspace only.
void MainWindow::DoDelete() {
	if (_busy || !_activeDepotTags.isEmpty()) return;
	const QVector<Target> selection = SelectedTargets();
	QStringList skipped;
	const QVector<Target> targets = EligibleTargets(Action::Delete, selection, &skipped);
	if (targets.isEmpty()) {
		OnLog(skipped.isEmpty() ? "(check at least one module)" : QString("(nothing to remove — %1)").arg(skipped.join("; ")));
		return;
	}

	auto matchesExtra = [](const ExtraLib& e, const QString& tag, const QString& name) {
		return e.tag == tag && QFileInfo(e.path).completeBaseName() == name;
	};
	auto isExtra = [&](const QString& tag, const QString& name) {
		for (const ExtraLib& e : _cfg.extraLibs)
			if (matchesExtra(e, tag, name)) return true;
		return false;
	};

	QVector<Target> workspaceTargets;
	QVector<QPair<QString, QString>> extras;		 // (tag, name) extra libraries
	for (const Target& t : targets) {
		if (isExtra(t.tag, t.name))
			extras.push_back({t.tag, t.name});
		else
			workspaceTargets.push_back(t);
	}

	QStringList lines;
	for (const Target& target : workspaceTargets) lines << QString("  •  %1 @ %2  — workspace file").arg(target.name, target.tag);
	for (const auto& e : extras) lines << QString("  •  %1 @ %2  — extra").arg(e.second, e.first);
	const int count = workspaceTargets.size() + extras.size();

	QString text = QString("Remove %1 from the workbench?\n\n%2\n\nThe analyzed .i64 and binaries on disk are NOT deleted.")
					   .arg(Modules(count), lines.join('\n'));
	if (!skipped.isEmpty())
		text += QString("\n\nKeeping %1 of the %2 checked:\n%3").arg(skipped.size()).arg(Modules(selection.size()), SkipList(skipped, 12));
	if (QMessageBox::warning(this, "Delete from workbench", text,
			QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok)
		return;
	LogSkipped("delete", skipped);

	ConfigView cfg = _cfg;
	// Extra libraries: drop the exact (tag, name) entries.
	for (const auto& e : extras)
		for (int i = cfg.extraLibs.size() - 1; i >= 0; --i)
			if (matchesExtra(cfg.extraLibs[i], e.first, e.second)) cfg.extraLibs.remove(i);
	for (const Target& target : workspaceTargets) {
		auto removeFrom = [&target](QVector<Workspace>& workspaces) {
			for (Workspace& workspace : workspaces)
				if (workspace.tag == target.tag)
					for (int i = workspace.files.size() - 1; i >= 0; --i)
						if (QFileInfo(workspace.files[i]).completeBaseName() == target.name) workspace.files.remove(i);
		};
		removeFrom(cfg.workspaces);
		removeFrom(cfg.steamWorkspaces);
		for (int i = cfg.portOverrides.size() - 1; i >= 0; --i)
			if (cfg.portOverrides[i].tag == target.tag && cfg.portOverrides[i].name == target.name) cfg.portOverrides.remove(i);
	}

	OnLog(QString("delete: removing %1 from the workbench (files on disk kept)").arg(Modules(count)));
	OnBusy(true);
	emit RequestSaveConfig(cfg);
}

void MainWindow::OpenSettings() {
	if (_stack->currentWidget() != _mainPage) return; // already in settings
	if (!_activeOperations.isEmpty() || !_activeDepotTags.isEmpty()) return;
	if (_refreshTimer) _refreshTimer->stop();		  // pause auto-Refresh while editing
	_settingsPanel = new SettingsPanel(_cfg, this);
	connect(_settingsPanel, &SettingsPanel::Cancelled, this, &MainWindow::CloseSettings);
	connect(_settingsPanel, &SettingsPanel::Saved, this, [this](const ConfigView& v) {
		_settingsPanel->setEnabled(false);
		OnBusy(true);
		emit RequestSaveConfig(v);
	});
	connect(_settingsPanel, &SettingsPanel::ImportRequested, this, [this](const QString& file) {
		_settingsPanel->setEnabled(false);
		OnBusy(true);
		emit RequestImportConfig(file); // outcome arrives via ConfigSaveFinished
	});
	_stack->addWidget(_settingsPanel);
	_stack->setCurrentWidget(_settingsPanel);
	UpdateSegmented(2);
}

void MainWindow::CloseSettings() {
	_stack->setCurrentWidget(_mainPage);
	if (_settingsPanel) {
		_stack->removeWidget(_settingsPanel);
		_settingsPanel->deleteLater();
		_settingsPanel = nullptr;
	}
	UpdateSegmented(_view);
	if (_refreshTimer) _refreshTimer->start(RefreshIntervalMs());
}

void MainWindow::ToggleAll() {
	if (_busy) return;
	bool anyUnchecked = false;
	for (int i = 0; i < _table->rowCount(); ++i)
		if (!_table->isRowHidden(i) && _table->item(i, COL_SEL)->checkState() != Qt::Checked) {
			anyUnchecked = true;
			break;
		}
	const Qt::CheckState s = anyUnchecked ? Qt::Checked : Qt::Unchecked;
	for (int i = 0; i < _table->rowCount(); ++i) {
		if (_table->isRowHidden(i)) continue;
		_table->item(i, COL_SEL)->setCheckState(s);
		UpdateRowSelection(i);
	}
	static_cast<SelectHeader*>(_table->horizontalHeader())->setCheckState(s);
	UpdateActionButtons();
}

// Quick-add a tag-scoped library from the main page: pick a binary file and a tag
// (an "extra library", same as the Settings section). It shows up under that tag
// after the save round-trips through ConfigLoaded -> BuildTable.
void MainWindow::AddBinary() {
	if (_busy || !_activeDepotTags.isEmpty()) return;
	// All files by default: extra libraries are exactly for one-offs whose
	// extension may differ from the configured scan extensions.
	const QString file = QFileDialog::getOpenFileName(
		this, "Add a binary", QString(),
		"All files (*)"
	);
	if (file.isEmpty()) return;

	QStringList tagChoices = _tags; // offer the existing tags; a new one can be typed
	if (tagChoices.isEmpty()) tagChoices << "extra";
	bool ok = false;
	const QString tag = QInputDialog::getItem(
							this, "Add binary", QString("Tag for %1:").arg(QFileInfo(file).completeBaseName()),
							tagChoices, 0, true, &ok
	)
							.trimmed();
	if (!ok || tag.isEmpty()) return;

	const QString name = QFileInfo(file).completeBaseName();
	if (InstanceExists(name, tag)) {
		OnLog(QString("(%1 @ %2 already exists)").arg(name, tag));
		return;
	}
	ConfigView cfg = _cfg;
	ExtraLib e;
	e.tag = tag;
	e.path = QDir::toNativeSeparators(file);
	e.color = Pal::AutoTagBg(tag).name().toUpper();
	// The auto slot (basePort + 1000 + index) may already be taken by a port
	// override or another library. Pick the first free port instead.
	e.port = FirstFreeExtraPort(cfg);
	cfg.extraLibs << e;
	OnBusy(true);
	emit RequestSaveConfig(cfg);
}

// Show ready-to-paste MCP client config for every configured instance, in the two
// formats the user's tools expect: Claude Code's .mcp.json and Codex's config.toml.
// Both address the per-library streamable-HTTP servers (http://host:port/mcp), so
// the target library must be started (Server = UP) for a client to connect.
void MainWindow::ShowMcpData() {
	// Resolve (tag, name, port) for every instance with the same port formula the
	// worker uses (AutoScanPort / AutoExtraPort + per-instance overrides).
	struct Srv {
		QString tag, name;
		int port;
	};
	QVector<Srv> list;
	auto overrideFor = [this](const QString& tag, const QString& name) -> int {
		for (const PortOverride& po : _cfg.portOverrides)
			if (po.tag == tag && po.name == name) return po.port;
		return 0;
	};
	for (const Workspace& sd : AllWorkspaces(_cfg))
		for (int fileIndex = 0; fileIndex < sd.files.size(); ++fileIndex) {
			const QString& file = sd.files[fileIndex];
			const QString name = QFileInfo(file).completeBaseName();
			if (name.isEmpty()) continue;
			const int ov = overrideFor(sd.tag, name);
			const int port = ov > 0 ? ov : AutoScanPort(_cfg.basePort, fileIndex, sd.portOffset);
			list.push_back({sd.tag, name, port});
		}
	for (int i = 0; i < _cfg.extraLibs.size(); ++i) {
		const ExtraLib& e = _cfg.extraLibs[i];
		const QString name = QFileInfo(e.path).completeBaseName();
		if (e.tag.isEmpty() || name.isEmpty()) continue;
		const int port = e.port > 0 ? e.port : AutoExtraPort(_cfg.basePort, i);
		list.push_back({e.tag, name, port});
	}
	if (list.isEmpty()) {
		QMessageBox::information(this, "MCP data", "No libraries are configured yet. Add one first, then reopen MCP data.");
		return;
	}

	std::sort(list.begin(), list.end(), [](const Srv& a, const Srv& b) { return a.port < b.port; });
	const QString host = _cfg.host.trimmed().isEmpty() ? QStringLiteral("127.0.0.1") : _cfg.host.trimmed();
	struct Payload {
		QString claudeJson;
		QString codexToml;
		int serverCount = 0;
	};
	auto payloadFor = [&list, &host](const QString& selectedTag) {
		QVector<Srv> filtered;
		for (const Srv& server : list)
			if (selectedTag.isEmpty() || server.tag == selectedTag) filtered.push_back(server);

		QHash<QString, int> nameCount;
		for (const Srv& server : filtered) nameCount[server.name]++;
		QVector<QPair<QString, QString>> servers;
		for (const Srv& server : filtered) {
			QString key = "ida-mcp-" + server.name;
			if (nameCount.value(server.name) > 1) key += "-" + server.tag;
			servers.push_back({key, QString("http://%1:%2/mcp").arg(host).arg(server.port)});
		}

		QJsonObject mcpServers;
		QString codexToml;
		for (const auto& server : servers) {
			mcpServers[server.first] = QJsonObject{{"type", "http"}, {"url", server.second}};
			codexToml += QString("[mcp_servers.%1]\nurl = \"%2\"\n\n").arg(server.first, server.second);
		}
		Payload payload;
		payload.claudeJson = QString::fromUtf8(QJsonDocument(QJsonObject{{"mcpServers", mcpServers}}).toJson(QJsonDocument::Indented));
		payload.codexToml = codexToml.trimmed() + "\n";
		payload.serverCount = servers.size();
		return payload;
	};
	Payload payload = payloadFor(QString());

	QDialog dlg(this);
	dlg.setObjectName("appSurface");
	dlg.setWindowTitle("MCP data");
	dlg.resize(720, 620);
	auto* root = new QVBoxLayout(&dlg);
	root->setContentsMargins(24, 22, 24, 18);
	root->setSpacing(16);

	// Header: icon + title + one-line subtitle. The chip reuses the app-bar's
	// #iconChip style (hardcoded there because %primaryContainer% is not a token).
	auto* header = new QHBoxLayout;
	header->setSpacing(14);
	auto* hIcon = new QFrame;
	hIcon->setObjectName("iconChip");
	hIcon->setFixedSize(44, 44);
	auto* hIconLay = new QVBoxLayout(hIcon);
	hIconLay->setContentsMargins(0, 0, 0, 0);
	hIconLay->addWidget(Ui::IconLabel("dns", 22, Pal::ON_PRIMARY_CONTAINER), 0, Qt::AlignCenter);
	auto* titleBox = new QVBoxLayout;
	titleBox->setSpacing(2);
	auto* title = new QLabel("MCP client configuration");
	title->setObjectName("mcpTitle");
	auto* sub = new QLabel;
	sub->setObjectName("appSubtitle");
	sub->setTextFormat(Qt::RichText);
	sub->setWordWrap(true);
	auto updateSubtitle = [sub](int count) {
		sub->setText(QString("<b>%1 configured server%2</b> — start a library (Server = UP) before a client connects.")
			.arg(count).arg(count == 1 ? "" : "s"));
	};
	updateSubtitle(payload.serverCount);
	titleBox->addWidget(title);
	titleBox->addWidget(sub);
	header->addWidget(hIcon, 0, Qt::AlignVCenter);
	header->addLayout(titleBox, 1);
	auto* tagCombo = Ui::ComboBox();
	tagCombo->addItem("All", QString());
	QStringList tags;
	for (const Srv& server : list)
		if (!tags.contains(server.tag)) tags << server.tag;
	for (const QString& tag : tags) tagCombo->addItem(tag, tag);
	auto* tagField = Ui::ComboField("Tag", tagCombo);
	tagField->setFixedWidth(190);
	header->addWidget(tagField, 0, Qt::AlignVCenter);
	root->addLayout(header);

	// Per-tab payload the shared footer buttons (Copy / Save) act on.
	struct TabData {
		QString content, saveName, saveFilter;
	};
	QVector<TabData> tabData = {
		{payload.claudeJson, ".mcp.json", "JSON (*.json);;All files (*)"},
		{payload.codexToml, "config.toml", "TOML (*.toml);;All files (*)"},
	};

	// A bare QTabBar + QStackedWidget (the main window's tag-tabs pattern): the
	// pills float freely with no QTabWidget pane frame around the content.
	auto* tabBar = new QTabBar;
	tabBar->setDrawBase(false);
	tabBar->setExpanding(false);
	tabBar->setIconSize(QSize(16, 16));
	tabBar->setCursor(Qt::PointingHandCursor);
	auto* pages = new QStackedWidget;
	// (iconName, description, CLI hint or empty, content, highlighter mode)
	QVector<QPlainTextEdit*> editors;
	auto makeTab = [&editors](const QString& iconName, const QString& desc, const QString& cli,
					  const QString& content, CodeHighlighter::Mode mode) {
		auto* w = new QWidget;
		auto* v = new QVBoxLayout(w);
		v->setContentsMargins(0, 0, 0, 0);
		v->setSpacing(14);

		// Tinted info panel: icon + wrapped description (+ optional inline CLI code).
		auto* info = new QFrame;
		info->setObjectName("mcpInfo");
		auto* infoLay = new QHBoxLayout(info);
		infoLay->setContentsMargins(14, 12, 14, 12);
		infoLay->setSpacing(11);
		infoLay->addWidget(Ui::IconLabel(iconName, 18, Pal::ON_SECONDARY_CONTAINER), 0, Qt::AlignTop);
		auto* infoText = new QVBoxLayout;
		infoText->setSpacing(8);
		auto* descLbl = new QLabel(desc);
		descLbl->setObjectName("mcpInfoText");
		descLbl->setWordWrap(true);
		descLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
		infoText->addWidget(descLbl);
		if (!cli.isEmpty()) {
			auto* cliLbl = new QLabel(cli);
			cliLbl->setObjectName("mcpInlineCode");
			cliLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
			infoText->addWidget(cliLbl);
		}
		infoLay->addLayout(infoText, 1);
		v->addWidget(info);

		// Monospace, read-only, syntax-highlighted code block.
		auto* edit = new QPlainTextEdit(content);
		edit->setObjectName("mcpCode");
		edit->setReadOnly(true);
		edit->setLineWrapMode(QPlainTextEdit::NoWrap);
		new CodeHighlighter(edit->document(), mode); // owned by the document
		Ui::RoundedSurface(edit, 12.0, Pal::OUTLINE_VARIANT);
		Ui::OverlayVerticalScrollBar(edit);
		editors.push_back(edit);
		v->addWidget(edit, 1);
		return w;
	};

	pages->addWidget(makeTab("info", "Claude Code reads MCP servers from a .mcp.json file. Save it in your project root (project "
									 "scope — shareable), or merge the entries into ~/.claude.json for every project. Each server "
									 "uses the streamable-HTTP transport (\"type\": \"http\"). CLI equivalent:",
							 "claude mcp add --transport http <name> <url>", payload.claudeJson, CodeHighlighter::Json));
	tabBar->addTab("Claude · .mcp.json");
	pages->addWidget(makeTab("info", "Codex reads MCP servers from ~/.codex/config.toml. Add these [mcp_servers.<name>] tables (or "
									 "merge them with any you already have). The url field connects over streamable HTTP to a "
									 "Workbench server.",
							 QString(), payload.codexToml, CodeHighlighter::Toml));
	tabBar->addTab("Codex · config.toml");
	root->addWidget(tabBar, 0, Qt::AlignLeft);
	root->addWidget(pages, 1);

	// Check-mark the active tab (same cue the main window uses on its tag tabs).
	auto markActive = [tabBar] {
		for (int i = 0; i < tabBar->count(); ++i)
			tabBar->setTabIcon(i, i == tabBar->currentIndex() ? Ui::Icon("check", Pal::SEG_ACTIVE, 16) : QIcon());
	};
	connect(tabBar, &QTabBar::currentChanged, &dlg, [markActive, pages](int index) {
		pages->setCurrentIndex(index);
		markActive();
	});
	markActive();
	connect(tagCombo, &QComboBox::currentIndexChanged, &dlg,
		[&payload, &payloadFor, &tabData, &editors, tagCombo, updateSubtitle](int) {
			payload = payloadFor(tagCombo->currentData().toString());
			tabData[0].content = payload.claudeJson;
			tabData[1].content = payload.codexToml;
			editors[0]->setPlainText(payload.claudeJson);
			editors[1]->setPlainText(payload.codexToml);
			updateSubtitle(payload.serverCount);
		});

	// One footer bar acting on the active tab: Copy / Save on the left, Close right.
	auto* copyBtn = Ui::IconButton("content_copy", "Copy to clipboard"); // filled primary
	auto* saveBtn = Ui::IconButton("download", "Save as file…", "tonal");
	auto* closeBtn = Ui::IconButton("close", "Close", "outlined");
	for (QPushButton* b : {copyBtn, saveBtn, closeBtn}) Ui::ClipRounded(b, 20.0);
	auto* footer = new QHBoxLayout;
	footer->setSpacing(10);
	footer->addWidget(copyBtn);
	footer->addWidget(saveBtn);
	footer->addStretch();
	footer->addWidget(closeBtn);
	root->addLayout(footer);

	connect(copyBtn, &QPushButton::clicked, &dlg, [copyBtn, tabBar, &tabData] {
		QApplication::clipboard()->setText(tabData[tabBar->currentIndex()].content);
		copyBtn->setText("Copied ✓");
		QTimer::singleShot(1400, copyBtn, [copyBtn] { copyBtn->setText("Copy to clipboard"); });
	});
	connect(saveBtn, &QPushButton::clicked, &dlg, [&dlg, tabBar, &tabData] {
		const TabData& td = tabData[tabBar->currentIndex()];
		const QString path = QFileDialog::getSaveFileName(&dlg, "Save MCP config", QDir(QDir::homePath()).filePath(td.saveName), td.saveFilter);
		if (path.isEmpty()) return;
		QSaveFile f(path);
		const QByteArray bytes = td.content.toUtf8();
		if (!(f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit()))
			QMessageBox::warning(&dlg, "Save as file", "Could not write " + QDir::toNativeSeparators(path));
	});
	connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

	dlg.exec();
}
