/**
*
*  \author     Paul Bovbel <pbovbel@clearpathrobotics.com>
*  \copyright  Copyright (c) 2014, Clearpath Robotics, Inc.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in the
*       documentation and/or other materials provided with the distribution.
*     * Neither the name of Clearpath Robotics, Inc. nor the
*       names of its contributors may be used to endorse or promote products
*       derived from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL CLEARPATH ROBOTICS, INC. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
* Please send comments, questions, or patches to code@clearpathrobotics.com
*
*/

#include "husky_base/husky_hardware.h"
#include <boost/assign/list_of.hpp>
#include <string>
#include <algorithm>

namespace
{
  const uint8_t LEFT = 0, RIGHT = 1;
};

namespace husky_base
{

  /**
  * Initialize Husky hardware
  */
  HuskyHardware::HuskyHardware(ros::NodeHandle nh, ros::NodeHandle private_nh)
      : nh_(nh),
        private_nh_(private_nh),
        system_status_task_(husky_status_msg_),
        power_status_task_(husky_status_msg_),
        safety_status_task_(husky_status_msg_),
        software_status_task_(husky_status_msg_)
  {
    private_nh_.param<double>("wheel_diameter", wheel_diameter_, 0.3555);
    private_nh_.param<double>("max_accel", max_accel_, 5.0);
    private_nh_.param<double>("max_speed", max_speed_, 1.0);

    std::string port;
    private_nh_.param<std::string>("port", port, "/dev/prolific");

    connect(port);
    resetTravelOffset();
    initializeDiagnostics();
    initializeWheelTicks();
    registerControlInterfaces();
  }

  /**
  * Connect to Husky Hardware via Horizon serial API
  */
  void HuskyHardware::connect(std::string port)
  {
    try
    {
      clearpath::Transport::instance().configure(port.c_str(), 3 /* max retries*/);
    }
    catch (clearpath::Exception *ex)
    {
      ROS_ERROR_STREAM("Error connecting to Husky: " << ex->message);
    }

    try
    {
      clearpath::SetMaxAccel(max_accel_, max_accel_).send();
      clearpath::SetMaxSpeed(max_speed_, max_speed_).send();
    }
    catch (clearpath::Exception *ex)
    {
      ROS_ERROR_STREAM("Error setting Husky parameters: " << ex->message);
    }
  }

  /**
  * Get current encoder travel offsets from MCU and bias future encoder readings against them
  */
  void HuskyHardware::resetTravelOffset()
  {
    Msg<clearpath::DataEncoders>::Ptr enc = Msg<clearpath::DataEncoders>::getUpdate();
    if (enc)
    {
      for (int i = 0; i < 4; i++)
      {
        joints_[i].position_offset = linearToAngular(enc->getTravel(i % 2));
      }
    }
    else
    {
      ROS_ERROR("Could not get encoder data to calibrate travel offset");
    }
  }

  /**
  * Register diagnostic tasks with updater class
  */
  void HuskyHardware::initializeDiagnostics()
  {
    Msg<clearpath::DataPlatformInfo>::Ptr info = Msg<clearpath::DataPlatformInfo>::getUpdate();

    std::ostringstream hardware_id_stream;
    hardware_id_stream << "Husky " << info->getModel() << "-" << info->getSerial();

    diagnostic_updater_.setHardwareID(hardware_id_stream.str());
    diagnostic_updater_.add(system_status_task_);
    diagnostic_updater_.add(power_status_task_);
    diagnostic_updater_.add(safety_status_task_);
    diagnostic_updater_.add(software_status_task_);
    diagnostic_publisher_ = nh_.advertise<husky_msgs::HuskyStatus>("status", 10);
  }

  void HuskyHardware::initializeWheelTicks()
  {
        wheel_publisher_ = nh_.advertise<husky_msgs::HuskyWheelTick>("wheel_tick", 10);
  }

  /**
  * Register interfaces with the RobotHW interface manager, allowing ros_control operation
  */
  void HuskyHardware::registerControlInterfaces()
  {
    ros::V_string joint_names = boost::assign::list_of("front_left_wheel")
        ("front_right_wheel")("rear_left_wheel")("rear_right_wheel");
    for (unsigned int i = 0; i < joint_names.size(); i++)
    {
      hardware_interface::JointStateHandle joint_state_handle(joint_names[i],
          &joints_[i].position, &joints_[i].velocity, &joints_[i].effort);
      joint_state_interface_.registerHandle(joint_state_handle);

      hardware_interface::JointHandle joint_handle(
          joint_state_handle, &joints_[i].velocity_command);
      velocity_joint_interface_.registerHandle(joint_handle);
    }
    registerInterface(&joint_state_interface_);
    registerInterface(&velocity_joint_interface_);
  }

