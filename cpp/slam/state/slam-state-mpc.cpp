#include "../slam.h"
#include "../traj/slam-traj.h"


#include <vector>
#include <iomanip>

#include "util/matrix.h"
#include "util/Timer.h"

extern "C" {
#include "stateMPC.h"
stateMPC_FLOAT **H, **f, **lb, **ub, **C, **e, **z;
#include "slam-state-casadi.h"
}

#include "boost/preprocessor.hpp"

const double alpha_belief = 10; // 10;
const double alpha_final_belief = 50; // 50;
const double alpha_control = .01; // .01

int T_MPC = T;

namespace cfg {
const double improve_ratio_threshold = .1; // .1
const double min_approx_improve = 1e-3; // 1e-4
const double min_trust_box_size = 1e-2; // 1e-3

const double trust_shrink_ratio = .5; // .5
const double trust_expand_ratio = 1.2; // 1.2

const double cnt_tolerance = 1e-2; // 1e-2
const double penalty_coeff_increase_ratio = 5; // 5
const double initial_penalty_coeff = 5; // 5

const double initial_trust_box_size = 1; // 5 // split up trust box size for X and U
double initial_Xpos_trust_box_size = 1; // 1;
double initial_Xangle_trust_box_size = M_PI/6; // M_PI/6;
double initial_Uvel_trust_box_size = 1; // 1;
double initial_Uangle_trust_box_size = M_PI/8; // M_PI/8;

const int max_penalty_coeff_increases = 8; // 8
const int max_sqp_iterations = 50; // 50
}


// utility to fill Matrix in column major format in FORCES array
template <size_t _numRows>
inline void fillCol(double *X, const Matrix<_numRows>& XCol) {
	int idx = 0;
	for(size_t r = 0; r < _numRows; ++r) {
		X[idx++] = XCol[r];
	}
}

template <size_t _numRows, size_t _numColumns>
inline void fillColMajor(double *X, const Matrix<_numRows, _numColumns>& XMat) {
	int idx = 0;
	for(size_t c = 0; c < _numColumns; ++c) {
		for(size_t r = 0; r < _numRows; ++r) {
			X[idx++] = XMat[c + r*_numColumns];
		}
	}
}

inline double wrapAngle(double angle) {
	return angle - 2*M_PI * floor(angle/(2*M_PI));
}

double nearestAngleFromTo(double from, double to) {

	while (to > from) {
		if (to - 2*M_PI < from) {
			if (fabs(to - from) < (fabs(to - 2*M_PI - from))) {
				return to;
			} else {
				return to - 2*M_PI;
			}
		}
		to -= 2*M_PI;
	}

	while (to < from) {
		if (to + 2*M_PI > from) {
			if (fabs(to - from) < (fabs(to + 2*M_PI - from))) {
				return to;
			} else {
				return to + 2*M_PI;
			}
		}
		to += 2*M_PI;
	}

	// should never reach this point
	return to;
}


void setupCasadiVars(const std::vector<Matrix<X_DIM> >& X, const std::vector<Matrix<U_DIM> >& U, double* XU_arr, double* Sigma0_arr, double* params_arr)
{
	int index = 0;
	for(int t = 0; t < T-1; ++t) {
		for(int i=0; i < X_DIM; ++i) {
			XU_arr[index++] = X[t][i];
		}

		for(int i=0; i < U_DIM; ++i) {
			XU_arr[index++] = U[t][i];
		}
	}
	for(int i=0; i < X_DIM; ++i) {
		XU_arr[index++] = X[T-1][i];
	}

	Matrix<X_DIM,X_DIM> Sigma0 = SqrtSigma0*SqrtSigma0;
	index = 0;
	for(int i=0; i < X_DIM; ++i) {
		for(int j=0; j < X_DIM; ++j) {
			Sigma0_arr[index++] = Sigma0(i,j);
		}
	}

	params_arr[0] = alpha_belief;
	params_arr[1] = alpha_control;
	params_arr[2] = alpha_final_belief;

}

double casadiComputeCost(const std::vector< Matrix<X_DIM> >& X, const std::vector< Matrix<U_DIM> >& U)
{
	double XU_arr[XU_DIM];
	double Sigma0_arr[X_DIM*X_DIM];
	double params_arr[3];

	setupCasadiVars(X, U, XU_arr, Sigma0_arr, params_arr);

	const double **casadi_input = new const double*[3];
	casadi_input[0] = XU_arr;
	casadi_input[1] = Sigma0_arr;
	casadi_input[2] = params_arr;

	double cost = 0;
	double **cost_arr = new double*[1];
	cost_arr[0] = &cost;

	evaluateCostWrap(casadi_input, cost_arr);

	return cost;
}

double casadiComputeMerit(const std::vector< Matrix<X_DIM> >& X, const std::vector< Matrix<U_DIM> >& U, double penalty_coeff)
{
	double merit = 0;

	merit = casadiComputeCost(X, U);

	Matrix<X_DIM> x, dynviol;
	Matrix<X_DIM, X_DIM> SqrtSigma;
	Matrix<B_DIM> b, b_tp1;
	vec(x0, SqrtSigma0, b);

	for(int t = 0; t < T-1; ++t) {
		unVec(b, x, SqrtSigma);
		b_tp1 = beliefDynamics(b, U[t]);
		dynviol = (X[t+1] - b_tp1.subMatrix<X_DIM,1>(0,0) );
		for(int i = 0; i < X_DIM; ++i) {
			if (i != P_DIM) {
				merit += penalty_coeff*fabs(dynviol[i]);
			} else {
				merit += penalty_coeff*wrapAngle(fabs(dynviol[i])); // since angles wrap
			}
		}
		b = b_tp1;
	}
	return merit;
}

