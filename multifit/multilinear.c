/* multifit/multilinear.c
 * 
 * Copyright (C) 2000, 2007, 2010 Brian Gough
 * Copyright (C) 2013 Patrick Alken
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or (at
 * your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <config.h>
#include <gsl/gsl_errno.h>
#include <gsl/gsl_multifit.h>
#include <gsl/gsl_blas.h>
#include <gsl/gsl_vector.h>
#include <gsl/gsl_matrix.h>
#include <gsl/gsl_linalg.h>

/* Fit
 *
 * y = X c
 *
 * where X is an M x N matrix of M observations for N variables.
 *
 * The solution includes a possible standard form Tikhonov regularization:
 *
 * c = (X^T X + lambda^2 I)^{-1} X^T y
 *
 * where lambda^2 is the Tikhonov regularization parameter.
 *
 * Inputs: X        - least squares matrix
 *                    X may point to work->A in the case of ridge
 *                    regression
 *         y        - right hand side vector
 *         tol      - singular value tolerance
 *         balance  - 1 to perform column balancing
 *         lambda   - Tikhonov regularization parameter lambda
 *         rank     - (output) effective rank
 *         c        - (output) model coefficient vector
 *         cov      - (output) covariance matrix
 *         chisq    - (output) residual chi^2
 *         work     - workspace
 */

static int
multifit_linear_svd (const gsl_matrix * X,
                     const gsl_vector * y,
                     const double tol,
                     const int balance,
                     const double lambda,
                     size_t * rank,
                     gsl_vector * c,
                     gsl_matrix * cov,
                     double *chisq,
                     gsl_multifit_linear_workspace * work)
{
  if (X->size1 != y->size)
    {
      GSL_ERROR
        ("number of observations in y does not match rows of matrix X",
         GSL_EBADLEN);
    }
  else if (X->size2 != c->size)
    {
      GSL_ERROR ("number of parameters c does not match columns of matrix X",
                 GSL_EBADLEN);
    }
  else if (cov->size1 != cov->size2)
    {
      GSL_ERROR ("covariance matrix is not square", GSL_ENOTSQR);
    }
  else if (c->size != cov->size1)
    {
      GSL_ERROR
        ("number of parameters does not match size of covariance matrix",
         GSL_EBADLEN);
    }
  else if (X->size1 != work->n || X->size2 != work->p)
    {
      GSL_ERROR
        ("size of workspace does not match size of observation matrix",
         GSL_EBADLEN);
    }
  else if (tol <= 0)
    {
      GSL_ERROR ("tolerance must be positive", GSL_EINVAL);
    }
  else
    {
      const size_t n = X->size1;
      const size_t p = X->size2;
      const double lambda_sq = lambda * lambda;

      size_t i, j, p_eff;

      gsl_matrix *A = work->A;
      gsl_matrix *Q = work->Q;
      gsl_matrix *QSI = work->QSI;
      gsl_vector *S = work->S;
      gsl_vector *xt = work->xt;
      gsl_vector *D = work->D;

      /* Copy X to workspace,  A <= X */

      if (X != A)
        gsl_matrix_memcpy (A, X);

      /* Balance the columns of the matrix A if requested */

      if (balance) 
        {
          gsl_linalg_balance_columns (A, D);
        }
      else
        {
          gsl_vector_set_all (D, 1.0);
        }

      /* Decompose A into U S Q^T */

      gsl_linalg_SV_decomp_mod (A, QSI, Q, S, xt);

      /*
       * Solve y = A c for c
       * c = Q diag(s_i / (s_i^2 + lambda_i^2)) U^T y
       */

      /* compute xt = U^T y */
      gsl_blas_dgemv (CblasTrans, 1.0, A, y, 0.0, xt);

      /* Scale the matrix Q,
       * QSI = Q (S^2 + lambda^2 I)^{-1} S
       *     = Q diag(s_i / (s_i^2 + lambda^2))
       * For standard least squares, lambda = 0 and QSI = Q S^{-1}
       */

      gsl_matrix_memcpy (QSI, Q);

      {
        double s0 = gsl_vector_get (S, 0);
        p_eff = 0;

        for (j = 0; j < p; j++)
          {
            gsl_vector_view column = gsl_matrix_column (QSI, j);
            double sj = gsl_vector_get (S, j);
            double alpha;

            if (sj <= tol * s0)
              {
                alpha = 0.0;
              }
            else
              {
                alpha = sj / (sj * sj + lambda_sq);
                p_eff++;
              }

            gsl_vector_scale (&column.vector, alpha);
          }

        *rank = p_eff;
      }

      gsl_vector_set_zero (c);

      gsl_blas_dgemv (CblasNoTrans, 1.0, QSI, xt, 0.0, c);

      /* Unscale the balancing factors */

      gsl_vector_div (c, D);

      /* Compute chisq, from residual r = y - X c */

      {
        double s2 = 0, r2 = 0, ridge = 0.0;

        for (i = 0; i < n; i++)
          {
            double yi = gsl_vector_get (y, i);
            gsl_vector_const_view row = gsl_matrix_const_row (X, i);
            double y_est, ri;
            gsl_blas_ddot (&row.vector, c, &y_est);
            ri = yi - y_est;
            r2 += ri * ri;
          }

        /* compute || L c ||^2 contribution to chi^2 */
        for (i = 0; i < p; ++i)
          {
            double ci = gsl_vector_get(c, i);
            ridge += lambda_sq * ci * ci;
          }

        s2 = r2 / (n - p_eff);   /* p_eff == rank */

        *chisq = r2 + ridge;

        /* Form variance-covariance matrix cov = s2 * (Q S^-1) (Q S^-1)^T */

        for (i = 0; i < p; i++)
          {
            gsl_vector_view row_i = gsl_matrix_row (QSI, i);
            double d_i = gsl_vector_get (D, i);

            for (j = i; j < p; j++)
              {
                gsl_vector_view row_j = gsl_matrix_row (QSI, j);
                double d_j = gsl_vector_get (D, j);
                double s;

                gsl_blas_ddot (&row_i.vector, &row_j.vector, &s);

                gsl_matrix_set (cov, i, j, s * s2 / (d_i * d_j));
                gsl_matrix_set (cov, j, i, s * s2 / (d_i * d_j));
              }
          }
      }

      return GSL_SUCCESS;
    }
}

