// Copyright (c) 2021 Franka Emika GmbH
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <cmath>
#include <exception>

#include <franka/exception.h>
#include <hardware_interface/handle.hpp>
#include <hardware_interface/hardware_info.hpp>
#include <hardware_interface/system_interface.hpp>
#include <hardware_interface/types/hardware_interface_return_values.hpp>
#include <hardware_interface/types/hardware_interface_type_values.hpp>
#include <rclcpp/macros.hpp>
#include <rclcpp/rclcpp.hpp>

#include "franka_hardware/franka_hardware_interface.hpp"

namespace franka_hardware {

using StateInterface = hardware_interface::StateInterface;
using CommandInterface = hardware_interface::CommandInterface;

FrankaHardwareInterface::FrankaHardwareInterface(std::shared_ptr<Robot> robot)
    : robot_{std::move(robot)} {}

std::vector<StateInterface> FrankaHardwareInterface::export_state_interfaces() {
  std::vector<StateInterface> state_interfaces;
  for (auto i = 0U; i < info_.joints.size(); i++) {
    state_interfaces.emplace_back(StateInterface(
        info_.joints[i].name, hardware_interface::HW_IF_POSITION, &hw_positions_.at(i)));
    state_interfaces.emplace_back(StateInterface(
        info_.joints[i].name, hardware_interface::HW_IF_VELOCITY, &hw_velocities_.at(i)));
    state_interfaces.emplace_back(
        StateInterface(info_.joints[i].name, hardware_interface::HW_IF_EFFORT, &hw_efforts_.at(i)));
  }

  state_interfaces.emplace_back(StateInterface(
      k_robot_name, k_robot_state_interface_name,
      reinterpret_cast<double*>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
          &hw_franka_robot_state_addr_)));
  state_interfaces.emplace_back(StateInterface(
      k_robot_name, k_robot_model_interface_name,
      reinterpret_cast<double*>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
          &hw_franka_model_ptr_)));
  return state_interfaces;
}

std::vector<CommandInterface> FrankaHardwareInterface::export_command_interfaces() {
  std::vector<CommandInterface> command_interfaces;
  command_interfaces.reserve(info_.joints.size());
  for (auto i = 0U; i < info_.joints.size(); i++) {
    command_interfaces.emplace_back(CommandInterface(
        info_.joints[i].name, hardware_interface::HW_IF_EFFORT, &hw_commands_.at(i)));
    command_interfaces.emplace_back(CommandInterface(
        info_.joints[i].name, hardware_interface::HW_IF_VELOCITY, &hw_commands_.at(i)));
  }
  return command_interfaces;
}

CallbackReturn FrankaHardwareInterface::on_activate(
    const rclcpp_lifecycle::State& /*previous_state*/) {
  hw_commands_.fill(0);
  read(rclcpp::Time(0),
       rclcpp::Duration(0, 0));  // makes sure that the robot state is properly initialized.
  RCLCPP_INFO(getLogger(), "Started");
  return CallbackReturn::SUCCESS;
}

CallbackReturn FrankaHardwareInterface::on_deactivate(
    const rclcpp_lifecycle::State& /*previous_state*/) {
  RCLCPP_INFO(getLogger(), "trying to Stop...");
  robot_->stopRobot();
  RCLCPP_INFO(getLogger(), "Stopped");
  return CallbackReturn::SUCCESS;
}

hardware_interface::return_type FrankaHardwareInterface::read(const rclcpp::Time& /*time*/,
                                                              const rclcpp::Duration& /*period*/) {
  if (hw_franka_model_ptr_ == nullptr) {
    hw_franka_model_ptr_ = robot_->getModel();
  }

  hw_franka_robot_state_ = robot_->readOnce();
  hw_positions_ = hw_franka_robot_state_.q;
  hw_velocities_ = hw_franka_robot_state_.dq;
  hw_efforts_ = hw_franka_robot_state_.tau_J;

  return hardware_interface::return_type::OK;
}

