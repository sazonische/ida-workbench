#pragma once
#include <QStyledItemDelegate>

// Default cells (checkbox/module/size) with the same full-row hover and
// separator treatment as the custom chip delegates.
class RowDelegate : public QStyledItemDelegate {
	Q_OBJECT
public:
	using QStyledItemDelegate::QStyledItemDelegate;
	void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
};

// Paints the selection column's checkbox as the same rounded M3 box the header
// uses, so the per-row check is visible under the custom dark theme (the style's
// default indicator renders invisibly against it). Toggling stays with the base
// editorEvent / the table's row-click handler.
class CheckDelegate : public QStyledItemDelegate {
	Q_OBJECT
public:
	using QStyledItemDelegate::QStyledItemDelegate;
	void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
};

// Paints Status / Δ values ("up-to-date", "updated", "diff", …) as a filled M3
// chip, and Server values ("UP"/"down") as an outlined pill with a status dot.
class ChipDelegate : public QStyledItemDelegate {
	Q_OBJECT
public:
	using QStyledItemDelegate::QStyledItemDelegate;
	void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
	QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
};

// Paints the Tag value as a rounded pill (primary tag = purple, others = grey).
// The item's Qt::UserRole bool marks the primary tag.
class TagDelegate : public QStyledItemDelegate {
	Q_OBJECT
public:
	using QStyledItemDelegate::QStyledItemDelegate;
	void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
};

// Numeric editor used by the settings port-offset column.  It prevents the
// silent "abc" -> 0 conversion performed by QString::toInt().
class PortOffsetDelegate : public QStyledItemDelegate {
	Q_OBJECT
public:
	using QStyledItemDelegate::QStyledItemDelegate;
	QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
	void setEditorData(QWidget* editor, const QModelIndex& index) const override;
	void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override;
};

// Keeps the trailing cell background full-width while moving only its content
// away from the overlay scrollbar.
class TrailingInsetDelegate : public QStyledItemDelegate {
	Q_OBJECT
public:
	using QStyledItemDelegate::QStyledItemDelegate;
	void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};
