//////////////////////////////////////////////////////////////////////////////////////
// This file is distributed under the University of Illinois/NCSA Open Source License.
// See LICENSE file in top directory for details.
//
// Copyright (c) 2016 Jeongnim Kim and QMCPACK developers.
//
// File developed by:
//
// File created by: Jeongnim Kim, jeongnim.kim@intel.com, Intel Corp.
//////////////////////////////////////////////////////////////////////////////////////


/** @file SplineC2CSoA.h
 *
 * Adoptor classes to handle complex-to-(real,complex) with arbitrary precision
 */
#ifndef QMCPLUSPLUS_EINSPLINE_C2C_SOA_ADOPTOR_H
#define QMCPLUSPLUS_EINSPLINE_C2C_SOA_ADOPTOR_H

#include <OhmmsSoA/Container.h>
#include <spline2/MultiBspline.hpp>
#include <spline2/MultiBsplineEval.hpp>
#include "QMCWaveFunctions/BsplineFactory/SplineAdoptorBase.h"
#include <Utilities/FairDivide.h>

namespace qmcplusplus
{

/** adoptor class to match std::complex<ST> spline with std::complex<TT> SPOs
 * @tparam ST precision of spline
 * @tparam TT precision of SPOs
 * @tparam D dimension
 *
 * Requires temporage storage and multiplication of phase vectors
 * Internal storage use double sized arrays of ST type, aligned and padded.
 */
template<typename ST, typename TT>
struct SplineC2CSoA: public SplineAdoptorBase<ST,3>
{
  static const int D=3;
  using BaseType=SplineAdoptorBase<ST,3>;
  using SplineType=typename bspline_traits<ST,3>::SplineType;
  using BCType=typename bspline_traits<ST,3>::BCType;
  using DataType=ST;
  using PointType=typename BaseType::PointType;
  using SingleSplineType=typename BaseType::SingleSplineType;
  using ComplexT=typename std::complex<TT>;

  using vContainer_type=Vector<ST,aligned_allocator<ST> >;
  using gContainer_type=VectorSoaContainer<ST,3>;
  using hContainer_type=VectorSoaContainer<ST,6>;

  using BaseType::first_spo;
  using BaseType::last_spo;
  using BaseType::GGt;
  using BaseType::PrimLattice;
  using BaseType::kPoints;
  using BaseType::MakeTwoCopies;
  using BaseType::offset;

  ///number of points of the original grid
  int BaseN[3];
  ///offset of the original grid, always 0
  int BaseOffset[3];
  ///multi bspline set
  MultiBspline<ST>* SplineInst;
  ///expose the pointer to reuse the reader and only assigned with create_spline
  ///also used as identifier of shallow copy
  SplineType* MultiSpline;

  vContainer_type  mKK;
  VectorSoaContainer<ST,3>  myKcart;

  vContainer_type myV;
  vContainer_type myL;
  gContainer_type myG;
  hContainer_type myH;

  SplineC2CSoA(): BaseType(), SplineInst(nullptr), MultiSpline(nullptr)
  {
    this->is_complex=true;
    this->is_soa_ready=true;
    this->AdoptorName="SplineC2CSoAAdoptor";
    this->KeyWord="SplineC2CSoA";
  }

  SplineC2CSoA(const SplineC2CSoA& a):
    SplineAdoptorBase<ST,3>(a),SplineInst(a.SplineInst),MultiSpline(nullptr),
    mKK(a.mKK), myKcart(a.myKcart)
  {
    const size_t n=a.myL.size();
    myV.resize(n); myG.resize(n); myL.resize(n); myH.resize(n);
  }

  ~SplineC2CSoA()
  {
    if(MultiSpline != nullptr) delete SplineInst;
  }

  inline void resizeStorage(size_t n, size_t nvals)
  {
    BaseType::init_base(n);
    size_t npad=getAlignedSize<ST>(2*n);
    myV.resize(npad);
    myG.resize(npad);
    myL.resize(npad);
    myH.resize(npad);
  }

  void bcast_tables(Communicate* comm)
  {
    chunked_bcast(comm, MultiSpline);
  }

  void gather_tables(Communicate* comm)
  {
    if(comm->size()==1) return;
    const int Nbands = kPoints.size();
    const int Nbandgroups = comm->size();
    offset.resize(Nbandgroups+1,0);
    FairDivideLow(Nbands,Nbandgroups,offset);
    for(size_t ib=0; ib<offset.size(); ib++)
      offset[ib]*=2;
    gatherv(comm, MultiSpline, MultiSpline->z_stride, offset);
  }

