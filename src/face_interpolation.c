//////////////////////////////////////////////////////////////////

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include "condition.h"
#include "face.h"
#include "info.h"
#include "linear.h"
#include "memory.h"
#include "numerics.h"
#include "solver.h"

//////////////////////////////////////////////////////////////////

#include "face.r"

//////////////////////////////////////////////////////////////////

static double **y, ***y_adj;

static double **R, **inv_R, **T;

static int *n_adj_bases, *n_int_bases;

static int *face_taylor;

static int ldp, lds, ldq, lda, ldb, ldf, ldd;
static int sizep, sizef;
static double **P, **S, **Q, **A, **B, **F, ***D;

static int info, *pivot, int_1 = 1;
static char trans[2] = "NT";
static double double_1 = 1.0, double_0 = 0.0;

static int taylor_power[2];

//////////////////////////////////////////////////////////////////

int face_interpolation_start()
{
	// locations
	y = matrix_double_new(NULL,2,solver_n_gauss());
	y_adj = tensor_double_new(NULL,2,2,solver_n_hammer()*(ELEMENT_MAX_N_FACES-2));
	if(y == NULL || y_adj == NULL) return FACE_MEMORY_ERROR;

	// transformation
	R = matrix_double_new(NULL,2,2);
	inv_R = matrix_double_new(NULL,2,2);
	T = matrix_double_new(NULL,solver_variable_max_n_bases(),solver_variable_max_n_bases());
	if(R == NULL || inv_R == NULL || T == NULL) return FACE_MEMORY_ERROR;

	// interpolation problem sizes
	int max_n_conditions = condition_max_n_constraints();
	int max_n_adj_bases = 2*solver_variable_max_n_bases();
	int max_n_int_bases = 2*solver_variable_max_n_bases() + max_n_conditions*solver_n_gauss();
	n_adj_bases = (int *)malloc(solver_n_variables() * sizeof(int));
	if(n_adj_bases == NULL) return FACE_MEMORY_ERROR;
	n_int_bases = (int *)malloc(solver_n_variables() * sizeof(int));
	if(n_int_bases == NULL) return FACE_MEMORY_ERROR;

	// face basis indices
	face_taylor = (int *)malloc(max_n_int_bases * sizeof(int));
	if(face_taylor == NULL) return FACE_MEMORY_ERROR;

	// temporary matrices
	ldp = lds = ldq = 2*(ELEMENT_MAX_N_FACES-2)*solver_n_hammer();
	lda = ldb = max_n_int_bases;
	ldf = ldd = solver_n_gauss();
	sizep = max_n_adj_bases*ldp;
	sizef = max_n_int_bases*ldf;
	P = matrix_double_new(NULL,max_n_adj_bases,ldp);
	S = matrix_double_new(NULL,max_n_adj_bases,lds);
	Q = matrix_double_new(NULL,max_n_int_bases,ldp);
	A = matrix_double_new(NULL,max_n_int_bases,max_n_int_bases);
	B = matrix_double_new(NULL,max_n_int_bases,max_n_int_bases);
	F = matrix_double_new(NULL,max_n_int_bases,solver_n_gauss());
	D = tensor_double_new(NULL,solver_variable_max_n_bases(),max_n_int_bases,solver_n_gauss());
	if(P == NULL || S == NULL || Q == NULL || A == NULL || B == NULL || F == NULL || D == NULL) return FACE_MEMORY_ERROR;

	// lapack
	pivot = (int *)malloc(max_n_int_bases * sizeof(int));
	if(pivot == NULL) return FACE_MEMORY_ERROR;

	return ELEMENT_SUCCESS;
}

//////////////////////////////////////////////////////////////////

void face_interpolation_end()
{
	matrix_free((void *)y);
	tensor_free((void *)y_adj);
	matrix_free((void *)R);
	matrix_free((void *)inv_R);
	matrix_free((void *)T);
	free(n_adj_bases);
	free(n_int_bases);
	free(face_taylor);
	matrix_free((void *)P);
	matrix_free((void *)S);
	matrix_free((void *)Q);
	matrix_free((void *)A);
	matrix_free((void *)B);
	matrix_free((void *)F);
	tensor_free((void *)D);
	free(pivot);
}

//////////////////////////////////////////////////////////////////

