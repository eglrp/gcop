#include "dynvisins.h"
#include "utils.h"

using namespace gcop;

/**
 * Standard perspective projection residual error
 */
struct PerspError {
  PerspError(const DynVisIns &vi, const Vector2d &z)
    : vi(vi), z(z) {}
  
  /**
   * @param o_ 3-dim rotation exp coordinates
   * @param p_ 3-dim position
   * @param l 3-dim feature position in 3d
   * @param res 2-dim vector residual
   */
  bool operator()(const double* o_,
                  const double* p_,
                  const double* l_,
                  double* res) const 
  {
    Matrix3d dR;
    SO3::Instance().exp(dR, Vector3d(o_));

    Matrix3d R = dR*vi.Ric;  // camera rotation

    Vector3d p(p_);
    Vector3d r = R.transpose()*(Vector3d(l_) - p);

    Vector2d e = (vi.K*(r/r[2]) - z)/vi.pxStd;

    res[0] = e[0]; res[1] = e[1];
    return true;
  }
  
  static ceres::CostFunction* Create(const DynVisIns &vi, const Vector2d &z) {
    return (new ceres::NumericDiffCostFunction<PerspError, ceres::CENTRAL, 2, 3, 3, 3>(
                                                                                       new PerspError(vi, z)));
  }
  
  const DynVisIns &vi;
  Vector2d z;
};


/**
 * Unit-spherical projection residual error, 
 * with simplified constant spherical unprojected covariance
 */
struct SphError {
  SphError(const DynVisIns &vi, const Vector3d &z)
    : vi(vi), z(z) {}
  
  /**
   * @param o_ 3-dim rotation exp coordinates
   * @param p_ 3-dim position
   * @param l_ 3-dim feature position in 3d
   * @param res 3-dim vector residual
   */
  bool operator()(const double* o_,
                  const double* p_,                  
                  const double* l_,
                  double* res) const 
  {
    Matrix3d dR;  
    SO3::Instance().exp(dR, Vector3d(o_));
    Matrix3d R = dR*vi.Ric;  // camera rotation

    Vector3d r = R.transpose()*(Vector3d(l_) - Vector3d(p_));
    Vector3d e = (r/r.norm()- z)/vi.sphStd;
    res[0] = e[0]; res[1] = e[1]; res[2] = e[2];
    return true;
  }
  
  static ceres::CostFunction* Create(const DynVisIns &vi, const Vector3d &z) {
    return (new ceres::NumericDiffCostFunction<SphError, ceres::CENTRAL, 3, 3, 3, 3>(
                                                                                  new SphError(vi, z)));
  }
  const DynVisIns &vi;
  Vector3d z;
};


/**
 * A basic cubic interpolator
 *
 * Author: Marin Kobilarov
 */
class Cubic {  
public:
  Cubic(const Vector3d &p0, const Vector3d &v0,
        const Vector3d &p1, const Vector3d &v1,
        double dt) : p0(p0), v0(v0), dt(dt) {
    double dt2 = dt*dt;
    Vector3d q1 = p1 - p0 - dt*v0;
    Vector3d q2 = v1 - v0;
    b = 6/dt2*q1 + -2/dt*q2;
    c = -6/(dt2*dt)*q1 + 3/dt2*q2;    
  }
  
  Cubic(const Vector3d &p0, const Vector3d &v0,
        double dt) : p0(p0), v0(v0), dt(dt) {
    b.setZero();
    c.setZero();
  }
  

  bool GetPos(Vector3d &p, double t) {
    if (t > dt)
      return false;
    double t2 = t*t;
    p = p0 + t*v0 + t2/2*b + t2*t/3*c;
    return true;
  }

  bool GetVel(Vector3d &v, double t) {
    if (t > dt)
      return false;
    v = v0 + t*b + t*t*c;
    return true;
  }

  bool GetAcc(Vector3d &a, double t) {
    if (t > dt)
      return false;
    a = b + 2*t*c;
    return true;
  }
  
