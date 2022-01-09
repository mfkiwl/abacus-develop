//This file contains interfaces with libtorch,
//including loading of model and calculating gradients

//The file contains 5 subroutines:
//1. cal_descriptor : obtains descriptors which are eigenvalues of pdm
//      by calling torch::linalg::eigh
//2. cal_gvx : gvx is used for training with force label, which is gradient of descriptors, 
//      calculated by d(des)/dX = d(pdm)/dX * d(des)/d(pdm) = gdmx * gvdm
//      using einsum
//3. cal_gvdm : d(des)/d(pdm)
//      calculated using torch::autograd::grad
//4. load_model : loads model for applying V_delta
//5. cal_gedm : calculates d(E_delta)/d(pdm)
//      this is the term V(D) that enters the expression H_V_delta = |alpha>V(D)<alpha|
//      caculated using torch::autograd::grad

#ifdef __DEEPKS

#include "LCAO_deepks.h"

//calculates descriptors from projected density matrices
void LCAO_Deepks::cal_descriptor(void)
{
    ModuleBase::TITLE("LCAO_Deepks", "cal_descriptor");

    //init pdm_tensor and d_tensor
    torch::Tensor tmp;

    //if pdm_tensor and d_tensor is not empty, clear it !!
    if (!this->d_tensor.empty())
    {
        this->d_tensor.erase(this->d_tensor.begin(), this->d_tensor.end());
    }
    if (!this->pdm_tensor.empty())
    {
        this->pdm_tensor.erase(this->pdm_tensor.begin(), this->pdm_tensor.end());
    }

    for (int inl = 0;inl < this->inlmax;++inl)
    {
        int nm = 2 * inl_l[inl] + 1;
        tmp = torch::ones({ nm, nm }, torch::TensorOptions().dtype(torch::kFloat64));
        for (int m1 = 0;m1 < nm;++m1)
        {
            for (int m2 = 0;m2 < nm;++m2)
            {
                tmp.index_put_({m1, m2}, this->pdm[inl][m1 * nm + m2]);
            }
        }
        //torch::Tensor tmp = torch::from_blob(this->pdm[inl], { nm, nm }, torch::requires_grad());
        tmp.requires_grad_(true);
        this->pdm_tensor.push_back(tmp);
        this->d_tensor.push_back(torch::ones({ nm }, torch::requires_grad(true)));
    }

    //cal d_tensor
    for (int inl = 0;inl < inlmax;++inl)
    {
        torch::Tensor vd;
        std::tuple<torch::Tensor, torch::Tensor> d_v(this->d_tensor[inl], vd);
        //d_v = torch::symeig(pdm_tensor[inl], /*eigenvalues=*/true, /*upper=*/true);
        d_v = torch::linalg::eigh(pdm_tensor[inl], /*uplo*/"U");
        d_tensor[inl] = std::get<0>(d_v);
    }
    return;
}

