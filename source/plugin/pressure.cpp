/******************************************************************************
 *
 * MantaFlow fluid solver framework
 * Copyright 2011 Tobias Pfaff, Nils Thuerey 
 *
 * This program is free software, distributed under the terms of the
 * GNU General Public License (GPL) 
 * http://www.gnu.org/licenses
 *
 * Plugins for pressure correction: solve_pressure, and ghost fluid helpers
 *
 ******************************************************************************/
#include "vectorbase.h"
#include "kernel.h"
#include "conjugategrad.h"
#include "particle.h"

using namespace std;
namespace Manta {

//! Kernel: Construct the right-hand side of the poisson equation
KERNEL(bnd=1, reduce=+) returns(int cnt=0) returns(double sum=0)
void MakeRhs (FlagGrid& flags, Grid<Real>& rhs, MACGrid& vel, 
			  Grid<Real>* perCellCorr, MACGrid* fractions)
{
	if (!flags.isFluid(i,j,k)) {
		rhs(i,j,k) = 0;
		return;
	}
	   
	// compute divergence 
	// no flag checks: assumes vel at obstacle interfaces is set to zero
	Real set(0);
	if(!fractions) {
		set =               vel(i,j,k).x - vel(i+1,j,k).x + 
				 			vel(i,j,k).y - vel(i,j+1,k).y; 
		if(vel.is3D()) set+=vel(i,j,k).z - vel(i,j,k+1).z;
	}else{
		set =               fractions->get(i,j,k).x*vel(i,j,k).x - fractions->get(i+1,j,k).x*vel(i+1,j,k).x + 
							fractions->get(i,j,k).y*vel(i,j,k).y - fractions->get(i,j+1,k).y*vel(i,j+1,k).y; 
		if(vel.is3D()) set+=fractions->get(i,j,k).z*vel(i,j,k).z - fractions->get(i,j,k+1).z*vel(i,j,k+1).z;
	}
	
	// per cell divergence correction
	if(perCellCorr) 
		set += perCellCorr->get(i,j,k);
	
	// obtain sum, cell count
	sum += set;
	cnt++;
	
	rhs(i,j,k) = set;
}

//! Kernel: Apply velocity update from poisson equation
KERNEL(bnd=1) 
void CorrectVelocity(FlagGrid& flags, MACGrid& vel, Grid<Real>& pressure) 
{
	int idx = flags.index(i,j,k);
	if (flags.isFluid(idx))
	{
		if (flags.isFluid(i-1,j,k)) vel[idx].x -= (pressure[idx] - pressure(i-1,j,k));
		if (flags.isFluid(i,j-1,k)) vel[idx].y -= (pressure[idx] - pressure(i,j-1,k));
		if (flags.is3D() && flags.isFluid(i,j,k-1)) vel[idx].z -= (pressure[idx] - pressure(i,j,k-1));
 
		if (flags.isEmpty(i-1,j,k)) vel[idx].x -= pressure[idx];
		if (flags.isEmpty(i,j-1,k)) vel[idx].y -= pressure[idx];
		if (flags.is3D() && flags.isEmpty(i,j,k-1)) vel[idx].z -= pressure[idx];
	}
	else if (flags.isEmpty(idx))
	{
		if (flags.isFluid(i - 1, j, k)) vel[idx].x += pressure(i - 1, j, k);
		if (flags.isFluid(i, j - 1, k)) vel[idx].y += pressure(i, j - 1, k);
		if (flags.is3D() && flags.isFluid(i,j,k-1)) vel[idx].z += pressure(i,j,k-1);
	}
}

inline void convertDescToVec(const string& desc, Vector3D<bool>& lo, Vector3D<bool>& up) {
	for (size_t i = 0; i<desc.size(); i++) {
		if (desc[i] == 'x') lo.x = true;
		else if (desc[i] == 'y') lo.y = true;
		else if (desc[i] == 'z') lo.z = true;
		else if (desc[i] == 'X') up.x = true;
		else if (desc[i] == 'Y') up.y = true;
		else if (desc[i] == 'Z') up.z = true;
		else errMsg("invalid character in boundary description string. Only [xyzXYZ] allowed.");
	}
}

PYTHON void resetOpenBound(FlagGrid& flags, Grid<Real>* phi = 0, BasicParticleSystem* parts = 0, Grid<Real>* real = 0, Grid<int>* index = 0, ParticleIndexSystem* indexSys = 0){
	// check if phi and parts -> pindex and gpi already created -> access particles from cell index, avoid additional looping over particles
	if (parts && (!index || !indexSys)){
		if (phi) debMsg("resetOpenBound for phi and particles, but missing index and indexSys for enhanced particle access!",1);
		for (int idx = 0; idx < (int)parts->size(); idx++) 
			if (parts->isActive(idx) && flags.isInBounds(parts->getPos(idx)) && flags.isOutflow(parts->getPos(idx))) parts->kill(idx);
	}
	FOR_IJK(flags){
		if (flags.isOutflow(i,j,k)){
			flags(i, j, k) &= ~FlagGrid::TypeFluid; // make sure there is not fluid flag set
			// the particles in a cell i,j,k are particles[index(i,j,k)] to particles[index(i+1,j,k)-1]
			if (parts && index && indexSys){
				int isysIdxS = index->index(i, j, k);
				int pStart = (*index)(isysIdxS), pEnd = 0;
				if (flags.isInBounds(isysIdxS + 1)) pEnd = (*index)(isysIdxS + 1);
				else								pEnd = indexSys->size();
				// now loop over particles in cell
				for (int p = pStart; p<pEnd; ++p) {
					int psrc = (*indexSys)[p].sourceIndex;
					if (parts->isActive(psrc) && flags.isInBounds(parts->getPos(psrc))) parts->kill(psrc);
				}
			}
			if (phi) (*phi)(i, j, k) = 0.5;
			if (real) (*real)(i, j, k) = 0;
		}
	}
	if (parts) parts->doCompress();
}

int checkBWidth(FlagGrid& flags, int bWidth){
	for (int i = 0; i < bWidth; i++){ // check boundary width
		if ((!flags.is3D() && !flags.isObstacle(i, 5, 0)) || (flags.is3D() && !flags.isObstacle(i, 5, 5))) {
			bWidth = i;
			break;
		}
	}
	return bWidth;
}

// set boundary cells of open walls to empty cells 
PYTHON void setOpenBound(FlagGrid& flags, string openBound = "", int type = FlagGrid::TypeOutflow | FlagGrid::TypeEmpty, int bWidth=-1){
	if (openBound == "") return;
	Vector3D<bool> lo, up;
	convertDescToVec(openBound, lo, up);
	if (flags.is2D() && (lo.z || up.z)) errMsg("open boundaries for z specified for 2D grid");

	// look for how many cells form the boundary in order to know which cells must be set to air / outflow
	// assume a maximum boundary width of 3 cells (everything beyond counts as inner obstacle)
	if (bWidth = -1) bWidth = checkBWidth(flags, 3);

	FOR_IJK(flags){
		bool loX = lo.x && i < bWidth; // a cell which belongs to the lower x open bound
		bool loY = lo.y && j < bWidth; // a cell which belongs to the lower y open bound
		bool upX = up.x && i >= flags.getSizeX() - bWidth; // a cell which belongs to the upper x open bound
		bool upY = up.y && j >= flags.getSizeY() - bWidth; // a cell which belongs to the upper y open bound
		bool innerI = i>=bWidth && i<flags.getSizeX() - bWidth; // a cell which does not belong to the lower or upper x bound
		bool innerJ = j>=bWidth && j<flags.getSizeY() - bWidth; // a cell which does not belong to the lower or upper y bound

		// when setting boundaries to open: don't set shared part of wall to empty if neighboring wall is not open
		if (flags.is2D() && (loX||upX||loY||upY)){
			if ((loX || upX || innerI) && (loY || upY || innerJ) && flags.isObstacle(i, j, k)) flags(i, j, k) = type;
		}
		else{
			bool loZ = lo.z && k < bWidth; // a cell which belongs to the lower z open bound
			bool upZ = up.z && k >= flags.getSizeZ() - bWidth; // a cell which belongs to the upper z open bound
			bool innerK = k>=bWidth && k<flags.getSizeZ() - bWidth; // a cell which does not belong to the lower or upper z bound
			if (loX || upX || loY || upY || loZ || upZ){
				if ((loX || upX || innerI) && (loY || upY || innerJ) && (loZ || upZ || innerK) && flags.isObstacle(i, j, k)) flags(i, j, k) = type;
			}
		}
	}
}

// *****************************************************************************
// Ghost fluid helpers

// calculate fraction filled with liquid (note, assumes inside value is < outside!)
inline static Real thetaHelper(Real inside, Real outside)
{
	Real denom = inside-outside;
	if (denom > -1e-04) return 0.5; // should always be neg, and large enough...
	return std::max(Real(0), std::min(Real(1), inside/denom));
}

// calculate ghost fluid factor, cell at idx should be a fluid cell
inline static Real ghostFluidHelper(int idx, int offset, const Grid<Real> &phi, Real gfClamp)
{
	Real alpha = thetaHelper(phi[idx], phi[idx+offset]);
	if (alpha < gfClamp) return alpha = gfClamp;
	return (1-(1/alpha)); 
}

//! Kernel: Adapt A0 for ghost fluid
KERNEL(bnd=1) 
void ApplyGhostFluidDiagonal(Grid<Real> &A0, const FlagGrid &flags, const Grid<Real> &phi, Real gfClamp)
{
	const int X = flags.getStrideX(), Y = flags.getStrideY(), Z = flags.getStrideZ();
	int idx = flags.index(i,j,k);
	if (!flags.isFluid(idx)) return;

	if (flags.isEmpty(i-1,j,k)) A0[idx] -= ghostFluidHelper(idx, -X, phi, gfClamp);
	if (flags.isEmpty(i+1,j,k)) A0[idx] -= ghostFluidHelper(idx, +X, phi, gfClamp);
	if (flags.isEmpty(i,j-1,k)) A0[idx] -= ghostFluidHelper(idx, -Y, phi, gfClamp);
	if (flags.isEmpty(i,j+1,k)) A0[idx] -= ghostFluidHelper(idx, +Y, phi, gfClamp);
	if (flags.is3D()) {
		if (flags.isEmpty(i,j,k-1)) A0[idx] -= ghostFluidHelper(idx, -Z, phi, gfClamp);
		if (flags.isEmpty(i,j,k+1)) A0[idx] -= ghostFluidHelper(idx, +Z, phi, gfClamp);
	}
}

//! Kernel: Apply velocity update: ghost fluid contribution
KERNEL(bnd=1)
void CorrectVelocityGhostFluid(MACGrid &vel, const FlagGrid &flags, const Grid<Real> &pressure, const Grid<Real> &phi, Real gfClamp)
{
	const int X = flags.getStrideX(), Y = flags.getStrideY(), Z = flags.getStrideZ();
	const int idx = flags.index(i,j,k);
	if (flags.isFluid(idx))
	{
		if (flags.isEmpty(i-1,j,k)) vel[idx][0] += pressure[idx] * ghostFluidHelper(idx, -X, phi, gfClamp);
		if (flags.isEmpty(i,j-1,k)) vel[idx][1] += pressure[idx] * ghostFluidHelper(idx, -Y, phi, gfClamp);
		if (flags.is3D() && flags.isEmpty(i,j,k-1)) vel[idx][2] += pressure[idx] * ghostFluidHelper(idx, -Z, phi, gfClamp);
	}
	else if (flags.isEmpty(idx))
	{
		if (flags.isFluid(i-1,j,k)) vel[idx][0] -= pressure(i-1,j,k) * ghostFluidHelper(idx-X, +X, phi, gfClamp);
		if (flags.isFluid(i,j-1,k)) vel[idx][1] -= pressure(i,j-1,k) * ghostFluidHelper(idx-Y, +Y, phi, gfClamp);
		if (flags.is3D() && flags.isFluid(i,j,k-1)) vel[idx][2] -= pressure(i,j,k-1) * ghostFluidHelper(idx-Z, +Z, phi, gfClamp);
	}
}


// improve behavior of clamping for large time steps:

inline static Real ghostFluidWasClamped(int idx, int offset, const Grid<Real> &phi, Real gfClamp)
{
	Real alpha = thetaHelper(phi[idx], phi[idx+offset]);
	if (alpha < gfClamp) return true;
	return false;
}

KERNEL(bnd=1)
void ReplaceClampedGhostFluidVels(MACGrid &vel, FlagGrid &flags, 
		const Grid<Real> &pressure, const Grid<Real> &phi, Real gfClamp )
{
	const int X = flags.getStrideX(), Y = flags.getStrideY(), Z = flags.getStrideZ();
	const int idx = flags.index(i,j,k);
	if (flags.isFluid(idx))
	{
		if( (flags.isEmpty(i-1,j,k)) && (ghostFluidWasClamped(idx, -X, phi, gfClamp)) )
			vel[idx-X][0] = vel[idx][0];
		if( (flags.isEmpty(i,j-1,k)) && (ghostFluidWasClamped(idx, -Y, phi, gfClamp)) )
			vel[idx-Y][1] = vel[idx][1];
		if( flags.is3D() && 
		   (flags.isEmpty(i,j,k-1)) && (ghostFluidWasClamped(idx, -Z, phi, gfClamp)) )
			vel[idx-Z][2] = vel[idx][2];
	}
	else if (flags.isEmpty(idx))
	{
		if( (i>-1) && (flags.isFluid(i-1,j,k)) && ( ghostFluidWasClamped(idx-X, +X, phi, gfClamp) ) )
			vel[idx][0] = vel[idx-X][0];
		if( (j>-1) && (flags.isFluid(i,j-1,k)) && ( ghostFluidWasClamped(idx-Y, +Y, phi, gfClamp) ) )
			vel[idx][1] = vel[idx-Y][1];
		if( flags.is3D() &&
		  ( (k>-1) && (flags.isFluid(i,j,k-1)) && ( ghostFluidWasClamped(idx-Z, +Z, phi, gfClamp) ) ))
			vel[idx][2] = vel[idx-Z][2];
	}
}

//! Kernel: Compute min value of Real grid
KERNEL(idx, reduce=+) returns(int numEmpty=0)
int CountEmptyCells(FlagGrid& flags) {
	if (flags.isEmpty(idx) ) numEmpty++;
}

// *****************************************************************************
// Main pressure solve

KERNEL (bnd=1) void KnupdateFractions(FlagGrid& flags, Grid<Real>& phi, MACGrid& fractions) {

	fractions(i,j,k).x = fractions(i,j,k).y = fractions(i,j,k).z = static_cast<float>(flags(i,j,k) & 1);

	Real tmp = 0.;
	tmp = fabs((phi(i,j,k) + phi(i-1,j,k))/2.);
    if(tmp < 1.0 && !((phi(i,j,k)<0) == (phi(i-1,j,k)<0)) ) {
    	fractions(i,j,k).x = tmp;
    }else if( (flags(i-1,j,k) == 2 && flags(i,j,k) == 1) || (flags(i,j,k) == 2 && flags(i-1,j,k) == 1)) {
		fractions(i,j,k).x = 0.;
    }
    tmp = fabs((phi(i,j,k) + phi(i,j-1,k))/2.);
    if(tmp < 1.0 && !((phi(i,j,k)<0) == (phi(i,j-1,k)<0)) ) {
    	fractions(i,j,k).y = tmp;
    }else if( (flags(i,j-1,k) == 2 && flags(i,j,k) == 1) || (flags(i,j,k) == 2 && flags(i,j-1,k) == 1)) {
		fractions(i,j,k).y = 0.;
    }
    if(flags.is3D()) {
	    tmp = fabs((phi(i,j,k) + phi(i,j,k-1))/2.);
	    if(tmp < 1.0 && !((phi(i,j,k)<0) == (phi(i,j,k-1)<0)) ) {
	    	fractions(i,j,k).z = tmp;
	    }else if( (flags(i,j,k-1) == 2 && flags(i,j,k) == 1) || (flags(i,j,k) == 2 && flags(i,j,k-1) == 1)) {
			fractions(i,j,k).z = 0.;
	    }
	}

}

PYTHON void updateFractions(FlagGrid& flags, Grid<Real>& phi, MACGrid& fractions) {
	KnupdateFractions(flags, phi, fractions);
}

//! Perform pressure projection of the velocity grid
PYTHON void solvePressure(MACGrid& vel, Grid<Real>& pressure, FlagGrid& flags,
                     Grid<Real>* phi = 0, 
                     Grid<Real>* perCellCorr = 0, 
                     MACGrid* fractions = 0,
                     Real gfClamp = 1e-04,
                     Real cgMaxIterFac = 1.5,
                     Real cgAccuracy = 1e-3,
                     bool precondition = true,
                     bool enforceCompatibility = false,
                     bool useL2Norm = false, 
					 Grid<Real>* retRhs = NULL )
{
	// reserve temp grids
	FluidSolver* parent = flags.getParent();
	Grid<Real> rhs(parent);
	Grid<Real> residual(parent);
	Grid<Real> search(parent);
	Grid<Real> A0(parent);
	Grid<Real> Ai(parent);
	Grid<Real> Aj(parent);
	Grid<Real> Ak(parent);
	Grid<Real> tmp(parent);
	Grid<Real> pca0(parent);
	Grid<Real> pca1(parent);
	Grid<Real> pca2(parent);
	Grid<Real> pca3(parent);
		
	// setup matrix and boundaries 
	MakeLaplaceMatrix (flags, A0, Ai, Aj, Ak, fractions);
	
	if (phi) {
		ApplyGhostFluidDiagonal(A0, flags, *phi, gfClamp);
	}
	
	// compute divergence and init right hand side
	MakeRhs kernMakeRhs (flags, rhs, vel, perCellCorr, fractions);
	
	if (enforceCompatibility)
		rhs += (Real)(-kernMakeRhs.sum / (Real)kernMakeRhs.cnt);
	
	// check whether we need to fix some pressure value...
	int fixPidx = -1;
	int numEmpty = CountEmptyCells(flags);
	if(numEmpty==0) {
		FOR_IJK_BND(flags,1) {
			if(flags.isFluid(i,j,k)) {
				fixPidx = flags.index(i,j,k);
				break;
			}
		}
		//debMsg("No empty cells! Fixing pressure of cell "<<fixPidx<<" to zero",1);
	}
	if(fixPidx>=0) {
		flags[fixPidx] |= FlagGrid::TypeZeroPressure;
		rhs[fixPidx] = 0.; 
	}

	// CG setup
	// note: the last factor increases the max iterations for 2d, which right now can't use a preconditioner 
	const int maxIter = (int)(cgMaxIterFac * flags.getSize().max()) * (flags.is3D() ? 1 : 4);
	GridCgInterface *gcg;
	if (vel.is3D())
		gcg = new GridCg<ApplyMatrix>  (pressure, rhs, residual, search, flags, tmp, &A0, &Ai, &Aj, &Ak );
	else
		gcg = new GridCg<ApplyMatrix2D>(pressure, rhs, residual, search, flags, tmp, &A0, &Ai, &Aj, &Ak );
	
	gcg->setAccuracy( cgAccuracy ); 
	gcg->setUseL2Norm( useL2Norm );

	// optional preconditioning
	gcg->setPreconditioner( precondition ? GridCgInterface::PC_mICP : GridCgInterface::PC_None, &pca0, &pca1, &pca2, &pca3);

	for (int iter=0; iter<maxIter; iter++) {
		if (!gcg->iterate()) iter=maxIter;
	} 
	debMsg("FluidSolver::solvePressure iterations:"<<gcg->getIterations()<<", res:"<<gcg->getSigma(), 1);
	delete gcg;
	
	CorrectVelocity(flags, vel, pressure ); 
	if (phi) {
		CorrectVelocityGhostFluid (vel, flags, pressure, *phi, gfClamp);
		// improve behavior of clamping for large time steps:
		ReplaceClampedGhostFluidVels (vel, flags, pressure, *phi, gfClamp);
	}

	if(fixPidx>=0)
		flags[fixPidx] &= ~FlagGrid::TypeZeroPressure;

	// optionally , return RHS
	if(retRhs) {
		retRhs->copyFrom( rhs );
	}
}

} // end namespace

