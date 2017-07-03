#include "config.h"
#include "protocol.h"
#include "controller.h"
#include "protocol.h"
#include "clock_50Hz.h"
#include "sbus.h"

extern sbusData_t sbusdata;

void calculateQx(void);
void printControlLoop(int16_t esc_speed, double speed, double brakedistance, CONTROLLER_MONITOR_t monitor, Endpoints endpoint);
void stickCycle(double pos, double brakedistance);

CONTROLLER_MONITOR_t monitor = FREE;

double speed_old = 0;
double yalt = 0.0f, ealt = 0.0f, ealt2 = 0.0f;

CONTROLLER_STATUS_t controller_status = CONTROLLER_INIT;
SAFE_MODE_t safemode = INVALID_RC;

int32_t pos_current_old = 0L;
double pos_target = 0.0f, pos_target_old = 0.0f;

int16_t stick_requested_value = 0;

uint8_t endpointclicks = 0;
uint16_t lastendpointswitch = 0;

#define Ta  0.02

double Q0, Q1, Q2;

int8_t moving_direction = 0;


void calculateQx()
{
    Q0 = activesettings.P+activesettings.I*Ta+activesettings.D/Ta;
    Q1 = -activesettings.P-2*activesettings.D/Ta;
    Q2 = activesettings.D/Ta;
}

void setPIDValues(double kp, double ki, double kd)
{
    activesettings.P = kp;
    activesettings.I = ki;
    activesettings.D = kd;
    calculateQx();
}

void setPValue(double v)
{
    activesettings.P = v;
    calculateQx();
}
void setIValue(double v)
{
    activesettings.I = v;
    calculateQx();
}
void setDValue(double v)
{
    activesettings.D = v;
    calculateQx();
}


int32_t getTargetPos(void)
{
    return pos_target;
}


int32_t getSpeed(void)
{
    return speed_old;
}

int32_t getPos(void)
{
    return ENCODER_VALUE;
}

void initController(void)
{
    calculateQx();
}

SAFE_MODE_t getSafeMode(void)
{
    return safemode;
}

double abs_d(double v)
{
    if (v<0.0)
    {
        return -v;
    }
    else
    {
        return v;
    }
}

uint16_t getProgrammingSwitch()
{
    return getDuty(activesettings.rc_channel_programming);
}

uint16_t getEndPointSwitch()
{
    return getDuty(activesettings.rc_channel_endpoint);
}


/*
 * getStickPositionRaw() returns the position of the stick considering the neutral range.
 * Everything within the neutral range means a stick position of zero, the first value outside the neutral range would be 1.
 * Examples:
 *   sbus reading: 885; neutral: 870..890 --> return 0;
 *   sbus reading: 860; neutral: 870..890 --> return 860-870=-10;
 *
 * Therefore the output is a linear range from min to max with the neutral range eliminated from the value.
 * Also, when the reading is too old, meaning older than 3 seconds, the value is reset to zero as emergency precaution.
 */
int16_t getStickPositionRaw()
{
    int16_t value = getDuty(activesettings.rc_channel_speed);
    if (value == 0)
    {
        // No valid value for that, use Neutral
        return 0;
    }
    else
    {
        /*
         * At the beginning the safemode == INVALID_RC, meaning no valid signal was received ever.
         * If a valid rc signal is received, then the safemode is changed according to the programming switch.
         * There is one more case, we got a valid speed signal but the stick is not in neutral. This is considered
         * an invalid signal as well. At startup the speed signal has to be in idle.
         */
        if (safemode == INVALID_RC && (value > activesettings.stick_neutral_pos + activesettings.stick_neutral_range ||
                                       value < activesettings.stick_neutral_pos - activesettings.stick_neutral_range))
        {
            return 0;
        }

        /*
         * Okay, so either we are in neutral or in normal operation, hence evaluate the programming switch.
         * A HIGH value is considered operational, a LOW value, a value of neutral or if the channel is not set at all (value == 0),
         * then we are in the programming mode with reduced speeds.
         * Note: All three conditions need to be met to exit the programming mode, hence the logic is placed here.
         */
        if (getProgrammingSwitch() > 1200)
        {
            safemode = OPERATIONAL;
        }
        else
        {
            if (safemode != PROGRAMMING)
            {
                // We just entered the programing mode, hence wait for the first end point
                endpointclicks = 0;
            }
            safemode = PROGRAMMING;
        }
    }

    value -= activesettings.stick_neutral_pos;

    if (value > activesettings.stick_neutral_range )
    {
        // above idle is just fine
        return value - activesettings.stick_neutral_range;
    }
    else if (value < -activesettings.stick_neutral_range)
    {
        // below negative idle is fine
        return value + activesettings.stick_neutral_range;
    }
    else
    {
        // within neutral range means zero
        return 0;
    }
}


int16_t getStickPosition()
{
    return stick_requested_value;
}

