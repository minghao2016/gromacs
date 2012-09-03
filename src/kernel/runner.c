/* -*- mode: c; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; c-file-style: "stroustrup"; -*-
 *
 * 
 *                This source code is part of
 * 
 *                 G   R   O   M   A   C   S
 * 
 *          GROningen MAchine for Chemical Simulations
 * 
 *                        VERSION 3.2.0
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 * 
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 * 
 * For more info, check our website at http://www.gromacs.org
 * 
 * And Hey:
 * Gallium Rubidium Oxygen Manganese Argon Carbon Silicon
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#ifdef __linux
#define _GNU_SOURCE
#include <sched.h>
#include <sys/syscall.h>
#endif
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "typedefs.h"
#include "smalloc.h"
#include "sysstuff.h"
#include "statutil.h"
#include "mdrun.h"
#include "md_logging.h"
#include "md_support.h"
#include "network.h"
#include "pull.h"
#include "names.h"
#include "disre.h"
#include "orires.h"
#include "pme.h"
#include "mdatoms.h"
#include "repl_ex.h"
#include "qmmm.h"
#include "mpelogging.h"
#include "domdec.h"
#include "partdec.h"
#include "coulomb.h"
#include "constr.h"
#include "mvdata.h"
#include "checkpoint.h"
#include "mtop_util.h"
#include "sighandler.h"
#include "tpxio.h"
#include "txtdump.h"
#include "gmx_detect_hardware.h"
#include "gmx_omp_nthreads.h"
#include "pull_rotation.h"
#include "calc_verletbuf.h"
#include "nbnxn_search.h"
#include "../mdlib/nbnxn_consts.h"
#include "gmx_fatal_collective.h"
#include "membed.h"
#include "md_openmm.h"
#include "gmx_omp.h"

#ifdef GMX_LIB_MPI
#include <mpi.h>
#endif
#ifdef GMX_THREAD_MPI
#include "tmpi.h"
#endif

#ifdef GMX_FAHCORE
#include "corewrap.h"
#endif

#ifdef GMX_OPENMM
#include "md_openmm.h"
#endif

#include "gpu_utils.h"
#include "nbnxn_cuda_data_mgmt.h"

typedef struct { 
    gmx_integrator_t *func;
} gmx_intp_t;

/* The array should match the eI array in include/types/enums.h */
#ifdef GMX_OPENMM  /* FIXME do_md_openmm needs fixing */
const gmx_intp_t integrator[eiNR] = { {do_md_openmm}, {do_md_openmm}, {do_md_openmm}, {do_md_openmm}, {do_md_openmm}, {do_md_openmm}, {do_md_openmm}, {do_md_openmm}, {do_md_openmm}, {do_md_openmm}, {do_md_openmm},{do_md_openmm}};
#else
const gmx_intp_t integrator[eiNR] = { {do_md}, {do_steep}, {do_cg}, {do_md}, {do_md}, {do_nm}, {do_lbfgs}, {do_tpi}, {do_tpi}, {do_md}, {do_md},{do_md}};
#endif

gmx_large_int_t     deform_init_init_step_tpx;
matrix              deform_init_box_tpx;
#ifdef GMX_THREAD_MPI
tMPI_Thread_mutex_t deform_init_box_mutex=TMPI_THREAD_MUTEX_INITIALIZER;
#endif


#ifdef GMX_THREAD_MPI
struct mdrunner_arglist
{
    gmx_hw_opt_t *hw_opt;
    FILE *fplog;
    t_commrec *cr;
    int nfile;
    const t_filenm *fnm;
    output_env_t oenv;
    gmx_bool bVerbose;
    gmx_bool bCompact;
    int nstglobalcomm;
    ivec ddxyz;
    int dd_node_order;
    real rdd;
    real rconstr;
    const char *dddlb_opt;
    real dlb_scale;
    const char *ddcsx;
    const char *ddcsy;
    const char *ddcsz;
    const char *nbpu_opt;
    int nsteps_cmdline;
    int nstepout;
    int resetstep;
    int nmultisim;
    int repl_ex_nst;
    int repl_ex_nex;
    int repl_ex_seed;
    real pforce;
    real cpt_period;
    real max_hours;
    const char *deviceOptions;
    unsigned long Flags;
    int ret; /* return value */
};


/* The function used for spawning threads. Extracts the mdrunner() 
   arguments from its one argument and calls mdrunner(), after making
   a commrec. */
static void mdrunner_start_fn(void *arg)
{
    struct mdrunner_arglist *mda=(struct mdrunner_arglist*)arg;
    struct mdrunner_arglist mc=*mda; /* copy the arg list to make sure 
                                        that it's thread-local. This doesn't
                                        copy pointed-to items, of course,
                                        but those are all const. */
    t_commrec *cr;  /* we need a local version of this */
    FILE *fplog=NULL;
    t_filenm *fnm;

    fnm = dup_tfn(mc.nfile, mc.fnm);

    cr = init_par_threads(mc.cr);

    if (MASTER(cr))
    {
        fplog=mc.fplog;
    }

    mda->ret=mdrunner(mc.hw_opt, fplog, cr, mc.nfile, fnm, mc.oenv, 
                      mc.bVerbose, mc.bCompact, mc.nstglobalcomm, 
                      mc.ddxyz, mc.dd_node_order, mc.rdd,
                      mc.rconstr, mc.dddlb_opt, mc.dlb_scale, 
                      mc.ddcsx, mc.ddcsy, mc.ddcsz,
                      mc.nbpu_opt,
                      mc.nsteps_cmdline, mc.nstepout, mc.resetstep,
                      mc.nmultisim, mc.repl_ex_nst, mc.repl_ex_nex, mc.repl_ex_seed, mc.pforce, 
                      mc.cpt_period, mc.max_hours, mc.deviceOptions, mc.Flags);
}

/* called by mdrunner() to start a specific number of threads (including 
   the main thread) for thread-parallel runs. This in turn calls mdrunner()
   for each thread. 
   All options besides nthreads are the same as for mdrunner(). */
static t_commrec *mdrunner_start_threads(gmx_hw_opt_t *hw_opt, 
              FILE *fplog,t_commrec *cr,int nfile, 
              const t_filenm fnm[], const output_env_t oenv, gmx_bool bVerbose,
              gmx_bool bCompact, int nstglobalcomm,
              ivec ddxyz,int dd_node_order,real rdd,real rconstr,
              const char *dddlb_opt,real dlb_scale,
              const char *ddcsx,const char *ddcsy,const char *ddcsz,
              const char *nbpu_opt,
              int nsteps_cmdline, int nstepout,int resetstep,
              int nmultisim,int repl_ex_nst,int repl_ex_nex, int repl_ex_seed,
              real pforce,real cpt_period, real max_hours, 
              const char *deviceOptions, unsigned long Flags)
{
    int ret;
    struct mdrunner_arglist *mda;
    t_commrec *crn; /* the new commrec */
    t_filenm *fnmn;

    /* first check whether we even need to start tMPI */
    if (hw_opt->nthreads_tmpi < 2)
    {
        return cr;
    }

    /* a few small, one-time, almost unavoidable memory leaks: */
    snew(mda,1);
    fnmn=dup_tfn(nfile, fnm);

    /* fill the data structure to pass as void pointer to thread start fn */
    mda->hw_opt=hw_opt;
    mda->fplog=fplog;
    mda->cr=cr;
    mda->nfile=nfile;
    mda->fnm=fnmn;
    mda->oenv=oenv;
    mda->bVerbose=bVerbose;
    mda->bCompact=bCompact;
    mda->nstglobalcomm=nstglobalcomm;
    mda->ddxyz[XX]=ddxyz[XX];
    mda->ddxyz[YY]=ddxyz[YY];
    mda->ddxyz[ZZ]=ddxyz[ZZ];
    mda->dd_node_order=dd_node_order;
    mda->rdd=rdd;
    mda->rconstr=rconstr;
    mda->dddlb_opt=dddlb_opt;
    mda->dlb_scale=dlb_scale;
    mda->ddcsx=ddcsx;
    mda->ddcsy=ddcsy;
    mda->ddcsz=ddcsz;
    mda->nbpu_opt=nbpu_opt;
    mda->nsteps_cmdline=nsteps_cmdline;
    mda->nstepout=nstepout;
    mda->resetstep=resetstep;
    mda->nmultisim=nmultisim;
    mda->repl_ex_nst=repl_ex_nst;
    mda->repl_ex_nex=repl_ex_nex;
    mda->repl_ex_seed=repl_ex_seed;
    mda->pforce=pforce;
    mda->cpt_period=cpt_period;
    mda->max_hours=max_hours;
    mda->deviceOptions=deviceOptions;
    mda->Flags=Flags;

    fprintf(stderr, "Starting %d tMPI threads\n",hw_opt->nthreads_tmpi);
    fflush(stderr);
    /* now spawn new threads that start mdrunner_start_fn(), while 
       the main thread returns */
    ret=tMPI_Init_fn(TRUE, hw_opt->nthreads_tmpi,
                     mdrunner_start_fn, (void*)(mda) );
    if (ret!=TMPI_SUCCESS)
        return NULL;

    /* make a new comm_rec to reflect the new situation */
    crn=init_par_threads(cr);
    return crn;
}


