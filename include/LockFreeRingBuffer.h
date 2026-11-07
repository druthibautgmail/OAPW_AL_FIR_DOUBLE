#ifndef LOCK_FREE_RING_BUFFER_H
#define LOCK_FREE_RING_BUFFER_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

// SPSC (Single-Producer / Single-Consumer) Lock-Free Ringbuffer
// Speziell optimiert für die ALSA Capture/Playback Architektur im OAPW_Player_HiFi
//
// Thread-Zuständigkeit (wichtig für die Korrektheit):
//   Producer (Capture-Thread):  push()                    schreibt NUR head
//   Consumer (Playback-Thread): pop(), discard(),
//                               discard_all()             schreiben NUR tail
//   Beliebiger Thread:          get_fill_level(), get_capacity(),
//                               get_dropped_items()       nur lesend
//   reset()  darf ausschließlich aufgerufen werden, wenn weder Producer noch
//            Consumer laufen (z.B. vor dem Start der Threads).
//
// head und tail zählen monoton (size_t); Füllstand = head - tail. Der Index im
// Array ist Zähler % capacity. Der Zähler läuft bei 64 Bit praktisch nie über.
template <typename T>
class LockFreeRingBuffer {
    static_assert(std::is_trivially_copyable<T>::value,
                  "LockFreeRingBuffer kopiert per memcpy und erfordert trivial kopierbare Typen");

private:
    std::vector<T> buffer;
    const size_t capacity;

    // Cache-Line Alignment zur Vermeidung von False Sharing zwischen den Threads.
    alignas(64) std::atomic<size_t> head; // Wird nur vom Producer (Capture-Thread) geschrieben
    alignas(64) std::atomic<size_t> tail; // Wird nur vom Consumer (Playback-Thread) geschrieben
    alignas(64) std::atomic<size_t> dropped_items; // Vom Producer hochgezählt (verworfene Elemente)

public:
    explicit LockFreeRingBuffer(size_t size)
        : buffer(size), capacity(size), head(0), tail(0), dropped_items(0) {}

    // Push: Schreibt ALLE items oder gar nichts (Aufruf durch Capture-Thread).
    // Rückgabe false = nicht genug Platz, die Daten wurden verworfen und gezählt.
    bool push(const T* data, size_t items) {
        if (items == 0) return true;

        const size_t current_head = head.load(std::memory_order_relaxed);
        const size_t current_tail = tail.load(std::memory_order_acquire);

        const size_t used = current_head - current_tail;
        if (items > capacity - used) {
            dropped_items.fetch_add(items, std::memory_order_relaxed);
            return false; // Overrun
        }

        // In höchstens zwei zusammenhängenden Segmenten kopieren
        const size_t index = current_head % capacity;
        const size_t first = std::min(items, capacity - index);
        std::memcpy(&buffer[index], data, first * sizeof(T));
        if (items > first) {
            std::memcpy(&buffer[0], data + first, (items - first) * sizeof(T));
        }

        head.store(current_head + items, std::memory_order_release);
        return true;
    }

    // Pop: Liest ALLE items oder gar nichts (Aufruf durch Playback/DSP-Thread).
    // Rückgabe false = zu wenige Daten (Underrun); der Pufferinhalt bleibt unverändert.
    bool pop(T* dest, size_t items) {
        if (items == 0) return true;

        const size_t current_head = head.load(std::memory_order_acquire);
        const size_t current_tail = tail.load(std::memory_order_relaxed);

        if (current_head - current_tail < items) {
            return false; // Underrun
        }

        const size_t index = current_tail % capacity;
        const size_t first = std::min(items, capacity - index);
        std::memcpy(dest, &buffer[index], first * sizeof(T));
        if (items > first) {
            std::memcpy(dest + first, &buffer[0], (items - first) * sizeof(T));
        }

        tail.store(current_tail + items, std::memory_order_release);
        return true;
    }

    // Verwirft bis zu `items` älteste Elemente (nur Consumer-Thread).
    // Gibt die tatsächlich verworfene Anzahl zurück.
    size_t discard(size_t items) {
        const size_t current_head = head.load(std::memory_order_acquire);
        const size_t current_tail = tail.load(std::memory_order_relaxed);
        const size_t n = std::min(items, current_head - current_tail);
        tail.store(current_tail + n, std::memory_order_release);
        return n;
    }

    // Verwirft den gesamten aktuellen Inhalt (nur Consumer-Thread).
    // Ersetzt den früheren reset()-Aufruf aus dem Playback-Thread: Es wird nur
    // tail bewegt, head bleibt beim Producer, daher kein Wettlauf mit push().
    size_t discard_all() {
        const size_t current_head = head.load(std::memory_order_acquire);
        const size_t current_tail = tail.load(std::memory_order_relaxed);
        tail.store(current_head, std::memory_order_release);
        return current_head - current_tail;
    }

    // Reset: Setzt Zähler und Statistik zurück.
    // NUR aufrufen, solange kein Producer- und kein Consumer-Thread läuft!
    // Aus dem laufenden Betrieb heraus stattdessen discard_all() (Consumer) verwenden.
    void reset() {
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
        dropped_items.store(0, std::memory_order_relaxed);
    }

    // Füllstand abfragen (z.B. für Diagnostik oder Re-Centering Logik)
    size_t get_fill_level() const {
        // tail zuerst lesen: head kann danach nur größer werden, die Differenz
        // wird dadurch nie negativ (bzw. als size_t riesig). Von einem dritten
        // Thread aus kann der Wert kurzzeitig etwas zu hoch sein; im Consumer
        // ist er eine sichere Untergrenze der verfügbaren Daten.
        const size_t current_tail = tail.load(std::memory_order_acquire);
        const size_t current_head = head.load(std::memory_order_acquire);
        return current_head - current_tail;
    }

    size_t get_capacity() const {
        return capacity;
    }

    // Anzahl der seit dem Start vom Producer verworfenen Elemente (Overrun-Diagnose).
    size_t get_dropped_items() const {
        return dropped_items.load(std::memory_order_relaxed);
    }
};

#endif // LOCK_FREE_RING_BUFFER_H