  template<typename GT, typename BCT>
  void create_spline(GT& xyz_g, BCT& xyz_bc)
  {
    resize_kpoints();
    SplineInst=new MultiBspline<ST>();
    SplineInst->create(xyz_g,xyz_bc,myV.size());
    MultiSpline=SplineInst->spline_m;
    for(size_t i=0; i<D; ++i)
    {
      BaseOffset[i]=0;
      BaseN[i]=xyz_g[i].num+3;
    }
    qmc_common.memory_allocated += SplineInst->sizeInByte();
  }

  inline void flush_zero()
  {
    SplineInst->flush_zero();
  }

  /** remap kPoints to pack the double copy */
  inline void resize_kpoints()
  {
    const size_t nk=kPoints.size();
    mKK.resize(nk);
    myKcart.resize(nk);
    for(size_t i=0; i<nk; ++i)
    {
      mKK[i]=-dot(kPoints[i],kPoints[i]);
      myKcart(i)=kPoints[i];
    }
  }

  inline void set_spline(SingleSplineType* spline_r, SingleSplineType* spline_i, int twist, int ispline, int level)
  {
    SplineInst->copy_spline(spline_r,2*ispline  ,BaseOffset, BaseN);
    SplineInst->copy_spline(spline_i,2*ispline+1,BaseOffset, BaseN);
  }

  void set_spline(ST* restrict psi_r, ST* restrict psi_i, int twist, int ispline, int level)
  {
    Vector<ST> v_r(psi_r,0), v_i(psi_i,0);
    SplineInst->set(2*ispline  ,v_r);
    SplineInst->set(2*ispline+1,v_i);
  }


  inline void set_spline_domain(SingleSplineType* spline_r, SingleSplineType* spline_i,
      int twist, int ispline, const int* offset_l, const int* mesh_l)
  {
  }

  bool read_splines(hdf_archive& h5f)
  {
    std::ostringstream o;
    o<<"spline_" << SplineAdoptorBase<ST,D>::MyIndex;
    einspline_engine<SplineType> bigtable(SplineInst->spline_m);
    return h5f.read(bigtable,o.str().c_str());//"spline_0");
  }

  bool write_splines(hdf_archive& h5f)
  {
    std::ostringstream o;
    o<<"spline_" << SplineAdoptorBase<ST,D>::MyIndex;
    einspline_engine<SplineType> bigtable(SplineInst->spline_m);
    return h5f.write(bigtable,o.str().c_str());//"spline_0");
  }

  template<typename VV>
  inline void assign_v(const PointType& r, const vContainer_type& myV, VV& psi, int first = 0, int last = -1) const
  {
    // protect last
    last = last<0 ? kPoints.size() : (last>kPoints.size() ? kPoints.size() : last);

    const ST x=r[0], y=r[1], z=r[2];
    const ST* restrict kx=myKcart.data(0);
    const ST* restrict ky=myKcart.data(1);
    const ST* restrict kz=myKcart.data(2);
    #pragma omp simd
    for (size_t j=first; j<last; ++j)
    {
      ST s, c;
      const ST val_r=myV[2*j  ];
      const ST val_i=myV[2*j+1];
      sincos(-(x*kx[j]+y*ky[j]+z*kz[j]),&s,&c);
      psi[j+first_spo] = ComplexT(val_r*c-val_i*s,val_i*c+val_r*s);
    }
  }

  template<typename VV>
  inline void evaluate_v(const ParticleSet& P, const int iat, VV& psi)
  {
    const PointType& r=P.activeR(iat);
    PointType ru(PrimLattice.toUnit_floor(r));

    #pragma omp parallel
    {
      int first, last;
      FairDivideAligned(myV.size(), getAlignment<ST>(),
                        omp_get_num_threads(),
                        omp_get_thread_num(),
                        first, last);

      spline2::evaluate3d(SplineInst->spline_m,ru,myV,first,last);
      assign_v(r,myV,psi,first/2,last/2);
    }
  }