//calculates gradient of descriptors from gradient of projected density matrices
void LCAO_Deepks::cal_gvx(const int nat)
{
    ModuleBase::TITLE("LCAO_Deepks","cal_gvx");
    //preconditions
    this->cal_gvdm(nat);
    if(!gdmr_vector.empty())
    {
        gdmr_vector.erase(gdmr_vector.begin(),gdmr_vector.end());
    }

    //gdmr_vector : nat(derivative) * 3 * inl(projector) * nm * nm
    if(GlobalV::MY_RANK==0)
    {
        //make gdmx as tensor
        int nlmax = this->inlmax/nat;
        for (int nl=0;nl<nlmax;++nl)
        {
            std::vector<torch::Tensor> bmmv;
            for (int ibt=0;ibt<nat;++ibt)
            {
                std::vector<torch::Tensor> xmmv;
                for (int i=0;i<3;++i)
                {
                    std::vector<torch::Tensor> ammv;
                    for (int iat=0; iat<nat; ++iat)
                    {
                        int inl = iat*nlmax + nl;
                        int nm = 2*this->inl_l[inl]+1;
                        std::vector<double> mmv;
                        for (int m1=0;m1<nm;++m1)
                        {
                            for(int m2=0;m2<nm;++m2)
                            {
                                if(i==0) mmv.push_back(this->gdmx[ibt][inl][m1*nm+m2]);
                                if(i==1) mmv.push_back(this->gdmy[ibt][inl][m1*nm+m2]);
                                if(i==2) mmv.push_back(this->gdmz[ibt][inl][m1*nm+m2]);
                            }
                        }//nm^2
                        torch::Tensor mm = torch::tensor(mmv, torch::TensorOptions().dtype(torch::kFloat64) ).reshape({nm, nm});    //nm*nm
                        ammv.push_back(mm);
                    }
                    torch::Tensor amm = torch::stack(ammv, 0);  //nat*nm*nm
                    xmmv.push_back(amm);
                }
                torch::Tensor bmm = torch::stack(xmmv, 0);  //3*nat*nm*nm
                bmmv.push_back(bmm); 
            }
            this->gdmr_vector.push_back(torch::stack(bmmv, 0)); //nbt*3*nat*nm*nm
        }
        assert(this->gdmr_vector.size()==nlmax);

        //einsum for each inl: 
        //gdmr_vector : b:nat(derivative) * x:3 * a:inl(projector) * m:nm * n:nm
        //gevdm_vector : a:inl * v:nm (descriptor) * m:nm (pdm, dim1) * n:nm (pdm, dim2)
        //gvx_vector : b:nat(derivative) * x:3 * a:inl(projector) * m:nm(descriptor)
        std::vector<torch::Tensor> gvx_vector;
        for (int nl = 0;nl<nlmax;++nl)
        {
            gvx_vector.push_back(at::einsum("bxamn, avmn->bxav", {this->gdmr_vector[nl], this->gevdm_vector[nl]}));
        }
        
        // cat nv-> \sum_nl(nv) = \sum_nl(nm_nl)=des_per_atom
        // concatenate index a(inl) and m(nm)
        this->gvx_tensor = torch::cat(gvx_vector, -1);

        assert(this->gvx_tensor.size(0) == nat);
        assert(this->gvx_tensor.size(1) == 3);
        assert(this->gvx_tensor.size(2) == nat);
        assert(this->gvx_tensor.size(3) == this->des_per_atom);
    }

    return;
}

//dDescriptor / dprojected density matrix
void LCAO_Deepks::cal_gvdm(const int nat)
{
    ModuleBase::TITLE("LCAO_Deepks", "cal_gvdm");
    if(!gevdm_vector.empty())
    {
        gevdm_vector.erase(gevdm_vector.begin(),gevdm_vector.end());
    }
    //cal gevdm(d(EigenValue(D))/dD)
    int nlmax = inlmax/nat;
    for (int nl=0;nl<nlmax;++nl)
    {
        std::vector<torch::Tensor> avmmv;
        for (int iat = 0;iat<nat;++iat)
        {
            int inl = iat*nlmax+nl;
            int nm = 2*this->inl_l[inl]+1;
            //repeat each block for nm times in an additional dimension
            torch::Tensor tmp_x = this->pdm_tensor[inl].reshape({nm, nm}).unsqueeze(0).repeat({nm, 1, 1});
            //torch::Tensor tmp_y = std::get<0>(torch::symeig(tmp_x, true));
            torch::Tensor tmp_y = std::get<0>(torch::linalg::eigh(tmp_x, "U"));
            torch::Tensor tmp_yshell = torch::eye(nm, torch::TensorOptions().dtype(torch::kFloat64));
            std::vector<torch::Tensor> tmp_rpt;     //repeated-pdm-tensor (x)
            std::vector<torch::Tensor> tmp_rdt; //repeated-d-tensor (y)
            std::vector<torch::Tensor> tmp_gst; //gvx-shell
            tmp_rpt.push_back(tmp_x);
            tmp_rdt.push_back(tmp_y);
            tmp_gst.push_back(tmp_yshell);
            std::vector<torch::Tensor> tmp_res;
            tmp_res = torch::autograd::grad(tmp_rdt, tmp_rpt, tmp_gst, false, false, /*allow_unused*/true); //nm(v)**nm*nm
            avmmv.push_back(tmp_res[0]);
        }
        torch::Tensor avmm = torch::stack(avmmv, 0); //nat*nv**nm*nm
        this->gevdm_vector.push_back(avmm);
    }
    assert(this->gevdm_vector.size() == nlmax);
    return;
}

void LCAO_Deepks::load_model(const string& model_file)
{
    ModuleBase::TITLE("LCAO_Deepks", "load_model");

    try
	{
        this->module = torch::jit::load(model_file);
    }
    catch (const c10::Error& e)

	{
        std::cerr << "error loading the model" << std::endl;
        return;
    }
	return;
}

