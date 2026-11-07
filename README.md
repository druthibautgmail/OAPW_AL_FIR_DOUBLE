# OAPW_HiFi_AL\_DOUBLE

## Open Ambiophonics Processing Wrapper – ALSA Loopback Edition mit Fast Fourier Transformation und Finite Response Filtern

**Version 21.0**

**OAPW_HiFi_AL_DOUBLE** ist eine für den Raspberry Pi optimierte Echtzeit-Audiosignalverarbeitung in C++. Die Software implementiert den *Recursive Ambiophonic Crosstalk Elimination* (RACE)-Algorithmus, basierend auf den wegweisenden Arbeiten von Ralph Glasgal und dem Ambiophonics Institute.

Ziel ist die Wiedergabe einer außergewöhnlich breiten, räumlich präzisen und holografisch wirkenden Stereobühne über zwei Lautsprecher – insbesondere in Setups mit relativ geringem Lautsprecherabstand von ca. 10-20° und damit abweichend von der "Goldenen Stereo-Regel" einer ca. 60°-Aufstellung.

Die ALSA-Loopback-Edition verwendet ein virtuelles ALSA-Audiogerät als Verbindung zwischen Audio-Player und DSP-Engine. Dadurch kann das Audiosignal systemintern abgegriffen, verlustfrei verarbeitet und anschließend nativ an den Hardware-DAC ausgegeben werden.

### Kerntechnologie: FFT-FIR in 64-Bit Double Precision & Overlap-Save

Die FFT-FIR-Variante nutzt die extrem schnelle C-Bibliothek `libfftw3` (Double Precision, Overlap-Save-Verfahren), um den verfügbaren Headroom (= Dynamik-Umfang) bei vertretbarer CPU-Mehrbelastung gegenüber der 32-Bit Float Variante zu vergrößern.

* **Phasenlinearer 3-Band-Crossover:** Das Eingangssignal wird in Tiefen, Mitten und Höhen aufgeteilt (FIR, Blackman-Harris-gefensterter Sinc, 32767 Taps). Alle drei Zweige haben dieselbe Laufzeit (16383 Samples). Das Mittenband ist als Rest gebildet (Impuls minus Tiefen minus Höhen). Dadurch ergeben Tiefen, Mitten und Höhen bei abgeschaltetem RACE wieder exakt das Eingangssignal, und es gibt keine Phasenverschiebung an den Trennfrequenzen. Die Standard-Trennfrequenzen liegen bei ca. 400 Hz und 4530 Hz, dem Bereich, den die Literatur (Glasgal) für die RACE-Bearbeitung nennt.
* **Echte Rekursion im RACE-Algorithmus (neu in V21):** Der verzögerte *Ausgang* der Gegenseite wird zurückgeführt (nicht mehr der Eingang wie in der früheren Feed-Forward-Struktur):
  `out_L[n] = in_L[n] − a/(1+p) · (out_R[n−d] − p · out_L[n−d])`, und symmetrisch für R.
  Dabei ist `d` die Laufzeit (Delay), `a` die Übersprechdämpfung als linearer Faktor und `p` der Center-Parameter. Diese Struktur entspricht dem rekursiven Aufbau des Originals. Mit der Standardeinstellung (68 µs, −2,3 dB, p = 0,1) ergibt sich bei tiefen Frequenzen für das Summensignal eine Absenkung von ca. 4,2 dB und für das Differenzsignal eine Anhebung von ca. 12,7 dB. Bei eingeschaltetem Low-Bypass wirkt das nur im Mittenband.
