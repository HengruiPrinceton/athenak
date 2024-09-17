#include <cmath>
#include <sstream>
#include <unistd.h>
#include <assert.h>
#include <iostream>
#include <fstream>
#include <string>

#if MPI_PARALLEL_ENABLED
#include <mpi.h>
#endif

#include "z4c_ahfind.hpp"
#include "z4c/z4c.hpp"
#include "adm/adm.hpp"
#include "athena.hpp"
#include "parameter_input.hpp"
#include "globals.hpp"
#include "mesh/mesh.hpp"
#include "utils/lagrange_interpolator.hpp"
#include "geodesic-grid/gauss_legendre.hpp"

using namespace z4c;

//----------------------------------------------------------------------------------------
// \brief constructor
//----------------------------------------------------------------------------------------
AHF::AHF(Mesh * pmesh, ParameterInput * pin, int n):
  S{nullptr},
  ah_found{false},
  initial_radius{1},
  center{NAN,NAN,NAN},
  maxit{10},
  pmesh{pmesh} {
    //! Initialize grid parameters given the input
    nlev = pin->GetOrAddInteger("ahfind", "nlev",20);
    int lmax = pin->GetOrAddInteger("ahfind","lmax",8);
    maxit = pin->GetOrAddInteger("ahfind","max_iteration",10);
    nfilt = (lmax+1)*(lmax+1);
    Real initial_radius = pin->GetOrAddReal("ahfind","initial_radius",1);
    auto pmbp = pmesh->pmb_pack;
    S = new GaussLegendreGrid(pmbp, nlev, initial_radius, nfilt);
    // probably need to write a new function to shift the location of the sphere
}

void AHF::FastFlow() {
  auto pmbp = pmesh->pmb_pack;
  auto &indcs = pmbp->pmesh->mb_indcs;
  auto &dg_ddd = pmbp->pz4c->dg_ddd;
  auto &H = null_expansion;

  // Evaluate Surface Null Expansion, Gundlach 1997 eqn 9
  // SurfaceNullExpansion(pmbp, S);
  AnalyticSurfaceNullExpansion(S);

  Real H_integrated = S->Integrate(H);
  std::cout << "Initial Norm of H: " << H_integrated << std::endl;
  std::cout << "Initial Radius: " << S->pointwise_radius.h_view(0) << std::endl;

  std::ofstream spherical_grid_output;
  // alpha and beta parametrization
  // change these to be input options later
  Real alpha = 1.;
  Real beta = 0.5;

  // H-flow Jacobi loop, take A = 1; B = 0; rho = 1
  Real A = alpha/(nfilt*(nfilt+1))+beta;
  Real B = beta/alpha;

  for (int itr=0; itr<maxit; ++itr) {
    // convert to spectral space
    auto H_spectral = S->SpatialToSpectral(H);

    // radius in spectral space
    auto r_spectral = S->SpatialToSpectral(S->pointwise_radius);

    DualArray1D<Real> r_spectral_np1;
    Kokkos::realloc(r_spectral_np1,nfilt);

    for (int i=0; i<nfilt; ++i) {
      int l = (int) sqrt(i);
      // std::cout << r_spectral.h_view(i) << std::endl;
      r_spectral_np1.h_view(i) =  r_spectral.h_view(i) - A/(1+B*l*(l+1))*H_spectral.h_view(i);
    }

    auto r_np1 = S->SpectralToSpatial(r_spectral_np1);

    // reset radius
    S->SetPointwiseRadius(r_np1,center);

    // reevaluate H
    // SurfaceNullExpansion(pmbp, S); 
    AnalyticSurfaceNullExpansion(S);

    H_integrated = S->Integrate(H);

    std::cout << "Itr " << itr+1 << "   Norm of H: " << std::abs(H_integrated) << "\t" << "Radius: " << S->pointwise_radius.h_view(0) << "\t" 
    << "H spectral 0th: "<< H_spectral.h_view(0) <<std::endl;
    if (std::abs(H_integrated)<=1e-5) {
      std::cout << "target residual achieved in " << itr+1 << " iterations; terminating horizon finder..." << std::endl;
      break;
    }
  }
}