  /**
   * Get body-fixed angular velocity at time t, assuming exponential parametrization
   * @param body-fixed angular velocity
   * @param t time
   * @return true if success
   */
  bool GetExpVel(Vector3d &w, double t) {
    if (!GetVel(w, t))
      return false;
    Matrix3d D;
    SO3::Instance().dexp(D, -w);
    w = D*w;
    return true;
  }
  
  Vector3d p0, v0;
  double dt;
  Vector3d b, c;
};
        

/**
 * Gyro error, assuming segment is parametrized as a cubic spline
 */
struct GyroCubicError {
  /**
   * @param vi 
   * @param dt delta-t for this segment
   * @param ts local times for each measurement
   * @param ws the sequence of measurements in this segment
   */
  GyroCubicError(const DynVisIns &vi, 
                 double dt,
                 const vector<double> &ts,
                 const vector<Vector3d> &ws)
    : vi(vi), dt(dt), ts(ts), ws(ws)
  {
    assert(dt > 0);
    assert(ts.size() == ws.size());
  }
  
  /**
   * Computes IMU error between two states xa and xb
   * @param wa_ angular velocity at start of segment
   * @param wb_ angular velocity at end of segment
   * @param res residual for all gyro measurements within segment
   */
  bool operator()(const double* ra_,
                  const double* dra_,
                  const double* rb_,
                  const double* drb_,
                  double* res) const 
  {
    Vector3d ra(ra_);
    Vector3d dra(dra_);
    Vector3d rb(rb_);
    Vector3d drb(drb_);

    Cubic cub(ra, dra, rb, drb, dt);
    Vector3d r, dr;
    Matrix3d D;

    for (int i = 0; i < ws.size(); ++i) {      
    
      const double &t = ts[i];
      cub.GetPos(r, t);  // get exp coord
      cub.GetVel(dr, t); // get exp coord vel

      SO3::Instance().dexp(D, -r);
      Vector3d w = D*dr; // the body-fixed angular velocity

      // for now just assume noise is spherical and defined in wStd
      Vector3d e = (w - ws[i] + vi.bg)/vi.wStd;
      memcpy(res + 3*i, e.data(), 3*sizeof(double));
    }
    return true;
  }
  
  static ceres::CostFunction* Create(const DynVisIns &vi, 
                                     double dt,
                                     const vector<double> &ts,
                                     const vector<Vector3d> &ws) {
    return new ceres::NumericDiffCostFunction<GyroCubicError, ceres::CENTRAL, ceres::DYNAMIC, 3, 3, 3, 3>(
                                                                                                          new GyroCubicError(vi, dt, ts, ws), ceres::TAKE_OWNERSHIP, ws.size()*3);
  }
  
  const DynVisIns &vi;
  double dt;                  ///< total time for this segment
  const vector<double> ts;    ///< sequence of relative times at which gyro measurements arrived
  const vector<Vector3d> ws;  ///< sequence of angular measurements  
};


/**
 * Accelerometer error, assuming segment is parametrized as a cubic spline
 */
struct AccCubicError {

  /**
   * @param vi 
   * @param dt delta-t for this segment
   * @param ts local times for each measurement (should be in [0,dt])
   * @param as the sequence of measurements in this segment
   */
  AccCubicError(const DynVisIns &vi, 
                double dt,
                const vector<double> &ts,
                const vector<Vector3d> &as)
    : vi(vi), dt(dt), ts(ts), as(as)
  {
    assert(dt > 0);
    assert(ts.size() == as.size());
  }
  
