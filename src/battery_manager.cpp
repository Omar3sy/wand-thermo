/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "battery_manager.h"
#include "display_manager.h"

#include "app/task_executor.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <platform/PlatformManager.h>

#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace chip;
using namespace chip::app;
using namespace chip::DeviceLayer;

namespace
{

/* Battery voltage range used to derive BatPercentRemaining.
 * Placeholder values for a nominal ~3V primary cell - tune to actual chemistry. */
constexpr int32_t kBatteryVoltageEmptyMv = 2200; /* 0% */
constexpr int32_t kBatteryVoltageFullMv = 3000;  /* 100% */

constexpr uint8_t kPowerSourceEndpoint = 0;

static const struct adc_dt_spec sBatteryAdc = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

} /* namespace */

CHIP_ERROR BatteryManager::Init()
{
	if (!adc_is_ready_dt(&sBatteryAdc)) {
		LOG_ERR("Battery ADC not ready");
	} else if (adc_channel_setup_dt(&sBatteryAdc)) {
		LOG_ERR("Battery ADC channel setup failed");
	}

	k_work_init_delayable(&mBatteryWork, BatteryManager::BatteryWorkHandler);
	k_work_schedule(&mBatteryWork, K_NO_WAIT);

	return CHIP_NO_ERROR;
}

void BatteryManager::BatteryWorkHandler(struct k_work *work)
{
	int32_t voltage = kBatteryVoltageFullMv; /* fallback if ADC read fails */

	if (adc_is_ready_dt(&sBatteryAdc)) {
		int16_t raw = 0;
		struct adc_sequence sequence = {
			.buffer = &raw,
			.buffer_size = sizeof(raw),
		};
		(void)adc_sequence_init_dt(&sBatteryAdc, &sequence);

		int ret = adc_read_dt(&sBatteryAdc, &sequence);
		if (ret == 0) {
			int32_t mv = raw;
			ret = adc_raw_to_millivolts_dt(&sBatteryAdc, &mv);
			if (ret == 0) {
				voltage = mv;
			} else {
				LOG_ERR("Battery: mV conversion failed: %d", ret);
			}
		} else {
			LOG_ERR("Battery: ADC read failed: %d", ret);
		}
	} else {
		LOG_ERR("Battery: ADC channel not ready");
	}

	int32_t clamped = voltage;
	if (clamped < kBatteryVoltageEmptyMv) {
		clamped = kBatteryVoltageEmptyMv;
	}
	if (clamped > kBatteryVoltageFullMv) {
		clamped = kBatteryVoltageFullMv;
	}

	/* Matter BatPercentRemaining is in half-percent units (0-200 = 0-100%). */
	uint8_t percent = static_cast<uint8_t>(
		(static_cast<uint32_t>(clamped - kBatteryVoltageEmptyMv) * 200) /
		static_cast<uint32_t>(kBatteryVoltageFullMv - kBatteryVoltageEmptyMv));

	LOG_INF("Battery: %d mV (%d.%d %%)", voltage, percent / 2, (percent % 2) * 5);

	PlatformMgr().LockChipStack();

	using namespace Clusters::PowerSource;
	Attributes::BatVoltage::Set(kPowerSourceEndpoint, static_cast<uint32_t>(voltage));
	Attributes::BatPercentRemaining::Set(kPowerSourceEndpoint, percent);
	Attributes::Status::Set(kPowerSourceEndpoint, PowerSourceStatusEnum::kActive);
	Attributes::Order::Set(kPowerSourceEndpoint, 1);
	Attributes::Description::Set(kPowerSourceEndpoint, chip::CharSpan::fromCharString("Battery"));
	Attributes::BatChargeLevel::Set(kPowerSourceEndpoint,
					percent < 20 ? BatChargeLevelEnum::kCritical : BatChargeLevelEnum::kOk);
	Attributes::BatPresent::Set(kPowerSourceEndpoint, true);
	Attributes::BatReplacementNeeded::Set(kPowerSourceEndpoint, percent < 10);
	Attributes::BatReplaceability::Set(kPowerSourceEndpoint, BatReplaceabilityEnum::kUserReplaceable);

	PlatformMgr().UnlockChipStack();

	Nrf::PostTask([percent] { DisplayManager::Instance().UpdateBattery(percent); });

	k_work_schedule(&BatteryManager::Instance().mBatteryWork, K_HOURS(1));
}
