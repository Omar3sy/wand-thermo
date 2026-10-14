/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "display_manager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

#include "font_render.h"
#include "eurotronic_logo.h"

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

namespace
{
constexpr chip::EndpointId kThermostatEndpoint = 1;
constexpr k_timeout_t kSetpointDisplayDuration = K_SECONDS(15);

const struct device *sDisplay = DEVICE_DT_GET(DT_NODELABEL(eink_display));

/* Drawn with the 5x7 font at scale 2 (10x14 px/char) - '*' and '+' render as
 * plain glyphs, no custom bitmap icon needed. */
constexpr char kFlameIconMarker = '*';
constexpr char kLinkIconMarker = '+';

/* Logo fest oben, dauerhaft sichtbar (bei jedem Full-Refresh neu gezeichnet,
 * da der Treiber nur Full-Frame-Writes kann). 190x57px, Rand 5px links. */
constexpr uint16_t kLogoX = 5;
constexpr uint16_t kLogoY = 3;

/* Scale 4 = 20x28px/Zeichen (Basis 5x7px) - deutlich besser lesbar auf dem
 * 200x200px-Panel aus normalem Betrachtungsabstand. Bei dieser Groesse
 * passen nur noch ca. 8 Zeichen/Zeile (195px nutzbare Breite / 24px
 * Zeichenbreite inkl. 1px-Abstand*scale), deshalb 3 kurze Zeilen statt
 * vorher 2 laengere - siehe RenderLine1-3(). Y-Positionen unter das Logo
 * verschoben (Logo endet bei kLogoY+EUROTRONIC_LOGO_HEIGHT=60, +10px Rand). */
constexpr uint8_t kTextScale = 4;
constexpr uint16_t kLine1Y = 70;
constexpr uint16_t kLine2Y = 108;
constexpr uint16_t kLine3Y = 146;

/* Eigene, groessere Ansicht fuer den Setpoint-Button: nur Logo + zweizeilig
 * "Setpoint:" (Label, kleiner) darueber und der Wert gross darunter - keine
 * Temperatur/Feuchtigkeit/Batterie-Zeilen - siehe RenderFrame(). */
constexpr uint8_t kSetpointLabelScale = 3;
constexpr uint16_t kSetpointLabelY = 75;
constexpr uint8_t kSetpointValueScale = 6;
constexpr uint16_t kSetpointValueY = 115;
constexpr uint16_t kTextX = 5;

} /* namespace */

CHIP_ERROR DisplayManager::Init()
{
	if (!device_is_ready(sDisplay)) {
		LOG_ERR("E-paper device not ready");
		return CHIP_ERROR_INCORRECT_STATE;
	}

	struct display_capabilities caps;
	display_get_capabilities(sDisplay, &caps);
	mPanelWidth = caps.x_resolution;
	mPanelHeight = caps.y_resolution;
	mFramebufferSize = static_cast<size_t>(mPanelWidth) * mPanelHeight / 8;
	mFramebuffer = static_cast<uint8_t *>(k_malloc(mFramebufferSize));
	if (mFramebuffer == nullptr) {
		LOG_ERR("No memory for display framebuffer (%zu bytes)", mFramebufferSize);
		return CHIP_ERROR_NO_MEMORY;
	}

	/* Minimal white-balance test (Teil 1): verifies SPI/busy-pin wiring and
	 * panel init before any text rendering is attempted. Draws one blank
	 * (all-white) full-frame refresh. */
	memset(mFramebuffer, 0xFF, mFramebufferSize);
	struct display_buffer_descriptor desc = {
		.buf_size = mFramebufferSize,
		.width = mPanelWidth,
		.height = mPanelHeight,
		.pitch = mPanelWidth,
	};
	int ret = display_write(sDisplay, 0, 0, &desc, mFramebuffer);
	if (ret != 0) {
		LOG_ERR("E-paper white-balance test failed: %d", ret);
		return CHIP_ERROR_INCORRECT_STATE;
	}
	LOG_INF("E-paper white-balance test done");

	k_work_init_delayable(&mSetpointTimeout, DisplayManager::SetpointTimeoutHandler);
	k_work_init_delayable(&mModeTimeout, DisplayManager::ModeTimeoutHandler);

	/* Erster Render immer als Full-Frame (zeichnet Logo + alle drei Zeilen
	 * zusammen) - Partial-Refresh setzt voraus, dass der aktuelle
	 * Panel-Inhalt bereits mit mFramebuffer uebereinstimmt, was beim
	 * allerersten Aufbau noch nicht der Fall ist. */
	RenderLine1Text();
	RenderLine2Text();
	RenderLine3Text();
	RenderFrame();

	return CHIP_NO_ERROR;
}