  /**
   * Computes IMU error between two states xa and xb
   * @param wa_ angular velocity at start of segment
   * @param wb_ angular velocity at end of segment
   * @param res residual for all gyro measurements within segment
   */
  bool operator()(const double* ra_,
                  const double* pa_,
                  const double* dra_,
                  const double* va_,
                  const double* rb_,
                  const double* pb_,
                  const double* drb_,
                  const double* vb_,
                  double* res) const 
  {

    Vector3d ra(ra_);
    Vector3d pa(pa_);
    Vector3d dra(dra_);
    Vector3d va(va_);
    Vector3d rb(rb_);
    Vector3d pb(pb_);
    Vector3d drb(drb_);
    Vector3d vb(vb_);

    Cubic cr(ra, dra, rb, drb, dt);
    Cubic cp(pa, va, pb, vb, dt);

    Vector3d r, a;
    Matrix3d R;
    
    for (int i = 0; i < as.size(); ++i) {      
      const double &t = ts[i];

      cr.GetPos(r, t);
      SO3::Instance().exp(R, r);
      cp.GetAcc(a, t);
            
      // for now just assume noise is spherical and defined in vi.aStd
      Vector3d e = (a - R*(as[i] - vi.ba) + vi.g0)/vi.aStd;
      //      cout << a.transpose() << as[i].transpose() << (R*as[i]).transpose() << e.transpose() << endl;
      memcpy(res + 3*i, e.data(), 3*sizeof(double));
    }
    return true;
  }
  
  static ceres::CostFunction* Create(const DynVisIns &vi, 
                                     double dt,
                                     const vector<double> &ts,
                                     const vector<Vector3d> &as) {
    return new ceres::NumericDiffCostFunction<AccCubicError, ceres::CENTRAL, ceres::DYNAMIC, 3, 3, 3, 3, 3, 3, 3, 3>(
                                                                                                        new AccCubicError(vi, dt, ts, as), ceres::TAKE_OWNERSHIP, as.size()*3);
  }
  
  const DynVisIns &vi;
  double dt;                  ///< total time for this segment
  const vector<double> ts;    ///< sequence of relative times at which gyro measurements arrived
  const vector<Vector3d> as;  ///< sequence of angular measurements
};


/**
 * Constant Velocity Rotational Error
 */
struct CvCubicRotError {
  /**
   * @param vi  visual-inertial estimaor
   * @param dt segment delta t
   */
  CvCubicRotError(DynVisIns &vi, 
                  double dt)
    : vi(vi), dt(dt)
  { 
    assert(dt > 0);
  }
  
  /**
   * Computes dyn error between two states
   */
  bool operator()(const double* ra_,
                  const double* dra_,
                  const double* rb_,
                  const double* drb_,
                  double* res) const 
  {
    Vector3d ra(ra_);
    Vector3d dra(dra_);
    Vector3d rb(rb_);
    Vector3d drb(drb_);

    Cubic cub(ra, dra, rb, drb, dt);

    Vector3d e1 = (sqrt(dt)/vi.dwStd)*(cub.b + dt*cub.c);
    Vector3d e2 = (sqrt(dt/3)/vi.dwStd)*(dt*cub.c);
    memcpy(res, e1.data(), 3*sizeof(double));
    memcpy(res + 3, e2.data(), 3*sizeof(double));

    return true;
  }
  
  static ceres::CostFunction* Create(DynVisIns &vi, 
                                     double dt) {
    return (new ceres::NumericDiffCostFunction<CvCubicRotError, ceres::CENTRAL, 6, 3, 3, 3, 3>(
                                                                                          new CvCubicRotError(vi, dt)));
  }
  DynVisIns &vi;
  double dt;
};



/**
 * Constant Velocity Rotational Error
 */
struct CvCubicPosError {
  /**
   * @param vi  visual-inertial estimaor
   * @param dt segment delta t
   */
  CvCubicPosError(DynVisIns &vi, 
                  double dt)
    : vi(vi), dt(dt)
  { 
    assert(dt > 0);
  }
  
