#include "command.h"
#include "ptr.h"
#include "progressbar.h"
#include "thread/exec.h"
#include "thread/queue.h"
#include "image/loop.h"
#include "image/buffer.h"
#include "image/buffer_preload.h"
#include "image/voxel.h"
#include "dwi/gradient.h"
#include "math/SH.h"
#include "dwi/directions/predefined.h"
#include "math/constrained_least_squares.h"


using namespace MR;
using namespace App;

void usage () {
  AUTHOR = "Ben Jeurissen (ben.jeurissen@gmail.com)";
  
  DESCRIPTION
    + "Multi-shell, multi-tissue CSD";

  REFERENCES = "Jeurissen, B.; Tournier, J.-D.; Dhollander, T.; Connelly, A.; Sijbers, J.
             "Multi-tissue constrained spherical deconvolution for improved analysis of multi-shell diffusion MRI data"
             "NeuroImage, in press, DOI: 10.1016/j.neuroimage.2014.07.061";

  ARGUMENTS
    + Argument ("dwi",
        "the input diffusion-weighted image.").type_image_in()
	+ Argument ("fodf",
        "the output fodf image.").type_image_out();
		
  OPTIONS
    + Option ("mask",
      "only perform computation within the specified binary brain mask image.")
    + Argument ("image").type_image_in ();
}

typedef double value_type;
typedef Image::BufferPreload<value_type> InputBufferType;
typedef Image::Buffer<bool> MaskBufferType;
typedef Image::Buffer<value_type> OutputBufferType;

class Shared {
	public:
		Shared (std::vector<size_t>& lmax_, std::vector<Math::Matrix<value_type> >& response_, Math::Matrix<value_type>& grad_) :
			lmax(lmax_),
			response(response_),
			grad(grad_)
		{
			/* create forward oonvolution matrix */
			DWI::Shells shells (grad);
			size_t nbvals = shells.count();
			size_t nsamples = grad.rows();
			size_t ntissues = lmax.size();
			size_t nparams = 0;
			size_t maxlmax = 0;
            for(std::vector<size_t>::iterator it = lmax.begin(); it != lmax.end(); ++it) {
	          nparams+=Math::SH::NforL(*it);
			  if (*it > maxlmax)
				maxlmax = *it;
            }
			C.allocate(nsamples,nparams);

			std::vector<size_t> dwilist;
			for (size_t i = 0; i < nsamples; i++)
			  dwilist.push_back(i);
			Math::Matrix<value_type> directions; DWI::gen_direction_matrix (directions, grad, dwilist);
			Math::Matrix<value_type> SHT; Math::SH::init_transform (SHT, directions, maxlmax);
			for (size_t i = 0; i < SHT.rows(); i++)
				for (size_t j = 0; j < SHT.columns(); j++)
					if (isnan(SHT(i,j)))
						SHT(i,j) = 0;

			Math::Matrix<value_type> delta(1,2);
			Math::Matrix<value_type> DSH__; Math::SH::init_transform (DSH__, delta, maxlmax);
			Math::Vector<value_type> DSH_ = DSH__.row(0);
			Math::Vector<value_type> DSH(maxlmax/2+1);
			size_t j = 0;
			for (size_t i = 0; i < DSH_.size(); i++)
				if (DSH_[i] != 0) {
					DSH[j] = DSH_[i];
					j++;
				}

			size_t pbegin = 0;
			for (size_t tissue_idx = 0; tissue_idx < ntissues; tissue_idx++) {
				size_t tissue_lmax = lmax[tissue_idx];
				size_t tissue_n = Math::SH::NforL(tissue_lmax);
				size_t tissue_nmzero = tissue_lmax/2+1;
				for (size_t shell_idx = 0; shell_idx < nbvals; shell_idx++) {
					//std::cout << "TISSUE,SHELL: " << tissue_idx << ", " << shell_idx << std::endl;
					Math::Vector<value_type> response_ = response[tissue_idx].row(shell_idx);
					//std::cout << "RESPONSE_:" << response_ << std::endl;
					Math::Vector<value_type> response__(response_);
					response__/=DSH.sub(0,tissue_nmzero);
					Math::Vector<value_type> fconv(tissue_n);
					int li = 0; int mi = 0;
					for (int l = 0; l <= tissue_lmax; l+=2) {
						for (int m = -l; m <= l; m++) {
							fconv[mi] = response__[li];
							mi++;
						}
						li++;
					}
					//std::cout << "FCONV:" << fconv << std::endl;
					std::vector<size_t> vols = shells[shell_idx].get_volumes();
					for (size_t idx = 0; idx < vols.size(); idx++) {
						Math::Vector<value_type> SHT_(SHT.row(vols[idx]).sub(0,tissue_n));
						Math::Vector<value_type> SHT__(SHT_);
						SHT__*=fconv;
						C.row(vols[idx]).sub(pbegin,pbegin+tissue_n) = SHT__;
					}
				}
				pbegin+=tissue_n;
			}
			
			/* create constraint matrix */
			std::vector<size_t> m(lmax.size());
			std::vector<size_t> n(lmax.size());
			size_t M = 0;
			size_t N = 0;
			
			Math::Matrix<value_type> HR_dirs;
			DWI::Directions::electrostatic_repulsion_300(HR_dirs);
			Math::Matrix<value_type> SHT300; Math::SH::init_transform (SHT300, HR_dirs, maxlmax);
			
			for(size_t i = 0; i < lmax.size(); i++) {
				if (lmax[i] > 0)
					m[i] = HR_dirs.rows();
				else
					m[i] = 1;
				M+=m[i];
				n[i] = Math::SH::NforL(lmax[i]);
				N+=n[i];
			}
			
			A.allocate(M,N);
			//b.allocate(A.rows());
			size_t b_m = 0; size_t b_n = 0;
			for(size_t i = 0; i < lmax.size(); i++) {
				A.sub(b_m,b_m+m[i],b_n,b_n+n[i]) = SHT300.sub(0,m[i],0,n[i]);
				b_m+=m[i]; b_n+=n[i];
			}
			//A*=-1;
			//H.allocate(C.columns(),C.columns());
			//mult(H,value_type(0),value_type(1),CblasTrans,C,CblasNoTrans,C);
		};

