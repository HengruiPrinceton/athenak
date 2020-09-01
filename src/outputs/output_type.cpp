//========================================================================================
// AthenaXXX astrophysical plasma code
// Copyright(C) 2020 James M. Stone <jmstone@ias.edu> and the Athena code team
// Licensed under the 3-clause BSD License (the "LICENSE")
//========================================================================================
//! \file output_type.cpp
//  \brief implements functions in OutputType class
//
// The number and types of outputs are all controlled by the number and values of
// parameters specified in <output[n]> blocks in the input file.  Each output block must
// be labelled by a unique integer "n".  Following the convention of the parser
// implemented in the ParameterInput class, a second output block with the same integer
// "n" of an earlier block will silently overwrite the values read by the first block. The
// numbering of the output blocks does not need to be consecutive, and blocks may appear
// in any order in the input file.  Moreover, unlike the C version of Athena, the total
// number of <output[n]> blocks does not need to be specified -- in Athena++ a new output
// type will be created for each and every <output[n]> block in the input file.
//
// Required parameters that must be specified in an <output[n]> block are:
//   - variable     = cons,prim,D,d,E,e,m,m1,m2,m3,v,v1=vx,v2=vy,v3=vz,p,
//                    bcc,bcc1,bcc2,bcc3,b,b1,b2,b3,phi,uov
//   - file_type    = rst,tab,vtk,hst,hdf5
//   - dt           = problem time between outputs
//
// EXAMPLE of an <output[n]> block for a VTK dump:
//   <output3>
//   file_type   = tab       # Tabular data dump
//   variable    = prim      # variables to be output
//   data_format = %12.5e    # Optional data format string
//   dt          = 0.01      # time increment between outputs
//   x2_slice    = 0.0       # slice in x2
//   x3_slice    = 0.0       # slice in x3
//
// Each <output[n]> block will result in a new node being created in a linked list of
// OutputType stored in the Outputs class.  During a simulation, outputs are made when
// the simulation time satisfies the criteria implemented in the XXXX
//
// To implement a new output type, write a new OutputType derived class, and construct
// an object of this class in the Outputs constructor at the location indicated by the
// comment text: 'NEW_OUTPUT_TYPES'.
//========================================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>    // strcmp
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>   // std::string, to_string()

#include "athena.hpp"
#include "athena_arrays.hpp"
#include "parameter_input.hpp"
#include "mesh/mesh.hpp"
#include "hydro/hydro.hpp"
#include "outputs.hpp"

//----------------------------------------------------------------------------------------
// OutputType base class constructor
// Sets parameters like size and indices of output arrays

