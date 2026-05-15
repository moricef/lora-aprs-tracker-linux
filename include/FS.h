#pragma once
// Linux File/Dir abstraction matching the Arduino SD/SPIFFS File API used in the firmware.
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include "Arduino.h"

#define FILE_READ   "r"
#define FILE_WRITE  "w"
#define FILE_APPEND "a"

class File {
public:
    File() : _fp(nullptr), _dir(nullptr), _isDir(false) {}

    // Open a file
    explicit File(const char* path, const char* mode) : _fp(nullptr), _dir(nullptr), _isDir(false) {
        if (!path) return;
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            _dir = opendir(path);
            _isDir = true;
            _name = path;
        } else {
            _fp = fopen(path, mode);
            _isDir = false;
            _name = path;
        }
    }

    ~File() { /* caller calls close() explicitly, matching Arduino */ }

    explicit operator bool() const { return _isDir ? (_dir != nullptr) : (_fp != nullptr); }

    void close() {
        if (_fp)  { fclose(_fp);  _fp  = nullptr; }
        if (_dir) { closedir(_dir); _dir = nullptr; }
    }

    // byte count available for read
    int available() {
        if (!_fp) return 0;
        long cur = ftell(_fp);
        fseek(_fp, 0, SEEK_END);
        long end = ftell(_fp);
        fseek(_fp, cur, SEEK_SET);
        return (int)(end - cur);
    }

    int read() {
        if (!_fp) return -1;
        return fgetc(_fp);
    }

    String readStringUntil(char terminator) {
        if (!_fp) return String();
        std::string line;
        int c;
        while ((c = fgetc(_fp)) != EOF) {
            if (c == terminator) break;
            line += (char)c;
        }
        return String(line);
    }

    size_t print(const String& s)  { return _fp ? fwrite(s.c_str(), 1, s.length(), _fp) : 0; }
    size_t print(const char* s)    { return (_fp && s) ? fputs(s, _fp) : 0; }
    size_t println(const String& s){ size_t n = print(s); if(_fp) { fputc('\n',_fp); n++; } return n; }
    size_t println(const char* s)  { size_t n = print(s); if(_fp) { fputc('\n',_fp); n++; } return n; }
    size_t write(const uint8_t* buf, size_t sz) { return _fp ? fwrite(buf, 1, sz, _fp) : 0; }

    bool isDirectory() const { return _isDir; }
    const char* name() const { return _name.c_str(); }

    // last write time (mtime)
    time_t getLastWrite() const {
        struct stat st;
        if (_name.empty() || stat(_name.c_str(), &st) != 0) return 0;
        return st.st_mtime;
    }

    size_t size() const {
        struct stat st;
        if (_name.empty() || stat(_name.c_str(), &st) != 0) return 0;
        return (size_t)st.st_size;
    }

    // Directory iteration
    File openNextFile() {
        if (!_dir) return File();
        struct dirent* ent;
        while ((ent = readdir(_dir)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string child = _name + "/" + ent->d_name;
            File f;
            struct stat st;
            if (stat(child.c_str(), &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    f._dir  = opendir(child.c_str());
                    f._isDir = true;
                } else {
                    f._fp   = fopen(child.c_str(), "r");
                    f._isDir = false;
                }
                f._name = child;
            }
            return f;
        }
        return File();
    }

private:
    FILE*       _fp;
    DIR*        _dir;
    bool        _isDir;
    std::string _name;
};