void casadiComputeCostGrad(const std::vector< Matrix<X_DIM> >& X, const std::vector< Matrix<U_DIM> >& U, double& cost, Matrix<XU_DIM>& Grad)
{
	double XU_arr[XU_DIM];
	double Sigma0_arr[X_DIM*X_DIM];
	double params_arr[3];

	setupCasadiVars(X, U, XU_arr, Sigma0_arr, params_arr);

	const double **casadi_input = new const double*[3];
	casadi_input[0] = XU_arr;
	casadi_input[1] = Sigma0_arr;
	casadi_input[2] = params_arr;

	double **costgrad_arr = new double*[2];
	costgrad_arr[0] = &cost;
	costgrad_arr[1] = Grad.getPtr();

	evaluateCostGradWrap(casadi_input, costgrad_arr);

}

double computeCost(const std::vector< Matrix<X_DIM> >& X, const std::vector< Matrix<U_DIM> >& U)
{
	double cost = 0;
	Matrix<B_DIM> b;
	Matrix<X_DIM> x;
	Matrix<X_DIM, X_DIM> SqrtSigma;
	vec(x0, SqrtSigma0, b);

	for(int t = 0; t < T-1; ++t) {
		unVec(b, x, SqrtSigma);
		cost += alpha_belief*tr(SqrtSigma*SqrtSigma);
		cost += alpha_control*tr(~U[t]*U[t]);
		b = beliefDynamics(b, U[t]);
	}
	unVec(b, x, SqrtSigma);
	cost += alpha_final_belief*tr(SqrtSigma*SqrtSigma);
	return cost;
}

double computeMerit(const std::vector< Matrix<X_DIM> >& X, const std::vector< Matrix<U_DIM> >& U, double penalty_coeff)
{
	double merit = 0;
	Matrix<X_DIM> x, dynviol;
	Matrix<X_DIM, X_DIM> SqrtSigma;
	Matrix<B_DIM> b, b_tp1;
	vec(x0, SqrtSigma0, b);

	for(int t = 0; t < T-1; ++t) {
		unVec(b, x, SqrtSigma);
		merit += alpha_belief*tr(SqrtSigma*SqrtSigma) + alpha_control*tr(~U[t]*U[t]);
		b_tp1 = beliefDynamics(b, U[t]);
		dynviol = (X[t+1] - b_tp1.subMatrix<X_DIM,1>(0,0) );
		for(int i = 0; i < X_DIM; ++i) {
			if (i != P_DIM) {
				merit += penalty_coeff*fabs(dynviol[i]);
			} else {
				merit += penalty_coeff*wrapAngle(fabs(dynviol[i]));
			}
		}
		b = b_tp1;
	}
	unVec(b, x, SqrtSigma);
	merit += alpha_final_belief*tr(SqrtSigma*SqrtSigma);
	return merit;
}

// Jacobians: dg(b,u)/db, dg(b,u)/du
void linearizeCarDynamics(const Matrix<X_DIM>& x, const Matrix<U_DIM>& u, Matrix<X_DIM,X_DIM>& F, Matrix<X_DIM,U_DIM>& G, Matrix<X_DIM>& h)
{
	F.reset();
	Matrix<X_DIM> xr(x), xl(x);
	for (size_t i = 0; i < X_DIM; ++i) {
		xr[i] += step; xl[i] -= step;
		F.insert(0,i, (dynfunc(xr, u, zeros<Q_DIM,1>()) - dynfunc(xl, u, zeros<Q_DIM,1>())) / (xr[i] - xl[i]));
		xr[i] = x[i]; xl[i] = x[i];
	}

	G.reset();
	Matrix<U_DIM> ur(u), ul(u);
	for (size_t i = 0; i < U_DIM; ++i) {
		ur[i] += step; ul[i] -= step;
		G.insert(0,i, (dynfunc(x, ur, zeros<Q_DIM,1>()) - dynfunc(x, ul, zeros<Q_DIM,1>())) / (ur[i] - ul[i]));
		ur[i] = u[i]; ul[i] = u[i];
	}

	h = dynfunc(x, u, zeros<Q_DIM,1>());
}