int
gsl_multifit_linear (const gsl_matrix * X,
                     const gsl_vector * y,
                     gsl_vector * c,
                     gsl_matrix * cov,
                     double *chisq, gsl_multifit_linear_workspace * work)
{
  size_t rank;
  int status;

  status = multifit_linear_svd (X, y, GSL_DBL_EPSILON, 1, 0.0,
                                &rank, c, cov, chisq, work);
  return status;
}

/* Handle the general case of the SVD with tolerance and rank */

int
gsl_multifit_linear_svd (const gsl_matrix * X,
                         const gsl_vector * y,
                         double tol,
                         size_t * rank,
                         gsl_vector * c,
                         gsl_matrix * cov,
                         double *chisq, gsl_multifit_linear_workspace * work)
{
  int status;
  
  status = multifit_linear_svd (X, y, tol, 1, 0.0, rank, c, cov,
                                chisq, work);
  return status;
}

int
gsl_multifit_linear_usvd (const gsl_matrix * X,
                          const gsl_vector * y,
                          double tol,
                          size_t * rank,
                          gsl_vector * c,
                          gsl_matrix * cov,
                          double *chisq, gsl_multifit_linear_workspace * work)
{
  int status;

  status = multifit_linear_svd (X, y, tol, 0, 0.0, rank, c, cov,
                                chisq, work);
  return status;
}