//obtain from the machine learning model dE_delta/dDescriptor
void LCAO_Deepks::cal_gedm(const int nat)
{
    //using this->pdm_tensor
    ModuleBase::TITLE("LCAO_Deepks", "cal_gedm");

    //forward
    std::vector<torch::jit::IValue> inputs;
    //input_dim:(natom, des_per_atom)
    inputs.push_back(torch::cat(this->d_tensor, /*dim=*/0).reshape({ nat, this->des_per_atom }));
    std::vector<torch::Tensor> ec;
    ec.push_back(module.forward(inputs).toTensor());    //Hartree
    this->E_delta = ec[0].item().toDouble() * 2;//Ry; *2 is for Hartree to Ry
    
    //cal gedm
    std::vector<torch::Tensor> gedm_shell;
    gedm_shell.push_back(torch::ones_like(ec[0]));
    this->gedm_tensor = torch::autograd::grad(ec, this->pdm_tensor, gedm_shell, /*retain_grad=*/true, /*create_graph=*/false, /*allow_unused=*/true);

    //gedm_tensor(Hartree) to gedm(Ry)
    for (int inl = 0;inl < inlmax;++inl)
    {
        int nm = 2 * inl_l[inl] + 1;
        for (int m1 = 0;m1 < nm;++m1)
        {
            for (int m2 = 0;m2 < nm;++m2)
            {
                int index = m1 * nm + m2;
                //*2 is for Hartree to Ry
                this->gedm[inl][index] = this->gedm_tensor[inl].index({ m1,m2 }).item().toDouble() * 2;
            }
        }
    }
    return;
}

