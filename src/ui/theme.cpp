#include "theme.h"

#include <QApplication>
#include <QFile>
#include <QString>
#include <QStyleHints>

namespace {

	// Material 3 color roles (baseline scheme) for one mode — the "data" behind the
	// QSS in resources/styles/material3.qss.
	struct M3 {
		const char *primary, *onPrimary, *primaryHover, *primaryPressed;
		const char *secondaryContainer, *onSecondaryContainer;
		const char *surface, *surfaceContainer, *surfaceContainerLow, *surfaceContainerHigh;
		const char *onSurface, *onSurfaceVariant, *outline, *outlineVariant;
		const char *hoverOverlay, *disabledBg, *disabledFg;
		const char* checkImage; // themed checkbox tick
	};

	const M3 LIGHT_THEME{
		"#65558F",
		"#FFFFFF",
		"#584979",
		"#4F4379",
		"#E8DEF8",
		"#4A4459",
		"#FDF7FF",
		"#F4EDFA",
		"#FFFFFF",
		"#F4EDFA",
		"#1D1B20",
		"#49454F",
		"#CAC4D0",
		"#E7E0EC",
		"rgba(101,85,143,0.08)",
		"rgba(29,27,32,0.12)",
		"rgba(29,27,32,0.38)",
		":/icons/check_light.png",
	};

	QString StyleSheet(const M3& theme) {
		QFile styleFile(":/styles/material3.qss");
		if (!styleFile.open(QIODevice::ReadOnly)) {
			return {};
		}
		QString stylesheetContent = QString::fromUtf8(styleFile.readAll());

		struct KeyValue {
			const char *key, *value;
		};
		const KeyValue replacements[] = {
			{"%primary%", theme.primary},
			{"%onPrimary%", theme.onPrimary},
			{"%primaryHover%", theme.primaryHover},
			{"%primaryPressed%", theme.primaryPressed},
			{"%secondaryContainer%", theme.secondaryContainer},
			{"%onSecondaryContainer%", theme.onSecondaryContainer},
			{"%surface%", theme.surface},
			{"%surfaceContainer%", theme.surfaceContainer},
			{"%surfaceContainerLow%", theme.surfaceContainerLow},
			{"%surfaceContainerHigh%", theme.surfaceContainerHigh},
			{"%onSurface%", theme.onSurface},
			{"%onSurfaceVariant%", theme.onSurfaceVariant},
			{"%outline%", theme.outline},
			{"%outlineVariant%", theme.outlineVariant},
			{"%hoverOverlay%", theme.hoverOverlay},
			{"%disabledBg%", theme.disabledBg},
			{"%disabledFg%", theme.disabledFg},
			{"%checkImage%", theme.checkImage},
		};
		for (const KeyValue& entry : replacements) {
			stylesheetContent.replace(QLatin1String(entry.key), QLatin1String(entry.value));
		}
		return stylesheetContent;
	}

} // namespace

void ApplyMaterial3(QApplication& app) {
	app.setStyle("Fusion"); // predictable base for QSS across platforms
	// The supplied HTML is a light Material 3 design.  Several semantic chips
	// and generated icon pixmaps use that exact palette, so following the OS
	// dark mode here produced a half-light/half-dark interface.  Apply the
	// reference scheme consistently until a complete dark reference exists.
	app.styleHints()->setColorScheme(Qt::ColorScheme::Light);
	app.setStyleSheet(StyleSheet(LIGHT_THEME));
}
