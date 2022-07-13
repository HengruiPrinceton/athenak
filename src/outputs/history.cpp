//========================================================================================
// AthenaXXX astrophysical plasma code
// Copyright(C) 2020 James M. Stone <jmstone@ias.edu> and the Athena code team
// Licensed under the 3-clause BSD License (the "LICENSE")
//========================================================================================
//! \file history.cpp
//  \brief writes history output data, volume-averaged quantities that are output
//         frequently in time to trace their evolution.

#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "athena.hpp"
#include "globals.hpp"
#include "mesh/mesh.hpp"
#include "eos/eos.hpp"
#include "hydro/hydro.hpp"
#include "mhd/mhd.hpp"
#include "outputs.hpp"

//----------------------------------------------------------------------------------------
// ctor: also calls BaseTypeOutput base class constructor

HistoryOutput::HistoryOutput(OutputParameters op, Mesh *pm) : BaseTypeOutput(op, pm) {
  // cycle through physics modules and add HistoryData struct for each
  hist_data.clear();

  if (pm->pmb_pack->phydro != nullptr) {
    hist_data.emplace_back(PhysicsModule::HydroDynamics);
  }

  if (pm->pmb_pack->pmhd != nullptr) {
    hist_data.emplace_back(PhysicsModule::MagnetoHydroDynamics);
  }

  if (pm->pgen->user_hist) {
    hist_data.emplace_back(PhysicsModule::UserDefined);
  }
}

//----------------------------------------------------------------------------------------
//! \fn void HistoryOutput::LoadOutputData()
//  \brief Wrapper function that cycles through hist_data vector and calls
//  appropriate LoadXXXData() function for that physics

void HistoryOutput::LoadOutputData(Mesh *pm) {
  for (auto &data : hist_data) {
    if (data.physics == PhysicsModule::HydroDynamics) {
      LoadHydroHistoryData(&data, pm);
    } else if (data.physics == PhysicsModule::MagnetoHydroDynamics) {
      LoadMHDHistoryData(&data, pm);
    } else if (data.physics == PhysicsModule::UserDefined) {
      (pm->pgen->user_hist_func)(&data, pm);
    }
    // user history output
    if (pm->pgen->user_hist) {
      (pm->pgen->user_hist_func)(&data, pm);
    }
  }
  return;
}

//----------------------------------------------------------------------------------------
//! \fn void HistoryOutput::LoadHydroHistoryData()
//  \brief Compute and store history data over all MeshBlocks on this rank
//  Data is stored in a Real array defined in derived class.

void HistoryOutput::LoadHydroHistoryData(HistoryData *pdata, Mesh *pm) {
  auto &eos_data = pm->pmb_pack->phydro->peos->eos_data;
  int &nhydro_ = pm->pmb_pack->phydro->nhydro;

  // set number of and names of history variables for hydro
  if (eos_data.is_ideal) {
    pdata->nhist = 8;
  } else {
    pdata->nhist = 7;
  }
  pdata->label[IDN] = "mass";
  pdata->label[IM1] = "1-mom";
  pdata->label[IM2] = "2-mom";
  pdata->label[IM3] = "3-mom";
  if (eos_data.is_ideal) {
    pdata->label[IEN] = "tot-E";
  }
  pdata->label[nhydro_  ] = "1-KE";
  pdata->label[nhydro_+1] = "2-KE";
  pdata->label[nhydro_+2] = "3-KE";

  // capture class variabels for kernel
  auto &u0_ = pm->pmb_pack->phydro->u0;
  auto &size = pm->pmb_pack->pmb->mb_size;
  int &nhist_ = pdata->nhist;

  // loop over all MeshBlocks in this pack
  auto &indcs = pm->pmb_pack->pmesh->mb_indcs;
  int is = indcs.is; int nx1 = indcs.nx1;
  int js = indcs.js; int nx2 = indcs.nx2;
  int ks = indcs.ks; int nx3 = indcs.nx3;
  const int nmkji = (pm->pmb_pack->nmb_thispack)*nx3*nx2*nx1;
  const int nkji = nx3*nx2*nx1;
  const int nji  = nx2*nx1;
  array_sum::GlobalSum sum_this_mbp;
  Kokkos::parallel_reduce("HistSums",Kokkos::RangePolicy<>(DevExeSpace(), 0, nmkji),
  KOKKOS_LAMBDA(const int &idx, array_sum::GlobalSum &mbp_sum) {
    // compute n,k,j,i indices of thread
    int m = (idx)/nkji;
    int k = (idx - m*nkji)/nji;
    int j = (idx - m*nkji - k*nji)/nx1;
    int i = (idx - m*nkji - k*nji - j*nx1) + is;
    k += ks;
    j += js;

    Real vol = size.d_view(m).dx1*size.d_view(m).dx2*size.d_view(m).dx3;

    // Hydro conserved variables:
    array_sum::GlobalSum hvars;
    hvars.the_array[IDN] = vol*u0_(m,IDN,k,j,i);
    hvars.the_array[IM1] = vol*u0_(m,IM1,k,j,i);
    hvars.the_array[IM2] = vol*u0_(m,IM2,k,j,i);
    hvars.the_array[IM3] = vol*u0_(m,IM3,k,j,i);
    if (eos_data.is_ideal) {
      hvars.the_array[IEN] = vol*u0_(m,IEN,k,j,i);
    }

    // Hydro KE
    hvars.the_array[nhydro_  ] = vol*0.5*SQR(u0_(m,IM1,k,j,i))/u0_(m,IDN,k,j,i);
    hvars.the_array[nhydro_+1] = vol*0.5*SQR(u0_(m,IM2,k,j,i))/u0_(m,IDN,k,j,i);
    hvars.the_array[nhydro_+2] = vol*0.5*SQR(u0_(m,IM3,k,j,i))/u0_(m,IDN,k,j,i);

    // fill rest of the_array with zeros, if nhist < NHISTORY_VARIABLES
    for (int n=nhist_; n<NHISTORY_VARIABLES; ++n) {
      hvars.the_array[n] = 0.0;
    }

    // sum into parallel reduce
    mbp_sum += hvars;
  }, Kokkos::Sum<array_sum::GlobalSum>(sum_this_mbp));

  // store data into hdata array
  for (int n=0; n<pdata->nhist; ++n) {
    pdata->hdata[n] = sum_this_mbp.the_array[n];
  }

  return;
}

