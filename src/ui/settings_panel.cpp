#include "settings_panel.h"
#include "delegates.h"
#include "palette.h"
#include "ui_util.h"
#include "version.h"

#include <QAbstractButton>
#include <QColorDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
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
	_portOverrides = v.portOverrides; // carried through to Values() unchanged
	_configPath = v.configPath;		  // info only — the file location is not editable
	_host = new QLineEdit(v.host);
	_gui = new QLineEdit(v.idaGui);
	_text = new QLineEdit(v.idaText);
	_logDir = new QLineEdit(v.logDir);
	_analysisArgs = new QLineEdit(v.analysisArgs);
	_basePort = new QLineEdit(QString::number(v.basePort));
	_basePort->setValidator(new QIntValidator(1024, 60000, _basePort));
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
		QString guiPath, textPath;
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
	srvLay->addWidget(SectionHeader("dns", "MCP servers", "— where the per-library servers bind"));
	auto* srvRow = new QHBoxLayout;
	srvRow->setSpacing(10);
	auto* bpField = OutlinedField("Base port", _basePort);
	bpField->setFixedWidth(220);
	srvRow->addWidget(bpField);
	srvRow->addWidget(OutlinedField("Host", _host), 1);
	srvLay->addLayout(srvRow);
	srvLay->addWidget(Ui::Hint("Ports are automatic: base port + a stable per-library slot + the tag's offset; "
							   "single libraries get base port + 1000 + their index. Any individual port can be "
							   "changed on the main page (double-click the Port cell)."));

	// --- workspaces ---
	srvLay->addWidget(SectionHeader("dns", "Server & analysis", "— host address and port allocation"));

	_host = new QLineEdit(v.host);
	_basePort = new QLineEdit(QString::number(v.basePort));
	_maxLog = new QLineEdit(QString::number(v.maxLogMB));
	_analysisArgs = new QLineEdit(v.analysisArgs);

	auto* portLay = new QHBoxLayout;
	portLay->addWidget(OutlinedField("Listen host", _host));
	portLay->addWidget(OutlinedField("Base port", _basePort));
	srvLay->addLayout(portLay);
	srvLay->addWidget(OutlinedField("Batch analysis arguments", _analysisArgs));

	// DepotDownloader card
	_depotExecutable = new QLineEdit(v.depotDownloader.executable);
	_depotTimeout = new QLineEdit(QString::number(v.depotDownloader.timeoutMinutes));

	auto* depotLay = new QHBoxLayout;
	depotLay->addWidget(BrowseRow("DepotDownloader", _depotExecutable, this, false, "Select DepotDownloader executable"), 1);
	auto* timeoutField = OutlinedField("Timeout (minutes)", _depotTimeout);
	timeoutField->setFixedWidth(160);
	depotLay->addWidget(timeoutField);
	srvLay->addLayout(depotLay);

	// Card 3: Folder Workspaces
	auto* dirBox = new QGroupBox();
	dirBox->setObjectName("settingsCard");
	auto* dirLay = new QVBoxLayout(dirBox);
	dirLay->setContentsMargins(0, 0, 0, 0);
	dirLay->setSpacing(10);
	dirLay->addWidget(SectionHeader("folder_special", "Folder workspaces", "— explicit Source folder + Output folder pairs for local builds"));

	_workspaces = new QTableWidget(0, 6);
	_workspaces->setHorizontalHeader(new TrailingInsetHeader(Qt::Horizontal, _workspaces));
	_workspaces->setHorizontalHeaderLabels({"Tag", "Source root", "Output folder", "Files", "Port offset", "Color"});
	_workspaces->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	_workspaces->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
	_workspaces->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
	_workspaces->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
	_workspaces->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Fixed);
	_workspaces->setColumnWidth(3, 90);
	_workspaces->setColumnWidth(4, 90);
	_workspaces->setColumnWidth(5, 110);
	_workspaces->verticalHeader()->setVisible(false);
	_workspaces->horizontalHeader()->setFixedHeight(40);
	_workspaces->setMinimumHeight(120);
	_workspaces->verticalHeader()->setDefaultSectionSize(40);
	_workspaces->setItemDelegateForColumn(4, new PortOffsetDelegate(_workspaces));

	connect(_workspaces, &QTableWidget::cellClicked, this, [this](int r, int c) {
		if (c == 3) {
			EditFiles(_workspaces, r);
		} else if (c == 5) {
			PickTagColor(r);
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

	// Card 4: Steam Workspaces
	auto* steamBox = new QGroupBox();
	steamBox->setObjectName("settingsCard");
	auto* steamLay = new QVBoxLayout(steamBox);
	steamLay->setContentsMargins(0, 0, 0, 0);
	steamLay->setSpacing(10);
	steamLay->addWidget(SectionHeader("download", "Steam workspaces", "— single directory per build; versions arrive via DepotDownloader"));

	_steamWorkspaces = new QTableWidget(0, 6);
	_steamWorkspaces->setHorizontalHeader(new TrailingInsetHeader(Qt::Horizontal, _steamWorkspaces));
	_steamWorkspaces->setHorizontalHeaderLabels({"Tag", "Workspace dir", "Files", "Depot", "Port offset", "Color"});
	_steamWorkspaces->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	_steamWorkspaces->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
	_steamWorkspaces->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
	_steamWorkspaces->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
	_steamWorkspaces->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Fixed);
	_steamWorkspaces->setColumnWidth(2, 90);
	_steamWorkspaces->setColumnWidth(3, 140);
	_steamWorkspaces->setColumnWidth(4, 90);
	_steamWorkspaces->setColumnWidth(5, 110);
	_steamWorkspaces->verticalHeader()->setVisible(false);
	_steamWorkspaces->horizontalHeader()->setFixedHeight(40);
	_steamWorkspaces->setMinimumHeight(120);
	_steamWorkspaces->verticalHeader()->setDefaultSectionSize(40);
	_steamWorkspaces->setItemDelegateForColumn(4, new PortOffsetDelegate(_steamWorkspaces));

	connect(_steamWorkspaces, &QTableWidget::cellClicked, this, [this](int r, int c) {
		if (c == 2) {
			EditFiles(_steamWorkspaces, r);
		} else if (c == 5) {
			PickTagColor(r);
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
	extraLay->addWidget(SectionHeader("library_books", "Libraries", "— individual binaries, each with a tag and its own MCP port"));
	_extra = new QTableWidget(0, 4);
	_extra->setHorizontalHeader(new TrailingInsetHeader(Qt::Horizontal, _extra));
	_extra->setHorizontalHeaderLabels({"Tag", "Binary path", "Port", "Color"});
	_extra->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	_extra->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
	_extra->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
	_extra->setColumnWidth(2, 90);
	_extra->setColumnWidth(3, 110);
	_extra->verticalHeader()->setVisible(false);
	_extra->horizontalHeader()->setFixedHeight(40);
	_extra->setMinimumHeight(110);
	_extra->verticalHeader()->setDefaultSectionSize(40);
	_extra->setItemDelegateForColumn(2, new PortOffsetDelegate(_extra));
	connect(_extra, &QTableWidget::cellClicked, this, [this](int r, int c) {
		if (c == 3) {
			PickExtraColor(r);
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
	Ui::RoundedOutline(extraBox, 16.0, Pal::OUTLINE_VARIANT);
	Ui::RoundedOutline(cfgBox, 16.0, Pal::OUTLINE_VARIANT);
	Ui::RoundedOutline(aboutBox, 16.0, Pal::OUTLINE_VARIANT);
	Ui::RoundedSurface(_workspaces, 12.0, Pal::OUTLINE_VARIANT);
	Ui::RoundedSurface(_steamWorkspaces, 12.0, Pal::OUTLINE_VARIANT);
	Ui::RoundedSurface(_extra, 12.0, Pal::OUTLINE_VARIANT);
	Ui::OverlayVerticalScrollBar(_workspaces, _workspaces->horizontalHeader()->height() + 2);
	Ui::OverlayVerticalScrollBar(_steamWorkspaces, _steamWorkspaces->horizontalHeader()->height() + 2);
	Ui::OverlayVerticalScrollBar(_extra, _extra->horizontalHeader()->height() + 2);
	_workspaces->setItemDelegateForColumn(5, new TrailingInsetDelegate(_workspaces));
	_steamWorkspaces->setItemDelegateForColumn(5, new TrailingInsetDelegate(_steamWorkspaces));
	_extra->setItemDelegateForColumn(3, new TrailingInsetDelegate(_extra));

	for (QPushButton* button : {dirAdd, dirDel, steamAdd, steamDel, extraAdd, extraDel}) {
		Ui::ClipRounded(button, 20.0);
	}

	// --- scroll body ---
	auto* body = new QWidget;
	auto* bodyLay = new QVBoxLayout(body);
	bodyLay->setContentsMargins(24, 4, 24, 16);
	bodyLay->setSpacing(16);
	bodyLay->addWidget(Ui::Hint("Tags identify builds in the MCP parser; for folder workspaces the Sync column flags "
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
	auto* off = new QTableWidgetItem(QString::number(workspace.portOffset));
	off->setTextAlignment(Qt::AlignCenter);
	_workspaces->setItem(r, 4, off);
	auto* col = new QTableWidgetItem;
	ApplyTagSwatch(col, workspace.tag, workspace.color.isEmpty() ? Pal::AutoTagBg(workspace.tag).name() : workspace.color);
	_workspaces->setItem(r, 5, col);
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
	QVariantMap depotData{{"appId", workspace.depot.appId},
		{"os", workspace.depot.os},
		{"manifest", workspace.depot.manifest}};
	depot->setData(Qt::UserRole, depotData);
	depot->setText(QString("%1 / %2").arg(workspace.depot.appId).arg(workspace.depot.os));
	depot->setTextAlignment(Qt::AlignCenter);
	depot->setFlags(depot->flags() & ~Qt::ItemIsEditable);
	depot->setToolTip(QString("Double-click to edit\nCurrent ManifestID: %1")
		.arg(workspace.depot.manifest.isEmpty() ? QStringLiteral("none") : workspace.depot.manifest));
	_steamWorkspaces->setItem(r, 3, depot);
	auto* off = new QTableWidgetItem(QString::number(workspace.portOffset));
	off->setTextAlignment(Qt::AlignCenter);
	_steamWorkspaces->setItem(r, 4, off);
	auto* col = new QTableWidgetItem;
	ApplyTagSwatch(col, workspace.tag, workspace.color.isEmpty() ? Pal::AutoTagBg(workspace.tag).name() : workspace.color);
	_steamWorkspaces->setItem(r, 5, col);
}

void SettingsPanel::PickTagColor(int row) {
	auto* item = _workspaces->item(row, 5);
	if (!item) {
		return;
	}
	const QString tag = _workspaces->item(row, 0) ? _workspaces->item(row, 0)->text() : QString();
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
	const QVariantMap oldData = item->data(Qt::UserRole).toMap();
	QDialog dialog(this);
	dialog.setWindowTitle("Depot source");
	dialog.setMinimumWidth(440);
	auto* layout = new QVBoxLayout(&dialog);
	auto* appId = new QLineEdit(QString::number(oldData.value("appId", 730).toInt()));
	appId->setValidator(new QIntValidator(1, 2147483647, appId));
	auto* osCombo = Ui::ComboBox();
	osCombo->addItems({"windows", "linux"});
	osCombo->setCurrentText(oldData.value("os", "windows").toString());
	auto* idsLayout = new QHBoxLayout;
	idsLayout->addWidget(OutlinedField("App ID", appId));
	auto* osField = Ui::ComboField("Depot OS", osCombo);
	idsLayout->addWidget(osField);
	layout->addLayout(idsLayout);
	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	layout->addWidget(buttons);
	if (dialog.exec() != QDialog::Accepted) {
		return;
	}
	QVariantMap data = oldData;
	const int newAppId = appId->text().toInt();
	const QString newOs = osCombo->currentText();
	if (newAppId != oldData.value("appId", 730).toInt() || newOs != oldData.value("os", "windows").toString()) {
		data["manifest"] = QString();
	}
	data["appId"] = newAppId;
	data["os"] = newOs;
	item->setData(Qt::UserRole, data);
	item->setText(QString("%1 / %2").arg(newAppId).arg(newOs));
	item->setToolTip(QString("Double-click to edit\nCurrent ManifestID: %1")
		.arg(data.value("manifest").toString().isEmpty() ? QStringLiteral("none") : data.value("manifest").toString()));
}

void SettingsPanel::AddExtraRow(const ExtraLib& extraLib) {
	const int r = _extra->rowCount();
	_extra->insertRow(r);
	_extra->setItem(r, 0, new QTableWidgetItem(extraLib.tag));
	_extra->setItem(r, 1, new QTableWidgetItem(extraLib.path));
	const int base = _basePort ? _basePort->text().trimmed().toInt() : 8500;
	const int shownPort = extraLib.port > 0 ? extraLib.port : AutoExtraPort(base, r);
	auto* off = new QTableWidgetItem(QString::number(shownPort));
	off->setTextAlignment(Qt::AlignCenter);
	_extra->setItem(r, 2, off);
	auto* col = new QTableWidgetItem;
	ApplyTagSwatch(col, extraLib.tag, extraLib.color);
	_extra->setItem(r, 3, col);
}

void SettingsPanel::PickExtraColor(int row) {
	auto* item = _extra->item(row, 3);
	if (!item) {
		return;
	}
	const QString tag = _extra->item(row, 0) ? _extra->item(row, 0)->text() : QString();
	const QString currentHex = item->data(Qt::UserRole).toString();
	const QColor initialColor = currentHex.isEmpty() ? Pal::AutoTagBg(tag) : QColor(currentHex);
	const QColor selectedColor = QColorDialog::getColor(initialColor, this, "Tag colour");
	if (!selectedColor.isValid()) {
		return;
	}
	ApplyTagSwatch(item, tag, selectedColor.name());
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
		const QString pathText = QInputDialog::getText(&dialog, "Add file", isSteam ? "Path relative to depot game directory:" : "Path relative to Source:",
			QLineEdit::Normal, QString(), &ok).trimmed();
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

	layout->addWidget(Ui::Hint(QString(isSteam
		? "Paths are relative to the depot game directory and preserved under each ManifestID."
		: "Files tracked under \"%1\". Every path is relative to Source and is preserved under Output and revisions.").arg(tag)));
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
	workspace.portOffset = _workspaces->rowCount() * 100;
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
	workspace.portOffset = (_workspaces->rowCount() + _steamWorkspaces->rowCount()) * 100;
	workspace.color = Pal::AutoTagBg(workspace.tag).name();
	workspace.depot.enabled = true;
	AddSteamWorkspaceRow(workspace);
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
	// Pre-fill a port that will actually pass validation: the auto slot may be
	// taken by a port override or another library (same logic as "Add binary").
	extraLib.port = FirstFreeExtraPort(Values());
	AddExtraRow(extraLib);
	_extra->editItem(_extra->item(_extra->rowCount() - 1, 0));
}

ConfigView SettingsPanel::Values() const {
	ConfigView view;
	view.host = _host->text().trimmed();
	view.idaGui = _gui->text().trimmed();
	view.idaText = _text->text().trimmed();
	view.logDir = _logDir->text().trimmed();
	view.analysisArgs = _analysisArgs->text().trimmed();
	view.configPath = _configPath; // informational — SaveConfig never moves the file
	view.depotDownloader.executable = _depotExecutable->text().trimmed();
	view.depotDownloader.timeoutMinutes = _depotTimeout->text().trimmed().toInt();
	{
		bool ok = false;
		const int mb = _maxLog->text().trimmed().toInt(&ok);
		view.maxLogMB = ok ? mb : 10;
	}
	{
		bool ok = false;
		const int bp = _basePort->text().trimmed().toInt(&ok);
		view.basePort = ok ? bp : 8500;
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
		workspace.portOffset = getCellText(_workspaces, r, 4).toInt();
		if (auto* item = _workspaces->item(r, 5)) {
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
		workspace.depot.enabled = true;
		if (auto* item = _steamWorkspaces->item(r, 2)) {
			workspace.files = item->data(Qt::UserRole).toStringList();
		}
		if (auto* item = _steamWorkspaces->item(r, 3)) {
			const QVariantMap depot = item->data(Qt::UserRole).toMap();
			workspace.depot.appId = depot.value("appId", 730).toInt();
			workspace.depot.os = depot.value("os", "windows").toString();
			workspace.depot.manifest = depot.value("manifest").toString();
		}
		workspace.portOffset = getCellText(_steamWorkspaces, r, 4).toInt();
		if (auto* item = _steamWorkspaces->item(r, 5)) {
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
		extraLib.port = getCellText(_extra, r, 2).toInt(); // absolute port (SaveConfig drops it when == auto)
		if (auto* item = _extra->item(r, 3)) {
			extraLib.color = item->data(Qt::UserRole).toString().trimmed();
		}
		if (!extraLib.tag.isEmpty() && !extraLib.path.isEmpty()) {
			view.extraLibs << extraLib;
		}
	}
	view.portOverrides = _portOverrides; // not editable here — pass through untouched
	return view;
}
