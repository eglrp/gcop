#ifndef GCOP_CASADI_SYSTEM_H
#define GCOP_CASADI_SYSTEM_H

#include "gcop_conversions.h"
#include "loop_timer.h"
#include "system.h"
#include <Eigen/Dense>
#include <assert.h>
#include <casadi/casadi.hpp>
#include <string>

namespace gcop {

using namespace std;
using namespace Eigen;
namespace cs = casadi;

/**
* A wrapper around casadi function interface to implement step functions using
* auto differentiation
*
* Author: Gowtham Garimella ggarime1[at]jhu[dot]edu
*/
template <typename T = VectorXd, int _nx = Dynamic, int _nu = Dynamic,
          int _np = Dynamic>
class CasadiSystem : public System<T, _nx, _nu, _np> {
private:
  using Base = System<T, _nx, _nu, _np>; ///< Base class
  using Vectormd = typename Base::Vectormd; ///< Base class parameter typedef

public:
  /**
 * @brief CasadiSystem constructor
 * @param X  The state manifold
 * @param p  The default parameters for the system
 * @param nu The control size. Only relevant if _nu is Dynamic
 * @param np The parameter vector size. Only relevant if _np is Dynamic
 * @param use_code_generation  If true will compile the casadi function into a
 * shared library on the fly
 */
  CasadiSystem(Manifold<T, _nx> &X, Vectormd p, int nu = 0, int np = 0,
               bool generate_state_gradients = false,
               bool generate_parameter_gradients = false,
               bool use_code_generation = false)
      : Base(X, nu, np), default_parameters_(p),
        generate_state_gradients_(generate_state_gradients),
        generate_parameter_gradients_(generate_parameter_gradients),
        use_code_generation_(use_code_generation),
        step_function_instantiated_(false) {}

  /**
   * @brief casadiStep
   * @param t Current time
   * @param h Time step
   * @param xa Current state
   * @param u Current control
   * @param p Parameter
   * @return Next state
   */
  virtual cs::MX casadiStep(const cs::MX &t, const cs::MX &h, const cs::MX &xa,
                            const cs::MX &u, const cs::MX &p) = 0;

  /**
  * @brief The name used to generate the function.
  *
  * Used to distinguish from other casadi functions.
  *
  * @return casadi step name
  */
  virtual std::string casadiStepName() = 0;

  /**
   * @brief instantiateStepFunction generate the shared library or create the
   * step function
   */
  void instantiateStepFunction() {
    cs::MX t = cs::MX::sym("t", 1);
    cs::MX h = cs::MX::sym("h", 1);
    cs::MX xa = cs::MX::sym("xa", this->X.n);
    cs::MX u = cs::MX::sym("u", this->U.n);
    cs::MX p = cs::MX::sym("p", this->np);
    cs::MX xb = casadiStep(t, h, xa, u, p);
    std::vector<cs::MX> args_out;
    args_out.push_back(xb); // xb
    if (generate_state_gradients_) {
      args_out.push_back(cs::MX::jacobian(xb, xa));
      args_out.push_back(cs::MX::jacobian(xb, u));
    }
    if (generate_parameter_gradients_) {
      args_out.push_back(cs::MX::jacobian(xb, p));
    }
    std::string step_name = casadiStepName();
    step_function_ = cs::Function(step_name.c_str(), {t, h, xa, u, p}, args_out);
    if (use_code_generation_) {
      std::cout << "Generating code" << std::endl;
      string function_name = step_function_.name();
      step_function_.generate(function_name);
      string compile_command = ("gcc -fPIC -shared -O3 -march=corei7 -ffast-math " + function_name +
                                ".c -o " + function_name + ".so");
      int flag = std::system(compile_command.c_str());
      casadi_assert(flag == 0, "Compilation failed");
      std::cout << "Creating external function" << std::endl;
      step_function_ = cs::external(function_name);
    }
    step_function_instantiated_ = true;
    std::cout << "Done instantiating function" << std::endl;
  }

  /**
   * @brief Step Perform a single step of the dynamics
   * @param xb  The state at the next step
   * @param t The curren time
   * @param xa The current state of the system
   * @param u  The current control
   * @param h  The time step for integration
   * @param p  The parameters of the system. If not provided will use default
   * parameters
   * @param A The jacobian wrt current state is returned if provided
   * @param B The jacobian wrt control is returned if provided
   * @param C The jacobian wrt parameters p is returned if provided
   * @return
   */
  double Step(T &xb, double t, const T &xa, const typename Base::Vectorcd &u,
              double h, const Vectormd *p = 0, typename Base::Matrixnd *A = 0,
              typename Base::Matrixncd *B = 0,
              typename Base::Matrixnmd *C = 0) {

    copy_loop_timer_.loop_start();
    // Prepare inputs t, h, xa,u,p
    std::vector<cs::DM> args;
    args.push_back(cs::DM(t));
    args.push_back(cs::DM(h));
    args.push_back(conversions::convertEigenToDM(xa));
    args.push_back(conversions::convertEigenToDM(u));
    if (p == 0) {
      args.push_back(conversions::convertEigenToDM(default_parameters_));
    } else {
      args.push_back(conversions::convertEigenToDM(*p));
    }
    copy_loop_timer_.loop_pause();

    fun_loop_timer_.loop_start();
    std::vector<cs::DM> result = step_function_(args);
    fun_loop_timer_.loop_end();

    copy_loop_timer_.loop_start();
    if (result.size() == 0 || result.size() > 4) {
      throw std::runtime_error(
          "The output of the casadi function should be between 1 and 4");
    }
    // Extract results, xb,
    xb = conversions::convertDMToEigen(result.at(0));

    if (generate_state_gradients_) {
      if (A != 0) {
        (*A) = conversions::convertDMToEigen(result.at(1));
      }
      if (B != 0) {
        (*B) = conversions::convertDMToEigen(result.at(2));
      }
    }
    if (generate_parameter_gradients_) {
      if (C != 0) {
        int ind = 3;
        if (!generate_state_gradients_) {
          ind = 1;
        }
        (*C) = conversions::convertDMToEigen(result.at(ind));
      }
    }
    copy_loop_timer_.loop_end();
  }

private:
  /**
   * @brief Default system parameters
   */
  Vectormd default_parameters_;
  /**
   * @brief Flag to check if casadi step function is created
   */
  bool step_function_instantiated_;
  /**
   * @brief The instantiated step function
   */
  cs::Function step_function_;
  /**
   * @brief Flag to specify if state gradients should be generated
   */
  bool generate_state_gradients_;
  /**
   * @brief Flag to specify if parameter gradients should be generated
   */
  bool generate_parameter_gradients_;
  /**
   * @brief Flag to specify whether code generation should be used
   */
  bool use_code_generation_;

public:
  /**
   * @brief copy_loop_timer_
   *
   * Timer to find the time taken in copying inputs to outputs
   */
  LoopTimer copy_loop_timer_;
  /**
   * @brief fun_loop_timer_
   *
   * Timer that evaluates the time taken in function evaluation
   */
  LoopTimer fun_loop_timer_;
};
}

#endif