static int get_tmpi_omp_thread_distribution(const gmx_hw_opt_t *hw_opt,
                                            int nthreads_tot,
                                            int ngpu)
{
    int nthreads_tmpi;

    /* There are no separate PME nodes here, as we ensured in
     * check_and_update_hw_opt that nthreads_tmpi>0 with PME nodes
     * and a conditional ensures we would not have ended up here.
     * Note that separate PME nodes might be switched on later.
     */
    if (ngpu > 0)
    {
        nthreads_tmpi = ngpu;
        if (nthreads_tot > 0 && nthreads_tot < nthreads_tmpi)
        {
            nthreads_tmpi = nthreads_tot;
        }
    }
    else if (hw_opt->nthreads_omp > 0)
    {
        if (hw_opt->nthreads_omp > nthreads_tot)
        {
            gmx_fatal(FARGS,"More OpenMP threads requested (%d) than the total number of threads requested (%d)",hw_opt->nthreads_omp,nthreads_tot);
        }
        nthreads_tmpi = nthreads_tot/hw_opt->nthreads_omp;
    }
    else
    {
        /* TODO choose nthreads_omp based on hardware topology
           when we have a hardware topology detection library */
        /* Don't use OpenMP parallelization */
        nthreads_tmpi = nthreads_tot;
    }

    return nthreads_tmpi;
}


/* Get the number of threads to use for thread-MPI based on how many
 * were requested, which algorithms we're using,
 * and how many particles there are.
 * At the point we have already called check_and_update_hw_opt.
 * Thus all options should be internally consistent and consistent
 * with the hardware, except that ntmpi could be larger than #GPU.
 */
static int get_nthreads_mpi(gmx_hw_info_t *hwinfo,
                            gmx_hw_opt_t *hw_opt,
                            t_inputrec *inputrec, gmx_mtop_t *mtop,
                            const t_commrec *cr,
                            FILE *fplog)
{
    int nthreads_tot_max,nthreads_tmpi,nthreads_new,ngpu;
    int min_atoms_per_mpi_thread;
    char *env;
    char sbuf[STRLEN];
    gmx_bool bCanUseGPU;

    if (hw_opt->nthreads_tmpi > 0)
    {
        /* Trivial, return right away */
        return hw_opt->nthreads_tmpi;
    }

    /* How many total (#tMPI*#OpenMP) threads can we start? */ 
    if (hw_opt->nthreads_tot > 0)
    {
        nthreads_tot_max = hw_opt->nthreads_tot;
    }
    else
    {
        nthreads_tot_max = tMPI_Thread_get_hw_number();
    }

    bCanUseGPU = (inputrec->cutoff_scheme == ecutsVERLET && hwinfo->bCanUseGPU);
    if (bCanUseGPU)
    {
        ngpu = hwinfo->gpu_info.ncuda_dev_use;
    }
    else
    {
        ngpu = 0;
    }

    nthreads_tmpi =
        get_tmpi_omp_thread_distribution(hw_opt,nthreads_tot_max,ngpu);

    if (inputrec->eI == eiNM || EI_TPI(inputrec->eI))
    {
        /* Steps are divided over the nodes iso splitting the atoms */
        min_atoms_per_mpi_thread = 0;
    }
    else
    {
        if (bCanUseGPU)
        {
            min_atoms_per_mpi_thread = MIN_ATOMS_PER_GPU;
        }
        else
        {
            min_atoms_per_mpi_thread = MIN_ATOMS_PER_MPI_THREAD;
        }
    }

    /* Check if an algorithm does not support parallel simulation.  */
    if (nthreads_tmpi != 1 &&
        ( inputrec->eI == eiLBFGS ||
          inputrec->coulombtype == eelEWALD ) )
    {
        nthreads_tmpi = 1;

        md_print_warn(cr,fplog,"The integration or electrostatics algorithm doesn't support parallel runs. Using a single thread-MPI thread.\n");
        if (hw_opt->nthreads_tmpi > nthreads_tmpi)
        {
            gmx_fatal(FARGS,"You asked for more than 1 thread-MPI thread, but an algorithm doesn't support that");
        }
    }
    else if (mtop->natoms/nthreads_tmpi < min_atoms_per_mpi_thread)
    {
        /* the thread number was chosen automatically, but there are too many
           threads (too few atoms per thread) */
        nthreads_new = max(1,mtop->natoms/min_atoms_per_mpi_thread);

        if (nthreads_new > 8 || (nthreads_tmpi == 8 && nthreads_new > 4))
        {
            /* TODO replace this once we have proper HT detection
             * Use only multiples of 4 above 8 threads
             * or with an 8-core processor
             * (to avoid 6 threads on 8 core processors with 4 real cores).
             */
            nthreads_new = (nthreads_new/4)*4;
        }
        else if (nthreads_new > 4)
        {
            /* Avoid 5 or 7 threads */
            nthreads_new = (nthreads_new/2)*2;
        }

        nthreads_tmpi = nthreads_new;

        fprintf(stderr,"\n");
        fprintf(stderr,"NOTE: Parallelization is limited by the small number of atoms,\n");
        fprintf(stderr,"      only starting %d thread-MPI threads.\n",nthreads_tmpi);
        fprintf(stderr,"      You can use the -nt and/or -ntmpi option to optimize the number of threads.\n\n");
    }

    return nthreads_tmpi;
}
#endif /* GMX_THREAD_MPI */


/* Environment variable for setting nstlist */
static const char*  NSTLIST_ENVVAR          =  "GMX_NSTLIST";
/* Try to increase nstlist when using a GPU with nstlist less than this */
static const int    NSTLIST_GPU_ENOUGH      = 20;
/* Increase nstlist until the non-bonded cost increases more than this factor */
static const float  NBNXN_GPU_LIST_OK_FAC   = 1.25;
/* Don't increase nstlist beyond a non-bonded cost increases of this factor */
static const float  NBNXN_GPU_LIST_MAX_FAC  = 1.40;

