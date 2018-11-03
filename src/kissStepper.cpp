/*
kissStepper - a lightweight library for the Easy Driver, Big Easy Driver, Allegro stepper motor drivers and others that use a Step/Dir interface
Written by Rylee Isitt. September 21, 2015
License: GNU Lesser General Public License (LGPL) V2.1

Despite the existence of several excellent libraries for driving stepper motors, I created this one to fulfill the following needs:
- Simplicity
- Handling of enable, step, and dir pins
- Based around an external loop
- Approximately linear acceleration using a fast algorithm
- High step frequency (or reasonably so, given the overhead involved)
- Use AVR/ARM libraries and port access to increase performance while keeping the API Arduino-friendly
- Teensy (Teensyduino) compatibility

Acceleration approximation math is based on Aryeh Eiderman's "Real Time Stepper Motor Linear Ramping Just by Addition and Multiplication", available at http://hwml.com/LeibRamp.pdf
*/

/*
Optimization notes:
- Keeping an integer copy of stepInterval improves performance at a cost of some memory
- Making stepInterval an integer instead of float greatly decreases accuracy of accel/decel and doesn't make a noticeable difference in performance
*/

#include "kissStepper.h"
#ifndef TEENSYDUINO
#include <util/delay_basic.h>
#endif

// ----------------------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------------------
// kissStepper without acceleration
// ----------------------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------------------

kissStepperNoAccel::kissStepperNoAccel(uint8_t PIN_DIR, uint8_t PIN_STEP, uint8_t PIN_ENABLE) :
    m_forwardLimit(DEFAULT_FORWARD_LIMIT),
    m_reverseLimit(DEFAULT_REVERSE_LIMIT),
    m_maxSpeed(DEFAULT_SPEED),
    PIN_DIR(PIN_DIR),
    PIN_STEP(PIN_STEP),
    PIN_ENABLE(PIN_ENABLE),
    m_distMoved(0),
    m_pos(0),
    m_stepBit(digitalPinToBitMask(PIN_STEP)),
    m_stepOut(portOutputRegister(digitalPinToPort(PIN_STEP)))
{}

// ----------------------------------------------------------------------------------------------------
// Initialize the motor in a default state
// ----------------------------------------------------------------------------------------------------

void kissStepperNoAccel::begin(void)
{

    // set pins to output
    pinMode(PIN_DIR, OUTPUT);
    pinMode(PIN_STEP, OUTPUT);
    if (PIN_ENABLE != 255) pinMode(PIN_ENABLE, OUTPUT);

    // initial STEP pin state
    digitalWrite(PIN_STEP, LOW);

    // set to move forwards (DIR pin low)
    setDir(true);

    // start with motor controller disabled
    disable();

}

// ----------------------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------------------

void kissStepperNoAccel::enable(void)
{
    if (PIN_ENABLE != 255) digitalWrite(PIN_ENABLE, LOW);
    m_enabled = true;
}

// ----------------------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------------------

void kissStepperNoAccel::disable(void)
{
    stop();
    if (PIN_ENABLE != 255) digitalWrite(PIN_ENABLE, HIGH);
    m_enabled = false;
}

// ----------------------------------------------------------------------------------------------------
// Does some basic checks, enforces limits, calculates the step interval, and switches to STATE_STARTING
// ----------------------------------------------------------------------------------------------------

bool kissStepperNoAccel::prepareMove(int32_t target)
{
    // only continue if not already moving
    if (m_kissState == STATE_STOPPED)
    {
        // constrain the target between reverseLimit and forwardLimit
        target = constrain(target, m_reverseLimit, m_forwardLimit);

        // only continue if movement is required (positive distance) and possible (positive speed)
        if ((target != m_pos) && (m_maxSpeed > 0))
        {

            // enable the motor controller if necessary
            if (!m_enabled) enable();

            // set the direction
            setDir(target > m_pos);

            // set initial state
            m_kissState = STATE_STARTING;

            // calculate speed profile
            m_distTotal = (target > m_pos) ? (target - m_pos) : (m_pos - target);

            // start motor at full speed
            // don't need to set float version of stepInterval since it isn't used during run
            m_stepIntervalWhole = ONE_SECOND / m_maxSpeed;
            if ((ONE_SECOND % m_maxSpeed) >= (m_maxSpeed / 2)) m_stepIntervalWhole++; // round for slightly better accuracy

            return true;
        }
    }
    return false;
}

/* ----------------------------------------------------------------------------------------------------
Makes the motor move. Call repeatedly and often for smooth motion.
Returns the kissStepper's state.
---------------------------------------------------------------------------------------------------- */

