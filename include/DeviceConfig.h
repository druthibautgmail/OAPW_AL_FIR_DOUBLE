#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "InputValidation.h"

// Geraete-Konfiguration (ALSA-Eingang, DAC, Samplerate, Format).
//
// Rangfolge (spaeter gewinnt):
//   1. Eingebaute Defaults (bisheriges Verhalten: Loopback -> hw:Katana,0 @ 192 kHz)
//   2. Datei oapw_device.txt (oder --device-file <pfad>)
//   3. Kommandozeile
//
// Datei-Format (eine Zeile pro Eintrag, '#' beginnt einen Kommentar):
//   capture=hw:Loopback,1,0
//   playback=hw:Katana,0
//   rate=192000
//   format=S32_LE            # Format zum DAC
//   capture_format=S32_LE    # Format vom Loopback
//
// Kommandozeile:
//   --playback hw:BossDAC,0   (Alias: --dac)
//   --capture  hw:Loopback,1,0
//   --rate 192000
//   --format S32_LE
//   --capture-format S32_LE
//   --device-file /pfad/oapw_device.txt
//   --list-devices            Karten und Geraete auflisten und beenden
//   --help
namespace DeviceConfigNS {

struct DeviceConfig {
    std::string capture        = "hw:Loopback,1,0";
    std::string playback       = "hw:Katana,0";
    unsigned int rate          = 192000;
    std::string captureFormat  = "S32_LE";
    std::string playbackFormat = "S32_LE";
};

enum class ParseResult { Run, ExitOk, ExitError };

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Nur Formate, fuer die AlsaCapture/AlsaPlayback eine Umwandlung haben.
// (S24_3LE, also 24 Bit gepackt zu 3 Byte, ist NICHT implementiert.)
inline bool parseFormat(const std::string& text, snd_pcm_format_t& out) {
    std::string t = trim(text);
    std::transform(t.begin(), t.end(), t.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (t == "S16_LE") { out = SND_PCM_FORMAT_S16_LE; return true; }
    if (t == "S24_LE") { out = SND_PCM_FORMAT_S24_LE; return true; }
    if (t == "S32_LE") { out = SND_PCM_FORMAT_S32_LE; return true; }
    return false;
}

// Setzt einen Eintrag. Rueckgabe false = unbekannter Schluessel oder ungueltiger Wert.
inline bool applyEntry(DeviceConfig& cfg, const std::string& key, const std::string& rawValue,
                       std::string& error) {
    const std::string value = trim(rawValue);
    snd_pcm_format_t dummy;
    if (key == "capture") {
        if (value.empty()) { error = "capture: leerer Wert"; return false; }
        cfg.capture = value;
    } else if (key == "playback" || key == "dac") {
        if (value.empty()) { error = "playback: leerer Wert"; return false; }
        cfg.playback = value;
    } else if (key == "rate") {
        double r = 0.0;
        if (!InputValidation::parseDouble(value, r) || r < 8000.0 || r > 768000.0 ||
            r != static_cast<double>(static_cast<unsigned int>(r))) {
            error = "rate: ungueltig (ganze Zahl zwischen 8000 und 768000): " + value;
            return false;
        }
        cfg.rate = static_cast<unsigned int>(r);
    } else if (key == "format") {
        if (!parseFormat(value, dummy)) {
            error = "format: unbekannt oder nicht unterstuetzt (erlaubt: S16_LE, S24_LE, S32_LE): " + value;
            return false;
        }
        cfg.playbackFormat = value;
    } else if (key == "capture_format" || key == "capture-format") {
        if (!parseFormat(value, dummy)) {
            error = "capture_format: unbekannt oder nicht unterstuetzt (erlaubt: S16_LE, S24_LE, S32_LE): " + value;
            return false;
        }
        cfg.captureFormat = value;
    } else {
        error = "unbekannter Schluessel: " + key;
        return false;
    }
    return true;
}

// Liest die Datei. Fehlt sie, bleiben die Defaults (kein Fehler).
// Rueckgabe false nur bei ungueltigem Inhalt (dann bricht der Start ab: lieber laut als
// still mit einem falschen Geraet zu spielen).
inline bool loadDeviceFile(DeviceConfig& cfg, const std::string& path, bool mustExist) {
    std::ifstream file(path);
    if (!file.is_open()) {
        if (mustExist) {
            std::cerr << "[Geraet] Datei nicht gefunden: " << path << std::endl;
            return false;
        }
        std::cout << "[Geraet] Keine " << path << " gefunden, nutze Defaults." << std::endl;
        return true;
    }
    std::string line;
    int lineNo = 0;
    bool ok = true;
    while (std::getline(file, line)) {
        ++lineNo;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            std::cerr << "[Geraet] " << path << ":" << lineNo << ": Zeile ohne '=': " << line << std::endl;
            ok = false;
            continue;
        }
        std::string err;
        if (!applyEntry(cfg, trim(line.substr(0, eq)), line.substr(eq + 1), err)) {
            std::cerr << "[Geraet] " << path << ":" << lineNo << ": " << err << std::endl;
            ok = false;
        }
    }
    if (ok) std::cout << "[Geraet] Einstellungen aus " << path << " geladen." << std::endl;
    return ok;
}

// Listet Karten und PCM-Geraete mit den Namen auf, die man bei playback=/capture= braucht.
inline void listDevices() {
    std::cout << "ALSA-Karten und Geraete (Namen fuer --playback / --capture):\n";
    int card = -1;
    bool any = false;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char hw[32];
        std::snprintf(hw, sizeof(hw), "hw:%d", card);
        snd_ctl_t* ctl = nullptr;
        if (snd_ctl_open(&ctl, hw, 0) < 0) continue;

        snd_ctl_card_info_t* cinfo;
        snd_ctl_card_info_alloca(&cinfo);
        std::string id = std::to_string(card), name;
        if (snd_ctl_card_info(ctl, cinfo) >= 0) {
            id = snd_ctl_card_info_get_id(cinfo);
            name = snd_ctl_card_info_get_name(cinfo);
        }
        std::cout << "\nKarte " << card << ": " << id << " (" << name << ")\n";

        int dev = -1;
        while (snd_ctl_pcm_next_device(ctl, &dev) == 0 && dev >= 0) {
            for (int s = 0; s < 2; ++s) {
                snd_pcm_info_t* pinfo;
                snd_pcm_info_alloca(&pinfo);
                snd_pcm_info_set_device(pinfo, static_cast<unsigned int>(dev));
                snd_pcm_info_set_subdevice(pinfo, 0);
                snd_pcm_info_set_stream(pinfo, s == 0 ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE);
                if (snd_ctl_pcm_info(ctl, pinfo) < 0) continue;
                std::cout << "  " << (s == 0 ? "Wiedergabe: " : "Aufnahme:   ")
                          << "hw:" << id << "," << dev << "   [" << snd_pcm_info_get_name(pinfo) << "]\n";
                any = true;
            }
        }
        snd_ctl_close(ctl);
    }
    if (!any) std::cout << "(keine Geraete gefunden)\n";
    std::cout << std::flush;
}

