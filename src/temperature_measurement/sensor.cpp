/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "sensor.h"
#include "scd41.h"
#include "temp_sensor_manager.h"

#include "app/task_executor.h"
#include "display_manager.h"

#include <stdint.h>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <controller/ReadInteraction.h>
#include <controller/WriteInteraction.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

using namespace ::chip;
using namespace ::chip::DeviceLayer;
using namespace ::chip::app;

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

namespace
{

constexpr chip::EndpointId mTemperatureMeasurementEndpointId = 1;
/* Endpoint 2 = dedicated "Humidity Sensor" (0x0307) device type in the ZAP,
 * carrying the RelativeHumidityMeasurement server cluster. */
constexpr chip::EndpointId kHumidityMeasurementEndpointId = 2;

constexpr uint8_t kScd41I2cAddr = 0x62;
/* The scd41 node has no dedicated Zephyr driver, so bind to its I2C bus
 * controller directly and address the device by its 7-bit address. */
static const struct device *sScd41I2cDev = DEVICE_DT_GET(DT_BUS(DT_NODELABEL(scd41)));

K_THREAD_STACK_DEFINE(sScd41ThreadStack, 2048);

} /* namespace */

TemperatureSensor::TemperatureSensor()
{
	mCycleCounter = 0;
	mMockTempIndex = 0;
	mPreviousTemperature = 0;
	mScd41ThreadStack = sScd41ThreadStack;
	BindingHandler::Init();
}

void TemperatureSensor::FullMeasurement()
{
	InternalMeasurement();
	ExternalMeasurement();
}

