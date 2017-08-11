/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <AP_HAL/AP_HAL.h>

#include "AP_MotorsHeli_RSC.h"

extern const AP_HAL::HAL& hal;

// init_servo - servo initialization on start-up
void AP_MotorsHeli_RSC::init_servo()
{
    // setup RSC on specified channel by default
    SRV_Channels::set_aux_channel_default(_aux_fn, _default_channel);
}

// set_power_output_range
void AP_MotorsHeli_RSC::set_power_output_range(float power_low, float power_high, float power_negc, uint16_t slewrate)
{
    _power_output_low = power_low;
    _power_output_high = power_high;
    _power_output_negc = power_negc;
    _power_slewrate = slewrate;
}

// output - update value to send to ESC/Servo
void AP_MotorsHeli_RSC::output(RotorControlState state)
{
    float dt;
    uint64_t now = AP_HAL::micros64();
    float last_control_output = _control_output;
    
    if (_last_update_us == 0) {
        _last_update_us = now;
        dt = 0.001f;
    } else {
        dt = 1.0e-6f * (now - _last_update_us);
        _last_update_us = now;
    }
    
    switch (state){
        case ROTOR_CONTROL_STOP:
            // set rotor ramp to decrease speed to zero, this happens instantly inside update_rotor_ramp()
            update_rotor_ramp(0.0f, dt);

            // control output forced to zero
            _control_output = 0.0f;
            break;

        case ROTOR_CONTROL_IDLE:
            // set rotor ramp to decrease speed to zero
            update_rotor_ramp(0.0f, dt);

            // set rotor control speed to idle speed parameter, this happens instantly and ignore ramping
            _control_output = _idle_output;
            break;

        case ROTOR_CONTROL_ACTIVE:
            // set main rotor ramp to increase to full speed
            update_rotor_ramp(1.0f, dt);

            if ((_control_mode == ROTOR_CONTROL_MODE_SPEED_PASSTHROUGH) || (_control_mode == ROTOR_CONTROL_MODE_SPEED_SETPOINT) ) {
                // set control rotor speed to ramp slewed value between idle and desired speed
                _control_output = _idle_output + (_rotor_ramp_output * (_desired_speed - _idle_output));
            } else if (_control_mode == ROTOR_CONTROL_MODE_OPEN_LOOP_POWER_OUTPUT) {
                // throttle output depending on estimated power demand. Output is ramped up from idle speed during rotor runup. A negative load
                // is for the left side of the V-curve (-ve collective) A positive load is for the right side (+ve collective)
               _control_output = calc_open_loop_power_control_output();
            } else if (_control_mode == ROTOR_CONTROL_MODE_GOVERNOR) {
                if (!_gov_enabled) {
                    // Governor is not enabled - we should just passthrough the desired value
                    _control_output = _idle_output + (_rotor_ramp_output * (_desired_speed - _idle_output));
                    // hal.console->printf("Governor disabled: desired speed = %f, control output = %f\n", _desired_speed, _control_output);
                } else if (fabs(_governor_rpm_setpoint - _rpm_feedback) < _governor_rpm_deadband) {
                    // We are within the RPM deadband - no control action to be taken
                    _control_output = last_control_output;
                } else {
                    // throttle output based on closed-loop control using PID.
                    _control_output = calc_closed_loop_power_control_output();
                }

            }
            break;
    }

    // update rotor speed run-up estimate
    update_rotor_runup(dt);

    if (_power_slewrate > 0) {
        // implement slew rate for throttle
        float max_delta = dt * _power_slewrate * 0.01f;
        _control_output = constrain_float(_control_output, last_control_output-max_delta, last_control_output+max_delta);
    }
    
    // output to rsc servo
    write_rsc(_control_output);
}

// calc_open_loop_power_control_output - calculates control output for use in open loop mode, or as feedforward for closed loop mode
float AP_MotorsHeli_RSC::calc_open_loop_power_control_output()
{

    float open_loop_power_control_output = 0.0;

    if (_load_feedforward >= 0) {
        float range = _power_output_high - _power_output_low;
        open_loop_power_control_output = _idle_output + (_rotor_ramp_output * ((_power_output_low - _idle_output) + (range * _load_feedforward)));
    } else {
        float range = _power_output_negc - _power_output_low;
        open_loop_power_control_output = _idle_output + (_rotor_ramp_output * ((_power_output_low - _idle_output) - (range * _load_feedforward)));
    }
    // throttle output depending on estimated power demand. Output is ramped up from idle speed during rotor runup.
    return open_loop_power_control_output;
}