  template<typename VM, typename VAV>
  inline void evaluateValues(const VirtualParticleSet& VP, VM& psiM, VAV& SPOMem)
  {
    #pragma omp parallel
    {
      int first, last;
      FairDivideAligned(myV.size(), getAlignment<ST>(),
                        omp_get_num_threads(),
                        omp_get_thread_num(),
                        first, last);

      const size_t m=psiM.cols();
      for(int iat=0; iat<VP.getTotalNum(); ++iat)
      {
        const PointType& r=VP.activeR(iat);
        PointType ru(PrimLattice.toUnit_floor(r));
        Vector<ComplexT> psi(psiM[iat],m);

        spline2::evaluate3d(SplineInst->spline_m,ru,myV,first,last);
        assign_v(r,myV,psi,first/2,last/2);
      }
    }
  }

  inline size_t estimateMemory(const int nP) { return 0; }

  /** assign_vgl
   */
  template<typename VV, typename GV>
  inline void assign_vgl(const PointType& r, VV& psi, GV& dpsi, VV& d2psi, int first = 0, int last = -1) const
  {
    // protect last
    last = last<0 ? kPoints.size() : (last>kPoints.size() ? kPoints.size() : last);

    constexpr ST zero(0);
    constexpr ST two(2);
    const ST g00=PrimLattice.G(0), g01=PrimLattice.G(1), g02=PrimLattice.G(2),
             g10=PrimLattice.G(3), g11=PrimLattice.G(4), g12=PrimLattice.G(5),
             g20=PrimLattice.G(6), g21=PrimLattice.G(7), g22=PrimLattice.G(8);
    const ST x=r[0], y=r[1], z=r[2];
    const ST symGG[6]={GGt[0],GGt[1]+GGt[3],GGt[2]+GGt[6],GGt[4],GGt[5]+GGt[7],GGt[8]};

    const ST* restrict k0=myKcart.data(0);
    const ST* restrict k1=myKcart.data(1);
    const ST* restrict k2=myKcart.data(2);

    const ST* restrict g0=myG.data(0);
    const ST* restrict g1=myG.data(1);
    const ST* restrict g2=myG.data(2);
    const ST* restrict h00=myH.data(0);
    const ST* restrict h01=myH.data(1);
    const ST* restrict h02=myH.data(2);
    const ST* restrict h11=myH.data(3);
    const ST* restrict h12=myH.data(4);
    const ST* restrict h22=myH.data(5);

    #pragma omp simd
    for (size_t j=first; j<last; ++j)
    {
      const size_t jr=j<<1;
      const size_t ji=jr+1;

      const ST kX=k0[j];
      const ST kY=k1[j];
      const ST kZ=k2[j];
      const ST val_r=myV[jr];
      const ST val_i=myV[ji];

      //phase
      ST s, c;
      sincos(-(x*kX+y*kY+z*kZ),&s,&c);

      //dot(PrimLattice.G,myG[j])
      const ST dX_r = g00*g0[jr]+g01*g1[jr]+g02*g2[jr];
      const ST dY_r = g10*g0[jr]+g11*g1[jr]+g12*g2[jr];
      const ST dZ_r = g20*g0[jr]+g21*g1[jr]+g22*g2[jr];

      const ST dX_i = g00*g0[ji]+g01*g1[ji]+g02*g2[ji];
      const ST dY_i = g10*g0[ji]+g11*g1[ji]+g12*g2[ji];
      const ST dZ_i = g20*g0[ji]+g21*g1[ji]+g22*g2[ji];

      // \f$\nabla \psi_r + {\bf k}\psi_i\f$
      const ST gX_r=dX_r+val_i*kX;
      const ST gY_r=dY_r+val_i*kY;
      const ST gZ_r=dZ_r+val_i*kZ;
      const ST gX_i=dX_i-val_r*kX;
      const ST gY_i=dY_i-val_r*kY;
      const ST gZ_i=dZ_i-val_r*kZ;

      const ST lcart_r=SymTrace(h00[jr],h01[jr],h02[jr],h11[jr],h12[jr],h22[jr],symGG);
      const ST lcart_i=SymTrace(h00[ji],h01[ji],h02[ji],h11[ji],h12[ji],h22[ji],symGG);
      const ST lap_r=lcart_r+mKK[j]*val_r+two*(kX*dX_i+kY*dY_i+kZ*dZ_i);
      const ST lap_i=lcart_i+mKK[j]*val_i-two*(kX*dX_r+kY*dY_r+kZ*dZ_r);
      const size_t psiIndex=j+first_spo;
      psi[psiIndex ]   = ComplexT(c*val_r-s*val_i,c*val_i+s*val_r);
      dpsi[psiIndex][0]= ComplexT(c*gX_r -s*gX_i, c*gX_i +s*gX_r);
      dpsi[psiIndex][1]= ComplexT(c*gY_r -s*gY_i, c*gY_i +s*gY_r);
      dpsi[psiIndex][2]= ComplexT(c*gZ_r -s*gZ_i, c*gZ_i +s*gZ_r);
      d2psi[psiIndex]  = ComplexT(c*lap_r-s*lap_i,c*lap_i+s*lap_r);
    }
  }

