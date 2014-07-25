#include "forwardmodel.h"

/*****************************************************************************/

double brightness(double rad,
		  double nu) {
  
  return C2*nu/gsl_log1p(C1*gsl_pow_3(nu)/rad);
}

/*****************************************************************************/

void formod(ctl_t *ctl,
	    atm_t *atm,
	    obs_t *obs,
	    aero_t *aero) {
  
  static int mask[NDMAX][NRMAX];
  
  int id, ir;
  
  /* Save observation mask... */
  for(id=0; id<ctl->nd; id++)
    for(ir=0; ir<obs->nr; ir++)
      mask[id][ir]=!gsl_finite(obs->rad[id][ir]);
  
  /* Hydrostatic equilibrium... */
  hydrostatic(ctl, atm);
  
  /* Do first ray path sequential (to initialize model)... */
  formod_pencil(ctl, atm, obs, aero, ctl->sca_mult, 0);
  
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic)
#endif
  
  /* Do remaining ray paths in parallel... */
  for(ir=1; ir<obs->nr; ir++)
    formod_pencil(ctl, atm, obs, aero, ctl->sca_mult, ir);
  
  /* Apply field-of-view convolution... */
  formod_fov(ctl, obs);
  
  /* Convert radiance to brightness temperature... */
  if(ctl->write_bbt)
    for(ir=0; ir<obs->nr; ir++)
      for(id=0; id<ctl->nd; id++)
	obs->rad[id][ir]=brightness(obs->rad[id][ir], ctl->nu[id]);
  
  /* Apply observation mask... */
  for(id=0; id<ctl->nd; id++)
    for(ir=0; ir<obs->nr; ir++)
      if(mask[id][ir])
	obs->rad[id][ir]=GSL_NAN;
}

/*****************************************************************************/

void formod_continua(ctl_t *ctl,
		     los_t *los,
		     int ip,
		     double *beta) {
  
  int id;
  
  /* Add extinction... */
  for(id=0; id<ctl->nd; id++)
    beta[id]=los->k[ip][ctl->window[id]];
  
  /* Add CO2 continuum... */
  if(ctl->ctm_co2)
    for(id=0; id<ctl->nd; id++)
      beta[id]+=ctmco2(ctl, ctl->nu[id], los->p[ip],
		       los->t[ip], los->u[ip])/los->ds[ip];
  
  /* Add H2O continuum... */
  if(ctl->ctm_h2o)
    for(id=0; id<ctl->nd; id++)
      beta[id]+=ctmh2o(ctl, ctl->nu[id], los->p[ip], los->t[ip],
		       los->q[ip], los->u[ip])/los->ds[ip];
  
  /* Add N2 continuum... */
  if(ctl->ctm_n2)
    for(id=0; id<ctl->nd; id++)
      beta[id]+=ctmn2(ctl->nu[id], los->p[ip], los->t[ip]);

  /* Add O2 continuum... */
  if(ctl->ctm_o2)
    for(id=0; id<ctl->nd; id++)
      beta[id]+=ctmo2(ctl->nu[id], los->p[ip], los->t[ip]);
}

/*****************************************************************************/