int
gsl_multifit_linear_ridge (const double lambda,
                           const gsl_matrix * X,
                           const gsl_vector * y,
                           gsl_vector * c,
                           gsl_matrix * cov,
                           double *chisq,
                           gsl_multifit_linear_workspace * work)
{
  size_t rank;
  int status;

  /* do not balance since it cannot be applied to the Tikhonov term */
  status = multifit_linear_svd (X, y, GSL_DBL_EPSILON, 0, lambda,
                                &rank, c, cov, chisq, work);

  return status;
} /* gsl_multifit_linear_ridge() */

/*
gsl_multifit_linear_ridge2()
  Perform ridge regression with matrix L = diag(lambda_1,lambda_2,...,lambda_p).
This is equivalent to "standard" Tikhonov regression with the change
of variables:

X~ = X * L^{-1}
c~ = L * c

and performing standard Tikhonov regularization on the system
X~ c~ = y with \lambda = 1

Inputs: lambda - vector representing diag(lambda_1,lambda_2,...,lambda_p)
        X      - least squares matrix
        y      - right hand side vector
        c      - (output) coefficients
        cov    - covariance matrix
        chisq  - residual
        work   - workspace
*/

int
gsl_multifit_linear_ridge2 (const gsl_vector * lambda,
                            const gsl_matrix * X,
                            const gsl_vector * y,
                            gsl_vector * c,
                            gsl_matrix * cov,
                            double *chisq,
                            gsl_multifit_linear_workspace * work)
{
  const size_t p = X->size2;

  if (p != lambda->size || lambda->size != c->size)
    {
      GSL_ERROR("lambda vector has incorrect length", GSL_EBADLEN);
    }
  else if (X->size1 != work->n || X->size2 != work->p)
    {
      GSL_ERROR
        ("size of workspace does not match size of observation matrix",
         GSL_EBADLEN);
    }
  else
    {
      size_t rank;
      int status;
      size_t j;

      /* construct X~ = X * L^{-1} matrix using work->A */
      for (j = 0; j < p; ++j)
        {
          gsl_vector_const_view Xj = gsl_matrix_const_column(X, j);
          gsl_vector_view Aj = gsl_matrix_column(work->A, j);
          double lambdaj = gsl_vector_get(lambda, j);

          if (lambdaj == 0.0)
            {
              GSL_ERROR("lambda matrix is singular", GSL_EDOM);
            }

          gsl_vector_memcpy(&Aj.vector, &Xj.vector);
          gsl_vector_scale(&Aj.vector, 1.0 / lambdaj);
        }

      /*
       * do not balance since it cannot be applied to the Tikhonov term;
       * lambda = 1 in the transformed system
       */
      status = multifit_linear_svd (work->A, y, GSL_DBL_EPSILON, 0,
                                    1.0, &rank, c, cov, chisq, work);

      if (status == GSL_SUCCESS)
        {
          /* compute true solution vector c = L^{-1} c~ */
          gsl_vector_div(c, lambda);
        }

      return status;
    }
} /* gsl_multifit_linear_ridge2() */

/* General weighted case */ 

