/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "display_manager.h"

#include <cstdio>
#include <cstdlib>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <zephyr/device.h>
#include <zephyr/drivers/auxdisplay.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

namespace
{
constexpr chip::EndpointId kThermostatEndpoint = 1;
constexpr k_timeout_t kSetpointDisplayDuration = K_SECONDS(30);

const struct device *sDisplay = DEVICE_DT_GET(DT_NODELABEL(auxdisplay));

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

	k_work_init_delayable(&mSetpointTimeout, DisplayManager::SetpointTimeoutHandler);

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

void DisplayManager::UpdateLocalTemperature(int16_t centiDegrees)
{
	mLastLocalTemp = centiDegrees;
	if (!mShowingSetpoint) {
		RenderLine1();
	}
}

void DisplayManager::UpdateHumidity(uint16_t centiPercent)
{
	mLastHumidity = centiPercent;
	if (!mShowingSetpoint) {
		RenderLine1();
	}
}

void DisplayManager::UpdateBattery(uint8_t percentHalfUnits)
{
	mLastBatteryPercent = percentHalfUnits;
	RenderLine2();
}

void DisplayManager::RenderLine1()
{
	char buf[17];

	if (mShowingSetpoint) {
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
	char buf[17];
	snprintf(buf, sizeof(buf), "Battery: %d%%", mLastBatteryPercent / 2);
	WriteLine(1, buf);
}