void formod_fov(ctl_t *ctl,
		obs_t *obs) {
  
  static obs_t obs2;
  
  static double dz[NSHAPE], rad[NDMAX][NRMAX], tau[NDMAX][NRMAX],
    w[NSHAPE], wsum, z[NRMAX], zfov;
  
  static int init=0, i, id, idx, ir, ir2, n, nz;
  
  /* Do not take into account FOV... */
  if(ctl->fov[0]=='-')
    return;
  
  /* Initialize FOV data... */
  if(!init) {
    init=1;
    read_shape(ctl->fov, dz, w, &n);
  }
  
  /* Copy observation data... */
  copy_obs(ctl, &obs2, obs, 0);
  
  /* Loop over ray paths... */
  for(ir=0; ir<obs->nr; ir++) {
    
    /* Get radiance and transmittance profiles... */
    nz=0;
    for(ir2=GSL_MAX(ir-NFOV, 0); ir2<GSL_MIN(ir+1+NFOV, obs->nr); ir2++)
      if(obs->time[ir2]==obs->time[ir]) {
	z[nz]=obs2.vpz[ir2];
	for(id=0; id<ctl->nd; id++) {
	  rad[id][nz]=obs2.rad[id][ir2];
	  tau[id][nz]=obs2.tau[id][ir2];
	}
	nz++;
      }
    if(nz<2)
      ERRMSG("Cannot apply FOV convolution!");
    
    /* Convolute profiles with FOV... */
    wsum=0;
    for(id=0; id<ctl->nd; id++) {
      obs->rad[id][ir]=0;
      obs->tau[id][ir]=0;
    }
    for(i=0; i<n; i++) {
      zfov=obs->vpz[ir]+dz[i];
      idx=locate(z, nz, zfov);
      for(id=0; id<ctl->nd; id++) {
        obs->rad[id][ir]+=w[i]
          *LIN(z[idx], rad[id][idx], z[idx+1], rad[id][idx+1], zfov);
        obs->tau[id][ir]+=w[i]
          *LIN(z[idx], tau[id][idx], z[idx+1], tau[id][idx+1], zfov);
      }
      wsum+=w[i];
    }
    for(id=0; id<ctl->nd; id++) {
      obs->rad[id][ir]/=wsum;
      obs->tau[id][ir]/=wsum;
    }
  }
}

/*****************************************************************************/

void formod_pencil(ctl_t *ctl,
		   atm_t *atm,
		   obs_t *obs,
		   aero_t *aero,
		   int scattering,
		   int ir) {
  
  static int init=0;

  static tbl_t *tbl;
  
  los_t *los;  
  
  double beta_ctm[NDMAX], beta_ext_tot, dx[3], eps, src_planck[NDMAX],
    src_sca[NDMAX], tau_path[NGMAX][NDMAX], tau_gas[NDMAX], x[3], x0[3], x1[3];
  
  int i, id, ip, ip0, ip1;
  
  /* Read tables... */
  if(!init) {
    init=1;
    printf("Allocate memory for tables: %.4g MB\n",
	   (double)sizeof(tbl_t)/1024./1024.);
    ALLOC(tbl, tbl_t, 1);
    read_tbl(ctl, tbl);
  }
  
  /* Allocate... */
  ALLOC(los, los_t, 1);
  
  /* Initialize... */
  for(id=0; id<ctl->nd; id++) {
    obs->rad[id][ir]=0;
    obs->tau[id][ir]=1;
  }
  
  /* Raytracing... */
  raytrace(ctl, atm, obs, aero, los, ir);
  
  /* Loop over LOS points... */
  for(ip=0; ip<los->np; ip++) {
    
    /* Get trace gas transmittance... */
    intpol_tbl(ctl, tbl, los, ip, tau_path, tau_gas);
    
    /* Get continuum absorption... */
    formod_continua(ctl, los, ip, beta_ctm);
    
    /* Compute Planck function... */
    srcfunc_planck(ctl, los->t[ip], src_planck);
  
    /* Compute radiative transfer with scattering source... */
    if(los->aerofac[ip]>0 && scattering>0) {
      
      /* Compute scattering source term... */
      geo2cart(los->z[ip], los->lon[ip], los->lat[ip], x);
      ip0=(ip>0 ? ip-1 : ip);
      ip1=(ip<los->np ? ip+1 : ip);
      geo2cart(los->z[ip0], los->lon[ip0], los->lat[ip0], x0);
      geo2cart(los->z[ip1], los->lon[ip1], los->lat[ip1], x1);
      for(i=0; i<3; i++)
	dx[i]=x1[i]-x0[i];
      srcfunc_sca(ctl,atm,aero,obs->time[ir],x,dx,los->aeroi[ip],src_sca,scattering);
      
      /* Loop over channels... */
      for(id=0; id<ctl->nd; id++)
	if(tau_gas[id]>0) {

	  /* Get gas and aerosol/cloud extinctions... */
	  beta_ext_tot = -log(tau_gas[id])/los->ds[ip]+beta_ctm[id] + 
	    los->aerofac[ip]*aero->beta_e[los->aeroi[ip]][id]; 

	  /* Get segment emissivity (epsilon = 1-t_gas*t_aerosol) ... */
	  eps=1-tau_gas[id]*exp(-(beta_ctm[id]+los->aerofac[ip]*
				  aero->beta_a[los->aeroi[ip]][id])*los->ds[ip]);

	  /* Compute radiance... */
	  obs->rad[id][ir] += obs->tau[id][ir] * 
	    (eps * src_planck[id] + aero->beta_s[los->aeroi[ip]][id]*src_sca[id]);
	  
	  /* Compute path transmittance... */
	  obs->tau[id][ir] *= exp(-beta_ext_tot*los->ds[ip]);
	}
    }
    
    /* Compute radiative transfer without scattering source... */
    else {
      
      /* Loop over channels... */
      for(id=0; id<ctl->nd; id++)
	if(tau_gas[id]>0) {
	  
	  /* Get segment emissivity... */
	  if (ctl->sca_n==0) {
	    eps=1-tau_gas[id]*exp(-1. * beta_ctm[id] * los->ds[ip]);
	  }
	  else if (strcmp(ctl->sca_ext, "beta_a")) {
	    eps=1-tau_gas[id]*exp(-(beta_ctm[id]+los->aerofac[ip]*
				    aero->beta_a[los->aeroi[ip]][id])*los->ds[ip]);
	  } else {
	    eps=1-tau_gas[id]*exp(-(beta_ctm[id]+los->aerofac[ip]*
				    aero->beta_e[los->aeroi[ip]][id])*los->ds[ip]);
	  }
	  
	  /* Compute radiance... */
	  obs->rad[id][ir]+=src_planck[id]*eps*obs->tau[id][ir];
	  
	  /* Compute path transmittance... */
	  if (ctl->sca_n==0) {
	    obs->tau[id][ir]*=(1-eps);
	  } else if (strcmp(ctl->sca_ext, "beta_a")) {
	    obs->tau[id][ir]*=(1-eps)*exp(-los->aerofac[ip]*aero->beta_a[los->aeroi[ip]][id]*los->ds[ip]);
	  } else {
	    obs->tau[id][ir]*=(1-eps)*exp(-los->aerofac[ip]*aero->beta_e[los->aeroi[ip]][id]*los->ds[ip]);
	  } 
	}
    }
  }
  
  /* Add surface... */
  if(los->tsurf>0) {
    srcfunc_planck(ctl, los->tsurf, src_planck);
    for(id=0; id<ctl->nd; id++)
      obs->rad[id][ir]+=src_planck[id]*obs->tau[id][ir];
  }
  
  /* Free... */
  free(los);
}