/* Try to increase nstlist when running on a GPU */
static void increase_nstlist(FILE *fp,t_commrec *cr,
                             t_inputrec *ir,const gmx_mtop_t *mtop,matrix box)
{
    char *env;
    int  nstlist_orig,nstlist_prev;
    verletbuf_list_setup_t ls;
    real rlist_inc,rlist_ok,rlist_max,rlist_new,rlist_prev;
    int  i;
    t_state state_tmp;
    gmx_bool bBox,bDD,bCont;
    const char *nstl_fmt="\nFor optimal performance with a GPU nstlist (now %d) should be larger.\nThe optimum depends on your CPU and GPU resources.\nYou might want to try several nstlist values.\n";
    const char *vbd_err="Can not increase nstlist for GPU run because verlet-buffer-drift is not set or used";
    const char *box_err="Can not increase nstlist for GPU run because the box is too small";
    const char *dd_err ="Can not increase nstlist for GPU run because of domain decomposition limitations";
    char buf[STRLEN];

    /* Number of + nstlist alternative values to try when switching  */
    const int nstl[]={ 20, 25, 40, 50 };
#define NNSTL  sizeof(nstl)/sizeof(nstl[0])

    env = getenv(NSTLIST_ENVVAR);
    if (env == NULL)
    {
        if (fp != NULL)
        {
            fprintf(fp,nstl_fmt,ir->nstlist);
        }
    }

    if (ir->verletbuf_drift == 0)
    {
        gmx_fatal(FARGS,"You are using an old tpr file with a GPU, please generate a new tpr file with an up to date version of grompp");
    }

    if (ir->verletbuf_drift < 0)
    {
        if (MASTER(cr))
        {
            fprintf(stderr,"%s\n",vbd_err);
        }
        if (fp != NULL)
        {
            fprintf(fp,"%s\n",vbd_err);
        }

        return;
    }

    nstlist_orig = ir->nstlist;
    if (env != NULL)
    {
        sprintf(buf,"Getting nstlist from environment variable GMX_NSTLIST=%s",env);
        if (MASTER(cr))
        {
            fprintf(stderr,"%s\n",buf);
        }
        if (fp != NULL)
        {
            fprintf(fp,"%s\n",buf);
        }
        sscanf(env,"%d",&ir->nstlist);
    }

    verletbuf_get_list_setup(TRUE,&ls);

    /* Allow rlist to make the list double the size of the cut-off sphere */
    rlist_inc = nbnxn_get_rlist_effective_inc(NBNXN_GPU_CLUSTER_SIZE,mtop->natoms/det(box));
    rlist_ok  = (max(ir->rvdw,ir->rcoulomb) + rlist_inc)*pow(NBNXN_GPU_LIST_OK_FAC,1.0/3.0) - rlist_inc;
    rlist_max = (max(ir->rvdw,ir->rcoulomb) + rlist_inc)*pow(NBNXN_GPU_LIST_MAX_FAC,1.0/3.0) - rlist_inc;
    if (debug)
    {
        fprintf(debug,"GPU nstlist tuning: rlist_inc %.3f rlist_max %.3f\n",
                rlist_inc,rlist_max);
    }

    i = 0;
    nstlist_prev = nstlist_orig;
    rlist_prev   = ir->rlist;
    do
    {
        if (env == NULL)
        {
            ir->nstlist = nstl[i];
        }

        /* Set the pair-list buffer size in ir */
        calc_verlet_buffer_size(mtop,det(box),ir,ir->verletbuf_drift,&ls,
                                NULL,&rlist_new);

        /* Does rlist fit in the box? */
        bBox = (sqr(rlist_new) < max_cutoff2(ir->ePBC,box));
        bDD  = TRUE;
        if (bBox && DOMAINDECOMP(cr))
        {
            /* Check if rlist fits in the domain decomposition */
            if (inputrec2nboundeddim(ir) < DIM)
            {
                gmx_incons("Changing nstlist with domain decomposition and unbounded dimensions is not implemented yet");
            }
            copy_mat(box,state_tmp.box);
            bDD = change_dd_cutoff(cr,&state_tmp,ir,rlist_new);
        }

        bCont = FALSE;

        if (env == NULL)
        {
            if (bBox && bDD && rlist_new <= rlist_max)
            {
                /* Increase nstlist */
                nstlist_prev = ir->nstlist;
                rlist_prev   = rlist_new;
                bCont = (i+1 < NNSTL && rlist_new < rlist_ok);
            }
            else
            {
                /* Stick with the previous nstlist */
                ir->nstlist = nstlist_prev;
                rlist_new   = rlist_prev;
                bBox = TRUE;
                bDD  = TRUE;
            }
        }

        i++;
    }
    while (bCont);

    if (!bBox || !bDD)
    {
        gmx_warning(!bBox ? box_err : dd_err);
        if (fp != NULL)
        {
            fprintf(fp,"\n%s\n",bBox ? box_err : dd_err);
        }
        ir->nstlist = nstlist_orig;
    }
    else if (ir->nstlist != nstlist_orig || rlist_new != ir->rlist)
    {
        sprintf(buf,"Changing nstlist from %d to %d, rlist from %g to %g",
                nstlist_orig,ir->nstlist,
                ir->rlist,rlist_new);
        if (MASTER(cr))
        {
            fprintf(stderr,"%s\n\n",buf);
        }
        if (fp != NULL)
        {
            fprintf(fp,"%s\n\n",buf);
        }
        ir->rlist     = rlist_new;
        ir->rlistlong = rlist_new;
    }
}

static void prepare_verlet_scheme(FILE *fplog,
                                  gmx_hw_info_t *hwinfo,
                                  t_commrec *cr,
                                  gmx_hw_opt_t *hw_opt,
                                  const char *nbpu_opt,
                                  t_inputrec *ir,
                                  const gmx_mtop_t *mtop,
                                  matrix box,
                                  gmx_bool *bUseGPU)
{
    /* Here we only check for GPU usage on the MPI master process,
     * as here we don't know how many GPUs we will use yet.
     * We check for a GPU on all processes later.
     */
    *bUseGPU = hwinfo->bCanUseGPU || (getenv("GMX_EMULATE_GPU") != NULL);

    if (ir->verletbuf_drift > 0)
    {
        /* Update the Verlet buffer size for the current run setup */
        verletbuf_list_setup_t ls;
        real rlist_new;

        /* Here we assume CPU acceleration is on. But as currently
         * calc_verlet_buffer_size gives the same results for 4x8 and 4x4
         * and 4x2 gives a larger buffer than 4x4, this is ok.
         */
        verletbuf_get_list_setup(*bUseGPU,&ls);

        calc_verlet_buffer_size(mtop,det(box),ir,
                                ir->verletbuf_drift,&ls,
                                NULL,&rlist_new);
        if (rlist_new != ir->rlist)
        {
            if (fplog != NULL)
            {
                fprintf(fplog,"\nChanging rlist from %g to %g for non-bonded %dx%d atom kernels\n\n",
                        ir->rlist,rlist_new,
                        ls.cluster_size_i,ls.cluster_size_j);
            }
            ir->rlist     = rlist_new;
            ir->rlistlong = rlist_new;
        }
    }

    /* With GPU or emulation we should check nstlist for performance */
    if ((EI_DYNAMICS(ir->eI) &&
         *bUseGPU &&
         ir->nstlist < NSTLIST_GPU_ENOUGH) ||
        getenv(NSTLIST_ENVVAR) != NULL)
    {
        /* Choose a better nstlist */
        increase_nstlist(fplog,cr,ir,mtop,box);
    }
}

