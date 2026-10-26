#include "settings_panel.h"
#include "delegates.h"
#include "mcp_client.h"
#include "palette.h"
#include "ui_util.h"
#include "version.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QStyle>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

static QWidget* SectionHeader(const QString& iconName, const QString& title, const QString& subtitle) {
	auto* widget = new QWidget;
	auto* layout = new QHBoxLayout(widget);
	layout->setContentsMargins(0, 0, 0, 8);
	layout->setSpacing(8);
	layout->addWidget(Ui::IconLabel(iconName, 18, Pal::PRIMARY));
	auto* titleLabel = new QLabel(title);
	titleLabel->setStyleSheet(QString("font-size:14px; font-weight:600; color:%1; background:transparent;").arg(Pal::TITLE_TEXT.name()));
	layout->addWidget(titleLabel);
	if (!subtitle.isEmpty()) {
		auto* subtitleLabel = new QLabel(subtitle);
		subtitleLabel->setStyleSheet(QString("font-size:12px; color:%1; background:transparent;").arg(Pal::OUTLINE.name()));
		layout->addWidget(subtitleLabel);
	}
	layout->addStretch();
	return widget;
}

// Paint a workspace row's explicit colour as a swatch.
static void ApplyTagSwatch(QTableWidgetItem* item, const QString& tag, const QString& hexColor) {
	const QColor background = hexColor.isEmpty() ? Pal::AutoTagBg(tag) : QColor(hexColor);
	const QString storedColor = background.name().toUpper();
	item->setBackground(background);
	item->setForeground(Pal::PillFg(background));
	item->setData(Qt::UserRole, storedColor);
	item->setText(storedColor);
	item->setTextAlignment(Qt::AlignCenter);
	item->setFlags(item->flags() & ~Qt::ItemIsEditable);
	item->setToolTip("Click to pick a tag colour");
}

// Render a workspace row's Files cell: the tracked paths live in UserRole,
// the cell shows a count and lists them in the tooltip. Double-click opens the editor.
static void SetFilesCell(QTableWidgetItem* item, const QStringList& files) {
	item->setData(Qt::UserRole, files);
	item->setText(files.isEmpty() ? QStringLiteral("— none —") : QString("%1 file%2").arg(files.size()).arg(files.size() == 1 ? "" : "s"));
	item->setToolTip(files.isEmpty() ? "Double-click to add files" : files.join("\n"));
	item->setTextAlignment(Qt::AlignCenter);
	item->setFlags(item->flags() & ~Qt::ItemIsEditable);
}

// A Steam row's Depot cell keeps the whole DepotConfig in UserRole and shows the depot
// that will actually be downloaded: explicit, or the built-in one of a known game.
static DepotConfig DepotFromCell(const QTableWidgetItem* item) {
	const QVariantMap data = item ? item->data(Qt::UserRole).toMap() : QVariantMap();
	DepotConfig depot;
	depot.enabled = true;
	depot.appId = data.value("appId", 730).toInt();
	depot.os = data.value("os", "windows").toString();
	depot.depotId = data.value("depotId", 0).toInt();
	depot.pathPrefix = data.value("pathPrefix").toString();
	depot.requiresLogin = data.value("requiresLogin", false).toBool();
	depot.manifest = data.value("manifest").toString();
	return depot;
}

static void SetDepotCell(QTableWidgetItem* item, const DepotConfig& depot) {
	item->setData(Qt::UserRole, QVariantMap{{"appId", depot.appId}, {"os", depot.os}, {"depotId", depot.depotId}, {"pathPrefix", depot.pathPrefix}, {"requiresLogin", depot.requiresLogin}, {"manifest", depot.manifest}});
	const DepotLayout layout = DepotLayoutFor(depot);
	const QString os = depot.os.isEmpty() ? QStringLiteral("any OS") : depot.os;
	item->setText(layout.depotId > 0 ? QString("%1 · %2 / %3").arg(depot.appId).arg(layout.depotId).arg(os) : QString("%1 / %2 — set DepotID").arg(depot.appId).arg(os));
	item->setTextAlignment(Qt::AlignCenter);
	item->setFlags(item->flags() & ~Qt::ItemIsEditable);
	item->setToolTip(QString("AppID · DepotID / OS — double-click to edit\nFiles relative to: %1\nCurrent ManifestID: %2")
						 .arg(layout.pathPrefix.isEmpty() ? QStringLiteral("depot root") : layout.pathPrefix + "/", depot.manifest.isEmpty() ? QStringLiteral("none") : depot.manifest));
}

static QGroupBox* OutlinedField(const QString& label, QLineEdit* lineEdit) {
	auto* field = new QGroupBox(label);
	field->setObjectName("fieldBox");
	field->setMinimumHeight(48);
	auto* layout = new QVBoxLayout(field);
	layout->setContentsMargins(10, 2, 10, 2);
	layout->addWidget(lineEdit);
	return field;
}

static QWidget* BrowseRow(const QString& label, QLineEdit* lineEdit, QWidget* parent, bool isDirectory, const QString& dialogTitle) {
	auto* row = new QWidget(parent);
	auto* layout = new QHBoxLayout(row);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(10);
	auto* browseBtn = Ui::IconButton("folder", "Browse…", "tonal");
	Ui::ClipRounded(browseBtn, 20.0);
	layout->addWidget(OutlinedField(label, lineEdit), 1);
	layout->addWidget(browseBtn);
	QObject::connect(browseBtn, &QPushButton::clicked, parent, [lineEdit, isDirectory, dialogTitle, parent] {
		const QString pickedPath = isDirectory ? QFileDialog::getExistingDirectory(parent, dialogTitle, lineEdit->text()) : QFileDialog::getOpenFileName(parent, dialogTitle, lineEdit->text());
		if (!pickedPath.isEmpty()) {
			lineEdit->setText(QDir::toNativeSeparators(pickedPath));
		}
	});
	return row;
}

