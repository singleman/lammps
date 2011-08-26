/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author: Aidan Thompson (SNL)
                        improved CG and backtrack ls, added quadratic ls
   Sources: Numerical Recipes frprmn routine
            "Conjugate Gradient Method Without the Agonizing Pain" by
            JR Shewchuk, http://www-2.cs.cmu.edu/~jrs/jrspapers.html#cg
------------------------------------------------------------------------- */

#include "lmptype.h"
#include "math.h"
#include "stdlib.h"
#include "string.h"
#include "min.h"
#include "atom.h"
#include "domain.h"
#include "comm.h"
#include "update.h"
#include "modify.h"
#include "fix_minimize.h"
#include "compute.h"
#include "neighbor.h"
#include "force.h"
#include "pair.h"
#include "bond.h"
#include "angle.h"
#include "dihedral.h"
#include "improper.h"
#include "kspace.h"
#include "output.h"
#include "thermo.h"
#include "timer.h"
#include "memory.h"
#include "error.h"

using namespace LAMMPS_NS;

#define MIN(A,B) ((A) < (B)) ? (A) : (B)
#define MAX(A,B) ((A) > (B)) ? (A) : (B)

/* ---------------------------------------------------------------------- */

Min::Min(LAMMPS *lmp) : Pointers(lmp)
{
  dmax = 0.1;
  searchflag = 0;
  linestyle = 0;

  elist_global = elist_atom = NULL;
  vlist_global = vlist_atom = NULL;

  nextra_global = 0;
  fextra = NULL;

  nextra_atom = 0;
  xextra_atom = fextra_atom = NULL;
  extra_peratom = extra_nlen = NULL;
  extra_max = NULL;
  requestor = NULL;
}

/* ---------------------------------------------------------------------- */

Min::~Min()
{
  delete [] elist_global;
  delete [] elist_atom;
  delete [] vlist_global;
  delete [] vlist_atom;

  delete [] fextra;

  memory->sfree(xextra_atom);
  memory->sfree(fextra_atom);
  memory->destroy(extra_peratom);
  memory->destroy(extra_nlen);
  memory->destroy(extra_max);
  memory->sfree(requestor);
}

/* ---------------------------------------------------------------------- */

void Min::init()
{
  // create fix needed for storing atom-based quantities
  // will delete it at end of run

  char **fixarg = new char*[3];
  fixarg[0] = (char *) "MINIMIZE";
  fixarg[1] = (char *) "all";
  fixarg[2] = (char *) "MINIMIZE";
  modify->add_fix(3,fixarg);
  delete [] fixarg;
  fix_minimize = (FixMinimize *) modify->fix[modify->nfix-1];

  // clear out extra global and per-atom dof
  // will receive requests for new per-atom dof during pair init()
  // can then add vectors to fix_minimize in setup()

  nextra_global = 0;
  delete [] fextra;
  fextra = NULL;

  nextra_atom = 0;
  memory->sfree(xextra_atom);
  memory->sfree(fextra_atom);
  memory->destroy(extra_peratom);
  memory->destroy(extra_nlen);
  memory->destroy(extra_max);
  memory->sfree(requestor);
  xextra_atom = fextra_atom = NULL;
  extra_peratom = extra_nlen = NULL;
  extra_max = NULL;
  requestor = NULL;

  // virial_style:
  // 1 if computed explicitly by pair->compute via sum over pair interactions
  // 2 if computed implicitly by pair->virial_compute via sum over ghost atoms

  if (force->newton_pair) virial_style = 2;
  else virial_style = 1;

  // setup lists of computes for global and per-atom PE and pressure

  ev_setup();

  // set flags for what arrays to clear in force_clear()
  // need to clear torques,erforce if arrays exists

  torqueflag = 0;
  if (atom->torque_flag) torqueflag = 1;
  erforceflag = 0;
  if (atom->erforce_flag) erforceflag = 1;

  // orthogonal vs triclinic simulation box

  triclinic = domain->triclinic;

  // reset reneighboring criteria if necessary

  neigh_every = neighbor->every;
  neigh_delay = neighbor->delay;
  neigh_dist_check = neighbor->dist_check;
  
  if (neigh_every != 1 || neigh_delay != 0 || neigh_dist_check != 1) {
    if (comm->me == 0) 
      error->warning("Resetting reneighboring criteria during minimization");
  }

  neighbor->every = 1;
  neighbor->delay = 0;
  neighbor->dist_check = 1;

  niter = neval = 0;

  // style-specific initialization

  init_style();
}