	public:
		std::vector<size_t> lmax;
		std::vector<Math::Matrix<value_type> > response;
		Math::Matrix<value_type>& grad;
		Math::Matrix<value_type> C;
		Math::Matrix<value_type> A;
};


class Processor {
  public:
    Processor (
	  InputBufferType::voxel_type& dwi_in_vox,
	  Ptr<MaskBufferType::voxel_type>& mask_in_vox,
	  OutputBufferType::voxel_type& fodf_out_vox,
	  Shared& shared_)
	  :
	  dwi_in(dwi_in_vox),
	  mask_in(mask_in_vox),
	  fodf_out(fodf_out_vox),
	  shared(shared_),
	  dwi(dwi_in.dim(3)),
	  fodf(fodf_out.dim(3))
	  { }

    void operator () (const Image::Iterator& pos) {
      if (!load_data(pos))
        return;
	  // dwi contains the data for one voxel (e.g. 153 x 1)
	  // shared.C is the forward convolution matrix (e.g. 153 x 47)
	  // shared.A is the constraint matrix (e.g. 302 x 47)
	  // shared.b just contains zeros (e.g. 302 x 1)
	  // fodf should be filled with the output of the fitting procedure (e.g 47 x 1)
	  
	  shared.C.save("C_mrtrix.txt");
	  shared.A.save("A_mrtrix.txt");
	  dwi.save("b_mrtrix.txt");
	  Math::ICLS3::Problem<value_type> problem (shared.C, shared.A);
	  Math::ICLS3::Solver<value_type> solver (problem);
	  solver (fodf, dwi);
	  fodf.save("x_mrtrix.txt");
	  write_back (pos);
    }

  private:
    InputBufferType::voxel_type dwi_in;
    Ptr<MaskBufferType::voxel_type> mask_in;
	OutputBufferType::voxel_type fodf_out;
	Shared shared;
	Math::Vector<value_type> dwi;
	Math::Vector<value_type> fodf;
	
    bool load_data (const Image::Iterator& pos) {
      if (mask_in) {
        Image::voxel_assign (*mask_in, pos);
        if (!mask_in->value())
          return false;
      }
	  Image::voxel_assign (dwi_in, pos);
	  
	  Image::Loop loop(3);
      for (loop.start(dwi_in); loop.ok(); loop.next(dwi_in)) {
        dwi[dwi_in[3]] = dwi_in.value();
		if (!std::isfinite (dwi[dwi_in[3]]))
          return false;
        if (dwi[dwi_in[3]] < 0.0)
          dwi[dwi_in[3]] = 0.0;
	  }
	  return true;
    }

    void write_back (const Image::Iterator& pos) {
      Image::voxel_assign (fodf_out, pos);
	  Image::Loop loop(3);
	  for (loop.start(fodf_out); loop.ok(); loop.next(fodf_out)) {
        fodf_out.value() = fodf[fodf_out[3]];
      }
    }

};

void run () {
  /* input DWI image */
  InputBufferType dwi_in_buffer (argument[0], Image::Stride::contiguous_along_axis(3));
  InputBufferType::voxel_type dwi_in_vox (dwi_in_buffer);
  
    /* input mask image */
  Ptr<MaskBufferType> mask_in_buffer;
  Ptr<MaskBufferType::voxel_type> mask_in_vox;
  Options opt = get_options ("mask");
  if (opt.size()) {
    mask_in_buffer = new MaskBufferType (opt[0][0]);
    Image::check_dimensions (*mask_in_buffer, dwi_in_buffer, 0, 3);
    mask_in_vox = new MaskBufferType::voxel_type (*mask_in_buffer);
  }
  
  /* gradient directions from header */
  Math::Matrix<value_type> grad = DWI::get_valid_DW_scheme<value_type> (dwi_in_buffer);
  
  /* for now,  lmaxes are hardcoded instead of read from the commandline */
  std::vector<size_t> lmax;
  lmax.push_back(0);
  lmax.push_back(0);
  lmax.push_back(8);
  
  /* for now, responses are hardcoded instead of read from the commandline */
  Math::Matrix<value_type> r1("csf.txt");
  Math::Matrix<value_type> r2("gm.txt");
  Math::Matrix<value_type> r3("wm.txt");
  std::vector<Math::Matrix<value_type> > response;
  response.push_back(r1);
  response.push_back(r2);
  response.push_back(r3);
  
  /* make sure responses abide to the lmaxes */
  size_t nparams = 0;
  for(size_t i = 0; i < lmax.size(); i++) {
	nparams+=Math::SH::NforL(lmax[i]);
	response[i].resize(response[i].rows(),lmax[i]/2+1);
  }
  
  /* precalculate everything */
  Shared shared (lmax,response,grad);
  

  Image::Header fodf_out_header (dwi_in_buffer); fodf_out_header.set_ndim (4); fodf_out_header.dim (3) = nparams;
  OutputBufferType fodf_out_buffer (argument[1], fodf_out_header);
  OutputBufferType::voxel_type fodf_out_vox (fodf_out_buffer);
  
  Image::ThreadedLoop loop ("working...", dwi_in_vox, 1, 0, 3);
  Processor processor (dwi_in_vox, mask_in_vox, fodf_out_vox, shared);
  loop.run (processor);
}