/*****************************************************************************/

void intpol_tbl(ctl_t *ctl,
		tbl_t *tbl,
		los_t *los,
		int ip,
		double tau_path[NGMAX][NDMAX],
		double tau_seg[NDMAX]) {
  
  double eps, eps00, eps01, eps10, eps11, u;
  
  int id, ig, ipr, it0, it1;
  
  /* Initialize... */
  if(ip<=0)
    for(ig=0; ig<ctl->ng; ig++)
      for(id=0; id<ctl->nd; id++)
	tau_path[ig][id]=1;
  
  /* Loop over channels... */
  for(id=0; id<ctl->nd; id++) {
    
    /* Initialize... */
    tau_seg[id]=1;
    
    /* Loop over emitters.... */
    for(ig=0; ig<ctl->ng; ig++) {
      
      /* Check size of table (pressure)... */
      if(tbl->np[ig][id]<2)
	eps=0;
      
      /* Check transmittance... */
      else if(tau_path[ig][id]<1e-9)
	eps=1;
      
      /* Interpolate... */
      else {
	
	/* Determine pressure and temperature indices... */
	ipr=locate(tbl->p[ig][id], tbl->np[ig][id], los->p[ip]);
	it0=locate(tbl->t[ig][id][ipr], tbl->nt[ig][id][ipr], los->t[ip]);
	it1=locate(tbl->t[ig][id][ipr+1], tbl->nt[ig][id][ipr+1], los->t[ip]);
	
	/* Check size of table (temperature and column density)... */
	if(tbl->nt[ig][id][ipr]<2 || tbl->nt[ig][id][ipr+1]<2
	   || tbl->nu[ig][id][ipr][it0]<2 || tbl->nu[ig][id][ipr][it0+1]<2
	   || tbl->nu[ig][id][ipr+1][it1]<2 || tbl->nu[ig][id][ipr+1][it1+1]<2)
	  eps=0;
	
	else {
	  
	  /* Get emissivities of extended path... */
	  u=intpol_tbl_u(tbl, ig, id, ipr, it0, 1-tau_path[ig][id]);
	  eps00=intpol_tbl_eps(tbl, ig, id, ipr, it0, u+los->u[ip][ig]);
	  eps00=GSL_MAX(GSL_MIN(eps00, 1), 0);
	  
	  u=intpol_tbl_u(tbl, ig, id, ipr, it0+1, 1-tau_path[ig][id]);
	  eps01=intpol_tbl_eps(tbl, ig, id, ipr, it0+1, u+los->u[ip][ig]);
	  eps01=GSL_MAX(GSL_MIN(eps01, 1), 0);
	  
	  u=intpol_tbl_u(tbl, ig, id, ipr+1, it1, 1-tau_path[ig][id]);
	  eps10=intpol_tbl_eps(tbl, ig, id, ipr+1, it1, u+los->u[ip][ig]);
	  eps10=GSL_MAX(GSL_MIN(eps10, 1), 0);
	  
	  u=intpol_tbl_u(tbl, ig, id, ipr+1, it1+1, 1-tau_path[ig][id]);
	  eps11=intpol_tbl_eps(tbl, ig, id, ipr+1, it1+1, u+los->u[ip][ig]);
	  eps11=GSL_MAX(GSL_MIN(eps11, 1), 0);
	  
	  /* Interpolate with respect to temperature... */
	  eps00=LIN(tbl->t[ig][id][ipr][it0], eps00,
		    tbl->t[ig][id][ipr][it0+1], eps01, los->t[ip]);
	  eps00=GSL_MAX(GSL_MIN(eps00, 1), 0);
	  
	  eps11=LIN(tbl->t[ig][id][ipr+1][it1], eps10,
		    tbl->t[ig][id][ipr+1][it1+1], eps11, los->t[ip]);
	  eps11=GSL_MAX(GSL_MIN(eps11, 1), 0);
	  
	  /* Interpolate with respect to pressure... */
	  eps00=LIN(tbl->p[ig][id][ipr], eps00,
		    tbl->p[ig][id][ipr+1], eps11, los->p[ip]);
	  eps00=GSL_MAX(GSL_MIN(eps00, 1), 0);
	  
	  /* Determine segment emissivity... */
	  eps=1-(1-eps00)/tau_path[ig][id];
	}
      }
      
      /* Get transmittance of extended path... */
      tau_path[ig][id]*=(1-eps);
      
      /* Get segment transmittance... */
      tau_seg[id]*=(1-eps);
    }
  }
}

