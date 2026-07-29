#include "delegates.h"
#include "palette.h"
#include "ui_util.h"

#include <QApplication>
#include <QColor>
#include <QFontMetricsF>
#include <QPainter>
#include <QSpinBox>
#include <QStyle>
#include <QTableWidget>

namespace {
	const QTableWidget* TableFor(const QStyleOptionViewItem& option) {
		const QWidget* widget = option.widget;
		while (widget) {
			if (const auto* table = qobject_cast<const QTableWidget*>(widget)) {
				return table;
			}
			widget = widget->parentWidget();
		}
		return nullptr;
	}

	void PaintCellSurface(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) {
		const QBrush background = index.data(Qt::BackgroundRole).value<QBrush>();
		if (background.style() != Qt::NoBrush && background.color().alpha() > 0) {
			painter->fillRect(option.rect, background);
		} else if (const auto* table = TableFor(option); table && table->property("hoveredRow").toInt() == index.row()) {
			painter->fillRect(option.rect, Pal::ROW_HOVER);
		}
		painter->setPen(Pal::ROW_SEPARATOR);
		painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
	}

	QColor ChipColor(const QString& text) {
		if (text == "Ready" || text == "in sync") {
			return Pal::CHIP_GREEN_FG;
		}
		if (text == "Update available" || text == "Open in IDA") {
			return Pal::CHIP_BLUE_FG;
		}
		if (text == "Not analyzed" || text == "Re-analyze" || text == "changed") {
			return Pal::CHIP_AMBER_FG;
		}
		return Pal::CHIP_GREY_FG; // No binary / source missing
	}

	QColor ChipBg(const QString& text) {
		if (text == "Ready" || text == "in sync") {
			return Pal::CHIP_GREEN_BG;
		}
		if (text == "Update available" || text == "Open in IDA") {
			return Pal::CHIP_BLUE_BG;
		}
		if (text == "Not analyzed" || text == "Re-analyze" || text == "changed") {
			return Pal::CHIP_AMBER_BG;
		}
		return Pal::CHIP_GREY_BG;
	}

	bool Skip(const QString& text) {
		return text.isEmpty() || text == "–" || text == "—";
	}

	QRect ContentRect(const QStyleOptionViewItem& option, const QModelIndex& index) {
		const auto* table = TableFor(option);
		if (!table) {
			return option.rect;
		}
		int lastVisible = table->columnCount() - 1;
		while (lastVisible >= 0 && table->isColumnHidden(lastVisible)) {
			--lastVisible;
		}
		if (index.column() != lastVisible) {
			return option.rect;
		}
		return option.rect.adjusted(0, 0, -table->property("overlayContentInset").toInt(), 0);
	}
} // namespace

void RowDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
	painter->save();
	PaintCellSurface(painter, option, index);
	painter->restore();
	QStyledItemDelegate::paint(painter, option, index);
	painter->save();
	painter->setPen(Pal::ROW_SEPARATOR);
	painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
	painter->restore();
}

void CheckDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
	painter->save();
	PaintCellSurface(painter, option, index);
	painter->restore();
	// Same rounded M3 checkbox as SelectHeader::paintSection: outlined when
	// unchecked, filled with a check when checked (no tri-state for rows).
	const auto state = static_cast<Qt::CheckState>(index.data(Qt::CheckStateRole).toInt());
	painter->save();
	painter->setRenderHint(QPainter::Antialiasing);
	const QRectF box(option.rect.center().x() - 9, option.rect.center().y() - 9, 18, 18);
	if (state == Qt::Checked) {
		painter->setPen(Qt::NoPen);
		painter->setBrush(Pal::PRIMARY);
		painter->drawRoundedRect(box, 4, 4);
		Ui::DrawSpinningGlyph(painter, "check", 16, Pal::ON_PRIMARY, box.center(), 0, true);
	} else {
		painter->setPen(QPen(Pal::OUTLINE, 2));
		painter->setBrush(Qt::NoBrush);
		painter->drawRoundedRect(box.adjusted(1, 1, -1, -1), 4, 4);
	}
	painter->restore();
}

void ChipDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
	const QString text = index.data(Qt::DisplayRole).toString();
	if (Skip(text)) {
		painter->save();
		PaintCellSurface(painter, option, index);
		painter->restore();
		QStyleOptionViewItem contentOption(option);
		contentOption.rect = ContentRect(option, index);
		QStyledItemDelegate::paint(painter, contentOption, index);
		painter->save();
		painter->setPen(Pal::ROW_SEPARATOR);
		painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
		painter->restore();
		return;
	}

	painter->save();
	painter->setRenderHint(QPainter::Antialiasing);
	PaintCellSurface(painter, option, index);
	QFont font = option.font;
	font.setPixelSize(12);
	font.setBold(true);
	painter->setFont(font);
	const QFontMetrics fontMetrics(font);
	const QRect content = ContentRect(option, index);
	const int pillHeight = qMin(content.height() - 10, 24);

	if (text == "UP" || text == "down") {
		// outlined pill with a leading status dot
		const bool isUp = (text == "UP");
		const int textWidth = fontMetrics.horizontalAdvance(text);
		const int pillWidth = textWidth + 30;
		QRect pill(0, 0, pillWidth, pillHeight);
		pill.moveCenter(content.center());
		painter->setPen(QPen(Pal::PILL_OUTLINE, 1));
		painter->setBrush(Qt::NoBrush);
		painter->drawRoundedRect(pill, pillHeight / 2.0, pillHeight / 2.0);
		const int dotRadius = 3;
		const int dotCenterX = pill.left() + 12;
		const int dotCenterY = pill.center().y();
		painter->setPen(Qt::NoPen);
		painter->setBrush(isUp ? Pal::SERVER_UP : Pal::SERVER_DOWN);
		painter->drawEllipse(QPoint(dotCenterX, dotCenterY), dotRadius, dotRadius);
		painter->setPen(Pal::ON_SURFACE_VARIANT);
		painter->drawText(QRect(dotCenterX + 8, pill.top(), pill.right() - (dotCenterX + 8) - 8, pill.height()), Qt::AlignVCenter | Qt::AlignHCenter, text);
	} else if (text.startsWith("Analyzing") || text.startsWith("Starting") || text.startsWith("Downloading")) {
		// in-progress chip with a spinning arrow (angle driven by the table's timer)
		const QColor foregroundColor = Pal::PRIMARY;
		const int textWidth = fontMetrics.horizontalAdvance(text);
		QRect chip(0, 0, textWidth + 38, pillHeight);
		chip.moveCenter(content.center());
		painter->setPen(Qt::NoPen);
		painter->setBrush(Pal::PRIMARY_CONTAINER);
		painter->drawRoundedRect(chip, pillHeight / 2.0, pillHeight / 2.0);
		const QPointF pivot(chip.left() + 16, chip.center().y());
		const auto* table = TableFor(option);
		const int angle = table ? table->property("spinAngle").toInt() : 0;
		Ui::DrawSpinningGlyph(painter, "autorenew", 14, foregroundColor, pivot, angle);
		painter->setPen(foregroundColor);
		painter->drawText(QRect(int(pivot.x()) + 11, chip.top(), chip.right() - int(pivot.x()) - 17, chip.height()), Qt::AlignVCenter | Qt::AlignLeft, text);
	} else {
		// filled semantic chip (Status / Δ)
		// Sync-column chips are compact (no icon); real status chips carry an icon.
		const bool statusChip = text != "in sync" && text != "changed" && text != "source missing";
		QString iconName;
		if (text == "Ready") {
			iconName = "check_circle";
		} else if (text == "Update available") {
			iconName = "new_releases";
		} else if (text == "Open in IDA") {
			iconName = "open_in_new";
		} else if (text == "Not analyzed" || text == "Re-analyze") {
			iconName = "autorenew";
		} else {
			iconName = "close"; // No binary / Source missing
		}
		const bool iconFilled = (text == "Ready" || text == "Update available");
		const int textWidth = fontMetrics.horizontalAdvance(text);
		const int pillWidth = textWidth + (statusChip ? 38 : 22);
		QRect chip(0, 0, pillWidth, pillHeight);
		chip.moveCenter(content.center());
		painter->setPen(Qt::NoPen);
		painter->setBrush(ChipBg(text));
		painter->drawRoundedRect(chip, pillHeight / 2.0, pillHeight / 2.0);
		const QColor foregroundColor = ChipColor(text);
		painter->setPen(foregroundColor);
		if (statusChip) {
			const QRect iconRect(chip.left() + 9, chip.center().y() - 7, 14, 14);
			Ui::Icon(iconName, foregroundColor, 14, iconFilled).paint(painter, iconRect);
			painter->drawText(QRect(iconRect.right() + 4, chip.top(), chip.right() - iconRect.right() - 10, chip.height()), Qt::AlignVCenter | Qt::AlignLeft, text);
		} else {
			painter->drawText(chip, Qt::AlignCenter, text);
		}
	}
	painter->restore();
}

void TrailingInsetDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
	QStyleOptionViewItem background(option);
	initStyleOption(&background, index);
	background.text.clear();
	background.icon = QIcon();
	background.features &= ~QStyleOptionViewItem::HasDisplay;
	background.features &= ~QStyleOptionViewItem::HasDecoration;
	background.features &= ~QStyleOptionViewItem::HasCheckIndicator;
	const QStyle* itemStyle = option.widget ? option.widget->style() : QApplication::style();
	itemStyle->drawControl(QStyle::CE_ItemViewItem, &background, painter, option.widget);

	QStyleOptionViewItem content(option);
	content.rect = ContentRect(option, index);
	QStyledItemDelegate::paint(painter, content, index);
}

QSize ChipDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
	QSize size = QStyledItemDelegate::sizeHint(option, index);
	size.setHeight(qMax(size.height(), 34));
	return size;
}

void TagDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
	const QString text = index.data(Qt::DisplayRole).toString();
	if (text.isEmpty()) {
		QStyledItemDelegate::paint(painter, option, index);
		return;
	}
	// Pill colour comes from config through the item. The fallback covers a row
	// being constructed before a newly generated colour has been saved.
	QColor backgroundColor = index.data(Qt::UserRole).value<QColor>();
	if (!backgroundColor.isValid()) {
		backgroundColor = Pal::AutoTagBg(text);
	}
	const QColor foregroundColor = Pal::PillFg(backgroundColor);

	painter->save();
	painter->setRenderHint(QPainter::Antialiasing);
	PaintCellSurface(painter, option, index);
	QFont font = option.font;
	font.setPixelSize(12);
	font.setWeight(QFont::Medium);
	painter->setFont(font);
	const QFontMetrics fontMetrics(font);
	const int pillHeight = qMin(option.rect.height() - 12, 24);
	const int pillWidth = fontMetrics.horizontalAdvance(text) + 22;
	QRect chip(0, 0, pillWidth, pillHeight);
	chip.moveCenter(option.rect.center());
	painter->setPen(Qt::NoPen);
	painter->setBrush(backgroundColor);
	painter->drawRoundedRect(chip, pillHeight / 2.0, pillHeight / 2.0);
	painter->setPen(foregroundColor);
	painter->drawText(chip, Qt::AlignCenter, text);
	painter->restore();
}

QWidget* PortOffsetDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex&) const {
	auto* editor = new QSpinBox(parent);
	editor->setRange(-60000, 65535);					   // tag offsets (can be negative) + absolute ports
	editor->setButtonSymbols(QAbstractSpinBox::NoButtons); // no up/down arrows — type the value
	editor->setFrame(false);
	return editor;
}

void PortOffsetDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const {
	static_cast<QSpinBox*>(editor)->setValue(index.data(Qt::EditRole).toInt());
}

void PortOffsetDelegate::setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const {
	auto* spin = static_cast<QSpinBox*>(editor);
	spin->interpretText();
	model->setData(index, spin->value(), Qt::EditRole);
}
