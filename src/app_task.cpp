/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"

#include "battery_manager.h"
#include "display_manager.h"
#include "temp_sensor_manager.h"
#include "temperature_manager.h"
#include "temperature_measurement/sensor.h"

#include "app/matter_init.h"
#include "app/task_executor.h"
#include "clusters/identify.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_MATTER_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

namespace
{
constexpr EndpointId kThermostatEndpointId = 1;

Nrf::Matter::IdentifyCluster sIdentifyCluster(kThermostatEndpointId);

#define TEMPERATURE_BUTTON_MASK DK_BTN2_MSK
#define BINDING_BUTTON_MASK DK_BTN2_MSK
#define MINUS_BUTTON_MASK DK_BTN3_MSK
#define PLUS_BUTTON_MASK DK_BTN4_MSK

constexpr int16_t kSetpointStep = 50;   /* 0.5 °C step, Matter units are centi-degrees */
constexpr int16_t kSetpointMin = 700;   /* 7.0 °C */
constexpr int16_t kSetpointMax = 3000;  /* 30.0 °C */
} /* namespace */

void AppTask::ButtonEventHandler(Nrf::ButtonState state, Nrf::ButtonMask hasChanged)
{
	if (BINDING_BUTTON_MASK & hasChanged) {
		if (BINDING_BUTTON_MASK & state) {
			Nrf::PostTask([] { TemperatureSensor::Instance().ExternalMeasurement(); });
		}
	}

	if (MINUS_BUTTON_MASK & hasChanged) {
		if (MINUS_BUTTON_MASK & state) {
			Nrf::PostTask([] {
				AppTask::AdjustSetpoint(-kSetpointStep);
				DisplayManager::Instance().ShowSetpointTemporarily();
			});
		}
	}

	if (PLUS_BUTTON_MASK & hasChanged) {
		if (PLUS_BUTTON_MASK & state) {
			Nrf::PostTask([] {
				AppTask::AdjustSetpoint(kSetpointStep);
				DisplayManager::Instance().ShowSetpointTemporarily();
			});
		}
	}
}

void AppTask::ThermostatHandler(const TemperatureButtonAction &action)
{
	if (action == TemperatureButtonAction::Pushed) {
		TemperatureManager::Instance().LogThermostatStatus();
	}
}

void AppTask::AdjustSetpoint(int16_t delta)
{
	using namespace chip::app::Clusters::Thermostat::Attributes;
	using chip::Protocols::InteractionModel::Status;

	int16_t current = 0;

	PlatformMgr().LockChipStack();

	if (OccupiedHeatingSetpoint::Get(kThermostatEndpointId, &current) == Status::Success) {
		int32_t newValue = static_cast<int32_t>(current) + delta;
		if (newValue < kSetpointMin) {
			newValue = kSetpointMin;
		}
		if (newValue > kSetpointMax) {
			newValue = kSetpointMax;
		}

		OccupiedHeatingSetpoint::Set(kThermostatEndpointId, static_cast<int16_t>(newValue));
		LOG_INF("Setpoint adjusted to %d.%d C", static_cast<int>(newValue / 100),
			static_cast<int>((newValue % 100) / 10));
	} else {
		LOG_ERR("Failed to read current setpoint");
	}

	PlatformMgr().UnlockChipStack();
}

CHIP_ERROR AppTask::Init()
{
	/* Initialize Matter stack */
	ReturnErrorOnFailure(Nrf::Matter::PrepareServer(Nrf::Matter::InitData{ .mPostServerInitClbk = [] {
		CHIP_ERROR err = TempSensorManager::Instance().Init();
		if (err != CHIP_NO_ERROR) {
			LOG_ERR("TempSensorManager Init fail");
			return err;
		}
		err = TemperatureManager::Instance().Init();
		if (err != CHIP_NO_ERROR) {
			LOG_ERR("TemperatureManager Init fail");
			return err;
		}
		err = BatteryManager::Instance().Init();
		if (err != CHIP_NO_ERROR) {
			LOG_ERR("BatteryManager Init fail");
			return err;
		}
		err = DisplayManager::Instance().Init();
		if (err != CHIP_NO_ERROR) {
			LOG_ERR("DisplayManager Init fail");
		}
		return err;
	} }));

	if (!Nrf::GetBoard().Init(ButtonEventHandler)) {
		LOG_ERR("User interface initialization failed.");
		return CHIP_ERROR_INCORRECT_STATE;
	}

	/* Register Matter event handler that controls the connectivity status LED based on the captured Matter network
	 * state. */
	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(Nrf::Board::DefaultMatterEventHandler, 0));

	ReturnErrorOnFailure(sIdentifyCluster.Init());

	return Nrf::Matter::StartServer();
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	while (true) {
		Nrf::DispatchNextTask();
	}

	return CHIP_NO_ERROR;
}
