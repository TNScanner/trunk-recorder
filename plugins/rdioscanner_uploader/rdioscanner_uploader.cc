#include <curl/curl.h>
#include <iomanip>
#include <time.h>
#include <vector>
#include <sstream>
#include <iostream>

#include "../../trunk-recorder/call_concluder/call_concluder.h"
#include "../../trunk-recorder/plugin_manager/plugin_api.h"
#include "../trunk-recorder/gr_blocks/decoder_wrapper.h"

#include <boost/algorithm/string.hpp>
#include <boost/dll/alias.hpp>
#include <boost/filesystem.hpp>
#include <sys/stat.h>

#define RDIO_DEBUG_ROUTE 1   // Lite routing debug

// ---------------- DATA STRUCTURES ----------------

struct RdioRoute {
    std::string name;
    uint32_t system_id;
    std::vector<uint32_t> talkgroups;
};

struct RdioSystem {
    std::string short_name;
    std::string api_key;
    uint32_t fallback_system_id;
    std::vector<RdioRoute> routes;
};

struct RdioUploaderData {
    std::string server;
    std::vector<RdioSystem> systems;
};

boost::mutex curl_share_mutex;


// -------------------- CLASS ----------------------

class Rdio_Scanner_Uploader : public Plugin_Api {
    RdioUploaderData data;
    CURLSH *curl_share;
    long curl_dns_ttl;

public:

// -------------------- HELPERS --------------------

    RdioSystem *get_system(const std::string &short_name) {
        for (auto &sys : data.systems) {
            if (sys.short_name == short_name)
                return &sys;
        }
        return nullptr;
    }

    static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
        ((std::string *)userp)->append((char *)contents, size * nmemb);
        return size * nmemb;
    }


// -------------------- ROUTE LOGIC --------------------

    uint32_t pick_system_id(const RdioSystem *sys, const Call_Data_t &call,
                            const RdioRoute **chosen_route)
    {
        *chosen_route = nullptr;
        uint32_t tg = call.talkgroup;

        std::vector<const RdioRoute*> exact_match;
        std::vector<const RdioRoute*> wildcard;

        for (const auto &r : sys->routes) {
            if (r.talkgroups.empty())
                wildcard.push_back(&r);
            else if (std::find(r.talkgroups.begin(), r.talkgroups.end(), tg) != r.talkgroups.end())
                exact_match.push_back(&r);
        }

#if RDIO_DEBUG_ROUTE
        BOOST_LOG_TRIVIAL(info)
            << "[RdioRoute] System=" << sys->short_name
            << " TG=" << tg
            << " exact=" << exact_match.size()
            << " wildcard=" << wildcard.size();
#endif

        if (!exact_match.empty()) {
            *chosen_route = exact_match[0];
            return exact_match[0]->system_id;
        }

        if (!wildcard.empty()) {
            *chosen_route = wildcard[0];
            return wildcard[0]->system_id;
        }

        *chosen_route = nullptr;
        return sys->fallback_system_id;
    }


