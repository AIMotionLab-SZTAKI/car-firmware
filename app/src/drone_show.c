#include <math.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "timers.h"

#include "commander.h"
#include "crtp_commander_high_level.h"
#include "drone_show.h"
#include "gcs_light_effects.h"
#include "light_program.h"
#include "log.h"
#include "param.h"
#include "pm.h"
#include "preflight.h"
#include "stabilizer.h"
#include "supervisor.h"

#ifdef SHOW_MODE_SILENT
#  define DEBUG_PRINT(fmt, ...) /* nothing */
#else
#  define DEBUG_MODULE "SHOW"
#  include "debug.h"
#endif

/* Bit flags corresponding to the various commands that we may receive over
 * the CRTP link. The values of these macros must be powers of two. */

#define CMD_START 1
#define CMD_PAUSE 2
#define CMD_STOP 4
#define CMD_RESTART 8

#ifndef SHOW_MODE_SILENT
static const char* stateMessages[NUM_STATES] = {
  "Initializing.",
  "Deactivated.",
  "Waiting for preflight checks.",
  "Waiting for start signal.",
  "Waiting for takeoff time.",
  "Takeoff.",
  "Performing show.",
  "Landing.",
  "Landed.",

  "Battery low, landing.",
  "Battery flat.",
  "Unrecoverable error."
};
#endif

/* 20fps because we are now driving the LED light from the loop */
#define LOOP_INTERVAL_MSEC 50
#define TAKEOFF_HEIGHT_METERS 1.0f
#define TAKEOFF_DURATION_MSEC 2000
#define TAKEOFF_VELOCITY_METERS_PER_SEC (TAKEOFF_HEIGHT_METERS * 1000.0f / TAKEOFF_DURATION_MSEC)
#define LANDING_VELOCITY_METERS_PER_SEC 0.5f
#define LOW_BATTERY_DURATION_MSEC 5000

static xTimerHandle timer;
static bool isInit = false;
static bool isEnabled = false;
static bool isTesting = false;
static bool wasInTestingMode = false;

/** Value of the internal clock of the Crazyflie when the show clock starts,
 * in microseconds.
 */
static uint64_t startTime;

/** Timestamp when we switched state for the last time.
 */
static uint64_t lastStateSwitchAt;

/**
 * Relative timestamp when the trajectory that the Crazyflie should fly starts
 * in the show timeline, in seconds. Note that the Crazyflie actually needs to
 * take off earlier than this time, precisely by TAKEOFF_DURATION_MSEC
 * milliseconds.
 */
static float startOfTrajectoryRelativeToStartTime;

/**
 * Height where the Crazyflie should attempt landing to, in meters. This can be
 * set with a parameter. When controlled by Skybrush Live, this parameter is
 * set during the upload of the trajectory.
 */
static float landingHeight;

static uint8_t lastColor[] = {0x00, 0x00, 0x00};
static xSemaphoreHandle pendingCommandsSemaphore;
static uint8_t pendingCommands;
static show_state_t state = STATE_INITIALIZING;
static uint8_t waitCounter;
static uint8_t lowBatteryCounter;

static struct {
  paramVarId_t ledColorRed;
  paramVarId_t ledColorGreen;
  paramVarId_t ledColorBlue;
  paramVarId_t ledRingEffect;
  paramVarId_t highLevelCommanderEnabled;
} paramIds;

static void droneShowTimer(xTimerHandle timer);

static uint8_t desiredLEDEffectForState(show_state_t state);
static float getSecondsSinceLastStateSwitch();
static float getSecondsSinceStart();
static uint64_t getUsecTimestampForLightPatterns();
static bool hasStartTimePassed();
static bool hasTakeoffTimePassed();
static bool isErrorState(show_state_t state);
static bool isLandingState(show_state_t state);
static bool isStateOnGround(show_state_t state);
static bool onEnteredState(show_state_t state, show_state_t oldState);
static void onLeavingState(show_state_t state, show_state_t newState);
static void setState(show_state_t newState);
static bool shouldRunPreflightChecksInState(show_state_t state);
static bool shouldRunLightProgramInState(show_state_t state);
static bool shouldStartWithFailingPreflightChecks();
static void updateLEDRing();
static void updateTestingMode();

static void processPendingCommands();
static void processPauseCommand();
static void processStartCommand();
static void processStopCommand();

