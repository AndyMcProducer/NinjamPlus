/*  A program to time complex forward and inverse fast fourier transform routines	*/
void four1(float data[], unsigned long nn, int isign);

#include <stdio.h>
#include <stdlib.h>
/* #include <fp.h> */
#include <math.h>
#include "fftlib.h"
#include "fftext.h"

#ifdef HAVE_SYS_TIMES
#  include <sys/times.h>
#  include <unistd.h>
#elif macintosh
#include <timer.h>
#else
#include <time.h>
#endif

#define NSIZES 3		/* the number of different ffts sizes to time */

typedef  struct{
	float Re;
	float Im;
	} Complex;

#if defined(HAVE_SYS_TIMES)
  static inline double uclock_sec(void) {
    static double ttclk = 0.;
    if (ttclk == 0.) ttclk = sysconf(_SC_CLK_TCK);
    struct tms t; return ((double)times(&t)) / ttclk;
  }
#elif macintosh
  inline double uclock_sec(void) {
    UnsignedWide TheTime;
    double t;
    Microseconds(&TheTime);
    t = TheTime.hi* 65536.0 * 65536.0;
    t += (double)(TheTime.lo));
    return t * 1e-6;
  }
#else
  double uclock_sec(void)
{ return (double)clock()/(double)CLOCKS_PER_SEC; }
#endif


void main(){
long 	fftSize[NSIZES] = {1024, 16384, 262144};	/* size of FFTs, must be powers of 2 */
long 	fftRepeats[NSIZES] = {40000, 1500, 60};		/* number of timing loops */
Complex	*a;
long 	isize;
long 	i1;
long 	TheErr;
long	N;
long	M;
double t0, t1, tdt, tds, tsum = 0.0;

printf(" %zu  Byte Floats \n", sizeof(a[0].Re));
for (isize = 0; isize < NSIZES; isize++){

	N = fftSize[isize];
	printf("ffts size = %7ld,  ", N);
	M = lround(LOG2(N));

	TheErr = fftInit(M);

	if(!TheErr){
		a = (Complex *) malloc(N*sizeof(Complex) );
		if (a == 0) TheErr = 2;
	}

	if(!TheErr){

			/*  set up a simple test case */
		for (i1=0; i1<N; i1++){
			a[i1].Re = sqrt(i1+.77777);
			a[i1].Im = sqrt(i1+.22222);	
		}

	#if macintosh
							/* make sure routines are in physical (not virtual) memory */
		Microseconds(&TheTime1);
		ffts((float *)a, M, 1);
		iffts((float *)a, M, 1);

		Microseconds(&TheTime1);

		for (i1=0;i1<fftRepeats[isize];i1++){		/* do many times for timing */
			ffts((float *)a, M, 1);
			iffts((float *)a, M, 1);
		}	

		Microseconds(&TheTime2);

		printf("ffts time = %12.1f  µs,  a[0].Re=%6e\n", TheTime/fftRepeats[isize]/2, a[0].Re);

	#else
		printf("start timing %12ld real ffts's\n", fftRepeats[isize]*2);
		t0 = uclock_sec();
		for (i1=0;i1<fftRepeats[isize];i1++){		/* do many times for timing */
			ffts((float *)a, M, 1);
			iffts((float *)a, M, 1);
		}
		t1 = uclock_sec();
		tdt = (t1 - t0) * 1e3;
		tds = (t1 - t0) * 1e6 / (2.0 * fftRepeats[isize]);
		tsum += tdt;
		printf("end timing\n");
		printf("duration per fft: %12.1f us\ttotal duration: %12.1f ms\n", tds, tdt);
	#endif
		free(a);
		fftFree();
	}
	else{
		if(TheErr==2)	printf(" out of memory \n");
		else	printf(" error \n");
		fftFree();
	}
}
printf("\n Total sum duration: %12.1f ms\n", tsum);
printf(" Done. \n");
return;
}
