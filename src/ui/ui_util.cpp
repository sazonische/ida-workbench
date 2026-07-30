#include "ui_util.h"
#include "palette.h"
#include "singleton.h"

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEnterEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QComboBox>
#include <QGroupBox>
#include <QListView>
#include <QListWidget>
#include <QMouseEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointF>
#include <QPushButton>
#include <QRegion>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QStyleOptionHeader>
#include <QTimer>
#include <QTableView>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <dwmapi.h>
#include <windows.h>
#endif

namespace {
	void RoundPopupWindow(QWidget* popupWindow, qreal cornerRadius) {
		if (!popupWindow) {
			return;
		}
#ifdef Q_OS_WIN
		const DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_ROUND;
		DwmSetWindowAttribute(reinterpret_cast<HWND>(popupWindow->winId()),
			DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
#endif
		// Qt::Popup uses a legacy menu shadow on Windows. Give the native window the
		// same rounded region so that shadow follows the corners instead of retaining
		// a rectangular silhouette; DWM still antialiases the visible surface.
		QPainterPath path;
		path.addRoundedRect(QRectF(popupWindow->rect()), cornerRadius, cornerRadius);
		popupWindow->setMask(QRegion(path.toFillPolygon().toPolygon()));
	}

	class MaterialComboBox final : public QComboBox {
	public:
		using QComboBox::QComboBox;
		void SetField(QWidget* field) { _field = field; }
		void SetPopupWindow(QWidget* popupWindow) {
			if (_popup == popupWindow) {
				return;
			}
			if (_popup) {
				_popup->removeEventFilter(this);
			}
			_popup = popupWindow;
			if (!_popup) {
				return;
			}
			_popup->installEventFilter(this);
			// Create and configure the native window before its first visible frame.
			RoundPopupWindow(_popup, 10.0);
		}
	protected:
		void showPopup() override {
			SetExpanded(true);
			const int popupWidth = _field ? _field->width() : width();
			view()->setFixedWidth(popupWidth);
			if (_popup) {
				_popup->setFixedWidth(popupWidth);
				RoundPopupWindow(_popup, 10.0);
			}
			QComboBox::showPopup();
		}
		void hidePopup() override {
			QComboBox::hidePopup();
			SetExpanded(false);
		}
		bool eventFilter(QObject* watched, QEvent* event) override {
			if (watched != _popup) {
				return QComboBox::eventFilter(watched, event);
			}
			if (event->type() == QEvent::Show) {
				SetExpanded(true);
				if (_field) {
					const QPoint fieldTop = _field->mapToGlobal(QPoint(0, 0));
					const int gap = _popup->y() >= fieldTop.y() ? 4 : -4;
					_popup->move(fieldTop.x(), _popup->y() + gap);
				}
				RoundPopupWindow(_popup, 10.0);
			} else if (event->type() == QEvent::Resize) {
				RoundPopupWindow(_popup, 10.0);
			} else if (event->type() == QEvent::Hide || event->type() == QEvent::Close ||
					   event->type() == QEvent::WindowDeactivate) {
				SetExpanded(false);
			}
			return QComboBox::eventFilter(watched, event);
		}
		void paintEvent(QPaintEvent* event) override {
			QComboBox::paintEvent(event);
			QStyleOptionComboBox option;
			initStyleOption(&option);
			const QRect arrow = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, this);
			QPainter painter(this);
			painter.setRenderHint(QPainter::Antialiasing);
			painter.setPen(QPen(isEnabled() ? Pal::ON_SURFACE_VARIANT : Pal::OUTLINE, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			const QPointF center = arrow.center();
			painter.drawLine(center + QPointF(-4, -2), center + QPointF(0, 2));
			painter.drawLine(center + QPointF(0, 2), center + QPointF(4, -2));
		}
	private:
		void SetExpanded(bool expanded) {
			if (!_field) {
				return;
			}
			_field->setProperty("expanded", expanded);
			_field->style()->unpolish(_field);
			_field->style()->polish(_field);
			_field->update();
		}
		QWidget* _field = nullptr;
		QWidget* _popup = nullptr;
	};

	class RoundedOutlineOverlay final : public QWidget {
	public:
		RoundedOutlineOverlay(QWidget* target, qreal radius, const QColor& color) :
			QWidget(target), _target(target), _radius(radius), _color(color) {
			setAttribute(Qt::WA_TransparentForMouseEvents);
			setAttribute(Qt::WA_NoSystemBackground);
			setAttribute(Qt::WA_TranslucentBackground);
			setAutoFillBackground(false);
			target->installEventFilter(this);
			SyncGeometry();
			show();
			raise();
		}

	protected:
		bool eventFilter(QObject* watched, QEvent* event) override {
			if (watched == _target && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
				SyncGeometry();
				raise();
			}
			return QWidget::eventFilter(watched, event);
		}

		void paintEvent(QPaintEvent*) override {
			QPainter painter(this);
			painter.setRenderHint(QPainter::Antialiasing);
			painter.setBrush(Qt::NoBrush);
			painter.setPen(QPen(_color, 1.0));
			painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), _radius, _radius);
		}

	private:
		void SyncGeometry() { setGeometry(_target->rect()); }
		QWidget* _target;
		qreal _radius;
		QColor _color;
	};

