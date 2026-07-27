// -*- coding: utf-8 -*-
// thingsboard_mqtt_demo.cpp — Simulate Jetson inspection gateway for ThingsBoard
#include "telemetry/thingsboard_mqtt_client.h"
#include "telemetry/image_http_server.h"
#include "core/logger.h"
#include "vision/camera.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>
#include <vector>
#include <unistd.h>

#include <dirent.h>

namespace
{

    std::atomic<bool> g_running{true};

    struct WifiSetupStatus
    {
        std::mutex mutex;
        std::string mode = "idle";
        std::string target_ssid;
        std::string last_error;
        std::string last_command;
    };

    std::string detect_ipv4(const std::string &preferred_iface);
    std::string mac_suffix(const std::string &mac);
    std::string read_file_all(const std::string &path);

    void handle_signal(int /*sig*/)
    {
        g_running = false;
    }

    long long now_ms()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    int getenv_int(const char *name, int fallback)
    {
        const char *value = std::getenv(name);
        if (!value || !value[0])
            return fallback;
        char *end = nullptr;
        long parsed = std::strtol(value, &end, 10);
        return end && *end == '\0' ? static_cast<int>(parsed) : fallback;
    }

    bool getenv_bool(const char *name, bool fallback)
    {
        const char *value = std::getenv(name);
        if (!value || !value[0])
            return fallback;
        std::string v = value;
        return v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "YES";
    }

    std::string getenv_or(const char *name, const std::string &fallback)
    {
        const char *value = std::getenv(name);
        return value && value[0] ? value : fallback;
    }

    std::string hostname()
    {
        char host[128];
        if (gethostname(host, sizeof(host)) == 0)
        {
            host[sizeof(host) - 1] = 0;
            return host;
        }
        return "jetson-inspect";
    }

    std::string read_text_file(const std::string &path)
    {
        std::ifstream f(path);
        std::string value;
        std::getline(f, value);
        return value;
    }

    bool valid_mac(const std::string &mac)
    {
        return mac.size() >= 17 && mac != "00:00:00:00:00:00";
    }

    std::string primary_mac_address()
    {
        const char *override_mac = std::getenv("TB_DEVICE_MAC");
        if (override_mac && override_mac[0])
            return override_mac;

        DIR *dir = opendir("/sys/class/net");
        if (!dir)
            return "unknown";

        std::string fallback;
        while (dirent *entry = readdir(dir))
        {
            std::string iface = entry->d_name;
            if (iface == "." || iface == ".." || iface == "lo")
                continue;

            std::string mac = read_text_file("/sys/class/net/" + iface + "/address");
            if (!valid_mac(mac))
                continue;

            if (fallback.empty())
                fallback = mac;
            if (iface.rfind("eth", 0) == 0 || iface.rfind("en", 0) == 0)
            {
                closedir(dir);
                return mac;
            }
        }

        closedir(dir);
        return fallback.empty() ? "unknown" : fallback;
    }

    double read_first_number(const std::string &path, double fallback = 0)
    {
        std::ifstream f(path);
        double v = fallback;
        f >> v;
        return v;
    }

    double uptime_sec()
    {
        return read_first_number("/proc/uptime", 0);
    }

    double cpu_temp_c()
    {
        double raw = read_first_number("/sys/class/thermal/thermal_zone0/temp", 0);
        return raw > 1000 ? raw / 1000.0 : raw;
    }

    double load_1m()
    {
        double loads[3] = {0, 0, 0};
        return getloadavg(loads, 3) > 0 ? loads[0] : 0;
    }

    double mem_used_pct()
    {
        std::ifstream f("/proc/meminfo");
        std::string key;
        double value = 0;
        std::string unit;
        double total = 0;
        double available = 0;
        while (f >> key >> value >> unit)
        {
            if (key == "MemTotal:")
                total = value;
            else if (key == "MemAvailable:")
                available = value;
        }
        return total > 0 ? (total - available) * 100.0 / total : 0;
    }

    double disk_used_pct()
    {
        struct statvfs st{};
        if (statvfs(".", &st) != 0 || st.f_blocks == 0)
            return 0;
        double used = static_cast<double>(st.f_blocks - st.f_bfree);
        return used * 100.0 / static_cast<double>(st.f_blocks);
    }

    std::string json_escape(const std::string &value)
    {
        std::ostringstream os;
        for (char c : value)
        {
            if (c == '\\')
                os << "\\\\";
            else if (c == '"')
                os << "\\\"";
            else if (c == '\n')
                os << "\\n";
            else
                os << c;
        }
        return os.str();
    }

    std::string json_string_field(const std::string &json, const std::string &key)
    {
        std::string marker = "\"" + key + "\"";
        std::size_t pos = json.find(marker);
        if (pos == std::string::npos)
            return "";
        pos = json.find(':', pos + marker.size());
        if (pos == std::string::npos)
            return "";
        pos = json.find('"', pos + 1);
        if (pos == std::string::npos)
            return "";

        std::string out;
        bool escaped = false;
        for (std::size_t i = pos + 1; i < json.size(); ++i)
        {
            char c = json[i];
            if (escaped)
            {
                switch (c)
                {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                default: out += c; break;
                }
                escaped = false;
            }
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                return out;
            else
                out += c;
        }
        return out;
    }

    std::string shell_quote(const std::string &value)
    {
        std::string out = "'";
        for (char c : value)
        {
            if (c == '\'')
                out += "'\\''";
            else
                out += c;
        }
        out += "'";
        return out;
    }

    std::string run_capture(const std::string &command)
    {
        std::string output;
        FILE *pipe = popen(command.c_str(), "r");
        if (!pipe)
            return output;
        char buffer[512];
        while (fgets(buffer, sizeof(buffer), pipe))
            output += buffer;
        pclose(pipe);
        return output;
    }

    struct HttpResponse
    {
        long status = 0;
        std::string body;
        std::string error;
    };

    std::string trim_copy(const std::string &value)
    {
        std::size_t begin = 0;
        while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
            begin++;
        std::size_t end = value.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
            end--;
        return value.substr(begin, end - begin);
    }

    std::string strip_trailing_slash(std::string value)
    {
        while (value.size() > 1 && value.back() == '/')
            value.pop_back();
        return value;
    }

    std::string bearer_auth_header(const std::string &auth_or_jwt)
    {
        std::string value = trim_copy(auth_or_jwt);
        if (value.empty())
            return "";
        if (value.rfind("Bearer ", 0) == 0 || value.rfind("bearer ", 0) == 0)
            return "Authorization: " + value;
        return "Authorization: Bearer " + value;
    }

    std::size_t curl_write_cb(char *ptr, std::size_t size, std::size_t nmemb, void *userdata)
    {
        auto *out = static_cast<std::string *>(userdata);
        out->append(ptr, size * nmemb);
        return size * nmemb;
    }

