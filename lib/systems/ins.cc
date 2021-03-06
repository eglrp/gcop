#include <limits>
#include "ins.h"
#include "insmanifold.h"
#include <iostream>
#include <assert.h>

using namespace gcop;
using namespace Eigen;


Ins::Ins(bool useAcc) : System<InsState, 15, 6>(InsManifold::Instance())
{
  sv = 3*1e-3;
  su  = 3*1e-8;
  sa = 3*1e-12;
  sra = 0.05;

  g0 << 0, 0, 9.80665;

  semiImplicit = true;
  useAcc_ = useAcc;
}


Ins::~Ins()
{
}

double Ins::Step(InsState &xb, double t, const InsState &xa, 
                 const Vector6d &u, double dt, const VectorXd *p,
                 Matrix15d *A, Matrix15x6d *B, Matrix15Xd *C) {
  
  SO3 &so3 = SO3::Instance();
  
  Vector3d w = u.head<3>() - xa.bg;             // corrected angular velocity
  Vector3d a = u.tail<3>() - xa.ba;             // corrected acceleration
  
  Matrix3d dR;
  so3.exp(dR, dt*w);

  xb.R = xa.R*dR;
  xb.bg = xa.bg;
  xb.ba = xa.ba;

  if(useAcc_)
    xb.v = xa.v + dt*(xa.R*a - g0);
  else
    xb.v = xa.v;

  if (semiImplicit)
    xb.p = xa.p + dt*xb.v;
  else
    xb.p = xa.p + dt*xa.v;

  // jacobian
  if (A) {
    Matrix3d D;
    so3.dexp(D, -dt*w);  

    A->setIdentity();
    A->topLeftCorner<3,3>() = dR.transpose();
    A->block<3,3>(0,3) = -dt*D;           // dR wrt R (trivialized)

    SO3::Instance().hat(D, a);

    if (semiImplicit && useAcc_)
    {
      double dt2 = dt*dt;
      
      A->block<3,3>(9,0) = -dt2*(xa.R*D);   // dp wrt R
      A->block<3,3>(9,6) = -dt2*xa.R;       // dp wrt a
    }

    A->block<3,3>(9,12) = dt*Matrix3d::Identity();  // dp drt v

    if(useAcc_)
      A->block<3,3>(12,0) = -dt*(xa.R*D);   // dv wrt R

    A->block<3,3>(12,6) = -dt*xa.R;       // dv wrt a

//    std::cout<<"In step A:\n"<<*A<<endl;
  }
  return 0;
}

bool Ins::Noise(Matrix15d &Q, double t, const InsState &x, const Vector6d &u, 
                double dt, const VectorXd *p) {

  Q.setZero();
  double dt2 = dt*dt;
  double dt3 = dt2*dt;
  double su2 = su*su;

  Q.topLeftCorner<3,3>().diagonal().setConstant(sv*sv*dt + su2*dt3/3);
  Q.block<3,3>(0,3).diagonal().setConstant(-su2*dt2/2);
  Q.block<3,3>(3,0).diagonal().setConstant(-su2*dt2/2);
  Q.block<3,3>(3,3).diagonal().setConstant(su2*dt);
  Q.block<3,3>(6,6).diagonal().setConstant(sa*sa*dt);

  double sra2 = sra*sra;

  Q.block<3,3>(9,9).diagonal().setConstant(sra2*dt3/3);
  Q.block<3,3>(9,12).diagonal().setConstant(-sra2*dt2/2);
  Q.block<3,3>(12,9).diagonal().setConstant(-sra2*dt2/2);
  Q.block<3,3>(12,12).diagonal().setConstant(sra2*dt);

  return true;
}