	class RoundedMaskFilter final : public QObject {
	public:
		RoundedMaskFilter(QWidget* target, qreal radius) :
			QObject(target), _target(target), _radius(radius) {
			target->installEventFilter(this);
			ApplyMask();
		}

	protected:
		bool eventFilter(QObject* watched, QEvent* event) override {
			if (watched == _target && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
				ApplyMask();
			}
			return QObject::eventFilter(watched, event);
		}

	private:
		void ApplyMask() {
			QPainterPath path;
			path.addRoundedRect(QRectF(_target->rect()), _radius, _radius);
			_target->setMask(QRegion(path.toFillPolygon().toPolygon()));
		}
		QWidget* _target;
		qreal _radius;
	};

	class OverlayScrollController final : public QObject {
	public:
		OverlayScrollController(QAbstractScrollArea* target, int topInset) :
			QObject(target), _target(target), _topInset(topInset),
			_overlay(new QScrollBar(Qt::Vertical, target)) {
			target->setProperty("overlayScrollBar", true);
			_overlay->setObjectName("tableOverlayScrollBar");
			_overlay->hide();
			auto* model = target->verticalScrollBar();
			target->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
			target->installEventFilter(this);
			connect(model, &QScrollBar::rangeChanged, this, [this, model](int minimum, int maximum) {
				_overlay->setRange(minimum, maximum);
				_overlay->setPageStep(model->pageStep());
				_overlay->setSingleStep(model->singleStep());
				_overlay->setVisible(maximum > minimum);
				SyncGeometry();
				_overlay->raise();
			});
			connect(model, &QScrollBar::valueChanged, _overlay, &QScrollBar::setValue);
			connect(_overlay, &QScrollBar::valueChanged, model, &QScrollBar::setValue);
			SyncGeometry();
		}

	protected:
		bool eventFilter(QObject* watched, QEvent* event) override {
			if (watched == _target && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
				SyncGeometry();
				_overlay->raise();
			}
			return QObject::eventFilter(watched, event);
		}

	private:
		void SyncGeometry() {
			_overlay->setGeometry(_target->width() - 13, _topInset, 10, qMax(0, _target->height() - _topInset - 4));
		}
		QAbstractScrollArea* _target;
		int _topInset;
		QScrollBar* _overlay;
	};

	// A soft rounded tooltip bubble. Native QToolTip windows are clipped by a 1-bit
	// mask when QSS gives them a border-radius, which leaves jagged corners — this
	// is a translucent window whose rounded rect is painted antialiased instead.
	// One shared instance serves every RoundIconButton.
	class TipBubble final : public QWidget, public Singleton<TipBubble> {
	public:
		static TipBubble* Instance() {
			return InstancePtr();
		}

		void ShowFor(const QWidget* anchor, const QString& text) {
			_text = text;
			const QFontMetrics fm(font());
			resize(fm.horizontalAdvance(text) + 22, fm.height() + 12);
			// centred under the anchor, like a Material plain tooltip
			const QPoint below = anchor->mapToGlobal(QPoint(anchor->width() / 2, anchor->height() + 6));
			move(below.x() - width() / 2, below.y());
			show();
			raise();
		}

	private:
		friend class Singleton<TipBubble>;

		TipBubble() :
			QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint) {
			setAttribute(Qt::WA_TranslucentBackground);
			setAttribute(Qt::WA_ShowWithoutActivating);
			setAttribute(Qt::WA_TransparentForMouseEvents);
			QFont f = font();
			f.setPixelSize(12);
			setFont(f);
		}

