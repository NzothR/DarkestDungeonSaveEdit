#include "ddse/infrastructure/http_drogon_server.hpp"

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <spawn.h>
#include <sys/socket.h>
#include <unistd.h>
extern "C" char** environ;
#endif

namespace ddse::infrastructure {
namespace {

constexpr std::string_view kSessionCookie = "ddse_session";
constexpr std::string_view kClientHeader = "X-DDSE-Request";
constexpr std::string_view kContentSecurityPolicy =
    "default-src 'self'; connect-src 'self'; img-src 'self' data: blob:; "
    "style-src 'self'; script-src 'self'; object-src 'none'; base-uri 'none'; "
    "frame-ancestors 'none'";

struct ServerContext {
    std::filesystem::path web_root;
    std::string session_token;
    std::string origin;
    std::string host;
    const application::ApplicationStatusService& status_service;
};

std::string make_session_token() {
    std::random_device random;
    constexpr char digits[] = "0123456789abcdef";
    std::string token;
    token.reserve(64);
    for (int i = 0; i < 32; ++i) {
        const auto value = static_cast<unsigned int>(random()) & 0xffU;
        token.push_back(digits[value >> 4U]);
        token.push_back(digits[value & 0x0fU]);
    }
    return token;
}

bool constant_time_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    unsigned char difference = 0;
    for (std::size_t i = 0; i < left.size(); ++i)
        difference |= static_cast<unsigned char>(left[i] ^ right[i]);
    return difference == 0;
}

bool has_expected_origin(const drogon::HttpRequestPtr& request,
                         const ServerContext& context) {
    const auto& origin = request->getHeader("Origin");
    return origin.empty() || origin == context.origin;
}

bool has_session_cookie(const drogon::HttpRequestPtr& request,
                        const ServerContext& context) {
    return constant_time_equal(request->getCookie(std::string{kSessionCookie}),
                               context.session_token);
}

bool has_expected_host(const drogon::HttpRequestPtr& request,
                       const ServerContext& context) {
    return request->getHeader("Host") == context.host;
}

drogon::HttpResponsePtr json_error(drogon::HttpStatusCode status,
                                   std::string_view code,
                                   std::string_view message) {
    Json::Value body(Json::objectValue);
    body["ok"] = false;
    Json::Value error(Json::objectValue);
    error["code"] = std::string{code};
    error["message"] = std::string{message};
    error["context"] = Json::Value(Json::objectValue);
    body["error"] = std::move(error);
    body["diagnostics"] = Json::Value(Json::arrayValue);
    auto response = drogon::HttpResponse::newHttpJsonResponse(std::move(body));
    response->setStatusCode(status);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("X-Content-Type-Options", "nosniff");
    return response;
}

drogon::HttpResponsePtr forbidden_response(std::string_view code,
                                           std::string_view message) {
    return json_error(drogon::k403Forbidden, code, message);
}

drogon::HttpResponsePtr serve_index(const drogon::HttpRequestPtr& request,
                                    const std::shared_ptr<ServerContext>& context) {
    if (!has_expected_host(request, *context) ||
        !has_expected_origin(request, *context)) {
        return forbidden_response("LOCAL_ORIGIN_REJECTED",
                                  "The local editor only accepts its own origin.");
    }

    const auto index_path = context->web_root / "index.html";
    std::ifstream input(index_path, std::ios::binary);
    if (!input) {
        return json_error(drogon::k500InternalServerError, "FRONTEND_NOT_FOUND",
                          "The bundled frontend could not be loaded.");
    }
    const std::string html{std::istreambuf_iterator<char>{input},
                           std::istreambuf_iterator<char>{}};
    auto response = drogon::HttpResponse::newHttpResponse(
        drogon::k200OK, drogon::CT_TEXT_HTML);
    response->setBody(html);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Content-Security-Policy",
                        std::string{kContentSecurityPolicy});
    response->addHeader("Referrer-Policy", "no-referrer");
    response->addHeader("X-Content-Type-Options", "nosniff");

    drogon::Cookie session{std::string{kSessionCookie}, context->session_token};
    session.setPath("/");
    session.setHttpOnly(true);
    session.setSameSite(drogon::Cookie::SameSite::kStrict);
    response->addCookie(std::move(session));
    return response;
}