* **Stabilitätsgrenzen (V21):** Eine Rückkopplung kann nur stabil sein, wenn `a < 1` gilt. Die Engine begrenzt `a` deshalb auf höchstens 0,95, `p` auf 0…1 und das Delay auf mindestens 1 Sample. Ungültige Werte (NaN, Inf) werden verworfen. Gerät die Rückkopplung doch einmal aus dem Takt, wird der Zustand geleert, statt dauerhaft zu kreisen.
* **Phasenlinearer 3-Band-Equalizer:** Die parametrischen EQ-Kurven werden rein als reelle Gain-Faktoren auf die komplexe FFT-Matrix multipliziert. Dies verhindert jegliche Phasenverschiebungen und garantiert eine perfekte Impulstreue (z.B. bei Transienten wie harten Klavieranschlägen, Percussion-Peaks und dem Anzupfen von Saiten-Instrumenten).
* **192 kHz / 64-Bit Double Pipeline:** Der DSP-Kern rechnet durchgehend mit 64-Bit Double bei 192 kHz. Die Umwandlung zurück ins PCM-Format des DAC erfolgt mit Rundung und Sättigung (kein Überlauf, NaN wird zu Stille).
* **Latenz:** Der FIR-Crossover arbeitet blockweise (32768 Samples). Aus Filterlaufzeit und Blockbildung ergeben sich ca. 256 ms im DSP, dazu kommen Ringpuffer und DAC-Puffer. Die Gesamtlatenz liegt grob im Bereich von einer halben Sekunde (Schätzung). Der Player eignet sich damit für Musikwiedergabe, nicht für Videosynchronisation.

*Cave!* Bei schnellen Änderungen der Inputs über die Regler im Web-GUI kann es gelegentlich zu kleinen Störungen des Audio-Signals kommen, die jedoch innerhalb weniger Millisekunden wieder abklingen. Dies ist dem Algorithmus geschuldet, der den Signalfluss für die Neuberechnung der Arrays kurzzeitig unterbrechen muss. Die CPU-Last liegt bei korrekter Anwendung der Compiler-Flags (siehe 2.2) auf einem Raspberry Pi 5 trotz der extrem hohen Rechenintensität bei vertretbaren ca. 6 %.

Ist die Konfiguration für das jeweilige Setup einmal optimiert, können die zugehörigen Parameter in einer `oapw_config.txt` gespeichert werden. Sie werden beim nächsten Start automatisch eingelesen (siehe 4.2).

---

## Neu in Version 21

* **Rekursives RACE** (siehe oben) mit Stabilitätsgrenzen.
* **Sicherer Lautstärke-Start:** Standardlautstärke 0,10 (ca. −20 dB). Eine gespeicherte Lautstärke wird beim Laden auf höchstens 0,25 (ca. −12 dB) begrenzt. Der Ausgang startet immer bei Stille und blendet die Lautstärke höchstens um 0,5 pro Sekunde auf (Vollaussteuerung in 2 s), Absenken wirkt innerhalb von ca. 50 ms. Das gilt auch für den Web-Regler. Der Makeup-Gain ist auf +6 dB begrenzt.
* **Geräte-Konfiguration ohne Neukompilieren:** DAC, Eingang, Samplerate und Format stehen in `oapw_device.txt` bzw. auf der Kommandozeile (siehe 3).
* **Vorabprüfung der Geräte:** Beim Start wird geprüft, ob Eingang und DAC Format, Kanalzahl und Samplerate wirklich unterstützen. Andernfalls bricht der Player mit einer klaren Meldung ab und nennt, was das Gerät stattdessen kann.
* **Diagnose-Statuszeile alle 60 Sekunden** (siehe 7).
* **Robusterer Datenweg:** Der Ringpuffer zwischen Capture und Playback ist ein lock-free Single-Producer/Single-Consumer-Puffer. Vor der Wiedergabe wird ein Polster aufgebaut. Bei Unterversorgung gibt der Player Stille aus statt Datenreste.
* **Eingabeprüfung:** Ungültige Zahlen in der Config-Datei oder in der Web-API (z. B. `abc`, `nan`, `12abc`) werden abgelehnt. Die Config meldet sie im Log, die API antwortet mit HTTP 400 und übernimmt in dem Fall keinen der gesendeten Werte.
* **Aufnahme:** Die Aufnahme (WAV/FLAC) funktioniert jetzt auch bei abgeschaltetem DSP (Bypass).