// -------------------- UPLOAD (minimal fields) --------------------

    bool do_upload(const Call_Data_t &call, const RdioSystem *sys,
                   uint32_t system_id, const RdioRoute *route)
    {
        if (sys->api_key.empty()) return false;

#if RDIO_DEBUG_ROUTE
        BOOST_LOG_TRIVIAL(info)
            << "[RdioUpload] Start shortName=" << sys->short_name
            << " systemId=" << system_id
            << " TG=" << call.talkgroup
            << " file=" << (call.compress_wav ? call.converted : call.filename);
#endif

        CURL *curl = curl_easy_init();
        if (!curl) return false;

        std::string response_buffer;

        curl_mime *mime = curl_mime_init(curl);
        curl_mimepart *part;

        boost::filesystem::path audioPath(call.compress_wav ? call.converted : call.filename);
        std::string audioName = audioPath.filename().string();

        // audio
        part = curl_mime_addpart(mime);
        curl_mime_filedata(part, audioPath.string().c_str());
        curl_mime_name(part, "audio");

        // filename
        part = curl_mime_addpart(mime);
        curl_mime_data(part, audioName.c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "audioName");

        // audio type
        part = curl_mime_addpart(mime);
        curl_mime_data(part, call.compress_wav ? "audio/mp4" : "audio/wav", CURL_ZERO_TERMINATED);
        curl_mime_name(part, "audioType");

        // datetime
        part = curl_mime_addpart(mime);
        curl_mime_data(part, std::to_string(call.start_time).c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "dateTime");

        // frequency (simple float → string)
        part = curl_mime_addpart(mime);
        curl_mime_data(part, std::to_string(call.freq).c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "frequency");

        // talkgroup
        part = curl_mime_addpart(mime);
        curl_mime_data(part, std::to_string(call.talkgroup).c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "talkgroup");

        // systemId
        part = curl_mime_addpart(mime);
        curl_mime_data(part, std::to_string(system_id).c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "system");

        // apiKey
        part = curl_mime_addpart(mime);
        curl_mime_data(part, sys->api_key.c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "key");

        // label
        part = curl_mime_addpart(mime);
        curl_mime_data(part, call.short_name.c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "systemLabel");

        // Perform request
        std::string url = data.server + "/api/call-upload";

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);

        long http_code = 0;
        curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        curl_mime_free(mime);
        curl_easy_cleanup(curl);

#if RDIO_DEBUG_ROUTE
        BOOST_LOG_TRIVIAL(info)
            << "[RdioUpload] HTTP=" << http_code
            << " response=\"" << response_buffer << "\"";
#endif

        return http_code == 200;
    }


// -------------------- MAIN UPLOAD SELECTOR --------------------

    int upload(Call_Data_t call_info)
    {
        if (call_info.encrypted) return 0;

        RdioSystem *sys = get_system(call_info.short_name);
        if (!sys) return 0;

        const RdioRoute *route = nullptr;
        uint32_t system_id = pick_system_id(sys, call_info, &route);

        bool ok = do_upload(call_info, sys, system_id, route);
        return ok ? 0 : 1;
    }

    int call_end(Call_Data_t call_info) {
        return upload(call_info);
    }


// -------------------- CONFIG PARSING --------------------

    int parse_config(json cfg)
    {
        if (!cfg.contains("server")) return 1;
        data.server = cfg["server"].get<std::string>();

        for (auto &s : cfg["systems"]) {
            RdioSystem sys;
            sys.short_name = s.value("shortName", "");
            sys.api_key = s.value("apiKey", "");
            sys.fallback_system_id = s.value("systemId", 0);

            if (s.contains("routes")) {
                for (auto &r : s["routes"]) {
                    RdioRoute route;
                    route.name = r.value("name", "");
                    route.system_id = r.value("systemId", 0);

                    for (auto &tg : r["talkgroups"])
                        route.talkgroups.push_back(tg.get<uint32_t>());

                    sys.routes.push_back(route);
                }
            }

#if RDIO_DEBUG_ROUTE
            BOOST_LOG_TRIVIAL(info)
                << "[Rdio Scanner] Loaded system=" << sys.short_name
                << " fallbackId=" << sys.fallback_system_id
                << " routes=" << sys.routes.size();
#endif

            data.systems.push_back(sys);
        }

        curl_share = curl_share_init();
        curl_dns_ttl = 300;

        return 0;
    }


// -------------------- CURL LOCKS --------------------

    static void curl_lock_cb(CURL *handle, curl_lock_data data,
                             curl_lock_access access, void *userptr)
    { curl_share_mutex.lock(); }

    static void curl_unlock_cb(CURL *handle, curl_lock_data data,
                               curl_lock_access access, void *userptr)
    { curl_share_mutex.unlock(); }


    static boost::shared_ptr<Rdio_Scanner_Uploader> create() {
        return boost::shared_ptr<Rdio_Scanner_Uploader>(
                new Rdio_Scanner_Uploader());
    }
};

BOOST_DLL_ALIAS(
    Rdio_Scanner_Uploader::create,
    create_plugin
)

