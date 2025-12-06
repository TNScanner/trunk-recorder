#include <curl/curl.h>
#include <iomanip>
#include <time.h>
#include <vector>
#include <sstream>
#include <iostream>
#include <algorithm>

#include "../../trunk-recorder/call_concluder/call_concluder.h"
#include "../../trunk-recorder/plugin_manager/plugin_api.h"
#include "../trunk-recorder/gr_blocks/decoder_wrapper.h"

#include <boost/algorithm/string.hpp>
#include <boost/dll/alias.hpp>
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>
#include <sys/stat.h>

#define RDIO_DEBUG_ROUTE 1   // Lite routing debug

// ---------------- DATA STRUCTURES ----------------

struct RdioRoute {
    std::string name;
    uint32_t system_id;
    std::vector<uint32_t> talkgroups;   // if empty => wildcard route
};

struct RdioSystem {
    std::string short_name;
    std::string api_key;
    uint32_t fallback_system_id;        // "systemId" from config
    std::vector<RdioRoute> routes;      // optional per-TG routes
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

    uint32_t pick_system_id(const RdioSystem *sys,
                            const Call_Data_t &call,
                            const RdioRoute **chosen_route)
    {
        *chosen_route = nullptr;
        uint32_t tg = call.talkgroup;

        std::vector<const RdioRoute*> exact_match;
        std::vector<const RdioRoute*> wildcard;

        for (const auto &r : sys->routes) {
            if (r.talkgroups.empty()) {
                // wildcard route (matches anything)
                wildcard.push_back(&r);
            } else {
                if (std::find(r.talkgroups.begin(), r.talkgroups.end(), tg) != r.talkgroups.end()) {
                    exact_match.push_back(&r);
                }
            }
        }

#if RDIO_DEBUG_ROUTE
        BOOST_LOG_TRIVIAL(info)
            << "[RdioRoute] System=" << sys->short_name
            << " TG=" << tg
            << " exact=" << exact_match.size()
            << " wildcard=" << wildcard.size();
#endif

        // 1) prefer exact TG match
        if (!exact_match.empty()) {
            *chosen_route = exact_match[0];
            return exact_match[0]->system_id;
        }

        // 2) else use first wildcard route (if any)
        if (!wildcard.empty()) {
            *chosen_route = wildcard[0];
            return wildcard[0]->system_id;
        }

        // 3) else fallback to systemId
        *chosen_route = nullptr;
        return sys->fallback_system_id;
    }


    // -------------------- UPLOAD --------------------

