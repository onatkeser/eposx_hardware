#include <limits>

#include "epos_hardware/epos.h"

#include <boost/cstdint.hpp>
#include <boost/foreach.hpp>

namespace epos_hardware {

Epos::Epos(const std::string &name, ros::NodeHandle &nh, ros::NodeHandle &config_nh,
           EposFactory *epos_factory, hardware_interface::ActuatorStateInterface &asi,
           hardware_interface::VelocityActuatorInterface &avi,
           hardware_interface::PositionActuatorInterface &api,
           hardware_interface::EffortActuatorInterface &aei,
           battery_state_interface::BatteryStateInterface &bsi)
    : name_(name), config_nh_(config_nh), diagnostic_updater_(nh, config_nh),
      epos_factory_(epos_factory), has_init_(false), position_(0), velocity_(0), effort_(0),
      current_(0), statusword_(0), position_cmd_(0), velocity_cmd_(0) {

  valid_ = true;
  if (!config_nh_.getParam("actuator_name", actuator_name_)) {
    ROS_ERROR("You must specify an actuator name");
    valid_ = false;
  }

  std::string serial_number_str; // use later agian
  if (!config_nh_.getParam("serial_number", serial_number_str)) {
    ROS_ERROR("You must specify a serial number");
    valid_ = false;
  } else {
    ROS_ASSERT(SerialNumberFromHex(serial_number_str, &serial_number_));
  }

  {
    std::map< std::string, std::string > str_map;
    if (config_nh_.getParam("operation_mode_map", str_map)) {
      for (std::map< std::string, std::string >::const_iterator str_pair = str_map.begin();
           str_pair != str_map.end(); ++str_pair) {
        if (str_pair->second == "profile_position") {
          operation_mode_map_[str_pair->first] = PROFILE_POSITION_MODE;
        } else if (str_pair->second == "profile_velocity") {
          operation_mode_map_[str_pair->first] = PROFILE_VELOCITY_MODE;
        } else if (str_pair->second == "current") {
          operation_mode_map_[str_pair->first] = CURRENT_MODE;
        } else {
          ROS_ERROR_STREAM(str_pair->second << " is not a valid operation mode");
          valid_ = false;
        }
      }
    }
  }

  config_nh_.param("rw_ros_units", rw_ros_units_, false);

  ROS_INFO_STREAM(actuator_name_);
  {
    hardware_interface::ActuatorStateHandle state_handle(actuator_name_, &position_, &velocity_,
                                                         &effort_);
    asi.registerHandle(state_handle);
    hardware_interface::ActuatorHandle position_handle(state_handle, &position_cmd_);
    api.registerHandle(position_handle);
    hardware_interface::ActuatorHandle velocity_handle(state_handle, &velocity_cmd_);
    avi.registerHandle(velocity_handle);
    hardware_interface::ActuatorHandle effort_handle(state_handle, &torque_cmd_);
    aei.registerHandle(effort_handle);
  }

  config_nh_.getParam("power_supply/name", power_supply_name_);
  if (!power_supply_name_.empty()) {
    // init measureable variables
    power_supply_state_.voltage = 0.;
    power_supply_state_.present = false;
    // init unmeasureable variables
    power_supply_state_.current = std::numeric_limits< float >::quiet_NaN();
    power_supply_state_.charge = std::numeric_limits< float >::quiet_NaN();
    power_supply_state_.capacity = std::numeric_limits< float >::quiet_NaN();
    power_supply_state_.design_capacity = std::numeric_limits< float >::quiet_NaN();
    power_supply_state_.percentage = std::numeric_limits< float >::quiet_NaN();
    power_supply_state_.power_supply_status =
        sensor_msgs::BatteryState::POWER_SUPPLY_STATUS_UNKNOWN;
    power_supply_state_.power_supply_health =
        sensor_msgs::BatteryState::POWER_SUPPLY_HEALTH_UNKNOWN;
    // init constants
    power_supply_state_.power_supply_technology = config_nh_.param< int >(
        "power_supply/technology", sensor_msgs::BatteryState::POWER_SUPPLY_TECHNOLOGY_UNKNOWN);
    power_supply_state_.location = config_nh_.param< std::string >("power_supply/location", "");
    power_supply_state_.serial_number =
        config_nh_.param< std::string >("power_supply/serial_number", "");
    // register name and state object
    battery_state_interface::BatteryStateHandle battery_state_handle(power_supply_name_,
                                                                     &power_supply_state_);
    bsi.registerHandle(battery_state_handle);
  }

  diagnostic_updater_.setHardwareID(serial_number_str);
  {
    std::stringstream motor_diagnostic_name_ss;
    motor_diagnostic_name_ss << name << ": "
                             << "Motor";
    diagnostic_updater_.add(motor_diagnostic_name_ss.str(),
                            boost::bind(&Epos::buildMotorStatus, this, _1));
  }
  {
    std::stringstream motor_output_diagnostic_name_ss;
    motor_output_diagnostic_name_ss << name << ": "
                                    << "Motor Output";
    diagnostic_updater_.add(motor_output_diagnostic_name_ss.str(),
                            boost::bind(&Epos::buildMotorOutputStatus, this, _1));
  }
}

Epos::~Epos() {
  unsigned int error_code;
  if (node_handle_)
    VCS_SetDisableState(node_handle_->device_handle->ptr, node_handle_->node_id, &error_code);
}

class ParameterSetLoader {
public:
  ParameterSetLoader(ros::NodeHandle nh) : nh_(nh) {}
  ParameterSetLoader(ros::NodeHandle parent_nh, const std::string &name) : nh_(parent_nh, name) {}
  template < class T > ParameterSetLoader &param(const std::string &name, T &value) {
    if (nh_.getParam(name, value))
      found_.push_back(name);
    else
      not_found_.push_back(name);
    return *this;
  }
  bool all_or_none(bool &found_all) {
    if (not_found_.size() == 0) {
      found_all = true;
      return true;
    }
    if (found_.size() == 0) {
      found_all = false;
      return true;
    }
    ROS_ERROR_STREAM("Expected all or none parameter set: (" << nh_.getNamespace() << ")");
    BOOST_FOREACH (const std::string &name, found_) {
      ROS_ERROR_STREAM("\tFound: " << nh_.resolveName(name));
    }
    BOOST_FOREACH (const std::string &name, not_found_) {
      ROS_ERROR_STREAM("\tExpected: " << nh_.resolveName(name));
    }
    return false;
  }

private:
  ros::NodeHandle nh_;
  std::vector< std::string > found_;
  std::vector< std::string > not_found_;
};

#define VCS(func, ...)                                                                             \
  if (!VCS_##func(node_handle_->device_handle->ptr, node_handle_->node_id, __VA_ARGS__,            \
                  &error_code)) {                                                                  \
    ROS_ERROR("Failed to " #func);                                                                 \
    return false;                                                                                  \
  }

#define VCS_SET_OBJECT(index, subindex, data, length)                                              \
  do {                                                                                             \
    unsigned int bytes_written;                                                                    \
    if (!VCS_SetObject(node_handle_->device_handle->ptr, node_handle_->node_id, index, subindex,   \
                       data, length, &bytes_written, &error_code)) {                               \
      ROS_ERROR("Failed to SetObject(%#06x, %#04x, len: %d)", index, subindex, length);            \
      return false;                                                                                \
    }                                                                                              \
  } while (true)

#define VCS_FROM_SINGLE_PARAM_REQUIRED(nh, type, name, func)                                       \
  type name;                                                                                       \
  if (!nh.getParam(#name, name)) {                                                                 \
    ROS_ERROR_STREAM(nh.resolveName(#name) << " not specified");                                   \
    return false;                                                                                  \
  } else {                                                                                         \
    VCS(func, name);                                                                               \
  }
#define VCS_FROM_SINGLE_PARAM_OPTIONAL(nh, type, name, func)                                       \
  bool name##_set;                                                                                 \
  type name;                                                                                       \
  if (name##_set = nh.getParam(#name, name)) {                                                     \
    VCS(func, name);                                                                               \
  }

bool Epos::init() {
  if (!valid_) {
    ROS_ERROR_STREAM("Not Initializing: 0x" << std::hex << serial_number_
                                            << ", initial construction failed");
    return false;
  }

  ROS_INFO_STREAM("Initializing: 0x" << std::hex << serial_number_);
  unsigned int error_code;
  node_handle_ = epos_factory_->CreateNodeHandle("EPOS4", "MAXON SERIAL V2", "USB", serial_number_,
                                                 &error_code);
  if (!node_handle_) {
    ROS_ERROR("Could not find motor");
    return false;
  }
  ROS_INFO_STREAM("Found Motor");

  if (!VCS_SetProtocolStackSettings(node_handle_->device_handle->ptr, 1000000, 500, &error_code)) {
    ROS_ERROR("Failed to SetProtocolStackSettings");
    return false;
  }

  if (!VCS_SetDisableState(node_handle_->device_handle->ptr, node_handle_->node_id, &error_code)) {
    ROS_ERROR("Failed to SetDisableState");
    return false;
  }

  {
    const std::map< std::string, OperationMode >::const_iterator initial_mode(
        operation_mode_map_.find(""));
    if (initial_mode != operation_mode_map_.end()) {
      VCS(SetOperationMode, initial_mode->second);
    } else {
      ROS_WARN("No initial operation mode");
    }
  }

  std::string fault_reaction_str;
#define SET_FAULT_REACTION_OPTION(val)                                                             \
  do {                                                                                             \
    int16_t data = val;                                                                            \
    VCS_SET_OBJECT(0x605E, 0x00, &data, 2);                                                        \
  } while (true)

  if (config_nh_.getParam("fault_reaction_option", fault_reaction_str)) {
    if (fault_reaction_str == "signal_only") {
      SET_FAULT_REACTION_OPTION(-1);
    } else if (fault_reaction_str == "disable_drive") {
      SET_FAULT_REACTION_OPTION(0);
    } else if (fault_reaction_str == "slow_down_ramp") {
      SET_FAULT_REACTION_OPTION(1);
    } else if (fault_reaction_str == "slow_down_quickstop") {
      SET_FAULT_REACTION_OPTION(2);
    } else {
      ROS_ERROR_STREAM(fault_reaction_str << " is not a valid fault reaction option");
      return false;
    }
  }

  if (!config_nh_.getParam("torque_constant", torque_constant_)) {
    ROS_WARN(
        "No torque constant specified, you can supply one using the 'torque_constant' parameter");
    torque_constant_ = 1.0;
  }

  ROS_INFO("Configuring Motor");
  {
    nominal_current_ = 0;
    max_current_ = 0;
    ros::NodeHandle motor_nh(config_nh_, "motor");

    VCS_FROM_SINGLE_PARAM_REQUIRED(motor_nh, int, type, SetMotorType);

    {
      bool dc_motor;
      double nominal_current;
      double max_output_current;
      double thermal_time_constant;
      if (!ParameterSetLoader(motor_nh, "dc_motor")
               .param("nominal_current", nominal_current)
               .param("max_output_current", max_output_current)
               .param("thermal_time_constant", thermal_time_constant)
               .all_or_none(dc_motor))
        return false;
      if (dc_motor) {
        nominal_current_ = nominal_current;
        max_current_ = max_output_current;
        VCS(SetDcMotorParameter,
            (int)(1000 * nominal_current),    // A -> mA
            (int)(1000 * max_output_current), // A -> mA
            (int)(10 * thermal_time_constant) // s -> 100ms
        );
      }
    }

    {
      bool ec_motor;
      double nominal_current;
      double max_output_current;
      double thermal_time_constant;
      int number_of_pole_pairs;
      if (!ParameterSetLoader(motor_nh, "ec_motor")
               .param("nominal_current", nominal_current)
               .param("max_output_current", max_output_current)
               .param("thermal_time_constant", thermal_time_constant)
               .param("number_of_pole_pairs", number_of_pole_pairs)
               .all_or_none(ec_motor))
        return false;

      if (ec_motor) {
        nominal_current_ = nominal_current;
        max_current_ = max_output_current;
        VCS(SetEcMotorParameter,
            (int)(1000 * nominal_current),     // A -> mA
            (int)(1000 * max_output_current),  // A -> mA
            (int)(10 * thermal_time_constant), // s -> 100ms
            number_of_pole_pairs);
      }
    }

    double max_speed;
    if (motor_nh.getParam("max_speed", max_speed)) {
      uint32_t data = max_speed;
      VCS_SET_OBJECT(0x6080, 0x00, &data, 4);
    }
  }

  ROS_INFO("Configuring Sensor");
  {
    encoder_resolution_ = 0;
    ros::NodeHandle sensor_nh(config_nh_, "sensor");

    VCS_FROM_SINGLE_PARAM_REQUIRED(sensor_nh, int, type, SetSensorType);

    {
      bool incremental_encoder;
      int resolution;
      bool inverted_polarity;

      if (!ParameterSetLoader(sensor_nh, "incremental_encoder")
               .param("resolution", resolution)
               .param("inverted_polarity", inverted_polarity)
               .all_or_none(incremental_encoder))
        return false;
      if (incremental_encoder) {
        VCS(SetIncEncoderParameter, resolution, inverted_polarity);
        if (inverted_polarity) {
          encoder_resolution_ = -resolution;
        } else {
          encoder_resolution_ = resolution;
        }
      }
    }

    {
      bool hall_sensor;
      bool inverted_polarity;

      if (!ParameterSetLoader(sensor_nh, "hall_sensor")
               .param("inverted_polarity", inverted_polarity)
               .all_or_none(hall_sensor))
        return false;
      if (hall_sensor) {
        VCS(SetHallSensorParameter, inverted_polarity);
      }
    }

    {
      bool ssi_absolute_encoder;
      int data_rate;
      int number_of_multiturn_bits;
      int number_of_singleturn_bits;
      bool inverted_polarity;

      if (!ParameterSetLoader(sensor_nh, "ssi_absolute_encoder")
               .param("data_rate", data_rate)
               .param("number_of_multiturn_bits", number_of_multiturn_bits)
               .param("number_of_singleturn_bits", number_of_singleturn_bits)
               .param("inverted_polarity", inverted_polarity)
               .all_or_none(ssi_absolute_encoder))
        return false;
      if (ssi_absolute_encoder) {
        VCS(SetSsiAbsEncoderParameter, data_rate, number_of_multiturn_bits,
            number_of_singleturn_bits, inverted_polarity);
        if (inverted_polarity) {
          encoder_resolution_ = -(1 << number_of_singleturn_bits);
        } else {
          encoder_resolution_ = (1 << number_of_singleturn_bits);
        }
      }
    }

    if (encoder_resolution_ == 0) {
      ROS_ERROR("No encoder resolution");
      return false;
    }
  }

  {
    ROS_INFO("Configuring Safety");
    ros::NodeHandle safety_nh(config_nh_, "safety");

    VCS_FROM_SINGLE_PARAM_OPTIONAL(safety_nh, int, max_following_error, SetMaxFollowingError);
    VCS_FROM_SINGLE_PARAM_OPTIONAL(safety_nh, int, max_profile_velocity, SetMaxProfileVelocity);
    VCS_FROM_SINGLE_PARAM_OPTIONAL(safety_nh, int, max_acceleration, SetMaxAcceleration);
    if (max_profile_velocity_set)
      max_profile_velocity_ = max_profile_velocity;
    else
      max_profile_velocity_ = -1;
  }

  {
    ROS_INFO("Configuring Position Regulator");
    ros::NodeHandle position_regulator_nh(config_nh_, "position_regulator");
    {
      bool position_regulator_gain;
      int p, i, d;
      if (!ParameterSetLoader(position_regulator_nh, "gain")
               .param("p", p)
               .param("i", i)
               .param("d", d)
               .all_or_none(position_regulator_gain))
        return false;
      if (position_regulator_gain) {
        VCS(SetPositionRegulatorGain, p, i, d);
      }
    }

    {
      bool position_regulator_feed_forward;
      int velocity, acceleration;
      if (!ParameterSetLoader(position_regulator_nh, "feed_forward")
               .param("velocity", velocity)
               .param("acceleration", acceleration)
               .all_or_none(position_regulator_feed_forward))
        return false;
      if (position_regulator_feed_forward) {
        VCS(SetPositionRegulatorFeedForward, velocity, acceleration);
      }
    }
  }

  {
    ROS_INFO("Configuring Velocity Regulator");
    ros::NodeHandle velocity_regulator_nh(config_nh_, "velocity_regulator");
    {
      bool velocity_regulator_gain;
      int p, i;
      if (!ParameterSetLoader(velocity_regulator_nh, "gain")
               .param("p", p)
               .param("i", i)
               .all_or_none(velocity_regulator_gain))
        return false;
      if (velocity_regulator_gain) {
        VCS(SetVelocityRegulatorGain, p, i);
      }
    }

    {
      bool velocity_regulator_feed_forward;
      int velocity, acceleration;
      if (!ParameterSetLoader(velocity_regulator_nh, "feed_forward")
               .param("velocity", velocity)
               .param("acceleration", acceleration)
               .all_or_none(velocity_regulator_feed_forward))
        return false;
      if (velocity_regulator_feed_forward) {
        VCS(SetVelocityRegulatorFeedForward, velocity, acceleration);
      }
    }
  }

  {
    ROS_INFO("Configuring Current Regulator");
    ros::NodeHandle current_regulator_nh(config_nh_, "current_regulator");
    {
      bool current_regulator_gain;
      int p, i;
      if (!ParameterSetLoader(current_regulator_nh, "gain")
               .param("p", p)
               .param("i", i)
               .all_or_none(current_regulator_gain))
        return false;
      if (current_regulator_gain) {
        VCS(SetCurrentRegulatorGain, p, i);
      }
    }
  }

  {
    ROS_INFO("Configuring Position Profile");
    ros::NodeHandle position_profile_nh(config_nh_, "position_profile");
    {
      bool position_profile;
      int velocity, acceleration, deceleration;
      if (!ParameterSetLoader(position_profile_nh)
               .param("velocity", velocity)
               .param("acceleration", acceleration)
               .param("deceleration", deceleration)
               .all_or_none(position_profile))
        return false;
      if (position_profile) {
        VCS(SetPositionProfile, velocity, acceleration, deceleration);
      }
    }

    {
      bool position_profile_window;
      int window;
      double time;
      if (!ParameterSetLoader(position_profile_nh, "window")
               .param("window", window)
               .param("time", time)
               .all_or_none(position_profile_window))
        return false;
      if (position_profile_window) {
        VCS(EnablePositionWindow, window,
            (int)(1000 * time) // s -> ms
        );
      }
    }
  }

  {
    ROS_INFO("Configuring Velocity Profile");
    ros::NodeHandle velocity_profile_nh(config_nh_, "velocity_profile");
    {
      bool velocity_profile;
      int acceleration, deceleration;
      if (!ParameterSetLoader(velocity_profile_nh)
               .param("acceleration", acceleration)
               .param("deceleration", deceleration)
               .all_or_none(velocity_profile))
        return false;
      if (velocity_profile) {
        VCS(SetVelocityProfile, acceleration, deceleration);
      }
    }

    {
      bool velocity_profile_window;
      int window;
      double time;
      if (!ParameterSetLoader(velocity_profile_nh, "window")
               .param("window", window)
               .param("time", time)
               .all_or_none(velocity_profile_window))
        return false;
      if (velocity_profile_window) {
        VCS(EnableVelocityWindow, window,
            (int)(1000 * time) // s -> ms
        );
      }
    }
  }

  ROS_INFO("Querying Faults");
  unsigned char num_errors;
  if (!VCS_GetNbOfDeviceError(node_handle_->device_handle->ptr, node_handle_->node_id, &num_errors,
                              &error_code))
    return false;
  for (int i = 1; i <= num_errors; ++i) {
    unsigned int error_number;
    if (!VCS_GetDeviceErrorCode(node_handle_->device_handle->ptr, node_handle_->node_id, i,
                                &error_number, &error_code))
      return false;
    ROS_WARN_STREAM("EPOS Device Error: 0x" << std::hex << error_number);
  }

  bool clear_faults;
  config_nh_.param< bool >("clear_faults", clear_faults, false);
  if (num_errors > 0) {
    if (clear_faults) {
      ROS_INFO("Clearing faults");
      if (!VCS_ClearFault(node_handle_->device_handle->ptr, node_handle_->node_id, &error_code)) {
        ROS_ERROR("Could not clear faults");
        return false;
      } else
        ROS_INFO("Cleared faults");
    } else {
      ROS_ERROR("Not clearing faults, but faults exist");
      return false;
    }
  }

  if (!VCS_GetNbOfDeviceError(node_handle_->device_handle->ptr, node_handle_->node_id, &num_errors,
                              &error_code))
    return false;
  if (num_errors > 0) {
    ROS_ERROR("Not all faults were cleared");
    return false;
  }

  config_nh_.param< bool >("halt_velocity", halt_velocity_, false);

  ROS_INFO_STREAM("Enabling Motor");
  if (!VCS_SetEnableState(node_handle_->device_handle->ptr, node_handle_->node_id, &error_code))
    return false;

  has_init_ = true;
  return true;
}

void Epos::doSwitch(const std::list< hardware_interface::ControllerInfo > &start_list,
                    const std::list< hardware_interface::ControllerInfo > &stop_list) {
  // switch epos's operation mode according to starting controllers
  for (std::list< hardware_interface::ControllerInfo >::const_iterator starting_controller =
           start_list.begin();
       starting_controller != start_list.end(); ++starting_controller) {
    const std::map< std::string, OperationMode >::const_iterator mode_to_switch(
        operation_mode_map_.find(starting_controller->name));
    if (mode_to_switch != operation_mode_map_.end()) {
      unsigned int error_code;
      VCS_SetOperationMode(node_handle_->device_handle->ptr, node_handle_->node_id,
                           mode_to_switch->second, &error_code);
    }
  }
}

void Epos::read() {
  if (!has_init_)
    return;

  unsigned int error_code;
  unsigned int bytes_read;

  // Read statusword
  VCS_GetObject(node_handle_->device_handle->ptr, node_handle_->node_id, 0x6041, 0x00, &statusword_,
                2, &bytes_read, &error_code);

  int position_raw;
  int velocity_raw;
  short current_raw;
  VCS_GetPositionIs(node_handle_->device_handle->ptr, node_handle_->node_id, &position_raw,
                    &error_code);
  VCS_GetVelocityIs(node_handle_->device_handle->ptr, node_handle_->node_id, &velocity_raw,
                    &error_code);
  VCS_GetCurrentIs(node_handle_->device_handle->ptr, node_handle_->node_id, &current_raw,
                   &error_code);
  if (rw_ros_units_) {
    // quad-counts of the encoder -> rad
    position_ = position_raw * M_PI / (2. * encoder_resolution_);
    // rpm -> rad/s
    velocity_ = velocity_raw * M_PI / 30.;
    // mA -> A
    current_ = current_raw / 1000.;
    // mNm -> Nm
    effort_ = currentToTorque(current_) / 1000.;
  } else {
    position_ = position_raw;
    velocity_ = velocity_raw;
    current_ = current_raw / 1000.0; // mA -> A
    effort_ = currentToTorque(current_);
  }

  // read battery status
  if (!power_supply_name_.empty()) {
    boost::uint16_t voltage10x(0);
    VCS_GetObject(node_handle_->device_handle->ptr, node_handle_->node_id, 0x2200, 0x01,
                  &voltage10x, 2, &bytes_read, &error_code);
    power_supply_state_.voltage = voltage10x / 10.;
    power_supply_state_.present = (voltage10x > 0);
  }
}

void Epos::write() {
  if (!has_init_)
    return;

  unsigned int error_code;
  if (operation_mode_ == PROFILE_VELOCITY_MODE) {
    if (isnan(velocity_cmd_))
      return;
    int cmd;
    if (rw_ros_units_) {
      // rad/s -> rpm
      cmd = (int)(velocity_cmd_ * 30. / M_PI);
    } else {
      cmd = (int)velocity_cmd_;
    }
    if (max_profile_velocity_ >= 0) {
      if (cmd < -max_profile_velocity_)
        cmd = -max_profile_velocity_;
      if (cmd > max_profile_velocity_)
        cmd = max_profile_velocity_;
    }

    if (cmd == 0 && halt_velocity_) {
      VCS_HaltVelocityMovement(node_handle_->device_handle->ptr, node_handle_->node_id,
                               &error_code);
    } else {
      VCS_MoveWithVelocity(node_handle_->device_handle->ptr, node_handle_->node_id, cmd,
                           &error_code);
    }
  } else if (operation_mode_ == PROFILE_POSITION_MODE) {
    if (isnan(position_cmd_))
      return;
    int cmd;
    if (rw_ros_units_) {
      // rad -> quad-counts of the encoder
      cmd = (int)(position_cmd_ * 2. * encoder_resolution_ / M_PI);
    } else {
      cmd = (int)position_cmd_;
    }
    VCS_MoveToPosition(node_handle_->device_handle->ptr, node_handle_->node_id, cmd, true, true,
                       &error_code);
  } else if (operation_mode_ == CURRENT_MODE) {
    if (isnan(torque_cmd_))
      return;
    int cmd;
    if (rw_ros_units_) {
      // Nm -> mNm
      cmd = (int)(torqueToCurrent(torque_cmd_) * 1000.);
    } else {
      cmd = (int)torqueToCurrent(torque_cmd_);
    }
    VCS_SetCurrentMust(node_handle_->device_handle->ptr, node_handle_->node_id, cmd, &error_code);
  }
}

void Epos::update_diagnostics() { diagnostic_updater_.update(); }

#define STATUSWORD(b, v) ((v >> b) & 1)
#define READY_TO_SWITCH_ON (0)
#define SWITCHED_ON (1)
#define ENABLE (2)
#define FAULT (3)
#define VOLTAGE_ENABLED (4)
#define QUICKSTOP (5)
#define WARNING (7)
#define TARGET_REACHED (10)
#define CURRENT_LIMIT_ACTIVE (11)

void Epos::buildMotorStatus(diagnostic_updater::DiagnosticStatusWrapper &stat) {
  stat.add("Actuator Name", actuator_name_);
  unsigned int error_code;
  if (has_init_) {
    bool enabled = STATUSWORD(READY_TO_SWITCH_ON, statusword_) &&
                   STATUSWORD(SWITCHED_ON, statusword_) && STATUSWORD(ENABLE, statusword_);
    if (enabled) {
      stat.summary(diagnostic_msgs::DiagnosticStatus::OK, "Enabled");
    } else {
      stat.summary(diagnostic_msgs::DiagnosticStatus::OK, "Disabled");
    }

    // Quickstop is enabled when bit is unset (only read quickstop when enabled)
    if (!STATUSWORD(QUICKSTOP, statusword_) && enabled) {
      stat.mergeSummary(diagnostic_msgs::DiagnosticStatus::WARN, "Quickstop");
    }

    if (STATUSWORD(WARNING, statusword_)) {
      stat.mergeSummary(diagnostic_msgs::DiagnosticStatus::WARN, "Warning");
    }

    if (STATUSWORD(FAULT, statusword_)) {
      stat.mergeSummary(diagnostic_msgs::DiagnosticStatus::ERROR, "Fault");
    }

    stat.add< bool >("Enabled", STATUSWORD(ENABLE, statusword_));
    stat.add< bool >("Fault", STATUSWORD(FAULT, statusword_));
    stat.add< bool >("Voltage Enabled", STATUSWORD(VOLTAGE_ENABLED, statusword_));
    stat.add< bool >("Quickstop", STATUSWORD(QUICKSTOP, statusword_));
    stat.add< bool >("Warning", STATUSWORD(WARNING, statusword_));

    unsigned char num_errors;
    if (VCS_GetNbOfDeviceError(node_handle_->device_handle->ptr, node_handle_->node_id, &num_errors,
                               &error_code)) {
      for (int i = 1; i <= num_errors; ++i) {
        unsigned int error_number;
        if (VCS_GetDeviceErrorCode(node_handle_->device_handle->ptr, node_handle_->node_id, i,
                                   &error_number, &error_code)) {
          std::stringstream error_msg;
          error_msg << "EPOS Device Error: 0x" << std::hex << error_number;
          stat.mergeSummary(diagnostic_msgs::DiagnosticStatus::ERROR, error_msg.str());
        } else {
          std::string error_str;
          if (GetErrorInfo(error_code, &error_str)) {
            std::stringstream error_msg;
            error_msg << "Could not read device error: " << error_str;
            stat.mergeSummary(diagnostic_msgs::DiagnosticStatus::ERROR, error_msg.str());
          } else {
            stat.mergeSummary(diagnostic_msgs::DiagnosticStatus::ERROR,
                              "Could not read device error");
          }
        }
      }
    } else {
      std::string error_str;
      if (GetErrorInfo(error_code, &error_str)) {
        std::stringstream error_msg;
        error_msg << "Could not read device errors: " << error_str;
        stat.mergeSummary(diagnostic_msgs::DiagnosticStatus::ERROR, error_msg.str());
      } else {
        stat.mergeSummary(diagnostic_msgs::DiagnosticStatus::ERROR, "Could not read device errors");
      }
    }

  } else {
    stat.summary(diagnostic_msgs::DiagnosticStatus::ERROR, "EPOS not initialized");
  }
}

void Epos::buildMotorOutputStatus(diagnostic_updater::DiagnosticStatusWrapper &stat) {
  std::string operation_mode_str;
  if (operation_mode_ == PROFILE_POSITION_MODE) {
    operation_mode_str = "Profile Position Mode";
    stat.add("Commanded Position",
             boost::lexical_cast< std::string >(position_cmd_) + " rotations");
  } else if (operation_mode_ == PROFILE_VELOCITY_MODE) {
    operation_mode_str = "Profile Velocity Mode";
    stat.add("Commanded Velocity", boost::lexical_cast< std::string >(velocity_cmd_) + " rpm");
  } else if (operation_mode_ == CURRENT_MODE) {
    operation_mode_str = "Current Mode";
    stat.add("Commanded Torque", boost::lexical_cast< std::string >(torque_cmd_) + " Nm");
    stat.add("Commanded Current",
             boost::lexical_cast< std::string >(torqueToCurrent(torque_cmd_)) + " A");
  } else {
    operation_mode_str = "Unknown Mode";
  }
  stat.add("Operation Mode", operation_mode_str);
  stat.add("Nominal Current", boost::lexical_cast< std::string >(nominal_current_) + " A");
  stat.add("Max Current", boost::lexical_cast< std::string >(max_current_) + " A");

  unsigned int error_code;
  if (has_init_) {
    stat.add("Position", boost::lexical_cast< std::string >(position_) + " rotations");
    stat.add("Velocity", boost::lexical_cast< std::string >(velocity_) + " rpm");
    stat.add("Torque", boost::lexical_cast< std::string >(effort_) + " Nm");
    stat.add("Current", boost::lexical_cast< std::string >(current_) + " A");

    stat.add< bool >("Target Reached", STATUSWORD(TARGET_REACHED, statusword_));
    stat.add< bool >("Current Limit Active", STATUSWORD(CURRENT_LIMIT_ACTIVE, statusword_));

    stat.summary(diagnostic_msgs::DiagnosticStatus::OK, "EPOS operating in " + operation_mode_str);
    if (STATUSWORD(CURRENT_LIMIT_ACTIVE, statusword_))
      stat.mergeSummary(diagnostic_msgs::DiagnosticStatus::WARN, "Current Limit Active");
    if (nominal_current_ > 0 && std::abs(current_) > nominal_current_) {
      stat.mergeSummaryf(diagnostic_msgs::DiagnosticStatus::WARN,
                         "Nominal Current Exceeded (Current: %f A)", current_);
    }

  } else {
    stat.summary(diagnostic_msgs::DiagnosticStatus::ERROR, "EPOS not initialized");
  }
}
} // namespace epos_hardware
