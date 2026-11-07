#include "AlsaPlayback.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <iomanip>

// Diagnose: Intervall der Statuszeile in Sekunden (0 = aus) und Mindestdauer, bevor eine
// Drift-Schätzung ausgegeben wird. Per -D überschreibbar (z.B. für Tests).
#ifndef OAPW_STATUS_INTERVAL_SEC
#define OAPW_STATUS_INTERVAL_SEC 60
#endif
#ifndef OAPW_TREND_MIN_SEC
#define OAPW_TREND_MIN_SEC 180
#endif
// Erst ab dieser Messdauer kann eine Drift-WARNUNG ausgelöst werden (davor ist die Schätzung
// zu ungenau). Der Füllstand ändert sich nur in Schritten von einer Periode; die Auflösung
// der Schätzung ist daher etwa Periode / Messdauer.
#ifndef OAPW_TREND_WARN_SEC
#define OAPW_TREND_WARN_SEC 600
#endif

// Zielfüllstand des Ringpuffers vor dem Abspielen: capacity / kTargetFillDivisor.
// Er wirkt als Polster gegen Jitter und gegen Takt-Drift zwischen Capture (Loopback)
// und DAC. Größerer Wert = kleineres Polster = weniger Latenz, aber früher Aussetzer
// bei leerlaufendem Puffer. Bei 131072 Floats (Stereo, 192 kHz) und Divisor 4 sind
// das 16384 Frames, also ca. 85 ms.
static constexpr size_t kTargetFillDivisor = 4;

// Float (-1.0 ... +1.0) -> ganzzahliger PCM-Wert. scale = groesster positiver Wert des
// Zielformats (32767, 8388607, 2147483647). Gerechnet wird in double, weil 2147483647 als
// float nicht darstellbar ist: Das alte "clamped * 2147483647.0f" ergab bei +1.0 den Wert
// 2^31, und die Umwandlung nach int32_t ist dann undefiniert (auf x86 kippt sie zu
// Vollaussteuerung mit falschem Vorzeichen). Zusaetzlich wird gerundet statt abgeschnitten,
// und NaN wird zu Stille.
static inline int32_t floatToPcm(float sample, double scale) {
    double x = static_cast<double>(sample);
    if (!(x >= -1.0)) x = (x != x) ? 0.0 : -1.0;   // NaN -> 0, kleiner -1 -> -1
    else if (x > 1.0) x = 1.0;
    return static_cast<int32_t>(std::lrint(x * scale));
}

AlsaPlayback::AlsaPlayback(const std::string& device_name,
                           snd_pcm_format_t format,
                           unsigned int sample_rate,
                           unsigned int channels,
                           LockFreeRingBuffer<float>& ring_buffer)
    : device_name_(device_name),
      format_(format),
      sample_rate_(sample_rate),
      channels_(channels),
      ring_buffer_(ring_buffer),
      dsp_callback_(nullptr),
      playback_handle_(nullptr),
      is_running_(false),
      period_frames_(1024) {}

AlsaPlayback::~AlsaPlayback() {
    stop();
}

void AlsaPlayback::set_dsp_callback(std::function<void(float*, size_t)> callback) {
    dsp_callback_ = callback;
}