static int
multifit_wlinear_svd (const gsl_matrix * X,
                      const gsl_vector * w,
                      const gsl_vector * y,
                      double tol,
                      int balance,
                      size_t * rank,
                      gsl_vector * c,
                      gsl_matrix * cov,
                      double *chisq, gsl_multifit_linear_workspace * work)
{
  if (X->size1 != y->size)
    {
      GSL_ERROR
        ("number of observations in y does not match rows of matrix X",
         GSL_EBADLEN);
    }
  else if (X->size2 != c->size)
    {
      GSL_ERROR ("number of parameters c does not match columns of matrix X",
                 GSL_EBADLEN);
    }
  else if (w->size != y->size)
    {
      GSL_ERROR ("number of weights does not match number of observations",
                 GSL_EBADLEN);
    }
  else if (cov->size1 != cov->size2)
    {
      GSL_ERROR ("covariance matrix is not square", GSL_ENOTSQR);
    }
  else if (c->size != cov->size1)
    {
      GSL_ERROR
        ("number of parameters does not match size of covariance matrix",
         GSL_EBADLEN);
    }
  else if (X->size1 != work->n || X->size2 != work->p)
    {
      GSL_ERROR
        ("size of workspace does not match size of observation matrix",
         GSL_EBADLEN);
    }
  else
    {
      const size_t n = X->size1;
      const size_t p = X->size2;

      size_t i, j, p_eff;

      gsl_matrix *A = work->A;
      gsl_matrix *Q = work->Q;
      gsl_matrix *QSI = work->QSI;
      gsl_vector *S = work->S;
      gsl_vector *t = work->t;
      gsl_vector *xt = work->xt;
      gsl_vector *D = work->D;

      /* Scale X,  A = sqrt(w) X */

      gsl_matrix_memcpy (A, X);

      for (i = 0; i < n; i++)
        {
          double wi = gsl_vector_get (w, i);

          if (wi < 0)
            wi = 0;

          {
            gsl_vector_view row = gsl_matrix_row (A, i);
            gsl_vector_scale (&row.vector, sqrt (wi));
          }
        }

      /* Balance the columns of the matrix A if requested */

      if (balance) 
        {
          gsl_linalg_balance_columns (A, D);
        }
      else
        {
          gsl_vector_set_all (D, 1.0);
        }

      /* Decompose A into U S Q^T */

      gsl_linalg_SV_decomp_mod (A, QSI, Q, S, xt);

      /* Solve sqrt(w) y = A c for c, by first computing t = sqrt(w) y */

      for (i = 0; i < n; i++)
        {
          double wi = gsl_vector_get (w, i);
          double yi = gsl_vector_get (y, i);
          if (wi < 0)
            wi = 0;
          gsl_vector_set (t, i, sqrt (wi) * yi);
        }

      gsl_blas_dgemv (CblasTrans, 1.0, A, t, 0.0, xt);

      /* Scale the matrix Q,  Q' = Q S^-1 */

      gsl_matrix_memcpy (QSI, Q);

      {
        double alpha0 = gsl_vector_get (S, 0);
        p_eff = 0;
        
        for (j = 0; j < p; j++)
          {
            gsl_vector_view column = gsl_matrix_column (QSI, j);
            double alpha = gsl_vector_get (S, j);

            if (alpha <= tol * alpha0) {
              alpha = 0.0;
            } else {
              alpha = 1.0 / alpha;
              p_eff++;
            }

            gsl_vector_scale (&column.vector, alpha);
          }

        *rank = p_eff;
      }

      gsl_vector_set_zero (c);

      /* Solution */

      gsl_blas_dgemv (CblasNoTrans, 1.0, QSI, xt, 0.0, c);

      /* Unscale the balancing factors */

      gsl_vector_div (c, D);

      /* Compute chisq, from residual r = y - X c */

      {
        double r2 = 0;

        for (i = 0; i < n; i++)
          {
            double yi = gsl_vector_get (y, i);
            double wi = gsl_vector_get (w, i);
            gsl_vector_const_view row = gsl_matrix_const_row (X, i);
            double y_est, ri;
            gsl_blas_ddot (&row.vector, c, &y_est);
            ri = yi - y_est;
            r2 += wi * ri * ri;
          }

        *chisq = r2;

        /* Form covariance matrix cov = (X^T W X)^-1 = (Q S^-1) (Q S^-1)^T */

        for (i = 0; i < p; i++)
          {
            gsl_vector_view row_i = gsl_matrix_row (QSI, i);
            double d_i = gsl_vector_get (D, i);

            for (j = i; j < p; j++)
              {
                gsl_vector_view row_j = gsl_matrix_row (QSI, j);
                double d_j = gsl_vector_get (D, j);
                double s;

                gsl_blas_ddot (&row_i.vector, &row_j.vector, &s);

                gsl_matrix_set (cov, i, j, s / (d_i * d_j));
                gsl_matrix_set (cov, j, i, s / (d_i * d_j));
              }
          }
      }

      return GSL_SUCCESS;
    }
}


