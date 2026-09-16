/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "sensor.h"
#include "scd41.h"
#include "temp_sensor_manager.h"
#include "temperature_manager.h"

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

/* Tolerance for the TRV-setpoint echo check in TrvSetpointReadHandler: the TRV
 * may report its own OccupiedHeatingSetpoint back slightly off from what we
 * sent (e.g. its own 0.5 C step resolution / internal rounding), so an exact
 * match would miss real echoes and falsely re-trigger the drift we're trying
 * to avoid. 25 = 0.25 C, generous enough to cover 0.5 C-step rounding. */
constexpr int16_t kSetpointEchoToleranceCentidegrees = 25;

/* Hard ceiling on how much of a TRV-vs-own-sensor offset we'll compensate
 * for. Guards against one erroneous/extreme reading (or a TRV genuinely
 * mounted somewhere very different from ambient, e.g. directly on the
 * radiator body) dominating the compensated setpoint sent to the TRV. */
constexpr int16_t kMaxTrvOffsetMagnitude = 500; /* +/- 5.0 C */

/* After this many consecutive failed TRV polls (across setpoint/heating-demand/
 * local-temperature reads), the TRV is considered unreachable and the display
 * shows "TRV offline" instead of silently keeping stale values. Reset to 0 on
 * any successful poll. 3 consecutive failures at the ~30s poll cycle gives a
 * short transient hiccup a chance to self-resolve before alarming the user. */
constexpr uint8_t kTrvOfflineFailureThreshold = 3;
uint8_t sTrvConsecutiveFailures = 0;

void RecordTrvPollSuccess()
{
	bool wasOffline = sTrvConsecutiveFailures >= kTrvOfflineFailureThreshold;
	sTrvConsecutiveFailures = 0;
	if (wasOffline) {
		Nrf::PostTask([] { DisplayManager::Instance().SetTrvOffline(false); });
	}
}

void RecordTrvPollFailure()
{
	if (sTrvConsecutiveFailures < UINT8_MAX) {
		sTrvConsecutiveFailures++;
	}
	if (sTrvConsecutiveFailures == kTrvOfflineFailureThreshold) {
		LOG_WRN("TRV unreachable after %u consecutive failed polls", sTrvConsecutiveFailures);
		Nrf::PostTask([] { DisplayManager::Instance().SetTrvOffline(true); });
	}
}

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
	PollTrvSetpoint();
	PollTrvHeatingDemand();
	PollTrvLocalTemperature();
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
	uint16_t co2_raw, temp_raw, rh_raw;
	int16_t matterTemp = 0;
	uint16_t matterHumidity = 0;
	bool valid = false;

	if (!device_is_ready(sScd41I2cDev)) {
		LOG_ERR("SCD41 I2C device not ready");
		return;
	}

	scd41_wake_up(sScd41I2cDev, kScd41I2cAddr);

	/* If the sensor happened to be left in periodic measurement mode (e.g.
	 * from a previous firmware/session, or its power-on default), a
	 * single-shot command still gets acknowledged but the readings it
	 * returns - relative humidity in particular - can be wrong/stuck near 0
	 * indefinitely. Sensirion's datasheet requires stop_periodic_measurement
	 * before switching into single-shot mode; only needs sending once. */
	static bool sPeriodicMeasurementStopped = false;
	if (!sPeriodicMeasurementStopped) {
		scd41_stop_periodic_measurement(sScd41I2cDev, kScd41I2cAddr);
		sPeriodicMeasurementStopped = true;
	}

	int ret = scd41_measure_single_shot(sScd41I2cDev, kScd41I2cAddr);
	if (ret == 0) {
		ret = scd41_read_measurement_raw(sScd41I2cDev, kScd41I2cAddr, &co2, &temp_millideg_c,
						  &humidity_milli_percent, &co2_raw, &temp_raw, &rh_raw);
	}

	scd41_power_down(sScd41I2cDev, kScd41I2cAddr);

	if (ret == 0) {
		LOG_INF("SCD41 raw words: co2_raw=%u temp_raw=%u rh_raw=%u", co2_raw, temp_raw, rh_raw);
	}

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
	/* Allocate once, up front, so every exit path (success, failure, null value,
	 * early return) has a reference to free it - fixes a leak present in the
	 * original Nordic sample code (onFailure never freed dataPointer). */
	auto *dataPointer = Platform::New<Nrf::Matter::BindingHandler::BindingData>(bindingData);
	if (dataPointer == nullptr) {
		LOG_ERR("Failed to allocate BindingData");
		return;
	}

	if (deviceProxy == nullptr || !deviceProxy->ConnectionReady()) {
		LOG_ERR("Device invalid");
		Platform::Delete<Nrf::Matter::BindingHandler::BindingData>(dataPointer);
		return;
	}

	auto onSuccess = [dataPointer](const ConcreteDataAttributePath &attributePath, const auto &dataResponse) {
		ChipLogProgress(NotSpecified, "Read Temperature Sensor attribute succeeded");

		if (dataResponse.IsNull()) {
			LOG_ERR("Device invalid");
			Platform::Delete<Nrf::Matter::BindingHandler::BindingData>(dataPointer);
			return;
		}

		int16_t responseValue = dataResponse.Value();

		TempSensorManager::Instance().SetOutdoorTemperature(responseValue);
		BindingHandler::OnInvokeCommandSucces(dataPointer);
	};

	auto onFailure = [dataPointer](const ConcreteDataAttributePath *attributePath, CHIP_ERROR error) {
		ChipLogError(NotSpecified, "Read Temperature Sensor attribute failed: %" CHIP_ERROR_FORMAT,
			     error.Format());
		TempSensorManager::Instance().ClearOutdoorTemperature();
		Platform::Delete<Nrf::Matter::BindingHandler::BindingData>(dataPointer);
	};

	Controller::ReadAttribute<Clusters::TemperatureMeasurement::Attributes::MeasuredValue::TypeInfo>(
		deviceProxy->GetExchangeManager(), deviceProxy->GetSecureSession().Value(), binding.remote, onSuccess,
		onFailure);
}