//----------------------------------------------------------------------------------------
// \!fn void  SurfaceNullExpansion(MeshBlockPack *pmbp, 
// GaussLegendreGrid *S, DualArray6D<Real> dg_ddd)
// \brief Compute \rho H for horizon finder
//
// This evaluate the surface null expansion multiplied by a weighting function \rho.
// 
//----------------------------------------------------------------------------------------
void AHF::SurfaceNullExpansion(MeshBlockPack *pmbp, GaussLegendreGrid *S) {
  // Load adm variables
  auto &adm = pmbp->padm->adm;
  auto &g_dd = adm.g_dd;
  auto &K_dd = adm.vK_dd;

  int nangles = S->nangles;
  auto surface_jacobian = S->surface_jacobian;
  auto d_surface_jacobian = S->d_surface_jacobian;
  auto dg_ddd = pmbp->pz4c->dg_ddd;

  // *****************  Step 4 of Schnetter 2002  *******************

  // Interpolate g_dd, K_dd, and dg_ddd onto the surface

  auto g_dd_surf =  S->InterpolateToSphere(g_dd);
  auto K_dd_surf =  S->InterpolateToSphere(K_dd);
  auto dg_ddd_surf = S->InterpolateToSphere(dg_ddd);

  // Calculating g_uu on the sphere 
  // all tensors on surface ends with "_surf"

  AthenaSurfaceTensor<Real,TensorSymm::SYM2,3,2> g_uu_surf;
  g_uu_surf.NewAthenaSurfaceTensor(nangles);

  for(int n=0; n<nangles; ++n) {
    Real detg = adm::SpatialDet(g_dd_surf(0,0,n), g_dd_surf(0,1,n), g_dd_surf(0,2,n),
                           g_dd_surf(1,1,n), g_dd_surf(1,2,n), g_dd_surf(2,2,n));
    adm::SpatialInv(1.0/detg,
            g_dd_surf(0,0,n), g_dd_surf(0,1,n), g_dd_surf(0,2,n),
            g_dd_surf(1,1,n), g_dd_surf(1,2,n), g_dd_surf(2,2,n),
            &g_uu_surf(0,0,n), &g_uu_surf(0,1,n), &g_uu_surf(0,2,n),
            &g_uu_surf(1,1,n), &g_uu_surf(1,2,n), &g_uu_surf(2,2,n));
  }

  // Christoffel symbols of the second kind on the surface, saved as rank3 tensor

  AthenaSurfaceTensor<Real,TensorSymm::SYM2,3,3> Gamma_udd_surf;
  Gamma_udd_surf.NewAthenaSurfaceTensor(nangles);

  for(int n=0; n<nangles; ++n) {
    for(int i=0; i<3; ++i)
    for(int j=0; j<3; ++j)
    for(int k=j; k<3; ++k) {
      Gamma_udd_surf(i,j,k,n) = 0;
      for(int s=0; s<3; ++s) {
        Gamma_udd_surf(i,j,k,n) += 0.5*g_uu_surf(i,s,n) * (dg_ddd_surf(j,k,s,n)
                                  +dg_ddd_surf(k,s,j,n)-dg_ddd_surf(s,j,k,n));
      }
    }
  }

  // *****************  Step 6 of Schnetter 2002  *******************
  // Evaluate Derivatives of F = r - h(theta,phi)
  // First in spherical components "_sb" stands for spherical basis

  AthenaSurfaceTensor<Real,TensorSymm::NONE,3,1> dF_d_surf_sb;
  dF_d_surf_sb.NewAthenaSurfaceTensor(nangles);

  DualArray1D<Real> place_holder_for_partial_theta;
  DualArray1D<Real> place_holder_for_partial_phi;
  Kokkos::realloc(place_holder_for_partial_theta,nangles);
  Kokkos::realloc(place_holder_for_partial_phi,nangles);

  place_holder_for_partial_theta = S->ThetaDerivative(S->pointwise_radius); // these should vanish!
  place_holder_for_partial_phi = S->PhiDerivative(S->pointwise_radius);

  for(int n=0; n<nangles; ++n) {
    // radial derivatives
    dF_d_surf_sb(0,n) = 1;
    // theta and phi derivatives
    dF_d_surf_sb(1,n) = place_holder_for_partial_theta.h_view(n);
    dF_d_surf_sb(2,n) = place_holder_for_partial_phi.h_view(n);
  }

  // Evaluate Second Derivatives of F in spherical components
  AthenaSurfaceTensor<Real,TensorSymm::SYM2,3,2> ddF_dd_surf_sb;
  ddF_dd_surf_sb.NewAthenaSurfaceTensor(nangles);

  DualArray1D<Real> place_holder_for_second_partials;
  Kokkos::realloc(place_holder_for_second_partials,nangles);

  // all second derivatives w.r.t. r vanishes as dr_F = 1
  for(int n=0; n<nangles; ++n) {
    ddF_dd_surf_sb(0,0,n) = 0;
    ddF_dd_surf_sb(0,1,n) = 0;
    ddF_dd_surf_sb(0,2,n) = 0;
  }
  // tt
  place_holder_for_second_partials = S->ThetaDerivative(place_holder_for_partial_theta);
  for(int n=0; n<nangles; ++n) {
    ddF_dd_surf_sb(1,1,n) = place_holder_for_second_partials.h_view(n);
  }
  // tp
  place_holder_for_second_partials = S->PhiDerivative(place_holder_for_partial_theta);
  for(int n=0; n<nangles; ++n) {
    ddF_dd_surf_sb(1,2,n) = place_holder_for_second_partials.h_view(n);
  }
  // pp
  place_holder_for_second_partials = S->PhiDerivative(place_holder_for_partial_phi);
  for(int n=0; n<nangles; ++n) {
    ddF_dd_surf_sb(2,2,n) = place_holder_for_second_partials.h_view(n);
  }

  // Convert to derivatives of F to cartesian basis
  AthenaSurfaceTensor<Real,TensorSymm::NONE,3,1> dF_d_surf;
  dF_d_surf.NewAthenaSurfaceTensor(nangles);

  // check the Surface Jacobian
  for(int n=0; n<nangles; ++n) {
    for(int i=0; i<3;++i) {
      dF_d_surf(i,n) = 0;
      for(int u=0; u<3;++u) {
        dF_d_surf(i,n) += surface_jacobian.h_view(n,u,i)*dF_d_surf_sb(u,n);
      }
    }
  }

  // Second Covariant derivatives of F in cartesian basis
  AthenaSurfaceTensor<Real,TensorSymm::SYM2,3,2> ddF_dd_surf;
  ddF_dd_surf.NewAthenaSurfaceTensor(nangles);
  for(int n=0; n<nangles; ++n) {
    for(int i=0; i<3;++i) {
      for(int j=0; j<3;++j) {
        ddF_dd_surf(i,j,n) = 0;
        for(int v=0; v<3;++v) {
          ddF_dd_surf(i,j,n) += d_surface_jacobian.h_view(n,i,v,j)*dF_d_surf_sb(v,n);
          ddF_dd_surf(i,j,n) += -Gamma_udd_surf(v,i,j,n)*dF_d_surf(v,n);
          for(int u=0; u<3;++u) {
            ddF_dd_surf(i,j,n) += surface_jacobian.h_view(n,v,j)*surface_jacobian.h_view(n,u,i)
                                                  *ddF_dd_surf_sb(u,v,n);
          }
        }
      }
    }
  }

  // auxiliary variable delta_F_abs, Gundlach 1997 eqn 8

  AthenaSurfaceTensor<Real,TensorSymm::NONE,3,0> delta_F_abs;
  delta_F_abs.NewAthenaSurfaceTensor(nangles);

  for(int n=0; n<nangles; ++n) {
    Real delta_F_sqr = 0;
    for(int i=0; i<3;++i) {
      for(int j=0; j<3;++j) {
        delta_F_sqr += g_uu_surf(i,j,n)*dF_d_surf(i,n)
                                *dF_d_surf(j,n);
      }
    }
    delta_F_abs(n) = sqrt(delta_F_sqr);
  }

  // contravariant form of delta_F

  AthenaSurfaceTensor<Real,TensorSymm::NONE,3,1> dF_u_surf;
  dF_u_surf.NewAthenaSurfaceTensor(nangles);

  for(int n=0; n<nangles; ++n) {
    for(int i=0; i<3;++i) {
      dF_u_surf(i,n) = 0;
      for(int j=0; j<3;++j) {
        dF_u_surf(i,n) += g_uu_surf(i,j,n)*dF_d_surf(j,n);
      }
    }
  }

  // Surface unit normal (in cartesian coordinate), Gundlach 1997 eqn 8
  AthenaSurfaceTensor<Real,TensorSymm::NONE,3,1> s;
  s.NewAthenaSurfaceTensor(nangles);

  for(int n=0; n<nangles; ++n) {
    for(int i=0; i<3;++i) {
      s(i,n) = 0;
      for(int j=0; j<3;++j) {
        s(i,n) += g_uu_surf(i,j,n)*dF_d_surf(j,n)/delta_F_abs(n);
      }
    }
  }

  // projection op (in cartesian coordinate), Gundlach 1997 eqn 28
  AthenaSurfaceTensor<Real,TensorSymm::SYM2,3,2> p1_uu_surf;
  p1_uu_surf.NewAthenaSurfaceTensor(nangles);

  for(int n=0; n<nangles; ++n) {
    for(int i=0; i<3;++i) {
      for(int j=0; j<3;++j) {
        p1_uu_surf(i,j,n) = g_uu_surf(i,j,n) - s(i,n)*s(j,n);
      }
    }
  }

  // Convert to derivatives of F to cartesian basis
  AthenaSurfaceTensor<Real,TensorSymm::NONE,3,1> dr_d_surf;
  dr_d_surf.NewAthenaSurfaceTensor(nangles);

  for(int n=0; n<nangles; ++n) {
    for(int i=0; i<3;++i) {
      dr_d_surf(i,n) = surface_jacobian.h_view(n,0,i);
    }
  }

  // Flat projection operator, Gundlach 1997 eqn 25
  AthenaSurfaceTensor<Real,TensorSymm::SYM2,3,2> p2_dd_surf;
  p2_dd_surf.NewAthenaSurfaceTensor(nangles);

  double gbar_uu;
  for(int n=0; n<nangles; ++n) {
    for(int i=0; i<3;++i) {
      for(int j=0; j<3;++j) {
        if (i==j) {
          gbar_uu = 1.;
        } else {
          gbar_uu = 0.;
        }
        p2_dd_surf(i,j,n) = gbar_uu-dr_d_surf(i,n)*dr_d_surf(j,n);
      }
    }
  }

  // contraction between p1 and p2
  AthenaSurfaceTensor<Real,TensorSymm::NONE,3,0> p1p2;
  p1p2.NewAthenaSurfaceTensor(nangles);
  for(int n=0; n<nangles; ++n) {
    for(int i=0; i<3;++i) {
      for(int j=0; j<3;++j) {
        p1p2(n) += p1_uu_surf(i,j,n)*p2_dd_surf(i,j,n);
      }
    }
  }

  // Weighting function for N-flow, Gundlach 1997 eqn 28
  AthenaSurfaceTensor<Real,TensorSymm::NONE,3,0> rho;
  rho.NewAthenaSurfaceTensor(nangles);
  for(int n=0; n<nangles; ++n) {
    rho(n) = 2*pow(S->pointwise_radius.h_view(n),2)*delta_F_abs(n)/p1p2(n);
  }

  // Surface inverse metric (in cartesian coordinate), Gundlach 1997 eqn 9
  AthenaSurfaceTensor<Real,TensorSymm::SYM2,3,2> m_uu_surf;
  m_uu_surf.NewAthenaSurfaceTensor(nangles);

  for(int n=0; n<nangles; ++n) {
    for(int i=0; i<3;++i) {
      for(int j=0; j<3;++j) {
        m_uu_surf(i,j,n) = g_uu_surf(i,j,n)
                - dF_u_surf(i,n)*dF_u_surf(j,n)
                /delta_F_abs(n)/delta_F_abs(n);
      }
    }
  }

  // Surface Null Expansion, Gundlach 1997 eqn 9
  //AthenaSurfaceTensor<Real,TensorSymm::NONE,3,0> H;
  //H.NewAthenaSurfaceTensor(nangles);

  auto &H = null_expansion;

  for(int n=0; n<nangles; ++n) {
    H(n) = 0;
    for(int i=0; i<3;++i) {
      for(int j=0; j<3;++j) {
        H(n) += m_uu_surf(i,j,n)*(ddF_dd_surf(i,j,n)
                        /delta_F_abs(n)-K_dd_surf(i,j,n))*delta_F_abs(n);//*rho(n); // last term added to give mean curvature flow
      }
    }
  }
}

// Analytical surface null expansion for Schwarzschild in isotropic coordinate, testing only
void AHF::AnalyticSurfaceNullExpansion(GaussLegendreGrid *S) {
  int nangles = S->nangles;
  auto &H = null_expansion;

  auto r = S->pointwise_radius;
  for(int n=0; n<nangles; ++n) {
    Real denominator = 2*r.h_view(n)+1;
    H(n) = 8*r.h_view(n)*(2*r.h_view(n)-1)/(denominator*denominator*denominator);
  }
}
