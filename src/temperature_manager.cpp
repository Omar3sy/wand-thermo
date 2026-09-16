/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "temperature_manager.h"
#include "display_manager.h"
#include "temperature_measurement/sensor.h"
#include "app/task_executor.h"
#include <app-common/zap-generated/cluster-objects.h>
#include <platform/PlatformManager.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace chip;
using namespace ::chip::DeviceLayer;
using namespace ::chip::app::Clusters;
using namespace chip::app::Clusters::Thermostat::Attributes;
using namespace Protocols::InteractionModel;

constexpr EndpointId kThermostatEndpoint = 1;

namespace
{

/* Convert and return only complete part of value to printable type */
uint8_t ReturnCompleteValue(int16_t Value)
{
	return static_cast<uint8_t>(Value / 100);
}

/* Converts and returns only reminder part of value to printable type.
 * This formula rounds reminder value to one significant figure
 */

uint8_t ReturnRemainderValue(int16_t Value)
{
	return static_cast<uint8_t>((Value % 100 + 5) / 10);
}

/* Debounces OccupiedHeatingSetpoint binding writes: rapid successive setpoint
 * changes (e.g. holding the +/- button) would otherwise start a new
 * ExternalSetpointWrite() while a previous binding write/CASE session is
 * still in flight, overlapping sessions and leaving stale ones behind.
 * Only the last value within the debounce window is written. */
constexpr uint32_t kSetpointWriteDebounceMs = 800;

k_timer sSetpointWriteDebounceTimer;
int16_t sLastPendingSetpointValue = 0;

void SetpointWriteDebounceHandler(k_timer *timer)
{
	Nrf::PostTask([] { TemperatureSensor::Instance().ExternalSetpointWrite(sLastPendingSetpointValue); });
}

/* Same debounce problem/fix as the setpoint above: a fast double-click on the
 * mode button (or a remote toggle right after a local one) could otherwise
 * fire two overlapping ExternalSystemModeWrite() binding writes. */
constexpr uint32_t kSystemModeWriteDebounceMs = 800;

k_timer sSystemModeWriteDebounceTimer;
uint8_t sLastPendingSystemModeValue = 0;

void SystemModeWriteDebounceHandler(k_timer *timer)
{
	Nrf::PostTask([] { TemperatureSensor::Instance().ExternalSystemModeWrite(sLastPendingSystemModeValue); });
}

} /* namespace */

CHIP_ERROR TemperatureManager::Init()
{
	CHIP_ERROR err = CHIP_NO_ERROR;

	PlatformMgr().LockChipStack();

	VerifyOrExit(Status::Success == LocalTemperature::Get(kThermostatEndpoint, mLocalTempCelsius),
		     err = CHIP_IM_GLOBAL_STATUS(Failure));
	VerifyOrExit(Status::Success == OutdoorTemperature::Get(kThermostatEndpoint, mOutdoorTempCelsius),
		     err = CHIP_IM_GLOBAL_STATUS(Failure));
	VerifyOrExit(Status::Success == OccupiedCoolingSetpoint::Get(kThermostatEndpoint, &mCoolingCelsiusSetPoint),
		     err = CHIP_IM_GLOBAL_STATUS(Failure));
	VerifyOrExit(Status::Success == OccupiedHeatingSetpoint::Get(kThermostatEndpoint, &mHeatingCelsiusSetPoint),
		     err = CHIP_IM_GLOBAL_STATUS(Failure));
	VerifyOrExit(Status::Success == SystemMode::Get(kThermostatEndpoint, &mThermMode),
		     err = CHIP_IM_GLOBAL_STATUS(Failure));

exit:
	PlatformMgr().UnlockChipStack();

	k_timer_init(&sSetpointWriteDebounceTimer, SetpointWriteDebounceHandler, nullptr);
	k_timer_init(&sSystemModeWriteDebounceTimer, SystemModeWriteDebounceHandler, nullptr);

	return err;
}

