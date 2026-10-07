/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#pragma once

#include "binding/binding_handler.h"

#include <app/ReadClient.h>
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
	void ExternalSetpointWrite(int16_t heatingSetpoint);
	void ExternalSystemModeWrite(uint8_t mode);

	/* Periodic poll of the bound TRV: mirrors its OccupiedHeatingSetpoint
	 * into our local setpoint, and reads PIHeatingDemand for display only. */
	void PollTrvSetpoint();
	void PollTrvHeatingDemand();
	void PollTrvLocalTemperature();

	/* Starts the Matter subscription to the primary TRV once its node is known.
	 * Idempotent, meant to be called on every measurement cycle. The polls above
	 * keep running as fallback, also while subscribed. */
	void EnsureTrvSubscribed();

	/* True if value is the setpoint the TRV already has from its last remote update,
	 * so a local write of the same value would be redundant. */
	static bool IsRemoteAppliedSetpoint(int16_t value);
	/* Called on every local write, so a later local change to the same value is
	 * never suppressed by a stale remote marker. */
	static void ClearRemoteAppliedSetpoint();

private:
	class TrvSubscriptionCallback;

	static void SetRemoteSetpoint(int16_t value);
	static bool sHasRemoteAppliedSetpoint;
	static int16_t sRemoteAppliedSetpoint;

	static void SubscribeTrvHandler(const chip::app::Clusters::Binding::TableEntry &binding,
					chip::OperationalDeviceProxy *deviceProxy,
					BindingHandler::BindingData &bindingData);

	static chip::app::ReadClient *sTrvReadClient;
	static TrvSubscriptionCallback sTrvCallback;
	static bool sTrvSubscribing;
	static bool sTrvSubscribed;
	static bool sTrvFallbackToPolling;
	static int64_t sTrvSubscribeStartMs;
	static chip::NodeId sTrvSubscribedNodeId;


	static void ExternalTemperatureMeasurementReadHandler(const chip::app::Clusters::Binding::TableEntry &binding,
							      chip::OperationalDeviceProxy *deviceProxy,
							      BindingHandler::BindingData &bindingData);

	static void ExternalSetpointWriteHandler(const chip::app::Clusters::Binding::TableEntry &binding,
						chip::OperationalDeviceProxy *deviceProxy,
						BindingHandler::BindingData &bindingData);

	static void TrvSetpointReadHandler(const chip::app::Clusters::Binding::TableEntry &binding,
					   chip::OperationalDeviceProxy *deviceProxy,
					   BindingHandler::BindingData &bindingData);

	static void TrvHeatingDemandReadHandler(const chip::app::Clusters::Binding::TableEntry &binding,
						chip::OperationalDeviceProxy *deviceProxy,
						BindingHandler::BindingData &bindingData);

	static void ExternalSystemModeWriteHandler(const chip::app::Clusters::Binding::TableEntry &binding,
						   chip::OperationalDeviceProxy *deviceProxy,
						   BindingHandler::BindingData &bindingData);

	static void TrvLocalTemperatureReadHandler(const chip::app::Clusters::Binding::TableEntry &binding,
						   chip::OperationalDeviceProxy *deviceProxy,
						   BindingHandler::BindingData &bindingData);

	static int16_t sPendingSetpointValue;
	static uint8_t sPendingSystemModeValue;
	static int16_t sTrvLocalTempOffset;
	/* True once a PollTrvLocalTemperature() cycle has actually completed, i.e.
	 * sTrvLocalTempOffset reflects a real reading rather than its 0 default. */
	static bool sHasValidTrvOffset;

	/* With multiple bound TRVs, PollTrv*() fan out to all of them - reading
	 * setpoint/offset/heating-demand from whichever answers last would make
	 * those values jump around randomly. Only the first TRV to ever answer a
	 * poll is used for sync/offset/display; writes still go to all bound
	 * TRVs (that's the point of binding more than one). 0 = not yet locked. */
	static chip::NodeId sPrimaryTrvNodeId;

	/* Last value we ourselves wrote to the TRV's OccupiedHeatingSetpoint, used
	 * by TrvSetpointReadHandler to tell "the TRV changed" from "we're reading
	 * back our own compensated write" and avoid the setpoint-echo/drift loop. */
	static int16_t sLastSentCompensatedSetpoint;

	/* SCD41 single-shot read runs in its own thread so the 5s blocking
	 * measurement never stalls the CHIP task. */
	static void Scd41ThreadEntry(void *, void *, void *);

	int16_t mPreviousTemperature;
	uint8_t mMockTempIndex;
	uint8_t mCycleCounter;

	k_thread mScd41Thread;
	k_thread_stack_t *mScd41ThreadStack;
};