void DisplayManager::RenderFrame()
{
	if (!device_is_ready(sDisplay) || mFramebuffer == nullptr) {
		return;
	}

	memset(mFramebuffer, 0xFF, mFramebufferSize);
	font_draw_bitmap(mFramebuffer, mPanelWidth, mPanelHeight, kLogoX, kLogoY, eurotronic_logo_bitmap,
			 EUROTRONIC_LOGO_WIDTH, EUROTRONIC_LOGO_HEIGHT);

	if (mShowingSetpoint) {
		/* Eigene Ansicht: Logo + Label "Setpoint:" + grosser Wert darunter
		 * (mLine1Text enthaelt in diesem Zustand bereits z.B. "21.5C",
		 * siehe RenderLine1()) - keine Temperatur/Feuchtigkeit/Batterie-
		 * Zeilen. */
		font_draw_string(mFramebuffer, mPanelWidth, mPanelHeight, kTextX, kSetpointLabelY, "Setpoint:",
				 kSetpointLabelScale, 1);
		font_draw_string(mFramebuffer, mPanelWidth, mPanelHeight, kTextX, kSetpointValueY, mLine1Text,
				 kSetpointValueScale, 1);
	} else {
		font_draw_string(mFramebuffer, mPanelWidth, mPanelHeight, kTextX, kLine1Y, mLine1Text, kTextScale, 1);
		font_draw_string(mFramebuffer, mPanelWidth, mPanelHeight, kTextX, kLine2Y, mLine2Text, kTextScale, 1);
		font_draw_string(mFramebuffer, mPanelWidth, mPanelHeight, kTextX, kLine3Y, mLine3Text, kTextScale, 1);
	}

	/* "Prev"-Puffer synchronisieren, damit der naechste RenderLinePartial()-
	 * Aufruf gegen das vergleicht, was jetzt wirklich auf dem Panel steht -
	 * sonst wuerde er faelschlich einen alten Diff gegenueber einem laengst
	 * ueberholten Stand berechnen. */
	strncpy(mLine1TextPrev, mLine1Text, sizeof(mLine1TextPrev) - 1);
	mLine1TextPrev[sizeof(mLine1TextPrev) - 1] = '\0';
	strncpy(mLine2TextPrev, mLine2Text, sizeof(mLine2TextPrev) - 1);
	mLine2TextPrev[sizeof(mLine2TextPrev) - 1] = '\0';
	strncpy(mLine3TextPrev, mLine3Text, sizeof(mLine3TextPrev) - 1);
	mLine3TextPrev[sizeof(mLine3TextPrev) - 1] = '\0';

	struct display_buffer_descriptor desc = {
		.buf_size = mFramebufferSize,
		.width = mPanelWidth,
		.height = mPanelHeight,
		.pitch = mPanelWidth,
	};
	int ret = display_write(sDisplay, 0, 0, &desc, mFramebuffer);
	if (ret != 0) {
		LOG_ERR("E-paper refresh failed: %d", ret);
	}
}

void DisplayManager::ShowSetpointTemporarily()
{
	using namespace chip::app::Clusters::Thermostat::Attributes;

	int16_t setpoint = 0;
	if (OccupiedHeatingSetpoint::Get(kThermostatEndpoint, &setpoint) ==
	    chip::Protocols::InteractionModel::Status::Success) {
		mLastSetpoint = setpoint;
	}

	mShowingSetpoint = true;
	RenderLine1();

	k_work_reschedule(&mSetpointTimeout, kSetpointDisplayDuration);
}

void DisplayManager::SetpointTimeoutHandler(struct k_work *work)
{
	DisplayManager &self = DisplayManager::Instance();
	self.mShowingSetpoint = false;
	self.RenderLine1();
}