/* ----------------------------------------------------------------------
   setup before run
------------------------------------------------------------------------- */

void Min::setup()
{
  if (comm->me == 0 && screen) fprintf(screen,"Setting up minimization ...\n");

  // setup extra global dof due to fixes
  // cannot be done in init() b/c update init() is before modify init()

  nextra_global = modify->min_dof();
  if (nextra_global) fextra = new double[nextra_global];

  // compute for potential energy

  int id = modify->find_compute("thermo_pe");
  if (id < 0) error->all("Minimization could not find thermo_pe compute");
  pe_compute = modify->compute[id];

  // style-specific setup does two tasks
  // setup extra global dof vectors
  // setup extra per-atom dof vectors due to requests from Pair classes
  // cannot be done in init() b/c update init() is before modify/pair init()

  setup_style();

  // ndoftotal = total dof for entire minimization problem
  // dof for atoms, extra per-atom, extra global

  bigint ndofme = 3*atom->nlocal;
  for (int m = 0; m < nextra_atom; m++)
    ndofme += extra_peratom[m]*atom->nlocal;
  MPI_Allreduce(&ndofme,&ndoftotal,1,MPI_LMP_BIGINT,MPI_SUM,world);
  ndoftotal += nextra_global;

  // setup domain, communication and neighboring
  // acquire ghosts
  // build neighbor lists

  atom->setup();
  if (triclinic) domain->x2lamda(atom->nlocal);
  domain->pbc();
  domain->reset_box();
  comm->setup();
  if (neighbor->style) neighbor->setup_bins();
  comm->exchange();
  if (atom->sortfreq > 0) atom->sort();
  comm->borders();
  if (triclinic) domain->lamda2x(atom->nlocal+atom->nghost);
  neighbor->build();
  neighbor->ncalls = 0;

  // remove these restriction eventually

  if (nextra_global && searchflag == 0)
    error->all("Cannot use a damped dynamics min style with fix box/relax");
  if (nextra_atom && searchflag == 0)
    error->all("Cannot use a damped dynamics min style with per-atom DOF");

  // atoms may have migrated in comm->exchange()

  reset_vectors();

  // compute all forces

  ev_set(update->ntimestep);
  force_clear();
  modify->setup_pre_force(vflag);

  if (force->pair) force->pair->compute(eflag,vflag);

  if (atom->molecular) {
    if (force->bond) force->bond->compute(eflag,vflag);
    if (force->angle) force->angle->compute(eflag,vflag);
    if (force->dihedral) force->dihedral->compute(eflag,vflag);
    if (force->improper) force->improper->compute(eflag,vflag);
  }

  if (force->kspace) {
    force->kspace->setup();
    force->kspace->compute(eflag,vflag);
  }

  if (force->newton) comm->reverse_comm();

  // update per-atom minimization variables stored by pair styles

  if (nextra_atom)
    for (int m = 0; m < nextra_atom; m++)
      requestor[m]->min_xf_get(m);

  modify->setup(vflag);
  output->setup(1);

  // stats for Finish to print

  ecurrent = pe_compute->compute_scalar();
  if (nextra_global) ecurrent += modify->min_energy(fextra);
  if (output->thermo->normflag) ecurrent /= atom->natoms;
	
  einitial = ecurrent;
  fnorm2_init = sqrt(fnorm_sqr());
  fnorminf_init = fnorm_inf();
}

