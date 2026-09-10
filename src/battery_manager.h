/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#pragma once

#include <lib/core/CHIPError.h>
#include <zephyr/kernel.h>

class BatteryManager {
public:
	CHIP_ERROR Init();

	static BatteryManager &Instance()
	{
		static BatteryManager sInstance;
		return sInstance;
	}

private:
	static void BatteryWorkHandler(struct k_work *work);
	k_work_delayable mBatteryWork;
};