int
gsl_multifit_wlinear (const gsl_matrix * X,
                      const gsl_vector * w,
                      const gsl_vector * y,
                      gsl_vector * c,
                      gsl_matrix * cov,
                      double *chisq, gsl_multifit_linear_workspace * work)
{
  size_t rank;
  int status  = multifit_wlinear_svd (X, w, y, GSL_DBL_EPSILON, 1,  &rank, c,
                                      cov, chisq, work);
  return status;
}

int
gsl_multifit_wlinear_svd (const gsl_matrix * X,
                          const gsl_vector * w,
                          const gsl_vector * y,
                          double tol,
                          size_t * rank,
                          gsl_vector * c,
                          gsl_matrix * cov,
                          double *chisq, gsl_multifit_linear_workspace * work)
{
  int status  = multifit_wlinear_svd (X, w, y, tol, 1, rank, c,
                                      cov, chisq, work);
  return status;

}

int
gsl_multifit_wlinear_usvd (const gsl_matrix * X,
                           const gsl_vector * w,
                           const gsl_vector * y,
                           double tol,
                           size_t * rank,
                           gsl_vector * c,
                           gsl_matrix * cov,
                           double *chisq, gsl_multifit_linear_workspace * work)
{
  int status  = multifit_wlinear_svd (X, w, y, tol, 0, rank, c,
                                      cov, chisq, work);
  return status;

}

/* Estimation of values for given x */

int
gsl_multifit_linear_est (const gsl_vector * x,
                         const gsl_vector * c,
                         const gsl_matrix * cov, double *y, double *y_err)
{

  if (x->size != c->size)
    {
      GSL_ERROR ("number of parameters c does not match number of observations x",
         GSL_EBADLEN);
    }
  else if (cov->size1 != cov->size2)
    {
      GSL_ERROR ("covariance matrix is not square", GSL_ENOTSQR);
    }
  else if (c->size != cov->size1)
    {
      GSL_ERROR ("number of parameters c does not match size of covariance matrix cov",
         GSL_EBADLEN);
    }
  else
    {
      size_t i, j;
      double var = 0;
      
      gsl_blas_ddot(x, c, y);       /* y = x.c */

      /* var = x' cov x */

      for (i = 0; i < x->size; i++)
        {
          const double xi = gsl_vector_get (x, i);
          var += xi * xi * gsl_matrix_get (cov, i, i);

          for (j = 0; j < i; j++)
            {
              const double xj = gsl_vector_get (x, j);
              var += 2 * xi * xj * gsl_matrix_get (cov, i, j);
            }
        }

      *y_err = sqrt (var);

      return GSL_SUCCESS;
    }
}

/*
gsl_multifit_linear_residuals()
  Compute vector of residuals from fit

Inputs: X - design matrix
        y - rhs vector
        c - fit coefficients
        r - (output) where to store residuals
*/

int
gsl_multifit_linear_residuals (const gsl_matrix *X, const gsl_vector *y,
                               const gsl_vector *c, gsl_vector *r)
{
  if (X->size1 != y->size)
    {
      GSL_ERROR
        ("number of observations in y does not match rows of matrix X",
         GSL_EBADLEN);
    }
  else if (X->size2 != c->size)
    {
      GSL_ERROR ("number of parameters c does not match columns of matrix X",
                 GSL_EBADLEN);
    }
  else if (y->size != r->size)
    {
      GSL_ERROR ("number of observations in y does not match number of residuals",
                 GSL_EBADLEN);
    }
  else
    {
      /* r = y - X c */
      gsl_vector_memcpy(r, y);
      gsl_blas_dgemv(CblasNoTrans, -1.0, X, c, 1.0, r);

      return GSL_SUCCESS;
    }
} /* gsl_multifit_linear_residuals() */