---

## 1. Systemanforderungen und Hardware

### Rechner

* **Raspberry Pi 5:** Empfohlene Referenz-Plattform für anspruchsvolle DSP-Verarbeitung mit nativen 192 kHz.
* **Raspberry Pi 4:** Alternative Plattform; die erreichbare maximale Abtastrate hängt von Konfiguration und Systemlast ab.
* **Raspberry Pi Compute Module:** Ebenfalls als Basis für dedizierte Audio-Systeme geeignet.

Der Raspberry Pi 3 wurde mit dieser Programmversion noch nicht abschließend auf seine Leistungsfähigkeit getestet, ist aber vermutlich wegen der hohen Rechenlast der 192 kHz / 32 Bit Verarbeitung, die intern mit 64-Bit DOUBLE Variablen, Vektoren und Arrays arbeitet, nicht geeignet.

### Audio-Hardware

Erforderlich ist ein hochwertiger, nativer 192-kHz-DAC (bevorzugt I²S-DAC-HAT), um das Phasenpotenzial des FIR-Algorithmus voll auszuschöpfen.

Mögliche Optionen sind:

* **Allo Katana** (Referenz-DAC)
* **Allo Boss DAC**
* **DAC HAT Pro**

*(Hinweis: Einfache USB-DACs, die auf 96 kHz limitiert sind (z. B. Sharkoon Gaming DAC), taugen nicht für diesen High-End-Anspruch und erzwingen ein verlustbehaftetes Downsampling über ALSA. Sie werden für die FIR-Edition ausdrücklich nicht empfohlen.)*

Der ALSA-Gerätename des DAC wird in der Datei `oapw_device.txt` eingetragen (siehe Abschnitt 3). Ein Eingriff in den Quelltext ist dafür nicht mehr nötig.

### Stromversorgung

Für einen hochwertigen Audioaufbau ist eine sorgfältig ausgelegte, rauscharme Stromversorgung zwingend erforderlich (z. B. LPS - Linear Power Supply). Ein separates lineares Netzteil für den DAC reduziert Störeinflüsse massiv. Eine Unterspannung am Pi führt zu Drosselung der CPU und kann Aussetzer verursachen (prüfen mit `vcgencmd get_throttled`).

### WLAN und Bluetooth

WLAN und Bluetooth können bei einem dedizierten Audio-System deaktiviert werden, um Funkaktivität und unnötige Systemlast zu reduzieren.

---

## 2. Software-Voraussetzungen

Vorausgesetzt wird ein aktuelles 64-Bit-Debian-System, beispielsweise:

* Debian Trixie / Bookworm
* Raspberry Pi OS 64-Bit

Benötigte Komponenten:

* GCC/G++ mit C++17-Unterstützung
* GNU Make
* ALSA-Entwicklerbibliotheken (`libasound2-dev`)
* FFTW3-Bibliotheken (`libfftw3-dev`)
* libsndfile (`libsndfile1-dev`, für die Aufnahmefunktion)
* Linux-Kernelmodul `snd-aloop`

### 2.1 Pakete installieren

```bash
sudo apt-get update
sudo apt-get install build-essential libasound2-dev libfftw3-dev libsndfile1-dev

```

### 2.2 Repository herunterladen & Kompilieren

Das Repository klonen:

```bash
git clone https://github.com/druthibautgmail/OAPW_AL_FIR_DOUBLE.git OAPW
cd OAPW

```

Das Programm kompilieren (nutzt alle verfügbaren Kerne für einen schnellen Build):

```bash
make -j4

```

**Wichtig zu den Compiler-Flags:**