static void WatchPath(QLineEdit* lineEdit, bool isFile) {
	auto checkValidity = [lineEdit, isFile] {
		const QString text = lineEdit->text().trimmed();
		const bool ok = text.isEmpty() || (isFile ? QFileInfo(text).isFile() : QFileInfo(text).isDir());
		lineEdit->setProperty("invalid", !ok);
		lineEdit->style()->unpolish(lineEdit);
		lineEdit->style()->polish(lineEdit);
		QWidget* field = lineEdit->parentWidget();
		while (field && field->objectName() != "fieldBox") {
			field = field->parentWidget();
		}
		if (field) {
			field->setProperty("invalid", !ok);
			field->style()->unpolish(field);
			field->style()->polish(field);
		}
	};
	QObject::connect(lineEdit, &QLineEdit::textChanged, lineEdit, [checkValidity](const QString&) { checkValidity(); });
	checkValidity();
}

static QHBoxLayout* BtnRow(std::initializer_list<QPushButton*> buttons) {
	auto* layout = new QHBoxLayout;
	for (QPushButton* button : buttons) {
		layout->addWidget(button);
	}
	layout->addStretch();
	return layout;
}

SettingsPanel::SettingsPanel(const ConfigView& v, QWidget* parent) :
	QWidget(parent) {
	setObjectName("appSurface");
	_configPath = v.configPath; // info only — the file location is not editable
	_uvx = new QLineEdit(v.uvx);
	_gui = new QLineEdit(v.idaGui);
	_text = new QLineEdit(v.idaText);
	_logDir = new QLineEdit(v.logDir);
	_analysisArgs = new QLineEdit(v.analysisArgs);
	_maxLog = new QLineEdit(QString::number(v.maxLogMB));
	_maxLog->setValidator(new QIntValidator(0, 10240, _maxLog));
	_depotExecutable = new QLineEdit(v.depotDownloader.executable);
	_depotTimeout = new QLineEdit(QString::number(v.depotDownloader.timeoutMinutes));
	_depotTimeout->setValidator(new QIntValidator(1, 240, _depotTimeout));

	// --- IDA installation ---
	auto* idaBox = new QGroupBox();
	idaBox->setObjectName("settingsCard");
	auto* idaLay = new QVBoxLayout(idaBox);
	idaLay->setContentsMargins(0, 0, 0, 0);
	idaLay->setSpacing(10);
	idaLay->addWidget(SectionHeader("folder_open", "IDA", "— the disassembler install and how it analyzes"));
	idaLay->addWidget(BrowseRow("IDA GUI (ida.exe)", _gui, this, false, "Select ida.exe"));
	idaLay->addWidget(BrowseRow("IDA text (idat.exe)", _text, this, false, "Select idat.exe"));
	auto* detectBtn = Ui::IconButton("manage_search", "Detect IDA", "tonal");
	Ui::ClipRounded(detectBtn, 20.0);
	detectBtn->setToolTip("Search the usual install locations for ida / idat and fill the paths above");
	idaLay->addLayout(BtnRow({detectBtn}));
	connect(detectBtn, &QPushButton::clicked, this, [this] {
		QString guiPath;
		QString textPath;
		if (AutoDetectIda(&guiPath, &textPath)) {
			_gui->setText(guiPath);
			_text->setText(textPath);
		} else {
			QMessageBox::information(this, "Detect IDA", "No IDA install was found in the usual locations. Set the paths with Browse…");
		}
	});
	idaLay->addWidget(OutlinedField("IDA analysis extra args", _analysisArgs));
	idaLay->addWidget(Ui::Hint("Optional extra loader/processor switches; empty is the recommended default. "
							   "Workbench adds -A, its analysis-completion script, output database (-o), and input binary. "
							   "-B is removed because it changes AF_DODATA and generates a large .asm listing."));
	WatchPath(_gui, true);
	WatchPath(_text, true);

	// --- MCP servers ---
	auto* srvBox = new QGroupBox();
	srvBox->setObjectName("settingsCard");
	auto* srvLay = new QVBoxLayout(srvBox);
	srvLay->setContentsMargins(0, 0, 0, 0);
	srvLay->setSpacing(10);
	srvLay->addWidget(SectionHeader("dns", "Official Hex-Rays MCP", "— IDA 9.4 or newer"));
	srvLay->addWidget(BrowseRow("uvx executable", _uvx, this, false, "Select uvx executable"));
	auto* uvHelp = Ui::Hint(QString("uvx is included with uv and is required to launch the IDA MCP server.\n"
									"Install uv in PowerShell: %1\n"
									"Then restart Workbench, or select uvx.exe above and save.")
								.arg(McpClient::UV_INSTALL_COMMAND));
	uvHelp->setTextInteractionFlags(Qt::TextSelectableByMouse);
	srvLay->addWidget(uvHelp);
	auto* copyUvInstall = Ui::IconButton("content_copy", "Copy uv install command", "tonal");
	connect(copyUvInstall, &QPushButton::clicked, this, [copyUvInstall] {
		QApplication::clipboard()->setText(McpClient::UV_INSTALL_COMMAND);
		copyUvInstall->setText("Copied");
	});
	auto* uvGuide = Ui::IconButton("open_in_new", "uv installation guide", "text");
	connect(uvGuide, &QPushButton::clicked, this, [] {
		QDesktopServices::openUrl(QUrl("https://docs.astral.sh/uv/getting-started/installation/"));
	});
	srvLay->addLayout(BtnRow({copyUvInstall, uvGuide}));
	srvLay->addWidget(Ui::Hint(QString("Workbench and your agents each launch the official server automatically. "
									   "IDA Nexus shares open databases between them. Install the GUI plugin once with: %1")
								   .arg(McpClient::PLUGIN_INSTALL_COMMAND)));

	// Card 3: Folder Workspaces
	auto* dirBox = new QGroupBox();
	dirBox->setObjectName("settingsCard");
	auto* dirLay = new QVBoxLayout(dirBox);
	dirLay->setContentsMargins(0, 0, 0, 0);
	dirLay->setSpacing(10);
	dirLay->addWidget(SectionHeader("folder_special", "Folder workspaces", "— explicit Source folder + Output folder pairs for local builds"));

	_workspaces = new QTableWidget(0, 5);
	_workspaces->setHorizontalHeader(new TrailingInsetHeader(Qt::Horizontal, _workspaces));
	_workspaces->setHorizontalHeaderLabels({"Tag", "Source root", "Output folder", "Files", "Color"});
	_workspaces->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	_workspaces->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
	_workspaces->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
	_workspaces->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
	_workspaces->setColumnWidth(3, 90);
	_workspaces->setColumnWidth(4, 110);
	_workspaces->verticalHeader()->setVisible(false);
	_workspaces->horizontalHeader()->setFixedHeight(40);
	_workspaces->setMinimumHeight(120);
	_workspaces->verticalHeader()->setDefaultSectionSize(40);

	connect(_workspaces, &QTableWidget::cellClicked, this, [this](int r, int c) {
		if (c == 3) {
			EditFiles(_workspaces, r);
		} else if (c == 4) {
			PickTagColor(_workspaces, r, c);
		}
	});

	for (const Workspace& w : v.workspaces) {
		AddWorkspaceRow(w);
	}

	auto* dirAdd = Ui::IconButton("create_new_folder", "Add folder workspace…", "tonal");
	auto* dirDel = Ui::IconButton("delete", "Remove selected", "text");
	dirLay->addWidget(_workspaces);
	dirLay->addLayout(BtnRow({dirAdd, dirDel}));
	connect(dirAdd, &QPushButton::clicked, this, &SettingsPanel::AddWorkspace);
	connect(dirDel, &QPushButton::clicked, this, [this] {
		if (_workspaces->currentRow() >= 0) {
			_workspaces->removeRow(_workspaces->currentRow());
		}
	});

	// Card 4: Steam — optional. Only Steam workspaces and DepotDownloader live here, so the
	// rest of Settings reads as a plain binary workbench.
	auto* steamBox = new QGroupBox();
	steamBox->setObjectName("settingsCard");
	auto* steamLay = new QVBoxLayout(steamBox);
	steamLay->setContentsMargins(0, 0, 0, 0);
	steamLay->setSpacing(10);
	steamLay->addWidget(SectionHeader("download", "Steam games", "— optional: binaries of any Steam app, downloaded by depot"));
	steamLay->addWidget(Ui::Hint("Each workspace is one depot of one app: its AppID and DepotID (both listed on SteamDB) plus the files to track. "
								 "Every build is kept in its own ManifestID folder. Counter-Strike 2 and CS:GO fill in their depots automatically. "
								 "Folder workspaces and libraries never need Steam."));

	_steamWorkspaces = new QTableWidget(0, 5);
	_steamWorkspaces->setHorizontalHeader(new TrailingInsetHeader(Qt::Horizontal, _steamWorkspaces));
	_steamWorkspaces->setHorizontalHeaderLabels({"Tag", "Workspace dir", "Files", "Depot", "Color"});
	_steamWorkspaces->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	_steamWorkspaces->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
	_steamWorkspaces->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
	_steamWorkspaces->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
	_steamWorkspaces->setColumnWidth(2, 90);
	_steamWorkspaces->setColumnWidth(3, 200);
	_steamWorkspaces->setColumnWidth(4, 110);
	_steamWorkspaces->verticalHeader()->setVisible(false);
	_steamWorkspaces->horizontalHeader()->setFixedHeight(40);
	_steamWorkspaces->setMinimumHeight(120);
	_steamWorkspaces->verticalHeader()->setDefaultSectionSize(40);

	connect(_steamWorkspaces, &QTableWidget::cellClicked, this, [this](int r, int c) {
		if (c == 2) {
			EditFiles(_steamWorkspaces, r);
		} else if (c == 4) {
			PickTagColor(_steamWorkspaces, r, c);
		}
	});
	connect(_steamWorkspaces, &QTableWidget::cellDoubleClicked, this, [this](int r, int c) {
		if (c == 3) {
			EditDepot(r);
		}
	});

	for (const Workspace& w : v.steamWorkspaces) {
		AddSteamWorkspaceRow(w);
	}

	auto* steamAdd = Ui::IconButton("create_new_folder", "Add Steam workspace…", "tonal");
	auto* steamDel = Ui::IconButton("delete", "Remove selected", "text");
	steamLay->addWidget(_steamWorkspaces);
	steamLay->addLayout(BtnRow({steamAdd, steamDel}));
	// One field per setting: every widget is created once above, with its validator, and shown
	// exactly once. Re-creating them here (as an earlier layout did) left the first copies on
	// screen but unreachable from Values() — edits in them were silently dropped, and the
	// validators went with the discarded pointers.
	auto* depotLay = new QHBoxLayout;
	depotLay->addWidget(BrowseRow("DepotDownloader", _depotExecutable, this, false, "Select DepotDownloader executable"), 1);
	auto* timeoutField = OutlinedField("Timeout (minutes)", _depotTimeout);
	timeoutField->setFixedWidth(160);
	depotLay->addWidget(timeoutField);
	steamLay->addLayout(depotLay);
	steamLay->addWidget(Ui::Hint("Left as DepotDownloader.exe, it is installed automatically on the first download."));
	connect(steamAdd, &QPushButton::clicked, this, &SettingsPanel::AddSteamWorkspace);
	connect(steamDel, &QPushButton::clicked, this, [this] {
		if (_steamWorkspaces->currentRow() >= 0) {
			_steamWorkspaces->removeRow(_steamWorkspaces->currentRow());
		}
	});

	// Card 5: Extra Binaries
	auto* extraBox = new QGroupBox();
	extraBox->setObjectName("settingsCard");
	auto* extraLay = new QVBoxLayout(extraBox);
	extraLay->setContentsMargins(0, 0, 0, 0);
	extraLay->setSpacing(10);
	extraLay->addWidget(SectionHeader("library_books", "Libraries", "— individual binaries grouped by tag"));
	_extra = new QTableWidget(0, 3);
	_extra->setHorizontalHeader(new TrailingInsetHeader(Qt::Horizontal, _extra));
	_extra->setHorizontalHeaderLabels({"Tag", "Binary path", "Color"});
	_extra->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	_extra->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
	_extra->setColumnWidth(2, 110);
	_extra->verticalHeader()->setVisible(false);
	_extra->horizontalHeader()->setFixedHeight(40);
	_extra->setMinimumHeight(110);
	_extra->verticalHeader()->setDefaultSectionSize(40);
	connect(_extra, &QTableWidget::cellClicked, this, [this](int r, int c) {
		if (c == 2) {
			PickTagColor(_extra, r, c);
		}
	});
	for (const ExtraLib& e : v.extraLibs) {
		AddExtraRow(e);
	}
	auto* extraAdd = Ui::IconButton("note_add", "Add file…", "tonal");
	auto* extraDel = Ui::IconButton("delete", "Remove selected", "text");
	extraLay->addWidget(Ui::Hint("The simplest way to serve one binary: point at the file and give it a tag; the file "
								 "name becomes the module name. \"Add binary…\" on the main page adds rows here too. "
								 "Use a folder workspace or Steam workspace below for explicit multi-file builds with stored versions."));
	extraLay->addWidget(_extra);
	extraLay->addLayout(BtnRow({extraAdd, extraDel}));
	connect(extraAdd, &QPushButton::clicked, this, &SettingsPanel::AddExtraFile);
	connect(extraDel, &QPushButton::clicked, this, [this] {
		if (_extra->currentRow() >= 0) {
			_extra->removeRow(_extra->currentRow());
		}
	});

	// --- config file & logs (housekeeping: import/export, backups, log cap) ---
	auto* cfgBox = new QGroupBox();
	cfgBox->setObjectName("settingsCard");
	auto* cfgLay = new QVBoxLayout(cfgBox);
	cfgLay->setContentsMargins(0, 0, 0, 0);
	cfgLay->setSpacing(10);
	cfgLay->addWidget(SectionHeader("settings", "Config & logs", "— import / export and the app log cap"));
	auto* cfgWhere = Ui::Hint(QString("Config file: %1 — always kept in your user folder next to the log file "
									  "and the helper scripts, so it survives app updates.")
								  .arg(QDir::toNativeSeparators(_configPath)));
	cfgWhere->setTextInteractionFlags(Qt::TextSelectableByMouse);
	cfgLay->addWidget(cfgWhere);
	auto* importBtn = Ui::IconButton("folder_open", "Import config…", "tonal");
	auto* exportBtn = Ui::IconButton("difference", "Export config…", "tonal");
	importBtn->setToolTip("Replace the whole configuration with a config.json from disk");
	exportBtn->setToolTip("Copy the saved config.json to a file (unsaved edits are not included)");
	Ui::ClipRounded(importBtn, 20.0);
	Ui::ClipRounded(exportBtn, 20.0);
	cfgLay->addLayout(BtnRow({importBtn, exportBtn}));
	connect(importBtn, &QPushButton::clicked, this, &SettingsPanel::ImportConfigFile);
	connect(exportBtn, &QPushButton::clicked, this, &SettingsPanel::ExportConfigFile);
	auto* logRow = new QHBoxLayout;
	auto* logCapField = OutlinedField("Max log size (MB)", _maxLog);
	logCapField->setFixedWidth(220);
	logRow->addWidget(logCapField);
	logRow->addWidget(BrowseRow("Log dir", _logDir, this, true, "Select log directory"), 1);
	cfgLay->addLayout(logRow);
	cfgLay->addWidget(Ui::Hint("Everything the LOG panel shows — plus the MCP server output and verbose diagnostics — "
							   "goes to one file (ida-workbench.log); the oldest lines are trimmed so it never exceeds "
							   "the cap. 0 = unlimited. Use Import config… to bring in a config.json from elsewhere; "
							   "Export config… to copy the current one out."));

	// --- About ---
	auto* aboutBox = new QGroupBox();
	aboutBox->setObjectName("settingsCard");
	auto* aboutLay = new QVBoxLayout(aboutBox);
	aboutLay->setContentsMargins(0, 0, 0, 0);
	aboutLay->setSpacing(10);
	aboutLay->addWidget(SectionHeader("info", "About", "— version and author"));
	auto* about = new QLabel(QString("<span style='font-size:13px;'>IDA Workbench <b>v%1</b></span><br>"
									 "Created by <a href=\"https://t.me/sazonische\">sazonische</a> — "
									 "<a href=\"https://t.me/sazonische\">https://t.me/sazonische</a>")
								 .arg(APP_VERSION));
	about->setObjectName("aboutText");
	about->setTextFormat(Qt::RichText);
	about->setOpenExternalLinks(true);
	about->setTextInteractionFlags(Qt::TextBrowserInteraction);
	aboutLay->addWidget(about);

	// Apply the same clip + topmost-outline model used by the main table.
	// QAbstractItemView's viewport and headers otherwise cover rounded borders.
	Ui::RoundedOutline(idaBox, 16.0, Pal::OUTLINE_VARIANT);
	Ui::RoundedOutline(srvBox, 16.0, Pal::OUTLINE_VARIANT);
	Ui::RoundedOutline(dirBox, 16.0, Pal::OUTLINE_VARIANT);
	Ui::RoundedOutline(steamBox, 16.0, Pal::OUTLINE_VARIANT);
	Ui::RoundedOutline(extraBox, 16.0, Pal::OUTLINE_VARIANT);
	Ui::RoundedOutline(cfgBox, 16.0, Pal::OUTLINE_VARIANT);
	Ui::RoundedOutline(aboutBox, 16.0, Pal::OUTLINE_VARIANT);
	Ui::RoundedSurface(_workspaces, 12.0, Pal::OUTLINE_VARIANT);
	Ui::RoundedSurface(_steamWorkspaces, 12.0, Pal::OUTLINE_VARIANT);
	Ui::RoundedSurface(_extra, 12.0, Pal::OUTLINE_VARIANT);
	Ui::OverlayVerticalScrollBar(_workspaces, _workspaces->horizontalHeader()->height() + 2);
	Ui::OverlayVerticalScrollBar(_steamWorkspaces, _steamWorkspaces->horizontalHeader()->height() + 2);
	Ui::OverlayVerticalScrollBar(_extra, _extra->horizontalHeader()->height() + 2);
	_workspaces->setItemDelegateForColumn(4, new TrailingInsetDelegate(_workspaces));
	_steamWorkspaces->setItemDelegateForColumn(4, new TrailingInsetDelegate(_steamWorkspaces));
	_extra->setItemDelegateForColumn(2, new TrailingInsetDelegate(_extra));

	for (QPushButton* button : {dirAdd, dirDel, steamAdd, steamDel, extraAdd, extraDel}) {
		Ui::ClipRounded(button, 20.0);
	}

	// --- scroll body ---
	auto* body = new QWidget;
	auto* bodyLay = new QVBoxLayout(body);
	bodyLay->setContentsMargins(24, 4, 24, 16);
	bodyLay->setSpacing(16);
	bodyLay->addWidget(Ui::Hint("Tags identify builds in the library list; for folder workspaces the Sync column flags "
								"whether each working copy still matches its configured Source (by content hash). "
								"A red border marks a missing file."));
	bodyLay->addWidget(idaBox);
	bodyLay->addWidget(srvBox);
	bodyLay->addWidget(extraBox);
	bodyLay->addWidget(dirBox);
	bodyLay->addWidget(steamBox);
	bodyLay->addWidget(cfgBox);
	bodyLay->addWidget(aboutBox);
	bodyLay->addStretch();

	auto* scroll = new QScrollArea;
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidget(body);
	Ui::OverlayVerticalScrollBar(scroll);

	// --- footer: Cancel / Save (import/export live in the Config & logs card) ---
	auto* cancelBtn = Ui::IconButton("close", "Cancel", "outlined");
	auto* saveBtn = Ui::IconButton("check", "Save", nullptr, false);
	Ui::ClipRounded(cancelBtn, 20.0);
	Ui::ClipRounded(saveBtn, 20.0);
	auto* footerBar = new QFrame;
	footerBar->setObjectName("settingsFooter");
	auto* footer = new QHBoxLayout(footerBar);
	footer->setContentsMargins(24, 14, 24, 20);
	footer->setSpacing(10);
	footer->addStretch();
	footer->addWidget(cancelBtn);
	footer->addWidget(saveBtn);
	connect(cancelBtn, &QPushButton::clicked, this, [this] { emit Cancelled(); });
	connect(saveBtn, &QPushButton::clicked, this, [this] { emit Saved(Values()); });

	auto* rootLay = new QVBoxLayout(this);
	rootLay->setContentsMargins(0, 0, 0, 0);
	rootLay->setSpacing(0);
	rootLay->addWidget(scroll, 1);
	rootLay->addWidget(footerBar);
}