void droneShowInit() {
  if (isInit) {
    return;
  }

  vSemaphoreCreateBinary(pendingCommandsSemaphore);

  timer = xTimerCreate("ShowTimer", M2T(LOOP_INTERVAL_MSEC), pdTRUE, NULL, droneShowTimer);
  xTimerStart(timer, LOOP_INTERVAL_MSEC);

  /* Retrieve the IDs of the log variables and parameters that we will need */
  paramIds.highLevelCommanderEnabled = paramGetVarId("commander", "enHighLevel");
  paramIds.ledColorRed = paramGetVarId("ring", "solidRed");
  paramIds.ledColorGreen = paramGetVarId("ring", "solidGreen");
  paramIds.ledColorBlue = paramGetVarId("ring", "solidBlue");
  paramIds.ledRingEffect = paramGetVarId("ring", "effect");

  if (!PARAM_VARID_IS_VALID(paramIds.highLevelCommanderEnabled)) {
    return;
  }

  /* This triggers all the stuff that has to happen when entering the idle
   * state; this is the only reason why we also needed an "initializing"
   * state. We can call this only here because the state change handlers might
   * need the log/param IDs that we only set up above */
  setState(STATE_IDLE);

  isInit = true;
}

const uint8_t* droneShowGetLastColor() {
  return lastColor;
}

show_state_t droneShowGetState() {
  return state;
}

bool droneShowIsEnabled(void) {
  return isEnabled;
}

bool droneShowIsProbablyAirborne(void) {
  return (
    state == STATE_TAKEOFF ||
    state == STATE_PERFORMING_SHOW ||
    state == STATE_LANDING ||
    state == STATE_LANDING_LOW_BATTERY
  );
}

bool droneShowIsInTestingMode(void) {
  /* we are in testing mode if we were explicitly set to be in testing mode or
   * if a USB cable is plugged in and the drone couldn't fly anyway */
  return isTesting || !supervisorCanFly();
}

void droneShowRequestLEDRingControlModeEvaluation(void) {
  if (PARAM_VARID_IS_VALID(paramIds.ledRingEffect)) {
    paramSetInt(paramIds.ledRingEffect, desiredLEDEffectForState(state));
  }
}

void droneShowStart(void) {
  return droneShowDelayedStart(0);
}

void droneShowDelayedStart(int16_t delayMsec) {
  bool startTimeChanged = 0;
  uint64_t now = usecTimestamp();

  if (delayMsec == 0) {
    /* We need to start immediately */
    startTime = now;
    startTimeChanged = 1;
  } else if (delayMsec > 0) {
    /* We need to start a bit later; calculate the expected start time */
    startTime = now + delayMsec * 1000.0f;
    startTimeChanged = 1;
    DEBUG_PRINT("Starting in %.2fs\n", delayMsec / 1000.0);
  } else if (delayMsec < 0 && startTime == 0) {
    /* We are late, but if we have no start time yet and the takeoff time is
     * far enough in the future, we can still catch up */
    if (startOfTrajectoryRelativeToStartTime * 1000.0f >= -delayMsec) {
      /* delayMsec is negative, so we will set a start time that is in the
       * past. This is okay, this is what we want. */
      startTime = now + delayMsec * 1000.0f;
      startTimeChanged = 1;
      DEBUG_PRINT("Catching up, late by %.2fs\n", -delayMsec / 1000.0);
    }
  }

  if (startTimeChanged) {
    lightProgramPlayerSchedulePlayFrom(
      startTime, /* programId = */ 0, /* timescale = */ 1
    );
  }

  if (startTime <= now) {
    xSemaphoreTake(pendingCommandsSemaphore, portMAX_DELAY);
    pendingCommands |= CMD_START;
    xSemaphoreGive(pendingCommandsSemaphore);
  }
}

void droneShowPause(void) {
  /*
  xSemaphoreTake(pendingCommandsSemaphore, portMAX_DELAY);
  pendingCommands |= CMD_PAUSE;
  xSemaphoreGive(pendingCommandsSemaphore);
  */

  /* Not implemented yet */
}

void droneShowRestart(void) {
  xSemaphoreTake(pendingCommandsSemaphore, portMAX_DELAY);
  pendingCommands |= CMD_RESTART;
  xSemaphoreGive(pendingCommandsSemaphore);
}

