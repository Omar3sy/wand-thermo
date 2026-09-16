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
		RenderLine2();
	}

private:
	static void SetpointTimeoutHandler(struct k_work *work);
	static void ModeTimeoutHandler(struct k_work *work);
	void RenderLine1();
	void RenderLine2();

	k_work_delayable mSetpointTimeout;
	bool mShowingSetpoint = false;

	k_work_delayable mModeTimeout;
	bool mShowingMode = false;
	bool mLastModeIsHeating = true;

	int16_t mLastLocalTemp = 0;
	uint16_t mLastHumidity = 0;
	int16_t mLastSetpoint = 0;
	uint8_t mLastBatteryPercent = 0;
	uint8_t mLastHeatingDemand = 0;
	bool mSystemModeIsHeating = true;
	bool mTrvOffline = false;
};
