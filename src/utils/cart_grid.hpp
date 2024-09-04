#ifndef UTILS_CARTESIAN_GRID_HPP_
#define UTILS_CARTESIAN_GRID_HPP_

//========================================================================================
// AthenaXXX astrophysical plasma code
// Copyright(C) 2020 James M. Stone <jmstone@ias.edu> and the Athena code team
// Licensed under the 3-clause BSD License (the "LICENSE")
//========================================================================================
//! \file cart_grid.hpp
//  \brief definitions for SphericalGrid class

#include "athena.hpp"

// Forward declarations
class MeshBlockPack;

//----------------------------------------------------------------------------------------
//! \class CartesianGrid

class CartesianGrid {
 public:
    // Creates a geodesic grid with refinement level nlev and radius rad
    CartesianGrid(MeshBlockPack *pmy_pack, Real center[3],
                        Real extend[3], int numpoints[3]);
    ~CartesianGrid();

    // parameters for the grid
    Real center_x1, center_x2, center_x3;   // grid centers
    Real min_x1, min_x2, min_x3;            // min for xyz
    Real max_x1, max_x2, max_x3;            // max value for xyz
    Real d_x1, d_x2, d_x3;                     // resolution
    int nx1, nx2, nx3;                      // number of points


    // For simplicity, unravell all points into a 1d array
    DualArray4D<Real> interp_vals;   // container for data interpolated to sphere
    void InterpolateToGrid(int nvars, DvceArray5D<Real> &val);  // interpolate to sphere

 private:
    MeshBlockPack* pmy_pack;  // ptr to MeshBlockPack containing this Hydro
    DualArray4D<int> interp_indcs;   // indices of MeshBlock and zones therein for interp
    DualArray5D<Real> interp_wghts;  // weights for interpolation
    void SetInterpolationCoordinates();  // set indexing for interpolation
    void SetInterpolationIndices();      // set indexing for interpolation
    void SetInterpolationWeights();      // set weights for interpolation
};

#endif // UTILS_CARTESIAN_GRID_HPP_