	protected:
		void paintEvent(QPaintEvent*) override {
			QPainter p(this);
			p.setRenderHint(QPainter::Antialiasing);
			p.setPen(Qt::NoPen);
			p.setBrush(QColor("#F4EDFA")); // surfaceContainerHigh (theme.cpp)
			p.drawRoundedRect(QRectF(rect()), 8.0, 8.0);
			p.setPen(Pal::TITLE_TEXT);
			p.drawText(rect(), Qt::AlignCenter, _text);
		}

	private:
		QString _text;
	};

	// M3 "standard icon button": a fixed circle that paints its own antialiased
	// state layer (QSS border-radius rounds through a jagged mask, QPainter does
	// not). The QPushButton icon stays the painted source, so setIcon() continues
	// to drive spinners; tooltips go through the shared TipBubble.
	class RoundIconButton final : public QPushButton {
	public:
		RoundIconButton(const QString& tooltip, const QColor& stateColor, int diameter, int iconPx) :
			_state(stateColor), _iconPx(iconPx) {
			setFixedSize(diameter, diameter);
			setCursor(Qt::PointingHandCursor);
			setFocusPolicy(Qt::NoFocus);
			// The text lives on the widget (not in a private copy) so callers can keep it
			// current with plain setToolTip() — a disabled action's reason changes with the
			// selection — and so menus mirroring toolTip() show the same words.
			setToolTip(tooltip);
			_tipTimer.setSingleShot(true);
			_tipTimer.setInterval(350); // tooltip-grade delay
			connect(&_tipTimer, &QTimer::timeout, this, [this] {
				if (underMouse() && !toolTip().isEmpty()) {
					TipBubble::Instance()->ShowFor(this, toolTip());
				}
			});
		}

	protected:
		// Suppress the native square tip: the styled bubble above is the only one shown.
		bool event(QEvent* event) override {
			if (event->type() == QEvent::ToolTip) {
				return true;
			}
			return QPushButton::event(event);
		}

		void paintEvent(QPaintEvent*) override {
			QPainter p(this);
			p.setRenderHint(QPainter::Antialiasing);
			if (isEnabled() && (isDown() || underMouse())) {
				QColor layer = _state;
				layer.setAlphaF(isDown() ? 0.12 : 0.08); // M3 state-layer opacities
				p.setPen(Qt::NoPen);
				p.setBrush(layer);
				p.drawEllipse(rect());
			}
			icon().paint(&p, QRect((width() - _iconPx) / 2, (height() - _iconPx) / 2, _iconPx, _iconPx),
						 Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled);
		}

		void enterEvent(QEnterEvent* event) override {
			_tipTimer.start();
			update();
			QPushButton::enterEvent(event);
		}
		void leaveEvent(QEvent* event) override {
			_tipTimer.stop();
			TipBubble::Instance()->hide();
			update();
			QPushButton::leaveEvent(event);
		}
		void mousePressEvent(QMouseEvent* event) override {
			_tipTimer.stop();
			TipBubble::Instance()->hide();
			QPushButton::mousePressEvent(event);
		}

	private:
		QColor _state;
		int _iconPx;
		QTimer _tipTimer;
	};

} // namespace

TrailingInsetHeader::TrailingInsetHeader(Qt::Orientation orientation, QWidget* parent) :
	QHeaderView(orientation, parent) {}

void TrailingInsetHeader::SetTrailingInset(int inset) {
	_trailingInset = qMax(0, inset);
	viewport()->update();
}

void TrailingInsetHeader::paintSection(QPainter* painter, const QRect& rect, int logicalIndex) const {
	int lastVisible = count() - 1;
	while (lastVisible >= 0 && isSectionHidden(lastVisible)) {
		--lastVisible;
	}
	if (_trailingInset <= 0 || logicalIndex != lastVisible) {
		QHeaderView::paintSection(painter, rect, logicalIndex);
		return;
	}

	QStyleOptionHeader option;
	initStyleOptionForIndex(&option, logicalIndex);
	option.rect = rect;
	style()->drawControl(QStyle::CE_HeaderSection, &option, painter, this);
	option.rect.adjust(0, 0, -_trailingInset, 0);
	style()->drawControl(QStyle::CE_HeaderLabel, &option, painter, this);
}

namespace Ui {