* `-ffast-math` schaltet implizit `-ffinite-math-only` ein. Dann entfernt der Compiler alle NaN/Inf-Prüfungen, und der Schutz der Rückkopplung wäre wirkungslos. Deshalb muss **`-fno-finite-math-only`** zusätzlich angegeben werden (empfohlen: `-O3 -ffast-math -fno-finite-math-only -flto -fopenmp`).
* Mit `-mcpu=native` entsteht Code für genau die CPU, auf der kompiliert wurde. Ein Pi-5-Build läuft nicht zwingend auf einem Pi 4. Deshalb auf jedem Rechner selbst kompilieren.
* Nach dem Austausch von Dateien (besonders von Header-Dateien) immer `make clean && make -j4` ausführen, sonst können veraltete Objektdateien zu Fehlern führen.

Den Player für einen ersten Test manuell starten (zuerst bei ausgeschalteten Endstufen):

```bash
./OAPW_Player

```

### 2.3 WLAN und Bluetooth optional deaktivieren

Einträge in `/boot/firmware/config.txt` ergänzen:

```ini
dtoverlay=disable-wifi
dtoverlay=disable-bt

```

*(Bei älteren Installationen liegt die Datei unter `/boot/config.txt`.)*

### 2.4 ALSA-Loopback aktivieren

Das Kernelmodul `snd-aloop` stellt das virtuelle Audiokabel bereit.

Modul laden und Autostart aktivieren:

```bash
sudo modprobe snd-aloop
echo snd-aloop | sudo tee -a /etc/modules

```

Prüfen, ob das Modul geladen wurde:

```bash
lsmod | grep snd_aloop
aplay -l

```

---

## 3. Geräte-Konfiguration (`oapw_device.txt` und Kommandozeile)

Die Hardware wird nicht mehr im Quelltext festgelegt. Bei jedem Start gilt die Rangfolge:

1. eingebaute Defaults (`hw:Loopback,1,0` → `hw:Katana,0`, 192000 Hz, S32_LE)
2. die Datei `oapw_device.txt` im Arbeitsverzeichnis
3. Optionen auf der Kommandozeile

Neue Programmversionen können deshalb einfach über die alten kopiert werden. `oapw_device.txt` und `oapw_config.txt` bleiben unberührt.

### 3.1 Welche Geräte gibt es?

```bash
./OAPW_Player --list-devices

```

Die Ausgabe zeigt Karten mit ihren Namen, z. B. `Wiedergabe: hw:BossDAC,0`. Es sollte der **Kartenname** (nach `hw:`) verwendet werden, nicht die Kartennummer, weil sich die Nummer nach einem Neustart ändern kann.

### 3.2 Die Datei `oapw_device.txt`

```ini
# Zeilen mit '#' sind Kommentare
capture=hw:Loopback,1,0
playback=hw:Katana,0
rate=192000
format=S32_LE            # Format zum DAC
capture_format=S32_LE    # Format vom Loopback
```

### 3.3 Kommandozeilen-Optionen

| Option | Bedeutung |
|---|---|
| `--playback <dev>` (Alias `--dac`) | DAC, z. B. `hw:BossDAC,0` |
| `--capture <dev>` | Eingang, z. B. `hw:Loopback,1,0` |
| `--rate <Hz>` | Samplerate (getestet ist bisher nur 192000) |
| `--format <fmt>` | DAC-Format: `S16_LE`, `S24_LE`, `S32_LE` |
| `--capture-format <fmt>` | Eingangsformat (wie oben) |
| `--device-file <pfad>` | andere Geräte-Datei statt `oapw_device.txt` |
| `--list-devices` | Karten und Geräte auflisten und beenden |
| `--help` | Hilfe |

### 3.4 Vorabprüfung und Fehlermeldungen

