#pragma once

#include <mc_tasks/SplineTrajectoryTask.h>

namespace mc_tasks
{
template<typename Derived>
SplineTrajectoryTask<Derived>::SplineTrajectoryTask(const mc_rbdyn::Robots & robots,
                                                    unsigned int robotIndex,
                                                    const std::string & surfaceName_,
                                                    double duration_,
                                                    double stiffness,
                                                    double posW,
                                                    double oriW,
                                                    const std::vector<std::pair<double, Eigen::Matrix3d>> & oriWp)
: TrajectoryTaskGeneric<tasks::qp::TransformTask>(robots, robotIndex, stiffness, posW), rIndex_(robotIndex),
  surfaceName_(surfaceName_), duration_(duration_)
{
  const mc_rbdyn::Robot & robot = robots.robot(robotIndex);
  const auto & surface = robot.surface(surfaceName_);
  type_ = "trajectory";
  name_ = "trajectory_" + robot.name() + "_" + surface.name();

  finalize(robots.mbs(), static_cast<int>(robotIndex), surface.bodyName(), surface.X_0_s(robot), surface.X_b_s());
  posWeight(posW);
  oriWeight(oriW);
  oriWaypoints(oriWp);
}

template<typename Derived>
void SplineTrajectoryTask<Derived>::update()
{
  auto & spline = static_cast<Derived &>(*this).spline();
  spline.samplingPoints(samples_);

  // Interpolate position
  auto res = spline.splev({currTime_}, 2);
  Eigen::Vector3d & pos = res[0][0];
  Eigen::Vector3d & vel = res[0][1];
  Eigen::Vector3d & acc = res[0][2];

  // Interpolate orientation
  Eigen::Matrix3d ori_target = oriSpline_->eval(currTime_);
  sva::PTransformd target(ori_target, pos);

  // Set the trajectory tracking task targets from the trajectory.
  Eigen::VectorXd refVel(6);
  Eigen::VectorXd refAcc(6);
  refVel.head<3>() = Eigen::Vector3d::Zero();
  refVel.tail<3>() = vel;
  refAcc.head<3>() = Eigen::Vector3d::Zero();
  refAcc.tail<3>() = acc;
  this->refVel(refVel);
  this->refAccel(refAcc);
  this->refPose(target);

  currTime_ = std::min(currTime_ + timeStep_, duration_);
}

template<typename Derived>
void SplineTrajectoryTask<Derived>::oriWaypoints(const std::vector<std::pair<double, Eigen::Matrix3d>> & oriWp)
{
  const auto & robot = robots.robot(rIndex_);
  const auto & X_0_s = robot.surface(surfaceName_).X_0_s(robot);
  oriWp_.push_back(std::make_pair(0., X_0_s.rotation()));
  for(const auto & wp : oriWp)
  {
    oriWp_.push_back(wp);
  }
  oriWp_.push_back(std::make_pair(duration_, finalTarget_.rotation()));
  oriSpline_.reset(new mc_trajectory::InterpolatedRotation(oriWp_));
}

template<typename Derived>
void SplineTrajectoryTask<Derived>::posWeight(const double posWeight)
{
  auto dimWeight = trajectoryT_->dimWeight();
  dimWeight.tail(3).setConstant(posWeight);
  trajectoryT_->dimWeight(dimWeight);
}

template<typename Derived>
double SplineTrajectoryTask<Derived>::posWeight() const
{
  return trajectoryT_->dimWeight()(3);
}

template<typename Derived>
void SplineTrajectoryTask<Derived>::oriWeight(const double oriWeight)
{
  auto dimWeight = trajectoryT_->dimWeight();
  dimWeight.head(3).setConstant(oriWeight);
  trajectoryT_->dimWeight(dimWeight);
}

template<typename Derived>
double SplineTrajectoryTask<Derived>::oriWeight() const
{
  return trajectoryT_->dimWeight()(0);
}

template<typename Derived>
void SplineTrajectoryTask<Derived>::target(const sva::PTransformd & target)
{
  finalTarget_ = target;
  auto & derived = static_cast<Derived &>(*this);
  derived.target(target);
  oriSpline_->waypoints().back().second = target.rotation();
}

template<typename Derived>
const sva::PTransformd & SplineTrajectoryTask<Derived>::target() const
{
  return finalTarget_;
}

template<typename Derived>
Eigen::VectorXd SplineTrajectoryTask<Derived>::eval() const
{
  const auto & robot = robots.robot(rIndex_);
  sva::PTransformd X_0_s = robot.surface(surfaceName_).X_0_s(robot);
  return sva::transformError(X_0_s, finalTarget_).vector();
}

template<typename Derived>
Eigen::VectorXd SplineTrajectoryTask<Derived>::evalTracking() const
{
  return TrajectoryBase::eval();
}

template<typename Derived>
bool SplineTrajectoryTask<Derived>::timeElapsed()
{
  return currTime_ >= duration_;
}

template<typename Derived>
void SplineTrajectoryTask<Derived>::displaySamples(unsigned s)
{
  samples_ = s;
}

template<typename Derived>
unsigned SplineTrajectoryTask<Derived>::displaySamples() const
{
  return samples_;
}

template<typename Derived>
void SplineTrajectoryTask<Derived>::refPose(const sva::PTransformd & target)
{
  errorT->target(target);
}

template<typename Derived>
const sva::PTransformd & SplineTrajectoryTask<Derived>::refPose() const
{
  return errorT->target();
}

template<typename Derived>
void SplineTrajectoryTask<Derived>::addToSolver(mc_solver::QPSolver & solver)
{
  if(!inSolver_)
  {
    timeStep_ = solver.dt();
  }
  TrajectoryBase::addToSolver(solver);
}

template<typename Derived>
void SplineTrajectoryTask<Derived>::addToLogger(mc_rtc::Logger & logger)
{
  logger.addLogEntry(name_ + "_surface_pose", [this]() {
    const auto & robot = robots.robot(rIndex_);
    return robot.surface(surfaceName_).X_0_s(robot);
  });
  logger.addLogEntry(name_ + "_target_pose", [this]() { return this->finalTarget_; });
  logger.addLogEntry(name_ + "_speed", [this]() { return sva::MotionVecd(this->speed()); });
}

template<typename Derived>
void SplineTrajectoryTask<Derived>::removeFromLogger(mc_rtc::Logger & logger)
{
  logger.removeLogEntry(name_ + "_target_pose");
  logger.removeLogEntry(name_ + "_speed");
}

template<typename Derived>
void SplineTrajectoryTask<Derived>::addToGUI(mc_rtc::gui::StateBuilder & gui)
{
  LOG_INFO("SplineTrajectoryTask::addToGUI");
  TrajectoryTask::addToGUI(gui);

  gui.addElement({"Tasks", name_, "Gains"},
                 mc_rtc::gui::NumberInput("posWeight", [this]() { return this->posWeight(); },
                                          [this](const double & d) { this->posWeight(d); }),
                 mc_rtc::gui::NumberInput("oriWeight", [this]() { return this->oriWeight(); },
                                          [this](const double & g) { this->oriWeight(g); }));

  auto & spline = static_cast<Derived &>(*this).spline();
  gui.addElement({"Tasks", name_}, mc_rtc::gui::Transform("Surface pose", [this]() {
                   return robots.robot(rIndex_).surface(surfaceName_).X_0_s(robots.robot(rIndex_));
                 }));

  gui.addElement({"Tasks", name_}, mc_rtc::gui::Transform("Target", [this]() { return finalTarget_; },
                                                          [this](const sva::PTransformd & t) { this->target(t); }));

  // Target is handled independently
  for(unsigned i = 1; i < oriSpline_->waypoints().size() - 1; ++i)
  {
    gui.addElement({"Tasks", name_, "Orientation Waypoint"},
                   mc_rtc::gui::Rotation("Waypoint " + std::to_string(i),
                                         [this, i, &spline]() {
                                           const auto & wp = oriSpline_->waypoints()[i];

                                           // Get position of orientation waypoint along the spline
                                           const auto & res = spline.splev({wp.first}, 2);
                                           const Eigen::Vector3d pos = res[0][0];
                                           return sva::PTransformd(wp.second, pos);
                                         },
                                         [this, i](const Eigen::Quaterniond & ori) {
                                           auto & wp = oriSpline_->waypoints()[i];
                                           wp.second = ori.toRotationMatrix();
                                         }));
  }
}

} // namespace mc_tasks