void TemperatureSensor::InternalMeasurement()
{
	/* Kick off the blocking SCD41 single-shot read in a dedicated thread
	 * so we never block the caller (CHIP task via Nrf::PostTask). */
	k_thread_create(&mScd41Thread, mScd41ThreadStack, K_THREAD_STACK_SIZEOF(sScd41ThreadStack),
			TemperatureSensor::Scd41ThreadEntry, nullptr, nullptr, nullptr,
			K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
	k_thread_name_set(&mScd41Thread, "scd41_read");
}

void TemperatureSensor::Scd41ThreadEntry(void *, void *, void *)
{
	uint16_t co2;
	int32_t temp_millideg_c;
	uint16_t humidity_milli_percent;
	int16_t matterTemp = 0;
	uint16_t matterHumidity = 0;
	bool valid = false;

	if (!device_is_ready(sScd41I2cDev)) {
		LOG_ERR("SCD41 I2C device not ready");
		return;
	}

	scd41_wake_up(sScd41I2cDev, kScd41I2cAddr);

	int ret = scd41_measure_single_shot(sScd41I2cDev, kScd41I2cAddr);
	if (ret == 0) {
		ret = scd41_read_measurement(sScd41I2cDev, kScd41I2cAddr, &co2, &temp_millideg_c,
					      &humidity_milli_percent);
	}

	scd41_power_down(sScd41I2cDev, kScd41I2cAddr);

	if (ret == 0) {
		/* Matter LocalTemperature is in units of 0.01 degC */
		matterTemp = static_cast<int16_t>(temp_millideg_c / 10);
		/* Matter RelativeHumidityMeasurement MeasuredValue is in units of 0.01 % */
		matterHumidity = static_cast<uint16_t>(humidity_milli_percent / 10);
		valid = true;
		LOG_INF("SCD41: CO2=%u ppm, Temp=%d.%02u degC, RH=%u.%02u%%", co2,
			(int)(temp_millideg_c / 1000), (unsigned)(abs(temp_millideg_c) % 1000) / 10,
			humidity_milli_percent / 1000, (humidity_milli_percent % 1000) / 10);
	} else {
		LOG_ERR("SCD41 measurement failed: %d", ret);
	}

	if (valid) {
		TempSensorManager::Instance().SetLocalTemperature(matterTemp);

		PlatformMgr().LockChipStack();
		Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::Set(kHumidityMeasurementEndpointId,
										    matterHumidity);
		PlatformMgr().UnlockChipStack();

		Nrf::PostTask([matterTemp] { DisplayManager::Instance().UpdateLocalTemperature(matterTemp); });
		Nrf::PostTask([matterHumidity] { DisplayManager::Instance().UpdateHumidity(matterHumidity); });
	} else {
		TempSensorManager::Instance().ClearLocalTemperature();

		PlatformMgr().LockChipStack();
		Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::SetNull(kHumidityMeasurementEndpointId);
		PlatformMgr().UnlockChipStack();
	}
}

void TemperatureSensor::ExternalMeasurement()
{
	Nrf::Matter::BindingHandler::BindingData *data = Platform::New<Nrf::Matter::BindingHandler::BindingData>();
	if (data) {
		data->ClusterId = Clusters::TemperatureMeasurement::Id;
		data->EndpointId = mTemperatureMeasurementEndpointId;
		data->InvokeCommandFunc = ExternalTemperatureMeasurementReadHandler;
		BindingHandler::RunBoundClusterAction(data);
	}
}

void TemperatureSensor::ExternalTemperatureMeasurementReadHandler(
	const chip::app::Clusters::Binding::TableEntry &binding, OperationalDeviceProxy *deviceProxy,
	Nrf::Matter::BindingHandler::BindingData &bindingData)
{
	auto onSuccess = [dataPointer = Platform::New<Nrf::Matter::BindingHandler::BindingData>(bindingData)](
				 const ConcreteDataAttributePath &attributePath, const auto &dataResponse) {
		ChipLogProgress(NotSpecified, "Read Temperature Sensor attribute succeeded");

		VerifyOrReturn(!(dataResponse.IsNull()), LOG_ERR("Device invalid");
			       Platform::Delete<Nrf::Matter::BindingHandler::BindingData>(dataPointer););

		int16_t responseValue = dataResponse.Value();

		TempSensorManager::Instance().SetOutdoorTemperature(responseValue);
		BindingHandler::OnInvokeCommandSucces(dataPointer);
	};

	auto onFailure = [](const ConcreteDataAttributePath *attributePath, CHIP_ERROR error) {
		ChipLogError(NotSpecified, "Read Temperature Sensor attribute failed: %" CHIP_ERROR_FORMAT,
			     error.Format());
		TempSensorManager::Instance().ClearOutdoorTemperature();
	};

	VerifyOrReturn(deviceProxy != nullptr && deviceProxy->ConnectionReady(), LOG_ERR("Device invalid"));

	Controller::ReadAttribute<Clusters::TemperatureMeasurement::Attributes::MeasuredValue::TypeInfo>(
		deviceProxy->GetExchangeManager(), deviceProxy->GetSecureSession().Value(), binding.remote, onSuccess,
		onFailure);
}

int16_t TemperatureSensor::sPendingSetpointValue = 0;

void TemperatureSensor::ExternalSetpointWrite(int16_t heatingSetpoint)
{
	sPendingSetpointValue = heatingSetpoint;

	Nrf::Matter::BindingHandler::BindingData *data = Platform::New<Nrf::Matter::BindingHandler::BindingData>();
	if (data) {
		data->ClusterId = Clusters::Thermostat::Id;
		data->EndpointId = mTemperatureMeasurementEndpointId;
		data->InvokeCommandFunc = ExternalSetpointWriteHandler;
		BindingHandler::RunBoundClusterAction(data);
	}
}

void TemperatureSensor::ExternalSetpointWriteHandler(
	const chip::app::Clusters::Binding::TableEntry &binding, OperationalDeviceProxy *deviceProxy,
	Nrf::Matter::BindingHandler::BindingData &bindingData)
{
	auto onSuccess = [dataPointer = Platform::New<Nrf::Matter::BindingHandler::BindingData>(bindingData)](
				 const ConcreteAttributePath &attributePath) {
		ChipLogProgress(NotSpecified, "Write OccupiedHeatingSetpoint succeeded");
		BindingHandler::OnInvokeCommandSucces(dataPointer);
	};

	auto onFailure = [](const ConcreteAttributePath *attributePath, CHIP_ERROR error) {
		ChipLogError(NotSpecified, "Write OccupiedHeatingSetpoint failed: %" CHIP_ERROR_FORMAT, error.Format());
	};

	VerifyOrReturn(deviceProxy != nullptr && deviceProxy->ConnectionReady(), LOG_ERR("Device invalid"));

	Controller::WriteAttribute<Clusters::Thermostat::Attributes::OccupiedHeatingSetpoint::TypeInfo>(
		deviceProxy->GetSecureSession().Value(), binding.remote, sPendingSetpointValue, onSuccess, onFailure);
}