* Vor dem Start prüft der Player Format, 2 Kanäle und Samplerate an beiden Geräten. Passt etwas nicht, bricht er ab und nennt, was das Gerät stattdessen unterstützt. Das ist beabsichtigt, damit nicht still mit umgebogenen Parametern gespielt wird.
* Unbekannte Schlüssel oder ungültige Werte in der Datei oder auf der Kommandozeile führen ebenfalls zum Abbruch mit Zeilenangabe.
* Fehlercode `-16` beim Öffnen bedeutet "Gerät belegt" (z. B. läuft noch ein anderer Prozess oder PipeWire), `-2` bedeutet "Name falsch".
* Unterstützt sind nur `S16_LE`, `S24_LE` und `S32_LE`. 24 Bit gepackt zu 3 Byte (`S24_3LE`, bei manchen USB-DACs) ist **nicht** implementiert.
* Bei einer anderen Samplerate als 192000 Hz erscheint ein Hinweis: Filterlängen und Latenz sind auf 192 kHz ausgelegt, andere Raten sind nicht getestet.

---

## 4. Signalfluss, Web-GUI und Konfiguration

### 4.1 Signalfluss – ALSA Loopback Pipeline (192 kHz / 32-Bit [intern: 64-Bit])

Der Signalweg verbindet den Audio-Player bitperfekt mit der OAPW-DSP-Engine.

```text
┌───────────────────────────────┐
│ Audio-Player (SOXR aktiv)     │
│ z. B. MPD oder myMPD          │
└───────────────┬───────────────┘
                │ 192 kHz
                ▼
┌───────────────────────────────┐
│ ALSA Loopback                 │
│ Playback: hw:Loopback,0,0     │
└───────────────┬───────────────┘
                │ Virtuelles Audiokabel
                ▼
┌───────────────────────────────┐
│ OAPW_HiFi_AL  (C++ Engine)    │
│ Capture: hw:Loopback,1,0      │
│                               │
│ • PCM -> 64Bit Conversion     │
│ • Lock-free Ringpuffer        │
│ • 3-Band FIR-Crossover (FFT)  │
│ • Phasenlinearer FIR-EQ       │
│ • RACE (rekursiv, Mittenband) │
│ • Lautstärke mit Rampe        │
│ • 64Bit -> PCM (S32_LE)       │
└───────────────┬───────────────┘
                │ 192 kHz / S32_LE
                ▼
┌───────────────────────────────┐
│ Hardware-DAC                  │
│ z. B. Allo Katana (hw:Katana) │
└───────────────┬───────────────┘
                │
                ▼
┌───────────────────────────────┐
│ Verstärker und Lautsprecher   │
└───────────────────────────────┘

```

### 4.2 Web-GUI

Die Engine startet standardmäßig einen Web-Server auf Port 8080.
Aufruf im Browser:

```text
http://<IP-Adresse-des-Raspberry-Pi>:8080

```

**Features der Web-GUI:**

* **Real-Time Analyzer:** Visualisierung des Echtzeit-Spektrums mit exaktem 192-kHz-Frequenz-Mapping und kalibrierter logarithmischer Dezibel-Skala.
* **Geometrie-Rechner:** Direkte Berechnung von Laufzeit (Delay) und Übersprechdämpfung (Attenuation) basierend auf dem realen Lautsprecher- und Hörerabstand. Dieses Feature befindet sich noch im Beta-Stadium. Bewährte Startwerte sind 68µs Delay und -2.3dB Attenuation.
* **DSP Parameter:** Steuerung der RACE-Matrix, Crossover-Frequenzen, des linearen FIR-Equalizers und des globalen Makeup-Gains (0 bis +6 dB).
* **Low-Pass ON/OFF-Switch:** Stellt den Low-Bypass für den Bass-Frequenzbereich auf 'An' bzw. 'Aus'. Bei 'An' wird der Bassanteil unbearbeitet am RACE-Algorithmus vorbeigeführt, bei 'Aus' wird das gesamte Frequenzspektrum bis zur oberen Trennfrequenz von RACE verarbeitet.