	static QString kFamily, kFamilyFill;

	static const QHash<QString, ushort>& Codepoints() {
		static const QHash<QString, ushort> m = {
			{"hub", 0xe9f4},
			{"dns", 0xe875},
			{"manage_search", 0xf02f},
			{"difference", 0xeb7d},
			{"check_circle", 0xf0be},
			{"list", 0xe896},
			{"tab", 0xe8d8},
			{"settings", 0xe8b8},
			{"check", 0xe668},
			{"play_arrow", 0xe037},
			{"stop", 0xe047},
			{"autorenew", 0xe863},
			{"swap_horiz", 0xe8d4},
			{"refresh", 0xe5d5},
			{"checklist", 0xe6b1},
			{"deployed_code", 0xf720},
			{"new_releases", 0xef76},
			{"folder_open", 0xe2c8},
			{"open_in_new", 0xe89e},
			{"folder_special", 0xe617},
			{"library_books", 0xe02f},
			{"attach_file", 0xe226},
			{"folder", 0xe2c7},
			{"terminal", 0xeb8e},
			{"create_new_folder", 0xe2cc},
			{"delete", 0xe92e},
			{"add", 0xe145},
			{"note_add", 0xe89c},
			{"close", 0xe5cd},
			{"remove", 0xe15b},
			{"content_copy", 0xe14d},
			{"download", 0xf090},
			{"info", 0xe88e},
		};
		return m;
	}

	void LoadIcons() {
		auto fam = [](const QString& path) -> QString {
			const int id = QFontDatabase::addApplicationFont(path);
			const QStringList fontFamilies = QFontDatabase::applicationFontFamilies(id);
			return fontFamilies.isEmpty() ? QString() : fontFamilies.first();
		};
		kFamily = fam(":/fonts/symbols.ttf");
		kFamilyFill = fam(":/fonts/symbols_fill.ttf");
		// UI + monospace fonts (Roboto Flex / Roboto Mono) — several weights per family
		for (const char* fontPath : {":/fonts/robotoflex-400.ttf", ":/fonts/robotoflex-500.ttf", ":/fonts/robotoflex-600.ttf", ":/fonts/robotoflex-700.ttf", ":/fonts/robotomono-400.ttf", ":/fonts/robotomono-500.ttf"}) {
			QFontDatabase::addApplicationFont(fontPath);
		}
	}

	QString Sym(const QString& name) {
		const ushort codepoint = Codepoints().value(name, 0);
		return codepoint ? QString(QChar(codepoint)) : QString();
	}

	static QFont SymFont(int px, bool filled) {
		QFont font(filled ? kFamilyFill : kFamily);
		font.setPixelSize(px);
		return font;
	}

	QFont SymbolFont(int px, bool filled) { return SymFont(px, filled); }

	QIcon Icon(const QString& name, const QColor& color, int px, bool filled) {
		const qreal dpr = qApp->devicePixelRatio();
		QPixmap pixmap(int(px * dpr), int(px * dpr));
		pixmap.setDevicePixelRatio(dpr);
		pixmap.fill(Qt::transparent);
		QPainter painter(&pixmap);
		painter.setRenderHint(QPainter::TextAntialiasing);
		painter.setFont(SymFont(px, filled));
		painter.setPen(color);
		painter.drawText(QRectF(0, 0, px, px), Qt::AlignCenter, Sym(name));
		painter.end();
		return QIcon(pixmap);
	}

	void DrawSpinningGlyph(QPainter* painter, const QString& name, int px, const QColor& color, const QPointF& center, qreal angle, bool filled) {
		painter->save();
		painter->setRenderHint(QPainter::TextAntialiasing);
		painter->setFont(SymFont(px, filled));
		painter->setPen(color);
		painter->translate(center);
		painter->rotate(angle);
		painter->drawText(QRectF(-px / 2.0, -px / 2.0, px, px), Qt::AlignCenter, Sym(name));
		painter->restore();
	}

	QIcon SpinningIcon(const QString& name, int px, const QColor& color, qreal angle, bool filled) {
		const qreal dpr = qApp->devicePixelRatio();
		QPixmap pixmap(int(px * dpr), int(px * dpr));
		pixmap.setDevicePixelRatio(dpr);
		pixmap.fill(Qt::transparent);
		QPainter painter(&pixmap);
		DrawSpinningGlyph(&painter, name, px, color, QPointF(px / 2.0, px / 2.0), angle, filled);
		painter.end();
		return QIcon(pixmap);
	}