int16_t TemperatureSensor::sPendingSetpointValue = 0;
int16_t TemperatureSensor::sLastSentCompensatedSetpoint = INT16_MIN; /* guarantees no accidental match */

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
	auto *dataPointer = Platform::New<Nrf::Matter::BindingHandler::BindingData>(bindingData);
	if (dataPointer == nullptr) {
		LOG_ERR("Failed to allocate BindingData");
		return;
	}

	if (deviceProxy == nullptr || !deviceProxy->ConnectionReady()) {
		LOG_ERR("Device invalid");
		Platform::Delete<Nrf::Matter::BindingHandler::BindingData>(dataPointer);
		return;
	}

	auto onSuccess = [dataPointer](const ConcreteAttributePath &attributePath) {
		ChipLogProgress(NotSpecified, "Write OccupiedHeatingSetpoint succeeded");
		BindingHandler::OnInvokeCommandSucces(dataPointer);
	};

	auto onFailure = [dataPointer](const ConcreteAttributePath *attributePath, CHIP_ERROR error) {
		ChipLogError(NotSpecified, "Write OccupiedHeatingSetpoint failed: %" CHIP_ERROR_FORMAT, error.Format());
		Platform::Delete<Nrf::Matter::BindingHandler::BindingData>(dataPointer);
	};

	if (!sHasValidTrvOffset) {
		/* We haven't received a single PollTrvLocalTemperature() reading yet
		 * (e.g. TRV was only just bound) - offset is still the 0 default, so
		 * this write goes out uncompensated. Kick off a fresh poll now so the
		 * *next* write - rather than only the next up-to-30s periodic cycle -
		 * has a real offset to work with. */
		LOG_WRN("No TRV offset known yet - sending setpoint uncompensated, requesting a fresh reading");
		TemperatureSensor::Instance().PollTrvLocalTemperature();
	}

	/* Compensate for the offset between the TRV's own LocalTemperature and ours
	 * (PollTrvLocalTemperature keeps sTrvLocalTempOffset up to date), so the TRV
	 * actually reaches the temperature the wand thermostat's sensor reports. */
	constexpr int32_t kTrvAbsMinSetpoint = 750;
	constexpr int32_t kTrvAbsMaxSetpoint = 2850;

	int32_t compensated = static_cast<int32_t>(sPendingSetpointValue) + sTrvLocalTempOffset;
	int32_t clamped = compensated;
	if (clamped < kTrvAbsMinSetpoint) {
		clamped = kTrvAbsMinSetpoint;
	}
	if (clamped > kTrvAbsMaxSetpoint) {
		clamped = kTrvAbsMaxSetpoint;
	}
	int16_t compensatedSetpoint = static_cast<int16_t>(clamped);
	sLastSentCompensatedSetpoint = compensatedSetpoint;

	if (clamped != compensated) {
		/* Not silent anymore: the TRV is actually getting a different value
		 * than "local setpoint + offset" would suggest, because it hit the
		 * TRV's hardware limit (750-2850) rather than our local UI limit
		 * (kSetpointMin/kSetpointMax in app_task.cpp, 700-3000). */
		LOG_WRN("Compensated setpoint %d clamped to %d (TRV hardware limit 7.5-28.5 C)", compensated,
			 compensatedSetpoint);
	}

	LOG_INF("Sending compensated setpoint: %d (raw=%d, offset=%d)", compensatedSetpoint, sPendingSetpointValue,
		sTrvLocalTempOffset);

	Controller::WriteAttribute<Clusters::Thermostat::Attributes::OccupiedHeatingSetpoint::TypeInfo>(
		deviceProxy->GetSecureSession().Value(), binding.remote, compensatedSetpoint, onSuccess, onFailure);
}