/*****************************************************************************/

double intpol_tbl_eps(tbl_t *tbl,
		      int ig,
		      int id,
		      int ip,
		      int it,
		      double u) {
  
  int idx;
  
  /* Get index... */
  idx=locate_tbl(tbl->u[ig][id][ip][it], tbl->nu[ig][id][ip][it], u);
  
  /* Interpolate... */
  return
    LIN(tbl->u[ig][id][ip][it][idx], tbl->eps[ig][id][ip][it][idx],
	tbl->u[ig][id][ip][it][idx+1], tbl->eps[ig][id][ip][it][idx+1], u);
}

/*****************************************************************************/

double intpol_tbl_u(tbl_t *tbl,
		    int ig,
		    int id,
		    int ip,
		    int it,
		    double eps) {
  
  int idx;
  
  /* Get index... */
  idx=locate_tbl(tbl->eps[ig][id][ip][it], tbl->nu[ig][id][ip][it], eps);
  
  /* Interpolate... */
  return
    LIN(tbl->eps[ig][id][ip][it][idx], tbl->u[ig][id][ip][it][idx],
	tbl->eps[ig][id][ip][it][idx+1], tbl->u[ig][id][ip][it][idx+1], eps);
}

/*****************************************************************************/

int locate_tbl(float *xx,
               int n,
               double x) {
  
  int i, ilo, ihi;
  
  ilo=0;
  ihi=n-1;
  i=(ihi+ilo)>>1;
  
  while(ihi>ilo+1) {
    i=(ihi+ilo)>>1;
    if(xx[i]>x)
      ihi=i;
    else
      ilo=i;
  }
  
  return ilo;
}

