#include "FirEq.h"

FirEq::FirEq(int sampleRate, int fftSize) : sr(sampleRate), nFft(fftSize) {
    // Bei einer komplexen FFT sind nFft/2 + 1 Bins relevant für die positiven Frequenzen
    numBins = nFft / 2 + 1;
    eqCurve.resize(numBins, 1.0f);
}

void FirEq::reset() {
    std::fill(eqCurve.begin(), eqCurve.end(), 1.0f);
}

void FirEq::addPeakingBand(float centerFreq, float gainDb, float q) {
    if (gainDb == 0.0f) return;

    // 1. Analoge Biquad-Koeffizienten nach Robert Bristow-Johnson berechnen
    double A = std::pow(10.0, gainDb / 40.0);
    double w0 = 2.0 * M_PI * centerFreq / sr;
    double alpha = std::sin(w0) / (2.0 * q);

    double a0 = 1.0 + alpha / A;
    double b0 = (1.0 + alpha * A) / a0;
    double b1 = (-2.0 * std::cos(w0)) / a0;
    double b2 = (1.0 - alpha * A) / a0;
    double a1 = (-2.0 * std::cos(w0)) / a0;
    double a2 = (1.0 - alpha / A) / a0;

    // 2. Frequenzgang (Magnitude) für jeden FFT-Bin evaluieren
    for (int i = 0; i < numBins; i++) {
        // Normierte Frequenz des aktuellen Bins (0 bis Pi)
        double w = M_PI * static_cast<double>(i) / (numBins - 1); 
        
        double cos_w = std::cos(w);
        double sin_w = std::sin(w);
        double cos_2w = std::cos(2.0 * w);
        double sin_2w = std::sin(2.0 * w);

        // Zähler (Numerator) und Nenner (Denominator) der Übertragungsfunktion H(e^jw)
        double num_real = b0 + b1 * cos_w + b2 * cos_2w;
        double num_imag = -(b1 * sin_w + b2 * sin_2w);
        
        double den_real = 1.0 + a1 * cos_w + a2 * cos_2w;
        double den_imag = -(a1 * sin_w + a2 * sin_2w);

        // Betrag (Magnitude) berechnen = sqrt(real^2 + imag^2)
        double mag_num = std::sqrt(num_real * num_real + num_imag * num_imag);
        double mag_den = std::sqrt(den_real * den_real + den_imag * den_imag);
        
        // 3. Auf die EQ-Kurve aufmultiplizieren (lineare Skalierung)
        eqCurve[i] *= static_cast<float>(mag_num / mag_den);
    }
}

void FirEq::applyToFFT(std::vector<std::complex<float>>& complexData) {
    // DC (Gleichanteil) und Nyquist-Frequenz
    complexData[0] *= eqCurve[0];
    complexData[nFft / 2] *= eqCurve[numBins - 1];

    // Multiplikation der restlichen Bins
    for (int i = 1; i < numBins - 1; i++) {
        float gain = eqCurve[i];
        
        // Komplexe Bins mit dem reellen Gain skalieren (erhält die Phase exakt bei 0!)
        complexData[i] *= gain;           // Positive Frequenzen
        complexData[nFft - i] *= gain;    // Negative Frequenzen (gespiegelt für Real-Output)
    }
}

void FirEq::applyToFFTW(fftwf_complex* fftwData) {
    // fftwData[i][0] ist der Realteil, fftwData[i][1] ist der Imaginärteil
    for (int i = 0; i < numBins; i++) {
        fftwData[i][0] *= eqCurve[i];
        fftwData[i][1] *= eqCurve[i];
    }
}