void SettingsPanel::AddWorkspaceRow(const Workspace& workspace) {
	const int r = _workspaces->rowCount();
	_workspaces->insertRow(r);
	_workspaces->setItem(r, 0, new QTableWidgetItem(workspace.tag));
	_workspaces->setItem(r, 1, new QTableWidgetItem(workspace.source));
	_workspaces->setItem(r, 2, new QTableWidgetItem(workspace.output));
	auto* files = new QTableWidgetItem;
	SetFilesCell(files, workspace.files);
	_workspaces->setItem(r, 3, files);
	auto* col = new QTableWidgetItem;
	ApplyTagSwatch(col, workspace.tag, workspace.color.isEmpty() ? Pal::AutoTagBg(workspace.tag).name() : workspace.color);
	_workspaces->setItem(r, 4, col);
}

void SettingsPanel::AddSteamWorkspaceRow(const Workspace& workspace) {
	const int r = _steamWorkspaces->rowCount();
	_steamWorkspaces->insertRow(r);
	_steamWorkspaces->setItem(r, 0, new QTableWidgetItem(workspace.tag));
	_steamWorkspaces->setItem(r, 1, new QTableWidgetItem(workspace.output));
	auto* files = new QTableWidgetItem;
	SetFilesCell(files, workspace.files);
	_steamWorkspaces->setItem(r, 2, files);
	auto* depot = new QTableWidgetItem;
	SetDepotCell(depot, workspace.depot);
	_steamWorkspaces->setItem(r, 3, depot);
	auto* col = new QTableWidgetItem;
	ApplyTagSwatch(col, workspace.tag, workspace.color.isEmpty() ? Pal::AutoTagBg(workspace.tag).name() : workspace.color);
	_steamWorkspaces->setItem(r, 4, col);
}