uint8_t TemperatureSensor::sPendingSystemModeValue = 0;

void TemperatureSensor::ExternalSystemModeWrite(uint8_t mode)
{
	sPendingSystemModeValue = mode;

	Nrf::Matter::BindingHandler::BindingData *data = Platform::New<Nrf::Matter::BindingHandler::BindingData>();
	if (data) {
		data->ClusterId = Clusters::Thermostat::Id;
		data->EndpointId = mTemperatureMeasurementEndpointId;
		data->InvokeCommandFunc = ExternalSystemModeWriteHandler;
		BindingHandler::RunBoundClusterAction(data);
	}
}

void TemperatureSensor::ExternalSystemModeWriteHandler(
	const chip::app::Clusters::Binding::TableEntry &binding, OperationalDeviceProxy *deviceProxy,
	Nrf::Matter::BindingHandler::BindingData &bindingData)
{
	auto *dataPointer = Platform::New<Nrf::Matter::BindingHandler::BindingData>(bindingData);
	if (dataPointer == nullptr) {
		LOG_ERR("Failed to allocate BindingData");
		return;
	}

	if (deviceProxy == nullptr || !deviceProxy->ConnectionReady()) {
		LOG_ERR("Device invalid");
		Platform::Delete<Nrf::Matter::BindingHandler::BindingData>(dataPointer);
		return;
	}

	auto onSuccess = [dataPointer](const ConcreteAttributePath &attributePath) {
		ChipLogProgress(NotSpecified, "Write SystemMode succeeded");
		BindingHandler::OnInvokeCommandSucces(dataPointer);
	};

	auto onFailure = [dataPointer](const ConcreteAttributePath *attributePath, CHIP_ERROR error) {
		ChipLogError(NotSpecified, "Write SystemMode failed: %" CHIP_ERROR_FORMAT, error.Format());
		Platform::Delete<Nrf::Matter::BindingHandler::BindingData>(dataPointer);
	};

	Controller::WriteAttribute<Clusters::Thermostat::Attributes::SystemMode::TypeInfo>(
		deviceProxy->GetSecureSession().Value(), binding.remote,
		static_cast<Clusters::Thermostat::SystemModeEnum>(sPendingSystemModeValue), onSuccess, onFailure);
}

void TemperatureSensor::PollTrvSetpoint()
{
	Nrf::Matter::BindingHandler::BindingData *data = Platform::New<Nrf::Matter::BindingHandler::BindingData>();
	if (data) {
		data->ClusterId = Clusters::Thermostat::Id;
		data->EndpointId = mTemperatureMeasurementEndpointId;
		data->InvokeCommandFunc = TrvSetpointReadHandler;
		BindingHandler::RunBoundClusterAction(data);
	}
}

