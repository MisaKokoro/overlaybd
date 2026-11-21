/*
   Copyright The Overlaybd Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include <photon/net/http/server.h>
#include <photon/net/socket.h>
#include <photon/net/http/url.h>
#include <map>
#include <fcntl.h>
#include <cerrno>
#include <photon/thread/thread.h>
#include "overlaybd/moonpool/uri.h"
#include <string>
#include <string_view>
#include "image_service.h"
#include "image_file.h"
#include "api_server.h"

class ApiHandler : public photon::net::http::HTTPHandler {
public:
    ImageService *imgservice;
    std::map<std::string, std::string> params;

    ApiHandler(ImageService *imgservice) : imgservice(imgservice) {}
    int handle_request(photon::net::http::Request& req,
                       photon::net::http::Response& resp,
                       std::string_view) override {
        auto target = req.target(); // string view, format: /snapshot?dev_id=${devID}&config=${config}
        std::string_view query("");
        auto pos = target.find('?');
        if (pos != std::string_view::npos) {
            query = target.substr(pos + 1);
        }
        // auto query = req.query();
        LOG_INFO("Snapshot query: `", query); // string view, format: dev_id=${devID}&config=${config}
        parse_params(query);
        auto dev_id = params["dev_id"];
        auto config_path = params["config"];
        LOG_DEBUG("dev_id: `, config: `", dev_id, config_path);
        
        int code;
        std::string msg;
        ImageFile* img_file = nullptr;

        if (dev_id.empty() || config_path.empty()) {
            code = 400;
            msg = std::string(R"delimiter({
        "success": false,
        "message": "Missing dev_id or config in snapshot request"
})delimiter");
            goto EXIT;
        }

        img_file = imgservice->find_image_file(dev_id);
        if (!img_file) {
            code = 404;
            msg = std::string(R"delimiter({
        "success": false,
        "message": "Image file not found"
})delimiter");
            goto EXIT;
        }

        if (img_file->create_snapshot(config_path.c_str()) < 0) {
            code = 500;
            msg = std::string(R"delimiter({
        "success": false,
        "message": "Failed to create snapshot`"
})delimiter");
            goto EXIT;
        }

        code = 200;
        msg = std::string(R"delimiter({
        "success": true,
        "message": "Snapshot created successfully"
})delimiter");

EXIT:
        resp.set_result(code);
        resp.headers.content_length(msg.size());
        resp.keep_alive(true);
        auto ret_w = resp.write((void*)msg.c_str(), msg.size());
        if (ret_w != (ssize_t)msg.size()) {
            LOG_ERRNO_RETURN(0, -1, "send body failed, target: `, `", req.target(), VALUE(ret_w));
        }
        LOG_DEBUG("send body done");
        return 0;
    }

    void parse_params(std::string_view query) { // format: dev_id=${devID}&config=${config}...
        if (query.empty())
            return;
        
        size_t start = 0;
        while (start < query.length()) {
            auto end = query.find('&', start);
            if (end == std::string_view::npos) { // last one
                end = query.length();
            }
            
            auto param = query.substr(start, end - start);
            auto eq_pos = param.find('=');
            if (eq_pos != std::string_view::npos) {
                auto key = param.substr(0, eq_pos);
                auto value = param.substr(eq_pos + 1);
                
                // url decode
                auto decoded_key = photon::net::http::url_unescape(key);
                auto decoded_value = photon::net::http::url_unescape(value);
                params[decoded_key] = decoded_value;
            } else {
                // key without value
                auto key = photon::net::http::url_unescape(param);
                params[key] = "";
            }
            start = end + 1;
        }
    }
};

// Uses the live ImageService so preflight observes the same local cache, pool
// namespace, transport and fallback policy as subsequent block-device reads.
class PoolCheckHandler : public photon::net::http::HTTPHandler {
    ImageService *service_;
    bool stopping_ = false;
    unsigned active_ = 0; // All handlers run on the owning Photon vCPU.

    int check(const std::string &body, std::string &message) {
        if (!service_->global_conf.pool().enable()) {
            message = "Mooncake pool is disabled";
            return 503;
        }
        if (service_->global_conf.pool().fallback() == "direct") {
            message = "direct fallback: full-layer preflight skipped";
            return 200;
        }
        rapidjson::Document cfg;
        cfg.Parse(body.c_str());
        if (cfg.HasParseError() || !cfg.IsObject() ||
            !cfg.HasMember("repoBlobUrl") || !cfg["repoBlobUrl"].IsString() ||
            !cfg.HasMember("file") || !cfg["file"].IsString() ||
            !cfg.HasMember("digest") || !cfg["digest"].IsString() ||
            !cfg.HasMember("size") || !cfg["size"].IsUint64()) {
            message = "invalid pool layer metadata";
            return 400;
        }
        auto path = MoonPool::resolve_uri(cfg["file"].GetString(), cfg["repoBlobUrl"].GetString(),
                                         cfg["digest"].GetString(), cfg["size"].GetUint64());
        if (path.empty()) {
            message = "invalid moonpool URI or layer metadata";
            return 400;
        }
        std::unique_ptr<IFile> file(service_->global_fs.cached_fs->open(path.c_str(), O_RDONLY));
        if (!file) {
            message = "cache-only layer open failed";
            return 503;
        }
        const auto size = cfg["size"].GetUint64();
        std::vector<char> buffer(1024 * 1024);
        for (uint64_t offset = 0; offset < size;) {
            if (stopping_) {
                message = "pool preflight service is stopping";
                return 503;
            }
            const auto count = std::min<uint64_t>(buffer.size(), size - offset);
            auto n = file->pread(buffer.data(), count, offset);
            if (n <= 0 || static_cast<uint64_t>(n) > count) {
                int error = errno;
                message = "cache-only image unavailable: digest=" +
                          std::string(cfg["digest"].GetString()) + ", offset=" +
                          std::to_string(offset) + ", errno=" + std::to_string(error);
                LOG_ERROR("moonpool pull preflight failed: `", message);
                return error == ENOENT ? 404 : 503;
            }
            offset += n;
        }
        message = "cache-only layer verified";
        return 200;
    }

public:
    explicit PoolCheckHandler(ImageService *service) : service_(service) {}
    void stop() {
        stopping_ = true;
        while (active_)
            photon::thread_usleep(1000);
    }
    int handle_request(photon::net::http::Request &req, photon::net::http::Response &resp,
                       std::string_view) override {
        ++active_;
        DEFER(--active_);
        req.get_socket_stream()->timeout(5 * 1000000);
        std::string message;
        int code = 400;
        const auto length = req.headers.content_length();
        if (req.verb() != photon::net::http::Verb::POST || length == 0 || length > 16384) {
            message = "POST a pool layer JSON object (maximum 16384 bytes)";
        } else {
            std::string body(length, '\0');
            size_t received = 0;
            while (received < length) {
                auto n = req.read(&body[received], length - received);
                if (n <= 0)
                    break;
                received += n;
            }
            if (received != length)
                message = "incomplete pool preflight request";
            else
                code = check(body, message);
        }
        resp.set_result(code);
        resp.headers.content_length(message.size());
        resp.keep_alive(false);
        return resp.write(message.data(), message.size()) == static_cast<ssize_t>(message.size()) ? 0 : -1;
    }
};

struct ApiServer {
    photon::net::ISocketServer* tcpserver = nullptr;
    photon::net::http::HTTPServer* httpserver = nullptr;
    ApiHandler* handler = nullptr;
    PoolCheckHandler* pool_handler = nullptr;

    ApiServer(ImageService *imgservice) : handler(new ApiHandler(imgservice)),
        pool_handler(new PoolCheckHandler(imgservice)) {}

    ~ApiServer() {
        if (tcpserver)
            tcpserver->terminate();
        pool_handler->stop();
        safe_delete(tcpserver);
        safe_delete(httpserver);
        delete handler;
        delete pool_handler;
    }

    int init(const std::string &addr) {
        photon::net::http::URL url(addr);
        std::string host = url.host().data(); // the string pointed by data() doesn't end up with '\0'
        auto pos = host.find(":");
        if (pos != host.npos) {
            host.resize(pos);
        }
        tcpserver = photon::net::new_tcp_socket_server();
        tcpserver->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
        if(tcpserver->bind(url.port(), photon::net::IPAddr(host.c_str())) < 0)
            LOG_ERRNO_RETURN(0, -1, "Failed to bind api server port `", url.port());
        if(tcpserver->listen() < 0)
            LOG_ERRNO_RETURN(0, -1, "Failed to listen api server port `", url.port());
        httpserver = photon::net::http::new_http_server();
        httpserver->add_handler(handler, false, "/snapshot");
        httpserver->add_handler(pool_handler, false, "/pool/check");
        tcpserver->set_handler(httpserver->get_connection_handler());
        tcpserver->start_loop();
        LOG_DEBUG("Api server listening on `:`, path: `", host, url.port(), "/snapshot");
        return 0;
    }
};

int start_api_server(ApiServer *&api_server , ImageService *imgservice, const std::string &addr) {
    api_server = new ApiServer(imgservice);
    return api_server->init(addr);
}

int stop_api_server(ApiServer *api_server) {
    if (api_server == nullptr) return 0;
    safe_delete(api_server);
    return 0;
}
