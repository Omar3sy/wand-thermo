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

/* Stueckweise Entladekurve fuer 2x Energizer Ultimate Lithium AA (L91) in
 * Serie, direkt an VDD (kein Regler/Diode dazwischen - sonst muss die ganze
 * Kurve um den Spannungsabfall des Bauteils verschoben werden). Basis:
 * Energizer L91-Datenblatt 1mA-Entladekurve (naechste oeffentlich verfuegbare
 * Rate zu unserem ~9uA-Idle-Profil - kein Sub-mA-Graph existiert) und die
 * nRF54L15-DK-Dokumentation ("nRF54L15 SoC direct supply", S.9: 1,8V-3,6V
 * erlaubter Bereich fuer Direktversorgung ohne Regler) als hartem 0%-Cutoff,
 * mit kleiner Marge ueber Nordics VDDPOR-Schwelle (1,75V laut Datenblatt).
 * Muss gegen den tatsaechlichen Schaltplan verifiziert werden, falls doch
 * ein Regler/eine Diode zwischen Batterie und VDD sitzt. */
struct BatteryCurvePoint {
	int32_t mv;
	uint8_t halfPercent; /* 0-200 = 0-100% */
};

constexpr BatteryCurvePoint kBatteryCurve[] = {
	{ 3400, 200 }, /* 100% - frisch */
	{ 3200, 170 }, /*  85% */
	{ 3100, 140 }, /*  70% - Plateau, L91 bei ~1mA sehr flach */
	{ 3000, 100 }, /*  50% */
	{ 2800, 50 },  /*  25% - Uebergang Richtung steilem Abfall */
	{ 2400, 20 },  /*  10% */
	{ 2000, 10 },  /*   5% */
	{ 1800, 0 },   /*   0% - Nordic-dokumentiertes Minimum fuer Direktversorgung */
};
constexpr size_t kBatteryCurvePoints = sizeof(kBatteryCurve) / sizeof(kBatteryCurve[0]);

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
	int32_t voltage = kBatteryCurve[0].mv; /* fallback if ADC read fails */

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

	/* Stueckweise lineare Interpolation zwischen den Stuetzpunkten der
	 * L91-Entladekurve (kBatteryCurve ist absteigend nach Spannung sortiert).
	 * Matter BatPercentRemaining ist in Halb-Prozent-Einheiten (0-200 = 0-100%). */
	uint8_t percent;
	if (voltage >= kBatteryCurve[0].mv) {
		percent = kBatteryCurve[0].halfPercent;
	} else if (voltage <= kBatteryCurve[kBatteryCurvePoints - 1].mv) {
		percent = kBatteryCurve[kBatteryCurvePoints - 1].halfPercent;
	} else {
		percent = kBatteryCurve[kBatteryCurvePoints - 1].halfPercent;
		for (size_t i = 0; i + 1 < kBatteryCurvePoints; i++) {
			const BatteryCurvePoint &hi = kBatteryCurve[i];
			const BatteryCurvePoint &lo = kBatteryCurve[i + 1];

			if (voltage <= hi.mv && voltage >= lo.mv) {
				int32_t span = hi.mv - lo.mv;
				int32_t offset = voltage - lo.mv;
				percent = static_cast<uint8_t>(
					lo.halfPercent +
					(offset * (hi.halfPercent - lo.halfPercent)) / span);
				break;
			}
		}
	}

	LOG_INF("Battery: %d mV (%d.%d %%)", voltage, percent / 2, (percent % 2) * 5);

	PlatformMgr().LockChipStack();

	using namespace Clusters::PowerSource;
	Attributes::BatVoltage::Set(kPowerSourceEndpoint, static_cast<uint32_t>(voltage));
	Attributes::BatPercentRemaining::Set(kPowerSourceEndpoint, percent);
	Attributes::Status::Set(kPowerSourceEndpoint, PowerSourceStatusEnum::kActive);
	Attributes::Order::Set(kPowerSourceEndpoint, 1);
	Attributes::Description::Set(kPowerSourceEndpoint, chip::CharSpan::fromCharString("Battery"));
	/* BUG FIX: percent ist in Halb-Prozent-Einheiten (0-200), die Vergleiche
	 * hier verglichen bisher faelschlich gegen echte Prozentwerte (20/10)
	 * statt gegen die Halb-Prozent-Skala - "critical" loeste also schon bei
	 * 10% echten Restladung statt 20% aus, "replacement needed" bei 5%
	 * statt 10%. Jetzt auf *2 korrigiert. */
	Attributes::BatChargeLevel::Set(kPowerSourceEndpoint,
					percent < 40 ? BatChargeLevelEnum::kCritical : BatChargeLevelEnum::kOk);
	Attributes::BatPresent::Set(kPowerSourceEndpoint, true);
	Attributes::BatReplacementNeeded::Set(kPowerSourceEndpoint, percent < 20);
	Attributes::BatReplaceability::Set(kPowerSourceEndpoint, BatReplaceabilityEnum::kUserReplaceable);

	PlatformMgr().UnlockChipStack();

	Nrf::PostTask([percent] { DisplayManager::Instance().UpdateBattery(percent); });

	k_work_schedule(&BatteryManager::Instance().mBatteryWork, K_HOURS(1));
}