static void convert_to_verlet_scheme(FILE *fplog,
                                     t_inputrec *ir,
                                     gmx_mtop_t *mtop,real box_vol)
{
    char *conv_mesg="Converting input file with group cut-off scheme to the Verlet cut-off scheme";

    if (fplog != NULL)
    {
        fprintf(fplog,"\n%s\n\n",conv_mesg);
    }
    fprintf(stderr,"\n%s\n\n",conv_mesg);

    if (!(ir->vdwtype == evdwCUT &&
          (ir->coulombtype == eelCUT ||
           EEL_RF(ir->coulombtype) ||
           ir->coulombtype == eelPME) &&
          ir->rcoulomb == ir->rvdw))
    {
        gmx_fatal(FARGS,"Can only convert tpr files to the Verlet cut-off scheme if they use cut-off VdW interactions, rcoulomb=rvdw and PME, RF or cut-off electrostatics");
    }

    if (inputrec2nboundeddim(ir) != 3)
    {
        gmx_fatal(FARGS,"Can only convert old tpr files to the Verlet cut-off scheme with 3D pbc");
    }

    if (EI_DYNAMICS(ir->eI) && ir->etc == etcNO)
    {
        gmx_fatal(FARGS,"Will not convert old tpr files to the Verlet cut-off scheme without temperature coupling");
    }

    if (ir->efep != efepNO || ir->implicit_solvent != eisNO)
    {
        gmx_fatal(FARGS,"Will not convert old tpr files to the Verlet cut-off scheme with free-energy calculations or implicit solvent");
    }

    ir->cutoff_scheme   = ecutsVERLET;
    ir->verletbuf_drift = 0.005;

    if (EI_DYNAMICS(ir->eI))
    {
        verletbuf_list_setup_t ls;

        verletbuf_get_list_setup(FALSE,&ls);
        calc_verlet_buffer_size(mtop,box_vol,ir,ir->verletbuf_drift,&ls,
                                NULL,&ir->rlist);
    }
    else
    {
        ir->rlist = 1.05*max(ir->rvdw,ir->rcoulomb);
    }

    gmx_mtop_remove_chargegroups(mtop);
}

#ifdef GMX_THREAD_MPI
/* TODO implement MPI_Scan in thread-MPI */
/* thread-MPI currently lacks MPI_Scan, so here's a partial implementation */
static
int MPI_Scan(void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype, 
             MPI_Op op, MPI_Comm comm)
{
    int rank,size,i;
    int *buf;
    int ret;

    if (!(count == 1 && datatype == MPI_INT && op == MPI_SUM))
    {
        gmx_incons("Invalid use of temporary TMPI MPI_Scan function");
    }
    MPI_Comm_rank(comm, &rank);
    MPI_Comm_size(comm, &size);
    snew(buf, size);
    for(i=0; i<size; i++)
    {
        buf[i] = (i <= rank) ? 0 : ((int *)sendbuf)[0];
    }
    ret = MPI_Allreduce(MPI_IN_PLACE, buf, size, datatype, op, comm);
    ((int *)recvbuf)[0] = buf[rank];
    sfree(buf);

    return ret;
}
#endif

/* Set CPU affinity. Can be important for performance.
   On some systems (e.g. Cray) CPU Affinity is set by default.
   But default assigning doesn't work (well) with only some ranks
   having threads. This causes very low performance.
   External tools have cumbersome syntax for setting affinity
   in the case that only some ranks have threads.
   Thus it is important that GROMACS sets the affinity internally
   if only PME is using threads.
*/
static void set_cpu_affinity(FILE *fplog,
                             const t_commrec *cr,
                             const gmx_hw_opt_t *hw_opt,
                             int nthreads_pme,
                             const t_inputrec *inputrec)
{
#ifdef GMX_OPENMP /* TODO: actually we could do this even without OpenMP?! */
#ifdef __linux /* TODO: only linux? why not everywhere if sched_setaffinity is available */
    if (hw_opt->bThreadPinning)
    {
        int thread, local_nthreads, offset, n_ht_physcore;
        char *env;

        /* threads on this MPI process or TMPI thread */
        if (cr->duty & DUTY_PP)
        {
            local_nthreads = gmx_omp_nthreads_get(emntNonbonded);
        }
        else
        {
            local_nthreads = gmx_omp_nthreads_get(emntPME);
        }

        /* map the current process to cores */
        thread = 0;
#ifdef GMX_MPI
        if (PAR(cr) || MULTISIM(cr))
        {
            /* We need to determine a scan of the thread counts in this
             * compute node.
             */
            MPI_Comm comm_intra;

            MPI_Comm_split(MPI_COMM_WORLD,gmx_hostname_num(),cr->nodeid_intra,
                           &comm_intra);
            MPI_Scan(&local_nthreads,&thread,1,MPI_INT,MPI_SUM,comm_intra);
            MPI_Comm_free(&comm_intra);
        }
#endif

        offset = 0;
        if (hw_opt->core_pinning_offset > 0)
        {
            offset = hw_opt->core_pinning_offset;
            if (SIMMASTER(cr))
            {
                fprintf(stderr, "Applying core pinning offset %d\n", offset);
            }
            if (fplog)
            {
                fprintf(fplog, "Applying core pinning offset %d\n", offset);
            }
        }

        n_ht_physcore = -1;
        if (hw_opt->bPinHyperthreading)
        {
            /* This should ONLY be used with hyperthreading turned on */
            /* FIXME remove this when the hardware detection will have the #threads/package info */
            n_ht_physcore = gmx_omp_get_num_procs()/2;

            if (SIMMASTER(cr))
            {
                fprintf(stderr, "Assuming Hyper-Threading with %d physical cores in a compute node\n",
                        n_ht_physcore);
            }
            if (fplog)
            {
                fprintf(fplog, "Assuming Hyper-Threading with %d physical cores in a compute node\n",
                        n_ht_physcore);
            }
        }

        /* set the per-thread affinity */
#pragma omp parallel firstprivate(thread) num_threads(local_nthreads)
        {
            cpu_set_t mask;
            int core;

            CPU_ZERO(&mask);
            thread += gmx_omp_get_thread_num();
            if (n_ht_physcore <= 0)
            {
                core = offset + thread;
            }
            else
            {
                /* Lock pairs of threads to the same hyperthreaded core */
                core = offset + thread/2 + (thread % 2)*n_ht_physcore;
            }
            CPU_SET(core, &mask);
            sched_setaffinity((pid_t) syscall (SYS_gettid), sizeof(cpu_set_t), &mask);
        }
    }
#endif /* __linux    */
#endif /* GMX_OPENMP */
}


