#pragma once
#include <iostream>
#include <mutex>
#include <string>

class Console {
public:
    static std::mutex& getMutex() {
        static std::mutex mutex_;
        return mutex_;
    }

    template<typename T>
    static void log(const T& msg) {
        std::lock_guard<std::mutex> lock(getMutex());
        std::cout << msg << std::endl;
    }

    template<typename T, typename... Args>
    static void log(const T& first, const Args&... args) {
        std::lock_guard<std::mutex> lock(getMutex());
        std::cout << first;
        ((std::cout << args), ...);
        std::cout << std::endl;
    }

    template<typename T>
    static void error(const T& msg) {
        std::lock_guard<std::mutex> lock(getMutex());
        std::cerr << msg << std::endl;
    }

    template<typename T, typename... Args>
    static void error(const T& first, const Args&... args) {
        std::lock_guard<std::mutex> lock(getMutex());
        std::cerr << first;
        ((std::cerr << args), ...);
        std::cerr << std::endl;
    }
};