void setupStateVars(stateMPC_params& problem, stateMPC_output& output)
{
	// problem inputs
	H = new stateMPC_FLOAT*[T];
	f = new stateMPC_FLOAT*[T];
	lb = new stateMPC_FLOAT*[T];
	ub = new stateMPC_FLOAT*[T];
	C = new stateMPC_FLOAT*[T-1];
	e = new stateMPC_FLOAT*[T];

	// problem outputs
	z = new stateMPC_FLOAT*[T];

#define SET_VARS(n)    \
		H[ BOOST_PP_SUB(n,1) ] = problem.H##n ;  \
		f[ BOOST_PP_SUB(n,1) ] = problem.f##n ;  \
		lb[ BOOST_PP_SUB(n,1) ] = problem.lb##n ;	\
		ub[ BOOST_PP_SUB(n,1) ] = problem.ub##n ;	\
		C[ BOOST_PP_SUB(n,1) ] = problem.C##n ;	\
		e[ BOOST_PP_SUB(n,1) ] = problem.e##n ;	\
		z[ BOOST_PP_SUB(n,1) ] = output.z##n ;

#define BOOST_PP_LOCAL_MACRO(n) SET_VARS(n)
#define BOOST_PP_LOCAL_LIMITS (1, TIMESTEPS-1)
#include BOOST_PP_LOCAL_ITERATE()

#define SET_LAST_VARS(n)    \
		H[ BOOST_PP_SUB(n,1) ] = problem.H##n ;  \
		f[ BOOST_PP_SUB(n,1) ] = problem.f##n ;  \
		lb[ BOOST_PP_SUB(n,1) ] = problem.lb##n ;	\
		ub[ BOOST_PP_SUB(n,1) ] = problem.ub##n ;	\
		e[ BOOST_PP_SUB(n,1) ] = problem.e##n ;	\
		z[ BOOST_PP_SUB(n,1) ] = output.z##n ;

#define BOOST_PP_LOCAL_MACRO(n) SET_LAST_VARS(n)
#define BOOST_PP_LOCAL_LIMITS (TIMESTEPS, TIMESTEPS)
#include BOOST_PP_LOCAL_ITERATE()

}

void resetStateMPCVars() {
	for(int t = 0; t < T-1; ++t) {
		for(int i=0; i < (3*X_DIM+U_DIM); ++i) { H[t][i] = 0; }
		for(int i=0; i < (3*X_DIM+U_DIM); ++i) { f[t][i] = 0; }
		for(int i=0; i < (3*X_DIM+U_DIM); ++i) { lb[t][i] = -1e2; }
		for(int i=0; i < (X_DIM+U_DIM); ++i) { ub[t][i] = 1e2; }
		for(int i=0; i < (X_DIM*(3*X_DIM+U_DIM)); ++i) { C[t][i] = 0; }
		for(int i=0; i < X_DIM; ++i) { e[t][i] = 0; }
		for(int i=0; i < (X_DIM+U_DIM); ++i) { z[t][i] = 0; }
	}
	for(int i=0; i < (X_DIM); ++i) { H[T-1][i] = 0; }
	for(int i=0; i < (X_DIM); ++i) { f[T-1][i] = 0; }
	for(int i=0; i < (X_DIM); ++i) { lb[T-1][i] = -1e2; }
	for(int i=0; i < (X_DIM); ++i) { ub[T-1][i] = 1e2; }
	for(int i=0; i < X_DIM; ++i) { e[T-1][i] = 0; }
	for(int i=0; i < (X_DIM); ++i) { z[T-1][i] = 0; }


	Matrix<X_DIM,3*X_DIM+U_DIM> CMat;
	CMat.reset();
	CMat.insert<X_DIM,X_DIM>(0,0,identity<X_DIM>());

	for(int t = 0; t < T-1; ++t) {
		fillColMajor(C[t], CMat);
	}

	/*
	int index = 0;
	for(int t = 0; t < T-1; ++t) {
		index = 0;
		for(int i = 0; i < X_DIM; ++i) { lb[t][index++] = -INFTY; }
		for(int i = 0; i < U_DIM; ++i) { lb[t][index++] = -INFTY; }
		for(int i = 0; i < 2*(X_DIM); ++i) { lb[t][index++] = 0; }


		index = 0;
		for(int i = 0; i < X_DIM; ++i) { ub[t][index++] = INFTY; }
		for(int i = 0; i < U_DIM; ++i) { ub[t][index++] = INFTY; }
	}

	for(int i = 0; i < X_DIM; ++i) { lb[T-1][i] = -INFTY; }
	for(int i = 0; i < X_DIM; ++i) { ub[T-1][i] = INFTY; }
	*/
}

void cleanupStateMPCVars()
{
	delete[] H;
	delete[] f;
	delete[] lb;
	delete[] ub;
	delete[] C;
	delete[] e;
	delete[] z;
}