  /** assign_vgl_from_l can be used when myL is precomputed and myV,myG,myL in cartesian
   */
  template<typename VV, typename GV>
  inline void assign_vgl_from_l(const PointType& r, VV& psi, GV& dpsi, VV& d2psi)
  {
    constexpr ST two(2);
    const ST x=r[0], y=r[1], z=r[2];

    const ST* restrict k0=myKcart.data(0);
    const ST* restrict k1=myKcart.data(1);
    const ST* restrict k2=myKcart.data(2);

    const ST* restrict g0=myG.data(0);
    const ST* restrict g1=myG.data(1);
    const ST* restrict g2=myG.data(2);

    const size_t N=last_spo-first_spo;
    #pragma omp simd
    for (size_t j=0; j<N; ++j)
    {
      const size_t jr=j<<1;
      const size_t ji=jr+1;

      const ST kX=k0[j];
      const ST kY=k1[j];
      const ST kZ=k2[j];
      const ST val_r=myV[jr];
      const ST val_i=myV[ji];

      //phase
      ST s, c;
      sincos(-(x*kX+y*kY+z*kZ),&s,&c);

      //dot(PrimLattice.G,myG[j])
      const ST dX_r = g0[jr];
      const ST dY_r = g1[jr];
      const ST dZ_r = g2[jr];

      const ST dX_i = g0[ji];
      const ST dY_i = g1[ji];
      const ST dZ_i = g2[ji];

      // \f$\nabla \psi_r + {\bf k}\psi_i\f$
      const ST gX_r=dX_r+val_i*kX;
      const ST gY_r=dY_r+val_i*kY;
      const ST gZ_r=dZ_r+val_i*kZ;
      const ST gX_i=dX_i-val_r*kX;
      const ST gY_i=dY_i-val_r*kY;
      const ST gZ_i=dZ_i-val_r*kZ;

      const ST lap_r=myL[jr]+mKK[j]*val_r+two*(kX*dX_i+kY*dY_i+kZ*dZ_i);
      const ST lap_i=myL[ji]+mKK[j]*val_i-two*(kX*dX_r+kY*dY_r+kZ*dZ_r);

      const size_t psiIndex=j+first_spo;
      psi[psiIndex ]   = ComplexT(c*val_r-s*val_i,c*val_i+s*val_r);
      dpsi[psiIndex][0]= ComplexT(c*gX_r -s*gX_i, c*gX_i +s*gX_r);
      dpsi[psiIndex][1]= ComplexT(c*gY_r -s*gY_i, c*gY_i +s*gY_r);
      dpsi[psiIndex][2]= ComplexT(c*gZ_r -s*gZ_i, c*gZ_i +s*gZ_r);
      d2psi[psiIndex]  = ComplexT(c*lap_r-s*lap_i,c*lap_i+s*lap_r);
    }
  }

  template<typename VV, typename GV>
  inline void evaluate_vgl(const ParticleSet& P, const int iat, VV& psi, GV& dpsi, VV& d2psi)
  {
    const PointType& r=P.activeR(iat);
    PointType ru(PrimLattice.toUnit_floor(r));

    #pragma omp parallel
    {
      int first, last;
      FairDivideAligned(myV.size(), getAlignment<ST>(),
                        omp_get_num_threads(),
                        omp_get_thread_num(),
                        first, last);

      spline2::evaluate3d_vgh(SplineInst->spline_m,ru,myV,myG,myH,first,last);
      assign_vgl(r,psi,dpsi,d2psi,first/2,last/2);
    }
  }