void TemperatureManager::LogThermostatStatus()
{
	LOG_INF("Thermostat:");
	LOG_INF("	Mode - %d", static_cast<uint8_t>(mThermMode));
	if (!(GetLocalTemp().IsNull())) {
		int16_t tempValue = GetLocalTemp().Value();
		LOG_INF("	LocalTemperature - %d,%d'C", ReturnCompleteValue(tempValue),
			ReturnRemainderValue(tempValue));
	} else {
		LOG_INF("	LocalTemperature - No Value");
	}
	if (!(GetOutdoorTemp().IsNull())) {
		int16_t tempValue = GetOutdoorTemp().Value();
		LOG_INF("	OutdoorTemperature - %d,%d'C", ReturnCompleteValue(tempValue),
			ReturnRemainderValue(tempValue));
	} else {
		LOG_INF("	OutdoorTemperature - No Value");
	}
	LOG_INF("	HeatingSetpoint - %d,%d'C", ReturnCompleteValue(mHeatingCelsiusSetPoint),
		ReturnRemainderValue(mHeatingCelsiusSetPoint));
	LOG_INF("	CoolingSetpoint - %d,%d'C \n", ReturnCompleteValue(mCoolingCelsiusSetPoint),
		ReturnRemainderValue(mCoolingCelsiusSetPoint));
}

void TemperatureManager::AttributeChangeHandler(EndpointId endpointId, AttributeId attributeId, uint8_t *value,
						uint16_t size)
{
	Status status;

	switch (attributeId) {
	case LocalTemperature::Id: {
		status = LocalTemperature::Get(kThermostatEndpoint, mLocalTempCelsius);
		VerifyOrReturn(Status::Success == status,
			       LOG_ERR("Failed to get Thermostat LocalTemperature attribute"));
	} break;

	case OutdoorTemperature::Id: {
		status = OutdoorTemperature::Get(kThermostatEndpoint, mOutdoorTempCelsius);
		VerifyOrReturn(Status::Success == status,
			       LOG_ERR("Failed to get Thermostat OutdoorTemperature attribute"));
	} break;

	case OccupiedCoolingSetpoint::Id: {
		mCoolingCelsiusSetPoint = *reinterpret_cast<int16_t *>(value);
		LOG_INF("Cooling TEMP: %d", mCoolingCelsiusSetPoint);
	} break;

	case OccupiedHeatingSetpoint::Id: {
		mHeatingCelsiusSetPoint = *reinterpret_cast<int16_t *>(value);
		LOG_INF("Heating TEMP %d", mHeatingCelsiusSetPoint);
		DisplayManager::Instance().ShowSetpointTemporarily();

		if (!mApplyingRemoteUpdate) {
			sLastPendingSetpointValue = mHeatingCelsiusSetPoint;
			k_timer_start(&sSetpointWriteDebounceTimer, K_MSEC(kSetpointWriteDebounceMs), K_NO_WAIT);
		}
	} break;

	case SystemMode::Id: {
		mThermMode = static_cast<app::Clusters::Thermostat::SystemModeEnum>(*value);
		LOG_INF("System Mode changed to %u", *value);

		if (!mApplyingRemoteUpdate) {
			sLastPendingSystemModeValue = *value;
			k_timer_start(&sSystemModeWriteDebounceTimer, K_MSEC(kSystemModeWriteDebounceMs), K_NO_WAIT);
		}
	} break;

	default: {
		LOG_ERR("Unhandled thermostat attribute %x", attributeId);
		return;
	} break;
	}

	LogThermostatStatus();
}

app::DataModel::Nullable<int16_t> TemperatureManager::GetLocalTemp()
{
	return mLocalTempCelsius;
}

app::DataModel::Nullable<int16_t> TemperatureManager::GetOutdoorTemp()
{
	return mOutdoorTempCelsius;
}
