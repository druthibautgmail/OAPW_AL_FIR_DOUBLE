#ifndef FIREQ_H
#define FIREQ_H

#include <vector>
#include <complex>
#include <cmath>
#include <fftw3.h>

class FirEq {
public:
    FirEq(int sampleRate, int fftSize);
    ~FirEq();

    void addPeakingBand(float centerFreq, float gainDb, float q);
    void reset();

    const fftw_complex* getSpectrum() const { return eqSpectrum; }

private:
    void updateFilter();

    int sr;
    int nFft;
    int M;
    int numBins;
    
    std::vector<float> eqCurveMag;
    
    double* ir_buffer;
    fftw_complex* eqSpectrum;
    
    fftw_plan plan_inverse;
    fftw_plan plan_forward;
};

#endif