  /**
  * External hook to trigger diagnostic update
  */
  void HuskyHardware::updateDiagnostics()
  {
    diagnostic_updater_.force_update();
    husky_status_msg_.header.stamp = ros::Time::now();
    diagnostic_publisher_.publish(husky_status_msg_);
  }

  /*Update the wheel ticks and publish */
  void HuskyHardware::updateWheelTicksFromHardware()
  {
    Msg<clearpath::DataEncodersRaw>::Ptr enc = Msg<clearpath::DataEncodersRaw>::getUpdate();
    
    int index = 0;
    if (enc)
      {
	husky_wheel_msg_.name.clear();
	husky_wheel_msg_.tickCount.clear();

	//ROS_INFO("Found %d encoders", enc->getCount());
	husky_wheel_msg_.name.push_back("left");
        husky_wheel_msg_.tickCount.push_back(enc->getTicks(index));

	index++;
	husky_wheel_msg_.name.push_back("right");
        husky_wheel_msg_.tickCount.push_back(enc->getTicks(index));
	
	husky_wheel_msg_.header.stamp = ros::Time::now();
	wheel_publisher_.publish(husky_wheel_msg_);
      }
  }

  /**
  * Pull latest speed and travel measurements from MCU, and store in joint structure for ros_control
  */
  void HuskyHardware::updateJointsFromHardware()
  {

    
    Msg<clearpath::DataEncoders>::Ptr enc = Msg<clearpath::DataEncoders>::getUpdate();
    if (enc)
    {
      for (int i = 0; i < 4; i++)
      {
        double new_position = linearToAngular(enc->getTravel(i % 2)) - joints_[i].position_offset;
        double delta = new_position - joints_[i].position;

        // detect firmware-side encoder rollover
        if (std::abs(delta) < 1.0)
        {
          joints_[i].position = new_position;
        }
        else
        {
          // swallow the measurement and update the offset if rollover has occured
          joints_[i].position_offset = delta;
        }
      }
    }

    Msg<clearpath::DataDifferentialSpeed>::Ptr speed = Msg<clearpath::DataDifferentialSpeed>::getUpdate();
    if (speed)
    {
      for (int i = 0; i < 4; i++)
      {
        if (i % 2 == LEFT)
        {
          joints_[i].velocity = linearToAngular(speed->getLeftSpeed());
        }
        else
        { // assume RIGHT
          joints_[i].velocity = linearToAngular(speed->getRightSpeed());
        }
      }
    }
  }

  /**
  * Get latest velocity commands from ros_control via joint structure, and send to MCU
  */
  void HuskyHardware::writeCommandsToHardware()
  {
    double diff_speed_left = angularToLinear(joints_[LEFT].velocity_command);
    double diff_speed_right = angularToLinear(joints_[RIGHT].velocity_command);

    limitDifferentialSpeed(diff_speed_left, diff_speed_right);

    try
    {
      clearpath::SetDifferentialSpeed(
          diff_speed_left, diff_speed_right,
          max_accel_, max_accel_).send();
    }
    catch (clearpath::Exception *ex)
    {
      ROS_ERROR_STREAM("Error sending speed command: " << ex->message);
    }
  }

  /**
  * Update diagnostics with control loop timing information
  */
  void HuskyHardware::reportLoopFrequency(const ros::TimerEvent &control_event)
  {
    software_status_task_.update(control_event);
  }

  /**
  * Scale left and right speed outputs to maintain ros_control's desired trajectory without saturating the outputs
  */
  void HuskyHardware::limitDifferentialSpeed(double &diff_speed_left, double &diff_speed_right)
  {
    double large_speed = std::max(std::abs(diff_speed_left), std::abs(diff_speed_right));

    if (large_speed > max_speed_)
    {
      diff_speed_left *= max_speed_ / large_speed;
      diff_speed_right *= max_speed_ / large_speed;
    }
  }

  /**
  * Husky reports travel in metres, need radians for ros_control RobotHW
  */
  double HuskyHardware::linearToAngular(const double &travel) const
  {
    return travel / wheel_diameter_ * 2;
  }

  /**
  * RobotHW provides velocity command in rad/s, Husky needs m/s,
  */
  double HuskyHardware::angularToLinear(const double &angle) const
  {
    return angle * wheel_diameter_ / 2;
  }


}  // namespace husky_base
