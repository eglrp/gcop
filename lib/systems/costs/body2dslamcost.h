#ifndef GCOP_BODY2DSLAMCOST_H
#define GCOP_BODY2DSLAMCOST_H

#include <limits>
#include <iostream>
#include "cost.h"
#include "body2dgraph.h"
#include "body2d.h"

namespace gcop {
  
  using namespace std;
  using namespace Eigen;

  typedef Matrix<double, Dynamic, 6> MatrixX6d;
  typedef Matrix<double, 6, Dynamic> Matrix6Xd;
  
  class Body2dSlamCost : public Cost<Body2dState, 6, 3> {
  public:
    Body2dSlamCost(Body2d<> &sys, double tf, const Body2dGraph &pg);
    
    double Lp(double t, const Body2dState &x, const Vector3d &u, const VectorXd &p,
              Vector6d *Lx = 0, Matrix6d *Lxx = 0,
              Vector3d *Lu = 0, Matrix3d *Luu = 0,
              Matrix63d *Lxu = 0, 
              VectorXd *Lp = 0, MatrixXd *Lpp = 0, MatrixX6d *Lpx = 0);

    const Body2dGraph &pg;
  };  
}

#endif