void handle_status(const drogon::HttpRequestPtr& request,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                   const std::shared_ptr<ServerContext>& context) {
    if (!has_expected_host(request, *context) ||
        !has_expected_origin(request, *context)) {
        callback(forbidden_response("LOCAL_ORIGIN_REJECTED",
                                    "The local editor only accepts its own origin."));
        return;
    }
    if (request->getHeader(std::string{kClientHeader}) != "1" ||
        !has_session_cookie(request, *context)) {
        callback(forbidden_response("LOCAL_SESSION_REQUIRED",
                                    "Open the editor page before calling the local API."));
        return;
    }

    const auto status = context->status_service.get_status();
    Json::Value value(Json::objectValue);
    value["apiVersion"] = static_cast<Json::UInt>(status.api_version);
    value["application"] = Json::Value(Json::objectValue);
    value["application"]["name"] = status.application_name;
    value["application"]["version"] = status.application_version;
    value["state"] = status.state;

    Json::Value body(Json::objectValue);
    body["ok"] = true;
    body["value"] = std::move(value);
    body["diagnostics"] = Json::Value(Json::arrayValue);
    auto response = drogon::HttpResponse::newHttpJsonResponse(std::move(body));
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("X-Content-Type-Options", "nosniff");
    callback(response);
}

bool wait_for_http_server(std::uint16_t port) {
    constexpr auto timeout = std::chrono::seconds{5};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
#ifdef _WIN32
        const auto socket_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_handle != INVALID_SOCKET) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            (void)InetPtonA(AF_INET, "127.0.0.1", &address.sin_addr);
            const auto connected = ::connect(
                socket_handle, reinterpret_cast<const sockaddr*>(&address),
                static_cast<int>(sizeof(address))) == 0;
            closesocket(socket_handle);
            if (connected) return true;
        }
#else
        const auto socket_handle = ::socket(AF_INET, SOCK_STREAM, 0);
        if (socket_handle >= 0) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            (void)inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
            const auto connected = ::connect(
                socket_handle, reinterpret_cast<const sockaddr*>(&address),
                static_cast<socklen_t>(sizeof(address))) == 0;
            ::close(socket_handle);
            if (connected) return true;
        }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return false;
}

std::optional<std::uint16_t> choose_ephemeral_loopback_port() {
#ifdef _WIN32
    WSADATA startup_data{};
    if (WSAStartup(MAKEWORD(2, 2), &startup_data) != 0) return std::nullopt;
    const auto socket_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        WSACleanup();
        return std::nullopt;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    (void)InetPtonA(AF_INET, "127.0.0.1", &address.sin_addr);
    const auto bound = ::bind(socket_handle, reinterpret_cast<const sockaddr*>(&address),
                              static_cast<int>(sizeof(address))) == 0;
    int address_size = static_cast<int>(sizeof(address));
    const auto found = bound && ::getsockname(
        socket_handle, reinterpret_cast<sockaddr*>(&address), &address_size) == 0;
    const auto port = found ? ntohs(address.sin_port) : 0;
    closesocket(socket_handle);
    WSACleanup();
#else
    const auto socket_handle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_handle < 0) return std::nullopt;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    (void)inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    const auto bound = ::bind(socket_handle, reinterpret_cast<const sockaddr*>(&address),
                              static_cast<socklen_t>(sizeof(address))) == 0;
    socklen_t address_size = static_cast<socklen_t>(sizeof(address));
    const auto found = bound && ::getsockname(
        socket_handle, reinterpret_cast<sockaddr*>(&address), &address_size) == 0;
    const auto port = found ? ntohs(address.sin_port) : 0;
    ::close(socket_handle);
#endif
    if (port == 0) return std::nullopt;
    return port;
}

bool open_browser(std::string_view url) {
#ifdef _WIN32
    const auto url_string = std::string{url};
    const auto result = reinterpret_cast<std::intptr_t>(
        ShellExecuteA(nullptr, "open", url_string.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL));
    return result > 32;
#else
    const auto url_string = std::string{url};
    const char* executable = "xdg-open";
#ifdef __APPLE__
    executable = "open";
#endif
    std::array<char*, 3> arguments{
        const_cast<char*>(executable), const_cast<char*>(url_string.c_str()), nullptr};
    pid_t child{};
    return posix_spawnp(&child, executable, nullptr, nullptr, arguments.data(),
                        environ) == 0;
#endif
}

} // namespace