/*****************************************************************************/

double planck(double t,
	      double nu) {
  
  return C1*gsl_pow_3(nu)/gsl_expm1(C2*nu/t);
}

/*****************************************************************************/

void read_shape(const char *filename,
		double *x,
		double *y,
		int *n) {
  
  FILE *in;
  
  char line[LEN];
  
  /* Write info... */
  printf("Read shape function: %s\n", filename);
  
  /* Open file... */
  if(!(in=fopen(filename, "r")))
    ERRMSG("Cannot open file!");
  
  /* Read data... */
  *n=0;
  while(fgets(line, LEN, in))
    if(sscanf(line,"%lg %lg", &x[*n], &y[*n])==2)
      if((++(*n))>NSHAPE)
        ERRMSG("Too many data points!");
  
  /* Check number of points... */
  if(*n<1)
    ERRMSG("Could not read any data!");
  
  /* Close file... */
  fclose(in);
}

/*****************************************************************************/

void read_tbl(ctl_t *ctl,
	      tbl_t *tbl) {
  
  FILE *in;
  
  char filename[LEN], line[LEN];
  
  double eps, eps_old, press, press_old, temp, temp_old, u, u_old;
  
  int id, ig, ip, it;
  
  /* Loop over trace gases and channels... */
  for(ig=0; ig<ctl->ng; ig++)
    for(id=0; id<ctl->nd; id++) {
      
      /* Set filename... */
      sprintf(filename, "%s_%.4f_%s.bin",
	      ctl->tblbase, ctl->nu[id], ctl->emitter[ig]);
      
      /* Try to open binary file... */
      if((in=fopen(filename, "r"))) {
	
	/* Write info... */
	printf("Read emissivity table: %s\n", filename);
	
	/* Read data... */
	FREAD(&tbl->np[ig][id], int, 1, in);
	if(tbl->np[ig][id]>TBLNPMAX)
	  ERRMSG("Too many pressure levels!");
	FREAD(&tbl->p[ig][id], double, tbl->np[ig][id], in);
	FREAD(tbl->nt[ig][id], int, tbl->np[ig][id], in);
	for(ip=0; ip<tbl->np[ig][id]; ip++) {
	  if(tbl->nt[ig][id][ip]>TBLNTMAX)
	    ERRMSG("Too many temperatures!");
	  FREAD(&tbl->t[ig][id][ip], double, tbl->nt[ig][id][ip], in);
	  FREAD(tbl->nu[ig][id][ip], int, tbl->nt[ig][id][ip], in);
	  for(it=0; it<tbl->nt[ig][id][ip]; it++) {
	    FREAD(&tbl->u[ig][id][ip][it], float,
		  GSL_MIN(tbl->nu[ig][id][ip][it], TBLNUMAX), in);
	    FREAD(&tbl->eps[ig][id][ip][it], float,
		  GSL_MIN(tbl->nu[ig][id][ip][it], TBLNUMAX), in);
	  }
	}
	
	/* Close file... */
	fclose(in);
      }
      
      /* Try to read ASCII file... */
      else {
	
	/* Initialize... */
	tbl->np[ig][id]=-1;
	eps_old=-999;
	press_old=-999;
	temp_old=-999;
	u_old=-999;
	
	/* Try to open file... */
	sprintf(filename, "%s_%.4f_%s.tab",
		ctl->tblbase, ctl->nu[id], ctl->emitter[ig]);
	if(!(in=fopen(filename, "r"))) {
	  printf("Missing emissivity table: %s\n", filename);
	  continue;
	}
	printf("Read emissivity table: %s\n", filename);
	
	/* Read data... */
	while(fgets(line, LEN, in)) {
	  
	  /* Parse line... */
	  if(sscanf(line,"%lg %lg %lg %lg", &press, &temp, &u, &eps)!=4)
	    continue;
	  
	  /* Determine pressure index... */
	  if(press!=press_old) {
	    press_old=press;
	    if((++tbl->np[ig][id])>=TBLNPMAX)
	      ERRMSG("Too many pressure levels!");
	    tbl->nt[ig][id][tbl->np[ig][id]]=-1;
	  }
	  
	  /* Determine temperature index... */
	  if(temp!=temp_old) {
	    temp_old=temp;
	    if((++tbl->nt[ig][id][tbl->np[ig][id]])>=TBLNTMAX)
	      ERRMSG("Too many temperatures!");
	    tbl->nu[ig][id][tbl->np[ig][id]]
	      [tbl->nt[ig][id][tbl->np[ig][id]]]=-1;
	  }
	  
	  /* Determine column density index... */
	  if((eps>eps_old && u>u_old) || tbl->nu[ig][id][tbl->np[ig][id]]
	     [tbl->nt[ig][id][tbl->np[ig][id]]]<0) {
	    eps_old=eps;
	    u_old=u;
	    if((++tbl->nu[ig][id][tbl->np[ig][id]]
		[tbl->nt[ig][id][tbl->np[ig][id]]])>=TBLNUMAX) {
	      tbl->nu[ig][id][tbl->np[ig][id]]
		[tbl->nt[ig][id][tbl->np[ig][id]]]--;
	      continue;
	    }
	  }
	  
	  /* Store data... */
	  tbl->p[ig][id][tbl->np[ig][id]]=press;
	  tbl->t[ig][id][tbl->np[ig][id]][tbl->nt[ig][id][tbl->np[ig][id]]]
	    =temp;
	  tbl->u[ig][id][tbl->np[ig][id]][tbl->nt[ig][id][tbl->np[ig][id]]]
	    [tbl->nu[ig][id][tbl->np[ig][id]]
	     [tbl->nt[ig][id][tbl->np[ig][id]]]]=(float)u;
	  tbl->eps[ig][id][tbl->np[ig][id]][tbl->nt[ig][id][tbl->np[ig][id]]]
	    [tbl->nu[ig][id][tbl->np[ig][id]]
	     [tbl->nt[ig][id][tbl->np[ig][id]]]]=(float)eps;
	}
	
	/* Increment counters... */
	tbl->np[ig][id]++;
	for(ip=0; ip<tbl->np[ig][id]; ip++) {
	  tbl->nt[ig][id][ip]++;
	  for(it=0; it<tbl->nt[ig][id][ip]; it++)
	    tbl->nu[ig][id][ip][it]++;
	}
	
	/* Close file... */
	fclose(in);
      }
    }
}