/* ----------------------------------------------------------------------
   setup without output or one-time post-init setup
   flag = 0 = just force calculation
   flag = 1 = reneighbor and force calculation
------------------------------------------------------------------------- */

void Min::setup_minimal(int flag)
{
  // setup domain, communication and neighboring
  // acquire ghosts
  // build neighbor lists

  if (flag) {
    if (triclinic) domain->x2lamda(atom->nlocal);
    domain->pbc();
    domain->reset_box();
    comm->setup();
    if (neighbor->style) neighbor->setup_bins();
    comm->exchange();
    comm->borders();
    if (triclinic) domain->lamda2x(atom->nlocal+atom->nghost);
    neighbor->build();
    neighbor->ncalls = 0;
  }

  // atoms may have migrated in comm->exchange()

  reset_vectors();

  // compute all forces

  ev_set(update->ntimestep);
  force_clear();
  modify->setup_pre_force(vflag);

  if (force->pair) force->pair->compute(eflag,vflag);

  if (atom->molecular) {
    if (force->bond) force->bond->compute(eflag,vflag);
    if (force->angle) force->angle->compute(eflag,vflag);
    if (force->dihedral) force->dihedral->compute(eflag,vflag);
    if (force->improper) force->improper->compute(eflag,vflag);
  }

  if (force->kspace) {
    force->kspace->setup();
    force->kspace->compute(eflag,vflag);
  }

  if (force->newton) comm->reverse_comm();

  // update per-atom minimization variables stored by pair styles

  if (nextra_atom)
    for (int m = 0; m < nextra_atom; m++)
      requestor[m]->min_xf_get(m);

  modify->setup(vflag);

  // stats for Finish to print

  ecurrent = pe_compute->compute_scalar();
  if (nextra_global) ecurrent += modify->min_energy(fextra);
  if (output->thermo->normflag) ecurrent /= atom->natoms;
	
  einitial = ecurrent;
  fnorm2_init = sqrt(fnorm_sqr());
  fnorminf_init = fnorm_inf();
}

/* ----------------------------------------------------------------------
   perform minimization, calling iterate() for N steps
------------------------------------------------------------------------- */

void Min::run(int n)
{
  // minimizer iterations

  stop_condition = iterate(n);
  stopstr = stopstrings(stop_condition);

  // if early exit from iterate loop:
  // set update->nsteps to niter for Finish stats to print
  // set output->next values to this timestep
  // call energy_force() to insure vflag is set when forces computed
  // output->write does final output for thermo, dump, restart files
  // add ntimestep to all computes that store invocation times
  //   since are hardwiring call to thermo/dumps and computes may not be ready

  if (stop_condition) {
    update->nsteps = niter;

    if (update->restrict_output == 0) {
      for (int idump = 0; idump < output->ndump; idump++)
	output->next_dump[idump] = update->ntimestep;
      output->next_dump_any = update->ntimestep;
      if (output->restart_every) output->next_restart = update->ntimestep;
    }
    output->next_thermo = update->ntimestep;

    modify->addstep_compute_all(update->ntimestep);
    ecurrent = energy_force(0);
    output->write(update->ntimestep);
  }
}

/* ---------------------------------------------------------------------- */

void Min::cleanup()
{
  // stats for Finish to print
	
  efinal = ecurrent;
  fnorm2_final = sqrt(fnorm_sqr());
  fnorminf_final = fnorm_inf();

  // reset reneighboring criteria

  neighbor->every = neigh_every;
  neighbor->delay = neigh_delay;
  neighbor->dist_check = neigh_dist_check;

  // delete fix at end of run, so its atom arrays won't persist

  modify->delete_fix("MINIMIZE");
}

/* ----------------------------------------------------------------------
   evaluate potential energy and forces
   may migrate atoms due to reneighboring
   return new energy, which should include nextra_global dof
   return negative gradient stored in atom->f
   return negative gradient for nextra_global dof in fextra
------------------------------------------------------------------------- */

