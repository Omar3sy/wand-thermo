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
#include <zephyr/drivers/auxdisplay.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

namespace
{
constexpr chip::EndpointId kThermostatEndpoint = 1;
constexpr k_timeout_t kSetpointDisplayDuration = K_SECONDS(15);

const struct device *sDisplay = DEVICE_DT_GET(DT_NODELABEL(auxdisplay));

/* Custom character slot for the "heating" flame icon. Not slot 0: its character
 * code would be a NUL byte, which would terminate the C strings we build. */
constexpr uint8_t kFlameIconIndex = 1;

/* 5x8 pixels, row by row, 0xff = pixel on. */
uint8_t sFlameIconPixels[40] = {
	0x00, 0x00, 0xff, 0x00, 0x00,
	0x00, 0x00, 0xff, 0xff, 0x00,
	0x00, 0xff, 0xff, 0xff, 0x00,
	0x00, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff,
	0x00, 0xff, 0xff, 0xff, 0x00,
	0x00, 0x00, 0xff, 0x00, 0x00,
};

/* Custom character slot for the "linked" check-mark icon, shown while the TRV
 * subscription is established. */
constexpr uint8_t kLinkIconIndex = 2;

/* 5x8 check mark, row by row, 0xff = pixel on. */
uint8_t sLinkIconPixels[40] = {
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff,
	0x00, 0x00, 0x00, 0xff, 0x00,
	0xff, 0x00, 0xff, 0x00, 0x00,
	0x00, 0xff, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
};

void WriteLine(uint8_t row, const char *text)
{
	if (!device_is_ready(sDisplay)) {
		return;
	}
	auxdisplay_cursor_position_set(sDisplay, AUXDISPLAY_POSITION_ABSOLUTE, 0, row);
	/* Pad to 16 chars so leftover characters from a longer previous string are cleared */
	char buf[17];
	snprintf(buf, sizeof(buf), "%-16s", text);
	auxdisplay_write(sDisplay, reinterpret_cast<const uint8_t *>(buf), 16);
}

} /* namespace */

CHIP_ERROR DisplayManager::Init()
{
	if (!device_is_ready(sDisplay)) {
		LOG_ERR("Auxdisplay device not ready");
		return CHIP_ERROR_INCORRECT_STATE;
	}

	struct auxdisplay_character flameIcon = {
		.index = kFlameIconIndex,
		.data = sFlameIconPixels,
		.character_code = 0,
	};
	if (auxdisplay_custom_character_set(sDisplay, &flameIcon) != 0) {
		LOG_WRN("Failed to upload heating icon");
	}

	struct auxdisplay_character linkIcon = {
		.index = kLinkIconIndex,
		.data = sLinkIconPixels,
		.character_code = 0,
	};
	if (auxdisplay_custom_character_set(sDisplay, &linkIcon) != 0) {
		LOG_WRN("Failed to upload link icon");
	}

	k_work_init_delayable(&mSetpointTimeout, DisplayManager::SetpointTimeoutHandler);
	k_work_init_delayable(&mModeTimeout, DisplayManager::ModeTimeoutHandler);

	RenderLine1();
	RenderLine2();

	return CHIP_NO_ERROR;
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
	if (!mShowingSetpoint && !mShowingMode) {
		RenderLine1();
	}
}

void DisplayManager::UpdateHumidity(uint16_t centiPercent)
{
	mLastHumidity = centiPercent;
	if (!mShowingSetpoint && !mShowingMode) {
		RenderLine1();
	}
}

void DisplayManager::UpdateBattery(uint8_t percentHalfUnits)
{
	mLastBatteryPercent = percentHalfUnits;
	RenderLine2();
}

void DisplayManager::UpdateHeatingDemand(uint8_t percent)
{
	mLastHeatingDemand = percent;
	RenderLine2();
}

void DisplayManager::RenderLine1()
{
	/* Oversized on purpose: %d can in theory print more digits than the
	 * 16-char LCD line holds (e.g. INT16_MIN); WriteLine() truncates to
	 * 16 chars for the display, this just keeps snprintf from warning
	 * about a formally possible overflow. */
	char buf[24];

	if (mShowingMode) {
		snprintf(buf, sizeof(buf), "Mode: %s", mLastModeIsHeating ? "HEATING" : "OFF");
	} else if (mShowingSetpoint) {
		snprintf(buf, sizeof(buf), "Setpoint: %d.%d C", mLastSetpoint / 100,
			 abs(mLastSetpoint % 100) / 10);
	} else {
		/* "T:21.3C  H:49%" - temperature and relative humidity together */
		snprintf(buf, sizeof(buf), "T:%d.%dC  H:%u%%", mLastLocalTemp / 100,
			 abs(mLastLocalTemp % 100) / 10, mLastHumidity / 100);
	}

	WriteLine(0, buf);
}

void DisplayManager::RenderLine2()
{
	char buf[24];

	if (mTrvOffline) {
		snprintf(buf, sizeof(buf), "B:%d%% TRV offline", mLastBatteryPercent / 2);
	} else {
		if (!mSystemModeIsHeating) {
			/* System off: heating-demand percentage is meaningless, show the mode instead */
			snprintf(buf, sizeof(buf), "B:%d%% Mode OFF", mLastBatteryPercent / 2);
		} else if (mLastHeatingDemand > 0) {
			snprintf(buf, sizeof(buf), "B:%d%% %c", mLastBatteryPercent / 2, kFlameIconIndex);
		} else {
			snprintf(buf, sizeof(buf), "B:%d%%", mLastBatteryPercent / 2);
		}
		if (mTrvLinked) {
			const size_t len = strlen(buf);
			snprintf(buf + len, sizeof(buf) - len, " %c", kLinkIconIndex);
		}
	}

	WriteLine(1, buf);
}
