//========================================================================================
// AthenaXXX astrophysical plasma code
// Copyright(C) 2020 James M. Stone <jmstone@ias.edu> and the Athena code team
// Licensed under the 3-clause BSD License (the "LICENSE")
//========================================================================================
//! \file vtk.cpp
//  \brief writes output data in (legacy) vtk format.
//  Data is written in RECTILINEAR_GRID geometry, in BINARY format, and in FLOAT type
//  Data over multiple MeshBlocks and MPI ranks is written to a single file using MPI-IO.

#include <algorithm>
#include <cstdio>      // fwrite(), fclose(), fopen(), fnprintf(), snprintf()
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>  // mkdir

#include "athena.hpp"
#include "coordinates/cell_locations.hpp"
#include "globals.hpp"
#include "mesh/mesh.hpp"
#include "hydro/hydro.hpp"
#include "outputs.hpp"

//----------------------------------------------------------------------------------------
// ctor: also calls OutputType base class constructor
// Checks compatibility options for VTK outputs

VTKOutput::VTKOutput(OutputParameters op, Mesh *pm)
  : OutputType(op, pm)
{
  // create directories for outputs. Comments in binary.cpp constructor explain why
  mkdir("vtk",0775);
}

//----------------------------------------------------------------------------------------
// Functions to detect big endian machine, and to byte-swap 32-bit words.  The vtk
// legacy format requires data to be stored as big-endian.

namespace swap_functions {

int IsBigEndian()
{
  std::int32_t n = 1;
  char *ep = reinterpret_cast<char *>(&n);
  return (*ep == 0); // Returns 1 (true) on a big endian machine
}

inline void Swap4Bytes(void *vdat)
{
  char tmp, *dat = static_cast<char *>(vdat);
  tmp = dat[0];  dat[0] = dat[3];  dat[3] = tmp;
  tmp = dat[1];  dat[1] = dat[2];  dat[2] = tmp;
}

} // namespace swap_functions

//----------------------------------------------------------------------------------------
//! \fn void VTKOutput:::WriteOutputFile(Mesh *pm)
//  \brief Cycles over all MeshBlocks and writes OutputData in (legacy) vtk format
//   All MeshBlocks are written to the same file.