hardware_interface::return_type FrankaHardwareInterface::write(const rclcpp::Time& /*time*/,
                                                               const rclcpp::Duration& /*period*/) {
  if (std::any_of(hw_commands_.begin(), hw_commands_.end(),
                  [](double hw_command) { return !std::isfinite(hw_command); })) {
    return hardware_interface::return_type::ERROR;
  }
  if (velocity_joint_interface_running_ || effort_interface_running_) {
    robot_->writeOnce(hw_commands_);
  }

  return hardware_interface::return_type::OK;
}

CallbackReturn FrankaHardwareInterface::on_init(const hardware_interface::HardwareInfo& info) {
  if (hardware_interface::SystemInterface::on_init(info) != CallbackReturn::SUCCESS) {
    return CallbackReturn::ERROR;
  }
  if (info_.joints.size() != kNumberOfJoints) {
    RCLCPP_FATAL(getLogger(), "Got %ld joints. Expected %ld.", info_.joints.size(),
                 kNumberOfJoints);
    return CallbackReturn::ERROR;
  }

  for (const auto& joint : info_.joints) {
    if (joint.command_interfaces.size() != 2) {
      RCLCPP_FATAL(getLogger(), "Joint '%s' has %zu command interfaces found. 2 expected.",
                   joint.name.c_str(), joint.command_interfaces.size());
      return CallbackReturn::ERROR;
    }
    if (joint.command_interfaces[0].name != hardware_interface::HW_IF_EFFORT &&
        joint.command_interfaces[0].name != hardware_interface::HW_IF_VELOCITY) {
      RCLCPP_FATAL(getLogger(),
                   "Joint '%s' has unexpected command interface '%s'. Expected '%s' and '%s' ",
                   joint.name.c_str(), joint.command_interfaces[0].name.c_str(),
                   hardware_interface::HW_IF_EFFORT, hardware_interface::HW_IF_VELOCITY);
      return CallbackReturn::ERROR;
    }
    if (joint.state_interfaces.size() != 3) {
      RCLCPP_FATAL(getLogger(), "Joint '%s' has %zu state interfaces found. 3 expected.",
                   joint.name.c_str(), joint.state_interfaces.size());
      return CallbackReturn::ERROR;
    }
    if (joint.state_interfaces[0].name != hardware_interface::HW_IF_POSITION) {
      RCLCPP_FATAL(getLogger(), "Joint '%s' has unexpected state interface '%s'. Expected '%s'",
                   joint.name.c_str(), joint.state_interfaces[0].name.c_str(),
                   hardware_interface::HW_IF_POSITION);
    }
    if (joint.state_interfaces[1].name != hardware_interface::HW_IF_VELOCITY) {
      RCLCPP_FATAL(getLogger(), "Joint '%s' has unexpected state interface '%s'. Expected '%s'",
                   joint.name.c_str(), joint.state_interfaces[1].name.c_str(),
                   hardware_interface::HW_IF_VELOCITY);
    }
    if (joint.state_interfaces[2].name != hardware_interface::HW_IF_EFFORT) {
      RCLCPP_FATAL(getLogger(), "Joint '%s' has unexpected state interface '%s'. Expected '%s'",
                   joint.name.c_str(), joint.state_interfaces[2].name.c_str(),
                   hardware_interface::HW_IF_EFFORT);
    }
  }
  if (!robot_) {
    std::string robot_ip;
    try {
      robot_ip = info_.hardware_parameters.at("robot_ip");
    } catch (const std::out_of_range& ex) {
      RCLCPP_FATAL(getLogger(), "Parameter 'robot_ip' is not set");
      return CallbackReturn::ERROR;
    }
    try {
      RCLCPP_INFO(getLogger(), "Connecting to robot at \"%s\" ...", robot_ip.c_str());
      robot_ = std::make_shared<Robot>(robot_ip, getLogger());
    } catch (const franka::Exception& e) {
      RCLCPP_FATAL(getLogger(), "Could not connect to robot");
      RCLCPP_FATAL(getLogger(), "%s", e.what());
      return CallbackReturn::ERROR;
    }
    RCLCPP_INFO(getLogger(), "Successfully connected to robot");
  }

  node_ = std::make_shared<FrankaParamServiceServer>(rclcpp::NodeOptions(), robot_);
  executor_ = std::make_shared<FrankaExecutor>();
  executor_->add_node(node_);
  return CallbackReturn::SUCCESS;
}

