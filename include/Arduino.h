#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <algorithm>
#include <time.h>

typedef uint8_t byte;

#ifndef TWO_PI
#define TWO_PI 6.283185307179586476925286766559
#endif

inline double radians(double deg) { return deg * 0.017453292519943295769236907684886; }
inline double degrees(double rad) { return rad * 57.295779513082320876798154814105; }
template<typename T> inline T sq(T x) { return x * x; }

// millis() — monotonic, wraps at ~49 days like Arduino
uint32_t millis();

// random(max) → [0, max-1],  random(min, max) → [min, max-1]
inline long random(long max) { return max > 0 ? (long)(rand() % max) : 0; }
inline long random(long mn, long mx) { return mn < mx ? mn + (long)(rand() % (mx - mn)) : mn; }

// ─────────────────────────────────────────────────────────────────────────────
// String
// ─────────────────────────────────────────────────────────────────────────────
class String {
public:
    std::string s;

    String()                                   {}
    String(const char* c)    : s(c ? c : "")  {}
    String(const std::string& str) : s(str)   {}
    String(char c)           : s(1, c)        {}
    String(int v)            : s(std::to_string(v)) {}
    String(long v)           : s(std::to_string(v)) {}
    String(unsigned long v)  : s(std::to_string(v)) {}
    String(uint32_t v)       : s(std::to_string(v)) {}
    String(float v, int dec = 2) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.*f", dec, (double)v); s = buf;
    }
    String(double v, int dec = 2) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.*f", dec, v); s = buf;
    }

    // assignment
    String& operator=(const String& o)   { s = o.s; return *this; }
    String& operator=(const char* c)     { s = c ? c : ""; return *this; }
    String& operator=(char c)            { s = std::string(1,c); return *this; }

    // concatenation
    String  operator+(const String& o)   const { return String(s + o.s); }
    String  operator+(const char* c)     const { return String(s + (c?c:"")); }
    String  operator+(char c)            const { return String(s + c); }
    String  operator+(int v)             const { return String(s + std::to_string(v)); }
    String  operator+(long v)            const { return String(s + std::to_string(v)); }
    String  operator+(unsigned long v)   const { return String(s + std::to_string(v)); }
    String  operator+(float v)           const { return *this + String(v); }
    String& operator+=(const String& o)  { s += o.s; return *this; }
    String& operator+=(const char* c)    { if(c) s += c; return *this; }
    String& operator+=(char c)           { s += c; return *this; }

    // comparison
    bool operator==(const String& o) const { return s == o.s; }
    bool operator==(const char* c)   const { return s == (c?c:""); }
    bool operator!=(const String& o) const { return s != o.s; }
    bool operator!=(const char* c)   const { return s != (c?c:""); }
    bool operator< (const String& o) const { return s <  o.s; }
    bool operator> (const String& o) const { return s >  o.s; }

    // index
    char operator[](unsigned int i) const { return i < s.size() ? s[i] : 0; }
    char& operator[](unsigned int i) { return s[i]; }

    explicit operator bool() const { return !s.empty(); }

    // queries
    unsigned int length()  const { return (unsigned int)s.size(); }
    bool isEmpty()         const { return s.empty(); }
    const char* c_str()    const { return s.c_str(); }

    int indexOf(char c, unsigned int from = 0) const {
        size_t p = s.find(c, from);
        return p == std::string::npos ? -1 : (int)p;
    }
    int indexOf(const String& sub, unsigned int from = 0) const {
        size_t p = s.find(sub.s, from);
        return p == std::string::npos ? -1 : (int)p;
    }
    int indexOf(const char* c, unsigned int from = 0) const {
        if (!c) return -1;
        size_t p = s.find(c, from);
        return p == std::string::npos ? -1 : (int)p;
    }
    int lastIndexOf(char c) const {
        size_t p = s.rfind(c);
        return p == std::string::npos ? -1 : (int)p;
    }
    int lastIndexOf(const String& sub) const {
        size_t p = s.rfind(sub.s);
        return p == std::string::npos ? -1 : (int)p;
    }

    String substring(unsigned int from) const {
        return from < s.size() ? String(s.substr(from)) : String();
    }
    String substring(unsigned int from, unsigned int to) const {
        if (from >= s.size()) return String();
        return String(s.substr(from, to - from));
    }

    bool startsWith(const String& sub) const {
        return s.size() >= sub.s.size() && s.compare(0, sub.s.size(), sub.s) == 0;
    }
    bool endsWith(const String& sub) const {
        return s.size() >= sub.s.size() &&
               s.compare(s.size() - sub.s.size(), sub.s.size(), sub.s) == 0;
    }

    void trim() {
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        if (b == std::string::npos) { s.clear(); return; }
        s = s.substr(b, e - b + 1);
    }
    void toUpperCase() {
        for (char& c : s) c = (char)toupper((unsigned char)c);
    }
    void toLowerCase() {
        for (char& c : s) c = (char)tolower((unsigned char)c);
    }

    void replace(const String& from, const String& to) {
        size_t pos = 0;
        while ((pos = s.find(from.s, pos)) != std::string::npos) {
            s.replace(pos, from.s.size(), to.s);
            pos += to.s.size();
        }
    }
    void replace(char from, char to) {
        for (char& c : s) if (c == from) c = to;
    }

    void remove(unsigned int from, unsigned int count = (unsigned int)-1) {
        if (from >= s.size()) return;
        if (count == (unsigned int)-1) s.erase(from);
        else s.erase(from, count);
    }

    bool concat(const String& o) { s += o.s; return true; }

    int    toInt()   const { return atoi(s.c_str()); }
    float  toFloat() const { return (float)atof(s.c_str()); }
    double toDouble()const { return atof(s.c_str()); }
};

// Free-function concatenation (left-hand non-String)
inline String operator+(const char* c, const String& r) { return String(c) + r; }
inline String operator+(char c,        const String& r) { return String(c) + r; }
inline String operator+(int v,         const String& r) { return String(v) + r; }

// String literal helpers
inline bool operator==(const char* c, const String& r) { return r == c; }
inline bool operator!=(const char* c, const String& r) { return r != c; }