void DisplayManager::ShowModeTemporarily(bool isHeating)
{
	mLastModeIsHeating = isHeating;
	mShowingMode = true;
	RenderLine1();

	k_work_reschedule(&mModeTimeout, K_SECONDS(3));
}

void DisplayManager::ModeTimeoutHandler(struct k_work *work)
{
	DisplayManager &self = DisplayManager::Instance();
	self.mShowingMode = false;
	self.RenderLine1();
}

void DisplayManager::UpdateLocalTemperature(int16_t centiDegrees)
{
	mLastLocalTemp = centiDegrees;

	/* Refresh nur bei >= 0.5 C Aenderung seit dem letzten angezeigten Wert -
	 * verhindert unnoetige Full-Refreshes (Flackern, Panel-Verschleiss) bei
	 * jeder kleinen SCD41-Messschwankung. Setpoint/Mode-Overlays bleiben
	 * unveraendert sofort reaktionsfreudig (eigener Trigger-Pfad). */
	constexpr int16_t kTempRefreshThreshold = 50; /* 0.5 C in centiDegrees */
	bool thresholdCrossed = abs(centiDegrees - mLastDisplayedTemp) >= kTempRefreshThreshold;

	if (!mShowingSetpoint && !mShowingMode && thresholdCrossed) {
		mLastDisplayedTemp = centiDegrees;
		RenderLine1();
	}
}

void DisplayManager::UpdateHumidity(uint16_t centiPercent)
{
	mLastHumidity = centiPercent;
	RenderLine2();
}

void DisplayManager::UpdateBattery(uint8_t percentHalfUnits)
{
	mLastBatteryPercent = percentHalfUnits;
	RenderLine3();
}

void DisplayManager::UpdateHeatingDemand(uint8_t percent)
{
	mLastHeatingDemand = percent;
	RenderLine3();
}

void DisplayManager::RenderLine1Text()
{
	/* Bei kTextScale=4 passen max. ~8 Zeichen/Zeile - deshalb hier nur
	 * noch Temperatur allein (Luftfeuchtigkeit ist jetzt Zeile 2). */
	if (mShowingMode) {
		snprintf(mLine1Text, sizeof(mLine1Text), "%s", mLastModeIsHeating ? "HEATING" : "OFF");
	} else if (mShowingSetpoint) {
		/* Ohne "S:"-Praefix: bei kSetpointScale=6 (36px/Zeichen) passen nur
		 * ca. 5 Zeichen in die 195px nutzbare Panelbreite - "21.5C" (5
		 * Zeichen) reicht, "S:21.5C" (7 Zeichen) waere zu breit. Der
		 * Kontext ("Setpoint") ist durch die eigene Vollbild-Ansicht
		 * ohnehin klar, siehe RenderFrame(). */
		snprintf(mLine1Text, sizeof(mLine1Text), "%d.%dC", mLastSetpoint / 100,
			 abs(mLastSetpoint % 100) / 10);
	} else {
		snprintf(mLine1Text, sizeof(mLine1Text), "T:%d.%dC", mLastLocalTemp / 100,
			 abs(mLastLocalTemp % 100) / 10);
	}
}

void DisplayManager::RenderLine1()
{
	RenderLine1Text();

	/* Setpoint/Mode-Overlay hat ein komplett anderes Layout als die normale
	 * 3-Zeilen-Ansicht (eigene Schriftgroessen/Positionen, siehe
	 * RenderFrame()). Nur beim tatsaechlichen Wechsel zwischen den Layouts
	 * (rein in bzw. raus aus dem Overlay) ist ein voller RenderFrame()
	 * noetig - sonst blieben Reste der jeweils anderen Ansicht stehen.
	 * Bleibt das Layout gleich (z.B. Setpoint-Wert aendert sich waehrend
	 * das Overlay schon angezeigt wird), reicht ein Partial-Update des
	 * betroffenen Textes. */
	bool wantOverlay = mShowingMode || mShowingSetpoint;

	if (wantOverlay != mLastRenderWasOverlay) {
		RenderFrame();
		mLastRenderWasOverlay = wantOverlay;
		mPartialRefreshCount = 0;
	} else if (mShowingSetpoint) {
		RenderSetpointValuePartial();
	} else if (mShowingMode) {
		/* Mode-Text aendert sich praktisch nie waehrend das Overlay schon
		 * steht (kein Hoch-/Runterzaehlen wie beim Setpoint) - einfachheits-
		 * halber hier kein eigener Partial-Pfad. */
		RenderFrame();
	} else {
		RenderLinePartial(0);
	}
}

