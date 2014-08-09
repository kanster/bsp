#ifndef __PR2_EIH_SYSTEM_H__
#define __PR2_EIH_SYSTEM_H__

#include "pr2_utils/pr2_sim/simulator.h"
#include "pr2_utils/pr2_sim/arm.h"
#include "pr2_utils/pr2_sim/camera.h"

#include <Eigen/Eigen>
#include <Eigen/StdVector>
using namespace Eigen;

#include "../../util/logging.h"

#define TIMESTEPS 5
#define DT 1.0 // Note: if you change this, must change the FORCES matlab file

#define G_DIM 3					// 3-d
#define J_DIM ARM_DIM           // joints
#define X_DIM (ARM_DIM+G_DIM)	// arm + object
#define U_DIM ARM_DIM
#define Q_DIM ARM_DIM
#define Z_DIM (ARM_DIM+G_DIM)	// relative object position
#define R_DIM (ARM_DIM+G_DIM)

#define TOTAL_VARS (TIMESTEPS*J_DIM + (TIMESTEPS-1)*U_DIM)

typedef Matrix<double,X_DIM,1> VectorX;
//typedef Matrix<double,J_DIM,1> VectorJ;
typedef Matrix<double,U_DIM,1> VectorU;
typedef Matrix<double,Q_DIM,1> VectorQ;
typedef Matrix<double,Z_DIM,1> VectorZ;
typedef Matrix<double,R_DIM,1> VectorR;
typedef Matrix<double,TOTAL_VARS,1> VectorTOTAL;

typedef Matrix<double,X_DIM,X_DIM> MatrixX;
typedef Matrix<double,J_DIM,J_DIM> MatrixJ;
typedef Matrix<double,U_DIM,U_DIM> MatrixU;
typedef Matrix<double,Q_DIM,Q_DIM> MatrixQ;
typedef Matrix<double,Z_DIM,Z_DIM> MatrixZ;
typedef Matrix<double,R_DIM,R_DIM> MatrixR;
typedef Matrix<double,TOTAL_VARS,TOTAL_VARS> MatrixTOTAL;
typedef Matrix<double,G_DIM,J_DIM> MatrixJac;

typedef std::vector<VectorX, aligned_allocator<VectorX>> StdVectorX;
typedef std::vector<VectorJ, aligned_allocator<VectorJ>> StdVectorJ;
typedef std::vector<VectorU, aligned_allocator<VectorU>> StdVectorU;
typedef std::vector<Matrix4d, aligned_allocator<Matrix4d>> StdMatrix4d;

class PR2EihSystem {
	const double step = 0.0078125*0.0078125;

	const double alpha_control = .01; // .01
	const double alpha_belief = 1e3; // 1
	const double alpha_final_belief = 1e3; // 1

public:
	PR2EihSystem(pr2_sim::Simulator *s, pr2_sim::Arm *a, pr2_sim::Camera *c);
	PR2EihSystem() : PR2EihSystem(NULL, NULL, NULL) { }

	VectorJ dynfunc(const VectorJ& j, const VectorU& u, const VectorQ& q, bool enforce_limits=false);
	VectorZ obsfunc(const VectorJ& j, const Vector3d& object, const VectorR& r);

	MatrixZ delta_matrix(const VectorJ& j, const Vector3d& object, const double alpha,
			const std::vector<geometry3d::Triangle>& obstacles);

	void belief_dynamics(const VectorX& x_t, const MatrixX& sigma_t, const VectorU& u_t, const double alpha,
			const std::vector<geometry3d::Triangle>& obstacles, VectorX& x_tp1, MatrixX& sigma_tp1);

	void get_limits(VectorJ& j_min, VectorJ& j_max, VectorU& u_min, VectorU& u_max);

	double cost(const StdVectorJ& J, const MatrixJ& j_sigma0, const StdVectorU& U, const Vector3d& obj, const Matrix3d& obj_sigma0,
			const double alpha, const std::vector<geometry3d::Triangle>& obstacles);
	VectorTOTAL cost_grad(StdVectorJ& J, const MatrixJ& j_sigma0, StdVectorU& U, const Vector3d& obj, const Matrix3d& obj_sigma0,
			const double alpha, const std::vector<geometry3d::Triangle>& obstacles);

	void plot(const StdVectorJ& J, const Vector3d& obj, const Matrix3d& obj_sigma,
			const std::vector<geometry3d::Triangle>& obstacles, bool pause=true);

private:
	pr2_sim::Simulator *sim;
	pr2_sim::Arm *arm;
	pr2_sim::Camera *cam;

	VectorJ j_min, j_max, u_min, u_max;
	MatrixQ Q;
	MatrixR R;

	void linearize_dynfunc(const VectorX& x, const VectorU& u, const VectorQ& q,
			Matrix<double,X_DIM,X_DIM>& A, Matrix<double,X_DIM,Q_DIM>& M);
	void linearize_obsfunc(const VectorX& x, const VectorR& r,
			Matrix<double,Z_DIM,X_DIM>& H);
};

#endif

