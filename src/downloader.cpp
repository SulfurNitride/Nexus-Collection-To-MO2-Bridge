#pragma once
#include "console.h"
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include <curl/curl.h>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

struct DownloadTask {
    std::string url;
    std::string outputPath;
    std::string modName;
    int fileId;
    long long fileSize;
    std::function<void(const std::string&)> onSuccess;
};

class Downloader {
public:
    Downloader(int threads = 0, const std::string& apiKey = "") : m_apiKey(apiKey) {
        if (threads <= 0) {
            m_threads = std::thread::hardware_concurrency();
            if (m_threads == 0) m_threads = 4;
        } else {
            m_threads = threads;
        }
        Console::log("Initializing Thread Pool with ", m_threads, " threads.");
        curl_global_init(CURL_GLOBAL_ALL);
        start();
    }

    ~Downloader() {
        stop();
        curl_global_cleanup();
    }

    void addTask(const DownloadTask& task) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push(task);
        }
        m_cv.notify_one();
    }

    void wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_finished_cv.wait(lock, [this] { return m_queue.empty() && m_active_workers == 0; });
    }

private:
    int m_threads;
    std::string m_apiKey;
    std::queue<DownloadTask> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_finished_cv;
    std::vector<std::thread> m_workers;
    bool m_stop = false;
    int m_active_workers = 0;

    void start() {
        for (int i = 0; i < m_threads; ++i) {
            m_workers.emplace_back([this] { worker(); });
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        for (auto& t : m_workers) {
            if (t.joinable()) t.join();
        }
    }

    static size_t write_data(void* ptr, size_t size, size_t nmemb, FILE* stream) {
        return fwrite(ptr, size, nmemb, stream);
    }

    void worker() {
        while (true) {
            DownloadTask task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
                
                if (m_stop && m_queue.empty()) return;
                
                task = m_queue.front();
                m_queue.pop();
                m_active_workers++;
            }

            bool success = false;
            if (task.url.empty()) {
                success = true; 
            } else {
                Console::log("Downloading: ", task.modName, "...");
                
                CURL* curl = curl_easy_init();
                if (curl) {
                    FILE* fp = fopen(task.outputPath.c_str(), "wb");
                    if (fp) {
                        struct curl_slist *headers = NULL;
                        if (!m_apiKey.empty()) {
                            std::string auth = "apikey: " + m_apiKey;
                            headers = curl_slist_append(headers, auth.c_str());
                            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                        }
                        
                        // Trim URL just in case
                        std::string cleanUrl = task.url;
                        cleanUrl.erase(0, cleanUrl.find_first_not_of(" \t\n\r"));
                        cleanUrl.erase(cleanUrl.find_last_not_of(" \t\n\r") + 1);

                        curl_easy_setopt(curl, CURLOPT_URL, cleanUrl.c_str());
                        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
                        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                        curl_easy_setopt(curl, CURLOPT_USERAGENT, "NexusBridge/1.0");
                        
                        CURLcode res = curl_easy_perform(curl);
                        if (res == CURLE_OK) {
                            success = true;
                        } else {
                            Console::error(std::string("Download failed for ") + task.modName + ": " + curl_easy_strerror(res));
                        }
                        
                        curl_slist_free_all(headers);
                        fclose(fp);
                    }
                    curl_easy_cleanup(curl);
                }
            }

            if (success && task.onSuccess) {
                task.onSuccess(task.outputPath);
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_active_workers--;
            }
            m_finished_cv.notify_all();
        }
    }
};
