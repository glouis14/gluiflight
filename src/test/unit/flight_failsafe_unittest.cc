/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdbool.h>

#include <limits.h>

extern "C" {
    #include "debug.h"

    #include "platform.h"

    #include "common/axis.h"
    #include "common/maths.h"

    #include "config/runtime_config.h"

    #include "rx/rx.h"
    #include "flight/failsafe.h"

    failsafeState_t* failsafeInit(rxConfig_t *intialRxConfig);
}

#include "unittest_macros.h"
#include "gtest/gtest.h"

uint32_t testFeatureMask = 0;

enum {
    COUNTER_MW_DISARM = 0,
};
#define CALL_COUNT_ITEM_COUNT 1

static int callCounts[CALL_COUNT_ITEM_COUNT];

#define CALL_COUNTER(item) (callCounts[item])

void resetCallCounters(void) {
    memset(&callCounts, 0, sizeof(callCounts));
}

#define TEST_MID_RC 1495 // something other than the default 1500 will suffice.

rxConfig_t rxConfig;
failsafeConfig_t failsafeConfig;

//
// Stepwise tests
//

TEST(FlightFailsafeTest, TestFailsafeInitialState)
{
    // given
    memset(&rxConfig, 0, sizeof(rxConfig));
    rxConfig.midrc = TEST_MID_RC;

    // and
    memset(&failsafeConfig, 0, sizeof(failsafeConfig));
    failsafeConfig.failsafe_delay = 10; // 1 second
    failsafeConfig.failsafe_off_delay = 50; // 5 seconds

    // when
    useFailsafeConfig(&failsafeConfig);
    failsafeInit(&rxConfig);

    // then
    EXPECT_EQ(false, failsafeIsMonitoring());
    EXPECT_EQ(false, failsafeIsActive());
    EXPECT_EQ(FAILSAFE_IDLE, failsafePhase());
}

TEST(FlightFailsafeTest, TestFailsafeStartMonitoring)
{
    // when
    failsafeStartMonitoring();

    // then
    EXPECT_EQ(true, failsafeIsMonitoring());
    EXPECT_EQ(false, failsafeIsActive());
    EXPECT_EQ(FAILSAFE_IDLE, failsafePhase());
}

TEST(FlightFailsafeTest, TestFailsafeFirstCycle)
{
    // when
    failsafeOnRxCycleStarted();
    failsafeOnValidDataReceived();

    // and
    failsafeUpdateState();

    // then
    EXPECT_EQ(false, failsafeIsActive());
    EXPECT_EQ(FAILSAFE_IDLE, failsafePhase());
}

/*
 * FIXME failsafe assumes that calls to failsafeUpdateState() happen at a set frequency (50hz)
 * but that is NOT the case when using a RX_SERIAL or RX_MSP as in that case the rx data is processed as soon
 * as it arrives which may be more or less frequent.
 *
 * Since the failsafe uses a counter the counter would not be updated at the same frequency that the maths
 * in the failsafe code is expecting the failsafe will either be triggered to early or too late when using
 * RX_SERIAL or RX_MSP.
 *
 *  uint8_t failsafe_delay;                 // Guard time for failsafe activation after signal lost. 1 step = 0.1sec - 1sec in example (10)
 *
 *  static bool failsafeHasTimerElapsed(void)
 *  {
 *    return failsafeState.counter > (5 * failsafeConfig->failsafe_delay);
 *  }
 *
 *  static bool failsafeShouldHaveCausedLandingByNow(void)
 *  {
 *    return failsafeState.counter > 5 * (failsafeConfig->failsafe_delay + failsafeConfig->failsafe_off_delay);
 *  }
 *
 *  void failsafeOnValidDataReceived(void)
 *  {
 *    if (failsafeState.counter > 20)
 *      failsafeState.counter -= 20;
 *    else
 *      failsafeState.counter = 0;
 *  }
 *
 *  1000ms / 50hz = 20
 */

#define FAILSAFE_UPDATE_HZ 50

TEST(FlightFailsafeTest, TestFailsafeNotActivatedWhenReceivingData)
{
    // when
    int callsToMakeToSimulateTenSeconds = FAILSAFE_UPDATE_HZ * 10;

    for (int i = 0; i < callsToMakeToSimulateTenSeconds; i++) {
        failsafeOnRxCycleStarted();
        failsafeOnValidDataReceived();

        failsafeUpdateState();

        // then
        EXPECT_EQ(false, failsafeIsActive());
        EXPECT_EQ(FAILSAFE_IDLE, failsafePhase());
    }
}

TEST(FlightFailsafeTest, TestFailsafeDetectsRxLossAndStartsLanding)
{

    // given
    ENABLE_ARMING_FLAG(ARMED);

    // when
    failsafeOnRxCycleStarted();
    // no call to failsafeOnValidDataReceived();

    failsafeUpdateState();

    // then
    EXPECT_EQ(false, failsafeIsActive());
    EXPECT_EQ(FAILSAFE_IDLE, failsafePhase());

    //
    // currently one cycle must occur (above) so that the next cycle (below) can detect the lack of an update.
    //

    // when
    for (int i = 0; i < FAILSAFE_UPDATE_HZ - 1; i++) {

        failsafeOnRxCycleStarted();
        // no call to failsafeOnValidDataReceived();

        failsafeUpdateState();

        // then
        EXPECT_EQ(FAILSAFE_RX_LOSS_DETECTED, failsafePhase());
        EXPECT_EQ(false, failsafeIsActive());

    }

    //
    // one more cycle currently needed before the counter is re-checked.
    //

    // when
    failsafeOnRxCycleStarted();
    // no call to failsafeOnValidDataReceived();
    failsafeUpdateState();

    // then
    EXPECT_EQ(true, failsafeIsActive());
    EXPECT_EQ(FAILSAFE_LANDING, failsafePhase());
}

TEST(FlightFailsafeTest, TestFailsafeCausesLanding)
{
    // given
    int callsToMakeToSimulateFiveSeconds = FAILSAFE_UPDATE_HZ * 5;

    // when
    for (int i = 0; i < callsToMakeToSimulateFiveSeconds - 1; i++) {

        failsafeOnRxCycleStarted();
        // no call to failsafeOnValidDataReceived();

        failsafeUpdateState();

        // then
        EXPECT_EQ(FAILSAFE_LANDING, failsafePhase());
        EXPECT_EQ(true, failsafeIsActive());

    }

    // when
    failsafeOnRxCycleStarted();
    // no call to failsafeOnValidDataReceived();
    failsafeUpdateState();

    // then
    EXPECT_EQ(false, failsafeIsActive());
    EXPECT_EQ(FAILSAFE_LANDED, failsafePhase());
    EXPECT_EQ(1, CALL_COUNTER(COUNTER_MW_DISARM));
    EXPECT_TRUE(ARMING_FLAG(PREVENT_ARMING));

}

// STUBS

extern "C" {
int16_t rcData[MAX_SUPPORTED_RC_CHANNEL_COUNT];
uint8_t armingFlags;

void delay(uint32_t) {}

bool feature(uint32_t mask) {
    return (mask & testFeatureMask);
}

void mwDisarm(void) {
    callCounts[COUNTER_MW_DISARM]++;
}

}