//----------------------------------------------------------------------------------------
//! \fn void HistoryOutput::LoadMHDHistoryData()
//  \brief Compute and store history data over all MeshBlocks on this rank
//  Data is stored in a Real array defined in derived class.

void HistoryOutput::LoadMHDHistoryData(HistoryData *pdata, Mesh *pm) {
  auto &eos_data = pm->pmb_pack->pmhd->peos->eos_data;
  int &nmhd_ = pm->pmb_pack->pmhd->nmhd;

  // set number of and names of history variables for mhd
  if (eos_data.is_ideal) {
    pdata->nhist = 11;
  } else {
    pdata->nhist = 10;
  }
  pdata->label[IDN] = "mass";
  pdata->label[IM1] = "1-mom";
  pdata->label[IM2] = "2-mom";
  pdata->label[IM3] = "3-mom";
  if (eos_data.is_ideal) {
    pdata->label[IEN] = "tot-E";
  }
  pdata->label[nmhd_  ] = "1-KE";
  pdata->label[nmhd_+1] = "2-KE";
  pdata->label[nmhd_+2] = "3-KE";
  pdata->label[nmhd_+3] = "1-ME";
  pdata->label[nmhd_+4] = "2-ME";
  pdata->label[nmhd_+5] = "3-ME";

  // capture class variabels for kernel
  auto &u0_ = pm->pmb_pack->pmhd->u0;
  auto &bx1f = pm->pmb_pack->pmhd->b0.x1f;
  auto &bx2f = pm->pmb_pack->pmhd->b0.x2f;
  auto &bx3f = pm->pmb_pack->pmhd->b0.x3f;
  auto &size = pm->pmb_pack->pmb->mb_size;
  int &nhist_ = pdata->nhist;

  // loop over all MeshBlocks in this pack
  auto &indcs = pm->pmb_pack->pmesh->mb_indcs;
  int is = indcs.is; int nx1 = indcs.nx1;
  int js = indcs.js; int nx2 = indcs.nx2;
  int ks = indcs.ks; int nx3 = indcs.nx3;
  const int nmkji = (pm->pmb_pack->nmb_thispack)*nx3*nx2*nx1;
  const int nkji = nx3*nx2*nx1;
  const int nji  = nx2*nx1;
  array_sum::GlobalSum sum_this_mbp;
  Kokkos::parallel_reduce("HistSums",Kokkos::RangePolicy<>(DevExeSpace(), 0, nmkji),
  KOKKOS_LAMBDA(const int &idx, array_sum::GlobalSum &mbp_sum) {
    // compute n,k,j,i indices of thread
    int m = (idx)/nkji;
    int k = (idx - m*nkji)/nji;
    int j = (idx - m*nkji - k*nji)/nx1;
    int i = (idx - m*nkji - k*nji - j*nx1) + is;
    k += ks;
    j += js;

    Real vol = size.d_view(m).dx1*size.d_view(m).dx2*size.d_view(m).dx3;

    // MHD conserved variables:
    array_sum::GlobalSum hvars;
    hvars.the_array[IDN] = vol*u0_(m,IDN,k,j,i);
    hvars.the_array[IM1] = vol*u0_(m,IM1,k,j,i);
    hvars.the_array[IM2] = vol*u0_(m,IM2,k,j,i);
    hvars.the_array[IM3] = vol*u0_(m,IM3,k,j,i);
    if (eos_data.is_ideal) {
      hvars.the_array[IEN] = vol*u0_(m,IEN,k,j,i);
    }

    // MHD KE
    hvars.the_array[nmhd_  ] = vol*0.5*SQR(u0_(m,IM1,k,j,i))/u0_(m,IDN,k,j,i);
    hvars.the_array[nmhd_+1] = vol*0.5*SQR(u0_(m,IM2,k,j,i))/u0_(m,IDN,k,j,i);
    hvars.the_array[nmhd_+2] = vol*0.5*SQR(u0_(m,IM3,k,j,i))/u0_(m,IDN,k,j,i);

    // MHD ME
    hvars.the_array[nmhd_+3] = vol*0.25*(SQR(bx1f(m,k,j,i+1)) + SQR(bx1f(m,k,j,i)));
    hvars.the_array[nmhd_+4] = vol*0.25*(SQR(bx2f(m,k,j+1,i)) + SQR(bx2f(m,k,j,i)));
    hvars.the_array[nmhd_+5] = vol*0.25*(SQR(bx3f(m,k+1,j,i)) + SQR(bx3f(m,k,j,i)));

    // fill rest of the_array with zeros, if nhist < NHISTORY_VARIABLES
    for (int n=nhist_; n<NHISTORY_VARIABLES; ++n) {
      hvars.the_array[n] = 0.0;
    }

    // sum into parallel reduce
    mbp_sum += hvars;
  }, Kokkos::Sum<array_sum::GlobalSum>(sum_this_mbp));

  // store data into hdata array
  for (int n=0; n<pdata->nhist; ++n) {
    pdata->hdata[n] = sum_this_mbp.the_array[n];
  }

  return;
}