inline void printHelp(const char* prog) {
    std::cout <<
        "Aufruf: " << prog << " [Optionen]\n"
        "  --playback <dev>        DAC, z.B. hw:Katana,0 (Alias --dac)\n"
        "  --capture <dev>         Eingang, z.B. hw:Loopback,1,0\n"
        "  --rate <Hz>             Samplerate (getestet: nur 192000)\n"
        "  --format <fmt>          DAC-Format: S16_LE, S24_LE, S32_LE\n"
        "  --capture-format <fmt>  Eingangs-Format (wie oben)\n"
        "  --device-file <pfad>    Geraetedatei (Default: oapw_device.txt)\n"
        "  --list-devices          Karten/Geraete auflisten und beenden\n"
        "  --help                  Diese Hilfe\n"
        "Rangfolge: Defaults < Geraetedatei < Kommandozeile.\n";
}

// Wertet Datei und Kommandozeile aus.
inline ParseResult parseArgs(int argc, char** argv, DeviceConfig& cfg) {
    // 1. Vorlauf: Optionen ohne Konfigurationswirkung und Pfad der Geraetedatei
    std::string devFile = "oapw_device.txt";
    bool devFileGiven = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { printHelp(argv[0]); return ParseResult::ExitOk; }
        if (a == "--list-devices")      { listDevices();      return ParseResult::ExitOk; }
        if (a == "--device-file" && i + 1 < argc) { devFile = argv[++i]; devFileGiven = true; }
        else if (a.rfind("--device-file=", 0) == 0) { devFile = a.substr(14); devFileGiven = true; }
    }

    // 2. Datei
    if (!loadDeviceFile(cfg, devFile, devFileGiven)) return ParseResult::ExitError;

    // 3. Kommandozeile ueberschreibt
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--", 0) != 0) {
            std::cerr << "[Geraet] Unbekanntes Argument: " << a << " (siehe --help)" << std::endl;
            return ParseResult::ExitError;
        }
        a = a.substr(2);
        std::string key, value;
        const size_t eq = a.find('=');
        if (eq != std::string::npos) {
            key = a.substr(0, eq);
            value = a.substr(eq + 1);
        } else {
            key = a;
            if (key == "help" || key == "list-devices" || key == "device-file") {
                if (key == "device-file") ++i; // bereits im Vorlauf behandelt
                continue;
            }
            if (i + 1 >= argc) {
                std::cerr << "[Geraet] Option --" << key << " braucht einen Wert." << std::endl;
                return ParseResult::ExitError;
            }
            value = argv[++i];
        }
        if (key == "device-file") continue;
        std::string err;
        if (!applyEntry(cfg, key, value, err)) {
            std::cerr << "[Geraet] --" << key << ": " << err << std::endl;
            return ParseResult::ExitError;
        }
    }
    return ParseResult::Run;
}

