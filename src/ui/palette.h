#pragma once
#include <QColor>
#include <QString>

// Single source of truth for the colours used by the C++ painting code
// (delegates, main window, settings). The QSS side keeps its own %token% copies
// in theme.cpp — keep the two in sync when a role changes.
namespace Pal {

	// --- core Material 3 roles ---
	inline const QColor PRIMARY{"#65558F"};
	inline const QColor ON_PRIMARY{"#FFFFFF"};
	inline const QColor PRIMARY_CONTAINER{"#EADDFF"};
	inline const QColor ON_PRIMARY_CONTAINER{"#4F378A"};
	inline const QColor SECONDARY_CONTAINER{"#E8DEF8"};
	inline const QColor ON_SECONDARY_CONTAINER{"#4A4459"};
	inline const QColor TERTIARY_CONTAINER{"#FFD8E4"};
	inline const QColor ON_TERTIARY_CONTAINER{"#633B48"};
	inline const QColor ERROR_CONTAINER{"#F9DEDC"};
	inline const QColor ERROR_COLOR{"#B3261E"};
	inline const QColor OUTLINE{"#79747E"};
	inline const QColor OUTLINE_VARIANT{"#E7E0EC"};
	inline const QColor ON_SURFACE_VARIANT{"#49454F"};
	inline const QColor SURFACE{"#FDF7FF"}; // app background (mirrors %surface% in theme.cpp)

	// --- table surfaces / lines ---
	inline const QColor ROW_SEPARATOR{"#F1EBF5"};
	inline const QColor ROW_HOVER{"#F7F1FB"};
	inline const QColor ROW_SELECTED{234, 221, 255, 102}; // PRIMARY_CONTAINER @ 40%

	// --- semantic status chips (foreground / background) ---
	inline const QColor CHIP_GREEN_FG{"#1F5729"};
	inline const QColor CHIP_GREEN_BG{"#D3EFD5"};
	inline const QColor CHIP_BLUE_FG{"#0B57D0"};
	inline const QColor CHIP_BLUE_BG{"#D3E3FD"};
	inline const QColor CHIP_AMBER_FG{"#7A4F01"};
	inline const QColor CHIP_AMBER_BG{"#FFEFD2"};
	inline const QColor CHIP_GREY_FG{"#79747E"};
	inline const QColor CHIP_GREY_BG{"#ECE9EF"};

	// --- server pill ---
	inline const QColor PILL_OUTLINE{"#CAC4D0"};
	inline const QColor SERVER_UP{"#43A047"};
	inline const QColor SERVER_DOWN{"#B3AEBA"};

	// --- action-button icon tints (mirror the button colours in material3.qss) ---
	inline const QColor START_FG{"#33691E"};
	inline const QColor ANALYZE_FG{"#4A4459"};
	inline const QColor REPLACE_FG{"#633B48"};
	inline const QColor STOP_FG{"#B3261E"};

	// --- readiness chips / misc accents ---
	inline const QColor OK_GREEN{"#2E6B39"};
	inline const QColor SEG_ACTIVE{"#1D192B"};
	inline const QColor SEG_INACTIVE{"#49454F"};
	inline const QColor ICON_MUTED{"#93889F"}; // leading module-row icon

	// --- column-sort header tones (no arrow; the text darkness signals the state) ---
	inline const QColor SORT_INACTIVE{"#49454F"};  // off        (normal header colour)
	inline const QColor SORT_ASC{"#1D1B20"};	   // ascending  (dark font)
	inline const QColor SORT_DESC{"#B3AEBA"};	   // descending (light font)
	inline const QColor TITLE_TEXT{"#1D1B20"};	   // settings section titles
	inline const QColor PILL_SERVER_BG{"#F4EDFA"}; // "servers up" header pill background
	inline const QColor DIVIDER{"#CAC4D0"};		   // thin vertical separators / segmented OUTLINE

	// Deterministic default for a tag whose config has no colour yet. The resolved
	// value is persisted immediately (Qt6 qHash(QString) is process-randomised).
	[[nodiscard]] inline unsigned StableHash(const QString& stringToHash) {
		unsigned hashValue = 2166136261u;
		for (const QChar character : stringToHash) {
			hashValue ^= character.unicode();
			hashValue *= 16777619u;
		}
		return hashValue;
	}

	// A stable, pleasant pill background for a tag that has no explicit colour.
	[[nodiscard]] inline QColor AutoTagBg(const QString& tag) {
		static const QColor AUTO_COLORS[] = {
			QColor("#EADDFF"),
			QColor("#FFD8E4"),
			QColor("#D3EFD5"),
			QColor("#D3E3FD"),
			QColor("#FFEFD2"),
			QColor("#CCE8E6"),
			QColor("#F0DBF5"),
			QColor("#E3E9C8"),
		};
		return AUTO_COLORS[StableHash(tag) % (sizeof(AUTO_COLORS) / sizeof(AUTO_COLORS[0]))];
	}

	// Readable text colour for a pill of the given background: a darker tonal shade
	// for light backgrounds, white for dark ones.
	[[nodiscard]] inline QColor PillFg(const QColor& backgroundColor) {
		const double luminance = (0.299 * backgroundColor.red() + 0.587 * backgroundColor.green() + 0.114 * backgroundColor.blue()) / 255.0;
		if (luminance > 0.5) {
			return backgroundColor.darker(340);
		}
		return QColor("#FFFFFF");
	}

} // namespace Pal
