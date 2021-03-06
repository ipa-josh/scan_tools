/*
 * Copyright (c) 2011, Ivan Dryanovski, William Morris
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the CCNY Robotics Lab nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*  This package uses Canonical Scan Matcher [1], written by
 *  Andrea Censi
 *
 *  [1] A. Censi, "An ICP variant using a point-to-line metric"
 *  Proceedings of the IEEE International Conference
 *  on Robotics and Automation (ICRA), 2008
 */

#include <laser_scan_matcher/laser_scan_matcher_multi.h>
#include <pcl_conversions/pcl_conversions.h>
#include <boost/assign.hpp>

namespace scan_tools
{

LaserScanMatcherMulti::LaserScanMatcherMulti(ros::NodeHandle nh, ros::NodeHandle nh_private):
  nh_(nh),
  nh_private_(nh_private),
  initialized_(false),
  received_imu_(false),
  received_odom_(false),
  received_vel_(false),
  scan_subscriber_(nh, "scan", 1),
  cloud_subscriber_(nh, "cloud", 1),
  action_sub1_(nh, "action", 1),
  action_sub2_(nh, "action", 1),
  cloud_sync_(SyncPolicy_Cloud(10), cloud_subscriber_, action_sub2_),
  scan_sync_ (SyncPolicy_Scan(10),  scan_subscriber_,  action_sub1_)
{
  ROS_INFO("Starting LaserScanMatcherMulti");

  // **** init parameters

  initParams();

  // **** state variables

  f2b_.setIdentity();
  //f2b_kf_.setIdentity();
  input_.laser[0] = 0.0;
  input_.laser[1] = 0.0;
  input_.laser[2] = 0.0;

  // Initialize output_ vectors as Null for error-checking
  output_.cov_x_m = 0;
  output_.dx_dy1_m = 0;
  output_.dx_dy2_m = 0;

  // **** publishers
  
  dbg_scan_publisher_[0] = nh_.advertise<sensor_msgs::LaserScan>("dbg_scan_ref", 5);
  dbg_scan_publisher_[1] = nh_.advertise<sensor_msgs::LaserScan>("dbg_scan_inp", 5);
  dbg_scan_publisher_[2] = nh_.advertise<sensor_msgs::LaserScan>("dbg_scan_refo", 5);
  
    odom_publisher_  = nh_.advertise<nav_msgs::Odometry>(
      "odom_matched", 5);

  if (publish_pose_)
  {
    pose_publisher_  = nh_.advertise<geometry_msgs::Pose2D>(
      "pose2D", 5);
  }

  if (publish_pose_stamped_)
  {
    pose_stamped_publisher_ = nh_.advertise<geometry_msgs::PoseStamped>(
      "pose_stamped", 5);
  }

  if (publish_pose_with_covariance_)
  {
    pose_with_covariance_publisher_  = nh_.advertise<geometry_msgs::PoseWithCovariance>(
      "pose_with_covariance", 5);
  }

  if (publish_pose_with_covariance_stamped_)
  {
    pose_with_covariance_stamped_publisher_ = nh_.advertise<geometry_msgs::PoseWithCovarianceStamped>(
      "pose_with_covariance_stamped", 5);
  }

  // *** subscribers

  /*if (use_cloud_input_)
  {
    cloud_subscriber_ = nh_.subscribe(
      "cloud", 1, &LaserScanMatcherMulti::cloudCallback, this);
  }
  else
  {
    scan_subscriber_ = nh_.subscribe(
      "scan", 1, &LaserScanMatcherMulti::scanCallback, this);
  }*/
  if (use_cloud_input_)
  {
	cloud_sync_.registerCallback(boost::bind(&LaserScanMatcherMulti::cloudCallback, this, _1, _2));
  }
  else
  {
	ROS_INFO("registerCallback scanCallback");
	scan_sync_.registerCallback(boost::bind(&LaserScanMatcherMulti::scanCallback, this, _1, _2));
  }

  if (use_imu_)
  {
    imu_subscriber_ = nh_.subscribe(
      "imu/data", 1, &LaserScanMatcherMulti::imuCallback, this);
  }
  if (use_odom_)
  {
    odom_subscriber_ = nh_.subscribe(
      "odom", 1, &LaserScanMatcherMulti::odomCallback, this);
  }
  if (use_vel_)
  {
    if (stamped_vel_)
      vel_subscriber_ = nh_.subscribe(
        "vel", 1, &LaserScanMatcherMulti::velStmpCallback, this);
    else
      vel_subscriber_ = nh_.subscribe(
        "vel", 1, &LaserScanMatcherMulti::velCallback, this);
  }
  
    eval1_subscriber_ = nh_.subscribe(
      "/s1", 1, &LaserScanMatcherMulti::eval1Callback, this);
    eval2_subscriber_ = nh_.subscribe(
      "/s2", 1, &LaserScanMatcherMulti::eval2Callback, this);
  
}

LaserScanMatcherMulti::~LaserScanMatcherMulti()
{
  ROS_INFO("Destroying LaserScanMatcherMulti");
}

void LaserScanMatcherMulti::initParams()
{
  if (!nh_private_.getParam ("base_frame", base_frame_))
    base_frame_ = "base_link";
  if (!nh_private_.getParam ("fixed_frame", fixed_frame_))
    fixed_frame_ = "world";

  // **** input type - laser scan, or point clouds?
  // if false, will subscribe to LaserScan msgs on /scan.
  // if true, will subscribe to PointCloud2 msgs on /cloud

  if (!nh_private_.getParam ("use_cloud_input", use_cloud_input_))
    use_cloud_input_= false;

  if (use_cloud_input_)
  {
    if (!nh_private_.getParam ("cloud_range_min", cloud_range_min_))
      cloud_range_min_ = 0.1;
    if (!nh_private_.getParam ("cloud_range_max", cloud_range_max_))
      cloud_range_max_ = 50.0;
    if (!nh_private_.getParam ("cloud_res", cloud_res_))
      cloud_res_ = 0.05;

    input_.min_reading = cloud_range_min_;
    input_.max_reading = cloud_range_max_;
  }

  // **** keyframe params: when to generate the keyframe scan
  // if either is set to 0, reduces to frame-to-frame matching

  if (!nh_private_.getParam ("kf_dist_linear", kf_dist_linear_))
    kf_dist_linear_ = 0.10;
  if (!nh_private_.getParam ("kf_dist_angular", kf_dist_angular_))
    kf_dist_angular_ = 10.0 * (M_PI / 180.0);

  kf_dist_linear_sq_ = kf_dist_linear_ * kf_dist_linear_;

  // **** What predictions are available to speed up the ICP?
  // 1) imu - [theta] from imu yaw angle - /imu topic
  // 2) odom - [x, y, theta] from wheel odometry - /odom topic
  // 3) vel - [x, y, theta] from velocity predictor - see alpha-beta predictors - /vel topic
  // If more than one is enabled, priority is imu > odom > vel

  if (!nh_private_.getParam ("use_imu", use_imu_))
    use_imu_ = true;
  if (!nh_private_.getParam ("use_odom", use_odom_))
    use_odom_ = true;
  if (!nh_private_.getParam ("use_vel", use_vel_))
    use_vel_ = false;

  // **** Are velocity input messages stamped?
  // if false, will subscribe to Twist msgs on /vel
  // if true, will subscribe to TwistStamped msgs on /vel
  if (!nh_private_.getParam ("stamped_vel", stamped_vel_))
    stamped_vel_ = false;

  // **** How to publish the output?
  // tf (fixed_frame->base_frame),
  // pose message (pose of base frame in the fixed frame)

  if (!nh_private_.getParam ("publish_tf", publish_tf_))
    publish_tf_ = true;
  if (!nh_private_.getParam ("publish_pose", publish_pose_))
    publish_pose_ = true;
  if (!nh_private_.getParam ("publish_pose_stamped", publish_pose_stamped_))
    publish_pose_stamped_ = true;
  if (!nh_private_.getParam ("publish_pose_with_covariance", publish_pose_with_covariance_))
    publish_pose_with_covariance_ = true;
  if (!nh_private_.getParam ("publish_pose_with_covariance_stamped", publish_pose_with_covariance_stamped_))
    publish_pose_with_covariance_stamped_ = true;

  if (!nh_private_.getParam("position_covariance", position_covariance_))
  {
    position_covariance_.resize(3);
    std::fill(position_covariance_.begin(), position_covariance_.end(), 1e-9);
  }

  if (!nh_private_.getParam("orientation_covariance", orientation_covariance_))
  {
    orientation_covariance_.resize(3);
    std::fill(orientation_covariance_.begin(), orientation_covariance_.end(), 1e-9);
  }
  // **** CSM parameters - comments copied from algos.h (by Andrea Censi)

  // Maximum angular displacement between scans
  if (!nh_private_.getParam ("max_angular_correction_deg", input_.max_angular_correction_deg))
    input_.max_angular_correction_deg = 45.0;

  // Maximum translation between scans (m)
  if (!nh_private_.getParam ("max_linear_correction", input_.max_linear_correction))
    input_.max_linear_correction = 0.50;

  // Maximum ICP cycle iterations
  if (!nh_private_.getParam ("max_iterations", input_.max_iterations))
    input_.max_iterations = 10;

  // A threshold for stopping (m)
  if (!nh_private_.getParam ("epsilon_xy", input_.epsilon_xy))
    input_.epsilon_xy = 0.000001;

  // A threshold for stopping (rad)
  if (!nh_private_.getParam ("epsilon_theta", input_.epsilon_theta))
    input_.epsilon_theta = 0.000001;

  // Maximum distance for a correspondence to be valid
  if (!nh_private_.getParam ("max_correspondence_dist", input_.max_correspondence_dist))
    input_.max_correspondence_dist = 0.3;

  // Noise in the scan (m)
  if (!nh_private_.getParam ("sigma", input_.sigma))
    input_.sigma = 0.010;

  // Use smart tricks for finding correspondences.
  if (!nh_private_.getParam ("use_corr_tricks", input_.use_corr_tricks))
    input_.use_corr_tricks = 1;

  // Restart: Restart if error is over threshold
  if (!nh_private_.getParam ("restart", input_.restart))
    input_.restart = 0;

  // Restart: Threshold for restarting
  if (!nh_private_.getParam ("restart_threshold_mean_error", input_.restart_threshold_mean_error))
    input_.restart_threshold_mean_error = 0.01;

  // Restart: displacement for restarting. (m)
  if (!nh_private_.getParam ("restart_dt", input_.restart_dt))
    input_.restart_dt = 1.0;

  // Restart: displacement for restarting. (rad)
  if (!nh_private_.getParam ("restart_dtheta", input_.restart_dtheta))
    input_.restart_dtheta = 0.1;

  // Max distance for staying in the same clustering
  if (!nh_private_.getParam ("clustering_threshold", input_.clustering_threshold))
    input_.clustering_threshold = 0.25;

  // Number of neighbour rays used to estimate the orientation
  if (!nh_private_.getParam ("orientation_neighbourhood", input_.orientation_neighbourhood))
    input_.orientation_neighbourhood = 20;

  // If 0, it's vanilla ICP
  if (!nh_private_.getParam ("use_point_to_line_distance", input_.use_point_to_line_distance))
    input_.use_point_to_line_distance = 1;

  // Discard correspondences based on the angles
  if (!nh_private_.getParam ("do_alpha_test", input_.do_alpha_test))
    input_.do_alpha_test = 0;

  // Discard correspondences based on the angles - threshold angle, in degrees
  if (!nh_private_.getParam ("do_alpha_test_thresholdDeg", input_.do_alpha_test_thresholdDeg))
    input_.do_alpha_test_thresholdDeg = 20.0;

  // Percentage of correspondences to consider: if 0.9,
  // always discard the top 10% of correspondences with more error
  if (!nh_private_.getParam ("outliers_maxPerc", input_.outliers_maxPerc))
    input_.outliers_maxPerc = 0.90;

  // Parameters describing a simple adaptive algorithm for discarding.
  //  1) Order the errors.
  //  2) Choose the percentile according to outliers_adaptive_order.
  //     (if it is 0.7, get the 70% percentile)
  //  3) Define an adaptive threshold multiplying outliers_adaptive_mult
  //     with the value of the error at the chosen percentile.
  //  4) Discard correspondences over the threshold.
  //  This is useful to be conservative; yet remove the biggest errors.
  if (!nh_private_.getParam ("outliers_adaptive_order", input_.outliers_adaptive_order))
    input_.outliers_adaptive_order = 0.7;

  if (!nh_private_.getParam ("outliers_adaptive_mult", input_.outliers_adaptive_mult))
    input_.outliers_adaptive_mult = 2.0;

  // If you already have a guess of the solution, you can compute the polar angle
  // of the points of one scan in the new position. If the polar angle is not a monotone
  // function of the readings index, it means that the surface is not visible in the
  // next position. If it is not visible, then we don't use it for matching.
  if (!nh_private_.getParam ("do_visibility_test", input_.do_visibility_test))
    input_.do_visibility_test = 0;

  // no two points in laser_sens can have the same corr.
  if (!nh_private_.getParam ("outliers_remove_doubles", input_.outliers_remove_doubles))
    input_.outliers_remove_doubles = 1;

  // If 1, computes the covariance of ICP using the method http://purl.org/censi/2006/icpcov
  if (!nh_private_.getParam ("do_compute_covariance", input_.do_compute_covariance))
    input_.do_compute_covariance = 0;

  // Checks that find_correspondences_tricks gives the right answer
  if (!nh_private_.getParam ("debug_verify_tricks", input_.debug_verify_tricks))
    input_.debug_verify_tricks = 0;

  // If 1, the field 'true_alpha' (or 'alpha') in the first scan is used to compute the
  // incidence beta, and the factor (1/cos^2(beta)) used to weight the correspondence.");
  if (!nh_private_.getParam ("use_ml_weights", input_.use_ml_weights))
    input_.use_ml_weights = 0;

  // If 1, the field 'readings_sigma' in the second scan is used to weight the
  // correspondence by 1/sigma^2
  if (!nh_private_.getParam ("use_sigma_weights", input_.use_sigma_weights))
    input_.use_sigma_weights = 0;
}

void LaserScanMatcherMulti::imuCallback(const sensor_msgs::Imu::ConstPtr& imu_msg)
{
  boost::mutex::scoped_lock(mutex_);
  latest_imu_msg_ = *imu_msg;
  if (!received_imu_)
  {
    last_used_imu_msg_ = *imu_msg;
    received_imu_ = true;
  }
}

sensor_msgs::LaserScan::ConstPtr g_eval_scan;

void LaserScanMatcherMulti::eval1Callback(const sensor_msgs::LaserScan::ConstPtr& msg)
{
	g_eval_scan = msg;
}

void LaserScanMatcherMulti::eval2Callback(const sensor_msgs::LaserScan::ConstPtr& msg)
{
	if(!g_eval_scan) return;
	
  LDP scan1, scan2;
  laserScanToLDP(g_eval_scan, scan1);
  laserScanToLDP(msg, scan2);
  
  input_.laser_ref  = scan1;
  input_.laser_sens = scan2;

  input_.min_reading = msg->range_min;
  input_.max_reading = msg->range_max;

  input_.first_guess[0] = 0;
  input_.first_guess[1] = 0;
  input_.first_guess[2] = 0;
  
  // If they are non-Null, free covariance gsl matrices to avoid leaking memory
  if (output_.cov_x_m)
  {
    gsl_matrix_free(output_.cov_x_m);
    output_.cov_x_m = 0;
  }
  if (output_.dx_dy1_m)
  {
    gsl_matrix_free(output_.dx_dy1_m);
    output_.dx_dy1_m = 0;
  }
  if (output_.dx_dy2_m)
  {
    gsl_matrix_free(output_.dx_dy2_m);
    output_.dx_dy2_m = 0;
  }
	gsl_set_error_handler_off();
	try {
  sm_icp(&input_, &output_);
} catch(...) {
	output_.valid = false;
}

  // unstamped Pose2D message
  geometry_msgs::Pose2D::Ptr pose_msg;
  pose_msg = boost::make_shared<geometry_msgs::Pose2D>();
  
  pose_msg->theta = -10000;

  ROS_INFO("output %d", output_.valid);
  if (output_.valid)
  {
    ROS_INFO("res %f %f %f", output_.x[0], output_.x[1], output_.x[2]);
    
    pose_msg->x = output_.x[0];
    pose_msg->y = output_.x[1];
    pose_msg->theta = output_.x[2];
  }
   pose_publisher_.publish(pose_msg);
}


void LaserScanMatcherMulti::odomCallback(const nav_msgs::Odometry::ConstPtr& _odom_msg)
{
  nav_msgs::Odometry odom_msg = *_odom_msg;
  
  if(received_odom_ && correction_T_) {
	const double t = 1./(odom_msg.header.stamp-latest_odom_msg_.header.stamp).toSec();
	  
	odom_msg.twist.twist.linear.x  += t*correction_T_->getOrigin().getX();
	odom_msg.twist.twist.linear.y  += t*correction_T_->getOrigin().getY();
	odom_msg.twist.twist.angular.z += t*tf::getYaw(correction_T_->getRotation());
	
	ROS_INFO("odom corr. %f %f %f", correction_T_->getOrigin().getX(), correction_T_->getOrigin().getY(), tf::getYaw(correction_T_->getRotation()));
	
	correction_T_.reset();
  }
  odom_publisher_.publish(odom_msg);
  
  boost::mutex::scoped_lock(mutex_);
  latest_odom_msg_ = odom_msg;
  if (!received_odom_)
  {
    last_used_odom_msg_ = odom_msg;
    received_odom_ = true;
  }
}

void LaserScanMatcherMulti::velCallback(const geometry_msgs::Twist::ConstPtr& twist_msg)
{
  boost::mutex::scoped_lock(mutex_);
  latest_vel_msg_ = *twist_msg;

  received_vel_ = true;
}

void LaserScanMatcherMulti::velStmpCallback(const geometry_msgs::TwistStamped::ConstPtr& twist_msg)
{
  boost::mutex::scoped_lock(mutex_);
  latest_vel_msg_ = twist_msg->twist;

  received_vel_ = true;
}

void LaserScanMatcherMulti::cloudCallback (const PointCloudT::ConstPtr& cloud, const ratslam_ros::TopologicalAction::ConstPtr &act_msg)
{
  // **** if first scan, cache the tf from base to the scanner
  Index idx(act_msg->src_x, act_msg->src_y, act_msg->src_th);

  std_msgs::Header cloud_header = pcl_conversions::fromPCL(cloud->header);

  if (!initialized_)
  {
    // cache the static tf from base to laser
    if (!getBaseToLaserTf(cloud_header.frame_id))
    {
      ROS_WARN("Skipping scan");
      return;
    }

    LDP curr_ldp_scan;
    PointCloudToLDP(cloud, curr_ldp_scan);
	prev_ldp_scans_[idx].reset(new ScanMem(curr_ldp_scan, sensor_msgs::LaserScan::ConstPtr()));
	prev_ldp_scans_[idx]->f2b_kf_ = f2b_;
    last_icp_time_ = cloud_header.stamp;
    initialized_ = true;
  }

  LDP curr_ldp_scan;
  PointCloudToLDP(cloud, curr_ldp_scan);
  processScan(curr_ldp_scan, cloud_header.stamp, idx, sensor_msgs::LaserScan::ConstPtr());
}

void LaserScanMatcherMulti::scanCallback (const sensor_msgs::LaserScan::ConstPtr& scan_msg, const ratslam_ros::TopologicalAction::ConstPtr &act_msg)
{
	ROS_INFO("scanCallback id: %d/%d/%d", act_msg->src_x, act_msg->src_y, act_msg->src_th);
  // **** if first scan, cache the tf from base to the scanner
  Index idx(act_msg->src_x, act_msg->src_y, act_msg->src_th);

  if (!initialized_)
  {
    ROS_INFO("!init");
    
    createCache(scan_msg);    // caches the sin and cos of all angles

    // cache the static tf from base to laser
    if (!getBaseToLaserTf(scan_msg->header.frame_id))
    {
      ROS_WARN("Skipping scan");
      return;
    }

    /*LDP curr_ldp_scan;
    laserScanToLDP(scan_msg, curr_ldp_scan);
	prev_ldp_scans_[idx].reset(new ScanMem(curr_ldp_scan, scan_msg));
	prev_ldp_scans_[idx]->f2b_kf_ = f2b_;*/
    last_icp_time_ = scan_msg->header.stamp;
    initialized_ = true;
  }

  LDP curr_ldp_scan;
  laserScanToLDP(scan_msg, curr_ldp_scan);
  processScan(curr_ldp_scan, scan_msg->header.stamp, idx, scan_msg);
}

void LaserScanMatcherMulti::processScan(LDP& curr_ldp_scan, const ros::Time& time, const Index &idx, const sensor_msgs::LaserScan::ConstPtr &cur_ros_scan)
{
  ros::WallTime start = ros::WallTime::now();

  // CSM is used in the following way:
  // The scans are always in the laser frame
  // The reference scan (prevLDPcan_) has a pose of [0, 0, 0]
  // The new scan (currLDPScan) has a pose equal to the movement
  // of the laser in the laser frame since the last scan
  // The computed correction is then propagated using the tf machinery

  if(prev_ldp_scans_.find(idx)==prev_ldp_scans_.end()) {
	  prev_ldp_scans_[idx].reset(new ScanMem(curr_ldp_scan, cur_ros_scan));
	  prev_ldp_scans_[idx]->f2b_kf_ = f2b_;
	  
		  tf::StampedTransform T;
	  {
		  try
		  {
			tf_listener_.waitForTransform(
			  cur_ros_scan->header.frame_id, "odom", cur_ros_scan->header.stamp, ros::Duration(1.0));
			tf_listener_.lookupTransform (
			  cur_ros_scan->header.frame_id, "odom", cur_ros_scan->header.stamp, T);
    ROS_INFO("tf capt. %f %f %f at %f (%f)", T.getOrigin().getX(), T.getOrigin().getY(), tf::getYaw(T.getRotation()), T.stamp_.toSec(), cur_ros_scan->header.stamp.toSec());
		  prev_ldp_scans_[idx]->f2b_kf_ = T;
		  }
		  catch (tf::TransformException ex)
		  {
			ROS_WARN("Could not get initial transform from base to laser frame, %s", ex.what());
		  }
	  }
	  
	  prev_ldp_scans_[idx]->scan_->odometry[0] = 0.0;
	  prev_ldp_scans_[idx]->scan_->odometry[1] = 0.0;
	  prev_ldp_scans_[idx]->scan_->odometry[2] = 0.0;

	  prev_ldp_scans_[idx]->scan_->estimate[0] = 0.0;
	  prev_ldp_scans_[idx]->scan_->estimate[1] = 0.0;
	  prev_ldp_scans_[idx]->scan_->estimate[2] = 0.0;

	  prev_ldp_scans_[idx]->scan_->true_pose[0] = 0.0;
	  prev_ldp_scans_[idx]->scan_->true_pose[1] = 0.0;
	  prev_ldp_scans_[idx]->scan_->true_pose[2] = 0.0;
	  
	  //ROS_INFO("new idx");
	  return;
  }

  input_.laser_ref  = prev_ldp_scans_[idx]->scan_;
  input_.laser_sens = curr_ldp_scan;

  // **** estimated change since last scan

  double dt = (time - last_icp_time_).toSec();
  double pr_ch_x, pr_ch_y, pr_ch_a;
  getPrediction(pr_ch_x, pr_ch_y, pr_ch_a, dt);
  ROS_INFO("pred %f %f %f", pr_ch_x, pr_ch_y, pr_ch_a);
  
  tf::Transform pr_ch, pr_O;
  {
	  tf::StampedTransform T;
	  try
	  {
			tf_listener_.waitForTransform(
			  cur_ros_scan->header.frame_id, "odom", cur_ros_scan->header.stamp, ros::Duration(1.0));
			tf_listener_.lookupTransform (
			  cur_ros_scan->header.frame_id, "odom", cur_ros_scan->header.stamp, T);
	  }
	  catch (tf::TransformException ex)
	  {
		ROS_WARN("Could not get initial transform from base to laser frame, %s", ex.what());
	  }
    ROS_INFO("tf_list %f %f %f at %f (%f)", T.getOrigin().getX(), T.getOrigin().getY(), tf::getYaw(T.getRotation()), T.stamp_.toSec(), cur_ros_scan->header.stamp.toSec());
    
    pr_O = pr_ch = T;
    pr_ch_x = T.getOrigin().getX();
    pr_ch_y = T.getOrigin().getY();
    pr_ch_a = tf::getYaw(T.getRotation());
	}

  // the predicted change of the laser's position, in the fixed frame

  //createTfFromXYTheta(pr_ch_x, pr_ch_y, pr_ch_a, pr_ch);

  // account for the change since the last kf, in the fixed frame

  //pr_ch = pr_ch * (f2b_ * prev_ldp_scans_[idx]->f2b_kf_.inverse());
  //pr_ch = (pr_ch.inverse()*prev_ldp_scans_[idx]->f2b_kf_);//.inverse();
  //pr_ch = (prev_ldp_scans_[idx]->f2b_kf_.inverse()*pr_ch);//.inverse();
  pr_ch = (pr_O*prev_ldp_scans_[idx]->f2b_kf_.inverse());//.inverse();

  // the predicted change of the laser's position, in the laser frame
  
  ROS_INFO("rotate %f around (%f/%f/%f)", laser_to_base_.getRotation().getAngle(), laser_to_base_.getRotation().getAxis().getX()
										, laser_to_base_.getRotation().getAxis().getY(), laser_to_base_.getRotation().getAxis().getZ());

  tf::Transform pr_ch_l;
  //pr_ch_l = laser_to_base_ * f2b_.inverse() * pr_ch * f2b_ * base_to_laser_ ;
  //pr_ch_l = laser_to_base_ * f2b_.inverse() * pr_ch * f2b_ * base_to_laser_ ;
  //pr_ch_l = laser_to_base_ * pr_ch * base_to_laser_ ;
  //pr_ch_l = pr_ch;
  pr_ch_l = laser_to_base_ * pr_ch * base_to_laser_ ;
  //pr_ch_l = laser_to_base_ * pr_O.inverse() * pr_ch * pr_O * base_to_laser_ ;

  input_.first_guess[0] = pr_ch_l.getOrigin().getX();
  input_.first_guess[1] = pr_ch_l.getOrigin().getY();
  input_.first_guess[2] = tf::getYaw(pr_ch_l.getRotation());
  
  /*input_.first_guess[0] = pr_ch.getOrigin().getX();
  input_.first_guess[1] = pr_ch.getOrigin().getY();
  input_.first_guess[2] = tf::getYaw(pr_ch.getRotation());*/
  
  ROS_INFO("tf last %f %f %f", prev_ldp_scans_[idx]->f2b_kf_.getOrigin().getX(), prev_ldp_scans_[idx]->f2b_kf_.getOrigin().getY(), tf::getYaw(prev_ldp_scans_[idx]->f2b_kf_.getRotation()));
  ROS_INFO("first_guess %f %f %f", pr_ch.getOrigin().getX(), pr_ch.getOrigin().getY(), tf::getYaw(pr_ch.getRotation()));
  ROS_INFO("first_guess %f %f %f", input_.first_guess[0], input_.first_guess[1], input_.first_guess[2]);

  // If they are non-Null, free covariance gsl matrices to avoid leaking memory
  if (output_.cov_x_m)
  {
    gsl_matrix_free(output_.cov_x_m);
    output_.cov_x_m = 0;
  }
  if (output_.dx_dy1_m)
  {
    gsl_matrix_free(output_.dx_dy1_m);
    output_.dx_dy1_m = 0;
  }
  if (output_.dx_dy2_m)
  {
    gsl_matrix_free(output_.dx_dy2_m);
    output_.dx_dy2_m = 0;
  }

  // *** scan match - using point to line icp from CSM

  sm_icp(&input_, &output_);
  tf::Transform corr_ch;

  ROS_INFO("output %d", output_.valid);
  if (output_.valid)
  {

    // the correction of the laser's position, in the laser frame
    tf::Transform corr_ch_l;
    createTfFromXYTheta(output_.x[0], output_.x[1], output_.x[2], corr_ch_l);

    // the correction of the base's position, in the base frame
    //corr_ch = pr_O * base_to_laser_ * pr_ch_l * laser_to_base_ * pr_O.inverse();
    corr_ch = base_to_laser_ * pr_ch_l * laser_to_base_;
    ROS_INFO("correction %f %f %f", corr_ch.getOrigin().getX(), corr_ch.getOrigin().getY(), tf::getYaw(corr_ch.getRotation()));
    //corr_ch = corr_ch_l;
    //corr_ch = pr_O * base_to_laser_ * corr_ch_l * laser_to_base_ * pr_O.inverse();
    corr_ch = base_to_laser_ * corr_ch_l * laser_to_base_;
    
    {
    	sensor_msgs::LaserScan m1, m2;
    	m1 = *prev_ldp_scans_[idx]->ros_scan_;
    	m2 = *cur_ros_scan;
    
        tf::StampedTransform transform_msg (pr_ch_l, time, "dbg_ref", "dbg_inp");
        
		tf::Transform T = pr_ch.inverse()*corr_ch;
		
        //{tf::StampedTransform transform_msg (pr_O.inverse(), time, "odom", "dbg_inp");
		{tf::StampedTransform transform_msg ((corr_ch*prev_ldp_scans_[idx]->f2b_kf_).inverse(), time, "odom", "dbg_inp");
        tf_broadcaster_.sendTransform (transform_msg);}
        ros::spinOnce();
		{tf::StampedTransform transform_msg ((pr_O).inverse(), time, "odom", "dbg_inpo");
        tf_broadcaster_.sendTransform (transform_msg);}
        ros::spinOnce();
        //{tf::StampedTransform transform_msg ((prev_ldp_scans_[idx]->f2b_kf_*T).inverse(), time, "odom", "dbg_ref");
        {tf::StampedTransform transform_msg ((prev_ldp_scans_[idx]->f2b_kf_).inverse(), time, "odom", "dbg_ref");
        tf_broadcaster_.sendTransform (transform_msg);}
        
        ros::spinOnce();
        
    	
    	m1.header.frame_id = "dbg_ref";
    	m2.header.frame_id = "dbg_inp";
    	m1.header.stamp = time;
    	m2.header.stamp = time;
    	sensor_msgs::LaserScan m3=m2;
    	m3.header.frame_id = "dbg_inpo";
    	dbg_scan_publisher_[0].publish(m1);
    	dbg_scan_publisher_[1].publish(m2);
    	dbg_scan_publisher_[2].publish(m3);
    }

    // update the pose in the world frame
    f2b_ = prev_ldp_scans_[idx]->f2b_kf_ * corr_ch;
    
    tf::Transform  tmp = pr_ch.inverse()*corr_ch_l;
    ROS_INFO("correction %f %f %f", corr_ch.getOrigin().getX(), corr_ch.getOrigin().getY(), tf::getYaw(corr_ch.getRotation()));
    ROS_INFO("correction %f %f %f", tmp.getOrigin().getX(), tmp.getOrigin().getY(), tf::getYaw(tmp.getRotation()));

    // **** publish
    
    if(odom_publisher_.getNumSubscribers()>0 && (std::abs(corr_ch.getOrigin().getX())>0.01f||std::abs(corr_ch.getOrigin().getY())>0.01f||std::abs(tf::getYaw(corr_ch.getRotation()))>0.01f) ) {
	  tf::StampedTransform T;
	  try
	  {
			tf_listener_.waitForTransform(
			  "base_link", "odom", cur_ros_scan->header.stamp, ros::Duration(1.0));
			tf_listener_.lookupTransform (
			  "base_link", "odom", cur_ros_scan->header.stamp, T);
	    //T.setOrigin(tf::Vector3(0,0,0));
		correction_T_.reset(new tf::Transform(T*(pr_ch.inverse()*corr_ch)*T.inverse()));
	  }
	  catch (tf::TransformException ex)
	  {
		ROS_WARN("Could not get initial transform from base to laser frame, %s", ex.what());
	  }
	}

    if (publish_pose_)
    {
      // unstamped Pose2D message
      geometry_msgs::Pose2D::Ptr pose_msg;
      pose_msg = boost::make_shared<geometry_msgs::Pose2D>();
      pose_msg->x = f2b_.getOrigin().getX();
      pose_msg->y = f2b_.getOrigin().getY();
      pose_msg->theta = tf::getYaw(f2b_.getRotation());
      pose_publisher_.publish(pose_msg);
    }
    if (publish_pose_stamped_)
    {
      // stamped Pose message
      geometry_msgs::PoseStamped::Ptr pose_stamped_msg;
      pose_stamped_msg = boost::make_shared<geometry_msgs::PoseStamped>();

      pose_stamped_msg->header.stamp    = time;
      pose_stamped_msg->header.frame_id = fixed_frame_;

      tf::poseTFToMsg(f2b_, pose_stamped_msg->pose);

      pose_stamped_publisher_.publish(pose_stamped_msg);
    }
    if (publish_pose_with_covariance_)
    {
      // unstamped PoseWithCovariance message
      geometry_msgs::PoseWithCovariance::Ptr pose_with_covariance_msg;
      pose_with_covariance_msg = boost::make_shared<geometry_msgs::PoseWithCovariance>();
      tf::poseTFToMsg(f2b_, pose_with_covariance_msg->pose);

      if (input_.do_compute_covariance)
      {
        pose_with_covariance_msg->covariance = boost::assign::list_of
          (gsl_matrix_get(output_.cov_x_m, 0, 0)) (0)  (0)  (0)  (0)  (0)
          (0)  (gsl_matrix_get(output_.cov_x_m, 0, 1)) (0)  (0)  (0)  (0)
          (0)  (0)  (static_cast<double>(position_covariance_[2])) (0)  (0)  (0)
          (0)  (0)  (0)  (static_cast<double>(orientation_covariance_[0])) (0)  (0)
          (0)  (0)  (0)  (0)  (static_cast<double>(orientation_covariance_[1])) (0)
          (0)  (0)  (0)  (0)  (0)  (gsl_matrix_get(output_.cov_x_m, 0, 2));
      }
      else
      {
        pose_with_covariance_msg->covariance = boost::assign::list_of
          (static_cast<double>(position_covariance_[0])) (0)  (0)  (0)  (0)  (0)
          (0)  (static_cast<double>(position_covariance_[1])) (0)  (0)  (0)  (0)
          (0)  (0)  (static_cast<double>(position_covariance_[2])) (0)  (0)  (0)
          (0)  (0)  (0)  (static_cast<double>(orientation_covariance_[0])) (0)  (0)
          (0)  (0)  (0)  (0)  (static_cast<double>(orientation_covariance_[1])) (0)
          (0)  (0)  (0)  (0)  (0)  (static_cast<double>(orientation_covariance_[2]));
      }

      pose_with_covariance_publisher_.publish(pose_with_covariance_msg);
    }
    if (publish_pose_with_covariance_stamped_)
    {
      // stamped Pose message
      geometry_msgs::PoseWithCovarianceStamped::Ptr pose_with_covariance_stamped_msg;
      pose_with_covariance_stamped_msg = boost::make_shared<geometry_msgs::PoseWithCovarianceStamped>();

      pose_with_covariance_stamped_msg->header.stamp    = time;
      pose_with_covariance_stamped_msg->header.frame_id = fixed_frame_;

      tf::poseTFToMsg(f2b_, pose_with_covariance_stamped_msg->pose.pose);

      if (input_.do_compute_covariance)
      {
        pose_with_covariance_stamped_msg->pose.covariance = boost::assign::list_of
          (gsl_matrix_get(output_.cov_x_m, 0, 0)) (0)  (0)  (0)  (0)  (0)
          (0)  (gsl_matrix_get(output_.cov_x_m, 0, 1)) (0)  (0)  (0)  (0)
          (0)  (0)  (static_cast<double>(position_covariance_[2])) (0)  (0)  (0)
          (0)  (0)  (0)  (static_cast<double>(orientation_covariance_[0])) (0)  (0)
          (0)  (0)  (0)  (0)  (static_cast<double>(orientation_covariance_[1])) (0)
          (0)  (0)  (0)  (0)  (0)  (gsl_matrix_get(output_.cov_x_m, 0, 2));
      }
      else
      {
        pose_with_covariance_stamped_msg->pose.covariance = boost::assign::list_of
          (static_cast<double>(position_covariance_[0])) (0)  (0)  (0)  (0)  (0)
          (0)  (static_cast<double>(position_covariance_[1])) (0)  (0)  (0)  (0)
          (0)  (0)  (static_cast<double>(position_covariance_[2])) (0)  (0)  (0)
          (0)  (0)  (0)  (static_cast<double>(orientation_covariance_[0])) (0)  (0)
          (0)  (0)  (0)  (0)  (static_cast<double>(orientation_covariance_[1])) (0)
          (0)  (0)  (0)  (0)  (0)  (static_cast<double>(orientation_covariance_[2]));
      }

      pose_with_covariance_stamped_publisher_.publish(pose_with_covariance_stamped_msg);
    }

    if (publish_tf_)
    {
      tf::StampedTransform transform_msg (f2b_, time, fixed_frame_, base_frame_);
      tf_broadcaster_.sendTransform (transform_msg);
    }
  }
  else
  {
    corr_ch.setIdentity();
    ROS_WARN("Error in scan matching");
  }

  // **** swap old and new

  /*if (newKeyframeNeeded(corr_ch))
  {
    // generate a keyframe
    ld_free(prev_ldp_scan_);
    prev_ldp_scan_ = curr_ldp_scan;
    f2b_kf_ = f2b_;
  }
  else*/
  {
    ld_free(curr_ldp_scan);
  }

  last_icp_time_ = time;

  // **** statistics

  double dur = (ros::WallTime::now() - start).toSec() * 1e3;
  ROS_DEBUG("Scan matcher total duration: %.1f ms", dur);
}

bool LaserScanMatcherMulti::newKeyframeNeeded(const tf::Transform& d)
{
  if (fabs(tf::getYaw(d.getRotation())) > kf_dist_angular_) return true;

  double x = d.getOrigin().getX();
  double y = d.getOrigin().getY();
  if (x*x + y*y > kf_dist_linear_sq_) return true;

  return false;
}

void LaserScanMatcherMulti::PointCloudToLDP(const PointCloudT::ConstPtr& cloud,
                                             LDP& ldp)
{
  double max_d2 = cloud_res_ * cloud_res_;

  PointCloudT cloud_f;

  cloud_f.points.push_back(cloud->points[0]);

  for (unsigned int i = 1; i < cloud->points.size(); ++i)
  {
    const PointT& pa = cloud_f.points[cloud_f.points.size() - 1];
    const PointT& pb = cloud->points[i];

    double dx = pa.x - pb.x;
    double dy = pa.y - pb.y;
    double d2 = dx*dx + dy*dy;

    if (d2 > max_d2)
    {
      cloud_f.points.push_back(pb);
    }
  }

  unsigned int n = cloud_f.points.size();

  ldp = ld_alloc_new(n);

  for (unsigned int i = 0; i < n; i++)
  {
    // calculate position in laser frame
    if (is_nan(cloud_f.points[i].x) || is_nan(cloud_f.points[i].y))
    {
      ROS_WARN("Laser Scan Matcher: Cloud input contains NaN values. \
                Please use a filtered cloud input.");
    }
    else
    {
      double r = sqrt(cloud_f.points[i].x * cloud_f.points[i].x +
                      cloud_f.points[i].y * cloud_f.points[i].y);

      if (r > cloud_range_min_ && r < cloud_range_max_)
      {
        ldp->valid[i] = 1;
        ldp->readings[i] = r;
      }
      else
      {
        ldp->valid[i] = 0;
        ldp->readings[i] = -1;  // for invalid range
      }
    }

    ldp->theta[i] = atan2(cloud_f.points[i].y, cloud_f.points[i].x);
    ldp->cluster[i]  = -1;
  }

  ldp->min_theta = ldp->theta[0];
  ldp->max_theta = ldp->theta[n-1];

  ldp->odometry[0] = 0.0;
  ldp->odometry[1] = 0.0;
  ldp->odometry[2] = 0.0;

  ldp->true_pose[0] = 0.0;
  ldp->true_pose[1] = 0.0;
  ldp->true_pose[2] = 0.0;
}

void LaserScanMatcherMulti::laserScanToLDP(const sensor_msgs::LaserScan::ConstPtr& scan_msg,
                                            LDP& ldp)
{
  unsigned int n = scan_msg->ranges.size();
  ldp = ld_alloc_new(n);

  for (unsigned int i = 0; i < n; i++)
  {
    // calculate position in laser frame

    double r = scan_msg->ranges[i];

    if (r > scan_msg->range_min && r < scan_msg->range_max)
    {
      // fill in laser scan data

      ldp->valid[i] = 1;
      ldp->readings[i] = r;
    }
    else
    {
      ldp->valid[i] = 0;
      ldp->readings[i] = -1;  // for invalid range
    }

    ldp->theta[i]    = scan_msg->angle_min + i * scan_msg->angle_increment;

    ldp->cluster[i]  = -1;
  }

  ldp->min_theta = ldp->theta[0];
  ldp->max_theta = ldp->theta[n-1];

  ldp->odometry[0] = 0.0;
  ldp->odometry[1] = 0.0;
  ldp->odometry[2] = 0.0;

  ldp->true_pose[0] = 0.0;
  ldp->true_pose[1] = 0.0;
  ldp->true_pose[2] = 0.0;
}

void LaserScanMatcherMulti::createCache (const sensor_msgs::LaserScan::ConstPtr& scan_msg)
{
  a_cos_.clear();
  a_sin_.clear();

  for (unsigned int i = 0; i < scan_msg->ranges.size(); ++i)
  {
    double angle = scan_msg->angle_min + i * scan_msg->angle_increment;
    a_cos_.push_back(cos(angle));
    a_sin_.push_back(sin(angle));
  }

  input_.min_reading = scan_msg->range_min;
  input_.max_reading = scan_msg->range_max;
}

bool LaserScanMatcherMulti::getBaseToLaserTf (const std::string& frame_id)
{
  ros::Time t = ros::Time::now();

  tf::StampedTransform base_to_laser_tf;
  try
  {
    tf_listener_.waitForTransform(
      base_frame_, frame_id, t, ros::Duration(1.0));
    tf_listener_.lookupTransform (
      base_frame_, frame_id, t, base_to_laser_tf);
  }
  catch (tf::TransformException ex)
  {
    ROS_WARN("Could not get initial transform from base to laser frame, %s", ex.what());
    return false;
  }
  base_to_laser_ = base_to_laser_tf;
  laser_to_base_ = base_to_laser_.inverse();

  return true;
}

// returns the predicted change in pose (in fixed frame)
// since the last time we did icp
void LaserScanMatcherMulti::getPrediction(double& pr_ch_x, double& pr_ch_y,
                                     double& pr_ch_a, double dt)
{
  boost::mutex::scoped_lock(mutex_);

  // **** base case - no input available, use zero-motion model
  pr_ch_x = 0.0;
  pr_ch_y = 0.0;
  pr_ch_a = 0.0;

  // **** use velocity (for example from ab-filter)
  if (use_vel_)
  {
    pr_ch_x = dt * latest_vel_msg_.linear.x;
    pr_ch_y = dt * latest_vel_msg_.linear.y;
    pr_ch_a = dt * latest_vel_msg_.angular.z;

    if      (pr_ch_a >= M_PI) pr_ch_a -= 2.0 * M_PI;
    else if (pr_ch_a < -M_PI) pr_ch_a += 2.0 * M_PI;
  }

  // **** use wheel odometry
  if (use_odom_ && received_odom_)
  {
    pr_ch_x = latest_odom_msg_.pose.pose.position.x -
              last_used_odom_msg_.pose.pose.position.x;

    pr_ch_y = latest_odom_msg_.pose.pose.position.y -
              last_used_odom_msg_.pose.pose.position.y;

    pr_ch_a = tf::getYaw(latest_odom_msg_.pose.pose.orientation) -
              tf::getYaw(last_used_odom_msg_.pose.pose.orientation);

    if      (pr_ch_a >= M_PI) pr_ch_a -= 2.0 * M_PI;
    else if (pr_ch_a < -M_PI) pr_ch_a += 2.0 * M_PI;

    last_used_odom_msg_ = latest_odom_msg_;
  }

  // **** use imu
  if (use_imu_ && received_imu_)
  {
    pr_ch_a = tf::getYaw(latest_imu_msg_.orientation) -
              tf::getYaw(last_used_imu_msg_.orientation);

    if      (pr_ch_a >= M_PI) pr_ch_a -= 2.0 * M_PI;
    else if (pr_ch_a < -M_PI) pr_ch_a += 2.0 * M_PI;

    last_used_imu_msg_ = latest_imu_msg_;
  }
}

void LaserScanMatcherMulti::createTfFromXYTheta(
  double x, double y, double theta, tf::Transform& t)
{
  t.setOrigin(tf::Vector3(x, y, 0.0));
  tf::Quaternion q;
  q.setRPY(0.0, 0.0, theta);
  t.setRotation(q);
}

} // namespace scan_tools