bool isValidInputs()
{
	for(int t = 0; t < T; ++t) {

		std::cout << "t: " << t << std::endl << std::endl;
		/*
			std::cout << "f: ";
			for(int i = 0; i < (3*B_DIM+U_DIM); ++i) {
				std::cout << f[t][i] << " ";
			}
			std::cout << std::endl;
		 */

		std::cout << "lb c: ";
		for(int i = 0; i < C_DIM; ++i) {
			std::cout << lb[t][i] << " ";
		}
		std::cout << std::endl;

		std::cout << "ub c: ";
		for(int i = 0; i < C_DIM; ++i) {
			std::cout << ub[t][i] << " ";
		}
		std::cout << std::endl;

		std::cout << "lb l: ";
		for(int i = 0; i < L_DIM; ++i) {
			std::cout << lb[t][C_DIM+i] << " ";
		}
		std::cout << std::endl;

		std::cout << "ub l: ";
		for(int i = 0; i < L_DIM; ++i) {
			std::cout << ub[t][C_DIM+i] << " ";
		}
		std::cout << std::endl;

		if (t < T-1) {
			std::cout << "lb u: ";
			for(int i = 0; i < U_DIM; ++i) {
				std::cout << lb[t][X_DIM+i] << " ";
			}
			std::cout << std::endl;

			std::cout << "ub u: ";
			for(int i = 0; i < U_DIM; ++i) {
				std::cout << ub[t][X_DIM+i] << " ";
			}
			std::cout << std::endl;
		}
		/*
			//std::cout << "ub s, t: ";
			//for(int i = 0; i < 2*B_DIM; ++i) {
			//	std::cout << ub[t][B_DIM+U_DIM+i] << " ";
			//}
			//std::cout << std::endl;

			std::cout << "C:" << std::endl;
			if (t == 0) {
				for(int i = 0; i < 2*B_DIM*(3*B_DIM+U_DIM); ++i) {
					std::cout << C[t][i] << " ";
				}
			} else {
				for(int i = 0; i < B_DIM*(3*B_DIM+U_DIM); ++i) {
					std::cout << C[t][i] << " ";
				}
			}
			std::cout << std::endl;

			std::cout << "e:" << std::endl;
			if (t == 0) {
				for(int i = 0; i < 2*B_DIM; ++i) {
					std::cout << e[t][i] << " ";
				}
			} else {
				for(int i = 0; i < B_DIM; ++i) {
					std::cout << e[t][i] << " ";
				}
			}

			std::cout << "e:" << std::endl;
			for(int i = 0; i < B_DIM; ++i) {
				std::cout << e[t][i] << " ";
			}
		 */
		std::cout << std::endl << std::endl;
	}
	/*
		std::cout << "e:" << std::endl;
		for(int i = 0; i < B_DIM; ++i) {
			std::cout << e[T-1][i] << " ";
		}
	 */


	std::cout << std::endl;
	std::cout << std::endl;
	return true;
}