**Sicherheitshinweis:** Der Web-Server lauscht auf allen Netzwerkschnittstellen und hat keine Anmeldung. Er gehört nur in ein vertrauenswürdiges Heimnetz und darf nicht aus dem Internet erreichbar sein.

### 4.3 Die Datei `oapw_config.txt`

Wird im Web-GUI gespeichert und beim Start gelesen. Schlüssel (je eine Zeile `name=wert`):

| Schlüssel | Bedeutung |
|---|---|
| `vol` | Lautstärke 0…1 (beim Laden auf höchstens 0,25 begrenzt) |
| `makeupGain` | Makeup-Gain in dB (höchstens +6) |
| `delay` | RACE-Laufzeit in µs |
| `att` | Übersprechdämpfung in dB |
| `center` | Center-Parameter p (0…1) |
| `freqLow`, `freqHigh` | untere und obere Trennfrequenz in Hz |
| `race`, `filters`, `eqActive`, `lowBypass` | Schalter (0 oder 1) |
| `eq0f`, `eq0q`, `eq0g` … `eq2g` | Frequenz, Güte und Gain der drei EQ-Bänder |

Ungültige Zeilen werden im Log gemeldet und ignoriert. Die Dämpfung `att` ist intern so begrenzt, dass der lineare Faktor unter 0,95 bleibt.

---

## 5. Automatischer Start als Hintergrunddienst (systemd)

Damit `OAPW_Player` nach einem Neustart des Raspberry Pi automatisch hochfährt, wird er als `systemd`-Dienst eingerichtet.

### 5.1 Dienst-Datei erstellen

```bash
sudo nano /etc/systemd/system/oapw.service

```

Füge folgenden Block ein (Pfade und User anpassen!):

```ini
[Unit]
Description=OAPW HiFi ALSA Loopback Player with Fast Fourier Transform / Feed Forward Finite Response Filter Design and DOUBLE Precision Math
After=network.target sound.target

[Service]
Type=simple
User=<Dein Username>
WorkingDirectory=/home/<Dein Username>/OAPW
ExecStart=/home/<Dein Username>/OAPW/OAPW_Player
StandardOutput=journal
StandardError=journal
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target

```

Das `WorkingDirectory` ist wichtig: `oapw_device.txt` und `oapw_config.txt` werden relativ zu diesem Verzeichnis gesucht. Optionen aus Abschnitt 3.3 können bei Bedarf hinter `ExecStart=…/OAPW_Player` angehängt werden.

### 5.2 Dienst aktivieren und verwalten

Dienst laden und starten:

```bash
sudo systemctl daemon-reload
sudo systemctl enable oapw.service
sudo systemctl start oapw.service

```

Logs zur Fehlerdiagnose (inkl. Statuszeilen, XRUNs und Clipping-Warnungen) abfragen:

```bash
sudo journalctl -u oapw.service -f

```

---

## 6. Hinweise zur Audioqualität und Performance

Die C++ Engine ist für kompromisslose Audioqualität bei 192 kHz ausgelegt. Wichtige Faktoren für den stabilen Betrieb:

* Pipewire und Wireplumber sollten systemweit deaktiviert/maskiert werden, um exklusiven ALSA-Zugriff zu garantieren.
* Der Zuspieler (z. B. MPD) muss das Quellmaterial zwingend vor der Übergabe an den Loopback resampeln (z. B. via SOXR), damit der DSP mit einem konstanten 192-kHz-Takt gefüttert wird.
* Thermisches Management: Der Raspberry Pi 5 muss ausreichend gekühlt werden, um Throttling zu vermeiden, da Einbrüche in der Taktrate zu Puffer-Underruns (`-EPIPE`) auf ALSA-Ebene führen können.
* Takt-Drift: Der virtuelle Loopback läuft mit dem Systemtakt, der DAC mit seinem eigenen Quarz. Beide weichen minimal voneinander ab (im Bereich weniger ppm), der Ringpuffer fängt das ab. Die Statuszeile (Abschnitt 7) zeigt den Trend. Ein Taktausgleich im Programm ist noch nicht eingebaut.