void droneShowStop(void) {
  xSemaphoreTake(pendingCommandsSemaphore, portMAX_DELAY);
  pendingCommands |= CMD_STOP;
  xSemaphoreGive(pendingCommandsSemaphore);

  startTime = 0;
}

bool droneShowTest(void) {
  return isInit;
}

/**
 * Timer function that is called regularly (10 times every second by default).
 * This is where we should handle the state transition logic.
 */
static void droneShowTimer(xTimerHandle timer) {
  /* First of all, if the module is not active, we are not in the idle state and
   * it can safely be assumed that the drone is on the ground, switch back to
   * the idle state. Otherwise, drive the drone to the landing state and then
   * switch back to idling when the landing has completed. */
  if (state != STATE_IDLE && !isEnabled) {
    if (isStateOnGround(state)) {
      setState(STATE_IDLE);
    } else if (!isErrorState(state)) {
      setState(STATE_LANDING);
    }
  }

  /* Next, if we are airborne and the battery voltage goes below the threshold
   * for an extended period of time, start landing */
  if (!isStateOnGround(state) && !isLandingState(state)) {
    if (pmGetBatteryVoltage() < PM_BAT_CRITICAL_LOW_VOLTAGE + 0.1f) {
      lowBatteryCounter++;
      if (lowBatteryCounter >= LOW_BATTERY_DURATION_MSEC / LOOP_INTERVAL_MSEC) {
        setState(STATE_LANDING_LOW_BATTERY);
      }
    } else {
      lowBatteryCounter = 0;
    }
  }

  /* Also, if we are airborne and the tumble detector kicks in, move to the
   * error state immediately */
  if (isEnabled) {
    if (droneShowIsProbablyAirborne() && supervisorIsTumbled()) {
      setState(STATE_ERROR);
    }
  }

  /* Process any pending commands that we have received */
  processPendingCommands();

  /* TODO(ntamas): after a state switch, we should evaluate what should happen
   * at the new state instead of waiting for the next iteration as that would
   * introduce a delay of ~50msec (one loop iteration) */

  /* Switch states if needed */
  switch (state) {
    case STATE_IDLE:
      if (isEnabled) {
        if (getPreflightCheckSummary() == preflightResultPass) {
          setState(STATE_WAIT_FOR_START_SIGNAL);
        } else {
          setState(STATE_WAIT_FOR_PREFLIGHT_CHECK);
        }
      }
      break;

    case STATE_WAIT_FOR_PREFLIGHT_CHECK:
      if (getPreflightCheckSummary() == preflightResultPass) {
        setState(STATE_WAIT_FOR_START_SIGNAL);
      } else if (hasStartTimePassed()) {
        if (shouldStartWithFailingPreflightChecks()) {
          setState(STATE_WAIT_FOR_TAKEOFF_TIME);
        }
      }
      break;

    case STATE_WAIT_FOR_START_SIGNAL:
      if (getPreflightCheckSummary() != preflightResultPass) {
        setState(STATE_WAIT_FOR_PREFLIGHT_CHECK);
      } else if (hasStartTimePassed()) {
        setState(STATE_WAIT_FOR_TAKEOFF_TIME);
      }
      break;

    case STATE_WAIT_FOR_TAKEOFF_TIME:
      if (hasTakeoffTimePassed()) {
        setState(STATE_TAKEOFF);
      }
      break;

    case STATE_TAKEOFF:
      if (waitCounter > 0) {
        waitCounter--;
      } else {
        setState(STATE_PERFORMING_SHOW);
      }
      break;

    case STATE_PERFORMING_SHOW:
      if (crtpCommanderHighLevelIsTrajectoryFinished()) {
        /* show finished, let's land */
        setState(STATE_LANDING);
      }
      break;

    case STATE_LANDING:
      if (crtpCommanderHighLevelIsTrajectoryFinished()) {
        /* landing finished, let's stop the commander completely and go to
         * the landed state */
        setState(STATE_LANDED);
      }
      break;

    case STATE_LANDING_LOW_BATTERY:
      if (crtpCommanderHighLevelIsTrajectoryFinished()) {
        /* landing finished, let's got to the landed state */
        setState(STATE_LANDED);
      }
      break;

    case STATE_LANDED:
      /* This is a sink state; we can go back to the idle state if we receive
       * a STOP or RESTART command, or we also go back automatically after
	     * 30 seconds */
      if (getSecondsSinceLastStateSwitch() > 30) {
        setState(STATE_IDLE);
      }
      break;

    default:
      setState(STATE_ERROR);
  }

  /* Update the LED ring on the drone */
  updateLEDRing();

  /* Update whether the drone is in testing mode */
  updateTestingMode();
}

