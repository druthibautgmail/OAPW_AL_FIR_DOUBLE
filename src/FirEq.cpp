#include "FirEq.h"
#include <algorithm>
#include <cstring>

static double blackmanHarrisEq(int n, int filterLength) {
    double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
    double arg = (2.0 * M_PI * n) / (filterLength - 1);
    return a0 - a1 * std::cos(arg) + a2 * std::cos(2.0 * arg) - a3 * std::cos(3.0 * arg);
}

FirEq::FirEq(int sampleRate, int fftSize) : sr(sampleRate), nFft(fftSize), M(32767) {
    numBins = nFft / 2 + 1;
    eqCurveMag.resize(numBins, 1.0f);
    
    ir_buffer = fftw_alloc_real(nFft);
    eqSpectrum = fftw_alloc_complex(numBins);
    
    plan_inverse = fftw_plan_dft_c2r_1d(nFft, eqSpectrum, ir_buffer, FFTW_ESTIMATE);
    plan_forward = fftw_plan_dft_r2c_1d(nFft, ir_buffer, eqSpectrum, FFTW_ESTIMATE);
    
    reset();
}

FirEq::~FirEq() {
    fftw_destroy_plan(plan_inverse);
    fftw_destroy_plan(plan_forward);
    fftw_free(ir_buffer);
    fftw_free(eqSpectrum);
}

void FirEq::reset() {
    std::fill(eqCurveMag.begin(), eqCurveMag.end(), 1.0f);
    updateFilter();
}

void FirEq::addPeakingBand(float centerFreq, float gainDb, float q) {
    if (gainDb == 0.0f) return;
    double A = std::pow(10.0, gainDb / 40.0);
    double w0 = 2.0 * M_PI * centerFreq / sr;
    double alpha = std::sin(w0) / (2.0 * q);
    double a0 = 1.0 + alpha / A, b0 = (1.0 + alpha * A) / a0;
    double b1 = (-2.0 * std::cos(w0)) / a0, b2 = (1.0 - alpha * A) / a0;
    double a1 = (-2.0 * std::cos(w0)) / a0, a2 = (1.0 - alpha / A) / a0;

    for (int i = 0; i < numBins; i++) {
        double w = M_PI * static_cast<double>(i) / (numBins - 1); 
        double cos_w = std::cos(w), sin_w = std::sin(w);
        double cos_2w = std::cos(2.0 * w), sin_2w = std::sin(2.0 * w);
        double num_real = b0 + b1 * cos_w + b2 * cos_2w, num_imag = -(b1 * sin_w + b2 * sin_2w);
        double den_real = 1.0 + a1 * cos_w + a2 * cos_2w, den_imag = -(a1 * sin_w + a2 * sin_2w);
        double mag_num = std::sqrt(num_real * num_real + num_imag * num_imag);
        double mag_den = std::sqrt(den_real * den_real + den_imag * den_imag);
        eqCurveMag[i] *= static_cast<float>(mag_num / mag_den);
    }
    updateFilter();
}

void FirEq::updateFilter() {
    for(int i = 0; i < numBins; i++) {
        eqSpectrum[i][0] = eqCurveMag[i];
        eqSpectrum[i][1] = 0.0f;
    }
    
    // 1. iFFT generiert unendliche Zero-Phase Impulsantwort (Peak bei t=0)
    fftw_execute(plan_inverse);
    
    std::vector<float> temp_ir(nFft, 0.0f);
    int half_M = (M - 1) / 2;
    float norm = 1.0f / nFft;
    
    // 2. Zero-Phase Windowing: Abschneiden der unendlichen Tails ohne Delay-Verschiebung
    for(int i = 0; i <= half_M; i++) {
        float w = blackmanHarrisEq(half_M + i, M);
        if (i == 0) {
            temp_ir[0] = ir_buffer[0] * norm * w;
        } else {
            temp_ir[i] = ir_buffer[i] * norm * w;
            temp_ir[nFft - i] = ir_buffer[nFft - i] * norm * w; // Wrap-around Flanke
        }
    }
    
    for(int i = 0; i < nFft; i++) ir_buffer[i] = temp_ir[i];
    
    // 3. r2c FFT liefert das endliche, aliasing-freie Zero-Phase Spektrum
    fftw_execute(plan_forward);
}
