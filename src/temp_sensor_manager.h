/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <lib/core/CHIPError.h>
#include <zephyr/kernel.h>

using namespace chip;

/* 5min Timer-Periode (Stromsparen) - uint32_t statt uint16_t, da 300000ms
 * den uint16_t-Wertebereich (max 65535) sprengen wuerde. */
constexpr uint32_t kSensorTimerPeriodMs = 300000;

class TempSensorManager {
public:
	CHIP_ERROR Init();

	static TempSensorManager &Instance()
	{
		static TempSensorManager sTempSensorManager;
		return sTempSensorManager;
	};

	static void SetLocalTemperature(int16_t temperature);
	static void ClearLocalTemperature();
	static void SetOutdoorTemperature(int16_t temperature);
	static void ClearOutdoorTemperature();

private:
	static void TimerEventHandler(k_timer *timer);
	static void SensorTimerEventHandler();
};