/**
 * Returns the number of microseconds elapsed since the start time of the show;
 * negative if the show has not started yet but we have a scheduled start
 * time; minus infinity if we have no scheduled start time yet.
 */
static int64_t getMicrosecondsSinceStart() {
  return startTime > 0 ? (int64_t) (usecTimestamp() - startTime) : INT64_MIN;
}

/**
 * Returns the number of seconds elapsed since the last state switch, in
 * seconds. The return value is guaranteed to be non-negative.
 */
static float getSecondsSinceLastStateSwitch() {
  return ((uint64_t) (usecTimestamp() - lastStateSwitchAt)) / 1000000.0f;
}

/**
 * Returns the number of seconds elapsed since the start time of the show;
 * negative if the show has not started yet but we have a scheduled start
 * time; minus infinity if we have no scheduled start time yet.
 */
static float getSecondsSinceStart() {
  if (startTime > 0) {
    return ((int64_t) (usecTimestamp() - startTime)) / 1000000.0f;
  } else {
    return -INFINITY;
  }
}

/**
 * Returns a microsecond timestamp that is suitable for (synchronized) light
 * patterns across multiple Crazyflie drones if an external timing signal is
 * provided.
 */
static uint64_t getUsecTimestampForLightPatterns() {
  if (startTime > 0) {
    return getMicrosecondsSinceStart();
  } else {
    return usecTimestamp();
  }
}

/**
 * Returns whether the show start time has passed. (It does not necessarily
 * mean that the _takeoff_ time has passed, though).
 */
static bool hasStartTimePassed() {
  return startTime > 0 && usecTimestamp() > startTime;
}

/**
 * Returns whether the takeoff time has passed and it is time to take off.
 */
static bool hasTakeoffTimePassed() {
  float secondsSinceStart;
  float takeoffTimeRelativeToStartTime;

  if (startTime <= 0) {
    /* No start time yet */
    return 0;
  }

  takeoffTimeRelativeToStartTime = (
    startOfTrajectoryRelativeToStartTime - TAKEOFF_DURATION_MSEC / 1000.0f
  );

  secondsSinceStart = getSecondsSinceStart();
  if (takeoffTimeRelativeToStartTime < 0) {
    /* This happens if the start time of the trajectory of the drone within the
     * show is less than the time we have allocated for the takeoff; in that case,
     * takeoffTimeRelativeToStartTime is negative. However, we still don't
     * ever want to start taking off _before_ the designated start time, so
     * we compare secondsSinceStart with zero instead. */
    return secondsSinceStart >= 0;
  } else {
    return (secondsSinceStart >= takeoffTimeRelativeToStartTime);
  }
}

/**
 * Returns the index of the desired LED effect of the LED ring deck for the
 * given show state.
 */
static uint8_t desiredLEDEffectForState(show_state_t state) {
  if (areGcsLightEffectsActive()) {
    return 7;     /* solid color */
  } else if (shouldRunPreflightChecksInState(state)) {
    return 7;     /* solid color */
  } else if (shouldRunLightProgramInState(state)) {
    return 7;     /* solid color */
  } else if (isErrorState(state)) {
    return 11;    /* siren */
  } else {
    return 0;     /* off */
  }
}

/**
 * Returns whether the given state means that the drone is currently in an
 * error state.
 */
static bool isErrorState(show_state_t state) {
  return (
    state == STATE_ERROR ||
    state == STATE_LANDING_LOW_BATTERY ||
    state == STATE_EXHAUSTED
  );
}

/**
 * Returns whether the given state means that the drone is currently attempting
 * to land.
 */
static bool isLandingState(show_state_t state) {
  return state == STATE_LANDING || state == STATE_LANDING_LOW_BATTERY;
}

/**
 * Returns whether the given state means that the drone is currently on the
 * ground.
 *
 * If the state means that the drone is on the ground (most likely), it means
 * that it is safe to deactivate the drone show module and switch back to the
 * idle state.
 */