  /**
   * Computes dyn error between two states
   */
  bool operator()(const double* pa_,
                  const double* va_,
                  const double* pb_,
                  const double* vb_,
                  double* res) const 
  {
    Vector3d pa(pa_);
    Vector3d va(va_);
    Vector3d pb(pb_);
    Vector3d vb(vb_);

    Cubic cub(pa, va, pb, vb, dt);

    Vector3d e1 = (sqrt(dt)/vi.dvStd)*(cub.b + dt*cub.c);
    Vector3d e2 = (sqrt(dt/3)/vi.dvStd)*(dt*cub.c);
    memcpy(res, e1.data(), 3*sizeof(double));
    memcpy(res + 3, e2.data(), 3*sizeof(double));

    return true;
  }
  
  static ceres::CostFunction* Create(DynVisIns &vi, 
                                     double dt) {
    return (new ceres::NumericDiffCostFunction<CvCubicPosError, ceres::CENTRAL, 6, 3, 3, 3, 3>(
                                                                                          new CvCubicPosError(vi, dt)));
  }
  DynVisIns &vi;
  double dt;
};



/** 
 * Prior residual on the state
 */
struct StatePrior {
  /**
   * @param vi visual-inertial estimator
   * @param x0 prior mean
   * @param P0 prior covariance
   */
  StatePrior(DynVisIns &vi, 
             const Body3dState &x0)
    : vi(vi), x0(x0)
  {    
    LLT<Matrix12d> llt(x0.P.inverse());   // assume P0>0
    this->W = llt.matrixU();   // the weight matrix W such that W'*W=inv(P0)
  }
  
  /**
   * Computes IMU state prior error
   * @param x_q 6-dim vector containing orientation and position
   * @param x_v 3-dim vector containing velocity v
   */
  bool operator()(const double* o_,
                  const double* p_,
                  const double* w_,
                  const double* v_,
                  double* res) const 
  {
    Matrix3d R;
    SO3::Instance().exp(R, Vector3d(o_));

    Vector3d eo;
    SO3::Instance().log(eo, x0.R.transpose()*R); 

    Vector12d e;
    e.head<3>() = eo;
    e.segment<3>(3) = Vector3d(p_) - x0.p;
    e.segment<3>(6) = Vector3d(w_) - x0.w;
    e.tail<3>() = Vector3d(v_) - x0.v;
    e = W*e;

    memcpy(res, e.data(), 12*sizeof(double));

    return true;
  }
  
  static ceres::CostFunction* Create(DynVisIns &vi, 
                                     const Body3dState &x0) {
    return (new ceres::NumericDiffCostFunction<StatePrior, ceres::CENTRAL, 12, 3, 3, 3, 3>(
                                                                                           new StatePrior(vi, x0)));
    
  }

  DynVisIns &vi;       ///< visual-inertial estimator
  Body3dState x0;      ///< prior state x0
  Matrix12d W;         ///< residual weight matrix W is such that W'*W=inv(P0)
};



DynVisIns::DynVisIns() : t(-1), tc(-1) {

  v = 0;
  
  // initial state / prior info
  x0.Clear();  
  x0.P.topLeftCorner<3,3>().diagonal().setConstant(.0001);  // R
  x0.P.block<3,3>(3,3).diagonal().setConstant(.0001);      // p
  x0.P.block<3,3>(6,6).diagonal().setConstant(.0001);    // w
  x0.P.block<3,3>(9,9).diagonal().setConstant(.0001);    // v
  
  // from IMU to Cam rotation: first -90Z  then -90X
  Ric << 0, 0, 1, -1, 0, 0, 0, -1, 0;      

  useImu = true;
  useCam = true;
  useDyn = true;
  usePrior = true;

  optBias = false;

  sphMeas = false;


  pxStd = 1;   // pixel measurement error standard deviation
  sphStd = 1;  // corresponding spherial error -- this will be set to the right value later
  
  dwStd = 1;   // angular acc white nose (rad/s^2)
  dvStd = 5;   // linear acc white noise (m/s^2)

  wStd = .001;    // gyro noise
  aStd = .02;     // acc noise

  g0 << 0,0,9.81;
}