double Min::energy_force(int resetflag)
{
  // check for reneighboring
  // always communicate since minimizer moved atoms

  int nflag = neighbor->decide();

  if (nflag == 0) {
    timer->stamp();
    comm->forward_comm();
    timer->stamp(Timer::COMM);
  } else {
    if (modify->n_min_pre_exchange) {
      timer->stamp();
      modify->min_pre_exchange();
      timer->stamp(Timer::MODIFY);
    }
    if (triclinic) domain->x2lamda(atom->nlocal);
    domain->pbc();
    if (domain->box_change) {
      domain->reset_box();
      comm->setup();
      if (neighbor->style) neighbor->setup_bins();
    }
    timer->stamp();
    comm->exchange();
    if (atom->sortfreq > 0 && 
	update->ntimestep >= atom->nextsort) atom->sort();
    comm->borders();
    if (triclinic) domain->lamda2x(atom->nlocal+atom->nghost);
    timer->stamp(Timer::COMM);
    neighbor->build();
    timer->stamp(Timer::NEIGHBOR);
  }

  ev_set(update->ntimestep);
  force_clear();

  timer->stamp();

  if (modify->n_min_pre_force) {
    modify->min_pre_force(vflag);
    timer->stamp(Timer::MODIFY);
  }

  if (force->pair) {
    force->pair->compute(eflag,vflag);
    timer->stamp(Timer::PAIR);
  }

  if (atom->molecular) {
    if (force->bond) force->bond->compute(eflag,vflag);
    if (force->angle) force->angle->compute(eflag,vflag);
    if (force->dihedral) force->dihedral->compute(eflag,vflag);
    if (force->improper) force->improper->compute(eflag,vflag);
    timer->stamp(Timer::BOND);
  }

  if (force->kspace) {
    force->kspace->compute(eflag,vflag);
    timer->stamp(Timer::KSPACE);
  }

  if (force->newton) {
    comm->reverse_comm();
    timer->stamp(Timer::COMM);
  }

  // update per-atom minimization variables stored by pair styles

  if (nextra_atom)
    for (int m = 0; m < nextra_atom; m++)
      requestor[m]->min_xf_get(m);

  // fixes that affect minimization

  if (modify->n_min_post_force) {
     timer->stamp();
     modify->min_post_force(vflag);
     timer->stamp(Timer::MODIFY);
  }

  // compute potential energy of system
  // normalize if thermo PE does

  double energy = pe_compute->compute_scalar();
  if (nextra_global) energy += modify->min_energy(fextra);
  if (output->thermo->normflag) energy /= atom->natoms;

  // if reneighbored, atoms migrated
  // if resetflag = 1, update x0 of atoms crossing PBC
  // reset vectors used by lo-level minimizer

  if (nflag) {
    if (resetflag) fix_minimize->reset_coords();
    reset_vectors();
  }

  return energy;
}

/* ----------------------------------------------------------------------
   clear force on own & ghost atoms
   setup and clear other arrays as needed
------------------------------------------------------------------------- */

void Min::force_clear()
{
  int i;

  // clear global force array
  // nall includes ghosts only if either newton flag is set

  int nall;
  if (force->newton) nall = atom->nlocal + atom->nghost;
  else nall = atom->nlocal;
  int ntot = nall * comm->nthreads;

  double **f = atom->f;
  for (i = 0; i < ntot; i++) {
    f[i][0] = 0.0;
    f[i][1] = 0.0;
    f[i][2] = 0.0;
  }

  if (torqueflag) {
    double **torque = atom->torque;
    for (i = 0; i < nall; i++) {
      torque[i][0] = 0.0;
      torque[i][1] = 0.0;
      torque[i][2] = 0.0;
    }
  }

  if (erforceflag) {
    double *erforce = atom->erforce;
    for (i = 0; i < nall; i++) erforce[i] = 0.0;
  }
}

/* ----------------------------------------------------------------------
   pair style makes request to add a per-atom variables to minimization
   requestor stores callback to pair class to invoke during min
     to get current variable and forces on it and to update the variable
   return flag that pair can use if it registers multiple variables
------------------------------------------------------------------------- */

