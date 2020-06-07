// include necessary libraries and astrometry code (solver.c, engine.c)
#include <stdlib.h>
#include <math.h>
#include <astrometry/os-features.h>
#include <astrometry/engine.h>
#include <astrometry/solver.h>
#include <astrometry/index.h>
#include <astrometry/starxy.h>
#include <astrometry/matchobj.h>
#include <astrometry/healpix.h>
#include <astrometry/bl.h>
#include <astrometry/log.h>
#include <astrometry/errors.h>
#include <astrometry/fileutils.h>
#include <ueye.h>
#include <sofa/sofa.h>
#include "/home/xscblast/astrometry/blind/solver.c"
#include "/home/xscblast/astrometry/blind/engine.c"

// include our header files
#include "camera.h"
#include "astrometry.h"
#include "lens_adapter.h"

#define _USE_MATH_DEFINES
/* Longitude and latitude constants (deg) */
#define backyard_lat   40.79166879
#define backyard_long -73.68133399
#define backyard_hm    59.24

engine_t * engine = NULL;
solver_t * solver = NULL;
FILE * fptr;

/* Astrometry parameters global structure, accessible from commands.c as well */
struct astrometry all_astro_params = {
	.rawtime = 0,
	.logodds = 1e8,
	.latitude = backyard_lat,
	.longitude = backyard_long,
	.hm = backyard_hm,
	.ra = 0, 
	.dec = 0,
	.fr = 0,
	.ps = 0,
	.ir = 0,
	.alt = 0,
	.az = 0,
};

/* Function to initialize astrometry.
** Input: None.
** Output: Flag indicating successful initialization (or not) of Astrometry system
*/
int initAstrometry() {
	engine = engine_new();
	solver = solver_new();
	if (engine_parse_config_file(engine, "/usr/local/astrometry/etc/astrometry.cfg")) {
		printf("Bad configuration file in Astrometry constructor.\n");
		return -1;
	}
	return 1;
}

/* Function to close astrometry.
** Input: None.
** Output: None (void).
*/
void closeAstrometry() {	
	engine_free(engine);
	solver_free(solver);
}