void DisplayManager::RenderLine2Text()
{
	snprintf(mLine2Text, sizeof(mLine2Text), "H:%u%%", mLastHumidity / 100);
}

void DisplayManager::RenderLine2()
{
	RenderLine2Text();

	if (mLastRenderWasOverlay) {
		/* Waehrend das Setpoint/Mode-Overlay aktiv ist, zeigt Zeile 2
		 * ohnehin nichts an (siehe RenderFrame()) - nichts zu tun, der
		 * naechste Full-Refresh beim Verlassen des Overlays uebernimmt das. */
		return;
	}

	RenderLinePartial(1);
}

void DisplayManager::RenderLine3Text()
{
	if (mTrvOffline) {
		snprintf(mLine3Text, sizeof(mLine3Text), "OFFLINE");
	} else {
		if (!mSystemModeIsHeating) {
			/* System off: heating-demand percentage is meaningless, show the mode instead */
			snprintf(mLine3Text, sizeof(mLine3Text), "B:%d%% off", mLastBatteryPercent / 2);
		} else if (mLastHeatingDemand > 0) {
			snprintf(mLine3Text, sizeof(mLine3Text), "B:%d%% %c", mLastBatteryPercent / 2,
				 kFlameIconMarker);
		} else {
			snprintf(mLine3Text, sizeof(mLine3Text), "B:%d%%", mLastBatteryPercent / 2);
		}
		if (mTrvLinked) {
			const size_t len = strlen(mLine3Text);
			snprintf(mLine3Text + len, sizeof(mLine3Text) - len, "%c", kLinkIconMarker);
		}
	}
}

void DisplayManager::RenderLine3()
{
	RenderLine3Text();

	if (mLastRenderWasOverlay) {
		return;
	}

	RenderLinePartial(2);
}

void DisplayManager::RenderLinePartial(uint8_t lineIndex)
{
	uint16_t lineY;
	char *text;
	char *prevText;
	size_t textCapacity;

	switch (lineIndex) {
	case 0:
		lineY = kLine1Y;
		text = mLine1Text;
		prevText = mLine1TextPrev;
		textCapacity = sizeof(mLine1Text);
		break;
	case 1:
		lineY = kLine2Y;
		text = mLine2Text;
		prevText = mLine2TextPrev;
		textCapacity = sizeof(mLine2Text);
		break;
	case 2:
		lineY = kLine3Y;
		text = mLine3Text;
		prevText = mLine3TextPrev;
		textCapacity = sizeof(mLine3Text);
		break;
	default:
		return;
	}

	RenderTextPartial(text, prevText, textCapacity, lineY, kTextScale);
}

void DisplayManager::RenderSetpointValuePartial()
{
	/* Waehrend die Setpoint-Vollbild-Ansicht aktiv ist, soll sich NUR der
	 * Zahlenwert aendern duerfen (z.B. 21.5C -> 21.6C per Tastendruck), ohne
	 * jedes Mal das ganze Panel neu zu zeichnen - Logo und "Setpoint:"-Label
	 * bleiben dabei unberuehrt stehen. Gleiche Diff-Logik wie bei den
	 * normalen Zeilen, nur mit der groesseren Setpoint-Schrift/-Position. */
	RenderTextPartial(mLine1Text, mLine1TextPrev, sizeof(mLine1Text), kSetpointValueY, kSetpointValueScale);
}