DynVisIns::~DynVisIns()
{
}



bool DynVisIns::ProcessImu(double t, const Vector3d &w, const Vector3d &a) {

  // initialize time if this is the first IMU measurement
  if (this->t < 0) {
    this->t = t;
    return true;
  }

  
  double dt = t - this->t;

  if (dt <= 0) {
    cout <<"[W] IMU data out of sync dt=" << dt << endl;
    return false;
  }

  // for now camera is required
  assert(useCam);
      
  // accumulate measurements for this last camera segment 
  // only if a camera frame has alrady been added
  if (useCam && xs.size()) {
    assert(t > tc);
    assert(tss.size());
    assert(wss.size());
    assert(ass.size());

    tss.back().push_back(t - tc); // add local time
    wss.back().push_back(w);    
    ass.back().push_back(a);    
  }

  // update current time
  this->t = t;
  return true;
}


/**
 * Process feature data
 * @param t time
 * @param zcs current measured points
 * @param zcInds current measured point indices
 * @return true on success
 */
bool DynVisIns::ProcessCam(double t, const vector<Vector2d> &zcs, const vector<int> &zcInds) 
{
  assert(useCam);  
  assert(this->t >= 0); // assume at least one IMU measurement has arrived 

  if (useImu) {
    double dt = t - this->t;
    //    cout << "dt=" << dt << endl;
    if (dt < 0) {
      cout << "[W] DynVisIns::ProcessCam: frame out of sync dt=" << dt << endl;
      return false;
    }

    // update global time and camera time
    this->t = t;

    // if this is not the first cam frame
    // then add the segment delta-t
    if (tc > 0) {
      dts.push_back(t - tc);
    }

    this->tc = tc;

    // add a new empty sequence of in-between-keyframes IMU measurements
    // these will be populated with IMU data in ProcessImu
    vector<double> ts;
    vector<Vector3d> ws;
    vector<Vector3d> as;
    tss.push_back(ts);
    wss.push_back(ws);
    ass.push_back(as);
  }

  assert(zcs.size() == zcInds.size());

  // push a copy of current state
  xs.push_back(x0); 
  // above one could use the propagated state x instead of x0 to initialize using IMU dead-reconing -- only a good idea if initial pose is correct, otherwise accelerometer-based odometry will be off
  
  // add to all observations
  zs.insert(zs.end(), zcs.begin(), zcs.end());
  
  for (int i = 0; i < zcs.size(); ++i) {

    // generate a spherical measurement
    Vector3d lu((zcs[i][0] - K(0,2))/K(0,0),
                (zcs[i][1] - K(1,2))/K(1,1),
                1);    
    lu = lu/lu.norm(); // unit normal in camera frame      
    lus.push_back(lu);

    // we assume that all feature id's correspond to different points to be optimized
    // we allow gaps is the sequence of id's, although this should not be necessary
    if (zcInds[i] >= ls.size()) {
      // create new points to advance to this id
      while (zcInds[i] >= ls.size())
        ls.push_back(Vector3d(1,0,0));
      
      assert(zcInds[i] == ls.size() - 1);
      
      //      Vector3d lc = x.R*Ric*lu; // unit normal in spatial frame
      //      Vector3d l = 3*lc + x.p; // initial depth is 5

      Vector3d lc = Ric*lu; // unit normal in spatial frame
      Vector3d l = 3*lc; // initial depth is e.g. 3 assuming one is indoors in a room
      
      ls.back() = l;

      /* covariance of points
      static Vector3d e3(0, 0, 1);
      Vector3d b = e3.cross(lc);
      b = b/b.norm();
      
      Matrix3d Rl;
      Rl.col(0) = lc;
      Rl.col(1) = b;
      Rl.col(2) = lc.cross(b);
      
      Matrix3d Pl;
      Pl << 25, 0, 0, 0, .1, 0, 0, 0, .1;

      P.block(15 + i*3, 15 + i*3, 3, 3) = Rl*Pl*Rl.transpose();
      */

    }

    // add the camera index of this measurement
    zCamInds.push_back(xs.size() - 1);
    // add the point index of this measurement
    zInds.push_back(zcInds[i]);
  }
  return true;
}