OutputType::OutputType(OutputParameters opar, Mesh *pm) :
   out_params(opar)
{
  // set size & starting indices of output arrays, adjusted accordingly if gz included 
  // Since all MeshBlocks the same, only need to compute values from first MB
  auto it = pm->mblocks.begin();
  if (out_params.include_gzs) {
    nout1 = it->mb_cells.nx1 + 2*(it->mb_cells.ng);
    nout2 = (it->mb_cells.nx2 > 1)? (it->mb_cells.nx2 + 2*(it->mb_cells.ng)) : 1;
    nout3 = (it->mb_cells.nx3 > 1)? (it->mb_cells.nx3 + 2*(it->mb_cells.ng)) : 1;
    ois = 0;
    ojs = 0;
    oks = 0;
  } else {
    nout1 = it->mb_cells.nx1;
    nout2 = it->mb_cells.nx2;
    nout3 = it->mb_cells.nx3;
    ois = it->mb_cells.is;
    ojs = it->mb_cells.js;
    oks = it->mb_cells.ks;
  }

  // reset array dimensions if data is being sliced
  if (out_params.slice1) { nout1 = 1; }
  if (out_params.slice2) { nout2 = 1; }
  if (out_params.slice3) { nout3 = 1; }

  // exit for history files
  if (out_params.file_type.compare("hst") == 0) {return;}

  // calculate spatial positions of output data
  x1_cc_.SetSize(pm->nmbthisrank, nout1);
  x1_fc_.SetSize(pm->nmbthisrank, nout1+1);
  x1_cc_.SetLabel("x1v");
  x1_fc_.SetLabel("x1f");
  x2_cc_.SetSize(pm->nmbthisrank, nout2);
  x2_fc_.SetSize(pm->nmbthisrank, nout2+1);
  x2_cc_.SetLabel("x2v");
  x2_fc_.SetLabel("x2f");
  x3_cc_.SetSize(pm->nmbthisrank, nout3);
  x3_fc_.SetSize(pm->nmbthisrank, nout3+1);
  x3_cc_.SetLabel("x3v");
  x3_fc_.SetLabel("x3f");

  // TODO get working with multuple meshblocks
  auto pmb = pm->mblocks.begin();
  for (int n=0; n<pm->nmbthisrank; ++n) {
    for (int i=0; i<nout1; ++i) {
      x1_cc_(n,i) = pm->CellCenterX((i-(pmb->mb_cells.is - ois)),
         pmb->mb_cells.nx1, pmb->mb_size.x1min, pmb->mb_size.x1max);
      x1_fc_(n,i) = pm->LeftEdgeX((i-(pmb->mb_cells.is - ois)),
         pmb->mb_cells.nx1, pmb->mb_size.x1min, pmb->mb_size.x1max);
    }
    x1_fc_(n,nout1) = pmb->mb_size.x1max;

    for (int j=0; j<nout2; ++j) {
      x2_cc_(n,j) = pm->CellCenterX((j-(pmb->mb_cells.js - ojs)),
         pmb->mb_cells.nx2, pmb->mb_size.x2min, pmb->mb_size.x2max);
      x2_fc_(n,j) = pm->LeftEdgeX((j-(pmb->mb_cells.js - ojs)),
         pmb->mb_cells.nx2, pmb->mb_size.x2min, pmb->mb_size.x2max);
    }
    x2_fc_(n,nout2) = pmb->mb_size.x2max;

    for (int k=0; k<nout3; ++k) {
      x3_cc_(n,k) = pm->CellCenterX((k-(pmb->mb_cells.ks - oks)),
         pmb->mb_cells.nx3, pmb->mb_size.x3min, pmb->mb_size.x3max);
      x3_cc_(n,k) = pm->LeftEdgeX((k-(pmb->mb_cells.ks - oks)),
         pmb->mb_cells.nx3, pmb->mb_size.x3min, pmb->mb_size.x3max);
    }
    x3_fc_(n,nout3) = pmb->mb_size.x3max;
  }

  // parse list of variables for each physics and flag variables to be output
  // hydro conserved variables
  int &nhydro = pm->mblocks.begin()->phydro->nhydro;
  hydro_cons_out_vars.SetSize(nhydro);
  for (int n=0; n<nhydro; ++n) { hydro_cons_out_vars(n) = false; }

  if (out_params.variable.compare("cons") == 0) {
    for (int n=0; n<nhydro; ++n) { hydro_cons_out_vars(n) = true; }
  }
  if (out_params.variable.compare("D") == 0)  { hydro_cons_out_vars(hydro::IDN) = true; }
  if (out_params.variable.compare("E") == 0)  { hydro_cons_out_vars(hydro::IEN) = true; }
  if (out_params.variable.compare("M1") == 0) { hydro_cons_out_vars(hydro::IM1) = true; }
  if (out_params.variable.compare("M2") == 0) { hydro_cons_out_vars(hydro::IM2) = true; }
  if (out_params.variable.compare("M3") == 0) { hydro_cons_out_vars(hydro::IM3) = true; }
  if (out_params.variable.compare("mom") == 0) {
    hydro_cons_out_vars(hydro::IM1) = true;
    hydro_cons_out_vars(hydro::IM2) = true;
    hydro_cons_out_vars(hydro::IM3) = true;
  }

  // hydro primitive variables
  hydro_prim_out_vars.SetSize(nhydro);
  for (int n=0; n<nhydro; ++n) { hydro_prim_out_vars(n) = false; }

  if (out_params.variable.compare("prim") == 0) {
    for (int n=0; n<nhydro; ++n) { hydro_prim_out_vars(n) = true; }
  }
  if (out_params.variable.compare("d") == 0)  { hydro_prim_out_vars(hydro::IDN) = true; }
  if (out_params.variable.compare("p") == 0)  { hydro_prim_out_vars(hydro::IPR) = true; }
  if (out_params.variable.compare("vx") == 0) { hydro_prim_out_vars(hydro::IVX) = true; }
  if (out_params.variable.compare("vy") == 0) { hydro_prim_out_vars(hydro::IVY) = true; }
  if (out_params.variable.compare("vz") == 0) { hydro_prim_out_vars(hydro::IVZ) = true; }
  if (out_params.variable.compare("vel") == 0) {
    hydro_prim_out_vars(hydro::IVX) = true;
    hydro_prim_out_vars(hydro::IVY) = true;
    hydro_prim_out_vars(hydro::IVZ) = true;
  }

  // check for valid output variable in <input> block
  int cnt=0;
  for (int n=0; n<nhydro; ++n) {
    if (hydro_cons_out_vars(n)) ++cnt;
    if (hydro_prim_out_vars(n)) ++cnt;
  }
  if (cnt==0) {
    std::cout << "### FATAL ERROR in " << __FILE__ << " at line " << __LINE__ << std::endl
       << "Output variable '" << out_params.variable << "' not implemented" << std::endl
       << "Allowed hydro variables: cons,D,E,mom,M1,M2,M3,prim,d,p,vel,vx,vy,vz"
       << std::endl;
    exit(EXIT_FAILURE);
  }
}

