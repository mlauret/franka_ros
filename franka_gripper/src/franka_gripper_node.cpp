// Copyright (c) 2017 Franka Emika GmbH
// Use of this source code is governed by the Apache-2.0 license, see LICENSE
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ros/init.h>
#include <ros/node_handle.h>
#include <ros/rate.h>
#include <ros/spinner.h>
#include <sensor_msgs/JointState.h>
#include <xmlrpcpp/XmlRpc.h>

#include <franka/gripper_state.h>
#include <franka_gripper/franka_gripper.h>

namespace {

template <typename T_action, typename T_goal, typename T_result>
void handleErrors(actionlib::SimpleActionServer<T_action>* server,
                  std::function<bool(const T_goal&)> handler,
                  const T_goal& goal) {
  T_result result;
  try {
    result.success = handler(goal);
    server->setSucceeded(result);
  } catch (const franka::Exception& ex) {
    ROS_ERROR_STREAM("" << ex.what());
    result.success = false;
    result.error = ex.what();
    server->setAborted(result);
  }
}

}  // anonymous namespace

using actionlib::SimpleActionServer;
using control_msgs::GripperCommandAction;
using franka_gripper::GraspAction;
using franka_gripper::MoveAction;
using franka_gripper::HomingAction;
using franka_gripper::StopAction;
using franka_gripper::GraspGoalConstPtr;
using franka_gripper::MoveGoalConstPtr;
using franka_gripper::StopGoalConstPtr;
using franka_gripper::HomingGoalConstPtr;
using franka_gripper::HomingResult;
using franka_gripper::GraspResult;
using franka_gripper::MoveResult;
using franka_gripper::StopResult;
using franka_gripper::homing;
using franka_gripper::stop;
using franka_gripper::grasp;
using franka_gripper::move;
using franka_gripper::gripperCommandExecuteCallback;
using franka_gripper::updateGripperState;

int main(int argc, char** argv) {
  ros::init(argc, argv, "franka_gripper_node");
  ros::NodeHandle node_handle("~");
  std::string robot_ip;
  if (!node_handle.getParam("robot_ip", robot_ip)) {
    ROS_ERROR("franka_gripper_node: Could not parse robot_ip parameter");
    return -1;
  }
  double width_tolerance(0.01);
  if (node_handle.getParam("width_tolerance", width_tolerance)) {
    ROS_INFO_STREAM("franka_gripper_node: Found width_tolerance" << width_tolerance);
  }

  double default_speed(0.1);
  if (node_handle.getParam("default_speed", default_speed)) {
    ROS_INFO_STREAM("franka_gripper_node: Found default_speed" << default_speed);
  }

  franka::Gripper gripper(robot_ip);

  std::function<bool(const HomingGoalConstPtr&)> homing_handler =
      std::bind(homing, std::cref(gripper), std::placeholders::_1);
  std::function<bool(const StopGoalConstPtr&)> stop_handler =
      std::bind(stop, std::cref(gripper), std::placeholders::_1);
  std::function<bool(const GraspGoalConstPtr&)> grasp_handler =
      std::bind(grasp, std::cref(gripper), std::placeholders::_1);
  std::function<bool(const MoveGoalConstPtr&)> move_handler =
      std::bind(move, std::cref(gripper), std::placeholders::_1);

  SimpleActionServer<HomingAction> homing_action_server_(
      node_handle, "homing",
      std::bind(handleErrors<HomingAction, HomingGoalConstPtr, HomingResult>,
                &homing_action_server_, homing_handler, std::placeholders::_1),
      false);

  SimpleActionServer<StopAction> stop_action_server_(
      node_handle, "stop", std::bind(handleErrors<StopAction, StopGoalConstPtr, StopResult>,
                                     &stop_action_server_, stop_handler, std::placeholders::_1),
      false);

  SimpleActionServer<MoveAction> move_action_server_(
      node_handle, "move", std::bind(handleErrors<MoveAction, MoveGoalConstPtr, MoveResult>,
                                     &move_action_server_, move_handler, std::placeholders::_1),
      false);

  SimpleActionServer<GraspAction> grasp_action_server_(
      node_handle, "grasp", std::bind(handleErrors<GraspAction, GraspGoalConstPtr, GraspResult>,
                                      &grasp_action_server_, grasp_handler, std::placeholders::_1),
      false);

  SimpleActionServer<GripperCommandAction> gripper_command_action_server(
      node_handle, "gripper_action",
      std::bind(&gripperCommandExecuteCallback, std::cref(gripper), default_speed,
                &gripper_command_action_server, std::placeholders::_1),
      false);

  homing_action_server_.start();
  stop_action_server_.start();
  move_action_server_.start();
  grasp_action_server_.start();
  gripper_command_action_server.start();

  double publish_rate(30.0);
  if (!node_handle.getParam("publish_rate", publish_rate)) {
    ROS_INFO_STREAM("franka_gripper_node: Could not find parameter publish_rate. Defaulting to "
                    << publish_rate);
  }

  XmlRpc::XmlRpcValue params;
  if (!node_handle.getParam("joint_names", params)) {
    ROS_ERROR("franka_gripper_node: Could not parse joint_names!");
    return -1;
  }
  if (params.size() != 2) {
    ROS_ERROR("franka_gripper_node: Got wrong number of joint_names!");
    return -1;
  }
  std::vector<std::string> joint_names(params.size());
  for (int i = 0; i < params.size(); ++i) {
    joint_names[i] = static_cast<std::string>(params[i]);
  }

  franka::GripperState gripper_state;
  std::mutex gripper_state_mutex;
  std::thread read_thread([&gripper_state, &gripper, &gripper_state_mutex]() {
    ros::Rate read_rate(10);
    while (read_rate.sleep() && ros::ok()) {
      std::lock_guard<std::mutex> lock(gripper_state_mutex);
      updateGripperState(gripper, &gripper_state);
    }
  });

  ros::Publisher gripper_state_publisher =
      node_handle.advertise<sensor_msgs::JointState>("joint_states", 1);
  ros::AsyncSpinner spinner(2);
  spinner.start();
  ros::Rate rate(publish_rate);
  while (ros::ok()) {
    if (gripper_state_mutex.try_lock()) {
      sensor_msgs::JointState joint_states;
      joint_states.header.stamp = ros::Time::now();
      joint_states.name.push_back(joint_names[0]);
      joint_states.name.push_back(joint_names[1]);
      joint_states.position.push_back(gripper_state.width * 0.5);
      joint_states.position.push_back(gripper_state.width * 0.5);
      joint_states.velocity.push_back(0.0);
      joint_states.velocity.push_back(0.0);
      joint_states.effort.push_back(0.0);
      joint_states.effort.push_back(0.0);
      gripper_state_publisher.publish(joint_states);
      gripper_state_mutex.unlock();
    }
    rate.sleep();
  }
  read_thread.join();
  return 0;
}