int Min::request(Pair *pair, int peratom, double maxvalue)
{
  int n = nextra_atom + 1;
  xextra_atom = (double **) memory->srealloc(xextra_atom,n*sizeof(double *),
					     "min:xextra_atom");
  fextra_atom = (double **) memory->srealloc(fextra_atom,n*sizeof(double *),
					     "min:fextra_atom");
  memory->grow(extra_peratom,n,"min:extra_peratom");
  memory->grow(extra_nlen,n,"min:extra_nlen");
  memory->grow(extra_max,n,"min:extra_max");
  requestor = (Pair **) memory->srealloc(requestor,n*sizeof(Pair *),
					 "min:requestor");

  requestor[nextra_atom] = pair;
  extra_peratom[nextra_atom] = peratom;
  extra_max[nextra_atom] = maxvalue;
  nextra_atom++;
  return nextra_atom-1;
}

/* ---------------------------------------------------------------------- */

void Min::modify_params(int narg, char **arg)
{
  if (narg == 0) error->all("Illegal min_modify command");

  int iarg = 0;
  while (iarg < narg) {
    if (strcmp(arg[iarg],"dmax") == 0) {
      if (iarg+2 > narg) error->all("Illegal min_modify command");
      dmax = atof(arg[iarg+1]);
      iarg += 2;
    } else if (strcmp(arg[iarg],"line") == 0) {
      if (iarg+2 > narg) error->all("Illegal min_modify command");
      if (strcmp(arg[iarg+1],"backtrack") == 0) linestyle = 0;
      else if (strcmp(arg[iarg+1],"quadratic") == 0) linestyle = 1;
      else error->all("Illegal min_modify command");
      iarg += 2;
    } else error->all("Illegal min_modify command");
  }
}

/* ----------------------------------------------------------------------
   setup lists of computes for global and per-atom PE and pressure
------------------------------------------------------------------------- */

void Min::ev_setup()
{
  delete [] elist_global;
  delete [] elist_atom;
  delete [] vlist_global;
  delete [] vlist_atom;
  elist_global = elist_atom = NULL;
  vlist_global = vlist_atom = NULL;

  nelist_global = nelist_atom = 0;
  nvlist_global = nvlist_atom = 0;
  for (int i = 0; i < modify->ncompute; i++) {
    if (modify->compute[i]->peflag) nelist_global++;
    if (modify->compute[i]->peatomflag) nelist_atom++;
    if (modify->compute[i]->pressflag) nvlist_global++;
    if (modify->compute[i]->pressatomflag) nvlist_atom++;
  }

  if (nelist_global) elist_global = new Compute*[nelist_global];
  if (nelist_atom) elist_atom = new Compute*[nelist_atom];
  if (nvlist_global) vlist_global = new Compute*[nvlist_global];
  if (nvlist_atom) vlist_atom = new Compute*[nvlist_atom];

  nelist_global = nelist_atom = 0;
  nvlist_global = nvlist_atom = 0;
  for (int i = 0; i < modify->ncompute; i++) {
    if (modify->compute[i]->peflag)
      elist_global[nelist_global++] = modify->compute[i];
    if (modify->compute[i]->peatomflag)
      elist_atom[nelist_atom++] = modify->compute[i];
    if (modify->compute[i]->pressflag)
      vlist_global[nvlist_global++] = modify->compute[i];
    if (modify->compute[i]->pressatomflag)
      vlist_atom[nvlist_atom++] = modify->compute[i];
  }
}

/* ----------------------------------------------------------------------
   set eflag,vflag for current iteration
   invoke matchstep() on all timestep-dependent computes to clear their arrays
   eflag/vflag based on computes that need info on this ntimestep
   always set eflag_global = 1, since need energy every iteration
   eflag = 0 = no energy computation
   eflag = 1 = global energy only
   eflag = 2 = per-atom energy only
   eflag = 3 = both global and per-atom energy
   vflag = 0 = no virial computation (pressure)
   vflag = 1 = global virial with pair portion via sum of pairwise interactions
   vflag = 2 = global virial with pair portion via F dot r including ghosts
   vflag = 4 = per-atom virial only
   vflag = 5 or 6 = both global and per-atom virial
------------------------------------------------------------------------- */