---

## 7. Diagnose: Statuszeile im Log

Alle 60 Sekunden schreibt der Player eine Zeile, z. B.:

```text
[Status 11:25:07] Ring 87.3 ms (min 80.0 / max 101.3) | DAC-Puffer min 325.3 ms | DSP max 17.0 ms | Underruns 0 (+0) | verworfen 0 (+0) | DAC-xruns 0 (+0) | Trend -0.2 ppm (±0) stabil innerhalb der Messgenauigkeit
```

| Feld | Bedeutung |
|---|---|
| `Ring` | Füllstand des Ringpuffers im Intervall (Mittel, min/max) |
| `DAC-Puffer min` | kleinster Füllstand der DAC-Warteschlange (nur während der Wiedergabe gemessen) |
| `DSP max` | längste Rechenzeit eines DSP-Aufrufs im Intervall |
| `Underruns` | Anzahl Unterversorgungen der Wiedergabe (in Klammern: neu seit der letzten Zeile) |
| `verworfen` | Anzahl vom Capture verworfener Samples bei vollem Ringpuffer |
| `DAC-xruns` | vom DAC gemeldete Unterläufe |
| `Trend` | geschätzte Taktabweichung zwischen Loopback und DAC in ppm mit Unsicherheit, ab ca. 3 Minuten Messdauer |

Beim Start steht außerdem eine Zeile `ALSA Playback: Periode … Frames, DAC-Puffer … Frames`. Sie zeigt, was der Treiber tatsächlich ausgehandelt hat.

Warnungen (`WARNUNG`, auf stderr) erscheinen z. B., wenn die DSP-Spitze mehr als die Hälfte der DAC-Warteschlange beansprucht oder wenn der Trend nach ca. 10 Minuten eindeutig von Null abweicht. Das Intervall lässt sich beim Kompilieren über das Makro `OAPW_STATUS_INTERVAL_SEC` ändern.

---

## 8. Bekannte Einschränkungen

* Reglerbewegungen im Web-GUI können kurze Störungen verursachen (siehe *Cave!* oben).
* Andere Samplerate als 192 kHz ist nicht getestet.
* Kein Taktausgleich zwischen Loopback und DAC (nur Beobachtung der Drift).
* Das Web-Interface hat keine Authentifizierung (siehe 4.2).

---

## 9. Danksagung

Mein ganz besonderer Dank gilt **Prof. Dr. Dieter Steinhilber**, dessen Geduld bei der Hör-Testung immer neuer Releases dieser Software außerordentlich groß war, dessen wertvolle Hinweise, technische Anregungen und tiefgreifendes Verständnis für High-End Audiophilie maßgeblich zur Weiterentwicklung und Reifung dieses Programms beigetragen haben. Konstruktive Kritik dient der stetigen Verbesserung, dem Kritiker gebührt die Co-Autorenschaft!

Ein weiterer Dank gilt **Dr. Michael Graw**, meinem ehemaligen Ambiophonie-Mentor, der mir in meiner aktiven Zeit als CTO der Drägerwerk AG in Lübeck die Augen und vor allem die Ohren für die Ambiophonie geöffnet hat.

Ebenso gilt unser Dank **Ralph Glasgal** für sein Lebenswerk, seine wegweisenden Forschungen am Ambiophonics Institute und die Entwicklung der Ambiophonie, die dieses Softwareprojekt inspiriert und ermöglicht haben.

---

*OAPW_AL_FIR_DOUBLE – Open Ambiophonics Processing Wrapper, High-End ALSA Loopback Edition with RACE, Fast Fourier Transform / Finite Response Filter Design applied in full Double Precision for stable and consistent Phase-Linearity.*
