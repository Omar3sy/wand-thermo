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

	RenderLine1();
	RenderLine2();
	RenderLine3();

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

void DisplayManager::RenderLine1()
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

	RenderFrame();
}

void DisplayManager::RenderLine2()
{
	snprintf(mLine2Text, sizeof(mLine2Text), "H:%u%%", mLastHumidity / 100);

	RenderFrame();
}

void DisplayManager::RenderLine3()
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

	RenderFrame();
}