    bool do_upload(const Call_Data_t &call,
                   const RdioSystem *sys,
                   uint32_t system_id,
                   const RdioRoute *route)
    {
        if (sys->api_key.empty()) return false;

        // Build strings similar to original uploader
        std::ostringstream freq;
        freq << std::fixed << std::setprecision(0) << call.freq;
        std::string freq_string = freq.str();

        std::ostringstream call_length;
        call_length << std::fixed << std::setprecision(0) << call.length;
        std::string call_length_string = call_length.str();

        // Sources list (from transmission_source_list)
        std::ostringstream source_list;
        source_list << std::fixed << std::setprecision(2) << "[";
        if (!call.transmission_source_list.empty()) {
            for (size_t i = 0; i < call.transmission_source_list.size(); i++) {
                const auto &src = call.transmission_source_list[i];
                source_list << "{ \"pos\": " << std::setprecision(2) << src.position
                            << ", \"src\": " << std::setprecision(0) << src.source << " }";
                if (i < call.transmission_source_list.size() - 1) {
                    source_list << ", ";
                } else {
                    source_list << "]";
                }
            }
        } else {
            source_list << "]";
        }
        std::string source_list_string = source_list.str();

        // Patch list (from patched_talkgroups)
        std::ostringstream patch_list;
        patch_list << std::fixed << std::setprecision(2) << "[";
        if (call.patched_talkgroups.size() > 1) {
            for (size_t i = 0; i < call.patched_talkgroups.size(); i++) {
                if (i != 0) patch_list << ",";
                patch_list << (int)call.patched_talkgroups[i];
            }
            patch_list << "]";
        } else {
            patch_list << "]";
        }
        std::string patch_list_string = patch_list.str();

        // Frequency error list (from transmission_error_list)
        std::ostringstream freq_list;
        freq_list << std::fixed << std::setprecision(2) << "[";
        if (!call.transmission_error_list.empty()) {
            for (size_t i = 0; i < call.transmission_error_list.size(); i++) {
                const auto &err = call.transmission_error_list[i];
                freq_list << "{"
                          << "\"freq\": " << std::fixed << std::setprecision(0) << call.freq
                          << ", \"time\": " << err.time
                          << ", \"pos\": " << std::fixed << std::setprecision(2) << err.position
                          << ", \"len\": " << err.total_len
                          << ", \"errorCount\": " << std::setprecision(0) << err.error_count
                          << ", \"spikeCount\": " << err.spike_count
                          << "}";
                if (i < call.transmission_error_list.size() - 1) {
                    freq_list << ", ";
                } else {
                    freq_list << "]";
                }
            }
        } else {
            freq_list << "]";
        }
        std::string freq_list_string = freq_list.str();

        // Audio path / name
        bool compress_wav = call.compress_wav;
        boost::filesystem::path audioPath(compress_wav ? call.converted : call.filename);
        std::string audioName = audioPath.filename().string();

#if RDIO_DEBUG_ROUTE
        BOOST_LOG_TRIVIAL(info)
            << "[RdioUpload] Start shortName=" << sys->short_name
            << " systemId=" << system_id
            << " TG=" << call.talkgroup
            << " file=" << audioPath.string();
#endif

        CURLM *multi_handle = nullptr;
        CURL *curl = curl_easy_init();
        if (!curl) return false;

        std::string response_buffer;
        curl_mime *mime = curl_mime_init(curl);
        curl_mimepart *part = nullptr;
        struct curl_slist *headerlist = nullptr;

        // audio file
        part = curl_mime_addpart(mime);
        curl_mime_filedata(part, audioPath.string().c_str());
        curl_mime_type(part, "application/octet-stream");
        curl_mime_name(part, "audio");

        // audioName
        part = curl_mime_addpart(mime);
        curl_mime_data(part, audioName.c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "audioName");

        // audioType
        part = curl_mime_addpart(mime);
        curl_mime_data(part, compress_wav ? "audio/mp4" : "audio/wav", CURL_ZERO_TERMINATED);
        curl_mime_name(part, "audioType");

        // dateTime (start_time)
        part = curl_mime_addpart(mime);
        {
            std::string dt = boost::lexical_cast<std::string>(call.start_time);
            curl_mime_data(part, dt.c_str(), CURL_ZERO_TERMINATED);
        }
        curl_mime_name(part, "dateTime");

        // frequencies list
        part = curl_mime_addpart(mime);
        curl_mime_data(part, freq_list_string.c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "frequencies");

        // frequency
        part = curl_mime_addpart(mime);
        curl_mime_data(part, freq_string.c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "frequency");

        // API key
        part = curl_mime_addpart(mime);
        curl_mime_data(part, sys->api_key.c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "key");

        // patches
        part = curl_mime_addpart(mime);
        curl_mime_data(part, patch_list_string.c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "patches");

        // talkgroup (ID)
        part = curl_mime_addpart(mime);
        {
            std::string tg = boost::lexical_cast<std::string>(call.talkgroup);
            curl_mime_data(part, tg.c_str(), CURL_ZERO_TERMINATED);
        }
        curl_mime_name(part, "talkgroup");

        // talkgroupGroup
        part = curl_mime_addpart(mime);
        curl_mime_data(part, call.talkgroup_group.c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "talkgroupGroup");

        // talkgroupLabel (alpha tag)
        part = curl_mime_addpart(mime);
        curl_mime_data(part, call.talkgroup_alpha_tag.c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "talkgroupLabel");

        // talkgroupTag
        part = curl_mime_addpart(mime);
        curl_mime_data(part, call.talkgroup_tag.c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "talkgroupTag");

        // talkgroupName (description)
        part = curl_mime_addpart(mime);
        curl_mime_data(part, call.talkgroup_description.c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "talkgroupName");

        // sources
        part = curl_mime_addpart(mime);
        curl_mime_data(part, source_list_string.c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "sources");

        // systemId (selected via routing)
        part = curl_mime_addpart(mime);
        {
            std::string sid = std::to_string(system_id);
            curl_mime_data(part, sid.c_str(), CURL_ZERO_TERMINATED);
        }
        curl_mime_name(part, "system");

        // systemLabel (short_name)
        part = curl_mime_addpart(mime);
        curl_mime_data(part, call.short_name.c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "systemLabel");

        // Prepare multi
        multi_handle = curl_multi_init();
        headerlist = curl_slist_append(headerlist, "Expect:");

        std::string url = data.server + "/api/call-upload";

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "TrunkRecorder1.0");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerlist);
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);

        // DNS cache (shared)
        curl_easy_setopt(curl, CURLOPT_SHARE, curl_share);
        curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, curl_dns_ttl);

        curl_multi_add_handle(multi_handle, curl);

        int still_running = 0;
        curl_multi_perform(multi_handle, &still_running);

        while (still_running) {
            struct timeval timeout;
            int rc;
            CURLMcode mc;

            fd_set fdread;
            fd_set fdwrite;
            fd_set fdexcep;
            int maxfd = -1;

            long curl_timeo = -1;

            FD_ZERO(&fdread);
            FD_ZERO(&fdwrite);
            FD_ZERO(&fdexcep);

            timeout.tv_sec = 1;
            timeout.tv_usec = 0;

            curl_multi_timeout(multi_handle, &curl_timeo);
            if (curl_timeo >= 0) {
                timeout.tv_sec = curl_timeo / 1000;
                if (timeout.tv_sec > 1)
                    timeout.tv_sec = 1;
                else
                    timeout.tv_usec = (curl_timeo % 1000) * 1000;
            }

            mc = curl_multi_fdset(multi_handle, &fdread, &fdwrite, &fdexcep, &maxfd);
            if (mc != CURLM_OK) {
                fprintf(stderr, "curl_multi_fdset() failed, code %d.\n", mc);
                break;
            }

            if (maxfd == -1) {
                struct timeval wait = {0, 100 * 1000}; /* 100ms */
                rc = select(0, NULL, NULL, NULL, &wait);
            } else {
                rc = select(maxfd + 1, &fdread, &fdwrite, &fdexcep, &timeout);
            }

            switch (rc) {
            case -1:
                /* select error */
                still_running = 0;
                break;
            case 0:
            default:
                curl_multi_perform(multi_handle, &still_running);
                break;
            }
        }

        curl_multi_cleanup(multi_handle);
        curl_mime_free(mime);
        curl_slist_free_all(headerlist);

        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        curl_easy_cleanup(curl);