// Takes the table it was clicked in: three tables share this, and hardcoding one of them
// silently repainted the wrong workspace's colour (and did nothing at all for rows the
// hardcoded table did not have).
void SettingsPanel::PickTagColor(QTableWidget* table, int row, int column) {
	auto* item = table->item(row, column);
	if (!item) {
		return;
	}
	const QString tag = table->item(row, 0) ? table->item(row, 0)->text() : QString();
	const QString currentHex = item->data(Qt::UserRole).toString();
	const QColor initialColor = currentHex.isEmpty() ? Pal::AutoTagBg(tag) : QColor(currentHex);
	const QColor selectedColor = QColorDialog::getColor(initialColor, this, "Tag colour");
	if (!selectedColor.isValid()) {
		return;
	}
	ApplyTagSwatch(item, tag, selectedColor.name());
}

void SettingsPanel::EditDepot(int row) {
	auto* item = _steamWorkspaces->item(row, 3);
	if (!item) {
		return;
	}
	const DepotConfig old = DepotFromCell(item);
	QDialog dialog(this);
	dialog.setWindowTitle("Steam depot");
	dialog.setMinimumWidth(560);
	auto* layout = new QVBoxLayout(&dialog);
	auto* appId = new QLineEdit(QString::number(old.appId));
	appId->setValidator(new QIntValidator(1, 2147483647, appId));
	auto* depotId = new QLineEdit(old.depotId > 0 ? QString::number(old.depotId) : QString());
	depotId->setValidator(new QIntValidator(1, 2147483647, depotId));
	depotId->setPlaceholderText("built-in for CS2 / CS:GO");
	auto* prefix = new QLineEdit(old.pathPrefix);
	prefix->setPlaceholderText("depot root");
	auto* osCombo = Ui::ComboBox();
	for (const QString& os : {QStringLiteral("windows"), QStringLiteral("linux"), QStringLiteral("macos")}) {
		osCombo->addItem(os, os);
	}
	osCombo->addItem("any", QString());
	osCombo->setCurrentIndex(qMax(0, osCombo->findData(old.os)));
	auto* login = new QCheckBox("Needs a Steam account that owns the app (most paid games)");
	login->setChecked(old.requiresLogin);

	auto* appRow = new QHBoxLayout;
	appRow->addWidget(OutlinedField("App ID", appId), 1);
	appRow->addWidget(Ui::ComboField("Depot OS", osCombo), 1);
	layout->addLayout(appRow);
	auto* depotRow = new QHBoxLayout;
	depotRow->addWidget(OutlinedField("Depot ID", depotId), 1);
	depotRow->addWidget(OutlinedField("Folder inside the depot", prefix), 1);
	layout->addLayout(depotRow);
	layout->addWidget(login);
	auto* resolved = Ui::Hint(QString());
	layout->addWidget(resolved);

	auto* footer = new QHBoxLayout;
	auto* steamDb = Ui::IconButton("manage_search", "Find depots on SteamDB", "text");
	footer->addWidget(steamDb);
	footer->addStretch();
	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
	footer->addWidget(buttons);
	layout->addLayout(footer);

	auto edited = [&old, appId, depotId, prefix, osCombo, login] {
		DepotConfig depot = old;
		depot.appId = appId->text().toInt();
		depot.os = osCombo->currentData().toString();
		depot.depotId = depotId->text().toInt();
		depot.pathPrefix = QDir::fromNativeSeparators(prefix->text().trimmed());
		depot.requiresLogin = login->isChecked();
		return depot;
	};
	// Say which depot a download will really use, before Save rather than after a failed run.
	auto refresh = [edited, resolved, buttons] {
		const DepotConfig depot = edited();
		const DepotLayout layout = DepotLayoutFor(depot);
		if (depot.appId <= 0 || layout.depotId <= 0) {
			resolved->setText("Enter the Depot ID that holds the files — SteamDB lists each app's depots with their OS.");
		} else {
			resolved->setText(QString("Downloads depot %1%2; files are listed relative to %3.%4")
								  .arg(layout.depotId)
								  .arg(depot.depotId > 0 ? QString() : QStringLiteral(" (built-in)"),
									   layout.pathPrefix.isEmpty() ? QStringLiteral("the depot root") : layout.pathPrefix + "/",
									   layout.accountRequired ? QStringLiteral(" Downloads ask for a Steam login.") : QString()));
		}
		buttons->button(QDialogButtonBox::Save)->setEnabled(depot.appId > 0 && layout.depotId > 0);
	};
	for (QLineEdit* input : {appId, depotId, prefix}) {
		connect(input, &QLineEdit::textChanged, &dialog, [refresh](const QString&) { refresh(); });
	}
	connect(osCombo, &QComboBox::currentIndexChanged, &dialog, [refresh](int) { refresh(); });
	connect(login, &QCheckBox::toggled, &dialog, [refresh](bool) { refresh(); });
	connect(steamDb, &QPushButton::clicked, &dialog, [appId] {
		QDesktopServices::openUrl(QUrl(QString("https://steamdb.info/app/%1/depots/").arg(appId->text().toInt())));
	});
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	refresh();
	if (dialog.exec() != QDialog::Accepted) {
		return;
	}
	DepotConfig depot = edited();
	const DepotLayout before = DepotLayoutFor(old);
	const DepotLayout after = DepotLayoutFor(depot);
	// A different depot or folder makes the stored Current pointer meaningless.
	if (depot.appId != old.appId || before.depotId != after.depotId || before.pathPrefix != after.pathPrefix) {
		depot.manifest.clear();
	}
	SetDepotCell(item, depot);
}