    std::string percent_escape(const std::string &value)
    {
        std::ostringstream os;
        os << std::uppercase << std::hex;
        for (unsigned char c : value)
        {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                os << static_cast<char>(c);
            else
                os << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c) << std::setfill(' ');
        }
        return os.str();
    }

    std::string url_escape(const std::string &value) { return percent_escape(value); }

    HttpResponse http_json_request(const std::string &method,
                                   const std::string &url,
                                   const std::string &auth_header,
                                   const std::string &body = "",
                                   const std::string &content_type = "application/json")
    {
        HttpResponse response;
        char body_template[] = "/tmp/jetson_tb_body_XXXXXX";
        int body_fd = mkstemp(body_template);
        if (body_fd < 0)
        {
            response.error = "mkstemp_body_failed";
            return response;
        }
        close(body_fd);

        std::string command = "curl -sS -L --connect-timeout 10 --max-time 25 ";
        command += "-X " + shell_quote(method) + " ";
        command += "-H " + shell_quote(auth_header) + " ";
        command += "-H " + shell_quote("Content-Type: " + content_type) + " ";
        command += "-H " + shell_quote("Accept: application/json") + " ";
        command += "-o " + shell_quote(body_template) + " -w '%{http_code}' ";
        if (method == "POST")
            command += "--data-binary " + shell_quote(body) + " ";
        command += shell_quote(url) + " 2>/tmp/jetson_tb_curl_error.log";

        std::string status_text = trim_copy(run_capture(command));
        response.body = read_file_all(body_template);
        std::remove(body_template);
        if (!status_text.empty() && std::isdigit(static_cast<unsigned char>(status_text[0])))
            response.status = std::strtol(status_text.c_str(), nullptr, 10);
        else
            response.error = read_file_all("/tmp/jetson_tb_curl_error.log");
        return response;
    }

    std::string uuid_from_json_near(const std::string &json, std::size_t start, std::size_t end)
    {
        if (start >= json.size())
            return "";
        if (end <= start || end > json.size())
            end = json.size();
        static const std::regex uuid_re("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}");
        std::smatch match;
        std::string window = json.substr(start, end - start);
        if (std::regex_search(window, match, uuid_re))
            return match.str(0);
        return "";
    }

    std::string device_uuid_from_response(const std::string &json)
    {
        std::size_t entity = json.find("\"entityType\":\"DEVICE\"");
        if (entity == std::string::npos)
            entity = json.find("\"entityType\" : \"DEVICE\"");
        std::size_t begin = entity == std::string::npos ? 0 : json.rfind("\"id\"", entity);
        if (begin == std::string::npos)
            begin = 0;
        return uuid_from_json_near(json, begin, entity == std::string::npos ? json.size() : entity + 80);
    }

    std::string exact_device_uuid_from_page(const std::string &json, const std::string &device_name)
    {
        std::string marker = "\"name\":\"" + json_escape(device_name) + "\"";
        std::size_t name_pos = json.find(marker);
        if (name_pos == std::string::npos)
            return "";
        std::size_t begin = json.rfind("\"id\"", name_pos);
        if (begin == std::string::npos)
            begin = 0;
        return uuid_from_json_near(json, begin, name_pos);
    }

    std::string credentials_uuid_from_response(const std::string &json)
    {
        std::size_t type_pos = json.find("\"credentialsType\"");
        if (type_pos == std::string::npos)
            type_pos = json.size();
        std::size_t begin = json.find("\"id\"");
        return uuid_from_json_near(json, begin == std::string::npos ? 0 : begin, type_pos);
    }

    std::string host_from_url(std::string url)
    {
        std::size_t scheme = url.find("://");
        if (scheme != std::string::npos)
            url = url.substr(scheme + 3);
        std::size_t slash = url.find('/');
        if (slash != std::string::npos)
            url = url.substr(0, slash);
        std::size_t colon = url.find(':');
        if (colon != std::string::npos)
            url = url.substr(0, colon);
        return url;
    }

    std::string mqtt_host_from_tb_url(const std::string &tb_url)
    {
        std::string configured = getenv_or("TB_ONBOARD_MQTT_HOST", "");
        if (!configured.empty())
            return configured;
        std::string host = host_from_url(tb_url);
        if (host == "thingsboard.cloud")
            return "mqtt.thingsboard.cloud";
        return host.empty() ? "mqtt.thingsboard.cloud" : host;
    }

    std::string suggested_tb_device_name(const std::string &mac,
                                         const std::string &current_device_name = "")
    {
        std::string configured = getenv_or("TB_ONBOARD_DEVICE_NAME", "");
        if (!configured.empty())
            return configured;
        if (!current_device_name.empty() && current_device_name != "unknown")
            return current_device_name;
        return "jetson-" + mac_suffix(mac);
    }

    bool ensure_dir(const std::string &path)
    {
        if (path.empty())
            return false;
        if (mkdir(path.c_str(), 0700) == 0 || errno == EEXIST)
            return true;
        return false;
    }

    std::string saved_env_path()
    {
        std::string configured = getenv_or("TB_CONFIG_ENV_FILE", "");
        if (!configured.empty())
            return configured;
        const char *home = std::getenv("HOME");
        if (!home || !home[0])
            return "/tmp/jetson-inspect-thingsboard.env";
        return std::string(home) + "/.config/jetson-inspect-v2/thingsboard.env";
    }

    void load_saved_thingsboard_env()
    {
        std::ifstream f(saved_env_path());
        if (!f)
            return;

        std::string line;
        while (std::getline(f, line))
        {
            line = trim_copy(line);
            if (line.empty() || line[0] == '#')
                continue;
            std::size_t eq = line.find('=');
            if (eq == std::string::npos || eq == 0)
                continue;
            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            if (std::getenv(key.c_str()) == nullptr)
                setenv(key.c_str(), value.c_str(), 0);
        }
    }

    bool save_thingsboard_env(const std::string &mqtt_host,
                              int mqtt_port,
                              const std::string &device_name,
                              const std::string &access_token,
                              const std::string &mac_address,
                              std::string *path_out)
    {
        const char *home = std::getenv("HOME");
        if (home && home[0])
        {
            ensure_dir(std::string(home) + "/.config");
            ensure_dir(std::string(home) + "/.config/jetson-inspect-v2");
        }

        std::string path = saved_env_path();
        std::ofstream out(path, std::ios::trunc);
        if (!out)
            return false;
        out << "TB_MQTT_HOST=" << mqtt_host << "\n"
            << "TB_MQTT_PORT=" << mqtt_port << "\n"
            << "TB_MQTT_CLIENT_ID=" << device_name << "\n"
            << "TB_MQTT_USERNAME=" << access_token << "\n"
            << "TB_MQTT_PASSWORD=" << access_token << "\n"
            << "TB_DEVICE_ID=" << device_name << "\n"
            << "TB_DEVICE_MAC=" << mac_address << "\n";
        out.close();
        chmod(path.c_str(), 0600);
        if (path_out)
            *path_out = path;
        return true;
    }

    std::string read_file_all(const std::string &path)
    {
        std::ifstream f(path);
        if (!f)
            return "";
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    std::vector<std::string> split_colon_line(const std::string &line)
    {
        std::vector<std::string> fields;
        std::string field;
        bool escaped = false;
        for (char c : line)
        {
            if (escaped)
            {
                field += c;
                escaped = false;
            }
            else if (c == '\\')
            {
                escaped = true;
            }
            else if (c == ':')
            {
                fields.push_back(field);
                field.clear();
            }
            else
            {
                field += c;
            }
        }
        fields.push_back(field);
        return fields;
    }

    std::string wifi_scan_json(const std::string &iface)
    {
        std::string command = "nmcli -t --escape yes -f SSID,SIGNAL,SECURITY dev wifi list ifname " +
                              shell_quote(iface) + " --rescan yes 2>/dev/null";
        std::string output = run_capture(command);
        bool from_cache = false;

        auto usable_network_count = [](const std::string &text) {
            std::istringstream lines(text);
            std::string line;
            int count = 0;
            while (std::getline(lines, line))
            {
                auto fields = split_colon_line(line);
                if (!fields.empty() && !fields[0].empty() && fields[0].rfind("JETSON-", 0) != 0)
                    count++;
            }
            return count;
        };

        if (output.empty() || usable_network_count(output) == 0)
        {
            std::string cached = read_file_all(getenv_or("JETSON_WIFI_SCAN_CACHE", "/tmp/jetson_wifi_scan_cache.txt"));
            if (!cached.empty())
            {
                output = cached;
                from_cache = true;
            }
        }

        std::ostringstream os;
        os << "{\"ok\":true,\"interface\":\"" << json_escape(iface) << "\""
           << ",\"source\":\"" << (from_cache ? "cache" : "live") << "\""
           << ",\"networks\":[";
        std::istringstream lines(output);
        std::string line;
        bool first = true;
        int count = 0;
        while (std::getline(lines, line) && count < 40)
        {
            auto fields = split_colon_line(line);
            if (fields.empty() || fields[0].empty())
                continue;
            if (fields[0].rfind("JETSON-", 0) == 0)
                continue;
            if (!first)
                os << ",";
            first = false;
            count++;
            std::string signal = fields.size() > 1 ? fields[1] : "0";
            std::string security = fields.size() > 2 ? fields[2] : "";
            os << "{\"ssid\":\"" << json_escape(fields[0]) << "\""
               << ",\"signal\":" << (signal.empty() ? "0" : signal)
               << ",\"security\":\"" << json_escape(security) << "\"}";
        }
        os << "]}";
        return os.str();
    }

    std::string setup_status_json(WifiSetupStatus &status, const std::string &iface)
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        std::string ip = detect_ipv4(iface);
        std::ostringstream os;
        os << "{\"ok\":true"
           << ",\"mode\":\"" << json_escape(status.mode) << "\""
           << ",\"interface\":\"" << json_escape(iface) << "\""
           << ",\"target_ssid\":\"" << json_escape(status.target_ssid) << "\""
           << ",\"ip\":\"" << json_escape(ip) << "\""
           << ",\"last_error\":\"" << json_escape(status.last_error) << "\""
           << "}";
        return os.str();
    }

    std::string schedule_wifi_connect(WifiSetupStatus &status,
                                      const std::string &iface,
                                      const std::string &ssid,
                                      const std::string &password)
    {
        if (ssid.empty())
            return "{\"ok\":false,\"error\":\"ssid_required\"}";

        {
            std::lock_guard<std::mutex> lock(status.mutex);
            status.mode = "connecting";
            status.target_ssid = ssid;
            status.last_error.clear();
            status.last_command = "nmcli dev wifi connect";
        }

        std::thread([&status, iface, ssid, password]() {
            int switch_delay_sec = getenv_int("JETSON_WIFI_SWITCH_DELAY_SEC", 8);
            if (switch_delay_sec < 2)
                switch_delay_sec = 2;
            std::this_thread::sleep_for(std::chrono::seconds(switch_delay_sec));
            std::string setup_con = getenv_or("JETSON_SETUP_CON_NAME", "jetson-setup-ap");
            std::string command = "date > /tmp/jetson_wifi_connect.log; ";
            command += "sudo -n /usr/bin/nmcli con down " + shell_quote(setup_con) + " >>/tmp/jetson_wifi_connect.log 2>&1 || true; ";
            command += "sudo -n /usr/bin/nmcli con delete " + shell_quote(setup_con) + " >>/tmp/jetson_wifi_connect.log 2>&1 || true; ";
            command += "sleep 1; ";
            command += "sudo -n /usr/bin/nmcli radio wifi on >>/tmp/jetson_wifi_connect.log 2>&1 || true; ";
            command += "sudo -n /usr/bin/nmcli dev wifi connect " + shell_quote(ssid);
            if (!password.empty())
                command += " password " + shell_quote(password);
            command += " ifname " + shell_quote(iface) + " >>/tmp/jetson_wifi_connect.log 2>&1";

            log_msg(LOG_WARNING, "WiFi provisioning switching interface=%s ssid=%s", iface.c_str(), ssid.c_str());
            int rc = std::system(command.c_str());
            std::lock_guard<std::mutex> lock(status.mutex);
            if (rc == 0)
            {
                status.mode = "connected";
                status.last_error.clear();
            }
            else
            {
                status.mode = "failed";
                status.last_error = "nmcli_connect_failed_rc_" + std::to_string(rc);
            }
            log_msg(LOG_WARNING, "WiFi provisioning command finished: ssid=%s rc=%d", ssid.c_str(), rc);
        }).detach();

        int switch_delay_sec = getenv_int("JETSON_WIFI_SWITCH_DELAY_SEC", 8);
        if (switch_delay_sec < 2)
            switch_delay_sec = 2;
        return std::string("{\"ok\":true,\"mode\":\"connecting\",\"switch_delay_sec\":") +
               std::to_string(switch_delay_sec) + ",\"message\":" +
               "\"WiFi config accepted. Jetson will switch networks after the countdown.\"" +
               ",\"target_ssid\":\"" + json_escape(ssid) + "\"}";
    }

    std::string mac_suffix(const std::string &mac)
    {
        std::string hex;
        for (unsigned char c : mac)
        {
            if (std::isxdigit(c))
                hex.push_back(static_cast<char>(std::toupper(c)));
        }
        if (hex.size() >= 6)
            return hex.substr(hex.size() - 6);
        return "UNKNOWN";
    }

    std::string setup_ssid_for_mac(const std::string &mac)
    {
        std::string fallback = "JETSON-" + mac_suffix(mac);
        return getenv_or("JETSON_SETUP_SSID", getenv_or("TB_SETUP_WIFI_SSID", fallback));
    }

    bool usable_ipv4(const std::string &ip)
    {
        return !ip.empty() && ip != "0.0.0.0" && ip.rfind("127.", 0) != 0;
    }

    std::string detect_ipv4(const std::string &preferred_iface)
    {
        ifaddrs *addrs = nullptr;
        if (getifaddrs(&addrs) != 0 || !addrs)
            return "";

        auto find_ip = [&](const std::string &iface) {
            for (ifaddrs *it = addrs; it; it = it->ifa_next)
            {
                if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET)
                    continue;
                if ((it->ifa_flags & IFF_UP) == 0 || (it->ifa_flags & IFF_LOOPBACK) != 0)
                    continue;
                if (!iface.empty() && iface != it->ifa_name)
                    continue;

                char buf[INET_ADDRSTRLEN] = {0};
                const auto *addr = reinterpret_cast<const sockaddr_in *>(it->ifa_addr);
                if (!inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf)))
                    continue;
                std::string ip = buf;
                if (usable_ipv4(ip))
                    return ip;
            }
            return std::string();
        };

        std::string ip = find_ip(preferred_iface);
        if (ip.empty())
            ip = find_ip("");
        freeifaddrs(addrs);
        return ip;
    }

    std::string setup_iface()
    {
        return getenv_or("JETSON_SETUP_IFACE",
                         getenv_or("JETSON_WIFI_IFACE", getenv_or("TB_SETUP_WIFI_IFACE", "wlan0")));
    }

    std::string setup_ip()
    {
        std::string configured = getenv_or("JETSON_SETUP_IP", getenv_or("TB_SETUP_WIFI_IP", ""));
        if (!configured.empty())
            return configured;

        std::string detected = detect_ipv4(setup_iface());
        return detected.empty() ? "" : detected;
    }

    std::string local_api_base_url(const std::string &ip, int port, const std::string &fallback_base_url)
    {
        std::string configured = getenv_or("JETSON_SETUP_API_BASE_URL", getenv_or("TB_LOCAL_API_BASE_URL", ""));
        if (!configured.empty())
            return configured;
        if (usable_ipv4(ip))
        {
            std::ostringstream fallback;
            fallback << "http://" << ip << ":" << port;
            return fallback.str();
        }
        return fallback_base_url;
    }

    std::string device_info_json(const std::string &device_id,
                                 const std::string &mac_address,
                                 const std::string &setup_ssid,
                                 const std::string &setup_ip,
                                 const std::string &setup_base_url,
                                 const telemetry::ThingsBoardMqttConfig &config,
                                 const telemetry::ImageHttpServer &image_server,
                                 const std::string &project_version)
    {
        std::ostringstream os;
        os << "{\"schema\":\"jetson.provision.v1\""
           << ",\"device\":{"
           << "\"id\":\"" << json_escape(device_id) << "\""
           << ",\"name\":\"" << json_escape(device_id) << "\""
           << ",\"hostname\":\"" << json_escape(hostname()) << "\""
           << ",\"mac\":\"" << json_escape(mac_address) << "\""
           << ",\"ssid\":\"" << json_escape(setup_ssid) << "\""
           << ",\"suggested_tb_device_name\":\"" << json_escape(suggested_tb_device_name(mac_address, device_id)) << "\""
           << "},\"project\":{"
           << "\"name\":\"jetson-inspect-v2\""
           << ",\"version\":\"" << json_escape(project_version) << "\""
           << ",\"mode\":\"thingsboard-demo\""
           << "},\"network\":{"
           << "\"setup_ip\":\"" << json_escape(setup_ip) << "\""
           << ",\"local_api_base_url\":\"" << json_escape(setup_base_url) << "\""
           << ",\"device_info_url\":\"" << json_escape(setup_base_url + "/device-info") << "\""
           << ",\"setup_page_url\":\"" << json_escape(setup_base_url + "/setup") << "\""
           << ",\"wifi_scan_url\":\"" << json_escape(setup_base_url + "/wifi/scan") << "\""
           << ",\"wifi_connect_url\":\"" << json_escape(setup_base_url + "/wifi/connect") << "\""
           << ",\"tb_onboard_url\":\"" << json_escape(setup_base_url + "/tb/onboard") << "\""
           << ",\"command_url\":\"" << json_escape(setup_base_url + "/api/command") << "\""
           << ",\"snapshot_url\":\"" << json_escape(setup_base_url + "/snapshot.jpg") << "\""
           << ",\"stream_url\":\"" << json_escape(setup_base_url + "/stream.mjpg") << "\""
           << ",\"health_url\":\"" << json_escape(setup_base_url + "/health") << "\""
           << "},\"public_endpoints\":{"
           << "\"snapshot_url\":\"" << json_escape(image_server.snapshot_url()) << "\""
           << ",\"stream_url\":\"" << json_escape(image_server.stream_url()) << "\""
           << ",\"health_url\":\"" << json_escape(image_server.health_url()) << "\""
           << ",\"device_info_url\":\"" << json_escape(image_server.device_info_url()) << "\""
           << "},\"thingsboard\":{"
           << "\"mqtt_host\":\"" << json_escape(config.host) << "\""
           << ",\"mqtt_port\":" << config.port
           << ",\"mqtt_client_id\":\"" << json_escape(config.client_id) << "\""
           << ",\"device_name\":\"" << json_escape(device_id) << "\""
           << "},\"capabilities\":{"
           << "\"telemetry\":true"
           << ",\"wifi_provisioning\":true"
           << ",\"image_stream\":true"
           << ",\"rpc_control\":true"
           << ",\"power_control\":" << (getenv_bool("TB_ALLOW_POWER_RPC", false) ? "true" : "false")
           << "}}";
        return os.str();
    }

    std::string tb_error_json(const std::string &error,
                              const HttpResponse &response = HttpResponse{})
    {
        std::ostringstream os;
        os << "{\"ok\":false,\"error\":\"" << json_escape(error) << "\"";
        if (response.status > 0)
            os << ",\"tb_status\":" << response.status;
        if (!response.error.empty())
            os << ",\"detail\":\"" << json_escape(response.error) << "\"";
        if (!response.body.empty())
        {
            std::string body = response.body.substr(0, 700);
            os << ",\"tb_body\":\"" << json_escape(body) << "\"";
        }
        os << "}";
        return os.str();
    }

    std::string thingsboard_onboard_json(const telemetry::HttpRequest &req,
                                         const std::string &current_device_name,
                                         const std::string &mac_address,
                                         const std::string &setup_ssid,
                                         const std::string &setup_base_url,
                                         const telemetry::ImageHttpServer &image_server,
                                         const std::string &project_version)
    {
        if (req.method != "POST")
            return "{\"ok\":false,\"error\":\"post_required\"}";

        std::string tb_url = strip_trailing_slash(json_string_field(req.body, "tbUrl"));
        if (tb_url.empty())
            tb_url = strip_trailing_slash(getenv_or("TB_REST_URL", "https://thingsboard.cloud"));
        if (tb_url.empty())
            return "{\"ok\":false,\"error\":\"tb_url_required\"}";

        std::string auth = json_string_field(req.body, "auth");
        if (auth.empty())
            auth = json_string_field(req.body, "jwt");
        std::string auth_header = bearer_auth_header(auth);
        if (auth_header.empty())
            return "{\"ok\":false,\"error\":\"thingsboard_jwt_required\"}";

        std::string device_name = json_string_field(req.body, "deviceName");
        if (device_name.empty())
            device_name = suggested_tb_device_name(mac_address, current_device_name);

        std::string access_token = json_string_field(req.body, "accessToken");
        if (access_token.empty())
            access_token = json_string_field(req.body, "mqttAccessToken");
        if (access_token.empty())
            access_token = device_name;

        std::string mqtt_host = json_string_field(req.body, "mqttHost");
        if (mqtt_host.empty())
            mqtt_host = mqtt_host_from_tb_url(tb_url);
        int mqtt_port = getenv_int("TB_ONBOARD_MQTT_PORT", 1883);

        std::string label = "Jetson " + mac_suffix(mac_address);
        std::ostringstream device_body;
        device_body << "{\"name\":\"" << json_escape(device_name) << "\""
                    << ",\"type\":\"jetson-inspect-v2\""
                    << ",\"label\":\"" << json_escape(label) << "\""
                    << ",\"additionalInfo\":{"
                    << "\"mac_address\":\"" << json_escape(mac_address) << "\""
                    << ",\"setup_ssid\":\"" << json_escape(setup_ssid) << "\""
                    << ",\"project\":\"jetson-inspect-v2\""
                    << "}}";

        std::string device_id;
        std::string save_url = tb_url + "/api/device?accessToken=" + url_escape(access_token);
        HttpResponse save_response = http_json_request("POST", save_url, auth_header, device_body.str());
        bool created_or_updated = save_response.status >= 200 && save_response.status < 300;
        if (created_or_updated)
            device_id = device_uuid_from_response(save_response.body);

        if (device_id.empty())
        {
            std::string search_url = tb_url + "/api/tenant/devices?pageSize=50&page=0&textSearch=" + url_escape(device_name);
            HttpResponse search_response = http_json_request("GET", search_url, auth_header);
            if (search_response.status < 200 || search_response.status >= 300)
                return tb_error_json("thingsboard_device_create_and_search_failed", save_response.status ? save_response : search_response);
            device_id = exact_device_uuid_from_page(search_response.body, device_name);
            if (device_id.empty())
                return tb_error_json("thingsboard_device_not_found_after_create", save_response);
        }

        std::string credential_warning;
        std::string credentials_url = tb_url + "/api/device/" + device_id + "/credentials";
        HttpResponse get_credentials = http_json_request("GET", credentials_url, auth_header);
        std::string credentials_id;
        if (get_credentials.status >= 200 && get_credentials.status < 300)
            credentials_id = credentials_uuid_from_response(get_credentials.body);

        std::ostringstream credentials_body;
        credentials_body << "{";
        if (!credentials_id.empty())
            credentials_body << "\"id\":{\"id\":\"" << json_escape(credentials_id) << "\"},";
        credentials_body << "\"deviceId\":{\"id\":\"" << json_escape(device_id) << "\",\"entityType\":\"DEVICE\"}"
                         << ",\"credentialsType\":\"ACCESS_TOKEN\""
                         << ",\"credentialsId\":\"" << json_escape(access_token) << "\""
                         << "}";
        HttpResponse credentials_response = http_json_request("POST", tb_url + "/api/device/credentials",
                                                              auth_header, credentials_body.str());
        if (credentials_response.status < 200 || credentials_response.status >= 300)
            credential_warning = "credentials_update_failed_status_" + std::to_string(credentials_response.status);

        std::ostringstream attrs_body;
        attrs_body << "{"
                   << "\"mac_address\":\"" << json_escape(mac_address) << "\""
                   << ",\"device_name\":\"" << json_escape(device_name) << "\""
                   << ",\"jetson_onboard\":{";
        attrs_body << "\"schema\":\"jetson.onboard.v1\""
                   << ",\"mac_address\":\"" << json_escape(mac_address) << "\""
                   << ",\"setup_ssid\":\"" << json_escape(setup_ssid) << "\""
                   << ",\"local_api_base_url\":\"" << json_escape(setup_base_url) << "\""
                   << ",\"project\":\"jetson-inspect-v2\""
                   << ",\"project_version\":\"" << json_escape(project_version) << "\""
                   << ",\"snapshot_url\":\"" << json_escape(image_server.snapshot_url()) << "\""
                   << ",\"stream_url\":\"" << json_escape(image_server.stream_url()) << "\""
                   << ",\"health_url\":\"" << json_escape(image_server.health_url()) << "\""
                   << "}}";

        HttpResponse attrs_response = http_json_request("POST",
                                                        tb_url + "/api/plugins/telemetry/DEVICE/" + device_id + "/SERVER_SCOPE",
                                                        auth_header, attrs_body.str(), "text/plain");
        if (attrs_response.status < 200 || attrs_response.status >= 300)
            return tb_error_json("thingsboard_attributes_save_failed", attrs_response);

        std::string env_path;
        bool env_saved = save_thingsboard_env(mqtt_host, mqtt_port, device_name, access_token,
                                              mac_address, &env_path);
        std::ostringstream out;
        out << "{\"ok\":true"
            << ",\"device\":{\"id\":\"" << json_escape(device_id) << "\""
            << ",\"name\":\"" << json_escape(device_name) << "\""
            << ",\"mac\":\"" << json_escape(mac_address) << "\"}"
            << ",\"mqtt\":{\"host\":\"" << json_escape(mqtt_host) << "\""
            << ",\"port\":" << mqtt_port
            << ",\"client_id\":\"" << json_escape(device_name) << "\""
            << ",\"username\":\"" << json_escape(access_token) << "\""
            << ",\"access_token\":\"" << json_escape(access_token) << "\"}"
            << ",\"saved_env\":{\"ok\":" << (env_saved ? "true" : "false")
            << ",\"path\":\"" << json_escape(env_path) << "\"}"
            << ",\"restart_required\":true";
        if (!credential_warning.empty())
            out << ",\"warning\":\"" << json_escape(credential_warning) << "\"";
        out << "}";
        return out.str();
    }

    telemetry::InspectionTelemetry make_cycle(const std::string &device_id,
                                              const std::string &mac_address,
                                              int product_id,
                                              int total_ok,
                                              int total_ng)
    {
        telemetry::InspectionTelemetry t;
        t.ts_ms = now_ms();
        t.device_id = device_id;
        t.mac_address = mac_address;
        t.product_id = product_id;
        t.total_ok = total_ok;
        t.total_ng = total_ng;
        t.cycle_rate = 1.8;
        t.app_state = AppState::RESULT_SHOWN;

        bool is_ng = product_id % 7 == 0;
        t.result.result = is_ng ? ProductResult::NG : ProductResult::OK;
        t.result.label = is_ng ? "NG" : "OK";
        t.result.info_text = is_ng ? "NG spike anomaly" : "OK shape stable";
        t.result.has_metrics = true;
        t.result.cycle_ms = 32.0 + (product_id % 5) * 2.5;

        t.result.metrics.area = is_ng ? 4710.0 : 5230.0;
        t.result.metrics.perimeter = is_ng ? 415.0 : 388.0;
        t.result.metrics.solidity = is_ng ? 0.72 : 0.93;
        t.result.metrics.width = is_ng ? 176 : 180;
        t.result.metrics.height = is_ng ? 236 : 240;
        t.result.metrics.top_points = is_ng ? 8 : 4;
        t.result.metrics.top_width = is_ng ? 41 : 55;
        t.result.metrics.top_width_ratio = is_ng ? 0.228 : 0.305;
        t.result.metrics.spike_ratio = is_ng ? 0.087 : 0.021;
        t.result.metrics.spike_min_w = is_ng ? 11 : 24;
        t.result.metrics.spike_max_w = is_ng ? 77 : 64;
        return t;
    }

    telemetry::DeviceHealthTelemetry make_health(const std::string &device_id,
                                                 const std::string &mac_address,
                                                 bool mqtt_connected,
                                                 bool camera_ok,
                                                 bool image_server_ok,
                                                 std::size_t mqtt_dropped)
    {
        telemetry::DeviceHealthTelemetry h;
        h.ts_ms = now_ms();
        h.device_id = device_id;
        h.mac_address = mac_address;
        h.hostname = hostname();
        h.uptime_sec = uptime_sec();
        h.cpu_load_1m = load_1m();
        h.mem_used_pct = mem_used_pct();
        h.disk_used_pct = disk_used_pct();
        h.cpu_temp_c = cpu_temp_c();
        h.camera_ok = camera_ok;
        h.gpio_ok = true;
        h.mqtt_connected = mqtt_connected;
        h.image_server_ok = image_server_ok;
        h.mqtt_dropped = mqtt_dropped;
        return h;
    }

    std::string health_json(const telemetry::DeviceHealthTelemetry &h)
    {
        std::ostringstream os;
        os << std::fixed << std::setprecision(2);
        os << "{\"device_id\":\"" << json_escape(h.device_id) << "\""
           << ",\"hostname\":\"" << json_escape(h.hostname) << "\""
           << ",\"uptime_sec\":" << h.uptime_sec
           << ",\"cpu_load_1m\":" << h.cpu_load_1m
           << ",\"mem_used_pct\":" << h.mem_used_pct
           << ",\"disk_used_pct\":" << h.disk_used_pct
           << ",\"cpu_temp_c\":" << h.cpu_temp_c
           << ",\"camera_ok\":" << (h.camera_ok ? "true" : "false")
           << ",\"mqtt_connected\":" << (h.mqtt_connected ? "true" : "false")
           << ",\"image_server_ok\":" << (h.image_server_ok ? "true" : "false")
           << ",\"mqtt_dropped\":" << h.mqtt_dropped << "}";
        return os.str();
    }

    std::string inspection_value_json(const telemetry::InspectionTelemetry &telemetry)
    {
        const auto &r = telemetry.result;
        const auto &m = r.metrics;

        std::ostringstream os;
        os << std::fixed << std::setprecision(3);
        os << "{\"schema\":\"" << json_escape(telemetry.schema_version) << "\""
           << ",\"device\":{\"id\":\"" << json_escape(telemetry.device_id) << "\""
           << ",\"mac\":\"" << json_escape(telemetry.mac_address) << "\"}"
           << ",\"product\":{"
           << "\"id\":" << telemetry.product_id
           << ",\"result\":\"" << json_escape(r.label) << "\""
           << ",\"is_ok\":" << (r.result == ProductResult::OK ? "true" : "false")
           << ",\"is_ng\":" << (r.result == ProductResult::NG ? "true" : "false")
           << "},\"counter\":{"
           << "\"ok\":" << telemetry.total_ok
           << ",\"ng\":" << telemetry.total_ng
           << ",\"total\":" << (telemetry.total_ok + telemetry.total_ng)
           << ",\"cycle_rate\":" << telemetry.cycle_rate
           << "},\"process\":{"
           << "\"app_state\":\"" << state_name(telemetry.app_state) << "\""
           << ",\"cycle_ms\":" << r.cycle_ms
           << ",\"info\":\"" << json_escape(r.info_text) << "\""
           << "},\"metrics\":{"
           << "\"available\":" << (r.has_metrics ? "true" : "false");

        if (r.has_metrics)
        {
            os << ",\"area\":" << m.area
               << ",\"perimeter\":" << m.perimeter
               << ",\"solidity\":" << m.solidity
               << ",\"width\":" << m.width
               << ",\"height\":" << m.height
               << ",\"top_points\":" << m.top_points
               << ",\"top_width\":" << m.top_width
               << ",\"top_width_ratio\":" << m.top_width_ratio
               << ",\"spike_ratio\":" << m.spike_ratio
               << ",\"spike_min_w\":" << m.spike_min_w
               << ",\"spike_max_w\":" << m.spike_max_w;
        }

        os << "}}";
        return os.str();
    }

    std::string health_value_json(const telemetry::DeviceHealthTelemetry &h)
    {
        std::ostringstream os;
        os << std::fixed << std::setprecision(2);
        os << "{\"schema\":\"" << json_escape(h.schema_version) << "\""
           << ",\"device\":{\"id\":\"" << json_escape(h.device_id) << "\""
           << ",\"mac\":\"" << json_escape(h.mac_address) << "\""
           << ",\"hostname\":\"" << json_escape(h.hostname) << "\"}"
           << ",\"system\":{"
           << "\"uptime_sec\":" << h.uptime_sec
           << ",\"cpu_load_1m\":" << h.cpu_load_1m
           << ",\"mem_used_pct\":" << h.mem_used_pct
           << ",\"disk_used_pct\":" << h.disk_used_pct
           << ",\"cpu_temp_c\":" << h.cpu_temp_c
           << "},\"services\":{"
           << "\"camera_ok\":" << (h.camera_ok ? "true" : "false")
           << ",\"gpio_ok\":" << (h.gpio_ok ? "true" : "false")
           << ",\"mqtt_connected\":" << (h.mqtt_connected ? "true" : "false")
           << ",\"image_server_ok\":" << (h.image_server_ok ? "true" : "false")
           << ",\"mqtt_dropped\":" << h.mqtt_dropped
           << "}}";
        return os.str();
    }

    cv::Mat make_demo_frame(const telemetry::InspectionTelemetry &telemetry,
                            bool light_enabled,
                            bool calibration_mode)
    {
        cv::Mat frame(480, 720, CV_8UC3, cv::Scalar(28, 33, 38));
        cv::Scalar result_color = telemetry.result.result == ProductResult::NG
                                      ? cv::Scalar(55, 70, 230)
                                      : cv::Scalar(70, 190, 95);
        cv::rectangle(frame, {40, 40}, {680, 390}, cv::Scalar(65, 72, 82), 2);
        cv::rectangle(frame, {210, 90}, {510, 350}, result_color, 3);
        cv::line(frame, {260, 110}, {300, 165}, result_color, 2);
        cv::line(frame, {465, 112}, {425, 170}, result_color, 2);

        cv::putText(frame, "jetson-inspect-v2", {40, 28}, cv::FONT_HERSHEY_SIMPLEX,
                    0.75, cv::Scalar(230, 230, 230), 2);
        cv::putText(frame, "RESULT: " + telemetry.result.label, {60, 440}, cv::FONT_HERSHEY_SIMPLEX,
                    0.9, result_color, 2);
        cv::putText(frame, "OK " + std::to_string(telemetry.total_ok) + "  NG " + std::to_string(telemetry.total_ng) + "  ID " + std::to_string(telemetry.product_id),
                    {60, 470}, cv::FONT_HERSHEY_SIMPLEX, 0.65, cv::Scalar(230, 230, 230), 2);
        cv::putText(frame, light_enabled ? "LIGHT ON" : "LIGHT OFF", {520, 440},
                    cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(220, 220, 220), 1);
        cv::putText(frame, calibration_mode ? "CALIB" : "RUN", {520, 470},
                    cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(220, 220, 220), 1);
        return frame;
    }

    std::string schedule_system_command(const std::string &action, const std::string &command)
    {
        if (!getenv_bool("TB_ALLOW_POWER_RPC", false))
        {
            return std::string("{\"ok\":false,\"action\":\"") + json_escape(action) +
                   "\",\"error\":\"power_rpc_disabled\",\"hint\":\"set TB_ALLOW_POWER_RPC=1\"}";
        }

        std::thread([action, command]() {
            log_msg(LOG_WARNING, "RPC power action scheduled: %s", action.c_str());
            std::this_thread::sleep_for(std::chrono::seconds(1));
            int rc = std::system(command.c_str());
            log_msg(LOG_WARNING, "RPC power action command exited: action=%s rc=%d", action.c_str(), rc);
        }).detach();

        return std::string("{\"ok\":true,\"action\":\"") + json_escape(action) + "\",\"scheduled\":true}";
    }

} // namespace