void TemperatureSensor::TrvSetpointReadHandler(
	const chip::app::Clusters::Binding::TableEntry &binding, OperationalDeviceProxy *deviceProxy,
	Nrf::Matter::BindingHandler::BindingData &bindingData)
{
	auto *dataPointer = Platform::New<Nrf::Matter::BindingHandler::BindingData>(bindingData);
	if (dataPointer == nullptr) {
		LOG_ERR("Failed to allocate BindingData");
		return;
	}

	if (deviceProxy == nullptr || !deviceProxy->ConnectionReady()) {
		LOG_ERR("Device invalid");
		Platform::Delete<Nrf::Matter::BindingHandler::BindingData>(dataPointer);
		return;
	}

	/* Note: unlike TemperatureMeasurement::MeasuredValue, Thermostat::OccupiedHeatingSetpoint
	 * is a plain (non-nullable) int16_t - dataResponse below is the value itself. */
	auto onSuccess = [dataPointer, remoteNodeId = binding.nodeId](const ConcreteDataAttributePath &attributePath,
									int16_t dataResponse) {
		ChipLogProgress(NotSpecified, "Read TRV OccupiedHeatingSetpoint succeeded");

		if (sPrimaryTrvNodeId == 0) {
			sPrimaryTrvNodeId = remoteNodeId;
			LOG_INF("Primary TRV locked to node 0x%llX", (unsigned long long)remoteNodeId);
		}
		if (remoteNodeId != sPrimaryTrvNodeId) {
			/* Not the primary TRV - its polls don't count for the offline
			 * detector either, otherwise a healthy secondary TRV would keep
			 * resetting the failure counter while the actual primary TRV is
			 * unreachable, masking the outage. */
			LOG_INF("Ignoring poll response from non-primary TRV (node 0x%llX)",
				(unsigned long long)remoteNodeId);
			BindingHandler::OnInvokeCommandSucces(dataPointer);
			return;
		}

		/* Only apply if this isn't just our own compensated write echoing back -
		 * otherwise the (uncompensated) TRV value would drift the local setpoint
		 * away from what the user actually set, every poll cycle. Compared with
		 * a tolerance since the TRV may round/re-report the value slightly off
		 * from what we sent. */
		int32_t diff = abs(static_cast<int32_t>(dataResponse) - static_cast<int32_t>(sLastSentCompensatedSetpoint));
		if (diff > kSetpointEchoToleranceCentidegrees) {
			PlatformMgr().LockChipStack();
			TemperatureManager::Instance().SetApplyingRemoteUpdate(true);
			Clusters::Thermostat::Attributes::OccupiedHeatingSetpoint::Set(mTemperatureMeasurementEndpointId,
											dataResponse);
			TemperatureManager::Instance().SetApplyingRemoteUpdate(false);
			PlatformMgr().UnlockChipStack();

			/* Without this, sLastSentCompensatedSetpoint stays frozen at whatever
			 * we last wrote ourselves, so every future poll of this same
			 * (unchanged) TRV value would again exceed the tolerance and re-apply
			 * it as if it were a new external change, forever. */
			sLastSentCompensatedSetpoint = dataResponse;

			LOG_INF("TRV setpoint differs from our last sent value - applying as external change");
		} else {
			LOG_INF("TRV setpoint matches our own last write - ignoring (echo)");
		}

		RecordTrvPollSuccess();
		BindingHandler::OnInvokeCommandSucces(dataPointer);
	};

	auto onFailure = [dataPointer, remoteNodeId = binding.nodeId](const ConcreteDataAttributePath *attributePath,
									CHIP_ERROR error) {
		ChipLogError(NotSpecified, "Read TRV setpoint failed: %" CHIP_ERROR_FORMAT, error.Format());
		if (sPrimaryTrvNodeId == 0 || remoteNodeId == sPrimaryTrvNodeId) {
			RecordTrvPollFailure();
		}
		Platform::Delete<Nrf::Matter::BindingHandler::BindingData>(dataPointer);
	};

	Controller::ReadAttribute<Clusters::Thermostat::Attributes::OccupiedHeatingSetpoint::TypeInfo>(
		deviceProxy->GetExchangeManager(), deviceProxy->GetSecureSession().Value(), binding.remote, onSuccess,
		onFailure);
}