int face_interpolation_calculate(FACE face)
{
	int a, c, i, j, n, q, v;

	// boundary condition
	CONDITION condition = face->boundary ? boundary_condition(face->boundary) : condition_empty();

	// numbers of bases
	int sum_n_adj_bases = 0, sum_n_int_bases = 0;
	for(v = 0; v < solver_n_variables(); v ++)
	{
		n_adj_bases[v] = face->n_borders*solver_variable_n_bases()[v];
		n_int_bases[v] = n_adj_bases[v] + condition_variable_n_constraints(condition)[v]*face->n_quadrature;
	}
	for(i = 0; i < solver_n_interpolations(); i ++)
	{
		sum_n_adj_bases += n_adj_bases[solver_interpolation_variable()[i]];
		sum_n_int_bases += n_int_bases[solver_interpolation_variable()[i]];
	}

	// allocate
	face->Q = (double ***)malloc(solver_n_interpolations() * sizeof(double **));
	if(face->Q == NULL) return FACE_MEMORY_ERROR;
	face->Q[0] = (double **)malloc(sum_n_int_bases * sizeof(double *));
	if(face->Q[0] == NULL) return FACE_MEMORY_ERROR;
	face->Q[0][0] = (double *)malloc(sum_n_int_bases * face->n_quadrature * sizeof(double));
	if(face->Q[0][0] == NULL) return FACE_MEMORY_ERROR;
	for(i = 1; i < solver_n_interpolations(); i ++) face->Q[i] = face->Q[i-1] + n_int_bases[solver_interpolation_variable()[i-1]];
	for(i = 1; i < solver_n_interpolations(); i ++) face->Q[i][0] = face->Q[i-1][0] + n_int_bases[solver_interpolation_variable()[i-1]] * face->n_quadrature;
	for(i = 0; i < solver_n_interpolations(); i ++)
		for(j = 1; j < n_int_bases[solver_interpolation_variable()[i]]; j ++)
			face->Q[i][j] = face->Q[i][j-1] + face->n_quadrature;

	// rotation to face coordinates
	R[0][0] = + face->normal[0]; R[0][1] = + face->normal[1];
	R[1][0] = - face->normal[1]; R[1][1] = + face->normal[0];
	double det_R = R[0][0]*R[1][1] - R[0][1]*R[1][0];
	inv_R[0][0] = + R[1][1]/det_R; inv_R[0][1] = - R[0][1]/det_R;
	inv_R[1][0] = - R[1][0]/det_R; inv_R[1][1] = + R[0][0]/det_R;
	numerics_transformation_matrix(solver_variable_max_order(),T,inv_R);

	// face integration locations
	for(q = 0; q < face->n_quadrature; q ++)
	{
		for(i = 0; i < 2; i ++)
		{
			y[i][q] = face->centre[i];
			for(j = 0; j < 2; j ++) y[i][q] += R[i][j]*(face->X[j][q] - face->centre[j]);
		}
	}

	// numbers of element integration locations
	int n_points[2], sum_n_points[3];
	for(a = 0; a < face->n_borders; a ++) n_points[a] = element_n_quadrature(face->border[a]);
	sum_n_points[0] = 0;
	for(a = 0; a < face->n_borders; a ++) sum_n_points[a+1] = sum_n_points[a] + n_points[a];

	// adjacent element geometry
	for(a = 0; a < face->n_borders; a ++)
	{
		for(q = 0; q < n_points[a]; q ++)
		{
			for(i = 0; i < 2; i ++)
			{
				y_adj[a][i][q] = face->centre[i];
				for(j = 0; j < 2; j ++)
					y_adj[a][i][q] += R[i][j]*(element_quadrature_x(face->border[a])[j][q] - face->centre[j]);
			}
		}
	}

	// for all variables
	for(v = 0; v < solver_n_variables(); v ++)
	{
		// face basis indices
		n = 0;
		for(i = 0; i < face->n_borders*solver_variable_order()[v] + condition_variable_n_constraints(condition)[v]; i ++)
			for(j = 0; j < face->n_borders*solver_variable_order()[v] + condition_variable_n_constraints(condition)[v]; j ++)
				if(i + face->n_borders*j < face->n_borders*solver_variable_order()[v] + condition_variable_n_constraints(condition)[v] && j < face->n_quadrature)
					face_taylor[n ++] = numerics_power_taylor(i,j);
		exit_if_false(n == n_int_bases[v],"mismatched number of interpolation bases");

		// no differential
		for(i = 0; i < 2; i ++) taylor_power[i] = 0;

		// element bases at the integration locations
		for(i = 0; i < face->n_borders*solver_variable_n_bases()[v]; i ++)
			for(j = 0; j < sum_n_points[face->n_borders]; j ++)
				P[i][j] = 0.0;
		for(a = 0; a < face->n_borders; a ++)
			for(i = 0; i < solver_variable_n_bases()[v]; i ++)
				numerics_basis(n_points[a],&P[i+a*solver_variable_n_bases()[v]][sum_n_points[a]],
						element_quadrature_x(face->border[a]),element_centre(face->border[a]),element_size(face->border[a]),
						i,taylor_power);

		// face bases at the integration locations
		for(a = 0; a < face->n_borders; a ++)
			for(i = 0; i < n_int_bases[v]; i ++)
				numerics_basis(n_points[a],&Q[i][sum_n_points[a]],
						y_adj[a],face->centre,0.5*face->size,
						face_taylor[i],taylor_power);

		// integration matrix
		dcopy_(&sizep,P[0],&int_1,S[0],&int_1);
		for(a = 0; a < face->n_borders; a ++)
			for(i = 0; i < n_points[a]; i ++)
				dscal_(&solver_variable_n_bases()[v],
						&element_quadrature_w(face->border[a])[i],
						&S[a*solver_variable_n_bases()[v]][i+sum_n_points[a]],&lds);

		// weak interpolation system
		dgemm_(&trans[1],&trans[0],
				&n_adj_bases[v],&n_int_bases[v],&sum_n_points[face->n_borders],
				&double_1,
				S[0],&lds,
				Q[0],&ldq,
				&double_0,
				A[0],&lda);

		// weak interpolation rhs
		dgemm_(&trans[1],&trans[0],
				&n_adj_bases[v],&n_adj_bases[v],&sum_n_points[face->n_borders],
				&double_1,
				S[0],&lds,
				P[0],&ldp,
				&double_0,
				B[0],&ldb);

		// boundary conditions
		for(c = 0; c < condition_variable_n_constraints(condition)[v]; c ++)
		{
			for(i = 0; i < 2; i ++)
				taylor_power[i] = numerics_taylor_power(condition_variable_differential(condition)[v][c],i);

			for(i = 0; i < n_int_bases[v]; i ++)
				numerics_basis(face->n_quadrature,&A[i][n_adj_bases[v]],
						y,face->centre,0.5*face->size,
						face_taylor[i],taylor_power);

			for(i = 0; i < face->n_quadrature; i ++)
				for(j = 0; j < n_int_bases[v]; j ++)
					B[j][i+c*face->n_quadrature+n_adj_bases[v]] =
						B[i+c*face->n_quadrature+n_adj_bases[v]][j] =
						(i+c*face->n_quadrature+n_adj_bases[v]) == j;
		}

		// solve interpolation problem
		dgesv_(&n_int_bases[v],&n_int_bases[v],
				A[0],&lda,
				pivot,
				B[0],&ldb,
				&info);

		// interpolate values to the face integration locations
		for(i = 0; i < solver_variable_n_bases()[v]; i ++)
		{
			for(j = 0; j < 2; j ++)
				taylor_power[j] = numerics_taylor_power(i,j);

			for(j = 0; j < n_int_bases[v]; j ++)
				numerics_basis(face->n_quadrature,F[j],
						y,face->centre,0.5*face->size,
						face_taylor[j],taylor_power);

			dgemm_(&trans[0],&trans[0],
					&face->n_quadrature,&n_int_bases[v],&n_int_bases[v],
					&double_1,
					F[0],&ldf,
					B[0],&ldb,
					&double_0,
					D[i][0],&ldd);
		}

		// transform from face to cartesian coordinates
		n = n_int_bases[v]*face->n_quadrature;
		for(i = 0; i < solver_n_interpolations(); i ++)
			if(solver_interpolation_variable()[i] == v)
				dgemv_(&trans[0],
						&n,&solver_variable_n_bases()[v],
						&double_1,
						D[0][0],&sizef,
						T[solver_interpolation_differential()[i]],&int_1,
						&double_0,
						face->Q[i][0],&int_1);
	}

	return FACE_SUCCESS;
}

//////////////////////////////////////////////////////////////////
