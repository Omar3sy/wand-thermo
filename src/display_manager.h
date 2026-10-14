/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#pragma once

#include <cstdint>

#include <lib/core/CHIPError.h>
#include <zephyr/kernel.h>

class DisplayManager {
public:
	CHIP_ERROR Init();

	static DisplayManager &Instance()
	{
		static DisplayManager sInstance;
		return sInstance;
	}

	/* Called from button handlers: shows setpoint for 15s, then falls back */
	void ShowSetpointTemporarily();

	/* Called after a System Mode toggle (Button0 double-click): shows
	 * "Mode: HEATING"/"Mode: OFF" for 3s, then falls back */
	void ShowModeTemporarily(bool isHeating);

	/* Called whenever LocalTemperature, Humidity, Battery or TRV heating
	 * demand values update */
	void UpdateLocalTemperature(int16_t centiDegrees);
	void UpdateHumidity(uint16_t centiPercent);
	void UpdateBattery(uint8_t percentHalfUnits);
	void UpdateHeatingDemand(uint8_t percent);

	/* Persistent (not temporary) System Mode state, reflected on line 2:
	 * suppresses the heating-demand percentage while the mode is Off. */
	void SetSystemModeIsHeating(bool isHeating)
	{
		mSystemModeIsHeating = isHeating;
		RenderLine2();
	}

	/* Called by the TRV poll failure/success tracking in sensor.cpp once
	 * consecutive failures cross the threshold (or a poll succeeds again),
	 * so the user sees "TRV offline" on the LCD instead of silently stale
	 * setpoint/heating-demand values. */
	void SetTrvOffline(bool offline)
	{
		mTrvOffline = offline;
		RenderLine3();
	}

	/* True while the TRV subscription is established, shown as a check mark on line 3. */
	void SetTrvLinked(bool linked)
	{
		mTrvLinked = linked;
		RenderLine3();
	}

private:
	static void SetpointTimeoutHandler(struct k_work *work);
	static void ModeTimeoutHandler(struct k_work *work);
	void RenderLine1();
	void RenderLine2();
	void RenderLine3();
	/* Berechnen nur den jeweiligen Zeilentext (mLineXText), ohne etwas zu
	 * rendern - getrennt von RenderLineX(), damit Init() den allerersten
	 * Aufbau als einzelnen Full-Frame-RenderFrame() machen kann. */
	void RenderLine1Text();
	void RenderLine2Text();
	void RenderLine3Text();
	/* Clears the framebuffer, draws mLine1Text/mLine2Text/mLine3Text into
	 * it with font_draw_string() and pushes one full-frame refresh. The
	 * e-paper driver only supports full-frame writes, so all three lines
	 * are always redrawn together, not line by line. */
	void RenderFrame();
	/* Zeichnet nur die angegebene Zeile (0/1/2) neu in den lokalen
	 * Framebuffer und schickt nur deren Zeilenband per Partial-Refresh
	 * (R90H/R91H/R92H) ans Panel - Logo und die anderen Zeilen bleiben auf
	 * dem Panel unveraendert stehen. Zaehlt mPartialRefreshCount hoch; nach
	 * kMaxPartialRefreshesBeforeFull wird stattdessen ein voller RenderFrame()
	 * erzwungen (Geisterbild-/Ghosting-Vorbeugung, siehe Recherche-Notiz zu
	 * Partial-Refresh). */
	void RenderLinePartial(uint8_t lineIndex);
	/* Wie RenderLinePartial(), aber fuer den Zahlenwert in der Setpoint-
	 * Vollbild-Ansicht (eigene Position/Schriftgroesse) - siehe RenderLine1(). */
	void RenderSetpointValuePartial();
	/* Gemeinsame Zeichen-Diff-Logik fuer RenderLinePartial() und
	 * RenderSetpointValuePartial(): vergleicht text/prevText zeichenweise und
	 * aktualisiert nur die Spalten der tatsaechlich geaenderten Zeichen per
	 * Partial-Refresh. */
	void RenderTextPartial(char *text, char *prevText, size_t textCapacity, uint16_t y, uint8_t scale);

	/* Bei kTextScale=4 passen max. ~8 Zeichen pro Zeile (200px Panelbreite)
	 * - deshalb jetzt 3 kurze Zeilen statt 2 laengere. */
	char mLine1Text[16] = "";
	char mLine2Text[16] = "";
	char mLine3Text[16] = "";
	/* Letzter tatsaechlich aufs Panel geschriebener Text je Zeile - RenderLinePartial()
	 * vergleicht Zeichen fuer Zeichen damit und aktualisiert nur die Spalten der
	 * geaenderten Zeichen (z.B. nur die "4" bei "H:54%" -> "H:55%"), statt der
	 * ganzen Zeile. Leerer String = "noch nie gerendert" -> erzwingt vollen
	 * Zeilen-Refresh beim naechsten Aufruf. */
	char mLine1TextPrev[16] = "";
	char mLine2TextPrev[16] = "";
	char mLine3TextPrev[16] = "";
	uint8_t *mFramebuffer = nullptr;
	size_t mFramebufferSize = 0;
	uint16_t mPanelWidth = 0;
	uint16_t mPanelHeight = 0;

	k_work_delayable mSetpointTimeout;
	bool mShowingSetpoint = false;

	k_work_delayable mModeTimeout;
	bool mShowingMode = false;
	bool mLastModeIsHeating = true;

	int16_t mLastLocalTemp = 0;
	/* Letzter Temperaturwert, der tatsaechlich einen Refresh ausgeloest hat
	 * - getrennt von mLastLocalTemp, damit UpdateLocalTemperature() einen
	 * Refresh unterdruecken kann, wenn sich seit dem letzten angezeigten
	 * Wert weniger als 0.5 C geaendert hat. INT16_MIN als Sentinel sorgt
	 * dafuer, dass der allererste Wert immer angezeigt wird. */
	int16_t mLastDisplayedTemp = INT16_MIN;
	uint16_t mLastHumidity = 0;
	int16_t mLastSetpoint = 0;
	uint8_t mLastBatteryPercent = 0;
	uint8_t mLastHeatingDemand = 0;
	bool mSystemModeIsHeating = true;
	bool mTrvOffline = false;
	bool mTrvLinked = false;

	/* Sicherheitsnetz gegen Ghosting/Image-Retention bei wiederholtem
	 * Partial-Refresh (Empfehlung aus Recherche: alle ~5 Partial-Updates
	 * einen echten Full-Refresh einschieben). */
	static constexpr uint8_t kMaxPartialRefreshesBeforeFull = 5;
	uint8_t mPartialRefreshCount = 0;

	/* Verhindert Partial-Refresh-Artefakte beim Wechsel zwischen der
	 * normalen 3-Zeilen-Ansicht und der Setpoint/Mode-Vollbild-Ansicht
	 * (unterschiedliche Layouts - ein Partial-Update wuerde Reste der
	 * jeweils anderen Ansicht stehen lassen). Siehe RenderLine1(). */
	bool mLastRenderWasOverlay = false;
};
