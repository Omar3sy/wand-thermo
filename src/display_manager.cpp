/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "display_manager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app/task_executor.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

#include "font_render.h"
#include "boot_sync_icon.h"
#include "heating_icon.h"
#include "heating_idle_icon.h"
#include "link_icon.h"
#include "setpoint_arrows_icon.h"
#include "misc_icons.h"

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

namespace
{
constexpr chip::EndpointId kThermostatEndpoint = 1;
constexpr k_timeout_t kSetpointDisplayDuration = K_SECONDS(15);

const struct device *sDisplay = DEVICE_DT_GET(DT_NODELABEL(eink_display));

constexpr uint16_t kTextX = 5;

/* Kein Firmenlogo mehr (auf Nutzerwunsch entfernt). Icon-Reihe oben mittig:
 * WLAN (immer sichtbar) + Punkte/X (Bindungsstatus), exakt auf gleicher
 * Hoehe nebeneinander (oben buendig, nicht mehr vertikal zueinander
 * versetzt) + Heizung. Grosser Zahlenwert darunter; im Setpoint-Overlay
 * sitzen die Pfeile NICHT mehr in dieser Reihe, sondern direkt unter dem
 * Wert, in derselben Spalte. Danach Luftfeuchte-Zeile, Batterie unten links. */
/* y=0 - ganz oben, genau parallel zum WLAN-Icon (auch im Leerlauf, nicht
 * nur beim aktiven Heizen). */
constexpr uint16_t kIconRowY = 10;
constexpr uint16_t kIconRowGap = 4;
constexpr uint16_t kIconRowWidth =
	LINK_ICON_WIDTH + kIconRowGap + DOTS_ICON_WIDTH + kIconRowGap + HEATING_ICON_WIDTH;
constexpr uint16_t kIconRowStartX = (200 - kIconRowWidth) / 2;

constexpr uint16_t kLinkIconX = kIconRowStartX;
constexpr uint16_t kLinkIconY = kIconRowY;
/* Punkte/X auf gleicher Hoehe wie WLAN (nicht mehr vertikal zentriert) -
 * sitzt dadurch oben buendig genau neben dem WLAN-Icon. */
constexpr uint16_t kDotsIconX = kIconRowStartX + LINK_ICON_WIDTH + kIconRowGap;
constexpr uint16_t kHeatingIconX = kIconRowStartX + LINK_ICON_WIDTH + kIconRowGap + DOTS_ICON_WIDTH + kIconRowGap;
/* Beide Zustaende (Leerlauf/Aktiv) auf gleicher Hoehe wie WLAN - genau
 * parallel, kein Hoehenunterschied mehr zwischen den beiden Heizungs-Icon-
 * Varianten (frueher stieg das aktive Icon noch eine Zeile hoeher). */
constexpr uint16_t kHeatingIconY = kIconRowY - 10;
constexpr uint16_t kHeatingIconActiveY = kIconRowY - 10;
/* Punkte/X auf gleicher Hoehe wie das aktive Heizungs-Icon, auf
 * Nutzerwunsch. */
constexpr uint16_t kDotsIconY = kHeatingIconActiveY + 12;
/* Reihe ist jetzt immer gleich breit (keine Pfeile mehr darin) - bleibt in
 * beiden Modi an derselben Position, kein Springen beim Rein-/Rausgehen aus
 * dem Setpoint-Overlay. */
constexpr uint16_t kIconRowMaxHeight = 56; /* = LINK_ICON_HEIGHT, groesstes Icon der Reihe */

/* Grosser Zahlenwert ohne "C"-Suffix (das "C" wird separat klein daneben
 * gezeichnet) - unterhalb der Icon-Reihe (endet bei kIconRowY+56=56). */
constexpr uint8_t kBigValueScale = 6;
/* Minimal nach rechts verschoben (war 5, wie kSetpointIntX/kTextX). */
constexpr uint16_t kBigValueX = 10;
constexpr uint16_t kBigValueY = 76;
constexpr uint16_t kUnitLabelScale = 2;
constexpr uint16_t kUnitLabelX = kBigValueX + 4 * 6 * 6 + 4;
constexpr uint16_t kUnitLabelY = kBigValueY + 8;
/* "21.5" bei Scale 6 ist 4 Zeichen * 36px = 144px breit - bandY=74,
 * bandH=7*6+4=46, endet bei y=120. */
constexpr uint16_t kBigValueBottomY = 120;

/* Setpoint-Overlay: eigene Darstellung des Werts statt der normalen
 * einzeiligen Zahl - Ganzzahlteil gross (kBigValueScale, Platz fuer max. 2
 * Ziffern reserviert, Bereich ist 7-28 Grad), Nachkommateil kleiner direkt
 * danach, "C" oben rechts darueber, Pfeile mit etwas Abstand darunter -
 * alles rechts neben dem Wert statt darunter. Eigene X/Y (kSetpointIntX/Y)
 * statt kBigValueX/Y - betrifft nur das Setpoint-Overlay, nicht die normale
 * Temperaturanzeige. Ganz links am Panelrand (kTextX), eine Zeile tiefer als
 * die normale Temperaturanzeige, damit rechts genug Platz fuer "C" + Pfeile
 * bleibt (36px breit) ohne ueber den 200px-Panelrand hinauszuragen.
 * RenderFrame()/RenderSetpointValuePartial() zeichnen das zusammen, siehe
 * dort. */
constexpr uint16_t kSetpointIntDigits = 2;
constexpr uint16_t kSetpointIntX = kTextX - 3; /* etwas naeher am Panelrand als kTextX */
/* Fester Wert statt von kBigValueY abgeleitet - Aenderungen an der normalen
 * Temperatur-Position (kBigValueY) sollen die Setpoint-Overlay-Position
 * nicht automatisch mitverschieben (das hatte sonst via kSetpointArrowsIconY/
 * kHumidityIconYSetpoint Kaskadeneffekte bis in die Normalansicht-Batterie-
 * Zeile). */
constexpr uint16_t kSetpointIntY = 76;
constexpr uint16_t kSetpointDecScale = 4;
constexpr uint16_t kSetpointDecX = kSetpointIntX + kSetpointIntDigits * 6 * kBigValueScale + 4;
constexpr uint16_t kSetpointDecY = kSetpointIntY + 7 * kBigValueScale - 7 * kSetpointDecScale;
constexpr uint16_t kSetpointDecWidth = 2 * 6 * kSetpointDecScale; /* ".5" = 2 Zeichen */
constexpr uint16_t kSetpointValueBottomY = kSetpointIntY + 7 * kBigValueScale + 2;
constexpr uint16_t kSetpointCScale = 2;
/* Pfeile auf Hoehe der Setpoint-Wert-Zeile (kSetpointIntY), rechtsbuendig am
 * Panelrand. "C" sitzt direkt nach der Nachkommastelle, wie ein
 * hochgestelltes Gradzeichen leicht angehoben (kSetpointDecY-6) statt
 * normal auf der Zeile - sieht dadurch aus wie "13.5°C" mit "C" als
 * angehobene Einheit direkt daneben, nicht weit oben abgesetzt. */
constexpr uint16_t kSetpointArrowsIconX = 200 - SETPOINT_ARROWS_ICON_WIDTH - 2;
constexpr uint16_t kSetpointArrowsIconY = kSetpointIntY;
constexpr uint16_t kSetpointArrowsBottomY = kSetpointArrowsIconY + SETPOINT_ARROWS_ICON_HEIGHT;
constexpr uint16_t kSetpointCX = kSetpointDecX + kSetpointDecWidth + 4;
constexpr uint16_t kSetpointCY = kSetpointDecY - 6;

/* Thermometer+"C"-Icon, nur waehrend des Setpoint-Overlays - markiert, dass
 * der grosse Wert gerade der editierbare Sollwert ist. Sitzt direkt neben
 * dem Wert (nach der Nachkommastelle, vor den Pfeilen), vertikal mittig zur
 * Zahlenhoehe statt oben links in der Icon-Reihe. */
constexpr uint16_t kThermIconX = kSetpointDecX + kSetpointDecWidth + 2;
constexpr uint16_t kThermIconY = kSetpointIntY + (7 * kBigValueScale - THERM_C_ICON_HEIGHT) / 2;

/* Luftfeuchte: Tropfen-Icon + Prozentzahl. Normalansicht nutzt jetzt
 * dieselbe Position wie das Setpoint-Overlay (frueher zwei unterschiedliche
 * Y-Werte, auf Nutzerwunsch jetzt identisch) - kHumidityIconYNormal
 * verweist direkt auf kHumidityIconYSetpoint statt einen eigenen Wert zu
 * haben. */
constexpr uint16_t kHumidityIconYSetpoint =
	(kSetpointArrowsBottomY > kSetpointValueBottomY ? kSetpointArrowsBottomY : kSetpointValueBottomY) + 16;
constexpr uint16_t kHumidityIconYNormal = kHumidityIconYSetpoint;
constexpr uint16_t kHumidityStartXNormal = 25;
constexpr uint16_t kHumidityStartXSetpoint = 25;
constexpr uint16_t kHumidityTextScale = 4;

/* Batterie: kleine Zahl unten links. y=184 ist das Maximum, das bei Scale 2
 * noch komplett innerhalb der 200px Panelhoehe bleibt (bandY=182,
 * bandH=7*2+4=18, Ende=200). */
constexpr uint16_t kBatteryTextScale = 2;
constexpr uint16_t kBatteryTextY = 184;

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