void TemperatureSensor::PollTrvHeatingDemand()
{
	Nrf::Matter::BindingHandler::BindingData *data = Platform::New<Nrf::Matter::BindingHandler::BindingData>();
	if (data) {
		data->ClusterId = Clusters::Thermostat::Id;
		data->EndpointId = mTemperatureMeasurementEndpointId;
		data->InvokeCommandFunc = TrvHeatingDemandReadHandler;
		BindingHandler::RunBoundClusterAction(data);
	}
}

void TemperatureSensor::TrvHeatingDemandReadHandler(
	const chip::app::Clusters::Binding::TableEntry &binding, OperationalDeviceProxy *deviceProxy,
	Nrf::Matter::BindingHandler::BindingData &bindingData)
{
	auto *dataPointer = Platform::New<Nrf::Matter::BindingHandler::BindingData>(bindingData);
	if (dataPointer == nullptr) {
		LOG_ERR("Failed to allocate BindingData");
		return;
	}

	if (deviceProxy == nullptr || !deviceProxy->ConnectionReady()) {
		LOG_ERR("Device invalid");
		Platform::Delete<Nrf::Matter::BindingHandler::BindingData>(dataPointer);
		return;
	}

	/* PIHeatingDemand is also a plain (non-nullable) uint8_t percentage. */
	auto onSuccess = [dataPointer, remoteNodeId = binding.nodeId](const ConcreteDataAttributePath &attributePath,
									uint8_t dataResponse) {
		ChipLogProgress(NotSpecified, "Read TRV PIHeatingDemand succeeded");

		if (sPrimaryTrvNodeId == 0) {
			sPrimaryTrvNodeId = remoteNodeId;
			LOG_INF("Primary TRV locked to node 0x%llX", (unsigned long long)remoteNodeId);
		}
		if (remoteNodeId != sPrimaryTrvNodeId) {
			LOG_INF("Ignoring poll response from non-primary TRV (node 0x%llX)",
				(unsigned long long)remoteNodeId);
			BindingHandler::OnInvokeCommandSucces(dataPointer);
			return;
		}

		LOG_INF("TRV Heating Demand: %u%%", dataResponse);

		Nrf::PostTask([dataResponse] { DisplayManager::Instance().UpdateHeatingDemand(dataResponse); });

		RecordTrvPollSuccess();
		BindingHandler::OnInvokeCommandSucces(dataPointer);
	};

	auto onFailure = [dataPointer, remoteNodeId = binding.nodeId](const ConcreteDataAttributePath *attributePath,
									CHIP_ERROR error) {
		ChipLogError(NotSpecified, "Read TRV heating demand failed: %" CHIP_ERROR_FORMAT, error.Format());
		if (sPrimaryTrvNodeId == 0 || remoteNodeId == sPrimaryTrvNodeId) {
			RecordTrvPollFailure();
		}
		Platform::Delete<Nrf::Matter::BindingHandler::BindingData>(dataPointer);
	};

	Controller::ReadAttribute<Clusters::Thermostat::Attributes::PIHeatingDemand::TypeInfo>(
		deviceProxy->GetExchangeManager(), deviceProxy->GetSecureSession().Value(), binding.remote, onSuccess,
		onFailure);
}

int16_t TemperatureSensor::sTrvLocalTempOffset = 0;
bool TemperatureSensor::sHasValidTrvOffset = false;
chip::NodeId TemperatureSensor::sPrimaryTrvNodeId = 0;

void TemperatureSensor::PollTrvLocalTemperature()
{
	Nrf::Matter::BindingHandler::BindingData *data = Platform::New<Nrf::Matter::BindingHandler::BindingData>();
	if (data) {
		data->ClusterId = Clusters::Thermostat::Id;
		data->EndpointId = mTemperatureMeasurementEndpointId;
		data->InvokeCommandFunc = TrvLocalTemperatureReadHandler;
		BindingHandler::RunBoundClusterAction(data);
	}
}