//----------------------------------------------------------------------------------------
//! \fn void HistoryOutput::WriteOutputFile()
//  \brief Cycles through hist_data vector and writes history file for each component

void HistoryOutput::WriteOutputFile(Mesh *pm, ParameterInput *pin) {
  for (auto &data : hist_data) {
    // first, perform in-place sum over all MPI ranks
#if MPI_PARALLEL_ENABLED
    if (global_variable::my_rank == 0) {
      MPI_Reduce(MPI_IN_PLACE, &(data.hdata[0]), data.nhist, MPI_ATHENA_REAL,
         MPI_SUM, 0, MPI_COMM_WORLD);
    } else {
      MPI_Reduce(&(data.hdata[0]), &(data.hdata[0]), data.nhist,
         MPI_ATHENA_REAL, MPI_SUM, 0, MPI_COMM_WORLD);
    }
#endif

    // only the master rank writes the file
    if (global_variable::my_rank == 0) {
      // create filename: "file_basename" + ".physics" + ".hst"
      // There is no file number or id in history output filenames.
      std::string fname;
      fname.assign(out_params.file_basename);
      switch (data.physics) {
        case PhysicsModule::HydroDynamics:
          fname.append(".hydro");
          break;
        case PhysicsModule::MagnetoHydroDynamics:
          fname.append(".mhd");
          break;
        case PhysicsModule::UserDefined:
          fname.append(".user");
          break;
        default:
          break;
      }
      fname.append(".hst");

      // open file for output
      FILE *pfile;
      if ((pfile = std::fopen(fname.c_str(),"a")) == nullptr) {
        std::cout << "### FATAL ERROR in " << __FILE__ << " at line " << __LINE__
          << std::endl << "Output file '" << fname << "' could not be opened" <<std::endl;
        exit(EXIT_FAILURE);
      }

      // Write header, if it has not been written already
      if (!(data.header_written)) {
        int iout = 1;
        std::fprintf(pfile,"# Athena++ history data\n");
        std::fprintf(pfile,"#  [%d]=time      ", iout++);
        std::fprintf(pfile,"[%d]=dt       ", iout++);
        for (int n=0; n<data.nhist; ++n) {
          std::fprintf(pfile,"[%d]=%.5s    ", iout++, data.label[n].c_str());
        }
        std::fprintf(pfile,"\n");                              // terminate line
        data.header_written = true;
      }

      // write history variables
      std::fprintf(pfile, out_params.data_format.c_str(), pm->time);
      std::fprintf(pfile, out_params.data_format.c_str(), pm->dt);
      for (int n=0; n<data.nhist; ++n)
        std::fprintf(pfile, out_params.data_format.c_str(), data.hdata[n]);
      std::fprintf(pfile,"\n"); // terminate line
      std::fclose(pfile);
    }
  } // End loop over hist_data vector

  // increment counters, clean up
  if (out_params.last_time < 0.0) {
    out_params.last_time = pm->time;
  } else {
    out_params.last_time += out_params.dt;
  }
  pin->SetReal(out_params.block_name, "last_time", out_params.last_time);
  return;
}
