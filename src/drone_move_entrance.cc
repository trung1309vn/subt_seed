/*
 * Copyright (C) 2018 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/
#include <chrono>
#include <geometry_msgs/Twist.h>
#include <subt_msgs/PoseFromArtifact.h>
#include <ros/ros.h>
#include <std_srvs/SetBool.h>
#include <rosgraph_msgs/Clock.h>

#include <string>

#include <subt_communication_broker/subt_communication_client.h>
#include <subt_ign/CommonTypes.hh>
#include <subt_ign/protobuf/artifact.pb.h>

/// \brief. Example control class, running as a ROS node to control a robot.
class Controller
{
  /// \brief Constructor.
  /// The controller uses the given name as a prefix of its topics, e.g.,
  /// "/x1/cmd_vel" if _name is specified as "x1".
  /// \param[in] _name Name of the robot.
  public: Controller(const std::string &_name);

  /// \brief A function that will be called every loop of the ros spin
  /// cycle.
  public: void Update();

  /// \brief Callback function for message from other comm clients.
  /// \param[in] _srcAddress The address of the robot who sent the packet.
  /// \param[in] _dstAddress The address of the robot who received the packet.
  /// \param[in] _dstPort The destination port.
  /// \param[in] _data The contents the packet holds.
  private: void CommClientCallback(const std::string &_srcAddress,
                                   const std::string &_dstAddress,
                                   const uint32_t _dstPort,
                                   const std::string &_data);

  /// \brief ROS node handler.
  private: ros::NodeHandle n;

  /// \brief publisher to send cmd_vel
  private: ros::Publisher velPub;

  /// \brief Communication client.
  private: std::unique_ptr<subt::CommsClient> client;

  /// \brief Client to request pose from origin.
  private: ros::ServiceClient originClient;

  /// \brief Service to request pose from origin.
  private: subt_msgs::PoseFromArtifact originSrv;

  /// \brief True if robot has arrived at destination.
  private: bool arrived{false};

  /// \brief True if started.
  private: bool started{false};

  /// \brief Last time a comms message to another robot was sent.
  private: std::chrono::time_point<std::chrono::system_clock> lastMsgSentTime;

  /// \brief Name of this robot.
  private: std::string name;

  /// \ brief Last height
  private: double lastHeight{0.0};

  private: bool isBrake{false};
};

/////////////////////////////////////////////////
Controller::Controller(const std::string &_name)
{
  ROS_INFO("Waiting for /clock, /subt/start, and /subt/pose_from_artifact");

  ros::topic::waitForMessage<rosgraph_msgs::Clock>("/clock", this->n);

  // Wait for the start service to be ready.
  ros::service::waitForService("/subt/start", -1);
  ros::service::waitForService("/subt/pose_from_artifact_origin", -1);
  this->name = _name;
  ROS_INFO("Using robot name[%s]\n", this->name.c_str());
}

/////////////////////////////////////////////////
void Controller::CommClientCallback(const std::string &_srcAddress,
                                    const std::string &_dstAddress,
                                    const uint32_t _dstPort,
                                    const std::string &_data)
{
  subt::msgs::ArtifactScore res;
  if (!res.ParseFromString(_data))
  {
    ROS_INFO("Message Contents[%s]", _data.c_str());
  }

  // Add code to handle communication callbacks.
  ROS_INFO("Message from [%s] to [%s] on port [%u]:\n [%s]", _srcAddress.c_str(),
      _dstAddress.c_str(), _dstPort, res.DebugString().c_str());
}

/////////////////////////////////////////////////
void Controller::Update()
{
  if (!this->started)
  {
    // Send start signal
    std_srvs::SetBool::Request req;
    std_srvs::SetBool::Response res;
    req.data = true;
    if (!ros::service::call("/subt/start", req, res))
    {
      ROS_ERROR("Unable to send start signal.");
    }
    else
    {
      ROS_INFO("Sent start signal.");
      this->started = true;
    }

    if (this->started)
    {
      // Create subt communication client
      this->client.reset(new subt::CommsClient(this->name));
      this->client->Bind(&Controller::CommClientCallback, this);

      // Create a cmd_vel publisher to control a vehicle.
      this->velPub = this->n.advertise<geometry_msgs::Twist>(
          this->name + "/cmd_vel", 1);

      // Create a cmd_vel publisher to control a vehicle.
      this->originClient = this->n.serviceClient<subt_msgs::PoseFromArtifact>(
          "/subt/pose_from_artifact_origin");
      this->originSrv.request.robot_name.data = this->name;
    }
    else
      return;
  }

  // Add code that should be processed every iteration.

  std::chrono::time_point<std::chrono::system_clock> now =
    std::chrono::system_clock::now();

  if (std::chrono::duration<double>(now - this->lastMsgSentTime).count() > 5.0)
  {
    // Here, we are assuming that the robot names are "X1" and "X3".
    if (this->name == "X3")
    {
      this->client->SendTo("Hello from " + this->name, "X1");
    }
    else
    {
      this->client->SendTo("Hello from " + this->name, "X3");
    }
    this->lastMsgSentTime = now;
  }


  if (this->arrived)
    return;

  bool call = this->originClient.call(this->originSrv);
  // Query current robot position w.r.t. entrance
  if (!call || !this->originSrv.response.success)
  {
    ROS_ERROR("Failed to call pose_from_artifact_origin service, \
    robot may not exist, be outside staging area, or the service is \
    not available.");

    // Stop robot
    geometry_msgs::Twist msg;
    this->velPub.publish(msg);
    return;
  }

  auto pose = this->originSrv.response.pose.pose;

  // Simple example for robot to go to entrance
  geometry_msgs::Twist msg;

  // Distance to goal
  double dist = pose.position.x * pose.position.x +
    pose.position.y * pose.position.y;

  // Arrived
  if (dist < 0.3 || pose.position.x >= -0.3)
  {
    msg.linear.x = -0.04975;
    if (pose.position.z > 0.25)
    {
      msg.linear.z = 14.85;
    }
    else
    {
      msg.linear.z = 0;
    }
    this->arrived = true;
    ROS_INFO("Arrived at entrance!");

    // Report an artifact
    // Hardcoded to tunnel_circuit_practice_01's exginguisher_3
    subt::msgs::Artifact artifact;
    artifact.set_type(static_cast<uint32_t>(subt::ArtifactType::TYPE_EXTINGUISHER));
    artifact.mutable_pose()->mutable_position()->set_x(-8.1);
    artifact.mutable_pose()->mutable_position()->set_y(37);
    artifact.mutable_pose()->mutable_position()->set_z(0.004);

    std::string serializedData;
    if (!artifact.SerializeToString(&serializedData))
    {
      ROS_ERROR("ReportArtifact(): Error serializing message [%s]",
          artifact.DebugString().c_str());
    }
    else if (!this->client->SendTo(serializedData, subt::kBaseStationName))
    {
      ROS_ERROR("CommsClient failed to Send serialized data.");
    }
  }
  // Move towards entrance
  else
  {
    // Yaw w.r.t. entrance
    // Quaternion to yaw:
    // https://en.wikipedia.org/wiki/Conversion_between_quaternions_and_Euler_angles#Source_Code_2
    auto onCenter = abs(pose.position.y) <= 0.1;
    auto westOfCenter = pose.position.y > 0.1;
    auto eastOfCenter = pose.position.y < -0.1;

    double gravity = 9.79967;
    double mass = 1.52;

    // PID gain
    double weight = gravity * mass;
    double offset = 0.1; // Adding variable to thrust
    double dest = 1.0;
    // Error
    double height = pose.position.z;
    double error = dest - height;
    double thrust;
    double pitch = -0.04975;
    double yaw = 0.0;
    double roll = 0.0;

    // Take off phase 
    if (error >= 0.05)
    {
      double Pout = offset * error / (dest - 0.05);
      thrust = weight + Pout; 
    }
    // Brake phase
    else
    {
      // Braking state 
      // Note: Adjust the threshold to make the brake more sensitive
      if (abs(height - this->lastHeight) > 0.004)
        thrust = weight + 0.05 - 0.5 * dest;
      // Stabilizing state - making drone hover
      else
      {
        thrust = weight;
        if (error <= -0.05)
          thrust = weight - 0.025;

        // Action state
        if (onCenter)
        {
          pitch += 0.05;
          thrust -= 0.01;
          if (this->isBrake)
          {
            if (pose.position.y > 0)
              roll = 0.2;
            else
              roll = -0.2;

            this->isBrake = false;
          }
        }
        // Robot is west of entrance
        else if (westOfCenter)
        {
          roll = -0.05;
          this->isBrake = true;
        }
        else if (eastOfCenter)
        {
          roll = 0.05;
          this->isBrake = true;
        }
      }
    }

    ROS_INFO("Delta height: %f", abs(height - this->lastHeight));
    //ROS_INFO("Tick: %d", this->tick);
    this->lastHeight = height;
    
    ROS_INFO("Drone's height: %f", height);
    ROS_INFO("Thrust: %f", thrust);
    ROS_INFO("Pitch: %f", pitch);
    ROS_INFO("Yaw: %f", yaw);
    ROS_INFO("Roll: %f\n", roll);
    this->lastHeight = height;

    msg.linear.z = thrust;
    msg.linear.x = pitch;
    msg.linear.y = roll;
    msg.angular.z = yaw;
    this->velPub.publish(msg);
  }
}

/////////////////////////////////////////////////
int main(int argc, char** argv)
{
  // Initialize ros
  ros::init(argc, argv, argv[1]);

  ROS_INFO("Starting seed competitor\n");
  std::string name;

  // Get the name of the robot based on the name of the "cmd_vel" topic if
  // the name was not passed in as an argument.
  if (argc < 2 || std::strlen(argv[1]) == 0)
  {
    ros::master::V_TopicInfo masterTopics;
    ros::master::getTopics(masterTopics);

    while (name.empty())
    {
      for (ros::master::V_TopicInfo::iterator it = masterTopics.begin();
          it != masterTopics.end(); ++it)
      {
        const ros::master::TopicInfo &info = *it;
        if (info.name.find("cmd_vel") != std::string::npos)
        {
          int rpos = info.name.rfind("/");
          name = info.name.substr(1, rpos - 1);
        }
      }
      if (name.empty())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  // Otherwise use the name provided as an argument.
  else
  {
    name = argv[1];
  }

  // Create the controller
  Controller controller(name);

  // This sample code iteratively calls Controller::Update. This is just an
  // example. You can write your controller using alternative methods.
  // To get started with ROS visit: http://wiki.ros.org/ROS/Tutorials
  ros::Rate loop_rate(20);
  while (ros::ok())
  {
    controller.Update();
    ros::spinOnce();
    loop_rate.sleep();
  }

  return 0;
}