bool DynVisIns::Compute() {
  
  v = new double[12*xs.size() + 3*ls.size() + (optBias ? 6 : 0)];
  
  ToVec(v);
  if (useCam) {
    // for efficiency, instead of computing a projected covariance on the tangent 
    // of the unit sphere, that needs to be recomputed for every measurement
    // since the projection depends on the measurement,
    // we assume a constant ball of radius sphStd, averaged on the u-v plane
    sphStd = pxStd/sqrt(fx*fx + fy*fy)/2;
    
    assert(this->ls.size());
        
    for (int i = 0; i < lus.size(); ++i) {

      ceres::CostFunction* cost_function =
        sphMeas ?
        SphError::Create(*this, lus[i]) :
        PerspError::Create(*this, zs[i]);
              
      double *x = v + 12*zCamInds[i];
      double *l = v + 12*xs.size() + 3*zInds[i];
      
      assert(zCamInds[i] < xs.size());
      assert(zInds[i] < ls.size());
      

      //      ceres::LossFunction *loss_function = new ceres::HuberLoss(100.0);
      //      ceres::LossFunctionWrapper* loss_function(new ceres::HuberLoss(1.0), ceres::TAKE_OWNERSHIP);

      problem.AddResidualBlock(cost_function,
                               NULL,
                               x, x + 3, l);
      
      // for now restrict point coordinates to [-5,5] meters, assuming we're in a small room
      problem.SetParameterLowerBound(l, 0, -5);
      problem.SetParameterLowerBound(l, 1, -5);
      problem.SetParameterLowerBound(l, 2, -5);
      problem.SetParameterUpperBound(l, 0, 5);
      problem.SetParameterUpperBound(l, 1, 5);
      problem.SetParameterUpperBound(l, 2, 5);      
    }
  }

  if (useImu) {
    assert(xs.size() >= tss.size());
    assert(dts.size() == tss.size());
    
    for (int i = 0; i < tss.size(); ++i) {
      vector<double> &ts = tss[i];
      vector<Vector3d> &ws = wss[i];
      vector<Vector3d> &as = ass[i];
      assert(ts.size() == ws.size());
      assert(ts.size() == as.size());
      if (ts.size()) {
        assert(dts[i] > 0);
        ceres::CostFunction* gyroCost = GyroCubicError::Create(*this, dts[i], ts, ws);
        
        double *xa = v + 12*i;
        double *xb = v + 12*(i+1);
        
        problem.AddResidualBlock(gyroCost,
                                 NULL /* squared loss */,
                                 xa, xa + 6, xb, xb + 6);

        ceres::CostFunction* accCost = AccCubicError::Create(*this, dts[i], ts, as);
        
        problem.AddResidualBlock(accCost,
                                 NULL /* squared loss */,
                                 xa, xa + 3, xa + 6, xa + 9,
                                 xb, xb + 3, xb + 6, xb + 9);        
      }
      // could also add some box constraints on the state?
    }
  }

  if (useDyn) {
    
    assert(xs.size() == dts.size() + 1);
    for (int i = 0; i < dts.size(); ++i) {
      assert(dts[i] > 0);
      
      double *xa = v + 12*i;
      double *xb = v + 12*(i+1);
      
      ceres::CostFunction* rotCost = CvCubicRotError::Create(*this, dts[i]);    
      problem.AddResidualBlock(rotCost,
                               NULL /* squared loss */,
                               xa, xa + 6, xb, xb + 6);
      
      ceres::CostFunction* posCost = CvCubicPosError::Create(*this, dts[i]);    
      problem.AddResidualBlock(posCost,
                               NULL /* squared loss */,
                               xa + 3, xa + 9, xb + 3, xb + 9);
      
      // could also add some box constraints on the state?
    }
  }

  if (usePrior) {
    ceres::CostFunction* cost = StatePrior::Create(*this, x0);
    problem.AddResidualBlock(cost,
                             NULL,
                             v, v + 3, v + 6, v + 9);
  }
    
  ceres::Solver::Options options;
  // options.linear_solver_type = ceres::DENSE_SCHUR;
  options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
  options.minimizer_progress_to_stdout = true;
  options.max_num_iterations = 50;

  ceres::Solver::Summary summary;
  ceres::Solve(options, &problem, &summary);
  std::cout << summary.FullReport() << "\n";  
  
  FromVec(v);

  delete[] v;
  v = 0;

  return true;
}

