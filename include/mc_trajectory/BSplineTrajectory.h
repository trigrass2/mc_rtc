#ifndef _H_BSPLINETRAJECTORY_H_
#define _H_BSPLINETRAJECTORY_H_

#include <Eigen/Core>
#include <unsupported/Eigen/Splines>

namespace mc_trajectory
{

typedef Eigen::Spline<double, 3, Eigen::Dynamic> Spline3d;
typedef Eigen::Matrix<double, 3, Eigen::Dynamic> Matrix3Xd;

struct BSplineTrajectory
{
public:
  BSplineTrajectory(const std::vector<Eigen::Vector3d> & controlPoints, double duration, unsigned int order = 4);

  std::vector<Eigen::Vector3d> splev(const std::vector<double> & t, unsigned int der = 0);
private:
  double duration;
  unsigned int p;
  Eigen::Matrix3Xd P;
  Eigen::VectorXd knot;
  Spline3d spline;
};

}

#endif
