/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#pragma once

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

	/* Called from button handlers: shows setpoint for 30s, then falls back */
	void ShowSetpointTemporarily();

	/* Called whenever LocalTemperature, Humidity or Battery values update */
	void UpdateLocalTemperature(int16_t centiDegrees);
	void UpdateHumidity(uint16_t centiPercent);
	void UpdateBattery(uint8_t percentHalfUnits);

private:
	static void SetpointTimeoutHandler(struct k_work *work);
	void RenderLine1();
	void RenderLine2();

	k_work_delayable mSetpointTimeout;
	bool mShowingSetpoint = false;

	int16_t mLastLocalTemp = 0;
	uint16_t mLastHumidity = 0;
	int16_t mLastSetpoint = 0;
	uint8_t mLastBatteryPercent = 0;
};
