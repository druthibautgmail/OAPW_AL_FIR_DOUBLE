#ifndef RACE_DSP_ENGINE_H
#define RACE_DSP_ENGINE_H

#include <vector>
#include <mutex>
#include <array>
#include <complex>
#include <cstdint>
#include <fftw3.h>
#include "FirEq.h"

class FirCrossover {
private:
    int M, L, N;
    int sampleRate;

    // 64-Bit Arrays für die Audio-Verarbeitung
    double *in_audio, *final_audio_out_low, *final_audio_out_mid, *final_audio_out_high;
    fftw_complex *out_audio_spec;
    fftw_complex *multiplied_spec_low, *multiplied_spec_mid, *multiplied_spec_high;

    double *filter_ir_low, *filter_ir_mid, *filter_ir_high;
    fftw_complex *filter_spec_low, *filter_spec_mid, *filter_spec_high;

    fftw_plan plan_forward;
    fftw_plan plan_inverse_low, plan_inverse_mid, plan_inverse_high;
    fftw_plan plan_filter_low, plan_filter_mid, plan_filter_high;

    std::vector<double> overlap_buffer;

    void generateWindowedSinc(double* ir_buffer, double cutoff1, double cutoff2, bool isBandpass);

public:
    FirCrossover(int blockLength = 32768, int filterLength = 32767, int sr = 192000);
    ~FirCrossover();

    void updateFilters(double freqLow, double freqHigh);
    void processBlock(const double* incoming_audio, double* out_low, double* out_mid, double* out_high, const fftw_complex* eqSpectrum = nullptr);
};

struct EqBand { 
    double f, q, g; 
};

class RACEDspEngine {
private:
    int sampleRate;
    double delayUs, attenuationDb, attenuationLinear, centerP;
    int dn;
    bool freqLimitRACE;
    double currentFreqLow, currentFreqHigh;
    double volume;
    double makeupGainDb;
    double makeupGainLinear;
    bool raceEnabled, filtersEnabled, eqEnabled;
    bool lowBypassEnabled; 

    // UI-Spektrum bleibt auf 32-Bit float für das Frontend
    std::vector<float> spectrumBuffer;
    int spectrumIndex;

    // 64-Bit Audio-Puffer
    std::vector<double> delayBufferL;
    std::vector<double> delayBufferR;
    int writeIndex;

    FirCrossover firCrossoverL;
    FirCrossover firCrossoverR;

    std::vector<double> inputFifoL;
    std::vector<double> inputFifoR;
    std::vector<double> outputFifoL;
    std::vector<double> outputFifoR;

    std::vector<double> lowL, midL, highL;
    std::vector<double> lowR, midR, highR;

    FirEq firEqualizer;
    std::array<EqBand, 3> eqBands;

    std::mutex dspMutex;

public:
    RACEDspEngine(double initialDelayUs, double initialAttenuationDb, double initialCenterP, bool initialFreqLimit, int sampleRate = 192000);
    ~RACEDspEngine();

    void setParameters(double newDelayUs, double newAttenuationDb, double newCenterP, bool newFreqLimit);
    void setCrossoverFreqLow(double freq);
    double getCrossoverFreqLow();
    void setCrossoverFreqHigh(double freq);
    double getCrossoverFreqHigh();
    void setVolume(double newVolume);
    void setMakeupGain(double gainDb);
    double getMakeupGain();    
    void setRaceEnabled(bool enabled);
    void setFiltersEnabled(bool enabled);
    void setEqEnabled(bool enabled);
    void setEqBand(int bandIndex, double freq, double q, double gainDb);

    void setLowBypassEnabled(bool enabled);
    bool getLowBypassEnabled(); 

    double getDelayUs();
    double getAttenuationDb();
    double getCenterP();
    bool getFreqLimit();
    double getVolume();
    bool getRaceEnabled();
    bool getFiltersEnabled();
    bool getEqEnabled();
    double getEqFreq(int bandIndex);
    double getEqQ(int bandIndex);
    double getEqGain(int bandIndex);

    std::vector<double> getSpectrumBands();
    
    // ALSA Eingang bleibt auf float*
    void processSamples(float* interleavedSamples, size_t frames);
};

#endif // RACE_DSP_ENGINE_H