static bool isStateOnGround(show_state_t state) {
  return (
    state != STATE_TAKEOFF &&
    state != STATE_PERFORMING_SHOW &&
    !isLandingState(state)
  );
}

/**
 * Handler function that is called when the drone show module has entered a
 * given state.
 */
static bool onEnteredState(show_state_t state, show_state_t oldState) {
  int result;
  bool success = 1;   /* be optimistic :) */
  float offset;

  /* If we have entered the idle or landed state, stop whatever the high-level
   * commander is doing */
  if (state == STATE_IDLE || state == STATE_LANDED) {
    crtpCommanderHighLevelStop();
    paramSetInt(paramIds.highLevelCommanderEnabled, 0);
  }

  /* If we have left the idle state and we did not go into the error state,
   * turn the high-level controller on */
  if ((oldState == STATE_IDLE || oldState == STATE_LANDED) &&
      (state != STATE_ERROR && state != STATE_IDLE && state != STATE_LANDED)) {
    paramSetInt(paramIds.highLevelCommanderEnabled, 1);
  }

  /* Enable the preflight checks if needed */
  preflightSetEnabled(shouldRunPreflightChecksInState(state));

  /* Update the effect of the LED ring if we have an LED ring */
  droneShowRequestLEDRingControlModeEvaluation();

  /* Clear the "startTime" variable if we are on the ground and we are not
   * waiting for the takeoff time */
  if (isStateOnGround(state) && state != STATE_WAIT_FOR_PREFLIGHT_CHECK &&
      state != STATE_WAIT_FOR_TAKEOFF_TIME) {
    startTime = 0;
    lightProgramPlayerStop();
  }

  /* If we have entered the preflight checks from the idle stage, start by
   * resetting the Kalman filter if it is not passing yet */
  if (state == STATE_WAIT_FOR_PREFLIGHT_CHECK && oldState == STATE_IDLE) {
    if (!isPreflightCheckPassing(PREFLIGHT_CHECK_KALMAN_FILTER)) {
      preflightResetKalmanFilterToHome();
    }
  }

  /* If we have entered the "takeoff" state, start the takeoff procedure.
   * We do this separately from the trajectory following to ensure that
   * we always take off vertically */
  if (state == STATE_TAKEOFF) {
    /* Start the takeoff */
    crtpCommanderHighLevelTakeoffWithVelocity(
      TAKEOFF_HEIGHT_METERS, TAKEOFF_VELOCITY_METERS_PER_SEC, /* relative = */ 1
    );

    /* Start a counter as well so we indicate that the trajectory starts with
     * a takeoff phase -- we will move to the "performing" phase when the
     * timeout is over */
    waitCounter = ceilf(((float)TAKEOFF_DURATION_MSEC) / LOOP_INTERVAL_MSEC) - 1;
    lowBatteryCounter = 0;
  }

  /* If we have entered the "performing" state, start the trajectory */
  if (state == STATE_PERFORMING_SHOW && oldState == STATE_TAKEOFF) {
    offset = getSecondsSinceStart() - startOfTrajectoryRelativeToStartTime;
    if (offset > 0) {
      DEBUG_PRINT("Offset into trajectory: %.2fs\n", (double) offset);
    }
    result = crtpCommanderHighLevelStartTrajectoryWithOffset(
      0,
      /* offset = */ offset >= 0 ? offset : 0,
      /* timescale = */ 1,
      /* relative = */ 0,
      /* reversed = */ 0
    );
    if (result) {
      DEBUG_PRINT("Failed to start trajectory (code %d)\n", result);
      success = 0;
    };
  }

  /* If we have entered the "landing" state, start the landing */
  if (!isStateOnGround(oldState) && (
    state == STATE_LANDING || state == STATE_LANDING_LOW_BATTERY
  )) {
    crtpCommanderHighLevelLandWithVelocity(landingHeight, LANDING_VELOCITY_METERS_PER_SEC, /* relative = */ 0);
  }

  /* Return whether the state switch was successful */
  return success;
}

/**
 * Handler function that is called when the drone show module is laeving a
 * given state.
 */
static void onLeavingState(show_state_t state, show_state_t oldState) {

}

/**
 * Processes a START command received over the CRTP link.
 */
static void processStartCommand() {
  if (isEnabled && isStateOnGround(state)) {
    setState(STATE_WAIT_FOR_TAKEOFF_TIME);
  }
}