void SettingsPanel::AddExtraRow(const ExtraLib& extraLib) {
	const int r = _extra->rowCount();
	_extra->insertRow(r);
	_extra->setItem(r, 0, new QTableWidgetItem(extraLib.tag));
	_extra->setItem(r, 1, new QTableWidgetItem(extraLib.path));
	auto* col = new QTableWidgetItem;
	ApplyTagSwatch(col, extraLib.tag, extraLib.color);
	_extra->setItem(r, 2, col);
}

// Edit the relative file paths of one workspace.
void SettingsPanel::EditFiles(QTableWidget* table, int row) {
	const int filesColumn = table == _steamWorkspaces ? 2 : 3;
	const bool isSteam = table == _steamWorkspaces;
	auto* cell = table->item(row, filesColumn);
	if (!cell) {
		return;
	}
	const QString tag = table->item(row, 0) ? table->item(row, 0)->text().trimmed() : QString();
	const QString source = table->item(row, 1) ? table->item(row, 1)->text().trimmed() : QString();

	QDialog dialog(this);
	dialog.setWindowTitle(QString("Files for \"%1\"").arg(tag));
	dialog.resize(460, 460);
	auto* layout = new QVBoxLayout(&dialog);
	auto* list = new QListWidget;
	Ui::RoundedSurface(list, 10.0, Pal::OUTLINE_VARIANT);
	Ui::OverlayVerticalScrollBar(list);
	// A wrong path is usually a typo or a renamed module, so it is fixed in place:
	// double-click (or F2) edits the entry instead of forcing a remove/re-add. A plain
	// click on the selected row must not start an edit — the next click is often Remove.
	list->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
	auto addFile = [list](const QString& path) {
		auto* item = new QListWidgetItem(path, list);
		item->setFlags(item->flags() | Qt::ItemIsEditable);
		return item;
	};
	for (const QString& fileName : cell->data(Qt::UserRole).toStringList()) {
		if (!fileName.trimmed().isEmpty()) {
			addFile(fileName.trimmed());
		}
	}

	auto getPresentFiles = [list] {
		QSet<QString> set;
		for (int i = 0; i < list->count(); ++i) {
			set.insert(list->item(i)->text().trimmed().toLower());
		}
		return set;
	};
	QPushButton* scanBtn = isSteam ? nullptr : Ui::IconButton("folder_open", "Choose files…", "tonal");
	auto* addBtn = Ui::IconButton("add", "Add relative path", "text");
	auto* delBtn = Ui::IconButton("delete", "Remove", "text");
	if (scanBtn) {
		Ui::ClipRounded(scanBtn, 20.0);
	}
	Ui::ClipRounded(addBtn, 20.0);
	Ui::ClipRounded(delBtn, 20.0);
	auto* buttonsLayout = new QHBoxLayout;
	if (scanBtn) {
		buttonsLayout->addWidget(scanBtn);
	}
	buttonsLayout->addWidget(addBtn);
	buttonsLayout->addWidget(delBtn);
	buttonsLayout->addStretch();

	connect(addBtn, &QPushButton::clicked, &dialog, [&dialog, list, isSteam, addFile] {
		bool ok = false;
		const QString pathText = QInputDialog::getText(&dialog, "Add file", isSteam ? "Path relative to the depot folder:" : "Path relative to Source:", QLineEdit::Normal, QString(), &ok).trimmed();
		if (ok && !pathText.isEmpty()) {
			list->setCurrentItem(addFile(pathText));
		}
	});
	connect(delBtn, &QPushButton::clicked, &dialog, [list] { delete list->currentItem(); });
	if (scanBtn) {
		connect(scanBtn, &QPushButton::clicked, &dialog, [&dialog, source, getPresentFiles, addFile] {
			const QStringList selected = QFileDialog::getOpenFileNames(&dialog, "Choose source files", source, "All files (*)");
			if (selected.isEmpty()) {
				return;
			}
			QSet<QString> existingFiles = getPresentFiles();
			for (const QString& filePath : selected) {
				const QString relative = QDir(source).relativeFilePath(filePath);
				if (!relative.startsWith("../") && !QDir::isAbsolutePath(relative) && !existingFiles.contains(relative.toLower())) {
					addFile(QDir::cleanPath(relative));
					existingFiles.insert(relative.toLower());
				}
			}
		});
	}

	auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
	for (QAbstractButton* button : buttonBox->buttons()) {
		Ui::ClipRounded(button, 20.0);
	}
	connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	layout->addWidget(Ui::Hint(QString(isSteam ? "Paths are relative to the depot folder shown in the Depot column and preserved under each ManifestID." : "Files tracked under \"%1\". Every path is relative to Source and is preserved under Output and revisions.").arg(tag)));
	layout->addWidget(list, 1);
	layout->addLayout(buttonsLayout);
	layout->addWidget(buttonBox);
	if (dialog.exec() != QDialog::Accepted) {
		return;
	}

	QStringList resultFiles;
	for (int i = 0; i < list->count(); ++i) {
		const QString itemText = list->item(i)->text().trimmed();
		if (!itemText.isEmpty() && !resultFiles.contains(itemText)) {
			resultFiles << itemText;
		}
	}
	SetFilesCell(cell, resultFiles);
}

