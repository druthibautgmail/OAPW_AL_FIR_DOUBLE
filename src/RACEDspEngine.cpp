#include "RACEDspEngine.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <iostream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------
// FFT Helper für den Web-Analyzer (UI bleibt 32-Bit Float)
// ---------------------------------------------------------
void computeFFT(std::vector<std::complex<float>>& data) {
    size_t n = data.size();
    if (n <= 1) return;
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * static_cast<float>(M_PI) / static_cast<float>(len);
        std::complex<float> wlen(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (size_t j = 0; j < len / 2; j++) {
                std::complex<float> u = data[i + j];
                std::complex<float> v = data[i + j + len / 2] * w;
                data[i + j] = u + v;
                data[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

// ---------------------------------------------------------
// FirCrossover Implementierung (Jetzt komplett 64-Bit Double)
// ---------------------------------------------------------
double blackmanHarris(int n, int filterLength) {
    double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
    double arg = (2.0 * M_PI * n) / (filterLength - 1);
    return a0 - a1 * std::cos(arg) + a2 * std::cos(2.0 * arg) - a3 * std::cos(3.0 * arg);
}

FirCrossover::FirCrossover(int blockLength, int filterLength, int sr) 
    : M(filterLength), L(blockLength), N(65536), sampleRate(sr) 
{
    in_audio = fftw_alloc_real(N);
    out_audio_spec = fftw_alloc_complex(N / 2 + 1);
    
    final_audio_out_low = fftw_alloc_real(N);
    final_audio_out_mid = fftw_alloc_real(N);
    final_audio_out_high = fftw_alloc_real(N);

    filter_ir_low = fftw_alloc_real(N);
    filter_ir_mid = fftw_alloc_real(N);
    filter_ir_high = fftw_alloc_real(N);

    filter_spec_low = fftw_alloc_complex(N / 2 + 1);
    filter_spec_mid = fftw_alloc_complex(N / 2 + 1);
    filter_spec_high = fftw_alloc_complex(N / 2 + 1);

    multiplied_spec_low = fftw_alloc_complex(N / 2 + 1);
    multiplied_spec_mid = fftw_alloc_complex(N / 2 + 1);
    multiplied_spec_high = fftw_alloc_complex(N / 2 + 1);

    overlap_buffer.resize(M - 1, 0.0);

    plan_forward = fftw_plan_dft_r2c_1d(N, in_audio, out_audio_spec, FFTW_ESTIMATE);
    plan_inverse_low = fftw_plan_dft_c2r_1d(N, multiplied_spec_low, final_audio_out_low, FFTW_ESTIMATE);
    plan_inverse_mid = fftw_plan_dft_c2r_1d(N, multiplied_spec_mid, final_audio_out_mid, FFTW_ESTIMATE);
    plan_inverse_high = fftw_plan_dft_c2r_1d(N, multiplied_spec_high, final_audio_out_high, FFTW_ESTIMATE);
    plan_filter_low = fftw_plan_dft_r2c_1d(N, filter_ir_low, filter_spec_low, FFTW_ESTIMATE);
    plan_filter_mid = fftw_plan_dft_r2c_1d(N, filter_ir_mid, filter_spec_mid, FFTW_ESTIMATE);
    plan_filter_high = fftw_plan_dft_r2c_1d(N, filter_ir_high, filter_spec_high, FFTW_ESTIMATE);
}

FirCrossover::~FirCrossover() {
    fftw_destroy_plan(plan_forward);
    fftw_destroy_plan(plan_inverse_low);
    fftw_destroy_plan(plan_inverse_mid);
    fftw_destroy_plan(plan_inverse_high);
    fftw_destroy_plan(plan_filter_low);
    fftw_destroy_plan(plan_filter_mid);
    fftw_destroy_plan(plan_filter_high);
    
    fftw_free(in_audio); fftw_free(out_audio_spec);
    fftw_free(final_audio_out_low); fftw_free(final_audio_out_mid); fftw_free(final_audio_out_high);
    fftw_free(filter_ir_low); fftw_free(filter_ir_mid); fftw_free(filter_ir_high);
    fftw_free(filter_spec_low); fftw_free(filter_spec_mid); fftw_free(filter_spec_high);
    fftw_free(multiplied_spec_low); fftw_free(multiplied_spec_mid); fftw_free(multiplied_spec_high);
}

void FirCrossover::generateWindowedSinc(double* ir_buffer, double cutoff1, double cutoff2, bool isBandpass) {
    double fc = cutoff1 / sampleRate;
    double sum = 0.0;
    int center = (M - 1) / 2;

    for (int i = 0; i < M; i++) {
        if (i == center) {
            ir_buffer[i] = 2.0 * M_PI * fc;
        } else {
            double x = 2.0 * M_PI * fc * (i - center);
            ir_buffer[i] = std::sin(x) / (i - center);
        }
        ir_buffer[i] *= blackmanHarris(i, M);
        sum += ir_buffer[i];
    }

    for (int i = 0; i < M; i++) ir_buffer[i] /= sum;

    if (cutoff2 > 0.0 && !isBandpass) { 
        for (int i = 0; i < M; i++) ir_buffer[i] = -ir_buffer[i];
        ir_buffer[center] += 1.0;
    }
    
    for (int i = M; i < N; i++) ir_buffer[i] = 0.0;
}

void FirCrossover::updateFilters(double freqLow, double freqHigh) {
    generateWindowedSinc(filter_ir_low, freqLow, 0.0, false);
    generateWindowedSinc(filter_ir_high, freqHigh, 1.0, false); 

    int center = (M - 1) / 2;
    for (int i = 0; i < N; i++) {
        double dirac = (i == center) ? 1.0 : 0.0;
        filter_ir_mid[i] = dirac - filter_ir_low[i] - filter_ir_high[i];
    }

    fftw_execute(plan_filter_low);
    fftw_execute(plan_filter_mid);
    fftw_execute(plan_filter_high);
}

// 64-Bit Signatur und sizeof() Korrekturen
void FirCrossover::processBlock(const double* incoming_audio, double* out_low, double* out_mid, double* out_high, const fftw_complex* eqSpectrum) {
    std::memcpy(in_audio, overlap_buffer.data(), (M - 1) * sizeof(double));
    std::memcpy(in_audio + (M - 1), incoming_audio, L * sizeof(double));
    for (int i = (M - 1 + L); i < N; i++) in_audio[i] = 0.0; 
    
    std::memcpy(overlap_buffer.data(), incoming_audio + L - (M - 1), (M - 1) * sizeof(double));

    fftw_execute(plan_forward);

    int numBins = N / 2 + 1;
    for (int i = 0; i < numBins; i++) {
        double a_re = out_audio_spec[i][0];
        double a_im = out_audio_spec[i][1];

        // Phasenlinearer FIR-EQ (echte komplexe Multiplikation)
        if (eqSpectrum != nullptr) {
            double eq_re = eqSpectrum[i][0];
            double eq_im = eqSpectrum[i][1];
            
            double tmp_re = (a_re * eq_re) - (a_im * eq_im);
            double tmp_im = (a_re * eq_im) + (a_im * eq_re);
            
            a_re = tmp_re;
            a_im = tmp_im;
        }

        double fl_re = filter_spec_low[i][0], fl_im = filter_spec_low[i][1];
        multiplied_spec_low[i][0] = (a_re * fl_re) - (a_im * fl_im);
        multiplied_spec_low[i][1] = (a_re * fl_im) + (a_im * fl_re);

        double fm_re = filter_spec_mid[i][0], fm_im = filter_spec_mid[i][1];
        multiplied_spec_mid[i][0] = (a_re * fm_re) - (a_im * fm_im);
        multiplied_spec_mid[i][1] = (a_re * fm_im) + (a_im * fm_re);

        double fh_re = filter_spec_high[i][0], fh_im = filter_spec_high[i][1];
        multiplied_spec_high[i][0] = (a_re * fh_re) - (a_im * fh_im);
        multiplied_spec_high[i][1] = (a_re * fh_im) + (a_im * fh_re);
    }

    fftw_execute(plan_inverse_low);
    fftw_execute(plan_inverse_mid);
    fftw_execute(plan_inverse_high);

    double norm = 1.0 / N;
    for (int i = 0; i < L; i++) {
        out_low[i]  = final_audio_out_low[M - 1 + i] * norm;
        out_mid[i]  = final_audio_out_mid[M - 1 + i] * norm;
        out_high[i] = final_audio_out_high[M - 1 + i] * norm;
    }
}

// ---------------------------------------------------------
// RACEDspEngine (FIR Hauptklasse)
// ---------------------------------------------------------
RACEDspEngine::RACEDspEngine(double initialDelayUs, double initialAttenuationDb, double initialCenterP, bool initialFreqLimit, int sr) 
    : sampleRate(sr), 
      firCrossoverL(32768, 32767, sr), 
      firCrossoverR(32768, 32767, sr),
      firEqualizer(sr, 65536)
{
    spectrumBuffer.resize(32768, 0.0f);
    spectrumIndex = 0;

    int maxDelaySamples = sr; 
    delayBufferL.resize(maxDelaySamples, 0.0);
    delayBufferR.resize(maxDelaySamples, 0.0);
    
    lowL.resize(32768, 0.0); midL.resize(32768, 0.0); highL.resize(32768, 0.0);
    lowR.resize(32768, 0.0); midR.resize(32768, 0.0); highR.resize(32768, 0.0);

    writeIndex = 0;
    volume = 1.0;
    makeupGainDb = 0.0;
    makeupGainLinear = 1.0;
    raceEnabled = true;
    filtersEnabled = true;
    eqEnabled = false;
    lowBypassEnabled = false;

    currentFreqLow = 400.0;
    currentFreqHigh = 4530.0;
    
    firCrossoverL.updateFilters(currentFreqLow, currentFreqHigh);
    firCrossoverR.updateFilters(currentFreqLow, currentFreqHigh);
    
    for (int i = 0; i < 3; ++i) {
        double f = (i == 0) ? 100.0 : (i == 1) ? 1000.0 : 5000.0;
        eqBands[i] = {f, 0.707, 0.0};
    }
    firEqualizer.reset();

    setParameters(initialDelayUs, initialAttenuationDb, initialCenterP, initialFreqLimit);
}

RACEDspEngine::~RACEDspEngine() {}

void RACEDspEngine::setParameters(double newDelayUs, double newAttenuationDb, double newCenterP, bool newFreqLimit) {
    std::lock_guard<std::mutex> lock(dspMutex);
    delayUs = newDelayUs;
    attenuationDb = newAttenuationDb;
    
    double safeAtten = std::abs(attenuationDb);
    attenuationLinear = std::pow(10.0, -safeAtten / 20.0);
    
    centerP = newCenterP;
    freqLimitRACE = newFreqLimit;
    
    dn = static_cast<int>(delayUs * static_cast<double>(sampleRate) / 1000000.0);
    if (dn < 5) dn = 10; 
}

void RACEDspEngine::setVolume(double newVolume) { std::lock_guard<std::mutex> lock(dspMutex); volume = newVolume; }

void RACEDspEngine::setMakeupGain(double gainDb) { 
    std::lock_guard<std::mutex> lock(dspMutex); 
    makeupGainDb = gainDb; 
    makeupGainLinear = std::pow(10.0, makeupGainDb / 20.0); 
}
double RACEDspEngine::getMakeupGain() { return makeupGainDb; }

void RACEDspEngine::setRaceEnabled(bool enabled) { std::lock_guard<std::mutex> lock(dspMutex); raceEnabled = enabled; }
void RACEDspEngine::setFiltersEnabled(bool enabled) { std::lock_guard<std::mutex> lock(dspMutex); filtersEnabled = enabled; }
void RACEDspEngine::setEqEnabled(bool enabled) { std::lock_guard<std::mutex> lock(dspMutex); eqEnabled = enabled; }

void RACEDspEngine::setEqBand(int bandIndex, double freq, double q, double gainDb) {
    std::lock_guard<std::mutex> lock(dspMutex);
    if(bandIndex >= 0 && bandIndex < 3) { 
        eqBands[bandIndex] = {freq, q, gainDb};
        
        firEqualizer.reset();
        for (int b = 0; b < 3; ++b) {
            if (eqBands[b].g != 0.0) {
                firEqualizer.addPeakingBand(eqBands[b].f, eqBands[b].g, eqBands[b].q);
            }
        }
    }
}

void RACEDspEngine::processSamples(float* interleavedSamples, size_t frames) {
    std::lock_guard<std::mutex> lock(dspMutex);

    // KORRIGIERTER Bypass-Block
    if (!filtersEnabled) {
        for (size_t i = 0; i < frames; i++) {
            double outL = static_cast<double>(interleavedSamples[i * 2]) * volume;
            double outR = static_cast<double>(interleavedSamples[i * 2 + 1]) * volume;
            
            outL = std::clamp(outL, -1.0, 1.0);
            outR = std::clamp(outR, -1.0, 1.0);
            
            interleavedSamples[i * 2] = static_cast<float>(outL);
            interleavedSamples[i * 2 + 1] = static_cast<float>(outR);
            
            spectrumBuffer[spectrumIndex] = static_cast<float>((outL + outR) * 0.5);
            spectrumIndex = (spectrumIndex + 1) % 32768;
        }
        return;
    }
    
    int L = 32768;
    int bufferSize = delayBufferL.size();

    // Casting auf Double beim Einlesen
    for (size_t i = 0; i < frames; i++) {
        inputFifoL.push_back(static_cast<double>(interleavedSamples[i * 2]));
        inputFifoR.push_back(static_cast<double>(interleavedSamples[i * 2 + 1]));
    }

    while (inputFifoL.size() >= (size_t)L) {
        const fftw_complex* eqData = eqEnabled ? firEqualizer.getSpectrum() : nullptr;

        firCrossoverL.processBlock(inputFifoL.data(), lowL.data(), midL.data(), highL.data(), eqData);
        firCrossoverR.processBlock(inputFifoR.data(), lowR.data(), midR.data(), highR.data(), eqData);

        inputFifoL.erase(inputFifoL.begin(), inputFifoL.begin() + L);
        inputFifoR.erase(inputFifoR.begin(), inputFifoR.begin() + L);

        for (int k = 0; k < L; k++) {
            double bypassL = 0.0;
            double bypassR = 0.0;
            double midInL = 0.0;
            double midInR = 0.0;

            if (freqLimitRACE) {
                if (lowBypassEnabled) {
                    bypassL = lowL[k] + highL[k];
                    bypassR = lowR[k] + highR[k];
                    midInL  = midL[k];
                    midInR  = midR[k];
                } else {
                    bypassL = highL[k];
                    bypassR = highR[k];
                    midInL  = lowL[k] + midL[k]; 
                    midInR  = lowR[k] + midR[k];
                }
            } else {
                bypassL = 0.0;
                bypassR = 0.0;
                midInL  = lowL[k] + midL[k] + highL[k];
                midInR  = lowR[k] + midR[k] + highR[k];
            }

            int readIndex = (writeIndex - dn + bufferSize) % bufferSize;
            double delayedMidL = delayBufferL[readIndex];
            double delayedMidR = delayBufferR[readIndex];

            double crossTalkL = 0.0;
            double crossTalkR = 0.0;

            if (raceEnabled) {
                crossTalkL = attenuationLinear * (delayedMidR - centerP * delayedMidL) / (1.0 + centerP);
                crossTalkR = attenuationLinear * (delayedMidL - centerP * delayedMidR) / (1.0 + centerP);
            }

            double currentRaceMidL = midInL - crossTalkL;
            double currentRaceMidR = midInR - crossTalkR;

            // Feed-Forward Architektur (Phasenrein)
            delayBufferL[writeIndex] = midInL; 
            delayBufferR[writeIndex] = midInR; 
            writeIndex = (writeIndex + 1) % bufferSize;

            double outL = bypassL + currentRaceMidL;
            double outR = bypassR + currentRaceMidR;

            outL *= makeupGainLinear;
            outR *= makeupGainLinear;

            outL *= volume;
            outR *= volume;

            if (outL > 1.0 || outL < -1.0 || outR > 1.0 || outR < -1.0) {
                static int clipCount = 0; 
                if (clipCount++ % 100 == 0) {
                   std::cerr << "[WARN] DSP Clipping erkannt! L: " << outL << ", R: " << outR << std::endl;
                }
            }

            // KORRIGIERT: Hard-Clipping am äußersten Rand für ALSA
            outL = std::clamp(outL, -1.0, 1.0);
            outR = std::clamp(outR, -1.0, 1.0);

            // UI Speisung
            spectrumBuffer[spectrumIndex] = static_cast<float>((outL + outR) * 0.5);
            spectrumIndex = (spectrumIndex + 1) % 32768;

            outputFifoL.push_back(outL);
            outputFifoR.push_back(outR);
        }
    }

    size_t available = std::min(frames, outputFifoL.size());
    for (size_t i = 0; i < available; i++) {
        interleavedSamples[i * 2] = static_cast<float>(outputFifoL[i]);
        interleavedSamples[i * 2 + 1] = static_cast<float>(outputFifoR[i]);
    }
    for (size_t i = available; i < frames; i++) {
        interleavedSamples[i * 2] = 0.0f;
        interleavedSamples[i * 2 + 1] = 0.0f;
    }
    
    if (available > 0) {
        outputFifoL.erase(outputFifoL.begin(), outputFifoL.begin() + available);
        outputFifoR.erase(outputFifoR.begin(), outputFifoR.begin() + available);
    }
}

std::vector<double> RACEDspEngine::getSpectrumBands() {
    std::vector<float> audioData(32768, 0.0f);
    {
        std::lock_guard<std::mutex> lock(dspMutex);
        for(int i = 0; i < 32768; i++) {
            audioData[i] = spectrumBuffer[(spectrumIndex + i) % 32768];
        }
    }
    
    std::vector<std::complex<float>> complexData(32768);
    for(int i = 0; i < 32768; i++) {
        float multiplier = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / 32767.0f));
        complexData[i] = std::complex<float>(audioData[i] * multiplier, 0.0f);
    }
    
    computeFFT(complexData);
    
    std::vector<double> bands(32, 0.0);
    float minFreq = 20.0f, maxFreq = 20000.0f;
    float logMin = std::log10(minFreq), logMax = std::log10(maxFreq);
    
    for (int b = 0; b < 32; b++) {
        float freqStart = std::pow(10.0f, logMin + (float)b / 32.0f * (logMax - logMin));
        float freqEnd   = std::pow(10.0f, logMin + (float)(b + 1) / 32.0f * (logMax - logMin));
        
        int binStart = (int)(freqStart * 32768.0f / static_cast<float>(sampleRate));
        int binEnd   = (int)(freqEnd * 32768.0f / static_cast<float>(sampleRate));
        
        if (binStart < 1) binStart = 1; 
        if (binEnd > 4095) binEnd = 4095; 
        if (binStart > binEnd) binEnd = binStart;
        
        // NEU: Energie-Aufsummierung (RMS) anstelle von Peak-Hold für lineare Rosa-Rauschen Darstellung
        float energySum = 0.0f;
        for (int i = binStart; i <= binEnd; i++) {
            float mag = std::abs(complexData[i]) / 16384.0f;
            energySum += (mag * mag); // Quadrieren für Leistung
        }
        
        float rmsMag = std::sqrt(energySum);
        float db = 20.0f * std::log10(rmsMag + 1e-12f); // 1e-12f verhindert log(0)
        
        // Optische Skalierung: -85 dB ist der Boden (0.0), -20 dB ist der obere rote Anschlag (1.0)
        float normalized = (db + 85.0f) / 65.0f; 
        
        if (normalized < 0.0f) normalized = 0.0f; 
        if (normalized > 1.0f) normalized = 1.0f;        
        bands[b] = static_cast<double>(normalized);
    }

    return bands;
}

double RACEDspEngine::getDelayUs() { return delayUs; }
double RACEDspEngine::getAttenuationDb() { return attenuationDb; }
double RACEDspEngine::getCenterP() { return centerP; }
bool RACEDspEngine::getFreqLimit() { return freqLimitRACE; }
double RACEDspEngine::getVolume() { return volume; }
bool RACEDspEngine::getRaceEnabled() { return raceEnabled; }
bool RACEDspEngine::getFiltersEnabled() { return filtersEnabled; }
bool RACEDspEngine::getEqEnabled() { return eqEnabled; }

double RACEDspEngine::getEqFreq(int bandIndex) { if (bandIndex >= 0 && bandIndex < 3) return eqBands[bandIndex].f; return 1000.0; }
double RACEDspEngine::getEqQ(int bandIndex) { if (bandIndex >= 0 && bandIndex < 3) return eqBands[bandIndex].q; return 0.707; }
double RACEDspEngine::getEqGain(int bandIndex) { if (bandIndex >= 0 && bandIndex < 3) return eqBands[bandIndex].g; return 0.0; }

void RACEDspEngine::setCrossoverFreqLow(double freq) {
    std::lock_guard<std::mutex> lock(dspMutex);
    currentFreqLow = freq;
    firCrossoverL.updateFilters(currentFreqLow, currentFreqHigh);
    firCrossoverR.updateFilters(currentFreqLow, currentFreqHigh);
}
double RACEDspEngine::getCrossoverFreqLow() { return currentFreqLow; }

void RACEDspEngine::setCrossoverFreqHigh(double freq) {
    std::lock_guard<std::mutex> lock(dspMutex);
    currentFreqHigh = freq;
    firCrossoverL.updateFilters(currentFreqLow, currentFreqHigh);
    firCrossoverR.updateFilters(currentFreqLow, currentFreqHigh);
}
double RACEDspEngine::getCrossoverFreqHigh() { return currentFreqHigh; }

void RACEDspEngine::setLowBypassEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(dspMutex);
    lowBypassEnabled = enabled;
}

bool RACEDspEngine::getLowBypassEnabled() {
    return lowBypassEnabled;
}