rclcpp::Logger FrankaHardwareInterface::getLogger() {
  return rclcpp::get_logger("FrankaHardwareInterface");
}

hardware_interface::return_type FrankaHardwareInterface::perform_command_mode_switch(
    const std::vector<std::string>& /*start_interfaces*/,
    const std::vector<std::string>& /*stop_interfaces*/) {
  if (!effort_interface_running_ && effort_interface_claimed_) {
    hw_commands_.fill(0);
    robot_->stopRobot();
    robot_->initializeTorqueInterface();
    effort_interface_running_ = true;
  } else if (effort_interface_running_ && !effort_interface_claimed_) {
    robot_->stopRobot();
    effort_interface_running_ = false;
  }

  if (!velocity_joint_interface_running_ && velocity_joint_interface_claimed_) {
    hw_commands_.fill(0);
    robot_->stopRobot();
    robot_->initializeJointVelocityInterface();
    velocity_joint_interface_running_ = true;
  } else if (velocity_joint_interface_running_ && !velocity_joint_interface_claimed_) {
    robot_->stopRobot();
    velocity_joint_interface_running_ = false;
  }
  return hardware_interface::return_type::OK;
}

hardware_interface::return_type FrankaHardwareInterface::prepare_command_mode_switch(
    const std::vector<std::string>& start_interfaces,
    const std::vector<std::string>& stop_interfaces) {
  auto is_effort_interface = [](const std::string& interface) {
    return interface.find(hardware_interface::HW_IF_EFFORT) != std::string::npos;
  };

  auto is_velocity_interface = [](const std::string& interface) {
    return interface.find(hardware_interface::HW_IF_VELOCITY) != std::string::npos;
  };

  auto generate_error_message = [&](const std::string& start_stop_command,
                                    const std::string& interface_name, int64_t num_interface) {
    RCLCPP_FATAL(this->getLogger(), "Expected %ld %s interfaces to %s, but got %ld instead.",
                 kNumberOfJoints, interface_name.c_str(), start_stop_command.c_str(),
                 num_interface);
    std::ostringstream error_message_stream;
    error_message_stream << "Invalid number of " << interface_name << " interfaces to "
                         << start_stop_command << ". Expected " << std::to_string(kNumberOfJoints);
    std::string error_message = error_message_stream.str();

    throw std::invalid_argument(error_message);
  };

  auto start_stop_interface =
      [&](const std::function<bool(const std::string& interface)>& find_interface_function,
          const std::string& interface_name, bool& claim_flag) {
        int64_t num_stop_interface =
            std::count_if(stop_interfaces.begin(), stop_interfaces.end(), find_interface_function);
        int64_t num_start_interface = std::count_if(
            start_interfaces.begin(), start_interfaces.end(), find_interface_function);

        if (num_stop_interface == kNumberOfJoints) {
          claim_flag = false;
        } else if (num_stop_interface != 0) {
          generate_error_message("stop", interface_name, num_stop_interface);
        }
        if (num_start_interface == kNumberOfJoints) {
          claim_flag = true;
        } else if (num_start_interface != 0) {
          generate_error_message("start", interface_name, num_start_interface);
        }
      };

  start_stop_interface(is_effort_interface, hardware_interface::HW_IF_EFFORT,
                       effort_interface_claimed_);
  start_stop_interface(is_velocity_interface, hardware_interface::HW_IF_VELOCITY,
                       velocity_joint_interface_claimed_);

  return hardware_interface::return_type::OK;
}
}  // namespace franka_hardware

#include "pluginlib/class_list_macros.hpp"
// NOLINTNEXTLINE
PLUGINLIB_EXPORT_CLASS(franka_hardware::FrankaHardwareInterface,
                       hardware_interface::SystemInterface)