void VTKOutput::WriteOutputFile(Mesh *pm, ParameterInput *pin)
{
  int big_end = swap_functions::IsBigEndian(); // =1 on big endian machine

  // output entire grid unless gid is specified
  int nout1,nout2,nout3;
  if ((pm->nmb_total > 1) && (out_params.gid < 0)) {
    nout1 = (out_params.slice1)? 1 : (pm->mesh_indcs.nx1);
    nout2 = (out_params.slice2)? 1 : (pm->mesh_indcs.nx2);
    nout3 = (out_params.slice3)? 1 : (pm->mesh_indcs.nx3);
  } else {
    nout1 = outmbs[0].oie - outmbs[0].ois + 1;
    nout2 = outmbs[0].oje - outmbs[0].ojs + 1;
    nout3 = outmbs[0].oke - outmbs[0].oks + 1;
  }
  int ncoord1 = (nout1 > 1)? nout1+1 : nout1;
  int ncoord2 = (nout2 > 1)? nout2+1 : nout2;
  int ncoord3 = (nout3 > 1)? nout3+1 : nout3;

  // allocate 1D vector of floats used to convert and output data
  float *data;
  int ndata = std::max(std::max(ncoord1, ncoord2), ncoord3);
  data = new float[ndata];

  // create filename: "vtk/file_basename" + "." + "file_id" + "." + XXXXX + ".vtk"
  // where XXXXX = 5-digit file_number
  std::string fname;
  char number[6];
  std::snprintf(number, sizeof(number), "%05d", out_params.file_number);

  fname.assign("vtk/");
  fname.append(out_params.file_basename);
  fname.append(".");
  fname.append(out_params.file_id);
  fname.append(".");
  fname.append(number);
  fname.append(".vtk");

  IOWrapper vtkfile;
  std::size_t header_offset=0;
  vtkfile.Open(fname.c_str(), IOWrapper::FileMode::write);

  // There are five basic parts to the VTK "legacy" file format.
  //  1. File version and identifier
  //  2. Header
  //  3. File format
  //  4. Dataset structure, including type and dimensions of data, and coordinates.
  {std::stringstream msg;
  msg << "# vtk DataFile Version 2.0" << std::endl
      << "# Athena++ data at time= " << pm->time
      << "  level= 0"  // assuming uniform mesh
      << "  nranks= " << global_variable::nranks
      << "  cycle=" << pm->ncycle
      << "  variables=" << GetOutputVariableString(out_params.variable).c_str()
      << std::endl << "BINARY" << std::endl
      << "DATASET STRUCTURED_POINTS" << std::endl
      << "DIMENSIONS " << ncoord1 << " " << ncoord2 << " " << ncoord3 << std::endl;
  vtkfile.Write(msg.str().c_str(),sizeof(char),msg.str().size());
  header_offset = msg.str().size();}

  // Specify the uniform Cartesian mesh with grid minima and spacings
  {std::stringstream msg;
  // output physical dimensions of entire grid, unless gid is specified
  Real x1min,x2min,x3min,dx1,dx2,dx3;
  if (out_params.gid < 0) {
    x1min = pm->mesh_size.x1min;
    x2min = pm->mesh_size.x2min;
    x3min = pm->mesh_size.x3min;
    dx1 = pm->mesh_size.dx1;
    dx2 = pm->mesh_size.dx2;
    dx3 = pm->mesh_size.dx3;
  } else {
    x1min = pm->pmb_pack->pmb->mb_size.h_view(out_params.gid).x1min;
    x2min = pm->pmb_pack->pmb->mb_size.h_view(out_params.gid).x2min;
    x3min = pm->pmb_pack->pmb->mb_size.h_view(out_params.gid).x3min;
    dx1 = pm->pmb_pack->pmb->mb_size.h_view(out_params.gid).dx1;
    dx2 = pm->pmb_pack->pmb->mb_size.h_view(out_params.gid).dx2;
    dx3 = pm->pmb_pack->pmb->mb_size.h_view(out_params.gid).dx3;
  }
  if (out_params.include_gzs) {
    x1min -= (pm->pmb_pack->pmesh->mb_indcs.ng)*dx1;
    x2min -= (pm->pmb_pack->pmesh->mb_indcs.ng)*dx2;
    x3min -= (pm->pmb_pack->pmesh->mb_indcs.ng)*dx3;
  }
  msg << std::scientific << std::setprecision(std::numeric_limits<Real>::max_digits10 - 1)
      << "ORIGIN " << x1min << " " << x2min << " " << x3min << " " <<  std::endl
      << "SPACING " << dx1  << " " << dx2   << " " << dx3   << " " <<  std::endl;
  vtkfile.Write(msg.str().c_str(),sizeof(char),msg.str().size());
  header_offset += msg.str().size();}

  //  5. Data.  An arbitrary number of scalars and vectors can be written (every node
  //  in the OutputData doubly linked lists), all in binary floats format
  {std::stringstream msg;
  msg << std::endl << "CELL_DATA " << nout1*nout2*nout3 << std::endl;
  vtkfile.Write(msg.str().c_str(),sizeof(char),msg.str().size());
  header_offset += msg.str().size();}

  // Loop over variables
  int nout_vars = outvars.size();
  int nout_mbs = (outmbs.size());
  for (int n=0; n<nout_vars; ++n) {
    // write data type (SCALARS or VECTORS) and name
    {std::stringstream msg;
    msg << std::endl << "SCALARS " << outvars[n].label.c_str() << " float" << std::endl
        << "LOOKUP_TABLE default" << std::endl;
    vtkfile.Write_at_all(msg.str().c_str(),sizeof(char),msg.str().size(),header_offset);
    header_offset += msg.str().size();}

    // Loop over MeshBlocks
    for (int m=0; m<nout_mbs; ++m) {
      auto &indcs = pm->pmb_pack->pmesh->mb_indcs;
      // in 3D grid of output MeshBlocks, calculate integer position of this
      LogicalLocation lloc = pm->lloclist[outmbs[m].mb_gid];
      int noutput_mb1=lloc.lx1;
      int noutput_mb2=lloc.lx2;
      int noutput_mb3=lloc.lx3;
      if (out_params.slice1 || (out_params.gid >= 0)) {noutput_mb1=0;}
      if (out_params.slice2 || (out_params.gid >= 0)) {noutput_mb2=0;}
      if (out_params.slice3 || (out_params.gid >= 0)) {noutput_mb3=0;}
      int &mb_nx1 = indcs.nx1;
      int &mb_nx2 = indcs.nx2;
      int &mb_nx3 = indcs.nx3;
      // use integer position of this output MeshBlock to compute data offset
      size_t data_offset = (noutput_mb1*mb_nx1 + noutput_mb2*(mb_nx2*nout1) +
      noutput_mb3*(mb_nx3*nout1*nout2))*sizeof(float);

      int &ois = outmbs[m].ois;
      int &oie = outmbs[m].oie;
      int &ojs = outmbs[m].ojs;
      int &oje = outmbs[m].oje;
      int &oks = outmbs[m].oks;
      int &oke = outmbs[m].oke;
      for (int k=oks; k<=oke; ++k) {
        for (int j=ojs; j<=oje; ++j) {
          for (int i=ois; i<=oie; ++i) {
            data[i-ois] = static_cast<float>(outdata(n,m,k-oks,j-ojs,i-ois));
          }

          // write data in big endian order
          if (!big_end) {
            for (int i=0; i<(oie-ois+1); ++i)
              swap_functions::Swap4Bytes(&data[i]);
          }
          size_t my_offset = header_offset + data_offset +
                             ((j-ojs)*nout1 + (k-oks)*nout1*nout2)*sizeof(float);
          vtkfile.Write_at_all(&data[0], sizeof(float), (oie-ois+1), my_offset);
        }
      }
    }  // end loop over MeshBlocks
    header_offset += (nout1*nout2*nout3)*sizeof(float);
  }

  // close the output file and clean up ptrs to data
  vtkfile.Close();
  delete [] data;

  // increment counters
  out_params.file_number++;
  if (out_params.last_time < 0.0) {
    out_params.last_time = pm->time;
  } else {
    out_params.last_time += out_params.dt;
  }
  pin->SetInteger(out_params.block_name, "file_number", out_params.file_number);
  pin->SetReal(out_params.block_name, "last_time", out_params.last_time);

  return;
}
