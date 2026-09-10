/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#pragma once

#include "binding/binding_handler.h"

#include <platform/CHIPDeviceLayer.h>
#include <zephyr/kernel.h>

using namespace ::Nrf::Matter;

class TemperatureSensor {
public:
	TemperatureSensor();

	static TemperatureSensor &Instance()
	{
		static TemperatureSensor sSensor;
		return sSensor;
	};

	void FullMeasurement();
	void ExternalMeasurement();
	void InternalMeasurement();

private:
	static void ExternalTemperatureMeasurementReadHandler(const chip::app::Clusters::Binding::TableEntry &binding,
							      chip::OperationalDeviceProxy *deviceProxy,
							      BindingHandler::BindingData &bindingData);

	/* SCD41 single-shot read runs in its own thread so the 5s blocking
	 * measurement never stalls the CHIP task. */
	static void Scd41ThreadEntry(void *, void *, void *);

	int16_t mPreviousTemperature;
	uint8_t mMockTempIndex;
	uint8_t mCycleCounter;

	k_thread mScd41Thread;
	k_thread_stack_t *mScd41ThreadStack;
};