// Prueft VOR dem Start, ob das Geraet Format, Kanalzahl und Samplerate wirklich kann, und
// meldet andernfalls, was es stattdessen kann. Die Pruefung belegt das Geraet nur kurz.
inline bool probeDevice(const std::string& device, snd_pcm_stream_t stream,
                        snd_pcm_format_t format, unsigned int rate, unsigned int channels,
                        const char* label) {
    snd_pcm_t* pcm = nullptr;
    int err = snd_pcm_open(&pcm, device.c_str(), stream, SND_PCM_NONBLOCK);
    if (err < 0) {
        std::cerr << "[Geraet] " << label << " " << device << " laesst sich nicht oeffnen: "
                  << snd_strerror(err) << "\n         (Namen anzeigen mit --list-devices; "
                  << "-16 = Geraet belegt, -2 = Name falsch)" << std::endl;
        return false;
    }
    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);

    bool ok = true;

    if (snd_pcm_hw_params_test_format(pcm, hw, format) < 0) {
        ok = false;
        std::cerr << "[Geraet] " << label << " " << device << " unterstuetzt "
                  << snd_pcm_format_name(format) << " nicht. Verfuegbar:";
        for (int f = 0; f <= static_cast<int>(SND_PCM_FORMAT_LAST); ++f) {
            const snd_pcm_format_t fmt = static_cast<snd_pcm_format_t>(f);
            if (snd_pcm_hw_params_test_format(pcm, hw, fmt) == 0) {
                const char* n = snd_pcm_format_name(fmt);
                if (n) std::cerr << " " << n;
            }
        }
        std::cerr << "\n         Umsetzbar sind nur S16_LE, S24_LE, S32_LE." << std::endl;
    }

    if (snd_pcm_hw_params_test_channels(pcm, hw, channels) < 0) {
        ok = false;
        unsigned int cmin = 0, cmax = 0;
        snd_pcm_hw_params_get_channels_min(hw, &cmin);
        snd_pcm_hw_params_get_channels_max(hw, &cmax);
        std::cerr << "[Geraet] " << label << " " << device << " unterstuetzt " << channels
                  << " Kanaele nicht (verfuegbar " << cmin << ".." << cmax << ")." << std::endl;
    }

    if (snd_pcm_hw_params_test_rate(pcm, hw, rate, 0) < 0) {
        ok = false;
        std::cerr << "[Geraet] " << label << " " << device << " unterstuetzt " << rate
                  << " Hz nicht. Verfuegbar von den ueblichen Raten:";
        static const unsigned int common[] = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};
        for (unsigned int r : common) {
            if (snd_pcm_hw_params_test_rate(pcm, hw, r, 0) == 0) std::cerr << " " << r;
        }
        std::cerr << std::endl;
    }

    snd_pcm_close(pcm);
    if (ok) {
        std::cout << "[Geraet] " << label << " " << device << ": " << snd_pcm_format_name(format)
                  << ", " << channels << " Kanaele, " << rate << " Hz  OK" << std::endl;
    }
    return ok;
}

} // namespace DeviceConfigNS

#endif // DEVICE_CONFIG_H