	QIcon SwatchIcon(const QColor& color, int px) {
		const qreal dpr = qApp->devicePixelRatio();
		QPixmap pixmap(int(px * dpr), int(px * dpr));
		pixmap.setDevicePixelRatio(dpr);
		pixmap.fill(Qt::transparent);
		QPainter painter(&pixmap);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setBrush(color);
		painter.setPen(QPen(color.darker(125), 1.0));
		painter.drawRoundedRect(QRectF(2, 2, px - 4, px - 4), 4, 4);
		return QIcon(pixmap);
	}

	QPixmap TintedPixmap(const QString& resourcePath, const QColor& color, int px) {
		QPixmap src(resourcePath);
		if (src.isNull()) {
			return {};
		}
		const qreal dpr = qApp->devicePixelRatio();
		const int side = int(px * dpr);
		const QPixmap scaled = src.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
		QPixmap out(scaled.size());
		out.setDevicePixelRatio(dpr);
		out.fill(Qt::transparent);
		QPainter painter(&out);
		painter.drawPixmap(0, 0, scaled);
		painter.setCompositionMode(QPainter::CompositionMode_SourceIn); // keep alpha, replace colour
		painter.fillRect(out.rect(), color);
		painter.end();
		return out;
	}

	QLabel* IconLabel(const QString& name, int px, const QColor& color, bool filled) {
		auto* label = new QLabel(Sym(name));
		const QString family = filled ? kFamilyFill : kFamily;
		label->setFont(SymFont(px, filled));
		label->setStyleSheet(QString("color:%1; background:transparent; font-family:'%2'; font-size:%3px;")
							 .arg(color.name(), family)
							 .arg(px));
		return label;
	}

	QPushButton* Button(const QString& text, const char* variant) {
		auto* button = new QPushButton(text);
		if (variant) {
			button->setProperty("variant", variant);
		}
		button->setCursor(Qt::PointingHandCursor);
		return button;
	}

	QPushButton* IconButton(const QString& iconName, const QString& text, const char* variant, bool filledIcon) {
		auto* button = Button(text, variant);
		QColor color = Pal::PRIMARY; // text / tonal
		if (!variant) {
			color = Pal::ON_PRIMARY; // filled primary
		} else if (QString(variant) == "tonal") {
			color = Pal::ON_SECONDARY_CONTAINER;
		}
		button->setIcon(Icon(iconName, color, 18, filledIcon));
		button->setIconSize(QSize(18, 18));
		return button;
	}

	QComboBox* ComboBox(QWidget* parent) {
		auto* combo = new MaterialComboBox(parent);
		combo->setObjectName("materialCombo");
		auto* view = new QListView(combo);
		view->setObjectName("materialComboPopup");
		view->setSpacing(2);
		view->setUniformItemSizes(true);
		combo->setView(view);
		combo->SetPopupWindow(view->window());
		combo->setMaxVisibleItems(10);
		combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		combo->setMinimumContentsLength(12);
		combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		return combo;
	}

	QGroupBox* Field(const QString& label, QWidget* content, QWidget* parent) {
		auto* field = new QGroupBox(label, parent);
		field->setObjectName("materialField");
		field->setFixedHeight(60);
		auto* layout = new QVBoxLayout(field);
		layout->setContentsMargins(4, 1, 4, 2);
		layout->setSpacing(0);
		layout->addWidget(content);
		return field;
	}

	QGroupBox* ComboField(const QString& label, QComboBox* combo, QWidget* parent) {
		auto* field = Field(label, combo, parent);
		if (auto* material = dynamic_cast<MaterialComboBox*>(combo)) {
			material->SetField(field);
		}
		return field;
	}

	QMenu* Menu(QWidget* parent) {
		auto* menu = new QMenu(parent);
		menu->setObjectName("materialMenu");
		return menu;
	}

	QWidget* VerticalDivider(int height, QWidget* parent) {
		auto* divider = new QFrame(parent);
		divider->setFixedSize(1, height);
		divider->setStyleSheet(QString("background:%1;").arg(Pal::DIVIDER.name()));
		return divider;
	}

