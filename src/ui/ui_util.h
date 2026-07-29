#pragma once
#include <QColor>
#include <QFont>
#include <QHeaderView>
#include <QIcon>
#include <QPixmap>
#include <QList>
#include <QString>
#include <QStringList>

class QPushButton;
class QLabel;
class QWidget;
class QAbstractScrollArea;
class QComboBox;
class QGroupBox;
class QMenu;
class QPainter;
class QPointF;

class TrailingInsetHeader : public QHeaderView {
public:
	explicit TrailingInsetHeader(Qt::Orientation orientation, QWidget* parent = nullptr);
	void SetTrailingInset(int inset);

protected:
	void paintSection(QPainter* painter, const QRect& rect, int logicalIndex) const override;

private:
	int _trailingInset = 0;
};

namespace Ui {

	// --- Material Symbols icons ---
	void LoadIcons();							   // register fonts (call once in main)
	[[nodiscard]] QString Sym(const QString& name); // glyph char for an icon name
	[[nodiscard]] QFont SymbolFont(int px, bool filled = false); // Material Symbols font at a pixel size
	[[nodiscard]] QIcon Icon(const QString& name, const QColor& color, int px = 18, bool filled = false);
	[[nodiscard]] QIcon SwatchIcon(const QColor& color, int px = 16);

	// Spin helpers — one place that rotates an icon glyph about the centre of its own
	// ink (glyphs are not centred on the em box, so a naive rotate makes them orbit).
	// DrawSpinningGlyph paints onto an existing painter at `center`; SpinningIcon
	// bakes the same into a standalone QIcon for button use. Shared by the Refresh
	// button and all in-progress status chips.
	void DrawSpinningGlyph(QPainter* p, const QString& name, int px, const QColor& color, const QPointF& center, qreal angle, bool filled = false);
	[[nodiscard]] QIcon SpinningIcon(const QString& name, int px, const QColor& color, qreal angle, bool filled = false);
	// A single-colour ("monochrome") render of an image resource: the image's alpha
	// shape filled solid with `color`. Used to brand-tint the app icon.
	[[nodiscard]] QPixmap TintedPixmap(const QString& resourcePath, const QColor& color, int px);
	[[nodiscard]] QLabel* IconLabel(const QString& name, int px, const QColor& color, bool filled = false);

	// A themed button. variant = nullptr (filled primary) | "tonal" | "text".
	[[nodiscard]] QPushButton* Button(const QString& text, const char* variant = nullptr);

	// A themed button with a leading Material Symbols icon.
	[[nodiscard]] QPushButton* IconButton(const QString& iconName, const QString& text, const char* variant = nullptr, bool filledIcon = false);

	// Shared Material 3 exposed dropdown and its floating-label container.
	[[nodiscard]] QComboBox* ComboBox(QWidget* parent = nullptr);
	[[nodiscard]] QGroupBox* Field(const QString& label, QWidget* content, QWidget* parent = nullptr);
	[[nodiscard]] QGroupBox* ComboField(const QString& label, QComboBox* combo, QWidget* parent = nullptr);
	[[nodiscard]] QMenu* Menu(QWidget* parent = nullptr);
	[[nodiscard]] QWidget* SegmentedControl(const QList<QPushButton*>& buttons, int width = 0, QWidget* parent = nullptr);
	[[nodiscard]] QWidget* VerticalDivider(int height, QWidget* parent = nullptr);

	// A round icon-only button (M3 "standard icon button"). Paints its own
	// antialiased circular state layer — QSS border-radius cannot produce a smooth
	// circle — and shows `tooltip` in a soft rounded bubble instead of the native
	// square QToolTip. `stateColor` tints the hover/pressed layer (neutral for most
	// actions, error red for destructive ones). setIcon() keeps working (spinners).
	[[nodiscard]] QPushButton* RoundIconButton(const QString& iconName, const QString& tooltip, const QColor& iconColor, const QColor& stateColor, int diameter = 40, int iconPx = 20);

	// Rounded surfaces need both clipping and a topmost outline: native Qt child
	// widgets otherwise paint over the parent's QSS border.
	void ClipRounded(QWidget* target, qreal radius);
	QWidget* RoundedOutline(QWidget* target, qreal radius, const QColor& color);
	QWidget* RoundedSurface(QWidget* target, qreal radius, const QColor& color);
	// Mirrors the hidden native scrollbar with a slim overlay that does not
	// reserve a content gutter. `topInset` keeps table overlays below headers.
	void OverlayVerticalScrollBar(QAbstractScrollArea* target, int topInset = 2);

	// A muted, word-wrapped hint label (objectName "appSubtitle").
	[[nodiscard]] QLabel* Hint(const QString& text);

	// Modal editor for a list of folders (Add folder… / Remove). Returns true and
	// fills `dirs` on Save; returns false (dirs untouched) on Cancel.
	bool EditFolderList(QWidget* parent, const QString& title, const QString& intro, QStringList& dirs);

} // namespace Ui