void DisplayManager::RenderTextPartial(char *text, char *prevText, size_t textCapacity, uint16_t y,
				       uint8_t scale)
{
	if (!device_is_ready(sDisplay) || mFramebuffer == nullptr) {
		return;
	}

	if (strcmp(text, prevText) == 0) {
		return; /* Text unveraendert - kein Refresh noetig */
	}

	/* Zeichen-Diff: nur die Spalten der tatsaechlich geaenderten Zeichen
	 * aktualisieren (z.B. nur die "4" bei "H:54%" -> "H:55%", oder nur die
	 * "5" bei einer Setpoint-Aenderung von 21.5C -> 21.6C), statt der
	 * ganzen Zeile. kGlyphWidth/kCharAdvance muessen zu font5x7.h/
	 * font_draw_string()'s Formel passen ((FONT5X7_WIDTH+1)*scale). */
	constexpr uint16_t kGlyphWidth = 5;
	uint16_t charAdvance = (kGlyphWidth + 1) * scale;
	uint16_t glyphPixelWidth = kGlyphWidth * scale;

	size_t newLen = strlen(text);
	size_t prevLen = strlen(prevText);
	size_t maxLen = newLen > prevLen ? newLen : prevLen;
	bool firstRenderOfThisText = (prevText[0] == '\0');

	size_t firstDiff = 0;
	size_t lastDiff = (newLen > 0) ? newLen - 1 : 0;

	if (!firstRenderOfThisText) {
		firstDiff = maxLen;
		for (size_t i = 0; i < maxLen; i++) {
			char oldC = (i < prevLen) ? prevText[i] : '\0';
			char newC = (i < newLen) ? text[i] : '\0';

			if (oldC != newC) {
				if (firstDiff == maxLen) {
					firstDiff = i;
				}
				lastDiff = i;
			}
		}
	}

	/* Zeilenband: 2px Rand oberhalb der Glyphen, Hoehe = Glyphenhoehe (7*scale)
	 * + Rand - deckt den Text vollstaendig ab. */
	uint16_t bandY = y - 2;
	uint16_t bandH = 7 * scale + 4;

	if (bandY + bandH > mPanelHeight) {
		bandH = mPanelHeight - bandY;
	}

	uint16_t startPx = kTextX + firstDiff * charAdvance;
	uint16_t endPx = kTextX + lastDiff * charAdvance + glyphPixelWidth - 1;

	/* Horizontal auf 8px-Grenzen ausrichten - vom Treiber fuer Partial-
	 * Refresh verlangt (siehe eink154_set_partial_window()). */
	uint16_t alignedStart = startPx & static_cast<uint16_t>(~0x07);
	uint16_t alignedEnd = endPx | 0x07;

	if (alignedEnd >= mPanelWidth) {
		alignedEnd = mPanelWidth - 1;
	}
	uint16_t width = alignedEnd - alignedStart + 1;

	size_t bytesPerRow = (mPanelWidth + 7) / 8;
	size_t colByteStart = alignedStart / 8;
	size_t colBytes = (width + 7) / 8;

	/* Nur den betroffenen Spaltenbereich in diesem Zeilenband loeschen
	 * (weiss) und neu zeichnen - nicht die ganze Zeile. */
	for (uint16_t row = 0; row < bandH; row++) {
		memset(&mFramebuffer[(bandY + row) * bytesPerRow + colByteStart], 0xFF, colBytes);
	}
	for (size_t i = firstDiff; i <= lastDiff; i++) {
		char c = (i < newLen) ? text[i] : ' ';
		uint16_t charX = kTextX + i * charAdvance;

		font_draw_char(mFramebuffer, mPanelWidth, mPanelHeight, charX, y, c, scale, 1);
	}

	strncpy(prevText, text, textCapacity - 1);
	prevText[textCapacity - 1] = '\0';

	mPartialRefreshCount++;
	if (mPartialRefreshCount >= kMaxPartialRefreshesBeforeFull) {
		/* Ghosting-Vorbeugung: nach N Partial-Updates einen echten
		 * Full-Refresh einschieben (siehe Recherche-Notiz zu Partial-Refresh). */
		mPartialRefreshCount = 0;
		RenderFrame();
		return;
	}

	struct display_buffer_descriptor desc = {
		.buf_size = static_cast<uint32_t>(colBytes) * bandH,
		.width = width,
		.height = bandH,
		.pitch = mPanelWidth, /* Framebuffer-Zeilen bleiben volle Panelbreite */
	};
	int ret = display_write(sDisplay, alignedStart, bandY, &desc,
				&mFramebuffer[bandY * bytesPerRow + colByteStart]);
	if (ret != 0) {
		LOG_ERR("E-paper partial refresh failed: %d", ret);
	}
}