int run_drogon_http_server(
    const application::ApplicationStatusService& status_service,
    const std::filesystem::path& web_root,
    std::uint16_t requested_port,
    bool open_browser_on_start) {
    std::error_code file_error;
    const auto index_path = web_root / "index.html";
    if (!std::filesystem::is_regular_file(index_path, file_error)) {
        std::cerr << "Frontend index.html is missing from " << web_root.string() << '\n';
        return 1;
    }

    auto context = std::make_shared<ServerContext>(ServerContext{
        .web_root = std::filesystem::absolute(web_root),
        .session_token = make_session_token(),
        .origin = {},
        .host = {},
        .status_service = status_service,
    });

    const auto selected_port = requested_port == 0
        ? choose_ephemeral_loopback_port()
        : std::optional<std::uint16_t>{requested_port};
    if (!selected_port) {
        std::cerr << "Could not allocate an available loopback port.\n";
        return 1;
    }

    auto& server = drogon::app();
    server.disableSession()
        .setThreadNum(1)
        .setLogLevel(trantor::Logger::kWarn)
        .setDocumentRoot(context->web_root.string())
        .setFileTypes({"html", "js", "css", "svg", "png", "webp", "ico"})
        .setStaticFilesCacheTime(0)
        .addListener("127.0.0.1", *selected_port);

    server.registerHandler(
        "/", [context](const drogon::HttpRequestPtr& request,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(serve_index(request, context));
        }, {drogon::Get});
    server.registerHandler(
        "/index.html", [context](const drogon::HttpRequestPtr& request,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(serve_index(request, context));
        }, {drogon::Get});
    server.registerHandler(
        "/api/status", [context](const drogon::HttpRequestPtr& request,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            handle_status(request, std::move(callback), context);
        }, {drogon::Get});
    server.setDefaultHandler(
        [](const drogon::HttpRequestPtr& request,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            if (request->path().starts_with("/api/")) {
                callback(json_error(drogon::k404NotFound, "ROUTE_NOT_FOUND",
                                    "The requested API route does not exist."));
            } else {
                callback(drogon::HttpResponse::newNotFoundResponse(request));
            }
        });

    std::promise<std::uint16_t> ready_promise;
    auto ready_future = ready_promise.get_future();
    std::atomic_bool ready_reported{false};
    server.registerBeginningAdvice([&server, context, &ready_promise, &ready_reported] {
        const auto listeners = server.getListeners();
        const auto listener = std::find_if(listeners.begin(), listeners.end(), [](const auto& address) {
            return address.toIp() == "127.0.0.1";
        });
        if (listener == listeners.end() || listener->toPort() == 0) {
            if (!ready_reported.exchange(true))
                ready_promise.set_exception(std::make_exception_ptr(
                    std::runtime_error{"Drogon did not create a loopback listener."}));
            return;
        }
        const auto port = listener->toPort();
        context->host = "127.0.0.1:" + std::to_string(port);
        context->origin = "http://" + context->host;
        if (!ready_reported.exchange(true)) ready_promise.set_value(port);
    });

    std::thread server_thread([&server, &ready_promise, &ready_reported] {
        try {
            server.run();
        } catch (...) {
            if (!ready_reported.exchange(true))
                ready_promise.set_exception(std::current_exception());
        }
    });

    std::uint16_t port{};
    try {
        if (ready_future.wait_for(std::chrono::seconds{10}) != std::future_status::ready) {
            std::cerr << "Timed out while starting the local HTTP service.\n";
            server.quit();
            server_thread.join();
            return 1;
        }
        port = ready_future.get();
    } catch (const std::exception& error) {
        std::cerr << "Unable to start the local HTTP service: " << error.what() << '\n';
        server.quit();
        server_thread.join();
        return 1;
    }

    if (!wait_for_http_server(port)) {
        std::cerr << "The local HTTP service did not become ready in time.\n";
        server.quit();
        server_thread.join();
        return 1;
    }

    const auto url = context->origin + "/";
    std::cout << "DDSE local service ready at " << url << std::endl;
    if (open_browser_on_start && !open_browser(url))
        std::cerr << "Could not open the default browser. Open " << url << " manually.\n";
    std::cout << "Press Ctrl+C to stop the local service." << std::endl;
    server_thread.join();
    return 0;
}

} // namespace ddse::infrastructure