void SettingsPanel::ImportConfigFile() {
	const QString file = QFileDialog::getOpenFileName(this, "Import config", QString(), "JSON config (*.json);;All files (*)");
	if (file.isEmpty()) {
		return;
	}
	const auto buttonPicked = QMessageBox::warning(this, "Import config", QString("Replace the whole configuration with\n%1?\n\n"
																				  "If the file is invalid the current configuration is kept.")
																			  .arg(QDir::toNativeSeparators(file)),
												   QMessageBox::Ok | QMessageBox::Cancel);
	if (buttonPicked == QMessageBox::Ok) {
		emit ImportRequested(file);
	}
}

void SettingsPanel::ExportConfigFile() {
	const QString file = QFileDialog::getSaveFileName(
		this, "Export config", QDir(QDir::homePath()).filePath("ida-workbench-config.json"),
		"JSON config (*.json);;All files (*)"
	);
	if (file.isEmpty()) {
		return;
	}
	// Exports the file as last saved — pending edits in this panel are not included.
	if (QFile::exists(file) && !QFile::remove(file)) {
		QMessageBox::warning(this, "Export config", "Cannot overwrite " + QDir::toNativeSeparators(file));
		return;
	}
	if (QFile::copy(_configPath, file)) {
		QMessageBox::information(this, "Export config", "Saved configuration exported to\n" + QDir::toNativeSeparators(file));
	} else {
		QMessageBox::warning(this, "Export config", "Could not write " + QDir::toNativeSeparators(file));
	}
}

