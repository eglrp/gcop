#include <iomanip>
#include <iostream>
#include "kinbody3dtrackcost.h"
#include "pddp.h"
#include "utils.h"
#include "viewer.h"
#include "kinbody3dview.h"
#include "kinbody3dcost.h"
#include "params.h"
#include "kinbody3dtrackview.h"
#include "kinrccar.h"
#include "unistd.h"

using namespace std;
using namespace Eigen;
using namespace gcop;

typedef Matrix<double, 6, 1> Vector6d;
typedef Ddp<Matrix4d, 6, 2> KinRccarDdp;

Params params;

void Run(Viewer* viewer)
{
  srand(1);
  
  if (viewer)
    viewer->SetCamera(18.875, 1.625, -.15, -.6, -35.5);
  
  double tf = 30;
  int nf = 100;
  params.GetDouble("tf", tf);
  params.GetInt("nf", nf);

  double Ts = 1;
  params.GetDouble("Ts", Ts);

  bool renderForces = false;
  params.GetBool("renderForces", renderForces);

  bool hideTrue = false;
  params.GetBool("hideTrue", hideTrue);
  bool hideEst = false;
  params.GetBool("hideEst", hideEst);

  bool hideOdom = false;
  params.GetBool("hideOdom", hideOdom);

  int slidingWindow = -1;
  params.GetInt("slidingWindow", slidingWindow);

  // control horizon params
  int iters = 30;
  double Tc = 2; // horizon
  int N; // optimal control segments
  params.GetInt("iters", iters);
  params.GetDouble("Tc", Tc);
  params.GetInt("N", N);
  double h = Tc/N;

  KinRccar sys;

  double r = 25;
  params.GetDouble("r", r);

  double vd = 5;
  params.GetDouble("vd", vd);

  // options: paramForce, forces
  Kinbody3dTrack<2> pg(sys, nf, vd, 0, tf, r, false, true);   ///< ground truth

  params.GetDouble("w", pg.w);
  params.GetVector2d("cw", pg.cw);
  params.GetDouble("cp", pg.cp);
  params.GetDouble("dmax", pg.dmax);

  pg.MakeTrue();

  Matrix4d x0;
  pg.Get(x0, vd, 0);

  //  Body2dTrack pg(sys, N, nf, 0, tf, false, false, true);    ///< noisy pose track
  //  Body2dTrack::Synthesize3(pgt, pg, tf);

  Kinbody3dView<2> tview(sys, &pg.xs, &pg.us);   // estimated path
  tview.lineWidth = 5;
  tview.rgba[0] = 0;
  tview.rgba[1] = 1;
  tview.rgba[2] = 0;
  tview.renderSystem = false;
  tview.renderForces = renderForces;

  Kinbody3dView<2> oview(sys, &pg.xos);   // odometry
  oview.lineWidth = 5;
  oview.renderSystem = false;
  oview.rgba[0] = 1;
  oview.rgba[1] = 0;
  oview.rgba[2] = 0;

  Kinbody3dTrackView<2> pgv(pg);               // optimized pose-track
  pgv.rgba[0] = 1; pgv.rgba[1] = 1; pgv.rgba[2] = 1; 

  pgv.drawLandmarks = true;

  pgv.drawForces = renderForces;
  pgv.forceScale=.1;


  if (viewer) {
    if (!hideEst)
      viewer->Add(tview);
    if (!hideOdom)
      viewer->Add(oview);
    viewer->Add(pgv);
  }

  struct timeval timer;
  getchar();

  Matrix4d xf;
  pg.Get(xf, vd, Tc);

  // cost
  Kinbody3dCost<2> cost(sys, Tc, xf);
  //params.GetDouble("ko", cost.ko);
  //if (cost.ko > 1e-10)
  //  cost.track = &pg;


  VectorXd Q(6);
  if (params.GetVectorXd("Q", Q))
    cost.Q = Q.asDiagonal();
  
  VectorXd Qf(6);
  if (params.GetVectorXd("Qf", Qf))
    cost.Qf = Qf.asDiagonal();
  
  VectorXd R(2);
  if (params.GetVectorXd("R", R)) 
    cost.R = R.asDiagonal();

  // times
  vector<double> ts(N+1);
  for (int k = 0; k <=N; ++k)
    ts[k] = k*h;

  // states
  vector<Matrix4d > xs(N+1, pg.xs[0]);

  // controls
  Vector2d u;
  u << 0,0;
  vector<Vector2d> us(N, u);

  // past true trajectory  
  vector<double> tps(1,0);
  vector<Matrix4d > xps(1, x0);
  vector<Vector2d> ups;

  KinRccarDdp ddp(sys, cost, ts, xs, us);
  ddp.mu = .01;
  ddp.debug = false;
  params.GetDouble("mu", ddp.mu);

  Kinbody3dView<2> cview(sys, &ddp.xs);
  cview.rgba[0] = 0;  cview.rgba[1] = 1;  cview.rgba[2] = 1;
  viewer->Add(cview);
  cview.renderSystem = false;
  cview.lineWidth = 4;

  // cview.renderForces = true;

  Kinbody3dView<2> pview(sys, &xps, &ups);
  if (!hideTrue)
      viewer->Add(pview);
  pview.rgba[0] = 1;  pview.rgba[1] = 1;  pview.rgba[2] = 0;
  pview.renderSystem = false;
  pview.renderForces = renderForces;

  Kinbody3dTrackCost<2> tcost(0, pg);  ///< cost function
  PDdp<Matrix4d, 6, 2> *pddp = 0;


  bool oc = false;
  params.GetBool("oc", oc);
  
  for (double t=0; t < tf; t+=h) {

    pg.Get(xf, vd, t+Tc);
    cost.tf = t + Tc;


    xs[0] = pg.xs.back();    
    for (int j=0; j < N; ++j) {
      sys.Step(xs[j+1], t + j*h, xs[j], us[j], h);
    }

    if (oc) {
      for (int j = 0; j < iters; ++j) {
        timer_start(timer);
        ddp.Iterate();
        long te = timer_us(timer);      
        cout << "Iteration #" << j << " took: " << te << " us." << endl;    
      }
    }

    // apply actual control (puluated with noise)
    Vector2d w;
    w << sqrt(pg.cw[0])*random_normal(), 
                          sqrt(pg.cw[1]); 
                          //sqrt(pg.cw[1])*random_normal(); 
    //w << 0,  0, 0, sqrt(pg.cw[3]), 0, 0; 
    std::cout << "w: " << w << std::endl;
    std::cout << "us[0]: " << us[0] << std::endl;
    //w << 0, sqrt(pg.cw[1]), 0, 0, 0, 0;
    
    // simulate true state
    Matrix4d xt;
    sys.Step(xt, t, xps.back(), us[0] + w, h);

    // add assumed control and true state to estimator
    pg.Add2(us[0], xt, h);


    // shift control trajectory forward
    for (int k = 0; k < N; ++k) {
      ts[k] = ts[k+1];
      if (k < N - 1)
        us[k] = us[k+1];
    }
    ts[N] = ts[N] + h;


    tcost.tf = t+h;
    
    if (t > Ts) {
      cout << "ts " << pg.ts.size() << endl;
      cout << "xs " << pg.xs.size() << endl;
      cout << "us " << pg.us.size() << endl;
      cout << "p " << pg.p.size() << endl;

      std::vector<double> ts_pddp;
      std::vector<Matrix4d> xs_pddp;
      std::vector<Vector2d> us_pddp;
  
      if(slidingWindow < 0 || pg.us.size() < slidingWindow)
      {
        ts_pddp = pg.ts;
        xs_pddp = pg.xs;
        us_pddp = pg.us;
      }
      else
      {
        ts_pddp = std::vector<double>(pg.ts.end()-slidingWindow-1, pg.ts.end());
        xs_pddp = std::vector<Matrix4d>(pg.xs.end()-slidingWindow-1, pg.xs.end());
        us_pddp = std::vector<Vector2d>(pg.us.end()-slidingWindow, pg.us.end());
      }

      pddp = new PDdp<Matrix4d, 6, 2>(pg.sys, tcost, ts_pddp, xs_pddp, us_pddp, pg.p, 3*pg.extforce);
      //pddp = new PDdp<Matrix4d, 6, 2>(pg.sys, tcost, pg.ts, pg.xs, pg.us, pg.p, 3*pg.extforce);
      pddp->debug = false;
      for (int b=0; b < 10;++b)
      {
        //getchar(); 
        timer_start(timer);
        pddp->Iterate();     
        long te = timer_us(timer);      
        cout << "Iteration #" << b << " took: " << te << " us." << endl;    
      }
      
      if(slidingWindow < 0 || pg.us.size() < slidingWindow)
      {
        for(int i = 0; i < pg.us.size(); i++)
        { 
          pg.us[i] = us_pddp.at(i);
        }
        for(int i = 0; i < pg.xs.size(); i++)
        { 
          pg.xs[i] = xs_pddp.at(i);
        }
      }
      else
      {
        int j = 0;
        for(int i = pg.us.size() - slidingWindow, j = 0; i < pg.us.size(); i++, j++)
        { 
          pg.us[i] = us_pddp.at(j);
        }
        for(int i = pg.xs.size() - slidingWindow - 1, j = 0; i < pg.xs.size(); i++, j++)
        { 
          pg.xs[i] = xs_pddp.at(j);
        }

      }
      
      //cout << "est x:" << pg.xs.back().first << endl;
      //cout << "true x:" << xt.first << endl << xt.second.head<3>() << endl;
      delete pddp;
    }

    // add true control and true state to list
    tps.push_back(ts[0]);
    xps.push_back(xt);
    ups.push_back(us[0] + w);
    //    ups.push_back(us[0] - pg.us[0]);

    cout << "FEATURES:" << pg.p.size() << endl;
    
    getchar(); 
    //viewer->saveSnapshot=true;
  }


  cout << "done!" << endl;
  while(1)
    usleep(10);    
}


#define DISP

int main(int argc, char** argv)
{
  if (argc > 1)
    params.Load(argv[1]);
  else
    params.Load("../../bin/kinrccartracktest.cfg");

#ifdef DISP
  Viewer *viewer = new Viewer;
  viewer->Init(&argc, argv);
  viewer->frameName = "../../logs/body3dtrack/frames/kinbody3d";
  viewer->displayName = "../../logs/body3dtrack/display/kinbody3d";

  pthread_t dummy;
  pthread_create( &dummy, NULL, (void *(*) (void *)) Run, viewer);

#else
  Run(0);
#endif


#ifdef DISP
  viewer->Start();
#endif


  return 0;
}