/**
 * Processes a PAUSE command received over the CRTP link.
 */
static void processPauseCommand() {
  /* NOT IMPLEMENTED YET */
  processStopCommand();
}

/**
 * Processes a RESTART command received over the CRTP link.
 */
static void processRestartCommand() {
  if (isStateOnGround(state)) {
    setState(STATE_IDLE);
  }
}

/**
 * Processes a STOP command received over the CRTP link.
 */
static void processStopCommand() {
  if (isStateOnGround(state)) {
    setState(STATE_IDLE);
  } else if (!isLandingState(state)) {
    setState(STATE_LANDING);
  }
}

/**
 * Processes all pending commands that we have received over the CRTP link.
 */
static void processPendingCommands() {
  uint8_t commands = 0;

  /* This function is invoked from the timer task so we cannot block on the
   * semaphore. If it is not available, we just return */
  if (xSemaphoreTake(pendingCommandsSemaphore, 0)) {
    commands = pendingCommands;
    pendingCommands = 0;
    xSemaphoreGive(pendingCommandsSemaphore);
  }

  /* Commands have an order of precedence: stop trumps pause, pause beats
   * start */
  if (commands & CMD_STOP) {
    processStopCommand();
  } else if (commands & CMD_PAUSE) {
    processPauseCommand();
  } else if (commands & CMD_START) {
    processStartCommand();
  } else if (commands & CMD_RESTART) {
    processRestartCommand();
  }
}

/**
 * Changes the state of the module and prints an appropriate logging message.
 */
static void setState(show_state_t newState) {
  show_state_t oldState;

  if (state == newState) {
    return;
  }

  onLeavingState(state, newState);

  oldState = state;
  state = newState;

  if (!onEnteredState(state, oldState)) {
    /* state switch failed */
    setState(STATE_ERROR);
  } else {
    /* state switch succeeded */
    if (oldState != STATE_INITIALIZING) {
      if (state < NUM_STATES) {
        DEBUG_PRINT("%s\n", stateMessages[state]);
      } else {
        DEBUG_PRINT("Switched to state %d.\n", (int) newState);
      }
    }
  }

  lastStateSwitchAt = usecTimestamp();
}

/**
 * Returns whether the preflight checks should be running in the given state.
 */
static bool shouldRunPreflightChecksInState(show_state_t state) {
  return (
    state == STATE_WAIT_FOR_PREFLIGHT_CHECK ||
    state == STATE_WAIT_FOR_START_SIGNAL
  );
}

/**
 * Returns whether the light program should be playing on the LED ring in the
 * given state.
 */
static bool shouldRunLightProgramInState(show_state_t state) {
  return (
    state == STATE_WAIT_FOR_TAKEOFF_TIME ||
    state == STATE_TAKEOFF ||
    state == STATE_PERFORMING_SHOW ||
    state == STATE_LANDING ||
    state == STATE_LANDED
  );
}

/**
 * Returns whether the drone should attempt to start the show even if the
 * preflight checks are failing.
 */
static bool shouldStartWithFailingPreflightChecks() {
  /* It makes absolutely no sense to start if there is no trajectory uploaded
   * to the drone. In all other cases, it _might_ make sense to start (although
   * it is somewhat unsafe */
  return isPreflightCheckPassing(PREFLIGHT_CHECK_TRAJECTORY_AND_LIGHTS);
}

/**
 * Helper function that modulates a color based on an Apple-style "breathing"
 * light pattern.
 */
static void modulateColorWithBreathingPattern(uint8_t* color, uint64_t timestamp) {
  static const float lookupTable[101] = {
    0.0, 0.037, 0.074, 0.11, 0.145, 0.18, 0.214, 0.248, 0.281, 0.313, 0.345,
    0.376, 0.406, 0.436, 0.465, 0.493, 0.521, 0.548, 0.574, 0.6, 0.625, 0.649,
    0.672, 0.695, 0.716, 0.737, 0.758, 0.777, 0.796, 0.814, 0.831, 0.847,
    0.863, 0.878, 0.891, 0.904, 0.917, 0.928, 0.939, 0.948, 0.957, 0.965,
    0.973, 0.979, 0.985, 0.989, 0.993, 0.996, 0.998, 1.0, 1.0, 1.0, 0.998,
    0.996, 0.993, 0.989, 0.985, 0.979, 0.973, 0.965, 0.957, 0.948, 0.939,
    0.928, 0.917, 0.904, 0.891, 0.878, 0.863, 0.847, 0.831, 0.814, 0.796,
    0.777, 0.758, 0.737, 0.716, 0.695, 0.672, 0.649, 0.625, 0.6, 0.574, 0.548,
    0.521, 0.493, 0.465, 0.436, 0.406, 0.376, 0.345, 0.313, 0.281, 0.248,
    0.214, 0.18, 0.145, 0.11, 0.074, 0.037, 0.0
  };
  uint8_t index = (timestamp % 5000000) / 50000;
  float factor = lookupTable[index];

  color[0] *= factor;
  color[1] *= factor;
  color[2] *= factor;
}