void SettingsPanel::AddWorkspace() {
	const QString output = QFileDialog::getExistingDirectory(this, "Choose the output folder");
	if (output.isEmpty()) {
		return;
	}
	const QString sourceStart = QFileInfo(output).absolutePath();
	const QString source = QFileDialog::getExistingDirectory(this, "Choose the source root", sourceStart);
	if (source.isEmpty()) {
		return;
	}
	Workspace workspace;
	workspace.tag = QFileInfo(source).fileName();
	workspace.source = QDir::toNativeSeparators(source);
	workspace.output = QDir::toNativeSeparators(output);
	workspace.color = Pal::AutoTagBg(workspace.tag).name();
	AddWorkspaceRow(workspace);
}

void SettingsPanel::AddSteamWorkspace() {
	const QString dir = QFileDialog::getExistingDirectory(this, "Choose the Steam workspace directory");
	if (dir.isEmpty()) {
		return;
	}
	Workspace workspace;
	workspace.tag = QFileInfo(dir).fileName();
	workspace.source = QDir::toNativeSeparators(dir);
	workspace.output = QDir::toNativeSeparators(dir);
	workspace.color = Pal::AutoTagBg(workspace.tag).name();
	workspace.depot.enabled = true;
	AddSteamWorkspaceRow(workspace);
	EditDepot(_steamWorkspaces->rowCount() - 1); // a Steam workspace is meaningless without its depot
}

