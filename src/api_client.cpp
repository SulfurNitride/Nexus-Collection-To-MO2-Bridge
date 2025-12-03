#include "console.h"
#include "json.h"
#include <string>
#include <vector>

class ApiClient {
public:
    static size_t write_callback(void* contents, size_t size, size_t nmemb, std::string* s) {
        size_t newLength = size * nmemb;
        try {
            s->append((char*)contents, newLength);
        } catch(std::bad_alloc& e) {
            return 0;
        }
        return newLength;
    }

    static std::string get(const std::string& url, const std::string& apiKey = "") {
        CURL* curl = curl_easy_init();
        std::string response;
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "NexusBridge/1.0"); // Reverting User-Agent
            
            struct curl_slist *headers = NULL;
            if (!apiKey.empty()) {
                std::string auth = "apikey: " + apiKey;
                headers = curl_slist_append(headers, auth.c_str());
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            }
            
            CURLcode res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                Console::error(std::string("API Request Failed: ") + curl_easy_strerror(res));
            }
            
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }
        return response;
    }

    static std::string post(const std::string& url, const std::string& payload, const std::string& apiKey = "") {
        CURL* curl = curl_easy_init();
        std::string response;
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "NexusBridge/1.0"); // Reverting User-Agent

            struct curl_slist *headers = NULL;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            if (!apiKey.empty()) {
                std::string auth = "apikey: " + apiKey;
                headers = curl_slist_append(headers, auth.c_str());
            }
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L); // Disabled verbose to reduce noise

            CURLcode res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                Console::error(std::string("API Request Failed: ") + curl_easy_strerror(res));
            }

            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }
        return response;
    }
};