/* Function for solving for pointing location on the sky.
** Input: pointers to the x coordinates of the stars (star_x), y coordinates of the stars (star_y), 
** magnitudes of the stars (star_mags), the number of blobs, the timing structure, and the observing file 
** name.
** Output: the status of finding a solution or not (sol_status).
*/
int lostInSpace(double * star_x, double * star_y, double * star_mags, unsigned num_blobs, struct tm * tm_info, char * datafile) {
	// timers for astrometry
	struct timespec astrom_tp_beginning, astrom_tp_end; 
	// telemetry variables
	double ra, dec, fr, ps, ir;
	// integer value for returning based on achieving a solution or not 
	int sol_status;
	// for apportioning Julian dates
	double d1, d2;
	// for printing the time for checking
	char time_display[100];
	// for AltAz SOFA function ('ob' means observed)
	double aob, zob, hob, dob, rob, eo;

	// set up solver configuration
	solver->funits_lower = MIN_PS;
	solver->funits_upper = MAX_PS;
	
	// set max number of sources
	solver->endobj = num_blobs;

	// disallow tiny quads
	solver->quadsize_min = 0.1*MIN(CAMERA_WIDTH - 2*CAMERA_MARGIN, CAMERA_HEIGHT - 2*CAMERA_MARGIN);

	// set parity which can speed up x2
	solver->parity = PARITY_BOTH;           // PARITY_NORMAL or PARITY_FLIP if that is the correct one
	                                        // only PARITY_BOTH seems to work maybe?
	
	// sets the odds ratio we will accept (logodds parameter)
	solver_set_keep_logodds(solver, log(all_astro_params.logodds));  

	solver->logratio_totune = log(1e6);
	solver->logratio_toprint = log(1e6);
	solver->distance_from_quad_bonus = 1;

	// figure out the index file range to search in
	double hprange = arcsec2dist(MAX_PS*hypot(CAMERA_WIDTH - 2*CAMERA_MARGIN, CAMERA_HEIGHT - 2*CAMERA_MARGIN)/2.0);

	// make list of stars
	starxy_t * field = starxy_new(num_blobs, 1, 0);

	// start timer for astrometry 
	if (clock_gettime(CLOCK_REALTIME, &astrom_tp_beginning) == -1) {
        printf("Error occurred when calling clock_gettime(), error # %d\n", errno);
    }

	starxy_set_x_array(field, star_x);
	starxy_set_y_array(field, star_y);
	starxy_set_flux_array(field, star_mags);
	starxy_sort_by_flux(field);

	solver_set_field(solver, field);
	solver_set_field_bounds(solver, 0, CAMERA_WIDTH - 2*CAMERA_MARGIN, 0, CAMERA_HEIGHT - 2*CAMERA_MARGIN);

	// add index files that are close to the guess for the target
	for (int i = 0; i < (int) pl_size((*engine).indexes); i++) {
		index_t * index = (index_t *) pl_get((*engine).indexes, i);

		if (ra >= -180 && dec >= -90 && index_is_within_range(index, ra, dec, dist2deg(hprange))) {
			solver_add_index(solver, index);
		} else if (ra < -180 || dec < -90) {
			solver_add_index(solver, index);
		}

		index_reload(index);
	}

	solver_log_params(solver);
	solver_run(solver);

	// solution status should be 0 since we have yet to achieve a solution (below) 
	sol_status = 0;
	if ((*solver).best_match_solves) {
		double pscale;
		tan_t * wcs;

		// get World Coordinate System data (wcs)
		wcs = &((*solver).best_match.wcstan);
		tan_pixelxy2radec(wcs, (CAMERA_WIDTH - 2*CAMERA_MARGIN - 1)/2.0, (CAMERA_HEIGHT - 2*CAMERA_MARGIN - 1)/2.0, &ra, &dec);
		
		// calculate pixel scale and field rotation
		ps = tan_pixel_scale(wcs);
		fr = tan_get_orientation(wcs); 

		// calculate Julian date
		strftime(time_display, sizeof(time_display), "%b %d %H:%M:%S", tm_info); 
		printf("Time going into iauDtf2d in lostInSpace(): %s\n", time_display);
		if (iauDtf2d("UTC", tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday, tm_info->tm_hour, 
	                        tm_info->tm_min, (double) tm_info->tm_sec, &d1, &d2) != 0) {
			printf("Julian date not properly calculated.\n");
		}

		// calculate AltAz
		if (iauAtco13(ra*(M_PI/180.0), dec*(M_PI/180.0), 
		        	  0.0, 0.0, 0.0, 0.0, 
				      d1, d2 + (all_camera_params.exposure_time/(2000.0*3600.0*24.0)), dut1, 
				      all_astro_params.longitude*(M_PI/180.0), all_astro_params.latitude*(M_PI/180.0), 
					  all_astro_params.hm, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
				      &aob, &zob, &hob, &dob, &rob, &eo) != 0) {
			printf("Review preceding Julian date calculation; dubious year or unacceptable date passed to AltAz calculation.\n");
		}

		// calculate parallactic angle and add it to field rotation to get image rotation
		ir = (iauHd2pa(hob, dob, all_astro_params.latitude*(M_PI/180.0)))*(180.0/M_PI) + fr;

		// end timer
		if (clock_gettime(CLOCK_REALTIME, &astrom_tp_end) == -1) {
        	printf("Error occurred when calling clock_gettime(), error # %d\n", errno);
    	}

		// update astro struct with telemetry
		all_astro_params.ir = ir;
		all_astro_params.ra = rob*(180.0/M_PI);
		all_astro_params.dec = dob*(180.0/M_PI);
		all_astro_params.alt = 90.0 - (zob*(180.0/M_PI));
		all_astro_params.az = aob*(180.0/M_PI); 
		all_astro_params.fr = fr;
		all_astro_params.ps = ps;

		printf("\n****************************************** TELEMETRY ******************************************\n");
		printf("Num blobs: %i | Obs. RA %lf | Obs. DEC %lf | FR %f | PS %lf | ALT %.15f | AZ %.15f | IR %lf\n", num_blobs,
		        all_astro_params.ra, all_astro_params.dec, all_astro_params.fr, all_astro_params.ps, all_astro_params.alt, 
				all_astro_params.az, all_astro_params.ir);
		printf("***********************************************************************************************\n\n");

		// calculate how long solution took to solve in terms of nanoseconds
		double start = (double) (astrom_tp_beginning.tv_sec*1e9) + (double) astrom_tp_beginning.tv_nsec;
		double end = (double) (astrom_tp_end.tv_sec*1e9) + (double) astrom_tp_end.tv_nsec;
    	double astrom_time = end - start;
		printf("Astrometry solved in %f msec.\n", astrom_time*1e-6);

		// write astrometry solution to data.txt file
		printf("Writing Astrometry solution to data file...\n");
		fptr = fopen(datafile, "a");
		if (fprintf(fptr, "%i|%lf|%lf|%lf|%lf|%.15f|%.15f|%lf|%f", num_blobs, all_astro_params.ra, all_astro_params.dec, 
					all_astro_params.fr, all_astro_params.ps, all_astro_params.alt, all_astro_params.az, 
					all_astro_params.ir, astrom_time*1e-6) < 0) {
			printf("Error occurred writing Astrometry solution to observing file, error # %d\n", errno);
		}
		fclose(fptr);

		// we achieved a solution!
		sol_status = 1;
	} 
	// clean everything up and return the status
	solver_cleanup_field(solver);
	solver_clear_indexes(solver);
	return sol_status;
}