void stickCycle(double pos, double brakedistance)
{
    int16_t value = getStickPositionRaw();
    // In passthrough mode the returned value is the raw value

    if (activesettings.mode != MODE_PASSTHROUGH)
    {
        /*
         * In all other modes the accel and speed limiters are turned on
         */
        int16_t maxaccel = activesettings.stick_max_accel;
        int16_t maxspeed = activesettings.stick_max_speed;

        if (safemode != OPERATIONAL)
        {
            maxaccel = activesettings.stick_max_accel_safemode;
            maxspeed = activesettings.stick_max_speed_safemode;
        }
        else if (activesettings.mode != MODE_PASSTHROUGH && activesettings.mode != MODE_LIMITER)
        {
            /*
             * If we would overshoot the end points, then ignore the requested value and assume the user wants to stop asap, hence value = 0.
             * Actually, below's maxaccel limiter will make that a smooth speed reduction.
             */
            if (pos + brakedistance >= activesettings.pos_end && value >= 0)
            {
                value = 0;
                monitor = ENDPOINTBRAKE;
            }
            else if (pos - brakedistance <= activesettings.pos_end && value <= 0)
            {
                value = 0;
                monitor = ENDPOINTBRAKE;
            }
        }

        int16_t diff = value - stick_requested_value;

        /*
         * No matter what the stick value says, the speed is limited to the max range.
         * It is important this comes before the the acceleration limiter as it might be the case
         * the user switched to programming mode running at full speed. This should include the slow
         * decel as well.
         */

        if (diff > maxaccel)
        {
            // Example: last time: 150; now stick: 200 --> diff: 50; max change is 10 per cycle --> new value: 150+10=160 as the limiter kicks in
            value = stick_requested_value + maxaccel;
        }
        else if (diff < -maxaccel)
        {
            // Example: last time: 150; now stick: 100 --> diff: -50; max change is 10 per cycle --> new value: 150-10=140 as the limiter kicks in
            value = stick_requested_value - maxaccel;
        }

        /*
         * It is important to calculate the new value based on the acceleration first, then we have the new target speed,
         * now it is limited to an absolute value.
         */
        if (value > maxspeed)
        {
            value = maxspeed;
        }
        else if (value < -maxspeed)
        {
            value = -maxspeed;
        }
    }
    stick_requested_value = value; // store the current effective stick position as value for the next cycle

    // Switch was raised from LOW to HIGH
    uint16_t currentendpointswitch = getEndPointSwitch();
    if (currentendpointswitch > 1200 && safemode == PROGRAMMING && lastendpointswitch <= 1200 && lastendpointswitch != 0)
    {
        int32_t pos = ENCODER_VALUE;
        if (endpointclicks == 0)
        {
            activesettings.pos_start = pos;
            endpointclicks = 1;
        }
        else
        {
            if (activesettings.pos_start < pos)
            {
                activesettings.pos_end = pos;
            }
            else
            {
                activesettings.pos_end = activesettings.pos_start;
                activesettings.pos_start = pos;
            }
        }
    }
    lastendpointswitch = currentendpointswitch; // Needed to identify a raising flank on the tip switch
}

void resetThrottle()
{
    ealt2 = 0.0f;
    ealt = 0.0f;
    yalt = 0.0f;

}