//----------------------------------------------------------------------------------------
// OutputType::LoadOutputData()
// create std::vector of AthenaArrays containing data specified in <output> block for
// this output type

void OutputType::LoadOutputData(Mesh *pm)
{
  out_data_.clear();  // start with a clean list

  // the out_data_ vector stores each variable to be output over all cells and MeshBlocks.
  // So start iteration over elements of out_data_ vector (variables)

  // TODO: get this working for multiple physics, which may be either defined/undef

  // output hydro conserved
  int &nhydro = pm->mblocks.begin()->phydro->nhydro;
  for (int n=0; n<nhydro; ++n) {
    if (hydro_cons_out_vars(n)) { // variable exists for output
      AthenaArray<Real> new_data;
      new_data.SetSize(pm->nmbthisrank, nout3, nout2, nout1);
      if (n == hydro::IDN) new_data.SetLabel("dens");
      if (n == hydro::IEN) new_data.SetLabel("tote");
      if (n == hydro::IM1) new_data.SetLabel("mom1");
      if (n == hydro::IM2) new_data.SetLabel("mom2");
      if (n == hydro::IM3) new_data.SetLabel("mom3");

      // loop over all MeshBlocks
      int imb=0;
      for (auto &mb : pm->mblocks) {
        int islice=0, jslice=0, kslice=0;
        //TODO fix this so there is no output if slice is out of range
        if (out_params.slice1) { 
          islice = pm->CellCenterIndex(out_params.slice_x1, mb.mb_cells.nx1,
            mb.mb_size.x1min, mb.mb_size.x1max);
        }   
        if (out_params.slice2) {
          jslice = pm->CellCenterIndex(out_params.slice_x2, mb.mb_cells.nx2,
            mb.mb_size.x2min, mb.mb_size.x2max);
        }   
        if (out_params.slice3) {
          kslice = pm->CellCenterIndex(out_params.slice_x3, mb.mb_cells.nx3,
            mb.mb_size.x3min, mb.mb_size.x3max);
        }
        
        // loop over all cells
        // deep copy one array for each MeshBlock on this rank
        // note the complicated addressing of array indices.  The output array is always
        // include ghost zones (unless needed), so it is always addressed starting at 0.
        // When the array is sliced, only the value at (ijk)slice is stored.
        for (int k=0; k<nout3; ++k) {
        for (int j=0; j<nout2; ++j) {
        for (int i=0; i<nout1; ++i) {
          new_data(imb,k,j,i) =
             mb.phydro->u0(n,(k+oks+kslice),(j+ojs+jslice),(i+ois+islice));
        }}}
        ++imb;
      }
      // append this variable to end of out_data_ vector
      out_data_.push_back(new_data);
    }
  }

  // output hydro primitive
  for (int n=0; n<nhydro; ++n) {
    if (hydro_prim_out_vars(n)) { // variable exists for output
      AthenaArray<Real> new_data;
      new_data.SetSize(pm->nmbthisrank, nout3, nout2, nout1);
      if (n == hydro::IDN) new_data.SetLabel("dens");
      if (n == hydro::IEN) new_data.SetLabel("pres");
      if (n == hydro::IM1) new_data.SetLabel("velx");
      if (n == hydro::IM2) new_data.SetLabel("vely");
      if (n == hydro::IM3) new_data.SetLabel("velz");
      
      // loop over all MeshBlocks
      int imb=0; 
      for (auto &mb : pm->mblocks) {
        int islice=0, jslice=0, kslice=0;
        //TODO fix this so there is no output if slice is out of range
        if (out_params.slice1) { 
          islice = pm->CellCenterIndex(out_params.slice_x1, mb.mb_cells.nx1,
            mb.mb_size.x1min, mb.mb_size.x1max);
        }   
        if (out_params.slice2) {
          jslice = pm->CellCenterIndex(out_params.slice_x2, mb.mb_cells.nx2,
            mb.mb_size.x2min, mb.mb_size.x2max);
        }   
        if (out_params.slice3) {
          kslice = pm->CellCenterIndex(out_params.slice_x3, mb.mb_cells.nx3,
            mb.mb_size.x3min, mb.mb_size.x3max);
        }
        
        // loop over all cells
        // deep copy one array for each MeshBlock on this rank
        // note the complicated addressing of array indices.  The output array is always
        // include ghost zones (unless needed), so it is always addressed starting at 0.
        // When the array is sliced, only the value at (ijk)slice is stored.
        for (int k=0; k<nout3; ++k) {
        for (int j=0; j<nout2; ++j) {
        for (int i=0; i<nout1; ++i) {
          new_data(imb,k,j,i) =
             mb.phydro->w0(n,(k+oks+kslice),(j+ojs+jslice),(i+ois+islice));
        }}}
        ++imb;
      }
      // append this variable to end of out_data_ vector
      out_data_.push_back(new_data);
    }
  }

}