kissState_t kissStepperNoAccel::move(void)
{
    uint32_t curTime = micros();
    if (m_kissState == STATE_RUN)
    {
        // between pulses (step pin low), check timing against stepIntervalWhole
        // Adding stepIntervalWhole to lastStepTime produces more accurate timing than setting lastStepTime = curTime
        if (curTime - m_lastStepTime >= m_stepIntervalWhole)
        {

            // increment lastStepTime
            m_lastStepTime += m_stepIntervalWhole;

            /*
                Do the step pulse.

                Using a pointer to a port isn't perfect, but it's still better than digitalWrite()

                We need to wait an additional number of clock cycles to make the step pulse the needed width.
                See data sheets of Allegro A3967, A4983, A4988, TI DRV8825 for minimum pulse width.
                Allegro A3967, A4983, A4988: 1 us minimum
                TI DRV8825: 1.9 us minimum
            */
            cli();
            uint8_t oldStepOut = *m_stepOut;
            *m_stepOut |= m_stepBit;
#ifdef TEENSYDUINO
            delayMicroseconds(PULSE_WIDTH_US);
#else
            _delay_loop_1(PULSE_WIDTH_LOOP_COUNT); // busy wait
#endif
            *m_stepOut = oldStepOut;
            sei();

            // adjust position
            m_distMoved++;

            // progress through speed profile
            if (m_distMoved == m_distTotal)
                stop();
        }
    }
    else if (m_kissState == STATE_STARTING)
    {
        m_lastStepTime = curTime;
        m_kissState = STATE_RUN;
    }

    return m_kissState;
}

// ----------------------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------------------

void kissStepperNoAccel::stop(void)
{
    updatePos();
    m_distTotal = 0;
    m_kissState = STATE_STOPPED;
}

// ----------------------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------------------
// kissStepper WITH acceleration
// ----------------------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------------------

kissStepper::kissStepper(uint8_t PIN_DIR, uint8_t PIN_STEP, uint8_t PIN_ENABLE) :
    kissStepperNoAccel(PIN_DIR, PIN_STEP, PIN_ENABLE),
    m_accel(DEFAULT_ACCEL)
{}

/* ----------------------------------------------------------------------------------------------------
Does some basic checks, enforces limits, calculates the step interval, and switches to STATE_STARTING.

This method also calculates the distance for acceleration (distAccel), constant velocity (distRun),
and total distance (distTotal).

The values are the cumulative number of step pin pulses produced before it's time to change state.
---------------------------------------------------------------------------------------------------- */

bool kissStepper::prepareMove(int32_t target)
{
    // only continue if not already moving
    if (m_kissState == STATE_STOPPED)
    {
        // constrain the target between reverseLimit and forwardLimit
        target = constrain(target, m_reverseLimit, m_forwardLimit);

        // only continue if movement is required (positive distance) and possible (positive speed)
        if ((target != m_pos) && (m_maxSpeed > 0))
        {

            // enable the motor controller if necessary
            if (!m_enabled) enable();

            // set the direction
            setDir(target > m_pos);

            // set initial state
            m_kissState = STATE_STARTING;

            // calculate speed profile
            uint32_t distRemaining = (target > m_pos) ? (target - m_pos) : (m_pos - target);
            m_distAccel = m_distRun = 0;
            m_distTotal = distRemaining;

            if (m_accel != 0)
            {

                // calculate distance for accel/decel
                // distAccel is the distance of accel/decel between 0 st/s and maxSpeed
                m_distAccel = calcMaxAccelDist();

                // if distAccel >= half the distance remaining, use a triangular speed profile
                // otherwise use a trapezoidal profile
                if (m_distAccel >= (distRemaining / 2))
                {
                    m_distAccel = distRemaining / 2;
                    m_distRun = m_distAccel;
                }
                else
                {
                    m_distRun = distRemaining - m_distAccel;
                }

                // calculate constant multiplier
                m_constMult = ((float)m_accel / ONE_SECOND) / ONE_SECOND;

                // calculate step interval at min speed (initial step delay)
                // minSpeedStepInterval = ONE_SECOND / sqrt(V0^2 + 2a)
                // because initial velocity is 0:
                // minSpeedStepInterval = ONE_SECOND / sqrt(2a)
                uint32_t minSpeedStepInterval = m_stepInterval = ONE_SECOND / sqrt(2.0 * m_accel);
                m_stepIntervalWhole = minSpeedStepInterval;
                if ((m_stepIntervalWhole - minSpeedStepInterval) > 0.5) m_stepIntervalWhole++;

                // calculate step interval at max speed
                m_maxSpeedStepInterval = ONE_SECOND / m_maxSpeed;
            }
            else
            {
                // no acceleration or deceleration
                m_distRun = distRemaining;

                // if not accelerating, start motor at full speed
                // don't need to set float version of stepInterval since it isn't used during run
                m_stepIntervalWhole = ONE_SECOND / m_maxSpeed;
                if ((ONE_SECOND % m_maxSpeed) >= (m_maxSpeed / 2)) m_stepIntervalWhole++; // round for slightly better accuracy
            }

            return true;
        }
    }
    return false;
}

