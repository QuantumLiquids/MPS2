// SPDX-License-Identifier: LGPL-3.0-only
/*
* Author: Hao-Xin Wang <wanghx18@mails.tsinghua.edu.cn>
* Creation Date: 2023-04-23
*
* Description: GraceQ/MPS2 project. Implementation details for Lanczos solver in DMRG.
*/

/**
@file lanczos_dmrg_solver_impl.h
@brief Implementation details for Lanczos solver in DMRG.
*/
#include "gqmps2/algorithm/lanczos_solver.h"    // LanczosParams
#include "gqten/gqten.h"
#include "gqten/utility/timer.h"                // Timer
#include "gqmps2/algorithm/dmrg/dmrg.h"         // EffectiveHamiltonianTerm

#include <iostream>
#include <vector>     // vector
#include <cstring>

#include "mkl.h"

namespace gqmps2 {

using namespace gqten;

// Forward declarations.
template<typename TenElemT, typename QNT>
GQTensor<TenElemT, QNT> *eff_ham_terms_mul_two_site_state(
    const EffectiveHamiltonianTermGroup<GQTensor<TenElemT, QNT>> &eff_ham,
    GQTensor<TenElemT, QNT> *state
);

/**
Obtain the lowest energy eigenvalue and corresponding eigenstate from the effective
Hamiltonian and a initial state using Lanczos algorithm.

@param rpeff_ham Effective Hamiltonian as a vector of pointer-to-tensors.
@param pinit_state Pointer to initial state for Lanczos iteration.
@param eff_ham_mul_state Function pointer to effective Hamiltonian multiply to state.
@param params Parameters for Lanczos solver.
*/
template<typename TenT>
LanczosRes<TenT> LanczosSolver(
    const EffectiveHamiltonianTermGroup<TenT> &eff_ham,
    TenT *pinit_state,
    TenT *(*eff_ham_mul_state)(const EffectiveHamiltonianTermGroup<TenT> &,
                               TenT *),     //this is a pointer pointing to a function
    const LanczosParams &params
) {
  // Take care that init_state will be destroyed after call the solver
  size_t eff_ham_eff_dim = pinit_state->size();

  LanczosRes<TenT> lancz_res;

  std::vector<std::vector<size_t>> energy_measu_ctrct_axes;
  if (pinit_state->Rank() == 3) {            // For single site update algorithm
    energy_measu_ctrct_axes = {{0, 1, 2}, {0, 1, 2}};
  } else if (pinit_state->Rank() == 4) {    // For two site update algorithm
    energy_measu_ctrct_axes = {{0, 1, 2, 3}, {0, 1, 2, 3}};
  }

  std::vector<TenT *> bases(params.max_iterations, nullptr);
  std::vector<GQTEN_Double> a(params.max_iterations, 0.0);
  std::vector<GQTEN_Double> b(params.max_iterations, 0.0);
  std::vector<GQTEN_Double> N(params.max_iterations, 0.0);

  // Initialize Lanczos iteration.
  pinit_state->Normalize();
  bases[0] = pinit_state;

#ifdef GQMPS2_TIMING_MODE
  Timer mat_vec_timer("lancz_mat_vec");
#endif

  auto last_mat_mul_vec_res = (*eff_ham_mul_state)(eff_ham, bases[0]);

#ifdef GQMPS2_TIMING_MODE
  mat_vec_timer.PrintElapsed();
#endif

  TenT temp_scalar_ten;
  auto base_dag = Dag(*bases[0]);
  Contract(
      last_mat_mul_vec_res, &base_dag,
      energy_measu_ctrct_axes,
      &temp_scalar_ten
  );
  a[0] = Real(temp_scalar_ten());;
  N[0] = 0.0;
  size_t m = 0;
  GQTEN_Double energy0;
  energy0 = a[0];
  // Lanczos iterations.
  while (true) {
    m += 1;
    auto gamma = last_mat_mul_vec_res;
    if (m == 1) {
      LinearCombine({-a[m - 1]}, {bases[m - 1]}, 1.0, gamma);
    } else {
      LinearCombine(
          {-a[m - 1], -std::sqrt(N[m - 1])},
          {bases[m - 1], bases[m - 2]},
          1.0,
          gamma
      );
    }
    auto norm_gamma = gamma->Normalize();
    GQTEN_Double eigval;
    GQTEN_Double *eigvec = nullptr;
    if (norm_gamma == 0.0) {
      if (m == 1) {
        lancz_res.iters = m;
        lancz_res.gs_eng = energy0;
        lancz_res.gs_vec = new TenT(*bases[0]);
        LanczosFree(eigvec, bases, last_mat_mul_vec_res);
        return lancz_res;
      } else {
        TridiagGsSolver(a, b, m, eigval, eigvec, 'V');
        auto gs_vec = new TenT(bases[0]->GetIndexes());
        LinearCombine(m, eigvec, bases, 0.0, gs_vec);
        lancz_res.iters = m;
        lancz_res.gs_eng = energy0;
        lancz_res.gs_vec = gs_vec;
        LanczosFree(eigvec, bases, m, last_mat_mul_vec_res);
        return lancz_res;
      }
    }

    N[m] = norm_gamma * norm_gamma;
    b[m - 1] = norm_gamma;
    bases[m] = gamma;

#ifdef GQMPS2_TIMING_MODE
    mat_vec_timer.ClearAndRestart();
#endif

    last_mat_mul_vec_res = (*eff_ham_mul_state)(eff_ham, bases[m]);

#ifdef GQMPS2_TIMING_MODE
    mat_vec_timer.PrintElapsed();
#endif

    TenT temp_scalar_ten;
    auto base_dag = Dag(*bases[m]);
    Contract(
        last_mat_mul_vec_res,
        &base_dag,
        energy_measu_ctrct_axes,
        &temp_scalar_ten
    );
    a[m] = Real(temp_scalar_ten());
    TridiagGsSolver(a, b, m + 1, eigval, eigvec, 'N');
    auto energy0_new = eigval;
    if (
        ((energy0 - energy0_new) < params.error) ||
            (m == eff_ham_eff_dim) ||
            (m == params.max_iterations - 1)
        ) {
      TridiagGsSolver(a, b, m + 1, eigval, eigvec, 'V');
      energy0 = energy0_new;
      auto gs_vec = new TenT(bases[0]->GetIndexes());
      LinearCombine(m + 1, eigvec, bases, 0.0, gs_vec);
      lancz_res.iters = m + 1;
      lancz_res.gs_eng = energy0;
      lancz_res.gs_vec = gs_vec;
      LanczosFree(eigvec, bases, m + 1, last_mat_mul_vec_res);
      return lancz_res;
    } else {
      energy0 = energy0_new;
    }
  }
}

/*
 * |----1                       1-----
 * |          1        1             |
 * |          |        |             |
 * |          |        |             |
 * |          0        0             |
 * |          1        2             |
 * |          |        |             |
 * |----0 0-------------------3 0----|
 */
template<typename TenElemT, typename QNT>
GQTensor<TenElemT, QNT> *eff_ham_terms_mul_two_site_state(
    const EffectiveHamiltonianTermGroup<GQTensor<TenElemT, QNT>> &eff_ham,
    GQTensor<TenElemT, QNT> *state
) {
  using TenT = GQTensor<TenElemT, QNT>;
  size_t num_terms = eff_ham.size();
  auto multiplication_res = std::vector<TenT>(num_terms);
  auto pmultiplication_res = std::vector<TenT *>(num_terms);
  const std::vector<TenElemT> &coefs = std::vector<TenElemT>(num_terms, TenElemT(1.0));
  for (size_t i = 0; i < num_terms; i++) {
    const EffectiveHamiltonianTerm<GQTensor<TenElemT, QNT>> &term = eff_ham[i];
    TenT temp1, temp2, temp3;
    Contract<TenElemT, QNT, false, true>(*state, *term[0], 0, 0, 1, temp1);
    Contract<TenElemT, QNT, false, true>(temp1, *term[1], 0, 0, 1, temp2);
    Contract<TenElemT, QNT, false, true>(temp2, *term[2], 0, 0, 1, temp3);
    Contract<TenElemT, QNT, false, true>(temp3, *term[3], 0, 0, 1, multiplication_res[i]);
    pmultiplication_res[i] = &multiplication_res[i];
  }

  auto res = new TenT;
  //TODO: optimize the summation
  LinearCombine(coefs, pmultiplication_res, TenElemT(0.0), res);

  return res;
}

} /* gqmps2 */