bool minimizeMeritFunction(std::vector< Matrix<X_DIM> >& X, std::vector< Matrix<U_DIM> >& U, stateMPC_params& problem, stateMPC_output& output, stateMPC_info& info, double penalty_coeff)
{
	LOG_DEBUG("Solving sqp problem with penalty parameter: %2.4f", penalty_coeff);

	std::vector< Matrix<X_DIM,X_DIM> > F(T-1);
	std::vector< Matrix<X_DIM,U_DIM> > G(T-1);
	std::vector< Matrix<X_DIM> > h(T-1);

	double Xeps = cfg::initial_trust_box_size;
	double Ueps = cfg::initial_trust_box_size;

	double Xpos_eps = cfg::initial_Xpos_trust_box_size;
	double Xangle_eps = cfg::initial_Xangle_trust_box_size;
	double Uvel_eps = cfg::initial_Uvel_trust_box_size;
	double Uangle_eps = cfg::initial_Uangle_trust_box_size;

	double optcost;

	std::vector<Matrix<X_DIM> > Xopt(T);
	std::vector<Matrix<U_DIM> > Uopt(T-1);

	double merit, model_merit, new_merit;
	double approx_merit_improve, exact_merit_improve, merit_improve_ratio;
	double constant_cost, hessian_constant, jac_constant;

	int sqp_iter = 1, index = 0;
	bool success;

	Matrix<X_DIM+U_DIM, X_DIM+U_DIM> HMat;
	Matrix<X_DIM,X_DIM> HfMat;
	Matrix<X_DIM> eVec;
	Matrix<X_DIM,3*X_DIM+U_DIM> CMat;

	Matrix<X_DIM,X_DIM> IX = identity<X_DIM>();
	Matrix<X_DIM,X_DIM> minusIX = IX;
	for(int i = 0; i < X_DIM; ++i) {
		minusIX(i,i) = -1;
	}

	Matrix<X_DIM+U_DIM> zbar;

	// full Hessian from current timstep
	Matrix<XU_DIM,XU_DIM> B = identity<XU_DIM>();

	Matrix<XU_DIM> Grad, Gradopt;
	double cost;
	int idx = 0;

	// sqp loop
	while(true)
	{
		// In this loop, we repeatedly construct a linear approximation to the nonlinear belief dynamics constraint
		LOG_DEBUG("  sqp iter: %d", sqp_iter);

		merit = casadiComputeMerit(X, U, penalty_coeff);
		//merit = computeMerit(X, U, penalty_coeff);

		LOG_DEBUG("  merit: %4.10f", merit);

		// Compute gradients
		casadiComputeCostGrad(X, U, cost, Grad);
		//cost = computeCost(X, U);

		// Problem linearization and definition
		// fill in H, f

		hessian_constant = 0;
		jac_constant = 0;
		idx = 0;

		for (int t = 0; t < T-1; ++t)
		{
			Matrix<X_DIM>& xt = X[t];
			Matrix<U_DIM>& ut = U[t];

			idx = t*(X_DIM+U_DIM);
			//LOG_DEBUG("idx: %d",idx);

			// fill in gradients and Hessians

			HMat.reset();
			for(int i = 0; i < (X_DIM+U_DIM); ++i) {
				double val = B(idx+i,idx+i);
				HMat(i,i) = (val < 0) ? 0 : val;
			}

			// since diagonal, fill directly
			for(int i = 0; i < (X_DIM+U_DIM); ++i) { H[t][i] = HMat(i,i); }
			// TODO: why does this work???
			for(int i = 0; i < (2*X_DIM); ++i) { H[t][i + (X_DIM+U_DIM)] = 1e3; } //1e4

			zbar.insert(0,0,xt);
			zbar.insert(X_DIM,0,ut);

			for(int i = 0; i < (X_DIM+U_DIM); ++i) {
				//if (t < T_MPC-1) {
					hessian_constant += HMat(i,i)*zbar[i]*zbar[i];
					jac_constant -= Grad[idx+i]*zbar[i];
				//}
				f[t][i] = Grad[idx+i] - HMat(i,i)*zbar[i];
			}

			// penalize dynamics slack variables
			for(int i = X_DIM+U_DIM; i < 3*X_DIM+U_DIM; ++i) { f[t][i] = penalty_coeff; }

			// fill in linearizations
			linearizeCarDynamics(xt, ut, F[t], G[t], h[t]);


			CMat.reset();
			eVec.reset();

			CMat.insert<X_DIM,X_DIM>(0,0,F[t]);
			CMat.insert<X_DIM,U_DIM>(0,X_DIM,G[t]);
			CMat.insert<X_DIM,X_DIM>(0,X_DIM+U_DIM,IX);
			CMat.insert<X_DIM,X_DIM>(0,2*X_DIM+U_DIM,minusIX);

			fillColMajor(C[t], CMat);

			if (t == 0) {
				eVec.insert<X_DIM,1>(0,0,X[0]);
				fillCol(e[0], eVec);
			}

			eVec = -h[t] + F[t]*xt + G[t]*ut;
			fillCol(e[t+1], eVec);
		}

		//if (T <= T_MPC) {
			// For last stage, fill in H, f
			Matrix<X_DIM>& xT = X[T-1];

			idx = (T-1)*(X_DIM+U_DIM);
			//LOG_DEBUG("idx: %d",idx);

			HfMat.reset();
			for(int i = 0; i < X_DIM; ++i) {
				double val = B(idx+i,idx+i);
				HfMat(i,i) = (val < 0) ? 0 : val;
			}

			// since diagonal, fill directly
			for(int i = 0; i < X_DIM; ++i) { H[T-1][i] = HMat(i,i); }

			for(int i = 0; i < X_DIM; ++i) {
				//if (T <= T_MPC) {
					hessian_constant += HfMat(i,i)*xT[i]*xT[i];
					jac_constant -= Grad[idx+i]*xT[i];
				//}
				f[T-1][i] = Grad[idx+i] - HfMat(i,i)*xT[i];
			}
		//}


		constant_cost = 0.5*hessian_constant + jac_constant + cost;
		LOG_DEBUG("  hessian cost: %4.10f", 0.5*hessian_constant);
		LOG_DEBUG("  jacobian cost: %4.10f", jac_constant);
		LOG_DEBUG("  constant cost: %4.10f", constant_cost);


		// trust region size adjustment
		while(true)
		{
			LOG_DEBUG("       trust region size: %2.6f %2.6f", Xeps, Ueps);

			// solve the innermost QP here
			for(int t = 0; t < T-1; ++t)
			{
				Matrix<X_DIM>& xt = X[t];
				Matrix<U_DIM>& ut = U[t];

				// Fill in lb, ub

				index = 0;
				// x lower bound
				//for(int i = 0; i < X_DIM; ++i) { lb[t][index++] = MAX(xMin[i], xt[i] - Xeps); }

				// car pos lower bound
				for(int i = 0; i < P_DIM; ++i) { lb[t][index++] = MAX(xMin[i], xt[i] - Xpos_eps); }
				// car angle lower bound
				lb[t][index++] = MAX(xMin[P_DIM], xt[P_DIM] - Xangle_eps);
				// landmark pos lower bound
				for(int i = C_DIM; i < X_DIM; ++i) { lb[t][index++] = MAX(xMin[i], xt[i] - Xpos_eps); }

				// u lower bound
				//for(int i = 0; i < U_DIM; ++i) { lb[t][index++] = MAX(uMin[i], ut[i] - Ueps); }

				// u velocity lower bound
				lb[t][index++] = MAX(uMin[0], ut[0] - Uvel_eps);
				// u angle lower bound
				lb[t][index++] = MAX(uMin[1], ut[1] - Uangle_eps);

				// for lower bound on L1 slacks
				for(int i = 0; i < 2*X_DIM; ++i) { lb[t][index++] = 0; }

				index = 0;
				// x upper bound
				//for(int i = 0; i < X_DIM; ++i) { ub[t][index++] = MIN(xMax[i], xt[i] + Xeps); }

				// car pos upper bound
				for(int i = 0; i < P_DIM; ++i) { ub[t][index++] = MIN(xMax[i], xt[i] + Xpos_eps); }
				// car angle upper bound
				ub[t][index++] = MIN(xMax[P_DIM], xt[P_DIM] + Xangle_eps);
				// landmark pos upper bound
				for(int i = C_DIM; i < X_DIM; ++i) { ub[t][index++] = MIN(xMax[i], xt[i] + Xpos_eps); }


				// u upper bound
				//for(int i = 0; i < U_DIM; ++i) { ub[t][index++] = MIN(uMax[i], ut[i] + Ueps); }

				// u velocity upper bound
				ub[t][index++] = MIN(uMax[0], ut[0] + Uvel_eps);
				// u angle upper bound
				ub[t][index++] = MIN(uMax[1], ut[1] + Uangle_eps);
			}

			// TODO: hardcoded first large step!
			//lb[0][X_DIM] = 4;

			Matrix<X_DIM>& xT_MPC = X[T_MPC-1];

			// Fill in lb, ub
			index = 0;
			double finalPosDelta = .1;
			double finalAngleDelta = M_PI/4;

			// xGoal lower bound
			for(int i = 0; i < P_DIM; ++i) { lb[T_MPC-1][index++] = xGoal[i] - finalPosDelta; }
			// loose on car angle and landmarks
			//lb[T_MPC-1][index++] = xGoal[2] - finalAngleDelta;
			lb[T_MPC-1][index++] = nearestAngleFromTo(xT_MPC[2], xGoal[2] - finalAngleDelta);
			for(int i = C_DIM; i < X_DIM; ++i) { lb[T_MPC-1][index++] = MAX(xMin[i], xT_MPC[i] - Xpos_eps); }

			index = 0;
			// xGoal upper bound
			for(int i = 0; i < P_DIM; ++i) { ub[T_MPC-1][index++] = xGoal[i] + finalPosDelta; }
			// loose on car angle and landmarks
			//ub[T_MPC-1][index++] = xGoal[2] + finalAngleDelta;
			ub[T_MPC-1][index++] = nearestAngleFromTo(xT_MPC[2], xGoal[2] + finalAngleDelta);
			for(int i = C_DIM; i < X_DIM; ++i) { ub[T_MPC-1][index++] = MIN(xMax[i], xT_MPC[i] + Xpos_eps); }


			// set timesteps after T_MPC to stay in goal region
			for(int t = T_MPC-1; t < T; ++t) {
				for(int i = 0; i < X_DIM; ++i) { lb[t][i] = lb[T_MPC-1][i]; }
				if (t < T-1) {
					for(int i = X_DIM; i < (X_DIM+U_DIM); ++i) { lb[t][i] = 0; }
				}

				for(int i = 0; i < X_DIM; ++i) { ub[t][i] = ub[T_MPC-1][i]; }
				if (t < T-1) {
					for(int i = X_DIM; i < (X_DIM+U_DIM); ++i) { ub[t][i] = 0; }
				}

			}



			// Verify problem inputs
			if (!isValidInputs()) {
				std::cout << "Inputs are not valid!" << std::endl;
				exit(-1);
			}


			int exitflag = stateMPC_solve(&problem, &output, &info);
			if (exitflag == 1) {
				for(int t = 0; t < T-1; ++t) {
					Matrix<X_DIM>& xt = Xopt[t];
					Matrix<U_DIM>& ut = Uopt[t];

					for(int i = 0; i < X_DIM; ++i) {
						xt[i] = z[t][i];
					}
					for(int i = 0; i < U_DIM; ++i) {
						ut[i] = z[t][X_DIM+i];
					}
					optcost = info.pobj;
				}
				for(int i = 0; i < X_DIM; ++i) {
					Xopt[T-1][i] = z[T-1][i];
				}
			}
			else {
				LOG_ERROR("Some problem in solver");
				pythonDisplayTrajectory(U, T, true);
				exit(-1);
			}


			LOG_DEBUG("       Optimized cost: %4.10f", optcost);

			model_merit = optcost + constant_cost;

			new_merit = casadiComputeMerit(Xopt, Uopt, penalty_coeff);
			//new_merit = computeMerit(Xopt, Uopt, penalty_coeff);

			LOG_DEBUG("       merit: %4.10f", merit);
			LOG_DEBUG("       model_merit: %4.10f", model_merit);
			LOG_DEBUG("       new_merit: %4.10f", new_merit);

			approx_merit_improve = merit - model_merit;
			exact_merit_improve = merit - new_merit;
			merit_improve_ratio = exact_merit_improve / approx_merit_improve;

			LOG_DEBUG("       approx_merit_improve: %1.6f", approx_merit_improve);
			LOG_DEBUG("       exact_merit_improve: %1.6f", exact_merit_improve);
			LOG_DEBUG("       merit_improve_ratio: %1.6f", merit_improve_ratio);

			if (approx_merit_improve < 0) {
				LOG_DEBUG("approx_merit_improve < 0")
				//pythonDisplayTrajectory(Uopt, T, true);
				//pythonDisplayTrajectory(Xopt, T, true);
				std::cout << "PAUSED INSIDE minimizeMeritFunction" << std::endl;
				std::cin.ignore();
			}

			//std::cout << "PAUSED INSIDE minimizeMeritFunction AFTER OPTIMIZATION" << std::endl;
			//std::cin.ignore();


			if (approx_merit_improve < -1e-5) {
				//LOG_ERROR("Approximate merit function got worse: %1.6f", approx_merit_improve);
				//LOG_ERROR("Either convexification is wrong to zeroth order, or you are in numerical trouble");
				//LOG_ERROR("Failure!");

				return false;
			} else if (approx_merit_improve < cfg::min_approx_improve) {
				LOG_DEBUG("Converged: improvement small enough");
				X = Xopt; U = Uopt;
				return true;
			} else if ((exact_merit_improve < 0) || (merit_improve_ratio < cfg::improve_ratio_threshold)) {
				//Xeps *= cfg::trust_shrink_ratio;
				//Ueps *= cfg::trust_shrink_ratio;
				Xpos_eps *= cfg::trust_shrink_ratio;
				Xangle_eps *= cfg::trust_shrink_ratio;
				Uvel_eps *= cfg::trust_shrink_ratio;
				Uangle_eps *= cfg::trust_shrink_ratio;
				LOG_DEBUG("Shrinking trust region size to: %2.6f %2.6f %2.6f %2.6f", Xpos_eps, Xangle_eps, Uvel_eps, Uangle_eps);
			} else {
				//Xeps *= cfg::trust_expand_ratio;
				//Ueps *= cfg::trust_expand_ratio;
				Xpos_eps *= cfg::trust_expand_ratio;
				Xangle_eps *= cfg::trust_expand_ratio;
				Uvel_eps *= cfg::trust_expand_ratio;
				Uangle_eps *= cfg::trust_expand_ratio;


				casadiComputeCostGrad(Xopt, Uopt, cost, Gradopt);

				Matrix<XU_DIM> s, y;

				idx = 0;
				for(int t = 0; t < T-1; ++t) {
					for(int i=0; i < X_DIM; ++i) {
						s[idx+i] = Xopt[t][i] - X[t][i];
						y[idx+i] = Gradopt[idx+i] - Grad[idx+i];
					}
					idx += X_DIM;

					for(int i=0; i < U_DIM; ++i) {
						s[idx+i] = Uopt[t][i] - U[t][i];
						y[idx+i] = Gradopt[idx+i] - Grad[idx+i];
					}
					idx += U_DIM;
				}
				for(int i=0; i < X_DIM; ++i) {
					s[idx+i] = Xopt[T-1][i] - X[T-1][i];
					y[idx+i] = Gradopt[idx+i] - Grad[idx+i];
				}

				double theta;
				Matrix<XU_DIM> Bs = B*s;

				bool decision = ((~s*y)[0] >= .2*(~s*Bs)[0]);
				if (decision) {
					theta = 1;
				} else {
					theta = (.8*(~s*Bs)[0])/((~s*Bs-~s*y)[0]);
				}

				//std::cout << "theta: " << theta << std::endl;

				Matrix<XU_DIM> r = theta*y + (1-theta)*Bs;
				//Matrix<XU_DIM> rBs = theta*(y -Bs);

				// SR1 update
				//B = B + (rBs*~rBs)/((~rBs*s)[0]);

				// L-BFGS update
				B = B - (Bs*~Bs)/((~s*Bs)[0]) + (r*~r)/((~s*r)[0]);

				// Do not update B
				//B = identity<XU_DIM>();

				X = Xopt; U = Uopt;

				LOG_DEBUG("Accepted, Increasing trust region size to:  %2.6f %2.6f %2.6f %2.6f", Xpos_eps, Xangle_eps, Uvel_eps, Uangle_eps);
				break;
			}

			//if (Xeps < cfg::min_trust_box_size && Ueps < cfg::min_trust_box_size) {
			if (Xpos_eps < cfg::min_trust_box_size && Xangle_eps < cfg::min_trust_box_size &&
					Uvel_eps < cfg::min_trust_box_size && Uangle_eps < cfg::min_trust_box_size) {
			    LOG_DEBUG("Converged: x tolerance");
			    return true;
			}

			//std::cout << "U" << std::endl;
			//for(int t=0; t < T-1; ++t) {
			//	std::cout << ~U[t];
			//}
			//std::cout << std::endl << std::endl;
			//pythonDisplayTrajectory(U, T, true);
			//pythonDisplayTrajectory(X, T, true);

		} // trust region loop
		sqp_iter++;
	} // sqp loop

	return success;
}