// calc_closed_loop_power_control_output - calculates control output for use in closed loop mode
float AP_MotorsHeli_RSC::calc_closed_loop_power_control_output()
{
    // In case we have no closed-loop control - just use open-loop
    if (_pid_rotor_gov == NULL) return calc_open_loop_power_control_output();

    float pid_output;             // pi closed-loop output contribution
    float target_rpm;            // target rpm is ramped

    target_rpm = _rotor_ramp_output * _governor_rpm_setpoint;
    float pid_input = (target_rpm - _rpm_feedback) / 100.0f;

    if (_gov_enabled){
        _pid_rotor_gov->set_input_filter_all(pid_input/*target_rpm - _rpm_feedback*/);
        pid_output = _pid_rotor_gov->get_pid();
    } else {
        _pid_rotor_gov->set_input_filter_all(0);
        pid_output = 0;
    }

    pid_output = constrain_float(pid_output, 0, 1);

    // float open_loop_output = calc_open_loop_power_control_output();


    /*hal.console->printf("Setpoint = %d, Target RPM: %f, ACTUAL: %f, Ramp = %f, Open Loop: %f, PID output: %f (%f %f %f)\n", _governor_rpm_setpoint, 
    target_rpm, 
    _rpm_feedback, 
    _rotor_ramp_output, 
    open_loop_output, 
    pid_output,
    _pid_rotor_gov->get_p(),
    _pid_rotor_gov->get_i(),
    _pid_rotor_gov->get_d()
    );
    */
    
    // total control output is sum of basic open loop control output plus PI contribution
    return /*open_loop_output +*/ pid_output;
}

// set_gov_enable
void AP_MotorsHeli_RSC::set_gov_enable(bool enabled, int16_t rpm, int16_t deadband, float rpm_feedback)
{
    _gov_enabled = enabled;
    _governor_rpm_setpoint = rpm;
    _governor_rpm_deadband = deadband;
    _rpm_feedback = rpm_feedback;
}



// update_rotor_ramp - slews rotor output scalar between 0 and 1, outputs float scalar to _rotor_ramp_output
void AP_MotorsHeli_RSC::update_rotor_ramp(float rotor_ramp_input, float dt)
{
    // sanity check ramp time
    if (_ramp_time <= 0) {
        _ramp_time = 1;
    }

    // ramp output upwards towards target
    if (_rotor_ramp_output < rotor_ramp_input) {
        // allow control output to jump to estimated speed
        if (_rotor_ramp_output < _rotor_runup_output) {
            _rotor_ramp_output = _rotor_runup_output;
        }
        // ramp up slowly to target
        _rotor_ramp_output += (dt / _ramp_time);
        if (_rotor_ramp_output > rotor_ramp_input) {
            _rotor_ramp_output = rotor_ramp_input;
        }
    }else{
        // ramping down happens instantly
        _rotor_ramp_output = rotor_ramp_input;
    }
}

// update_rotor_runup - function to slew rotor runup scalar, outputs float scalar to _rotor_runup_ouptut
void AP_MotorsHeli_RSC::update_rotor_runup(float dt)
{
    // sanity check runup time
    if (_runup_time < _ramp_time) {
        _runup_time = _ramp_time;
    }
    if (_runup_time <= 0 ) {
        _runup_time = 1;
    }

    // ramp speed estimate towards control out
    float runup_increment = dt / _runup_time;
    if (_rotor_runup_output < _rotor_ramp_output) {
        _rotor_runup_output += runup_increment;
        if (_rotor_runup_output > _rotor_ramp_output) {
            _rotor_runup_output = _rotor_ramp_output;
        }
    }else{
        _rotor_runup_output -= runup_increment;
        if (_rotor_runup_output < _rotor_ramp_output) {
            _rotor_runup_output = _rotor_ramp_output;
        }
    }

    // update run-up complete flag

    // if control mode is disabled, then run-up complete always returns true
    if ( _control_mode == ROTOR_CONTROL_MODE_DISABLED ){
        _runup_complete = true;
        return;
    }

    // if rotor ramp and runup are both at full speed, then run-up has been completed
    if (!_runup_complete && (_rotor_ramp_output >= 1.0f) && (_rotor_runup_output >= 1.0f)) {
        _runup_complete = true;
    }
    // if rotor speed is less than critical speed, then run-up is not complete
    // this will prevent the case where the target rotor speed is less than critical speed
    if (_runup_complete && (get_rotor_speed() <= _critical_speed)) {
        _runup_complete = false;
    }
}

// get_rotor_speed - gets rotor speed either as an estimate, or (ToDO) a measured value
float AP_MotorsHeli_RSC::get_rotor_speed() const
{
    // if no actual measured rotor speed is available, estimate speed based on rotor runup scalar.
    return _rotor_runup_output;
}

// write_rsc - outputs pwm onto output rsc channel
// servo_out parameter is of the range 0 ~ 1
void AP_MotorsHeli_RSC::write_rsc(float servo_out)
{
    if (_control_mode == ROTOR_CONTROL_MODE_DISABLED){
        // do not do servo output to avoid conflicting with other output on the channel
        // ToDo: We should probably use RC_Channel_Aux to avoid this problem
        return;
    } else {
        // calculate PWM value based on H_RSC_PWM_MIN, H_RSC_PWM_MAX and H_RSC_PWM_REV
        uint16_t pwm = servo_out * (_pwm_max - _pwm_min);
        if (_pwm_rev >= 0) {
            pwm = _pwm_min + pwm;
        } else {
            pwm = _pwm_max - pwm;
        }
        SRV_Channels::set_output_pwm(_aux_fn, pwm);
    }
}