/**
 * Updates the color on the LED ring.
 */
static void updateLEDRing() {
  uint64_t now;
  bool canStart;
  bool updateLights = true;
  preflight_check_result_t preflightCheckSummary;

  if (areGcsLightEffectsActive()) {
    /* The user has explicitly overridden the color of the LED ring from the
     * GCS so we use that color */
    gcsLightEffectEvaluate(lastColor);
  } else if (shouldRunPreflightChecksInState(state)) {
    /* Preflight checks are running in the current state so set the color of the
     * LED ring based on the preflight checks */
    preflightCheckSummary = getPreflightCheckSummary();
    canStart = (preflightCheckSummary == preflightResultPass) || shouldStartWithFailingPreflightChecks();

    if (canStart && getSecondsSinceStart() >= -10) {
      /* light blue: start time received, will start soon */
      lastColor[0] = 0;
      lastColor[1] = 25;
      lastColor[2] = 50;
      if (preflightCheckSummary == preflightResultPass) {
        now = getUsecTimestampForLightPatterns();
        modulateColorWithBreathingPattern(lastColor, now);
      }
    } else {
      /* show the status of the preflight check instead */
      switch (preflightCheckSummary) {
        case preflightResultFail:
          lastColor[0] = 50;
          lastColor[1] = lastColor[2] = 0;
          break;

        case preflightResultWait:
          now = getUsecTimestampForLightPatterns();
          lastColor[0] = lastColor[1] = (now % 1000000 <= 125000) ? 50 : 0;
          lastColor[2] = 0;
          break;

        case preflightResultPass:
          now = getUsecTimestampForLightPatterns();
          lastColor[1] = 50;
          lastColor[0] = lastColor[2] = 0;
          modulateColorWithBreathingPattern(lastColor, now);
          break;

        default:
          lastColor[0] = lastColor[1] = lastColor[2] = 0;
          break;
      }
    }
  } else if (shouldRunLightProgramInState(state)) {
    /* Light program is running in this state so evaluate the light program */
    lightProgramPlayerEvaluate(lastColor);
  } else {
    /* we are not controlling the LED ring in this state. Errors are handled by
     * the "siren" pattern */
    lastColor[0] = lastColor[1] = lastColor[2] = 0;
    updateLights = false;
  }

  if (updateLights) {
    paramSetInt(paramIds.ledColorRed, lastColor[0]);
    paramSetInt(paramIds.ledColorGreen, lastColor[1]);
    paramSetInt(paramIds.ledColorBlue, lastColor[2]);
  }
}

/**
 * Updates whether the drone is currently in testing mode.
 */
static void updateTestingMode() {
  /* Prevent the motors from spinning if we are in testing mode. Note that we
   * act only if the value of droneShowIsInTestingMode() changes -- this is to
   * ensure that we do not start fighting with the tumble detector if the drone
   * tumbles during a show */
  if (droneShowIsInTestingMode()) {
    if (!wasInTestingMode) {
      stabilizerSetEmergencyStop();
      wasInTestingMode = true;
    }
  } else {
    if (wasInTestingMode) {
      stabilizerResetEmergencyStop();
      wasInTestingMode = false;
    }
  }
}

PARAM_GROUP_START(show)
PARAM_ADD(PARAM_UINT8, enabled, &isEnabled)
PARAM_ADD(PARAM_UINT8, testing, &isTesting)
PARAM_ADD(PARAM_FLOAT, takeoffTime, &startOfTrajectoryRelativeToStartTime)
PARAM_ADD(PARAM_FLOAT, landingHeight, &landingHeight)
PARAM_GROUP_STOP(show)