/* ----------------------------------------------------------------------------------------------------
Makes the motor move. Call repeatedly and often for smooth motion.
Returns the kissStepper's state.
---------------------------------------------------------------------------------------------------- */

kissState_t kissStepper::move(void)
{
    uint32_t curTime = micros();
    if (m_kissState > STATE_STARTING)
    {
        // between pulses (step pin low), check timing against stepIntervalWhole
        // Adding stepIntervalWhole to lastStepTime produces more accurate timing than setting lastStepTime = curTime
        if (curTime - m_lastStepTime >= m_stepIntervalWhole)
        {

            // increment lastStepTime
            m_lastStepTime += m_stepIntervalWhole;

            /*
                Do the step pulse.

                Using a pointer to a port isn't perfect, but it's still better than digitalWrite()

                We need to wait an additional number of clock cycles to make the step pulse the needed width.
                See data sheets of Allegro A3967, A4983, A4988, TI DRV8825 for minimum pulse width.
                Allegro A3967, A4983, A4988: 1 us minimum
                TI DRV8825: 1.9 us minimum
            */
            cli();
            uint8_t oldStepOut = *m_stepOut;
            *m_stepOut |= m_stepBit;
#ifdef TEENSYDUINO
            delayMicroseconds(PULSE_WIDTH_US);
#else
            _delay_loop_1(PULSE_WIDTH_LOOP_COUNT); // busy wait
#endif
            *m_stepOut = oldStepOut;
            sei();

            // adjust position
            m_distMoved++;

            // progress through speed profile
            if (m_kissState == STATE_RUN)
            {
                if (m_distMoved == m_distRun)
                {
                    if (m_distTotal == m_distRun)
                        stop();
                    else
                    {
                        m_kissState = STATE_DECEL;
                        m_stepIntervalWhole = m_stepInterval = decelStep(m_stepInterval, m_constMult);
                    }
                }
            }
            else if (m_kissState == STATE_ACCEL)
            {
                if (m_distMoved == m_distAccel)
                {
                    // if the run part of the profile has non-zero distance, the value of distRun will be greater than distAccel
                    if (m_distRun != m_distAccel)
                    {
                        m_kissState = STATE_RUN;
                        // set stepInterval to maxSpeedStepInterval when entering run
                        m_stepInterval = m_stepIntervalWhole = m_maxSpeedStepInterval;
                    }
                    else
                    {
                        m_kissState = STATE_DECEL;
                        m_stepIntervalWhole = m_stepInterval = decelStep(m_stepInterval, m_constMult);
                    }
                }
                else
                    m_stepIntervalWhole = m_stepInterval = accelStep(m_stepInterval, m_constMult);
            }
            else
            {
                if (m_distMoved == m_distTotal)
                    stop();
                else
                    m_stepIntervalWhole = m_stepInterval = decelStep(m_stepInterval, m_constMult);
            }
        }
    }
    else if (m_kissState == STATE_STARTING)
    {
        // start with the first part of the profile with non-zero length
        m_lastStepTime = curTime;
        if (m_distAccel != 0)
            m_kissState = STATE_ACCEL;
        else if (m_distRun != 0)
            m_kissState = STATE_RUN;
        else if (m_distTotal != 0)
            m_kissState = STATE_DECEL;
        else // this should never happen... but fail gracefully if it does
            stop();
    }

    return m_kissState;
}

// ----------------------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------------------

void kissStepper::decelerate(void)
{
    if (m_kissState > STATE_STARTING)
    {
        if (m_accel > 0)
        {
            uint32_t distRemaining = getDistRemaining();
            uint32_t maxDecelDist = calcDecelDist();
            uint32_t decelDist = (maxDecelDist > distRemaining) ? distRemaining : maxDecelDist;
            m_distAccel = 0;
            m_distRun = 0;
            m_distTotal = m_distMoved + decelDist;
            m_kissState = STATE_DECEL;
        }
        else
            stop();
    }
}

// ----------------------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------------------

void kissStepper::stop(void)
{
    updatePos();
    m_distAccel = m_distRun = m_distTotal = 0;
    m_kissState = STATE_STOPPED;
}