double statePenaltyCollocation(std::vector< Matrix<X_DIM> >& X, std::vector< Matrix<U_DIM> >& U, stateMPC_params& problem, stateMPC_output& output, stateMPC_info& info)
{
	resetStateMPCVars();

	double penalty_coeff = cfg::initial_penalty_coeff;
	//double trust_box_size = cfg::initial_trust_box_size;

	int penalty_increases = 0;

	Matrix<X_DIM> dynviol;

	// penalty loop
	while(penalty_increases < cfg::max_penalty_coeff_increases)
	{
		bool success = minimizeMeritFunction(X, U, problem, output, info, penalty_coeff);

		double cntviol = 0;
		for(int t = 0; t < T-1; ++t) {
			dynviol = (X[t+1] - dynfunc(X[t], U[t], zeros<Q_DIM,1>()));
			for(int i = 0; i < X_DIM; ++i) {
				cntviol += fabs(dynviol[i]);
			}
		}

	    success = success && (cntviol < cfg::cnt_tolerance);

		LOG_DEBUG("Constraint violations: %2.10f",cntviol);

	    if (!success) {
	        penalty_increases++;
	        penalty_coeff = penalty_coeff*cfg::penalty_coeff_increase_ratio;
	        //trust_box_size = cfg::initial_trust_box_size;
	    }
	    else {
	    	return computeCost(X, U);
	    	//return casadiComputeCost(X, U);
	    }
	}
	return computeCost(X, U);
	//return casadiComputeCost(X, U);
}