static void check_and_update_hw_opt(gmx_hw_opt_t *hw_opt,
                                    int cutoff_scheme)
{
    char *env;

    if ((env = getenv("OMP_NUM_THREADS")) != NULL)
    {
        int nt_omp;

        sscanf(env,"%d",&nt_omp);
        if (nt_omp <= 0)
        {
            gmx_fatal(FARGS,"OMP_NUM_THREADS is invalid: '%s'",env);
        }

        if (hw_opt->nthreads_omp > 0 && nt_omp != hw_opt->nthreads_omp)
        {
            gmx_fatal(FARGS,"OMP_NUM_THREADS (%d) and the number of threads requested on the command line (%d) have different values",nt_omp,hw_opt->nthreads_omp);
        }

        /* Setting the number of OpenMP threads.
         * NOTE: this function is only called on the master node.
         */
        fprintf(stderr,"Getting the number of OpenMP threads from OMP_NUM_THREADS: %d\n",nt_omp);
        hw_opt->nthreads_omp = nt_omp;
    }

#ifndef GMX_THREAD_MPI
    if (hw_opt->nthreads_tot > 0)
    {
        gmx_fatal(FARGS,"Setting the total number of threads is only supported with thread-MPI and Gromacs was compiled without thread-MPI");
    }
    if (hw_opt->nthreads_tmpi > 0)
    {
        gmx_fatal(FARGS,"Setting the number of thread-MPI threads is only supported with thread-MPI and Gromacs was compiled without thread-MPI");
    }
#endif

    if (hw_opt->nthreads_tot > 0 && hw_opt->nthreads_omp_pme <= 0)
    {
        /* We have the same number of OpenMP threads for PP and PME processes,
         * thus we can perform several consistency checks.
         */
        if (hw_opt->nthreads_tmpi > 0 &&
            hw_opt->nthreads_omp > 0 &&
            hw_opt->nthreads_tot != hw_opt->nthreads_tmpi*hw_opt->nthreads_omp)
        {
            gmx_fatal(FARGS,"The total number of threads requested (%d) does not match the thread-MPI threads (%d) times the OpenMP threads (%d) requested",
                      hw_opt->nthreads_tot,hw_opt->nthreads_tmpi,hw_opt->nthreads_omp);
        }

        if (hw_opt->nthreads_tmpi > 0 &&
            hw_opt->nthreads_tot % hw_opt->nthreads_tmpi != 0)
        {
            gmx_fatal(FARGS,"The total number of threads requested (%d) is not divisible by the number of thread-MPI threads requested (%d)",
                      hw_opt->nthreads_tot,hw_opt->nthreads_tmpi);
        }

        if (hw_opt->nthreads_omp > 0 &&
            hw_opt->nthreads_tot % hw_opt->nthreads_omp != 0)
        {
            gmx_fatal(FARGS,"The total number of threads requested (%d) is not divisible by the number of OpenMP threads requested (%d)",
                      hw_opt->nthreads_tot,hw_opt->nthreads_omp);
        }

        if (hw_opt->nthreads_tmpi > 0 &&
            hw_opt->nthreads_omp <= 0)
        {
            hw_opt->nthreads_omp = hw_opt->nthreads_tot/hw_opt->nthreads_tmpi;
        }
    }

#ifndef GMX_OPENMP
    if (hw_opt->nthreads_omp > 1)
    {
        gmx_fatal(FARGS,"OpenMP threads are requested, but Gromacs was compiled without OpenMP support");
    }
#endif

    if (cutoff_scheme == ecutsGROUP)
    {
        /* We only have OpenMP support for PME only nodes */
        if (hw_opt->nthreads_omp > 1)
        {
            gmx_fatal(FARGS,"OpenMP threads have been requested with cut-off scheme %s, but these are only supported with cut-off scheme %s",
                      ecutscheme_names[cutoff_scheme],
                      ecutscheme_names[ecutsVERLET]);
        }
        hw_opt->nthreads_omp = 1;
    }

    if (hw_opt->nthreads_omp_pme > 0 && hw_opt->nthreads_omp <= 0)
    {
        gmx_fatal(FARGS,"You need to specify -ntomp in addition to -ntomp_pme");
    }

    if (hw_opt->nthreads_tot == 1)
    {
        hw_opt->nthreads_tmpi = 1;

        if (hw_opt->nthreads_omp > 1)
        {
            gmx_fatal(FARGS,"You requested %d OpenMP threads with %d total threads",
                      hw_opt->nthreads_tmpi,hw_opt->nthreads_tot);
        }
        hw_opt->nthreads_omp = 1;
    }

    if (hw_opt->nthreads_omp_pme <= 0 && hw_opt->nthreads_omp > 0)
    {
        hw_opt->nthreads_omp_pme = hw_opt->nthreads_omp;
    }

    if (debug)
    {
        fprintf(debug,"hw_opt: nt %d ntmpi %d ntomp %d ntomp_pme %d gpu_id '%s'\n",
                hw_opt->nthreads_tot,
                hw_opt->nthreads_tmpi,
                hw_opt->nthreads_omp,
                hw_opt->nthreads_omp_pme,
                hw_opt->gpu_id!=NULL ? hw_opt->gpu_id : "");
                
    }
}


/* Override the value in inputrec with value passed on the commnad line (if any) */
static void override_nsteps_cmdline(FILE *fplog,
                                    int nsteps_cmdline,
                                    t_inputrec *ir,
                                    const t_commrec *cr)
{
    assert(ir);
    assert(cr);

    /* override with anything else than the default -2 */
    if (nsteps_cmdline > -2)
    {
        char stmp[STRLEN];

        ir->nsteps = nsteps_cmdline;
        if (EI_DYNAMICS(ir->eI))
        {
            sprintf(stmp, "Overriding nsteps with value passed on the command line: %d steps, %.3f ps",
                    nsteps_cmdline, nsteps_cmdline*ir->delta_t);
        }
        else
        {
            sprintf(stmp, "Overriding nsteps with value passed on the command line: %d steps",
                    nsteps_cmdline);
        }

        md_print_warn(cr, fplog, "%s\n", stmp);
    }
}

/* Data structure set by SIMMASTER which needs to be passed to all nodes
 * before the other nodes have read the tpx file and called gmx_detect_hardware.
 */
typedef struct {
    int      cutoff_scheme; /* The cutoff-scheme from inputrec_t */
    gmx_bool bUseGPU;       /* Use GPU or GPU emulation          */
} master_inf_t;

