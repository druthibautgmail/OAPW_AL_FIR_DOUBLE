#!/bin/bash

PROCESS="OAPW_Player"
HOSTNAME=$(hostname)
OUTFILE="oapw_metrics_${HOSTNAME}.csv"

# PID des OAPW_Player ermitteln
PID=$(pgrep -x "$PROCESS")

if [ -z "$PID" ]; then
    echo "Fehler: $PROCESS läuft aktuell nicht. Bitte erst starten!"
    exit 1
fi

echo "Logging gestartet für $PROCESS (PID $PID) auf Host: $HOSTNAME..."
echo "Schreibe Daten in: $OUTFILE"
echo "Beenden mit [CTRL+C]"

# CSV Header (Semikolon für deutsches Excel)
echo "Time;Temp_C;OAPW_CPU_%;OAPW_MEM_%;System_Load" > "$OUTFILE"

while true; do
    TIME=$(date '+%H:%M:%S')
    
    # System-Temperatur auslesen und skalieren
    TEMP_RAW=$(cat /sys/class/thermal/thermal_zone0/temp)
    TEMP_C=$(awk "BEGIN {printf \"%.1f\", $TEMP_RAW/1000}")
    
    # Exklusive CPU- und RAM-Last des OAPW_Player via top
    STATS=$(top -b -n 1 -p "$PID" | tail -1)
    CPU=$(echo "$STATS" | awk '{print $9}')
    MEM=$(echo "$STATS" | awk '{print $10}')
    
    # Globale Systemauslastung (1-Minuten Load Average)
    LOAD=$(cat /proc/loadavg | awk '{print $1}')
    
    # Dezimalpunkte in Kommas umwandeln für direkte Excel-Kompatibilität
    TEMP_C=${TEMP_C//./,}
    CPU=${CPU//./,}
    MEM=${MEM//./,}
    LOAD=${LOAD//./,}
    
    # Datenzeile in die CSV schreiben
    echo "$TIME;$TEMP_C;$CPU;$MEM;$LOAD" >> "$OUTFILE"
    
    sleep 1
done