void TemperatureSensor::TrvLocalTemperatureReadHandler(
	const chip::app::Clusters::Binding::TableEntry &binding, OperationalDeviceProxy *deviceProxy,
	Nrf::Matter::BindingHandler::BindingData &bindingData)
{
	auto *dataPointer = Platform::New<Nrf::Matter::BindingHandler::BindingData>(bindingData);
	if (dataPointer == nullptr) {
		LOG_ERR("Failed to allocate BindingData");
		return;
	}

	if (deviceProxy == nullptr || !deviceProxy->ConnectionReady()) {
		LOG_ERR("Device invalid");
		Platform::Delete<Nrf::Matter::BindingHandler::BindingData>(dataPointer);
		return;
	}

	/* Unlike OccupiedHeatingSetpoint/PIHeatingDemand, Thermostat::LocalTemperature
	 * IS nullable - dataResponse needs the .IsNull()/.Value() treatment. */
	auto onSuccess = [dataPointer, remoteNodeId = binding.nodeId](const ConcreteDataAttributePath &attributePath,
									const auto &dataResponse) {
		ChipLogProgress(NotSpecified, "Read TRV LocalTemperature succeeded");

		if (sPrimaryTrvNodeId == 0) {
			sPrimaryTrvNodeId = remoteNodeId;
			LOG_INF("Primary TRV locked to node 0x%llX", (unsigned long long)remoteNodeId);
		}
		if (remoteNodeId != sPrimaryTrvNodeId) {
			LOG_INF("Ignoring poll response from non-primary TRV (node 0x%llX)",
				(unsigned long long)remoteNodeId);
			BindingHandler::OnInvokeCommandSucces(dataPointer);
			return;
		}

		if (!dataResponse.IsNull()) {
			int16_t trvLocalTemp = dataResponse.Value();
			auto ownTemp = TemperatureManager::Instance().GetLocalTemp();
			if (!ownTemp.IsNull()) {
				int16_t rawOffset = static_cast<int16_t>(trvLocalTemp - ownTemp.Value());
				int16_t cappedOffset = rawOffset;
				if (cappedOffset > kMaxTrvOffsetMagnitude) {
					cappedOffset = kMaxTrvOffsetMagnitude;
				} else if (cappedOffset < -kMaxTrvOffsetMagnitude) {
					cappedOffset = -kMaxTrvOffsetMagnitude;
				}

				if (cappedOffset != rawOffset) {
					/* A very large offset (e.g. TRV mounted right on the
					 * radiator body) would otherwise dominate the compensated
					 * setpoint and push it straight into the TRV's own
					 * hardware clamp in ExternalSetpointWriteHandler. Capping
					 * it here keeps a single bad/extreme reading from taking
					 * over, though a persistently large real offset still
					 * means the achievable room temperature via this TRV is
					 * physically limited - no clamp fixes that. */
					LOG_WRN("TRV offset %d exceeds +/-%d, capping to %d", rawOffset,
						kMaxTrvOffsetMagnitude, cappedOffset);
				}

				sTrvLocalTempOffset = cappedOffset;
				sHasValidTrvOffset = true;
				LOG_INF("TRV/Wandthermostat offset updated: %d (TRV=%d, own=%d)",
					sTrvLocalTempOffset, trvLocalTemp, ownTemp.Value());
			}
		}

		RecordTrvPollSuccess();
		BindingHandler::OnInvokeCommandSucces(dataPointer);
	};

	auto onFailure = [dataPointer, remoteNodeId = binding.nodeId](const ConcreteDataAttributePath *attributePath,
									CHIP_ERROR error) {
		ChipLogError(NotSpecified, "Read TRV LocalTemperature failed: %" CHIP_ERROR_FORMAT, error.Format());
		if (sPrimaryTrvNodeId == 0 || remoteNodeId == sPrimaryTrvNodeId) {
			RecordTrvPollFailure();
		}
		Platform::Delete<Nrf::Matter::BindingHandler::BindingData>(dataPointer);
	};

	Controller::ReadAttribute<Clusters::Thermostat::Attributes::LocalTemperature::TypeInfo>(
		deviceProxy->GetExchangeManager(), deviceProxy->GetSecureSession().Value(), binding.remote, onSuccess,
		onFailure);
}