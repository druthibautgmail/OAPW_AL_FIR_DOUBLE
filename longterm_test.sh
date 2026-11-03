#!/bin/bash

LOGFILE="oapw_longterm_$(date +%Y%m%d_%H%M).log"

echo "=== OAPW Langzeit-Test gestartet: $(date) ===" > $LOGFILE

# 1. MPD auf Endlos-Schleife vorbereiten
mpc repeat on >> $LOGFILE
mpc random on >> $LOGFILE
mpc play >> $LOGFILE

# 2. OAPW_Player im Hintergrund starten, stderr & stdout ins Logfile umleiten
./OAPW_Player >> $LOGFILE 2>&1 &
OAPW_PID=$!

echo "OAPW_Player läuft mit PID $OAPW_PID" >> $LOGFILE

# 3. Hardware-Monitoring Schleife (minütlich)
while true; do
    TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
    TEMP=$(vcgencmd measure_temp | grep -o '[0-9.]*')
    # Nur loggen, wenn die Temperatur kritisch wird (> 70°C), um das Log sauber zu halten,
    # oder für den ersten Test einfach immer protokollieren:
    echo "[$TIMESTAMP] RPi5 Temp: ${TEMP}°C" >> $LOGFILE
    
    # Prüfe ob der Prozess noch lebt
    if ! kill -0 $OAPW_PID 2>/dev/null; then
        echo "[$TIMESTAMP] FATAL: OAPW_Player Prozess ist abgestürzt!" >> $LOGFILE
        break
    fi
    
    sleep 60
done