// ******** Main Loop *********
void controllercycle()
{
    monitor = FREE; // might be overwritten by stickCycle() so has to come first
    /*
       We need to work with the average speed, and that is the motor speed and the new speed at max deceleration

       speed_old	accel	new_speed	distance_to_stop		old_pos_target	    new_pos_target	old_pos_target+distance_to_stop
             100	1		99	        4950	                  0	                 99	     	    4950
              99	1		98	        4851	                 99	                197	     	    4950
              98	1		97	        4753	                197	                294	      	    4950
              97	1		96	        4656	                294	                390	      	    4950
              96	1		95	        4560	                390	                485	      	    4950
              95	1		94	        4465	                485	                579	     	    4950
              94	1		93	        4371	                579	                672	     	    4950
              93	1		92	        4278	                672	                764				4950
              92	1		91	        4186	                764	                855				4950
              91	1		90	        4095	                855	                945				4950
              ..
              ..
              ..
     */
    int32_t pos_current = ENCODER_VALUE;
    double accel = ((double)activesettings.stick_max_accel) * activesettings.stick_speed_factor;
    double distance_to_stop = (abs_d(speed_old) * (abs_d(speed_old)-accel) / 2.0f / accel);
    double pos = (double) pos_current;

    if (activesettings.mode == MODE_ABSOLUTE_POSITION)
    {
        // in case of mode-absolute the actual position does not matter, it is the target position that counts
        stickCycle(pos_target_old, distance_to_stop); // go through the stick position calculation with its limiters, max accel etc
    }
    else
    {
        stickCycle(pos, distance_to_stop); // go through the stick position calculation with its limiters, max accel etc
    }

    /*
     * The stick position and the speed_new variables both define essentially the ESC target. So if the
     * stick is currently set to 100% forward, that means 100% thrust or 100% speed. The main difference is
     * at breaking and here the end point break in particular: In this case the thrust has to be set to zero
     * in order to break as hard as possible whereas the speed output can be gradually reduced to zero.
     */
    int16_t stickposition = getStickPosition();
    int16_t esc_output = stickposition;

    int16_t speed_current = pos_current_old - pos_current;

    if (activesettings.mode != MODE_PASSTHROUGH && activesettings.mode != MODE_LIMITER)
    {
        /*
         * In passthrough mode or limiter mode, the cablecam can move freely, no end points are considered. In all other modes...
         */
        if (activesettings.mode == MODE_ABSOLUTE_POSITION)
        {
            /*
             * In absolute mode everything revolves around the target pos. The speed is the change in target pos etc.
             * The actual position does not matter, as the stick controls where the cablecam should be. It is an idealistic world,
             * where the target position can be calculated mathematically correct.
             * The PID loop is responsible to keep the cablecam near the target position.
             */


            /*
             * The new target is the old target increased by the stick signal.
             */
            pos_target += ((double)stickposition) * activesettings.stick_speed_factor;

            // In OPERATIONAL mode the position including the break distance has to be within the end points, in programming mode you can go past that
            if (safemode == OPERATIONAL)
            {
                // Of course the target can never exceed the end points, unless in programming mode
                if (pos_target > activesettings.pos_end)
                {
                    pos_target = activesettings.pos_end;
                }
                else if (pos_target < activesettings.pos_start)
                {
                    pos_target = activesettings.pos_start;
                }
            }
            speed_old = pos_target - pos_target_old;
            pos_target_old = pos_target;


            /*
             * The PID loop calculates the error between target pos and actual pos and does change the throttle/speed signal in order to keep the error as small as possible.
             */
            double y = 0.0f;


            double e = pos_target - pos;     // This is the amount of steps the target pos does not match the reality

            if (e >= -activesettings.max_position_error && e <= activesettings.max_position_error)
            {
                // The cablecam cannot catchup with the target position --> EMERGENCY BRAKE
                resetThrottle();
                esc_output = 0;
                monitor = EMERGENCYBRAKE;
            }
            else
            {

                y = yalt + Q0*e + Q1*ealt + Q2*ealt2;         // PID loop calculation

                esc_output = (int16_t) y;

                ealt2 = ealt;
                ealt = e;
                yalt = y;
            }
        }
        else
        {
            // activesettings.mode != MODE_ABSOLUTE_POSITION

            // esc_out = stickposition for speed based ESCs or in case of a classic ESC esc_out = 0 if monitor == ENDPOINTBREAK
            speed_old = (double) speed_current;
        }
    }


    pos_current_old = pos_current; // required for the actual speed calculation

    if (is1Hz())
    {
        printControlLoop(stickposition, speed_old, distance_to_stop, monitor, EndPoint_USB);
    }
}

void printControlLoop(int16_t input, double speed, double brakedistance, CONTROLLER_MONITOR_t monitor, Endpoints endpoint)
{
    PrintSerial_string("Time: ", endpoint);
    PrintSerial_long(sbusdata.sbusLastValidFrame, endpoint);
    PrintSerial_string("  Raw: ", endpoint);
    PrintSerial_int(getDuty(activesettings.rc_channel_speed), endpoint);
    if (safemode == INVALID_RC)
    {
        PrintSerial_string("  INVALID_RC", endpoint);
    }
    else if (safemode == PROGRAMMING)
    {
        PrintSerial_string("  PROGRAMMING", endpoint);
    }
    else if (safemode == OPERATIONAL)
    {
        PrintSerial_string("  OPERATIONAL", endpoint);
    }
    else
    {
        PrintSerial_string("  ???", endpoint);
    }
    PrintSerial_string("  Input: ", endpoint);
    PrintSerial_int(input, endpoint);
    PrintSerial_string("  Speed: ", endpoint);
    PrintSerial_double(speed, endpoint);
    PrintSerial_string("  Brakedistance: ", endpoint);
    PrintSerial_double(brakedistance, endpoint);

    if (monitor == FREE)
    {
        PrintSerial_string("  FREE", endpoint);
    }
    else if (monitor == ENDPOINTBRAKE)
    {
        PrintSerial_string("  ENDPOINTBRAKE", endpoint);
    }
    else if (monitor == EMERGENCYBRAKE)
    {
        PrintSerial_string("  EMERGENCYBRAKE", endpoint);
    }
    else
    {
        PrintSerial_string("  ???", endpoint);
    }


    PrintSerial_string("  Pos: ", endpoint);
    PrintlnSerial_long(ENCODER_VALUE, endpoint);
}