/*****************************************************************************/

void srcfunc_planck(ctl_t *ctl,
		    double t,
		    double *src) {
  
  static double f[NSHAPE], fsum, nu[NSHAPE], plancka[NDMAX][1201],
    tmin=100, tmax=400, temp[1201];
  
  static int i, init=0, n, nplanck=1201;
  
  char filename[LEN];
  
  int id, it;
  
  /* Initialize source function table... */
  if(!init) {
    init=1;
    
    /* Write info... */
    printf("Initialize source function table...\n");
    
    /* Loop over channels... */
    for(id=0; id<ctl->nd; id++) {
      
      /* Read filter function... */
      sprintf(filename, "%s_%.4f.filt", ctl->tblbase, ctl->nu[id]);
      read_shape(filename, nu, f, &n);
      
      /* Compute source function table... */
      for(it=0; it<nplanck; it++) {
	
	/* Set temperature... */
	temp[it]=LIN(0.0, tmin, nplanck-1.0, tmax, (double)it);
	
	/* Integrate Planck function... */
	fsum=0;
	plancka[id][it]=0;
	for(i=0; i<n; i++) {
	  fsum+=f[i];
	  plancka[id][it]+=f[i]*planck(temp[it], nu[i]);
	}
	plancka[id][it]/=fsum;
      }
    }
  }
  
  /* Determine index in temperature array... */
  it=locate(temp, nplanck, t);
  
  /* Interpolate Planck function value... */
  for(id=0; id<ctl->nd; id++)
    src[id]=LIN(temp[it], plancka[id][it], temp[it+1], plancka[id][it+1], t);
}
