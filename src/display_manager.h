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

	/* Persistent (not temporary) System Mode state - suppresses the heating
	 * icon while the mode is Off, even if PIHeatingDemand is still > 0. */
	void SetSystemModeIsHeating(bool isHeating)
	{
		mSystemModeIsHeating = isHeating;
		RenderHeatingIconPartial();
	}

	/* Called by the TRV poll failure/success tracking in sensor.cpp once
	 * consecutive failures cross the threshold (or a poll succeeds again),
	 * so the user sees the crossed-out link icon instead of silently stale
	 * setpoint/heating-demand values. */
	void SetTrvOffline(bool offline)
	{
		mTrvOffline = offline;
		RenderLinkIconPartial();
		RenderHeatingIconPartial();
	}

	/* True while the TRV subscription is established, shown as the link icon
	 * (plus the "binding confirmed" dots) instead of the crossed-out one. */
	void SetTrvLinked(bool linked)
	{
		mTrvLinked = linked;
		RenderLinkIconPartial();
	}

private:
	/* Boot-Splash: zeigt kurz nach dem Reset/Boot nur das Sync-Icon zentriert
	 * an (statt sofort die Normalansicht mit lauter Default-/Null-Werten),
	 * bis der Timer ablaeuft - dann einmaliger Wechsel zur normalen Ansicht
	 * per RenderFrame(). Rein zeitgesteuert (nicht an echte Sensor-/TRV-
	 * Bereitschaft gekoppelt), haelt die Aenderung dadurch unabhaengig vom
	 * Rest der Boot-Sequenz. */
	static constexpr int32_t kBootSplashDurationMs = 1500;
	static void BootSplashTimeoutHandler(struct k_work *work);
	k_work_delayable mBootSplashTimeout;
	bool mShowingBootSplash = true;

	static void SetpointTimeoutHandler(struct k_work *work);
	static void ModeTimeoutHandler(struct k_work *work);
	static void SetpointRenderDebounceHandler(struct k_work *work);
	static void ModeRenderDebounceHandler(struct k_work *work);
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
	void RenderTextPartial(char *text, char *prevText, size_t textCapacity, uint16_t x, uint16_t y, uint8_t scale);
	/* Zeichnet den Setpoint-Wert in der abweichenden Setpoint-Overlay-
	 * Darstellung: Ganzzahlteil gross, Nachkommateil (inkl. Punkt) kleiner
	 * direkt danach, "C" oben rechts daneben - "text" ist im Format "%d.%d"
	 * (siehe RenderLine1Text()). Zeichnet kein Clear und keine Pfeile (die
	 * zeichnet der Aufrufer separat, siehe RenderFrame()). */
	void DrawSetpointValueSplit(const char *text);

	/* Das Panel sitzt 90 Grad gedreht im Gehaeuse (Querformat-Montage) -
	 * Logo/Text/Icons werden weiterhin ganz normal "logisch" (Hochformat-
	 * Koordinaten wie bisher) in mFramebuffer gezeichnet; erst beim
	 * tatsaechlichen Schreiben ans Panel wird das benoetigte Rechteck um 90
	 * Grad gedreht in mRotatedScratch aufgebaut und physisch geschrieben.
	 * logX0/Y0/X1/Y1 sind inklusive, in logischen (ungedrehten) Koordinaten -
	 * exakt dieselben Werte, die vorher direkt an display_write() gingen. */
	void WriteRotatedRegion(uint16_t logX0, uint16_t logY0, uint16_t logX1, uint16_t logY1);
	/* true = weiss, false = schwarz - liest ein einzelnes Pixel aus dem
	 * logischen mFramebuffer (gleiche bit=0-schwarz/MSB-first-Konvention wie
	 * font_render.c). */
	bool LogicalPixelIsWhite(uint16_t lx, uint16_t ly) const;
	/* Zeichnet/entfernt die Icons in der rechten Icon-Spalte per Partial-
	 * Refresh, sobald sich der jeweilige Anzeige-Zustand (heizt gerade / TRV
	 * verbunden/getrennt) tatsaechlich aendert - sonst No-Op. */
	void RenderHeatingIconPartial();
	void RenderLinkIconPartial();
	/* "Bindung bestaetigt"-Punkte unter dem Link-Icon - wird von
	 * RenderLinkIconPartial() mit aufgerufen (gleicher Sichtbarkeits-
	 * Zustand wie das normale Link-Icon). */
	void RenderDotsIconPartial();
	/* Gemeinsame Zeichen-/Bitmap-Logik fuer beide Icons oben: zeichnet bzw.
	 * loescht ein festes Rechteck per Partial-Refresh. */
	void RenderIconPartial(const uint8_t *bitmap, uint16_t iconWidth, uint16_t iconHeight, uint16_t x, uint16_t y,
				bool show, bool *shownState);

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
	/* Scratch-Puffer fuer WriteRotatedRegion() - gleiche Groesse wie
	 * mFramebuffer (volle Panelflaeche), da ein Full-Refresh das ganze
	 * gedrehte Bild braucht; Partial-Refreshes nutzen nur den vorderen Teil
	 * davon. Einmalig in Init() alloziert statt pro Aufruf, um Heap-
	 * Fragmentierung/Laufzeitkosten auf dem App-Thread zu vermeiden. */
	uint8_t *mRotatedScratch = nullptr;
	uint16_t mPanelWidth = 0;
	uint16_t mPanelHeight = 0;

	k_work_delayable mSetpointTimeout;
	bool mShowingSetpoint = false;

	/* Entprellung gegen schnelles Mehrfach-Druecken des Setpoint-Buttons:
	 * jeder Druck aktualisiert mLastSetpoint sofort (guenstig, kein SPI),
	 * aber der eigentliche (teure, blockierende) Display-Refresh wird erst
	 * ausgeloest, wenn fuer kSetpointRenderDebounceMs kein weiterer Druck
	 * kam - sonst stapeln sich bei schnellem Klicken viele blockierende
	 * Busy-Wait-Zyklen auf dem App-Thread und koennen in Extremfaellen den
	 * Watchdog auslösen ("Systemabsturz" bei Vielfach-Klicks). */
	static constexpr int32_t kSetpointRenderDebounceMs = 150;
	k_work_delayable mSetpointRenderDebounce;

	k_work_delayable mModeTimeout;
	bool mShowingMode = false;

	/* Gleiches Entprell-Prinzip wie mSetpointRenderDebounce (siehe dort) -
	 * aus Konsistenz-/Sicherheitsgruenden, auch wenn der Mode-Button
	 * (Doppelklick) seltener gespammt wird als +/-. */
	k_work_delayable mModeRenderDebounce;
	bool mLastModeIsHeating = true;

	int16_t mLastLocalTemp = 0;
	/* Letzter Temperaturwert, der tatsaechlich einen Refresh ausgeloest hat
	 * - getrennt von mLastLocalTemp, damit UpdateLocalTemperature() einen
	 * Refresh unterdruecken kann, wenn sich seit dem letzten angezeigten
	 * Wert weniger als 0.5 C geaendert hat. INT16_MIN als Sentinel sorgt
	 * dafuer, dass der allererste Wert immer angezeigt wird. */
	int16_t mLastDisplayedTemp = INT16_MIN;
	uint16_t mLastHumidity = 0;
	/* Analog zu mLastDisplayedTemp: letzter Feuchtigkeitswert, der einen
	 * Refresh ausgeloest hat. UINT16_MAX als Sentinel sorgt dafuer, dass der
	 * allererste Wert immer angezeigt wird (Differenz ist dann riesig). */
	uint16_t mLastDisplayedHumidity = UINT16_MAX;
	int16_t mLastSetpoint = 0;
	uint8_t mLastBatteryPercent = 0;
	uint8_t mLastHeatingDemand = 0;
	bool mSystemModeIsHeating = true;
	bool mTrvOffline = false;
	bool mTrvLinked = false;

	/* Zuletzt tatsaechlich aufs Panel gezeichneter Icon-Zustand - verhindert
	 * unnoetige Partial-Refreshes, wenn sich der gewuenschte Zustand nicht
	 * geaendert hat (analog zu mLineXTextPrev). Heizungs-Icon hat drei
	 * moegliche Zustaende statt nur an/aus: versteckt (TRV offline),
	 * Umriss (gebunden, Leerlauf), aktiv (gebunden, heizt gerade). */
	enum class HeatingIconState : uint8_t { kHidden, kIdle, kActive };
	HeatingIconState mHeatingIconDrawnState = HeatingIconState::kHidden;
	/* true = mLastHeatingDemand>0 && Systemmodus heizt - unabhaengig von
	 * mTrvOffline, da RenderFrame()/RenderHeatingIconPartial() das Icon bei
	 * Offline ganz ausblenden, aber den "waere aktiv"-Zustand trotzdem
	 * nachhalten (fuer den Moment, in dem der TRV wieder online geht). */
	bool mHeatingIconActive = false;
	/* true = Punkte ("Bindung bestaetigt") sind aktuell gezeichnet, false =
	 * X ("keine Bindung") - das WLAN-Icon selbst ist immer sichtbar und
	 * braucht daher keinen eigenen Sichtbarkeits-Zustand. */
	bool mDotsBoundShown = false;

	/* Sicherheitsnetz gegen Ghosting/Image-Retention bei wiederholtem
	 * Partial-Refresh - Microtechs eigene Empfehlung (Monica, 2026-10-19):
	 * "perform 10 partial refreshes followed by one full refresh" (Alternative:
	 * 30:1). Hier auf 15 gesetzt, zwischen den beiden genannten Werten. */
	static constexpr uint8_t kMaxPartialRefreshesBeforeFull = 15;
	uint8_t mPartialRefreshCount = 0;

	/* Verhindert Partial-Refresh-Artefakte beim Wechsel zwischen der
	 * normalen 3-Zeilen-Ansicht und der Setpoint/Mode-Vollbild-Ansicht
	 * (unterschiedliche Layouts - ein Partial-Update wuerde Reste der
	 * jeweils anderen Ansicht stehen lassen). Siehe RenderLine1(). */
	bool mLastRenderWasOverlay = false;
};