bool AlsaPlayback::start() {
    int err;
    snd_pcm_hw_params_t* hw_params;

    // 1. Gerät im Playback-Modus (DAC) öffnen
    if ((err = snd_pcm_open(&playback_handle_, device_name_.c_str(), SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        std::cerr << "Fehler beim Öffnen des Playback-Geräts (" << device_name_ << "): " 
                  << snd_strerror(err) << std::endl;
        return false;
    }

    // 2. Hardware-Parameter konfigurieren
    snd_pcm_hw_params_malloc(&hw_params);
    snd_pcm_hw_params_any(playback_handle_, hw_params);

    snd_pcm_hw_params_set_access(playback_handle_, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(playback_handle_, hw_params, format_);
    snd_pcm_hw_params_set_channels(playback_handle_, hw_params, channels_);

    unsigned int exact_rate = sample_rate_;
    int dir = 0;
    snd_pcm_hw_params_set_rate_near(playback_handle_, hw_params, &exact_rate, &dir);
    if (exact_rate != sample_rate_) {
        std::cerr << "Warnung: DAC " << device_name_ << " nutzt " << exact_rate 
                  << " Hz anstelle der angefragten " << sample_rate_ << " Hz." << std::endl;
    }

    snd_pcm_hw_params_set_period_size_near(playback_handle_, hw_params, &period_frames_, &dir);

    if ((err = snd_pcm_hw_params(playback_handle_, hw_params)) < 0) {
        std::cerr << "Fehler beim Setzen der DAC HW-Parameter: " << snd_strerror(err) << std::endl;
        snd_pcm_hw_params_free(hw_params);
        return false;
    }
    // Diagnose: tatsächlich ausgehandelte Periode und Hardware-Puffergröße ausgeben
    {
        snd_pcm_uframes_t actual_period = 0, actual_buffer = 0;
        int actual_dir = 0;
        snd_pcm_hw_params_get_period_size(hw_params, &actual_period, &actual_dir);
        snd_pcm_hw_params_get_buffer_size(hw_params, &actual_buffer);
        std::cout << "ALSA Playback: Periode " << actual_period << " Frames, DAC-Puffer "
                  << actual_buffer << " Frames";
        // Die RACE-Engine verarbeitet intern in Blöcken zu 32768 Frames (siehe RACEDspEngine).
        // Teilt die Periode diesen Wert nicht, kann die Ausgabe periodisch aussetzen.
        if (actual_period != 0 && (32768 % actual_period) != 0) {
            std::cout << "  [Hinweis: Periode teilt 32768 nicht]";
        }
        std::cout << std::endl;
    }
    snd_pcm_hw_params_free(hw_params);

    if ((err = snd_pcm_prepare(playback_handle_)) < 0) {
        std::cerr << "Fehler bei snd_pcm_prepare (Playback): " << snd_strerror(err) << std::endl;
        return false;
    }

    // Thread starten
    is_running_.store(true, std::memory_order_release);
    playback_thread_ = std::thread(&AlsaPlayback::playbackLoop, this);

    std::cout << "ALSA Playback gestartet auf " << device_name_ << " mit " 
              << exact_rate << " Hz, " << channels_ << " Kanälen." << std::endl;

    return true;
}

void AlsaPlayback::stop() {
    if (is_running_.load(std::memory_order_acquire)) {
        is_running_.store(false, std::memory_order_release);
        if (playback_thread_.joinable()) {
            playback_thread_.join();
        }
    }
    
    if (playback_handle_) {
        snd_pcm_drain(playback_handle_);
        snd_pcm_close(playback_handle_);
        playback_handle_ = nullptr;
    }
}

void AlsaPlayback::recover_from_xrun(int err) {
    if (err == -EPIPE) {
        // Underrun: Wir haben dem DAC nicht schnell genug Daten geliefert.
        std::cerr << "Playback Underrun (-EPIPE). DAC fordert Daten. Resetting..." << std::endl;
        snd_pcm_prepare(playback_handle_);
    } else if (err == -ESTRPIPE) {
        // Suspend
        while ((err = snd_pcm_resume(playback_handle_)) == -EAGAIN) {
            usleep(100);
        }
        if (err < 0) {
            snd_pcm_prepare(playback_handle_);
        }
    } else {
        std::cerr << "ALSA Write-Fehler: " << snd_strerror(err) << std::endl;
    }
}

void AlsaPlayback::playbackLoop() {
    using Clock = std::chrono::steady_clock;

    int bytes_per_sample = snd_pcm_format_physical_width(format_) / 8;
    int total_samples = period_frames_ * channels_;
    
    std::vector<float> dsp_buffer(total_samples);
    std::vector<uint8_t> raw_buffer(total_samples * bytes_per_sample, 0);

    // Zielfüllstand (in Floats) für das Vorfüllen; mindestens 2 Perioden, und es muss
    // noch mindestens eine Periode Platz für den Producer bleiben.
    const size_t total_items = static_cast<size_t>(total_samples);
    const size_t capacity = ring_buffer_.get_capacity();
    size_t target_fill = std::max(capacity / kTargetFillDivisor, 2 * total_items);
    if (target_fill + total_items > capacity) {
        target_fill = (capacity > total_items) ? capacity - total_items : total_items;
        std::cerr << "Warnung: Ringpuffer (" << capacity << ") ist für Periode "
                  << period_frames_ << " zu klein, Zielfüllstand reduziert." << std::endl;
    }

    bool primed = false;           // true = Ringpuffer ist vorgefüllt, es wird abgespielt
    size_t underrun_count = 0;
    size_t last_dropped_items = 0;
    size_t dac_xruns = 0;          // negative Rückgaben von snd_pcm_writei (Underrun, Suspend, Fehler)
    size_t short_writes = 0;

    auto items_ms = [this](double items) {
        return items / channels_ * 1000.0 / sample_rate_;
    };
    auto fill_ms = [&](size_t items) { return items_ms(static_cast<double>(items)); };

    // --- Diagnose: DAC-Puffergröße für die Füllstandsmessung der Hardware-Warteschlange ---
    snd_pcm_uframes_t dac_buffer_frames = 0, dac_period_frames = 0;
    if (snd_pcm_get_params(playback_handle_, &dac_buffer_frames, &dac_period_frames) < 0) {
        dac_buffer_frames = 0;
    }

    // --- Diagnose: Statistik pro Intervall ---
    const bool status_enabled = (OAPW_STATUS_INTERVAL_SEC > 0);
    const auto status_interval = std::chrono::seconds(OAPW_STATUS_INTERVAL_SEC);
    auto interval_start = Clock::now();
    size_t iv_fill_min = SIZE_MAX, iv_fill_max = 0, iv_fill_n = 0;
    double iv_fill_sum = 0.0;
    double iv_dsp_max_ms = 0.0;
    long iv_dac_queue_min = LONG_MAX;
    bool iv_unprimed_seen = false;
    size_t iv_underruns0 = 0, iv_dropped0 = 0, iv_xruns0 = 0, iv_short0 = 0;
    // Drift-Schätzung: Mittlerer Füllstand eines "sauberen" Intervalls als Basislinie
    bool base_valid = false;
    double base_avg_frames = 0.0;
    Clock::time_point base_time{};

    while (is_running_.load(std::memory_order_relaxed)) {
        bool have_audio = false;

        // 1a. Vorfüllen: Bis der Zielfüllstand erreicht ist, wird digitale Stille an den
        // DAC gesendet (hält den Hardware-Takt am Laufen). Gilt beim Start und nach jedem
        // Underrun, z.B. wenn der MediaPlayer pausiert oder der Takt-Drift den Puffer
        // geleert hat.
        if (!primed && ring_buffer_.get_fill_level() >= target_fill) {
            primed = true;
            const size_t dropped = ring_buffer_.get_dropped_items();
            std::cout << "Ringpuffer vorgefüllt (" << fill_ms(ring_buffer_.get_fill_level())
                      << " ms), starte Wiedergabe";
            if (dropped != last_dropped_items) {
                std::cout << "; seit letzter Meldung vom Capture verworfen: "
                          << (dropped - last_dropped_items) << " Samples (Overrun)";
                last_dropped_items = dropped;
            }
            std::cout << std::endl;
        }

        // 1b. Betrieb: Daten aus dem Ringpuffer holen. Der Pufferinhalt bleibt bei einem
        // Underrun unverändert (kein reset aus dem Consumer-Thread); das Stück läuft nach
        // dem erneuten Vorfüllen an derselben Stelle weiter.
        if (primed) {
            const size_t fill_now = ring_buffer_.get_fill_level();
            iv_fill_min = std::min(iv_fill_min, fill_now);
            iv_fill_max = std::max(iv_fill_max, fill_now);
            iv_fill_sum += static_cast<double>(fill_now);
            ++iv_fill_n;

            if (ring_buffer_.pop(dsp_buffer.data(), total_items)) {
                have_audio = true;
            } else {
                primed = false;
                ++underrun_count;
                std::cerr << "Ringpuffer-Underrun #" << underrun_count << " (Füllstand "
                          << fill_ms(ring_buffer_.get_fill_level())
                          << " ms): sende Stille bis zum Zielfüllstand ("
                          << fill_ms(target_fill) << " ms)" << std::endl;
            }
        }
        if (!primed) iv_unprimed_seen = true;

        if (!have_audio) {
            std::fill(raw_buffer.begin(), raw_buffer.end(), 0);
        } else {
            // 2. RACE DSP Algorithmus anwenden (falls ein Callback konfiguriert ist)
            if (dsp_callback_) {
                const auto t0 = Clock::now();
                dsp_callback_(dsp_buffer.data(), period_frames_);
                const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                iv_dsp_max_ms = std::max(iv_dsp_max_ms, ms);
            }

            // 3. Float32 (-1.0 bis 1.0) zurück in das DAC-Hardware-Format konvertieren
            if (format_ == SND_PCM_FORMAT_S16_LE) {
                int16_t* pcm16 = reinterpret_cast<int16_t*>(raw_buffer.data());
                for (int i = 0; i < total_samples; ++i) {
                    pcm16[i] = static_cast<int16_t>(floatToPcm(dsp_buffer[i], 32767.0));
                }
            } else if (format_ == SND_PCM_FORMAT_S32_LE) {
                int32_t* pcm32 = reinterpret_cast<int32_t*>(raw_buffer.data());
                for (int i = 0; i < total_samples; ++i) {
                    pcm32[i] = floatToPcm(dsp_buffer[i], 2147483647.0);
                }
            } else if (format_ == SND_PCM_FORMAT_S24_LE) {
                // S24_LE (in 32-bit Container gepackt, je nach ALSA padding)
                int32_t* pcm24 = reinterpret_cast<int32_t*>(raw_buffer.data());
                for (int i = 0; i < total_samples; ++i) {
                    // 24 Bit im 32-Bit-Container (untere 24 Bit, vorzeichenrichtig)
                    pcm24[i] = floatToPcm(dsp_buffer[i], 8388607.0);
                }
            }
        }

        // Diagnose: Wie viele Frames liegen vor dem Schreiben in der DAC-Warteschlange?
        // Nur während der Wiedergabe messen: Beim Start (und beim Vorfüllen) wird die
        // Warteschlange erst aufgebaut und ist anfangs leer; das ist kein Problem und
        // würde sonst als "DAC-Puffer min 0.0 ms" und falsche Warnung erscheinen.
        if (primed && dac_buffer_frames > 0) {
            const snd_pcm_sframes_t avail = snd_pcm_avail_update(playback_handle_);
            if (avail >= 0 && static_cast<snd_pcm_uframes_t>(avail) <= dac_buffer_frames) {
                iv_dac_queue_min = std::min(iv_dac_queue_min,
                    static_cast<long>(dac_buffer_frames) - static_cast<long>(avail));
            }
        }

        // 4. Synchrones Schreiben auf den Hardware-DAC
        int frames_written = snd_pcm_writei(playback_handle_, raw_buffer.data(), period_frames_);

        if (frames_written < 0) {
            // Tritt meist auf, wenn das System überlastet ist und snd_pcm_writei zu spät aufgerufen wird
            ++dac_xruns;
            recover_from_xrun(frames_written);
        } else if (frames_written != (int)period_frames_) {
            ++short_writes;
            std::cerr << "Warnung: Short write auf DAC (" << frames_written 
                      << "/" << period_frames_ << ")" << std::endl;
        }

        // 5. Statuszeile (einmal pro Intervall)
        if (status_enabled) {
            const auto now = Clock::now();
            if (now - interval_start >= status_interval) {
                const size_t dropped_now = ring_buffer_.get_dropped_items();
                const size_t d_under = underrun_count - iv_underruns0;
                const size_t d_drop = dropped_now - iv_dropped0;
                const size_t d_xrun = dac_xruns - iv_xruns0;
                const size_t d_short = short_writes - iv_short0;

                std::vector<std::string> warnings;
                std::ostringstream line;
                line << std::fixed << std::setprecision(1);

                char stamp[32] = "";
                const std::time_t tt = std::time(nullptr);
                std::tm tmv{};
                if (localtime_r(&tt, &tmv)) std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tmv);
                line << "[Status " << stamp << "] ";

                if (iv_fill_n > 0) {
                    line << "Ring " << items_ms(iv_fill_sum / static_cast<double>(iv_fill_n))
                         << " ms (min " << fill_ms(iv_fill_min) << " / max " << fill_ms(iv_fill_max) << ")";
                } else {
                    line << "Ring: Wiedergabe pausiert/Vorfüllung";
                }

                double dac_queue_min_ms = -1.0;
                if (iv_dac_queue_min != LONG_MAX) {
                    dac_queue_min_ms = static_cast<double>(iv_dac_queue_min) * 1000.0 / sample_rate_;
                    line << " | DAC-Puffer min " << dac_queue_min_ms << " ms";
                }
                if (dsp_callback_) {
                    line << " | DSP max " << iv_dsp_max_ms << " ms";
                }
                line << " | Underruns " << underrun_count << " (+" << d_under << ")"
                     << " | verworfen " << dropped_now << " (+" << d_drop << ")"
                     << " | DAC-xruns " << dac_xruns << " (+" << d_xrun << ")";

                if (d_under > 0) warnings.push_back("Ring-Underrun im Intervall");
                if (d_drop > 0)  warnings.push_back("Capture verwirft Samples (Ring voll)");
                if (d_xrun > 0)  warnings.push_back("DAC-Underrun/Fehler (xrun)");
                if (d_short > 0) warnings.push_back("Short writes");
                if (dac_queue_min_ms >= 0.0 && iv_dsp_max_ms > 0.5 * dac_queue_min_ms) {
                    warnings.push_back("DSP-Spitze > 50% der DAC-Warteschlange");
                }

                // Drift-Schätzung aus dem mittleren Füllstand "sauberer" Intervalle
                const bool clean = (iv_fill_n > 0) && !iv_unprimed_seen && d_under == 0 && d_drop == 0;
                if (!clean) {
                    base_valid = false;
                } else {
                    const double avg_frames = iv_fill_sum / static_cast<double>(iv_fill_n) / channels_;
                    if (!base_valid) {
                        base_valid = true;
                        base_avg_frames = avg_frames;
                        base_time = now;
                    } else {
                        const double elapsed = std::chrono::duration<double>(now - base_time).count();
                        if (elapsed >= OAPW_TREND_MIN_SEC) {
                            const double ppm = (avg_frames - base_avg_frames) / elapsed / sample_rate_ * 1e6;
                            // Messgenauigkeit: eine Perioden-Stufe des Füllstands, verteilt auf die Messdauer
                            const double unc = static_cast<double>(period_frames_) / elapsed / sample_rate_ * 1e6;
                            line << " | Trend " << std::showpos << ppm << std::noshowpos
                                 << " ppm (±" << std::setprecision(0) << unc << std::setprecision(1) << ")";
                            if (std::fabs(ppm) <= unc) {
                                line << " stabil innerhalb der Messgenauigkeit";
                            } else {
                                const double rate_fps = std::fabs(ppm) * 1e-6 * sample_rate_;
                                double margin_frames;
                                const char* what;
                                if (ppm < 0) {
                                    margin_frames = avg_frames - static_cast<double>(period_frames_);
                                    what = "Underrun";
                                } else {
                                    margin_frames = (static_cast<double>(capacity - total_items) / channels_) - avg_frames;
                                    what = "Overrun";
                                }
                                const double minutes = std::max(0.0, margin_frames) / rate_fps / 60.0;
                                line << " (" << what << " in ca. ";
                                if (minutes > 1440.0) line << "> 24 h";
                                else line << std::setprecision(0) << minutes << " min" << std::setprecision(1);
                                if (elapsed < OAPW_TREND_WARN_SEC) line << ", vorläufig";
                                line << ")";
                                if (minutes < 60.0 && elapsed >= OAPW_TREND_WARN_SEC && std::fabs(ppm) > 2.0 * unc) {
                                    warnings.push_back("Takt-Drift: Ring läuft in < 1 h aus dem Zielbereich");
                                }
                            }
                        }
                    }
                }

                if (warnings.empty()) {
                    std::cout << line.str() << std::endl;
                } else {
                    line << " | WARNUNG:";
                    for (const auto& w : warnings) line << " " << w << ";";
                    std::cerr << line.str() << std::endl;
                }

                // Intervall zurücksetzen
                interval_start = now;
                iv_fill_min = SIZE_MAX; iv_fill_max = 0; iv_fill_sum = 0.0; iv_fill_n = 0;
                iv_dsp_max_ms = 0.0; iv_dac_queue_min = LONG_MAX; iv_unprimed_seen = false;
                iv_underruns0 = underrun_count; iv_dropped0 = dropped_now;
                iv_xruns0 = dac_xruns; iv_short0 = short_writes;
            }
        }
    }
}