int mdrunner(gmx_hw_opt_t *hw_opt,
             FILE *fplog,t_commrec *cr,int nfile,
             const t_filenm fnm[], const output_env_t oenv, gmx_bool bVerbose,
             gmx_bool bCompact, int nstglobalcomm,
             ivec ddxyz,int dd_node_order,real rdd,real rconstr,
             const char *dddlb_opt,real dlb_scale,
             const char *ddcsx,const char *ddcsy,const char *ddcsz,
             const char *nbpu_opt,
             int nsteps_cmdline, int nstepout,int resetstep,
             int nmultisim,int repl_ex_nst,int repl_ex_nex,
             int repl_ex_seed, real pforce,real cpt_period,real max_hours,
             const char *deviceOptions, unsigned long Flags)
{
    gmx_bool   bForceUseGPU,bTryUseGPU;
    double     nodetime=0,realtime;
    t_inputrec *inputrec;
    t_state    *state=NULL;
    matrix     box;
    gmx_ddbox_t ddbox={0};
    int        npme_major,npme_minor;
    real       tmpr1,tmpr2;
    t_nrnb     *nrnb;
    gmx_mtop_t *mtop=NULL;
    t_mdatoms  *mdatoms=NULL;
    t_forcerec *fr=NULL;
    t_fcdata   *fcd=NULL;
    real       ewaldcoeff=0;
    gmx_pme_t  *pmedata=NULL;
    gmx_vsite_t *vsite=NULL;
    gmx_constr_t constr;
    int        i,m,nChargePerturbed=-1,status,nalloc;
    char       *gro;
    gmx_wallcycle_t wcycle;
    gmx_bool       bReadRNG,bReadEkin;
    int        list;
    gmx_runtime_t runtime;
    int        rc;
    gmx_large_int_t reset_counters;
    gmx_edsam_t ed=NULL;
    t_commrec   *cr_old=cr; 
    int         nthreads_pme=1;
    int         nthreads_pp=1;
    gmx_membed_t membed=NULL;
    gmx_hw_info_t *hwinfo=NULL;
    master_inf_t minf={-1,FALSE};

    /* CAUTION: threads may be started later on in this function, so
       cr doesn't reflect the final parallel state right now */
    snew(inputrec,1);
    snew(mtop,1);
    
    if (Flags & MD_APPENDFILES) 
    {
        fplog = NULL;
    }

    bForceUseGPU = (strncmp(nbpu_opt, "gpu", 3) == 0);
    bTryUseGPU   = (strncmp(nbpu_opt, "auto", 4) == 0) || bForceUseGPU;

    snew(state,1);
    if (SIMMASTER(cr)) 
    {
        /* Read (nearly) all data required for the simulation */
        read_tpx_state(ftp2fn(efTPX,nfile,fnm),inputrec,state,NULL,mtop);

        if (inputrec->cutoff_scheme != ecutsVERLET &&
            getenv("GMX_VERLET_SCHEME") != NULL)
        {
            convert_to_verlet_scheme(fplog,inputrec,mtop,det(state->box));
        }

        /* Detect hardware, gather information. With tMPI only thread 0 does it
         * and after threads are started broadcasts hwinfo around. */
        snew(hwinfo, 1);
        gmx_detect_hardware(fplog, hwinfo, cr,
                            bForceUseGPU, bTryUseGPU, hw_opt->gpu_id);

        minf.cutoff_scheme = inputrec->cutoff_scheme;
        minf.bUseGPU       = FALSE;

        if (inputrec->cutoff_scheme == ecutsVERLET)
        {
            prepare_verlet_scheme(fplog,hwinfo,cr,hw_opt,nbpu_opt,
                                  inputrec,mtop,state->box,
                                  &minf.bUseGPU);
        }
        else if (hwinfo->bCanUseGPU)
        {
            if (bForceUseGPU)
            {
                gmx_fatal(FARGS,"GPU requested, but can't be used without cutoff-scheme=Verlet");
            }

            md_print_warn(cr,fplog,
                          "NOTE: GPU(s) found, but the current simulation can not use GPUs\n"
                          "      To use a GPU, set the mdp option: cutoff-scheme = Verlet\n");
        }
    }
#ifndef GMX_THREAD_MPI
    if (PAR(cr))
    {
        gmx_bcast_sim(sizeof(minf),&minf,cr);
    }
#endif
    if (minf.bUseGPU && cr->npmenodes == -1)
    {
        /* Don't automatically use PME-only nodes with GPUs */
        cr->npmenodes = 0;
    }

#ifdef GMX_THREAD_MPI
    /* With thread-MPI inputrec is only set here on the master thread */
    if (SIMMASTER(cr))
#endif
    {
        check_and_update_hw_opt(hw_opt,minf.cutoff_scheme);

#ifdef GMX_THREAD_MPI
        if (cr->npmenodes > 0 && hw_opt->nthreads_tmpi <= 0)
        {
            gmx_fatal(FARGS,"You need to explicitly specify the number of MPI threads (-nt_mpi) when using separate PME nodes");
        }
#endif

        if (hw_opt->nthreads_omp_pme != hw_opt->nthreads_omp &&
            cr->npmenodes <= 0)
        {
            gmx_fatal(FARGS,"You need to explicitly specify the number of PME nodes (-npme) when using different number of OpenMP threads for PP and PME nodes");
        }
    }

#ifdef GMX_THREAD_MPI
    if (SIMMASTER(cr))
    {
        /* NOW the threads will be started: */
        hw_opt->nthreads_tmpi = get_nthreads_mpi(hwinfo,
                                                 hw_opt,
                                                 inputrec, mtop,
                                                 cr, fplog);
        if (hw_opt->nthreads_tot > 0 && hw_opt->nthreads_omp <= 0)
        {
            hw_opt->nthreads_omp = hw_opt->nthreads_tot/hw_opt->nthreads_tmpi;
        }

        if (hw_opt->nthreads_tmpi > 1)
        {
            /* now start the threads. */
            cr=mdrunner_start_threads(hw_opt, fplog, cr_old, nfile, fnm, 
                                      oenv, bVerbose, bCompact, nstglobalcomm, 
                                      ddxyz, dd_node_order, rdd, rconstr, 
                                      dddlb_opt, dlb_scale, ddcsx, ddcsy, ddcsz,
                                      nbpu_opt,
                                      nsteps_cmdline, nstepout, resetstep, nmultisim, 
                                      repl_ex_nst, repl_ex_nex, repl_ex_seed, pforce,
                                      cpt_period, max_hours, deviceOptions, 
                                      Flags);
            /* the main thread continues here with a new cr. We don't deallocate
               the old cr because other threads may still be reading it. */
            if (cr == NULL)
            {
                gmx_comm("Failed to spawn threads");
            }
        }
    }
#endif
    /* END OF CAUTION: cr is now reliable */

    /* g_membed initialisation *
     * Because we change the mtop, init_membed is called before the init_parallel *
     * (in case we ever want to make it run in parallel) */
    if (opt2bSet("-membed",nfile,fnm))
    {
        if (MASTER(cr))
        {
            fprintf(stderr,"Initializing membed");
        }
        membed = init_membed(fplog,nfile,fnm,mtop,inputrec,state,cr,&cpt_period);
    }

    if (PAR(cr))
    {
        /* now broadcast everything to the non-master nodes/threads: */
        init_parallel(fplog, cr, inputrec, mtop);

        /* This check needs to happen after get_nthreads_mpi() */
        if (inputrec->cutoff_scheme == ecutsVERLET && (Flags & MD_PARTDEC))
        {
            gmx_fatal_collective(FARGS,cr,NULL,
                                 "The Verlet cut-off scheme is not supported with particle decomposition.\n"
                                 "You can achieve the same effect as particle decomposition by running in parallel using only OpenMP threads.");
        }
    }
    if (fplog != NULL)
    {
        pr_inputrec(fplog,0,"Input Parameters",inputrec,FALSE);
    }

#if defined GMX_THREAD_MPI
    /* With tMPI we detected on thread 0 and we'll just pass the hwinfo pointer
     * to the other threads  -- slightly uncool, but works fine, just need to
     * make sure that the data doesn't get freed twice. */
    if (cr->nnodes > 1)
    {
        if (!SIMMASTER(cr))
        {
            snew(hwinfo, 1);
        }
        gmx_bcast(sizeof(&hwinfo), &hwinfo, cr);
    }
#else
    if (PAR(cr) && !SIMMASTER(cr))
    {
        /* now we have inputrec on all nodes, can run the detection */
        /* TODO: perhaps it's better to propagate within a node instead? */
        snew(hwinfo, 1);
        gmx_detect_hardware(fplog, hwinfo, cr,
                                 bForceUseGPU, bTryUseGPU, hw_opt->gpu_id);
    }
#endif

    /* TODO this should use the hwinfo->cpu_info as soon as this will include
     * information on the number of cores and threads/package */
    gmx_omp_nthreads_detecthw();

    /* now make sure the state is initialized and propagated */
    set_state_entries(state,inputrec,cr->nnodes);

    /* remove when vv and rerun works correctly! */
    if (PAR(cr) && EI_VV(inputrec->eI) && ((Flags & MD_RERUN) || (Flags & MD_RERUN_VSITE)))
    {
        gmx_fatal(FARGS,
                  "Currently can't do velocity verlet with rerun in parallel.");
    }

    /* A parallel command line option consistency check that we can
       only do after any threads have started. */
    if (!PAR(cr) &&
        (ddxyz[XX] > 1 || ddxyz[YY] > 1 || ddxyz[ZZ] > 1 || cr->npmenodes > 0))
    {
        gmx_fatal(FARGS,
                  "The -dd or -npme option request a parallel simulation, "
#ifndef GMX_MPI
                  "but %s was compiled without threads or MPI enabled"
#else
#ifdef GMX_THREAD_MPI
                  "but the number of threads (option -nt) is 1"
#else
                  "but %s was not started through mpirun/mpiexec or only one process was requested through mpirun/mpiexec"
#endif
#endif
                  , ShortProgram()
            );
    }

    if ((Flags & MD_RERUN) &&
        (EI_ENERGY_MINIMIZATION(inputrec->eI) || eiNM == inputrec->eI))
    {
        gmx_fatal(FARGS, "The .mdp file specified an energy mininization or normal mode algorithm, and these are not compatible with mdrun -rerun");
    }

    if (can_use_allvsall(inputrec,mtop,TRUE,cr,fplog) && PAR(cr))
    {
        /* All-vs-all loops do not work with domain decomposition */
        Flags |= MD_PARTDEC;
    }

    if (!EEL_PME(inputrec->coulombtype) || (Flags & MD_PARTDEC))
    {
        if (cr->npmenodes > 0)
        {
            if (!EEL_PME(inputrec->coulombtype))
            {
                gmx_fatal_collective(FARGS,cr,NULL,
                                     "PME nodes are requested, but the system does not use PME electrostatics");
            }
            if (Flags & MD_PARTDEC)
            {
                gmx_fatal_collective(FARGS,cr,NULL,
                                     "PME nodes are requested, but particle decomposition does not support separate PME nodes");
            }
        }

        cr->npmenodes = 0;
    }

#ifdef GMX_FAHCORE
    fcRegisterSteps(inputrec->nsteps,inputrec->init_step);
#endif

    /* NMR restraints must be initialized before load_checkpoint,
     * since with time averaging the history is added to t_state.
     * For proper consistency check we therefore need to extend
     * t_state here.
     * So the PME-only nodes (if present) will also initialize
     * the distance restraints.
     */
    snew(fcd,1);

    /* This needs to be called before read_checkpoint to extend the state */
    init_disres(fplog,mtop,inputrec,cr,Flags & MD_PARTDEC,fcd,state);

    if (gmx_mtop_ftype_count(mtop,F_ORIRES) > 0)
    {
        if (PAR(cr) && !(Flags & MD_PARTDEC))
        {
            gmx_fatal(FARGS,"Orientation restraints do not work (yet) with domain decomposition, use particle decomposition (mdrun option -pd)");
        }
        /* Orientation restraints */
        if (MASTER(cr))
        {
            init_orires(fplog,mtop,state->x,inputrec,cr->ms,&(fcd->orires),
                        state);
        }
    }

    if (DEFORM(*inputrec))
    {
        /* Store the deform reference box before reading the checkpoint */
        if (SIMMASTER(cr))
        {
            copy_mat(state->box,box);
        }
        if (PAR(cr))
        {
            gmx_bcast(sizeof(box),box,cr);
        }
        /* Because we do not have the update struct available yet
         * in which the reference values should be stored,
         * we store them temporarily in static variables.
         * This should be thread safe, since they are only written once
         * and with identical values.
         */
#ifdef GMX_THREAD_MPI
        tMPI_Thread_mutex_lock(&deform_init_box_mutex);
#endif
        deform_init_init_step_tpx = inputrec->init_step;
        copy_mat(box,deform_init_box_tpx);
#ifdef GMX_THREAD_MPI
        tMPI_Thread_mutex_unlock(&deform_init_box_mutex);
#endif
    }

    if (opt2bSet("-cpi",nfile,fnm)) 
    {
        /* Check if checkpoint file exists before doing continuation.
         * This way we can use identical input options for the first and subsequent runs...
         */
        if( gmx_fexist_master(opt2fn_master("-cpi",nfile,fnm,cr),cr) )
        {
            load_checkpoint(opt2fn_master("-cpi",nfile,fnm,cr),&fplog,
                            cr,Flags & MD_PARTDEC,ddxyz,
                            inputrec,state,&bReadRNG,&bReadEkin,
                            (Flags & MD_APPENDFILES),
                            (Flags & MD_APPENDFILESSET));
            
            if (bReadRNG)
            {
                Flags |= MD_READ_RNG;
            }
            if (bReadEkin)
            {
                Flags |= MD_READ_EKIN;
            }
        }
    }

    if (((MASTER(cr) || (Flags & MD_SEPPOT)) && (Flags & MD_APPENDFILES))
#ifdef GMX_THREAD_MPI
        /* With thread MPI only the master node/thread exists in mdrun.c,
         * therefore non-master nodes need to open the "seppot" log file here.
         */
        || (!MASTER(cr) && (Flags & MD_SEPPOT))
#endif
        )
    {
        gmx_log_open(ftp2fn(efLOG,nfile,fnm),cr,!(Flags & MD_SEPPOT),
                             Flags,&fplog);
    }

    /* override nsteps with value from cmdline */
    override_nsteps_cmdline(fplog, nsteps_cmdline, inputrec, cr);

    if (SIMMASTER(cr)) 
    {
        copy_mat(state->box,box);
    }

    if (PAR(cr)) 
    {
        gmx_bcast(sizeof(box),box,cr);
    }

    /* Essential dynamics */
    if (opt2bSet("-ei",nfile,fnm))
    {
        /* Open input and output files, allocate space for ED data structure */
        ed = ed_open(nfile,fnm,Flags,cr);
    }

    if (PAR(cr) && !((Flags & MD_PARTDEC) ||
                     EI_TPI(inputrec->eI) ||
                     inputrec->eI == eiNM))
    {
        cr->dd = init_domain_decomposition(fplog,cr,Flags,ddxyz,rdd,rconstr,
                                           dddlb_opt,dlb_scale,
                                           ddcsx,ddcsy,ddcsz,
                                           mtop,inputrec,
                                           box,state->x,
                                           &ddbox,&npme_major,&npme_minor);

        make_dd_communicators(fplog,cr,dd_node_order);

        /* Set overallocation to avoid frequent reallocation of arrays */
        set_over_alloc_dd(TRUE);
    }
    else
    {
        /* PME, if used, is done on all nodes with 1D decomposition */
        cr->npmenodes = 0;
        cr->duty = (DUTY_PP | DUTY_PME);
        npme_major = 1;
        npme_minor = 1;
        if (!EI_TPI(inputrec->eI))
        {
            npme_major = cr->nnodes;
        }
        
        if (inputrec->ePBC == epbcSCREW)
        {
            gmx_fatal(FARGS,
                      "pbc=%s is only implemented with domain decomposition",
                      epbc_names[inputrec->ePBC]);
        }
    }

    if (PAR(cr))
    {
        /* After possible communicator splitting in make_dd_communicators.
         * we can set up the intra/inter node communication.
         */
        gmx_setup_nodecomm(fplog,cr);
    }

    /* Initialize per-node process ID and counters. */
    gmx_init_intra_counters(cr);

#ifdef GMX_MPI
    md_print_info(cr,fplog,"Using %d MPI %s\n",
                  cr->nnodes,
#ifdef GMX_THREAD_MPI
                  cr->nnodes==1 ? "thread" : "threads"
#else
                  cr->nnodes==1 ? "process" : "processes"
#endif
                  );
#endif

    gmx_omp_nthreads_init(fplog, cr,
                          hw_opt->nthreads_omp,
                          hw_opt->nthreads_omp_pme,
                          (cr->duty & DUTY_PP) == 0,
                          inputrec->cutoff_scheme == ecutsVERLET);

    gmx_check_hw_runconf_consistency(fplog, hwinfo, cr, hw_opt->nthreads_tmpi, minf.bUseGPU);

    /* getting number of PP/PME threads
       PME: env variable should be read only on one node to make sure it is 
       identical everywhere;
     */
    /* TODO nthreads_pp is only used for pinning threads.
     * This is a temporary solution until we have a hw topology library.
     */
    nthreads_pp  = gmx_omp_nthreads_get(emntNonbonded);
    nthreads_pme = gmx_omp_nthreads_get(emntPME);

    wcycle = wallcycle_init(fplog,resetstep,cr,nthreads_pp,nthreads_pme);

    if (PAR(cr))
    {
        /* Master synchronizes its value of reset_counters with all nodes 
         * including PME only nodes */
        reset_counters = wcycle_get_reset_counters(wcycle);
        gmx_bcast_sim(sizeof(reset_counters),&reset_counters,cr);
        wcycle_set_reset_counters(wcycle, reset_counters);
    }

    snew(nrnb,1);
    if (cr->duty & DUTY_PP)
    {
        /* For domain decomposition we allocate dynamically
         * in dd_partition_system.
         */
        if (DOMAINDECOMP(cr))
        {
            bcast_state_setup(cr,state);
        }
        else
        {
            if (PAR(cr))
            {
                bcast_state(cr,state,TRUE);
            }
        }

        /* Initiate forcerecord */
        fr = mk_forcerec();
        fr->hwinfo = hwinfo;
        init_forcerec(fplog,oenv,fr,fcd,inputrec,mtop,cr,box,FALSE,
                      opt2fn("-table",nfile,fnm),
                      opt2fn("-tabletf",nfile,fnm),
                      opt2fn("-tablep",nfile,fnm),
                      opt2fn("-tableb",nfile,fnm),
                      nbpu_opt,
                      FALSE,pforce);

        /* version for PCA_NOT_READ_NODE (see md.c) */
        /*init_forcerec(fplog,fr,fcd,inputrec,mtop,cr,box,FALSE,
          "nofile","nofile","nofile","nofile",FALSE,pforce);
          */        
        fr->bSepDVDL = ((Flags & MD_SEPPOT) == MD_SEPPOT);

        /* Initialize QM-MM */
        if(fr->bQMMM)
        {
            init_QMMMrec(cr,box,mtop,inputrec,fr);
        }

        /* Initialize the mdatoms structure.
         * mdatoms is not filled with atom data,
         * as this can not be done now with domain decomposition.
         */
        mdatoms = init_mdatoms(fplog,mtop,inputrec->efep!=efepNO);

        /* Initialize the virtual site communication */
        vsite = init_vsite(mtop,cr);

        calc_shifts(box,fr->shift_vec);

        /* With periodic molecules the charge groups should be whole at start up
         * and the virtual sites should not be far from their proper positions.
         */
        if (!inputrec->bContinuation && MASTER(cr) &&
            !(inputrec->ePBC != epbcNONE && inputrec->bPeriodicMols))
        {
            /* Make molecules whole at start of run */
            if (fr->ePBC != epbcNONE)
            {
                do_pbc_first_mtop(fplog,inputrec->ePBC,box,mtop,state->x);
            }
            if (vsite)
            {
                /* Correct initial vsite positions are required
                 * for the initial distribution in the domain decomposition
                 * and for the initial shell prediction.
                 */
                construct_vsites_mtop(fplog,vsite,mtop,state->x);
            }
        }

        if (EEL_PME(fr->eeltype))
        {
            ewaldcoeff = fr->ewaldcoeff;
            pmedata = &fr->pmedata;
        }
        else
        {
            pmedata = NULL;
        }
    }
    else
    {
        /* This is a PME only node */

        /* We don't need the state */
        done_state(state);

        ewaldcoeff = calc_ewaldcoeff(inputrec->rcoulomb, inputrec->ewald_rtol);
        snew(pmedata,1);
    }

#if defined GMX_THREAD_MPI && defined TMPI_THREAD_AFFINITY
    /* With the number of TMPI threads equal to the number of cores
     * we already pinned in thread-MPI, so don't pin again here.
     */
    if (hw_opt->nthreads_tmpi != tMPI_Thread_get_hw_number())
#endif
    {
        /* Set the CPU affinity */
        set_cpu_affinity(fplog,cr,hw_opt,nthreads_pme,inputrec);
    }

    /* Initiate PME if necessary,
     * either on all nodes or on dedicated PME nodes only. */
    if (EEL_PME(inputrec->coulombtype))
    {
        if (mdatoms)
        {
            nChargePerturbed = mdatoms->nChargePerturbed;
        }
        if (cr->npmenodes > 0)
        {
            /* The PME only nodes need to know nChargePerturbed */
            gmx_bcast_sim(sizeof(nChargePerturbed),&nChargePerturbed,cr);
        }

        if (cr->duty & DUTY_PME)
        {
            status = gmx_pme_init(pmedata,cr,npme_major,npme_minor,inputrec,
                                  mtop ? mtop->natoms : 0,nChargePerturbed,
                                  (Flags & MD_REPRODUCIBLE),nthreads_pme);
            if (status != 0) 
            {
                gmx_fatal(FARGS,"Error %d initializing PME",status);
            }
        }
    }


    if (integrator[inputrec->eI].func == do_md
#ifdef GMX_OPENMM
        ||
        integrator[inputrec->eI].func == do_md_openmm
#endif
        )
    {
        /* Turn on signal handling on all nodes */
        /*
         * (A user signal from the PME nodes (if any)
         * is communicated to the PP nodes.
         */
        signal_handler_install();
    }

    if (cr->duty & DUTY_PP)
    {
        if (inputrec->ePull != epullNO)
        {
            /* Initialize pull code */
            init_pull(fplog,inputrec,nfile,fnm,mtop,cr,oenv, inputrec->fepvals->init_lambda,
                      EI_DYNAMICS(inputrec->eI) && MASTER(cr),Flags);
        }
        
        if (inputrec->bRot)
        {
           /* Initialize enforced rotation code */
           init_rot(fplog,inputrec,nfile,fnm,cr,state->x,box,mtop,oenv,
                    bVerbose,Flags);
        }

        constr = init_constraints(fplog,mtop,inputrec,ed,state,cr);

        if (DOMAINDECOMP(cr))
        {
            dd_init_bondeds(fplog,cr->dd,mtop,vsite,constr,inputrec,
                            Flags & MD_DDBONDCHECK,fr->cginfo_mb);

            set_dd_parameters(fplog,cr->dd,dlb_scale,inputrec,fr,&ddbox);

            setup_dd_grid(fplog,cr->dd);
        }

        /* Now do whatever the user wants us to do (how flexible...) */
        integrator[inputrec->eI].func(fplog,cr,nfile,fnm,
                                      oenv,bVerbose,bCompact,
                                      nstglobalcomm,
                                      vsite,constr,
                                      nstepout,inputrec,mtop,
                                      fcd,state,
                                      mdatoms,nrnb,wcycle,ed,fr,
                                      repl_ex_nst,repl_ex_nex,repl_ex_seed,
                                      membed,
                                      cpt_period,max_hours,
                                      deviceOptions,
                                      Flags,
                                      &runtime);

        if (inputrec->ePull != epullNO)
        {
            finish_pull(fplog,inputrec->pull);
        }
        
        if (inputrec->bRot)
        {
            finish_rot(fplog,inputrec->rot);
        }

    } 
    else 
    {
        /* do PME only */
        gmx_pmeonly(*pmedata,cr,nrnb,wcycle,ewaldcoeff,FALSE,inputrec);
    }

    if (EI_DYNAMICS(inputrec->eI) || EI_TPI(inputrec->eI))
    {
        /* Some timing stats */  
        if (SIMMASTER(cr))
        {
            if (runtime.proc == 0)
            {
                runtime.proc = runtime.real;
            }
        }
        else
        {
            runtime.real = 0;
        }
    }

    wallcycle_stop(wcycle,ewcRUN);

    /* Finish up, write some stuff
     * if rerunMD, don't write last frame again 
     */
    finish_run(fplog,cr,ftp2fn(efSTO,nfile,fnm),
               inputrec,nrnb,wcycle,&runtime,
               fr != NULL && fr->nbv != NULL && fr->nbv->bUseGPU ?
                 nbnxn_cuda_get_timings(fr->nbv->cu_nbv) : NULL,
               nthreads_pp, 
               EI_DYNAMICS(inputrec->eI) && !MULTISIM(cr));

    if ((cr->duty & DUTY_PP) && fr->nbv != NULL && fr->nbv->bUseGPU)
    {
        char gpu_err_str[STRLEN];

        /* free GPU memory and uninitialize GPU */
        nbnxn_cuda_free(fplog, fr->nbv->cu_nbv);

        if (!free_gpu(gpu_err_str))
        {
            gmx_warning("On node %d failed to free GPU #%d: %s",
                        cr->nodeid, get_current_gpu_device_id(), gpu_err_str);
        }
    }

    if (opt2bSet("-membed",nfile,fnm))
    {
        sfree(membed);
    }

#ifdef GMX_THREAD_MPI
    if (PAR(cr) && SIMMASTER(cr))
#endif
    {
        gmx_hardware_info_free(hwinfo);
    }

    /* Does what it says */  
    print_date_and_time(fplog,cr->nodeid,"Finished mdrun",&runtime);

    /* Close logfile already here if we were appending to it */
    if (MASTER(cr) && (Flags & MD_APPENDFILES))
    {
        gmx_log_close(fplog);
    }	

    rc=(int)gmx_get_stop_condition();

#ifdef GMX_THREAD_MPI
    /* we need to join all threads. The sub-threads join when they
       exit this function, but the master thread needs to be told to 
       wait for that. */
    if (PAR(cr) && MASTER(cr))
    {
        tMPI_Finalize();
    }
#endif

    return rc;
}