#if RDIO_DEBUG_ROUTE
        BOOST_LOG_TRIVIAL(info)
            << "[RdioUpload] HTTP=" << response_code
            << " response=\"" << response_buffer << "\"";
#endif

        // Log like original uploader
        std::string loghdr = log_header(call.short_name,
                                        call.call_num,
                                        call.talkgroup_display,
                                        call.freq);

        if (response_code == 200) {
            struct stat file_info;
            stat(audioPath.string().c_str(), &file_info);
            BOOST_LOG_TRIVIAL(info)
                << loghdr
                << "Rdio Scanner Upload Success - file size: "
                << file_info.st_size;
            return true;
        }

        BOOST_LOG_TRIVIAL(error)
            << loghdr
            << "Rdio Scanner Upload Error: "
            << response_buffer;

        return false;
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

    int parse_config(json config_data)
    {
        std::string log_prefix = "\t[Rdio Scanner]\t";

        if (!config_data.contains("server")) {
            BOOST_LOG_TRIVIAL(error) << log_prefix << "Missing 'server'";
            return 1;
        }

        data.server = config_data["server"].get<std::string>();
        BOOST_LOG_TRIVIAL(info) << log_prefix << "Server: " << data.server;

        if (!config_data.contains("systems")) {
            BOOST_LOG_TRIVIAL(error) << log_prefix << "Missing 'systems'";
            return 1;
        }

        // For redacting API keys
        boost::regex api_regex("(.*)(.{2}$)");
        boost::cmatch what;

        for (auto &sysJson : config_data["systems"]) {
            RdioSystem sys;
            sys.short_name = sysJson.value("shortName", "");
            sys.api_key = sysJson.value("apiKey", "");
            sys.fallback_system_id = sysJson.value("systemId", 0);

            if (sys.short_name.empty() || sys.api_key.empty()) {
                continue;
            }

            // Routes (optional)
            if (sysJson.contains("routes")) {
                for (auto &routeJson : sysJson["routes"]) {
                    RdioRoute route;
                    route.name = routeJson.value("name", "");
                    route.system_id = routeJson.value("systemId", 0);

                    if (routeJson.contains("talkgroups")) {
                        for (auto &tg : routeJson["talkgroups"]) {
                            route.talkgroups.push_back(tg.get<uint32_t>());
                        }
                    }

                    sys.routes.push_back(route);
                }
            }

            // Redact last two chars of API for log
            std::string redacted = "******";
            if (regex_match(sys.api_key.c_str(), what, api_regex)) {
                redacted += std::string(what[2].first, what[2].second);
            }

            BOOST_LOG_TRIVIAL(info)
                << log_prefix
                << "Loaded system=" << sys.short_name
                << " fallbackId=" << sys.fallback_system_id
                << " routes=" << sys.routes.size()
                << " apiKey=" << redacted;

            data.systems.push_back(sys);
        }

        if (data.systems.empty()) {
            BOOST_LOG_TRIVIAL(error)
                << log_prefix
                << "Rdio Scanner Server set, but no systems configured";
            return 1;
        }

        // Shared CURL DNS cache
        curl_share = curl_share_init();
        curl_share_setopt(curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(curl_share, CURLSHOPT_LOCKFUNC, curl_lock_cb);
        curl_share_setopt(curl_share, CURLSHOPT_UNLOCKFUNC, curl_unlock_cb);
        curl_dns_ttl = 300;

        return 0;
    }


    // -------------------- CURL LOCKS --------------------

    static void curl_lock_cb(CURL *handle, curl_lock_data data,
                             curl_lock_access access, void *userptr)
    {
        curl_share_mutex.lock();
    }

    static void curl_unlock_cb(CURL *handle, curl_lock_data data,
                               curl_lock_access access, void *userptr)
    {
        curl_share_mutex.unlock();
    }


    // Factory
    static boost::shared_ptr<Rdio_Scanner_Uploader> create() {
        return boost::shared_ptr<Rdio_Scanner_Uploader>(
            new Rdio_Scanner_Uploader());
    }
};

BOOST_DLL_ALIAS(
    Rdio_Scanner_Uploader::create,
    create_plugin
)