int main()
{
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    load_saved_thingsboard_env();

    auto image_config = telemetry::ImageHttpServerConfig::from_env();
    telemetry::ImageHttpServer image_server(image_config);
    bool image_server_ok = image_server.start();
    if (!image_server_ok)
    {
        return EXIT_FAILURE;
    }

    auto config = telemetry::ThingsBoardMqttConfig::from_env();
    if (std::getenv("TB_MQTT_HOST") == nullptr && std::getenv("TB_HOST") == nullptr)
    {
        config.host = "mqtt.thingsboard.cloud";
    }
    if (std::getenv("TB_MQTT_CLIENT_ID") == nullptr)
    {
        config.client_id = "jetson-1";
    }
    if (std::getenv("TB_MQTT_USERNAME") == nullptr && std::getenv("TB_USERNAME") == nullptr &&
        std::getenv("TB_MQTT_ACCESS_TOKEN") == nullptr && std::getenv("TB_ACCESS_TOKEN") == nullptr)
    {
        config.username = "jetson-1";
        config.access_token = config.username;
    }
    if (std::getenv("TB_MQTT_PASSWORD") == nullptr && std::getenv("TB_PASSWORD") == nullptr)
    {
        config.password = "jetson-1";
    }

    telemetry::ThingsBoardMqttClient client(config);

    std::atomic<bool> light_enabled{false};
    std::atomic<bool> calibration_mode{false};
    std::atomic<bool> reset_requested{false};
    const std::string device_id = getenv_or("TB_DEVICE_ID", config.client_id);
    const std::string mac_address = primary_mac_address();
    const std::string project_version = getenv_or("TB_PROJECT_VERSION", "demo");
    const std::string setup_network_iface = setup_iface();
    const std::string setup_ap_ssid = setup_ssid_for_mac(mac_address);
    const std::string setup_ap_ip = setup_ip();
    const std::string setup_base_url = local_api_base_url(setup_ap_ip, image_config.port,
                                                          image_config.base_url());
    WifiSetupStatus wifi_setup_status;
    wifi_setup_status.mode = "setup";
    std::mutex local_telemetry_mutex;
    long long latest_inspection_ts = 0;
    long long latest_health_ts = 0;
    std::string latest_inspection_value = "{}";
    std::string latest_health_value = "{}";

    auto handle_remote_command = [&](const std::string &method,
                                     const std::string &params_json) -> std::string {
        log_msg(LOG_WARNING, "Remote command method=%s params=%s", method.c_str(), params_json.c_str());
        if (method == "ping")
            return "{\"ok\":true,\"message\":\"pong\"}";
        if (method == "setLight")
        {
            bool enabled = params_json.find("true") != std::string::npos ||
                           params_json.find("1") != std::string::npos;
            light_enabled = enabled;
            return std::string("{\"ok\":true,\"light_enabled\":") + (enabled ? "true}" : "false}");
        }
        if (method == "setCalibrationMode")
        {
            bool enabled = params_json.find("true") != std::string::npos ||
                           params_json.find("1") != std::string::npos;
            calibration_mode = enabled;
            return std::string("{\"ok\":true,\"calibration_mode\":") + (enabled ? "true}" : "false}");
        }
        if (method == "resetCounters")
        {
            reset_requested = true;
            return "{\"ok\":true,\"action\":\"resetCounters\"}";
        }
        if (method == "captureSnapshot")
        {
            return std::string("{\"ok\":true,\"snapshot_url\":\"") +
                   json_escape(image_server.snapshot_url()) + "\"}";
        }
        if (method == "resetJetson" || method == "rebootJetson")
            return schedule_system_command(method, "sudo /sbin/shutdown -r now");
        if (method == "shutdownJetson" || method == "powerOffJetson")
            return schedule_system_command(method, "sudo /sbin/shutdown -h now");
        if (method == "restartApp")
            return "{\"ok\":true,\"accepted\":false,\"reason\":\"demo_mode\"}";
        return "{\"ok\":false,\"error\":\"unknown_method\"}";
    };

    image_server.set_setup_handler([&](const telemetry::HttpRequest &req) -> std::string {
        if (req.path == "/wifi/scan")
            return wifi_scan_json(setup_network_iface);
        if (req.path == "/setup/status")
            return setup_status_json(wifi_setup_status, setup_network_iface);
        if (req.path == "/wifi/connect")
        {
            if (req.method != "POST")
                return "{\"ok\":false,\"error\":\"post_required\"}";
            std::string ssid = json_string_field(req.body, "ssid");
            std::string password = json_string_field(req.body, "password");
            return schedule_wifi_connect(wifi_setup_status, setup_network_iface, ssid, password);
        }
        if (req.path == "/tb/onboard")
        {
            return thingsboard_onboard_json(req, device_id, mac_address, setup_ap_ssid, setup_base_url,
                                           image_server, project_version);
        }
        if (req.path == "/api/telemetry" || req.path == "/telemetry")
        {
            std::lock_guard<std::mutex> lock(local_telemetry_mutex);
            std::ostringstream out;
            out << "{\"ok\":true,\"mac\":\"" << json_escape(mac_address) << "\""
                << ",\"telemetry\":{";
            bool wrote = false;
            if (latest_inspection_ts > 0)
            {
                out << "\"jetson_inspection\":[{\"ts\":" << latest_inspection_ts
                    << ",\"value\":" << latest_inspection_value << "}]";
                wrote = true;
            }
            if (latest_health_ts > 0)
            {
                if (wrote)
                    out << ",";
                out << "\"jetson_health\":[{\"ts\":" << latest_health_ts
                    << ",\"value\":" << latest_health_value << "}]";
            }
            out << "}}";
            return out.str();
        }
        if (req.path == "/api/command" || req.path == "/command")
        {
            if (req.method != "POST")
                return "{\"ok\":false,\"error\":\"post_required\"}";
            std::string method = json_string_field(req.body, "method");
            if (method.empty())
                return "{\"ok\":false,\"error\":\"method_required\"}";
            return handle_remote_command(method, req.body);
        }
        return "{\"ok\":false,\"error\":\"unknown_setup_endpoint\"}";
    });

    client.set_rpc_handler([&](const telemetry::RpcRequest &req) -> std::string
                           {
        return handle_remote_command(req.method, req.params_json); });

    telemetry::ProjectInfoAttributes project_attrs;
    project_attrs.device_id = device_id;
    project_attrs.mac_address = mac_address;
    project_attrs.project_name = "jetson-inspect-v2";
    project_attrs.project_version = project_version;
    project_attrs.app_mode = "thingsboard-demo";
    project_attrs.line_id = getenv_or("TB_LINE_ID", "line-01");
    project_attrs.station_id = getenv_or("TB_STATION_ID", "station-01");

    telemetry::ImageEndpointAttributes image_attrs;
    image_attrs.device_id = device_id;
    image_attrs.mac_address = mac_address;
    image_attrs.snapshot_url = image_server.snapshot_url();
    image_attrs.stream_url = image_server.stream_url();
    image_attrs.health_url = image_server.health_url();
    image_attrs.jpeg_quality = image_config.jpeg_quality;
    image_attrs.stream_fps = image_config.stream_fps;

    image_server.set_device_info_json(device_info_json(device_id, mac_address, setup_ap_ssid,
                                                       setup_ap_ip, setup_base_url, config,
                                                       image_server, project_version));

    telemetry::DeviceIdentityAttributes identity_attrs;
    identity_attrs.device_id = device_id;
    identity_attrs.mac_address = mac_address;
    identity_attrs.setup_ssid = setup_ap_ssid;
    identity_attrs.setup_ip = setup_ap_ip;
    identity_attrs.device_info_url = setup_base_url + "/device-info";
    identity_attrs.mqtt_host = config.host;
    identity_attrs.mqtt_client_id = config.client_id;

    bool static_attrs_published = false;
    auto publish_static_attributes = [&]() {
        if (static_attrs_published)
            return;
        client.publish_attributes(project_attrs);
        client.publish_image_attributes(image_attrs);
        client.publish_identity_attributes(identity_attrs);
        static_attrs_published = true;
    };

    bool mqtt_started = client.start();
    if (mqtt_started)
    {
        publish_static_attributes();
    }
    else
    {
        log_msg(LOG_WARNING, "ThingsBoard MQTT not available yet; local setup server keeps running");
    }

    const int interval_ms = getenv_int("TB_DEMO_INTERVAL_MS", 1000);
    const int max_cycles = getenv_int("TB_DEMO_CYCLES", 0); // 0 = run until stopped
    const bool use_real_camera = getenv_bool("TB_DEMO_USE_REAL_CAMERA", true);
    const bool fallback_demo_frame = getenv_bool("TB_DEMO_CAMERA_FALLBACK_FRAME", true);
    int camera_retry_cycles = getenv_int("TB_DEMO_CAMERA_RETRY_CYCLES", 10);
    if (camera_retry_cycles <= 0)
        camera_retry_cycles = 10;

    LibcameraCapture camera;
    bool camera_ok = false;
    int last_camera_retry_sent = -camera_retry_cycles;
    if (use_real_camera)
    {
        camera_ok = camera.start();
        if (camera_ok)
            log_msg(LOG_WARNING, "Real camera stream enabled");
        else
            log_msg(LOG_ERROR, "Real camera not available; using demo frame fallback");
    }
    else
    {
        log_msg(LOG_WARNING, "Real camera disabled by TB_DEMO_USE_REAL_CAMERA=0; using demo frame");
    }

    int product_id = getenv_int("TB_DEMO_START_PRODUCT_ID", 1);
    int total_ok = getenv_int("TB_DEMO_START_OK", 0);
    int total_ng = getenv_int("TB_DEMO_START_NG", 0);
    int health_every = getenv_int("TB_HEALTH_INTERVAL_CYCLES", 5);
    if (health_every <= 0)
        health_every = 5;

    log_msg(LOG_WARNING, "ThingsBoard MQTT demo started interval=%dms cycles=%d",
            interval_ms, max_cycles);
    log_msg(LOG_WARNING, "Jetson setup identity: ssid=%s device_info=%s mac=%s",
            setup_ap_ssid.c_str(), identity_attrs.device_info_url.c_str(), mac_address.c_str());

    int sent = 0;
    while (g_running && (max_cycles == 0 || sent < max_cycles))
    {
        if (!mqtt_started && sent % health_every == 0)
        {
            mqtt_started = client.start();
            if (mqtt_started)
            {
                log_msg(LOG_WARNING, "ThingsBoard MQTT started after network became available");
                publish_static_attributes();
            }
        }

        if (reset_requested.exchange(false))
        {
            product_id = 1;
            total_ok = 0;
            total_ng = 0;
            log_msg(LOG_WARNING, "Demo counters reset by RPC");
        }

        bool is_ng = product_id % 7 == 0;
        if (is_ng)
            total_ng++;
        else
            total_ok++;

        auto telemetry = make_cycle(device_id, mac_address, product_id, total_ok, total_ng);
        cv::Mat stream_frame;
        if (use_real_camera)
        {
            if (!camera_ok && sent - last_camera_retry_sent >= camera_retry_cycles)
            {
                last_camera_retry_sent = sent;
                camera_ok = camera.start();
                if (camera_ok)
                    log_msg(LOG_WARNING, "Real camera stream recovered");
            }
            if (camera_ok)
            {
                stream_frame = camera.capture();
                if (stream_frame.empty())
                {
                    log_msg(LOG_ERROR, "Real camera returned empty frame; stopping camera and retrying later");
                    camera.stop();
                    camera_ok = false;
                }
            }
        }
        if (!stream_frame.empty())
            image_server.update_frame(stream_frame);
        else if (fallback_demo_frame)
            image_server.update_frame(make_demo_frame(telemetry, light_enabled.load(), calibration_mode.load()));

        {
            std::lock_guard<std::mutex> lock(local_telemetry_mutex);
            latest_inspection_ts = telemetry.ts_ms;
            latest_inspection_value = inspection_value_json(telemetry);
        }
        if (client.enqueue(telemetry))
        {
            log_msg(LOG_WARNING, "Demo queued product=%d result=%s ok=%d ng=%d dropped=%zu",
                    product_id, telemetry.result.label.c_str(), total_ok, total_ng,
                    client.dropped_count());
        }

        if (sent % health_every == 0)
        {
            auto health = make_health(device_id, mac_address, client.connected(), camera_ok, image_server_ok, client.dropped_count());
            image_server.set_health_json(health_json(health));
            {
                std::lock_guard<std::mutex> lock(local_telemetry_mutex);
                latest_health_ts = health.ts_ms;
                latest_health_value = health_value_json(health);
            }
            client.enqueue_health(health);
        }

        product_id++;
        sent++;
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }

    camera.close();
    if (mqtt_started)
        client.stop();
    log_msg(LOG_WARNING, "ThingsBoard MQTT demo stopped");
    return EXIT_SUCCESS;
}
