#include <iostream>
#include "gdocp.h"
#include "particle2d.h"
#include "rnlqcost.h"
#include "viewer.h"
#include "particle2dview.h"
#include "utils.h"
#include "diskconstraint.h"
#include "constraintcost.h"
#include "multicost.h"

using namespace std;
using namespace Eigen;
using namespace gcop;

typedef GDocp<Vector4d, 4, 2> Particle2dGDocp;
typedef DiskConstraint<Vector4d, 4, 2> Particle2dDiskConstraint;
typedef ConstraintCost<Vector4d, 4, 2, Dynamic, 1> DiskConstraintCost;


void solver_process(Viewer* viewer)
{
  viewer->SetCamera(22.625, 45, 1.25, 3.15, -10);

  int N = 32;
  double tf = 10;
  double h = tf/N;

  Particle2d sys;

  Vector4d xf = Vector4d::Zero();
  RnLqCost<4, 2> cost(sys, tf, xf);
  cost.Q = Matrix4d(Vector4d(.01, .01, .005, .005).asDiagonal());
  cost.Qf = Matrix4d(Vector4d(1, 1, 5, 5).asDiagonal());
  cost.R = Matrix2d(Vector2d(.1, .1).asDiagonal());  


  // times
  vector<double> ts(N+1);
  for (int k = 0; k <=N; ++k)
    ts[k] = k*h;

  // states
  vector<Vector4d> xs(N+1);
  xs[0] << -5, -5, .1, 0;

  // initial guess for the controls (almost any guess should work for this system)
  vector<Vector2d> us(N);
  for (int i = 0; i < N/2; ++i) {
    us[i] = Vector2d(.05, .025);  
    us[N/2+i] = Vector2d(-.05, -.025);
  }
  
  Disk disk(Vector2d(-2.5,-2.5), 2);
  Particle2dDiskConstraint constraint(disk, 0);
  DiskConstraintCost dcost(sys, tf, constraint);
  
  MultiCost<Vector4d, 4, 2> mcost(sys, tf);
  mcost.costs.push_back(&cost);   
  //  mcost.costs.push_back(&dcost);


  Particle2dGDocp gdocp(sys, mcost, ts, xs, us);

  // should converge in one iteration since it is a linear system
  int iters = 25;  

  Particle2dView view(sys, &gdocp.xs);
  viewer->Add(view);  

  struct timeval timer;
  //  gdocp.debug = false; // turn off debug for speed

    timer_start(timer);
    gdocp.Iterate();
    long te = timer_us(timer);

    cout << "Solution" << " took: " << te << " us." << endl;
    //    getchar();
    //  }
  //  for (int k = 0; k <= N; ++k)
  //cout << "xf=" << endl;
  //  cout << ddp.xs.back() << endl;

  cout << "done!" << endl;

  while(1)
    usleep(10);
}


#define DISP

int main(int argc, char** argv)
{

#ifdef DISP
  Viewer *viewer = new Viewer;
  viewer->Init(&argc, argv);
  viewer->frameName = "videos/sys";

  pthread_t dummy;
  pthread_create( &dummy, NULL, (void *(*) (void *)) solver_process, viewer);

#else
  solver_process(0);
#endif


#ifdef DISP
  viewer->Start();
#endif


  return 0;
}