  template<typename VV, typename GV, typename GGV>
  void assign_vgh(const PointType& r, VV& psi, GV& dpsi, GGV& grad_grad_psi, int first = 0, int last = -1) const
  {
    // protect last
    last = last<0 ? kPoints.size() : (last>kPoints.size() ? kPoints.size() : last);

    const ST g00=PrimLattice.G(0), g01=PrimLattice.G(1), g02=PrimLattice.G(2),
             g10=PrimLattice.G(3), g11=PrimLattice.G(4), g12=PrimLattice.G(5),
             g20=PrimLattice.G(6), g21=PrimLattice.G(7), g22=PrimLattice.G(8);
    const ST x=r[0], y=r[1], z=r[2];

    const ST* restrict k0=myKcart.data(0);
    const ST* restrict k1=myKcart.data(1);
    const ST* restrict k2=myKcart.data(2);

    const ST* restrict g0=myG.data(0);
    const ST* restrict g1=myG.data(1);
    const ST* restrict g2=myG.data(2);
    const ST* restrict h00=myH.data(0);
    const ST* restrict h01=myH.data(1);
    const ST* restrict h02=myH.data(2);
    const ST* restrict h11=myH.data(3);
    const ST* restrict h12=myH.data(4);
    const ST* restrict h22=myH.data(5);

    #pragma omp simd
    for (size_t j=first; j<last; ++j)
    {
      int jr=j<<1;
      int ji=jr+1;

      const ST kX=k0[j];
      const ST kY=k1[j];
      const ST kZ=k2[j];
      const ST val_r=myV[jr];
      const ST val_i=myV[ji];

      //phase
      ST s, c;
      sincos(-(x*kX+y*kY+z*kZ),&s,&c);

      //dot(PrimLattice.G,myG[j])
      const ST dX_r = g00*g0[jr]+g01*g1[jr]+g02*g2[jr];
      const ST dY_r = g10*g0[jr]+g11*g1[jr]+g12*g2[jr];
      const ST dZ_r = g20*g0[jr]+g21*g1[jr]+g22*g2[jr];

      const ST dX_i = g00*g0[ji]+g01*g1[ji]+g02*g2[ji];
      const ST dY_i = g10*g0[ji]+g11*g1[ji]+g12*g2[ji];
      const ST dZ_i = g20*g0[ji]+g21*g1[ji]+g22*g2[ji];

      // \f$\nabla \psi_r + {\bf k}\psi_i\f$
      const ST gX_r=dX_r+val_i*kX;
      const ST gY_r=dY_r+val_i*kY;
      const ST gZ_r=dZ_r+val_i*kZ;
      const ST gX_i=dX_i-val_r*kX;
      const ST gY_i=dY_i-val_r*kY;
      const ST gZ_i=dZ_i-val_r*kZ;

      const size_t psiIndex=j+first_spo;
      psi[psiIndex] =ComplexT(c*val_r-s*val_i,c*val_i+s*val_r);
      dpsi[psiIndex][0]=ComplexT(c*gX_r -s*gX_i, c*gX_i +s*gX_r);
      dpsi[psiIndex][1]=ComplexT(c*gY_r -s*gY_i, c*gY_i +s*gY_r);
      dpsi[psiIndex][2]=ComplexT(c*gZ_r -s*gZ_i, c*gZ_i +s*gZ_r);

      const ST h_xx_r=v_m_v(h00[jr],h01[jr],h02[jr],h11[jr],h12[jr],h22[jr],g00,g01,g02,g00,g01,g02)+kX*(gX_i+dX_i);
      const ST h_xy_r=v_m_v(h00[jr],h01[jr],h02[jr],h11[jr],h12[jr],h22[jr],g00,g01,g02,g10,g11,g12)+kX*(gY_i+dY_i);
      const ST h_xz_r=v_m_v(h00[jr],h01[jr],h02[jr],h11[jr],h12[jr],h22[jr],g00,g01,g02,g20,g21,g22)+kX*(gZ_i+dZ_i);
      const ST h_yx_r=v_m_v(h00[jr],h01[jr],h02[jr],h11[jr],h12[jr],h22[jr],g10,g11,g12,g00,g01,g02)+kY*(gX_i+dX_i);
      const ST h_yy_r=v_m_v(h00[jr],h01[jr],h02[jr],h11[jr],h12[jr],h22[jr],g10,g11,g12,g10,g11,g12)+kY*(gY_i+dY_i);
      const ST h_yz_r=v_m_v(h00[jr],h01[jr],h02[jr],h11[jr],h12[jr],h22[jr],g10,g11,g12,g20,g21,g22)+kY*(gZ_i+dZ_i);
      const ST h_zx_r=v_m_v(h00[jr],h01[jr],h02[jr],h11[jr],h12[jr],h22[jr],g20,g21,g22,g00,g01,g02)+kZ*(gX_i+dX_i);
      const ST h_zy_r=v_m_v(h00[jr],h01[jr],h02[jr],h11[jr],h12[jr],h22[jr],g20,g21,g22,g10,g11,g12)+kZ*(gY_i+dY_i);
      const ST h_zz_r=v_m_v(h00[jr],h01[jr],h02[jr],h11[jr],h12[jr],h22[jr],g20,g21,g22,g20,g21,g22)+kZ*(gZ_i+dZ_i);

      const ST h_xx_i=v_m_v(h00[ji],h01[ji],h02[ji],h11[ji],h12[ji],h22[ji],g00,g01,g02,g00,g01,g02)-kX*(gX_r+dX_r);
      const ST h_xy_i=v_m_v(h00[ji],h01[ji],h02[ji],h11[ji],h12[ji],h22[ji],g00,g01,g02,g10,g11,g12)-kX*(gY_r+dY_r);
      const ST h_xz_i=v_m_v(h00[ji],h01[ji],h02[ji],h11[ji],h12[ji],h22[ji],g00,g01,g02,g20,g21,g22)-kX*(gZ_r+dZ_r);
      const ST h_yx_i=v_m_v(h00[ji],h01[ji],h02[ji],h11[ji],h12[ji],h22[ji],g10,g11,g12,g00,g01,g02)-kY*(gX_r+dX_r);
      const ST h_yy_i=v_m_v(h00[ji],h01[ji],h02[ji],h11[ji],h12[ji],h22[ji],g10,g11,g12,g10,g11,g12)-kY*(gY_r+dY_r);
      const ST h_yz_i=v_m_v(h00[ji],h01[ji],h02[ji],h11[ji],h12[ji],h22[ji],g10,g11,g12,g20,g21,g22)-kY*(gZ_r+dZ_r);
      const ST h_zx_i=v_m_v(h00[ji],h01[ji],h02[ji],h11[ji],h12[ji],h22[ji],g20,g21,g22,g00,g01,g02)-kZ*(gX_r+dX_r);
      const ST h_zy_i=v_m_v(h00[ji],h01[ji],h02[ji],h11[ji],h12[ji],h22[ji],g20,g21,g22,g10,g11,g12)-kZ*(gY_r+dY_r);
      const ST h_zz_i=v_m_v(h00[ji],h01[ji],h02[ji],h11[ji],h12[ji],h22[ji],g20,g21,g22,g20,g21,g22)-kZ*(gZ_r+dZ_r);

      grad_grad_psi[psiIndex][0]=ComplexT(c*h_xx_r-s*h_xx_i, c*h_xx_i+s*h_xx_r);
      grad_grad_psi[psiIndex][1]=ComplexT(c*h_xy_r-s*h_xy_i, c*h_xy_i+s*h_xy_r);
      grad_grad_psi[psiIndex][2]=ComplexT(c*h_xz_r-s*h_xz_i, c*h_xz_i+s*h_xz_r);
      grad_grad_psi[psiIndex][3]=ComplexT(c*h_yx_r-s*h_yx_i, c*h_yx_i+s*h_yx_r);
      grad_grad_psi[psiIndex][4]=ComplexT(c*h_yy_r-s*h_yy_i, c*h_yy_i+s*h_yy_r);
      grad_grad_psi[psiIndex][5]=ComplexT(c*h_yz_r-s*h_yz_i, c*h_yz_i+s*h_yz_r);
      grad_grad_psi[psiIndex][6]=ComplexT(c*h_zx_r-s*h_zx_i, c*h_zx_i+s*h_zx_r);
      grad_grad_psi[psiIndex][7]=ComplexT(c*h_zy_r-s*h_zy_i, c*h_zy_i+s*h_zy_r);
      grad_grad_psi[psiIndex][8]=ComplexT(c*h_zz_r-s*h_zz_i, c*h_zz_i+s*h_zz_r);
    }
  }

  template<typename VV, typename GV, typename GGV>
  void evaluate_vgh(const ParticleSet& P, const int iat, VV& psi, GV& dpsi, GGV& grad_grad_psi)
  {
    const PointType& r=P.activeR(iat);
    PointType ru(PrimLattice.toUnit_floor(r));

    #pragma omp parallel
    {
      int first, last;
      FairDivideAligned(myV.size(), getAlignment<ST>(),
                        omp_get_num_threads(),
                        omp_get_thread_num(),
                        first, last);

      spline2::evaluate3d_vgh(SplineInst->spline_m,ru,myV,myG,myH,first,last);
      assign_vgh(r,psi,dpsi,grad_grad_psi,first/2,last/2);
    }
  }
};

}
#endif