void SettingsPanel::AddExtraFile() {
	const QString file = QFileDialog::getOpenFileName(this, "Pick a binary");
	if (file.isEmpty()) {
		return;
	}
	ExtraLib extraLib;
	extraLib.tag = "extra";
	extraLib.path = QDir::toNativeSeparators(file);
	extraLib.color = Pal::AutoTagBg(extraLib.tag).name();
	AddExtraRow(extraLib);
	_extra->editItem(_extra->item(_extra->rowCount() - 1, 0));
}

ConfigView SettingsPanel::Values() const {
	ConfigView view;
	view.uvx = _uvx->text().trimmed();
	view.idaGui = _gui->text().trimmed();
	view.idaText = _text->text().trimmed();
	view.logDir = _logDir->text().trimmed();
	view.analysisArgs = _analysisArgs->text().trimmed();
	view.configPath = _configPath; // informational — SaveConfig never moves the file
	view.depotDownloader.executable = _depotExecutable->text().trimmed();
	{
		// An empty or unparsable field must keep the default, not fall through to 0 —
		// SaveConfig clamps that to a 1-minute timeout, which kills real downloads.
		bool ok = false;
		const int minutes = _depotTimeout->text().trimmed().toInt(&ok);
		view.depotDownloader.timeoutMinutes = ok ? minutes : 30;
	}
	{
		bool ok = false;
		const int mb = _maxLog->text().trimmed().toInt(&ok);
		view.maxLogMB = ok ? mb : 10;
	}

	auto getCellText = [](QTableWidget* tableWidget, int row, int col) {
		auto* item = tableWidget->item(row, col);
		return item ? item->text().trimmed() : QString();
	};
	for (int r = 0; r < _workspaces->rowCount(); ++r) {
		Workspace workspace;
		workspace.tag = getCellText(_workspaces, r, 0);
		workspace.source = getCellText(_workspaces, r, 1);
		workspace.output = getCellText(_workspaces, r, 2);
		if (auto* item = _workspaces->item(r, 3)) {
			workspace.files = item->data(Qt::UserRole).toStringList();
		}
		if (auto* item = _workspaces->item(r, 4)) {
			workspace.color = item->data(Qt::UserRole).toString().trimmed();
		}
		if (!workspace.tag.isEmpty()) {
			view.workspaces << workspace;
		}
	}
	for (int r = 0; r < _steamWorkspaces->rowCount(); ++r) {
		Workspace workspace;
		workspace.tag = getCellText(_steamWorkspaces, r, 0);
		workspace.source = getCellText(_steamWorkspaces, r, 1);
		workspace.output = workspace.source;
		if (auto* item = _steamWorkspaces->item(r, 2)) {
			workspace.files = item->data(Qt::UserRole).toStringList();
		}
		workspace.depot = DepotFromCell(_steamWorkspaces->item(r, 3));
		if (auto* item = _steamWorkspaces->item(r, 4)) {
			workspace.color = item->data(Qt::UserRole).toString().trimmed();
		}
		if (!workspace.tag.isEmpty()) {
			view.steamWorkspaces << workspace;
		}
	}
	for (int r = 0; r < _extra->rowCount(); ++r) {
		ExtraLib extraLib;
		extraLib.tag = getCellText(_extra, r, 0);
		extraLib.path = getCellText(_extra, r, 1);
		if (auto* item = _extra->item(r, 2)) {
			extraLib.color = item->data(Qt::UserRole).toString().trimmed();
		}
		if (!extraLib.tag.isEmpty() && !extraLib.path.isEmpty()) {
			view.extraLibs << extraLib;
		}
	}
	return view;
}