/*
void hermite3(Vector3d &c1, Vector3d &c2, Vector3d &c3,
              const Vector3d &pa, const Vector3d *va,
              const Vector3d &pb, const Vector3d *vb)
{
  Vector3d d = pb - pa;
  c1 = d*va.norm()/d.norm();
  c2 = 3*(pb - pa) - c1 - 2*va;
  c3 = -2*(pb - pa) + c1 + va;
}
*/

// generate synthetic data
bool DynVisIns::GenData(DynVisIns &tvi, int ni) 
{
  fx = 453.23520207;
  fy = 453.72298392;
  cx = 391.85891497;
  cy = 282.24403976;
  K(0,0) = fx; K(0,1) = 0; K(0,2) = cx;
  K(1,0) = 0; K(1,1) = fy; K(1,2) = cy;

  // tvi is the "true" VI estimator
  vector<Body3dState> &xs = tvi.xs;
  vector<Vector3d> &ls = tvi.ls;

  this->ls.resize(ls.size());

  // generate a grid of features on a plane 3 meters ahead
  int n1 = sqrt(ls.size());
  assert(ls.size() == n1*n1);
  for (int i = 0; i < n1; ++i) {
    for (int j = 0; j < n1; ++j) {
      int ind = i*n1 + j;        
      // true points are at a vertical plane at distance 3 meters forward
      ls[ind] = Vector3d( 3, ((double)(j-n1/2.0))/n1, ((double)(i-n1/2.0))/n1);

      // initialize estimator points using unit vectors pointing towards the points
      this->ls[ind] = ls[ind]/ls[ind].norm();
    }
  }    
  cout << "Generated " << ls.size() << " points" << endl;

  // #of segments
  int ns = xs.size() - 1;

  double t0 = 0;
  double dt = 1.0/ns;  // time-step of each segment
  
  // initial state
  //  Body3dState &x = xs[0];
  //  x.R.setIdentity();
  //  x.p << 0, -1, .5;
  //  x.w << 0, 0, .5;
  //  x.v << 0, .5, 0;

  this->xs.resize(xs.size());
  this->tss.resize(xs.size() - 1);
  this->wss.resize(xs.size() - 1);
  this->ass.resize(xs.size() - 1);  

  // initialize first state using the prior
  this->xs[0] = this->x0;

  Vector3d r, p, dr, v;
  FromState(r, p, dr, v, xs[0]);
  
  for (int i = 0; i < ns; ++i) {
    
    dts.push_back(dt);   // camera segment deltas
    
    // random angular acceleration
    Vector3d aw = dwStd*Vector3d(randn(), randn(), randn());
    Vector3d av = dvStd*Vector3d(randn(), randn(), randn());
    Vector3d jerk(0,0,0);  // zero jerk
    
    Cubic cw(r, dr, dt); cw.b = aw; cw.c = jerk;
    Cubic cv(p, v, dt); cv.b = av; cv.c = jerk;
    
    // generate IMU measurements
    if (useImu) {                        
      vector<double> ts;  // local times
      vector<Vector3d> ws;      
      vector<Vector3d> as;
      Matrix3d D;
      
      for (int j=1; j <=ni; j++) {
        double ti = j*dt/(ni+1); // relative IMU time
        Body3dState xt;
        Vector3d rt, pt, drt, vt, at;
        cw.GetPos(rt, ti);
        cw.GetVel(drt, ti);           
        cv.GetPos(pt, ti); 
        cv.GetVel(vt, ti);
        cv.GetAcc(at, ti);
        
        ToState(xt, rt, pt, drt, vt);
        ts.push_back(ti);   // add an IMU measurement          
        ws.push_back(xt.w);   // gyro reading in body frame
        as.push_back(xt.R.transpose()*(at + g0));  // accel reading in body frame
      }
      tss[i] = ts;
      wss[i] = ws;
      ass[i] = as;
    }
    
    // update pos and vel
    cw.GetPos(r, dt);
    cw.GetVel(dr, dt);
    cv.GetPos(p, dt);
    cv.GetVel(v, dt);      
    
    // true state
    ToState(xs[i+1], r, p, dr, v);
    
    // init estimator data to first state
    this->xs[i+1] = this->xs[0];
  }
  
  // generate feature meas
  if (useCam)  {
    for (int k = 0; k < xs.size(); ++k) {
      Body3dState &x = xs[k];        
      for (int j = 0; j < ls.size(); ++j) {
        Matrix3d R = x.R*Ric;  // camera rotation      
        Vector3d r = R.transpose()*(ls[j] - x.p);
        
        // spherical measurements
        this->lus.push_back(r/r.norm());      
        
          // pixel measurements
        this->zs.push_back(K*(r/r[2]));
        
        this->zInds.push_back(j);
        this->zCamInds.push_back(k);
      }
    }
  }
 
  // useImu = false;
  
  cout << "Generated " << lus.size() << " feature measurements" << endl;
  cout << "Generated " << tss.size() << " IMU segments" << endl;

  return true;
}