	mRotatedScratch = static_cast<uint8_t *>(k_malloc(mFramebufferSize));
	if (mRotatedScratch == nullptr) {
		LOG_ERR("No memory for display rotation scratch buffer (%zu bytes)", mFramebufferSize);
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
	k_work_init_delayable(&mSetpointRenderDebounce, DisplayManager::SetpointRenderDebounceHandler);
	k_work_init_delayable(&mModeRenderDebounce, DisplayManager::ModeRenderDebounceHandler);

	k_work_init_delayable(&mBootSplashTimeout, DisplayManager::BootSplashTimeoutHandler);
	k_work_schedule(&mBootSplashTimeout, K_MSEC(kBootSplashDurationMs));

	/* Erster Render immer als Full-Frame (zeichnet Logo + alle drei Zeilen
	 * zusammen) - Partial-Refresh setzt voraus, dass der aktuelle
	 * Panel-Inhalt bereits mit mFramebuffer uebereinstimmt, was beim
	 * allerersten Aufbau noch nicht der Fall ist. mShowingBootSplash ist zu
	 * diesem Zeitpunkt noch true, RenderFrame() zeichnet daher zunaechst nur
	 * den Boot-Splash statt der (noch lauter Default-/Null-Werte enthaltenden)
	 * Normalansicht. */
	RenderLine1Text();
	RenderLine2Text();
	RenderLine3Text();
	RenderFrame();

	return CHIP_NO_ERROR;
}

void DisplayManager::BootSplashTimeoutHandler(struct k_work *work)
{
	/* k_work laeuft auf der System-Workqueue, nicht auf dem CHIP/App-Thread,
	 * der SPI-Bus und Framebuffer eigentlich besitzt (alle anderen
	 * Display-Updates ueber Threads hinweg gehen deshalb durch
	 * Nrf::PostTask(), siehe sensor.cpp) - ohne das blieb der Splash auf dem
	 * Panel haengen, bis ein Tastendruck (der bereits auf dem App-Thread
	 * laeuft) den naechsten Render erzwang. */
	Nrf::PostTask([] {
		DisplayManager &self = DisplayManager::Instance();
		self.mShowingBootSplash = false;
		self.RenderFrame();
	});
}

void DisplayManager::DrawSetpointValueSplit(const char *text)
{
	const char *dot = strchr(text, '.');
	size_t intLen = dot != nullptr ? static_cast<size_t>(dot - text) : strlen(text);

	char intPart[8];
	size_t copyLen = intLen < sizeof(intPart) - 1 ? intLen : sizeof(intPart) - 1;
	memcpy(intPart, text, copyLen);
	intPart[copyLen] = '\0';

	/* Rechtsbuendig im fuer kSetpointIntDigits reservierten Feld statt
	 * linksbuendig - sonst klafft bei einstelligen Werten (z.B. "9" statt
	 * "22") eine grosse Luecke zwischen der Ziffer und dem Komma, weil
	 * kSetpointDecX/kSetpointCX/die Pfeile von der vollen 2-stelligen Breite
	 * ausgehen. */
	uint16_t intX = kSetpointIntX;
	if (copyLen < kSetpointIntDigits) {
		intX += static_cast<uint16_t>((kSetpointIntDigits - copyLen) * 6 * kBigValueScale);
	}

	font_draw_string(mFramebuffer, mPanelWidth, mPanelHeight, intX, kSetpointIntY, intPart, kBigValueScale, 1);
	if (dot != nullptr) {
		font_draw_string(mFramebuffer, mPanelWidth, mPanelHeight, kSetpointDecX, kSetpointDecY, dot,
				 kSetpointDecScale, 1);
	}
	/* Icon wird hier mitgezeichnet (statt nur einmal in RenderFrame), weil
	 * der Partial-Update-Pfad (RenderSetpointValuePartial()) das gesamte
	 * Wert-Rechteck inkl. dieses Bereichs bei jeder Aenderung loescht - ohne
	 * das Icon hier erneut zu zeichnen wuerde es nach dem ersten
	 * Tastendruck verschwinden. */
	font_draw_bitmap(mFramebuffer, mPanelWidth, mPanelHeight, kThermIconX, kThermIconY, therm_c_icon_bitmap,
			 THERM_C_ICON_WIDTH, THERM_C_ICON_HEIGHT);
}

void DisplayManager::RenderFrame()
{
	if (!device_is_ready(sDisplay) || mFramebuffer == nullptr) {
		return;
	}

	memset(mFramebuffer, 0xFF, mFramebufferSize);

	if (mShowingBootSplash) {
		/* Waehrend des Boot-Splashs nur das Sync-Icon zentriert zeigen,
		 * sonst nichts - vermeidet, kurz nach dem Reset die Normalansicht
		 * mit lauter Default-/Null-Werten (0.0C, 0% Feuchte, X statt
		 * Punkte) aufblitzen zu lassen, bevor echte Daten da sind. */
		uint16_t iconX = (mPanelWidth - BOOT_SYNC_ICON_WIDTH) / 2;
		uint16_t iconY = (mPanelHeight - BOOT_SYNC_ICON_HEIGHT) / 2;
		font_draw_bitmap(mFramebuffer, mPanelWidth, mPanelHeight, iconX, iconY, boot_sync_icon_bitmap,
				 BOOT_SYNC_ICON_WIDTH, BOOT_SYNC_ICON_HEIGHT);
		WriteRotatedRegion(0, 0, mPanelWidth - 1, mPanelHeight - 1);
		return;
	}

	/* Grosser Wert: Temperatur normal, "HEATING"/"OFF" waehrend des Mode-
	 * Overlays - mLine1Text enthaelt bereits den richtigen Inhalt, siehe
	 * RenderLine1Text(). Kleines "C" daneben, ausser bei "HEATING"/"OFF".
	 * Der Setpoint-Fall hat eine eigene, abweichende Darstellung (Ganzzahl
	 * gross + Nachkommateil klein + "C"+Pfeile daneben statt darunter), siehe
	 * DrawSetpointValueSplit(). */
	if (mShowingSetpoint) {
		DrawSetpointValueSplit(mLine1Text);
	} else {
		font_draw_string(mFramebuffer, mPanelWidth, mPanelHeight, kBigValueX, kBigValueY, mLine1Text,
				 kBigValueScale, 1);
		if (!mShowingMode) {
			font_draw_bitmap(mFramebuffer, mPanelWidth, mPanelHeight, kUnitLabelX,
					 kUnitLabelY - DEGREE_ICON_HEIGHT + 2, degree_icon_bitmap, DEGREE_ICON_WIDTH,
					 DEGREE_ICON_HEIGHT);
			font_draw_string(mFramebuffer, mPanelWidth, mPanelHeight, kUnitLabelX + DEGREE_ICON_WIDTH,
					 kUnitLabelY, "C", kUnitLabelScale, 1);
		}
	}

	/* Luftfeuchte bleibt in allen drei Zustaenden sichtbar (normal/Setpoint-
	 * Overlay/Mode-Overlay) - nur der grosse Wert und die Icon-Spalte aendern
	 * sich, siehe Mockup ("Nach Tastendruck" zeigt Luftfeuchte unveraendert
	 * weiter an). Y-Position haengt vom Modus ab - das 72px hohe WLAN-Icon
	 * verdraengt sonst die Batteriezeile aus dem Panel, siehe
	 * kHumidityIconYNormal/kHumidityIconYSetpoint oben. Batterie wird deshalb
	 * waehrend des Setpoint-Overlays ausgeblendet (weniger wichtig als der
	 * gerade editierte Wert, steht in der Normalansicht ohnehin dauerhaft da). */
	uint16_t humidityIconY = mShowingSetpoint ? kHumidityIconYSetpoint : kHumidityIconYNormal;
	uint16_t humidityStartX = mShowingSetpoint ? kHumidityStartXSetpoint : kHumidityStartXNormal;
	font_draw_bitmap(mFramebuffer, mPanelWidth, mPanelHeight, humidityStartX, humidityIconY, droplet_icon_bitmap,
			 DROPLET_ICON_WIDTH, DROPLET_ICON_HEIGHT);
	font_draw_string(mFramebuffer, mPanelWidth, mPanelHeight, humidityStartX + DROPLET_ICON_WIDTH + 6,
			 humidityIconY + 1, mLine2Text, kHumidityTextScale, 1);
	if (!mShowingSetpoint) {
		font_draw_string(mFramebuffer, mPanelWidth, mPanelHeight, kTextX, kBatteryTextY, mLine3Text,
				 kBatteryTextScale, 1);
	}

	/* Icon-Spalte rechts: WLAN-Icon ist dauerhaft sichtbar (Funkmodul ist
	 * immer da, unabhaengig vom TRV-Bindungsstatus). Die Punkte darunter
	 * zeigen stattdessen den Bindungsstatus: Punkte = TRV gebunden & online,
	 * X = keine Bindung (bzw. TRV offline). Danach Heizung, im Setpoint-
	 * Overlay zusaetzlich die Pfeile. */
	font_draw_bitmap(mFramebuffer, mPanelWidth, mPanelHeight, kLinkIconX, kLinkIconY, link_icon_bitmap,
			 LINK_ICON_WIDTH, LINK_ICON_HEIGHT);

	mDotsBoundShown = !mTrvOffline && mTrvLinked;
	if (mDotsBoundShown) {
		font_draw_bitmap(mFramebuffer, mPanelWidth, mPanelHeight, kDotsIconX, kDotsIconY, dots_icon_bitmap,
				 DOTS_ICON_WIDTH, DOTS_ICON_HEIGHT);
	} else {
		font_draw_bitmap(mFramebuffer, mPanelWidth, mPanelHeight, kDotsIconX, kDotsIconY, dots_x_icon_bitmap,
				 DOTS_X_ICON_WIDTH, DOTS_X_ICON_HEIGHT);
	}

	/* Heizungs-Icon ist dauerhaft sichtbar, sobald der TRV gebunden ist
	 * (nicht mehr nur waehrend aktivem Heizbedarf) - zeigt dabei eine von
	 * zwei Varianten: Umriss ohne Dampf-Linien (heating_icon_bitmap) im
	 * Leerlauf (nur erfolgreich gebunden), Icon MIT Dampf-Linien
	 * (heating_idle_icon_bitmap) waehrend aktiv geheizt wird - die
	 * Bitmap-Namen sind historisch vertauscht, die Zuordnung hier ist
	 * massgeblich. Ganz ausgeblendet nur, wenn der TRV offline/nicht
	 * gebunden ist (dann gibt es keine verlaesslichen Heizdaten). */
	mHeatingIconActive = mSystemModeIsHeating && mLastHeatingDemand > 0;
	mHeatingIconDrawnState = mTrvOffline	       ? HeatingIconState::kHidden
				  : mHeatingIconActive ? HeatingIconState::kActive
						       : HeatingIconState::kIdle;
	if (mHeatingIconDrawnState == HeatingIconState::kActive) {
		font_draw_bitmap(mFramebuffer, mPanelWidth, mPanelHeight, kHeatingIconX, kHeatingIconActiveY,
				 heating_idle_icon_bitmap, HEATING_IDLE_ICON_WIDTH, HEATING_IDLE_ICON_HEIGHT);
	} else if (mHeatingIconDrawnState == HeatingIconState::kIdle) {
		font_draw_bitmap(mFramebuffer, mPanelWidth, mPanelHeight, kHeatingIconX, kHeatingIconY,
				 heating_icon_bitmap, HEATING_ICON_WIDTH, HEATING_ICON_HEIGHT);
	}

	if (mShowingSetpoint) {
		font_draw_bitmap(mFramebuffer, mPanelWidth, mPanelHeight, kSetpointArrowsIconX, kSetpointArrowsIconY,
				 setpoint_arrows_icon_bitmap, SETPOINT_ARROWS_ICON_WIDTH, SETPOINT_ARROWS_ICON_HEIGHT);
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

	WriteRotatedRegion(0, 0, mPanelWidth - 1, mPanelHeight - 1);
}

bool DisplayManager::LogicalPixelIsWhite(uint16_t lx, uint16_t ly) const
{
	size_t bytesPerRow = (mPanelWidth + 7) / 8;
	uint8_t byte = mFramebuffer[ly * bytesPerRow + lx / 8];
	return (byte >> (7 - (lx % 8))) & 1;
}

void DisplayManager::WriteRotatedRegion(uint16_t logX0, uint16_t logY0, uint16_t logX1, uint16_t logY1)
{
	if (!device_is_ready(sDisplay) || mFramebuffer == nullptr || mRotatedScratch == nullptr) {
		return;
	}

	/* 90 Grad im Uhrzeigersinn: physisch (px,py) <- logisch (lx,ly) ueber
	 * px = N-1-ly, py = lx (N = Panelkantenlaenge, Panel ist quadratisch).
	 * Fuer ein logisches Rechteck ergibt sich das physische Rechteck daraus
	 * direkt (Y-Spanne wird zur X-Spanne und umgekehrt, mit Spiegelung). */
	const uint16_t n = mPanelWidth; /* quadratisches Panel, mPanelWidth == mPanelHeight */
	uint16_t physX0 = n - 1 - logY1;
	uint16_t physX1 = n - 1 - logY0;
	uint16_t physY0 = logX0;
	uint16_t physY1 = logX1;

	/* 8px-Spaltengrenzen - der Treiber verlangt das fuer Partial-Refresh
	 * (siehe eink154_set_partial_window()), genau wie bei den unrotierten
	 * Partial-Updates vorher. */
	uint16_t alignedPhysX0 = physX0 & static_cast<uint16_t>(~0x07);
	uint16_t alignedPhysX1 = physX1 | 0x07;
	if (alignedPhysX1 >= n) {
		alignedPhysX1 = n - 1;
	}

	uint16_t physWidth = alignedPhysX1 - alignedPhysX0 + 1;
	uint16_t physHeight = physY1 - physY0 + 1;
	size_t colBytes = (physWidth + 7) / 8;
	size_t neededSize = colBytes * physHeight;

	if (neededSize > mFramebufferSize) {
		LOG_ERR("Rotated region too large (%zu > %zu)", neededSize, mFramebufferSize);
		return;
	}

	for (uint16_t row = 0; row < physHeight; row++) {
		uint16_t py = physY0 + row;
		for (size_t cb = 0; cb < colBytes; cb++) {
			uint8_t byte = 0;
			for (uint8_t bit = 0; bit < 8; bit++) {
				uint16_t px = alignedPhysX0 + cb * 8 + bit;
				uint8_t val = 1; /* ausserhalb des Panels: weiss */
				if (px < n) {
					uint16_t lx = py;
					uint16_t ly = n - 1 - px;
					val = LogicalPixelIsWhite(lx, ly) ? 1 : 0;
				}
				byte = static_cast<uint8_t>((byte << 1) | val);
			}
			mRotatedScratch[row * colBytes + cb] = byte;
		}
	}

	struct display_buffer_descriptor desc = {
		.buf_size = static_cast<uint32_t>(neededSize),
		.width = physWidth,
		.height = physHeight,
		.pitch = physWidth,
	};
	int ret = display_write(sDisplay, alignedPhysX0, physY0, &desc, mRotatedScratch);
	if (ret != 0) {
		LOG_ERR("E-paper rotated refresh failed: %d", ret);
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

	bool wasAlreadyShowing = mShowingSetpoint;
	mShowingSetpoint = true;

	if (!wasAlreadyShowing) {
		/* Erstmaliges Oeffnen der Ansicht (Full-Refresh, Layoutwechsel) -
		 * passiert nur einmal pro Druckserie, sofort rendern. */
		RenderLine1();
	} else {
		/* Weitere Druecke waehrend die Ansicht schon offen ist: den
		 * eigentlichen, teuren Display-Refresh entprellen - siehe
		 * kSetpointRenderDebounceMs. Verhindert, dass sich bei schnellem
		 * Mehrfach-Klicken blockierende Busy-Wait-Zyklen auf dem App-Thread
		 * stapeln (konnte sonst in Extremfaellen den Watchdog ausloesen). */
		k_work_reschedule(&mSetpointRenderDebounce, K_MSEC(kSetpointRenderDebounceMs));
	}

	k_work_reschedule(&mSetpointTimeout, kSetpointDisplayDuration);
}

void DisplayManager::SetpointRenderDebounceHandler(struct k_work *work)
{
	DisplayManager::Instance().RenderLine1();
}

void DisplayManager::SetpointTimeoutHandler(struct k_work *work)
{
	DisplayManager &self = DisplayManager::Instance();

	k_work_cancel_delayable(&self.mSetpointRenderDebounce);
	self.mShowingSetpoint = false;
	self.RenderLine1();
}

void DisplayManager::ShowModeTemporarily(bool isHeating)
{
	mLastModeIsHeating = isHeating;

	bool wasAlreadyShowing = mShowingMode;
	mShowingMode = true;

	if (!wasAlreadyShowing) {
		RenderLine1();
	} else {
		k_work_reschedule(&mModeRenderDebounce, K_MSEC(kSetpointRenderDebounceMs));
	}

	k_work_reschedule(&mModeTimeout, K_SECONDS(3));
}

void DisplayManager::ModeRenderDebounceHandler(struct k_work *work)
{
	DisplayManager::Instance().RenderLine1();
}

void DisplayManager::ModeTimeoutHandler(struct k_work *work)
{
	DisplayManager &self = DisplayManager::Instance();

	k_work_cancel_delayable(&self.mModeRenderDebounce);
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

	/* Refresh nur bei >= 5 Prozentpunkten Aenderung seit dem letzten
	 * angezeigten Wert - gleiche Logik wie bei der Temperatur-Schwelle,
	 * verhindert unnoetige Refreshes bei jeder kleinen Messschwankung. */
	constexpr uint16_t kHumidityRefreshThreshold = 500; /* 5 Prozentpunkte in centiPercent */
	uint16_t diff = (centiPercent > mLastDisplayedHumidity) ? (centiPercent - mLastDisplayedHumidity)
								 : (mLastDisplayedHumidity - centiPercent);

	if (diff >= kHumidityRefreshThreshold) {
		mLastDisplayedHumidity = centiPercent;
		RenderLine2();
	}
}

void DisplayManager::UpdateBattery(uint8_t percentHalfUnits)
{
	mLastBatteryPercent = percentHalfUnits;
	RenderLine3();
}

void DisplayManager::UpdateHeatingDemand(uint8_t percent)
{
	mLastHeatingDemand = percent;
	RenderHeatingIconPartial();
}

void DisplayManager::RenderLine1Text()
{
	/* Kein "C"-Suffix mehr in der Zahl selbst - wird separat klein daneben
	 * gezeichnet, siehe RenderFrame()/kUnitLabelX. */
	if (mShowingMode) {
		snprintf(mLine1Text, sizeof(mLine1Text), "%s", mLastModeIsHeating ? "HEATING" : "OFF");
	} else if (mShowingSetpoint) {
		snprintf(mLine1Text, sizeof(mLine1Text), "%d.%d", mLastSetpoint / 100,
			 abs(mLastSetpoint % 100) / 10);
	} else {
		snprintf(mLine1Text, sizeof(mLine1Text), "%d.%d", mLastLocalTemp / 100,
			 abs(mLastLocalTemp % 100) / 10);
	}
}

void DisplayManager::RenderLine1()
{
	RenderLine1Text();

	/* Setpoint/Mode-Overlay unterscheidet sich vom Normalzustand nur im
	 * grossen Wert selbst plus den dazugehoerigen Icons (Thermometer-Kreis
	 * bzw. Pfeile in der Icon-Spalte) - Luftfeuchte/Batterie/Link/Heizung
	 * bleiben unveraendert sichtbar, siehe RenderFrame(). Nur beim
	 * tatsaechlichen Wechsel zwischen den beiden Zustaenden (rein in bzw.
	 * raus aus dem Overlay) ist ein voller RenderFrame() noetig, weil dabei
	 * Icons dazukommen/verschwinden - ein reiner Text-Partial-Update kann
	 * das nicht. Bleibt der Zustand gleich (z.B. Setpoint-Wert aendert sich
	 * waehrend das Overlay schon angezeigt wird), reicht ein Partial-Update
	 * des Textes. */
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
	snprintf(mLine2Text, sizeof(mLine2Text), "%u%%", mLastHumidity / 100);
}

void DisplayManager::RenderLine2()
{
	RenderLine2Text();
	RenderLinePartial(1);
}

void DisplayManager::RenderLine3Text()
{
	snprintf(mLine3Text, sizeof(mLine3Text), "B:%d%%", mLastBatteryPercent / 2);
}

void DisplayManager::RenderLine3()
{
	RenderLine3Text();

	/* Waehrend des Setpoint-Overlays ist die Batteriezeile ausgeblendet
	 * (siehe RenderFrame()) - ein Partial-Update wuerde sie hier faelschlich
	 * wieder reinzeichnen. */
	if (mShowingSetpoint) {
		return;
	}

	RenderLinePartial(2);
}

void DisplayManager::RenderHeatingIconPartial()
{
	mHeatingIconActive = mSystemModeIsHeating && mLastHeatingDemand > 0;

	HeatingIconState wantState = mTrvOffline      ? HeatingIconState::kHidden
				     : mHeatingIconActive ? HeatingIconState::kActive
							  : HeatingIconState::kIdle;
	if (wantState == mHeatingIconDrawnState) {
		return; /* Zustand unveraendert - kein Refresh noetig */
	}

	if (!device_is_ready(sDisplay) || mFramebuffer == nullptr) {
		return;
	}
	mHeatingIconDrawnState = wantState;

	uint16_t alignedStart = kHeatingIconX & static_cast<uint16_t>(~0x07);
	uint16_t alignedEnd = (kHeatingIconX + HEATING_ICON_WIDTH - 1) | 0x07;
	if (alignedEnd >= mPanelWidth) {
		alignedEnd = mPanelWidth - 1;
	}
	uint16_t width = alignedEnd - alignedStart + 1;

	size_t bytesPerRow = (mPanelWidth + 7) / 8;
	size_t colByteStart = alignedStart / 8;
	size_t colBytes = (width + 7) / 8;

	/* Clear-Bereich muss beide moeglichen Y-Positionen abdecken
	 * (kHeatingIconActiveY=0 bis kHeatingIconY+HEATING_ICON_HEIGHT), nicht
	 * nur die aktuell gezeichnete - sonst bleiben beim Wechsel zwischen
	 * Idle (y=kIconRowY) und Aktiv (y=0) Reste der jeweils anderen Position
	 * stehen. */
	constexpr uint16_t kClearY0 = kHeatingIconActiveY;
	constexpr uint16_t kClearHeight = kHeatingIconY + HEATING_ICON_HEIGHT - kHeatingIconActiveY;
	for (uint16_t row = 0; row < kClearHeight; row++) {
		memset(&mFramebuffer[(kClearY0 + row) * bytesPerRow + colByteStart], 0xFF, colBytes);
	}
	if (wantState == HeatingIconState::kActive) {
		font_draw_bitmap(mFramebuffer, mPanelWidth, mPanelHeight, kHeatingIconX, kHeatingIconActiveY,
				 heating_idle_icon_bitmap, HEATING_IDLE_ICON_WIDTH, HEATING_IDLE_ICON_HEIGHT);
	} else if (wantState == HeatingIconState::kIdle) {
		font_draw_bitmap(mFramebuffer, mPanelWidth, mPanelHeight, kHeatingIconX, kHeatingIconY,
				 heating_icon_bitmap, HEATING_ICON_WIDTH, HEATING_ICON_HEIGHT);
	}

	mPartialRefreshCount++;
	if (mPartialRefreshCount >= kMaxPartialRefreshesBeforeFull) {
		mPartialRefreshCount = 0;
		RenderFrame();
		return;
	}

	WriteRotatedRegion(alignedStart, kClearY0, alignedEnd, kClearY0 + kClearHeight - 1);
}

void DisplayManager::RenderLinkIconPartial()
{
	/* Das WLAN-Icon selbst ist dauerhaft sichtbar und aendert sich nie -
	 * kein Partial-Update noetig. Nur die Punkte darunter zeigen den
	 * Bindungsstatus an. */
	RenderDotsIconPartial();
}

void DisplayManager::RenderDotsIconPartial()
{
	/* Punkte vs. X ist keine "zeig/versteck"-Entscheidung wie bei den
	 * anderen Icons (an dieser Position ist immer genau eine der beiden
	 * Varianten sichtbar) - deshalb eigene kleine Diff-Logik statt
	 * RenderIconPartial(). */
	bool boundNow = !mTrvOffline && mTrvLinked;

	if (!device_is_ready(sDisplay) || mFramebuffer == nullptr) {
		return;
	}
	if (boundNow == mDotsBoundShown) {
		return; /* Zustand unveraendert - kein Refresh noetig */
	}
	mDotsBoundShown = boundNow;

	const uint8_t *bitmap = boundNow ? dots_icon_bitmap : dots_x_icon_bitmap;
	uint16_t iconWidth = boundNow ? DOTS_ICON_WIDTH : DOTS_X_ICON_WIDTH;
	uint16_t iconHeight = boundNow ? DOTS_ICON_HEIGHT : DOTS_X_ICON_HEIGHT;

	uint16_t alignedStart = kDotsIconX & static_cast<uint16_t>(~0x07);
	uint16_t alignedEnd = (kDotsIconX + iconWidth - 1) | 0x07;
	if (alignedEnd >= mPanelWidth) {
		alignedEnd = mPanelWidth - 1;
	}
	uint16_t width = alignedEnd - alignedStart + 1;

	size_t bytesPerRow = (mPanelWidth + 7) / 8;
	size_t colByteStart = alignedStart / 8;
	size_t colBytes = (width + 7) / 8;

	for (uint16_t row = 0; row < iconHeight; row++) {
		memset(&mFramebuffer[(kDotsIconY + row) * bytesPerRow + colByteStart], 0xFF, colBytes);
	}
	font_draw_bitmap(mFramebuffer, mPanelWidth, mPanelHeight, kDotsIconX, kDotsIconY, bitmap, iconWidth,
			 iconHeight);

	mPartialRefreshCount++;
	if (mPartialRefreshCount >= kMaxPartialRefreshesBeforeFull) {
		mPartialRefreshCount = 0;
		RenderFrame();
		return;
	}

	WriteRotatedRegion(alignedStart, kDotsIconY, alignedEnd, kDotsIconY + iconHeight - 1);
}

void DisplayManager::RenderIconPartial(const uint8_t *bitmap, uint16_t iconWidth, uint16_t iconHeight, uint16_t x,
					uint16_t y, bool show, bool *shownState)
{
	if (!device_is_ready(sDisplay) || mFramebuffer == nullptr) {
		return;
	}

	if (show == *shownState) {
		return; /* Zustand unveraendert - kein Refresh noetig */
	}
	*shownState = show;

	/* Horizontal auf 8px-Grenzen ausrichten, wie RenderTextPartial() -
	 * vom Treiber fuer Partial-Refresh verlangt. */
	uint16_t alignedStart = x & static_cast<uint16_t>(~0x07);
	uint16_t alignedEnd = (x + iconWidth - 1) | 0x07;
	if (alignedEnd >= mPanelWidth) {
		alignedEnd = mPanelWidth - 1;
	}
	uint16_t width = alignedEnd - alignedStart + 1;

	size_t bytesPerRow = (mPanelWidth + 7) / 8;
	size_t colByteStart = alignedStart / 8;
	size_t colBytes = (width + 7) / 8;

	for (uint16_t row = 0; row < iconHeight; row++) {
		memset(&mFramebuffer[(y + row) * bytesPerRow + colByteStart], 0xFF, colBytes);
	}
	if (show) {
		font_draw_bitmap(mFramebuffer, mPanelWidth, mPanelHeight, x, y, bitmap, iconWidth, iconHeight);
	}

	mPartialRefreshCount++;
	if (mPartialRefreshCount >= kMaxPartialRefreshesBeforeFull) {
		mPartialRefreshCount = 0;
		RenderFrame();
		return;
	}

	WriteRotatedRegion(alignedStart, y, alignedEnd, y + iconHeight - 1);
}

void DisplayManager::RenderLinePartial(uint8_t lineIndex)
{
	uint16_t lineX;
	uint16_t lineY;
	char *text;
	char *prevText;
	size_t textCapacity;
	uint8_t scale;

	switch (lineIndex) {
	case 0:
		lineX = kBigValueX;
		lineY = kBigValueY;
		text = mLine1Text;
		prevText = mLine1TextPrev;
		textCapacity = sizeof(mLine1Text);
		scale = kBigValueScale;
		break;
	case 1:
		lineX = (mShowingSetpoint ? kHumidityStartXSetpoint : kHumidityStartXNormal) + DROPLET_ICON_WIDTH + 6;
		lineY = (mShowingSetpoint ? kHumidityIconYSetpoint : kHumidityIconYNormal) + 1;
		text = mLine2Text;
		prevText = mLine2TextPrev;
		textCapacity = sizeof(mLine2Text);
		scale = kHumidityTextScale;
		break;
	case 2:
		lineX = kTextX;
		lineY = kBatteryTextY;
		text = mLine3Text;
		prevText = mLine3TextPrev;
		textCapacity = sizeof(mLine3Text);
		scale = kBatteryTextScale;
		break;
	default:
		return;
	}

	RenderTextPartial(text, prevText, textCapacity, lineX, lineY, scale);
}

void DisplayManager::RenderSetpointValuePartial()
{
	/* Waehrend die Setpoint-Vollbild-Ansicht aktiv ist, soll sich NUR der
	 * Zahlenwert aendern duerfen (z.B. 21.5C -> 21.6C per Tastendruck), ohne
	 * jedes Mal das ganze Panel neu zu zeichnen - Icons bleiben dabei
	 * unberuehrt stehen. Anders als RenderTextPartial() (zeichenweiser Diff)
	 * wird hier bei jeder Aenderung einfach das ganze Wert-Rechteck (Ganzzahl
	 * + Nachkommateil + "C") neu gezeichnet - einfacher, und das Rechteck ist
	 * klein genug, dass der Mehraufwand gegenueber einem Zeichen-Diff nicht
	 * ins Gewicht faellt. */
	if (strcmp(mLine1Text, mLine1TextPrev) == 0) {
		return;
	}

	constexpr uint16_t kClearX0 = kSetpointIntX & static_cast<uint16_t>(~0x07);
	constexpr uint16_t kClearX1 = (kSetpointCX + DEGREE_ICON_WIDTH + 6 * kSetpointCScale + 7) | 0x07;
	constexpr uint16_t kClearY0 = kSetpointIntY - 2;
	constexpr uint16_t kClearY1 = kSetpointValueBottomY;

	size_t bytesPerRow = (mPanelWidth + 7) / 8;
	size_t colByteStart = kClearX0 / 8;
	size_t colBytes = (kClearX1 - kClearX0 + 1 + 7) / 8;
	for (uint16_t row = kClearY0; row <= kClearY1; row++) {
		memset(&mFramebuffer[row * bytesPerRow + colByteStart], 0xFF, colBytes);
	}

	DrawSetpointValueSplit(mLine1Text);

	strncpy(mLine1TextPrev, mLine1Text, sizeof(mLine1TextPrev) - 1);
	mLine1TextPrev[sizeof(mLine1TextPrev) - 1] = '\0';

	mPartialRefreshCount++;
	if (mPartialRefreshCount >= kMaxPartialRefreshesBeforeFull) {
		mPartialRefreshCount = 0;
		RenderFrame();
		return;
	}

	WriteRotatedRegion(kClearX0, kClearY0, kClearX1, kClearY1);
}

void DisplayManager::RenderTextPartial(char *text, char *prevText, size_t textCapacity, uint16_t x, uint16_t y,
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

	uint16_t startPx = x + firstDiff * charAdvance;
	uint16_t endPx = x + lastDiff * charAdvance + glyphPixelWidth - 1;

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
		uint16_t charX = x + i * charAdvance;

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

	WriteRotatedRegion(alignedStart, bandY, alignedEnd, bandY + bandH - 1);
}