	QWidget* SegmentedControl(const QList<QPushButton*>& buttons, int width, QWidget* parent) {
		auto* control = new QFrame(parent);
		control->setObjectName("segmented");
		control->setAttribute(Qt::WA_StyledBackground, true);
		control->setFixedHeight(40);
		if (width > 0) {
			control->setFixedWidth(width);
		}
		auto* layout = new QHBoxLayout(control);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(0);
		for (int i = 0; i < buttons.size(); ++i) {
			QPushButton* button = buttons[i];
			button->setObjectName(i == 0 ? "segLeft" : (i == buttons.size() - 1 ? "segRight" : "segMiddle"));
			button->setFixedHeight(40);
			layout->addWidget(button);
			if (i + 1 < buttons.size()) {
				layout->addWidget(VerticalDivider(40));
			}
		}
		RoundedSurface(control, 20.0, Pal::DIVIDER);
		return control;
	}

	QPushButton* RoundIconButton(const QString& iconName, const QString& tooltip, const QColor& iconColor, const QColor& stateColor, int diameter, int iconPx) {
		auto* button = new ::RoundIconButton(tooltip, stateColor, diameter, iconPx);
		button->setIcon(Icon(iconName, iconColor, iconPx));
		return button;
	}

	void ClipRounded(QWidget* target, qreal radius) {
		new RoundedMaskFilter(target, radius);
	}

	QWidget* RoundedOutline(QWidget* target, qreal radius, const QColor& color) {
		return new RoundedOutlineOverlay(target, radius, color);
	}

	QWidget* RoundedSurface(QWidget* target, qreal radius, const QColor& color) {
		ClipRounded(target, radius);
		return RoundedOutline(target, radius, color);
	}

	void OverlayVerticalScrollBar(QAbstractScrollArea* target, int topInset) {
		if (auto* table = qobject_cast<QTableView*>(target)) {
			target->setProperty("overlayContentInset", 14);
			if (auto* header = dynamic_cast<TrailingInsetHeader*>(table->horizontalHeader())) {
				header->SetTrailingInset(14);
			}
		}
		new OverlayScrollController(target, topInset);
	}

	QLabel* Hint(const QString& text) {
		auto* label = new QLabel(text);
		label->setObjectName("appSubtitle");
		label->setWordWrap(true);
		return label;
	}

	bool EditFolderList(QWidget* parent, const QString& title, const QString& intro, QStringList& dirs) {
		QDialog dialog(parent);
		dialog.setWindowTitle(title);
		dialog.resize(560, 380);
		auto* layout = new QVBoxLayout(&dialog);
		auto* list = new QListWidget;
		RoundedSurface(list, 10.0, Pal::OUTLINE_VARIANT);
		OverlayVerticalScrollBar(list);
		for (const QString& folderPath : dirs) {
			if (!folderPath.trimmed().isEmpty()) {
				new QListWidgetItem(folderPath.trimmed(), list);
			}
		}

		auto* addBtn = IconButton("create_new_folder", "Add folder…", "tonal");
		auto* delBtn = IconButton("delete", "Remove", "text");
		ClipRounded(addBtn, 20.0);
		ClipRounded(delBtn, 20.0);
		auto* buttonsLayout = new QHBoxLayout;
		buttonsLayout->addWidget(addBtn);
		buttonsLayout->addWidget(delBtn);
		buttonsLayout->addStretch();

		auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
		for (QAbstractButton* button : buttonBox->buttons()) {
			ClipRounded(button, 20.0);
		}
		QObject::connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		QObject::connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		QObject::connect(addBtn, &QPushButton::clicked, &dialog, [&dialog, list] {
			const QString pickedPath = QFileDialog::getExistingDirectory(&dialog, "Add a folder");
			if (!pickedPath.isEmpty()) {
				new QListWidgetItem(QDir::toNativeSeparators(pickedPath), list);
			}
		});
		QObject::connect(delBtn, &QPushButton::clicked, &dialog, [list] { delete list->currentItem(); });

		layout->addWidget(Hint(intro));
		layout->addWidget(list, 1);
		layout->addLayout(buttonsLayout);
		layout->addWidget(buttonBox);

		if (dialog.exec() != QDialog::Accepted) {
			return false;
		}
		dirs.clear();
		for (int i = 0; i < list->count(); ++i) {
			const QString itemText = list->item(i)->text().trimmed();
			if (!itemText.isEmpty()) {
				dirs << itemText;
			}
		}
		return true;
	}

} // namespace Ui