void LCAO_Deepks::cal_orbital_precalc(const std::vector<ModuleBase::matrix> &dm_hl)
{
    ModuleBase::TITLE("LCAO_Deepks", "calc_orbital_precalc");
    
    const int nat = GlobalC::ucell.nat;
    this->cal_gvdm(nat);
    const double Rcut_Alpha = GlobalC::ORB.Alpha[0].getRcut();
    this->init_orbital_pdm_shell();
   
    for (int T0 = 0; T0 < GlobalC::ucell.ntype; T0++)
    {
		Atom* atom0 = &GlobalC::ucell.atoms[T0]; 
        
        for (int I0 =0; I0< atom0->na; I0++)
        {
            const int iat = GlobalC::ucell.itia2iat(T0,I0);
            const ModuleBase::Vector3<double> tau0 = atom0->tau[I0];
            GlobalC::GridD.Find_atom(GlobalC::ucell, atom0->tau[I0] ,T0, I0);

            for (int ad1=0; ad1<GlobalC::GridD.getAdjacentNum()+1 ; ++ad1)
            {
                const int T1 = GlobalC::GridD.getType(ad1);
                const int I1 = GlobalC::GridD.getNatom(ad1);
                const int start1 = GlobalC::ucell.itiaiw2iwt(T1, I1, 0);
                const ModuleBase::Vector3<double> tau1 = GlobalC::GridD.getAdjacentTau(ad1);
				const Atom* atom1 = &GlobalC::ucell.atoms[T1];
				const int nw1_tot = atom1->nw*GlobalV::NPOL;
				const double Rcut_AO1 = GlobalC::ORB.Phi[T1].getRcut(); 

				for (int ad2=0; ad2 < GlobalC::GridD.getAdjacentNum()+1 ; ad2++)
				{
					const int T2 = GlobalC::GridD.getType(ad2);
					const int I2 = GlobalC::GridD.getNatom(ad2);
					const int start2 = GlobalC::ucell.itiaiw2iwt(T2, I2, 0);
					const ModuleBase::Vector3<double> tau2 = GlobalC::GridD.getAdjacentTau(ad2);
					const Atom* atom2 = &GlobalC::ucell.atoms[T2];
					const int nw2_tot = atom2->nw*GlobalV::NPOL;
					
					const double Rcut_AO2 = GlobalC::ORB.Phi[T2].getRcut();
                	const double dist1 = (tau1-tau0).norm() * GlobalC::ucell.lat0;
                	const double dist2 = (tau2-tau0).norm() * GlobalC::ucell.lat0;

					if (dist1 > Rcut_Alpha + Rcut_AO1 || dist2 > Rcut_Alpha + Rcut_AO2)
					{
						continue;
					}

					for (int iw1=0; iw1<nw1_tot; ++iw1)
					{
						const int iw1_all = start1 + iw1; // this is \mu
						const int iw1_local = GlobalC::ParaO.trace_loc_row[iw1_all];
						if(iw1_local < 0)continue;
						const int iw1_0 = iw1/GlobalV::NPOL;
						for (int iw2=0; iw2<nw2_tot; ++iw2)
						{
							const int iw2_all = start2 + iw2; // this is \nu
							const int iw2_local = GlobalC::ParaO.trace_loc_col[iw2_all];
							if(iw2_local < 0)continue;
							const int iw2_0 = iw2/GlobalV::NPOL;

                            std::vector<double> nlm1 = this->nlm_save[iat][ad1][iw1_all][0];
                            std::vector<double> nlm2 = this->nlm_save[iat][ad2][iw2_all][0];

                            assert(nlm1.size()==nlm2.size());
                            int ib=0;
                            for (int L0 = 0; L0 <= GlobalC::ORB.Alpha[0].getLmax();++L0)
                            {
                                for (int N0 = 0;N0 < GlobalC::ORB.Alpha[0].getNchi(L0);++N0)
                                {
                                    const int inl = this->inl_index[T0](I0, L0, N0);
                                    const int nm = 2*L0+1;
                                    
                                    for (int m1=0; m1<nm; ++m1) // m1 = 1 for s, 3 for p, 5 for d
                                    {
                                        for (int m2=0; m2<nm; ++m2) // m1 = 1 for s, 3 for p, 5 for d
                                        {
                                            //int ispin = 0; //only works for closed shell;
                                            for (int is = 0; is < GlobalV::NSPIN; ++is)
                                            {   
                                                orbital_pdm_shell[0][inl][m1*nm+m2] += dm_hl[0](iw2_local,iw1_local)*nlm1[ib+m1]*nlm2[ib+m2];
                                            } 
                                        }
                                    }
                                    ib+=nm;
                                }
                            }                            

						}//iw2
					}//iw1
				}//ad2
			}//ad1   
            
        }
    }
#ifdef __MPI
    for(int inl = 0; inl < this->inlmax; inl++)
    {
        Parallel_Reduce::reduce_double_all(this->orbital_pdm_shell[0][inl],(2 * this->lmaxd + 1) * (2 * this->lmaxd + 1));
    }
#endif    
    
    // transfer orbital_pdm_shell to orbital_pdm_shell_vector
    

    int nlmax = this->inlmax/GlobalC::ucell.nat;
   
    std::vector<torch::Tensor> orbital_pdm_shell_vector;
    
    for(int nl = 0; nl < nlmax; ++nl)
    {
        std::vector<torch::Tensor> iammv;
        for(int hl=0; hl<1; ++hl)
        {
            std::vector<torch::Tensor> ammv;
            for (int iat=0; iat<GlobalC::ucell.nat; ++iat)
            {
                int inl = iat*nlmax+nl;
                int nm = 2*this->inl_l[inl]+1;
                std::vector<double> mmv;
                
                for (int m1=0; m1<nm; ++m1) // m1 = 1 for s, 3 for p, 5 for d
                {
                    for (int m2=0; m2<nm; ++m2) // m1 = 1 for s, 3 for p, 5 for d
                    {
                        mmv.push_back(this->orbital_pdm_shell[hl][inl][m1*nm+m2]);
                    }
                
                }
                torch::Tensor mm = torch::tensor(mmv, torch::TensorOptions().dtype(torch::kFloat64) ).reshape({nm, nm});    //nm*nm
                ammv.push_back(mm);
            }
            
            torch::Tensor amm = torch::stack(ammv, 0); 
            iammv.push_back(amm);
        }
        
        torch::Tensor iamm = torch::stack(iammv, 0);  //inl*nm*nm
        orbital_pdm_shell_vector.push_back(iamm);
    }
       
    
    assert(orbital_pdm_shell_vector.size() == nlmax);
        
    
    //einsum for each nl: 
    std::vector<torch::Tensor> orbital_precalc_vector;
    for (int nl = 0; nl<nlmax; ++nl)
    {
        orbital_precalc_vector.push_back(at::einsum("iamn, avmn->iav", {orbital_pdm_shell_vector[nl], this->gevdm_vector[nl]}));
    }
       
    this->orbital_precalc_tensor = torch::cat(orbital_precalc_vector, -1);
       
    this->del_orbital_pdm_shell();
	return;
}

#endif