void Min::ev_set(bigint ntimestep)
{
  int i,flag;

  int eflag_global = 1;
  for (i = 0; i < nelist_global; i++)
    elist_global[i]->matchstep(ntimestep);

  flag = 0;
  int eflag_atom = 0;
  for (i = 0; i < nelist_atom; i++)
    if (elist_atom[i]->matchstep(ntimestep)) flag = 1;
  if (flag) eflag_atom = 2;

  if (eflag_global) update->eflag_global = update->ntimestep;
  if (eflag_atom) update->eflag_atom = update->ntimestep;
  eflag = eflag_global + eflag_atom;

  flag = 0;
  int vflag_global = 0;
  for (i = 0; i < nvlist_global; i++)
    if (vlist_global[i]->matchstep(ntimestep)) flag = 1;
  if (flag) vflag_global = virial_style;

  flag = 0;
  int vflag_atom = 0;
  for (i = 0; i < nvlist_atom; i++)
    if (vlist_atom[i]->matchstep(ntimestep)) flag = 1;
  if (flag) vflag_atom = 4;

  if (vflag_global) update->vflag_global = update->ntimestep;
  if (vflag_atom) update->vflag_atom = update->ntimestep;
  vflag = vflag_global + vflag_atom;
}

/* ----------------------------------------------------------------------
   compute and return ||force||_2^2
------------------------------------------------------------------------- */

double Min::fnorm_sqr()
{
  int i,n;
  double *fatom;

  double local_norm2_sqr = 0.0;
  for (i = 0; i < nvec; i++) local_norm2_sqr += fvec[i]*fvec[i];
  if (nextra_atom) {
    for (int m = 0; m < nextra_atom; m++) {
      fatom = fextra_atom[m];
      n = extra_nlen[m];
      for (i = 0; i < n; i++)
	local_norm2_sqr += fatom[i]*fatom[i];
    }
  }

  double norm2_sqr = 0.0;
  MPI_Allreduce(&local_norm2_sqr,&norm2_sqr,1,MPI_DOUBLE,MPI_SUM,world);

  if (nextra_global)
    for (i = 0; i < nextra_global; i++) 
      norm2_sqr += fextra[i]*fextra[i];
  
  return norm2_sqr;
}

/* ----------------------------------------------------------------------
   compute and return ||force||_inf
------------------------------------------------------------------------- */

double Min::fnorm_inf()
{
  int i,n;
  double *fatom;

  double local_norm_inf = 0.0;
  for (i = 0; i < nvec; i++)
    local_norm_inf = MAX(fabs(fvec[i]),local_norm_inf);
  if (nextra_atom) {
    for (int m = 0; m < nextra_atom; m++) {
      fatom = fextra_atom[m];
      n = extra_nlen[m];
      for (i = 0; i < n; i++)
	local_norm_inf = MAX(fabs(fatom[i]),local_norm_inf);
    }
  }

  double norm_inf = 0.0;
  MPI_Allreduce(&local_norm_inf,&norm_inf,1,MPI_DOUBLE,MPI_MAX,world);

  if (nextra_global)
    for (i = 0; i < nextra_global; i++) 
      norm_inf = MAX(fabs(fextra[i]),norm_inf);

  return norm_inf;
}

/* ----------------------------------------------------------------------
   possible stop conditions
------------------------------------------------------------------------- */

char *Min::stopstrings(int n)
{
  char *strings[] = {"max iterations",
		     "max force evaluations",
		     "energy tolerance",
		     "force tolerance",
		     "search direction is not downhill",
		     "linesearch alpha is zero",
		     "forces are zero",
		     "quadratic factors are zero",
		     "trust region too small",
		     "HFTN minimizer error"};
  return strings[n];
}