bool DynVisIns::LoadFile(const char* filename) {
  ifstream file(filename);    
  
  if (!file.is_open()) {
    std::cerr << "[E] unable to open file " << filename << std::endl;
    return false;
  }
  
  double dummy;
  double t; // time
  int n;    // nof observations at time t
  int k = 0;
  int msgType;

  do {
    file >> msgType >> t;
    if (msgType == 3) {
      file >> fx >> fy >> cx >> cy >> n;
      
      // params are fx, fy, cx, cy
      K(0,0) = fx; K(0,1) = 0; K(0,2) = cx;
      K(1,0) = 0; K(1,1) = fy; K(1,2) = cy;
      
      if (!n) 
        continue;
      
      vector<Vector2d> zcs(n);
      vector<int> zcInds(n);
      
      for (int i = 0; i < n; ++i) {
        file >> zcs[i][0] >> zcs[i][1];
      }
      for (int i = 0; i < n; ++i) {
        file >> zcInds[i];
      }        
      //      if (x.v.norm() > 0.1)
      if (useCam)
        ProcessCam(t, zcs, zcInds);     
    }

    if (msgType == 1) {
      Vector3d a0, w, a;
      file >> a0[0] >> a0[1] >> a0[2] >> w[0] >> w[1] >> w[2] >> a[0] >> a[1] >> a[2];

      //      imu.a0 = a0;
      //      imu.a0 << 0, 0, 9.81;
      g0 << 0, 0, 9.81;
      cout << a.transpose() << endl;

      if (useImu)
        ProcessImu(t, w, a);
    }
    if (msgType == 2) {
      for (int i = 0; i <6; ++i)
        file >> dummy;
    }      
    k++;
    //    if (dtss.size()>55)
    //      break;
  } while(!file.eof());
  
  file.close();    
  cout << "Added " << xs.size() << " frames " << ls.size() << " points, using " << zs.size() << " feature measurements" << endl;
  
  return true;
}