int main(int argc, char* argv[])
{
	LOG_INFO("Initializing problem parameters");
	initProblemParams();

	stateMPC_params problem;
	stateMPC_output output;
	stateMPC_info info;
	setupStateVars(problem, output);

	util::Timer solveTimer, trajTimer;
	double totalSolveTime = 0, trajTime = 0;

	double totalTrajCost = 0;

	std::vector<Matrix<B_DIM> > B_total(T*NUM_WAYPOINTS);
	std::vector<Matrix<U_DIM> > U_total((T-1)*NUM_WAYPOINTS);
	int Bidx = 0, Uidx = 0;
	vec(x0, SqrtSigma0, B_total[0]);

	std::vector<Matrix<B_DIM> > B(T);
	std::vector<Matrix<X_DIM> > X(T);

	Matrix<B_DIM> b;
	Matrix<X_DIM> x;

	Matrix<U_DIM> uinit;

	for(int i=0; i < NUM_WAYPOINTS; ++i) {
		LOG_INFO("Going to waypoint %d",i);
		// goal is waypoint position + direct angle + landmarks
		xGoal.insert(0, 0, waypoints[i]);

		// want to be facing the next waypoint
		if (i < NUM_WAYPOINTS - 1) {
			xGoal[2] = atan2(waypoints[i+1][1] - waypoints[i][1], waypoints[i+1][0] - waypoints[i][0]);
		} else {
			xGoal[2] = atan2(xGoal[1] - x0[1], xGoal[0] - x0[0]);
		}


		xGoal.insert(C_DIM, 0, x0.subMatrix<L_DIM,1>(C_DIM,0));

		std::vector<Matrix<U_DIM> > U(T-1);

		util::Timer_tic(&trajTimer);

		bool initTrajSuccess = initTraj(x0.subMatrix<C_DIM,1>(0,0), xGoal.subMatrix<C_DIM,1>(0,0), U, T);
		if (!initTrajSuccess) {
			LOG_ERROR("Failed to initialize trajectory, exiting slam-state");
			exit(-1);
		}

		double initTrajTime = util::Timer_toc(&trajTimer);
		trajTime += initTrajTime;

		for(int t=0; t < T; ++t) {
			T_MPC = T - t;


			if (t >= 0) {
				cfg::initial_Xpos_trust_box_size = 1; // 1;
				cfg::initial_Xangle_trust_box_size = M_PI/6; // M_PI/6;
				cfg::initial_Uvel_trust_box_size = 1; // 1;
				cfg::initial_Uangle_trust_box_size = M_PI/8; // M_PI/8;
			}



			bool initTrajSuccess = initTraj(x0.subMatrix<C_DIM,1>(0,0), xGoal.subMatrix<C_DIM,1>(0,0), U, T_MPC);
			if (!initTrajSuccess) {
				LOG_ERROR("Failed to initialize trajectory, using last inputs attempted");
				exit(-1);
			}



			X[0] = x0;
			for(int t=0; t < T-1; ++t) {
				X[t+1].insert(0, 0, dynfunc(X[t],U[t],zeros<Q_DIM,1>()));
			}

			//pythonDisplayTrajectory(X, T, true);


			double initTrajCost = computeCost(X, U);
			LOG_INFO("Initial trajectory cost: %4.10f", initTrajCost);

			double initCasadiTrajCost = casadiComputeCost(X, U);
			LOG_INFO("Initial casadi trajectory cost: %4.10f", initCasadiTrajCost);

			//pythonDisplayTrajectory(B, U, waypoints, landmarks, T, true);

			util::Timer_tic(&solveTimer);

			double cost = statePenaltyCollocation(X, U, problem, output, info);

			double solvetime = util::Timer_toc(&solveTimer);
			totalSolveTime += solvetime;

			pythonDisplayTrajectory(U, T, true);

			b = beliefDynamics(B_total[Bidx], U[0]);
			unVec(b, x0, SqrtSigma0);

			B_total[++Bidx] = b;
			U_total[Uidx++] = U[0];


			// shift X "forward" (to the left)
			// shift U "forward" (to the left)

			LOG_INFO("Optimized cost: %4.10f", cost);
			LOG_INFO("Actual cost: %4.10f", computeCost(X,U));

			for(int i=0; i < T-1; ++i) {
				std::cout << ~U[i];
			}
			std::cout << std::endl;

			//pythonDisplayTrajectory(B_total, U_total, waypoints, landmarks, (i*T) + (t+1), true);
		}


		pythonDisplayTrajectory(B_total, U_total, waypoints, landmarks, (i+1)*T, true);
		/*
		vec(x0, SqrtSigma0, B[0]);
		X[0] = x0;
		for (int t = 0; t < T-1; ++t) {
			B[t+1] = beliefDynamics(B[t], U[t]);
			unVec(B[t+1], x, s);
			X[t+1] = x;
		}

		for (int t = 0; t < T-1; ++t) {
			B_total[t+T*i] = B[t];
			U_total[t+(T-1)*i] = U[t];
		}
		B_total[T-1+T*i] = B[T-1];

		totalTrajCost += computeCost(X,U);


		LOG_INFO("Initial cost: %4.10f", initTrajCost);
		LOG_INFO("Optimized cost: %4.10f", cost);
		LOG_INFO("Actual cost: %4.10f", computeCost(X,U));
		LOG_INFO("Trajectory solve time: %5.3f ms", initTrajTime*1000);
		LOG_INFO("Solve time: %5.3f ms", solvetime*1000);
		 */

		//unVec(B[T-1], x0, SqrtSigma0);

		//pythonDisplayTrajectory(B, U, waypoints, landmarks, T, true);

	}



	LOG_INFO("Total trajectory cost: %4.10f", totalTrajCost);
	LOG_INFO("Total trajectory solve time: %5.3f ms", trajTime*1000);
	LOG_INFO("Total solve time: %5.3f ms", totalSolveTime*1000);


	pythonDisplayTrajectory(B_total, U_total, waypoints, landmarks, T*NUM_WAYPOINTS, true);

	cleanupStateMPCVars();

	return 0;
}
