#ifndef INPUT_VALIDATION_H
#define INPUT_VALIDATION_H

#include <cctype>
#include <cmath>
#include <string>

// Sichere Auswertung von Zahlen aus Web-API und Config-Datei.
// Anders als ein nacktes std::stod
//   - wirft nichts (kein Abbruch bei "abc" oder "1e999"),
//   - lehnt NaN und Inf ab ("nan", "inf"),
//   - lehnt angehängten Müll ab ("12abc"); nur Leerraum nach der Zahl ist erlaubt
//     (damit auch Zeilen mit "\r" aus Windows-Editoren funktionieren).
namespace InputValidation {

inline bool parseDouble(const std::string& text, double& out) {
    try {
        std::size_t used = 0;
        const double value = std::stod(text, &used);
        while (used < text.size() && std::isspace(static_cast<unsigned char>(text[used]))) ++used;
        if (used != text.size() || !std::isfinite(value)) return false;
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

// Schalter: 0 = aus, jede andere endliche Zahl = an (wie bisher "stoi(...) != 0").
inline bool parseFlag(const std::string& text, bool& out) {
    double value = 0.0;
    if (!parseDouble(text, value)) return false;
    out = (value != 0.0);
    return true;
}

} // namespace InputValidation

#endif // INPUT_VALIDATION_H
