#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>
#include <libnotify/notify.h>

#include <boost/asio.hpp>
#include <memory>
#include <string>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <deque>
#include <unordered_map>
#include <atomic>

#include "shared/protocol.hpp"
#include "shared/message.hpp"
#include "discovery.hpp"
#include "peer_network.hpp"
#include "storage.hpp"
#include "resources.hpp"

namespace asio = boost::asio;

// ============================================================
// Application context — all state lives here
// ============================================================
struct AppContext {
    GtkWindow*      window   = nullptr;
    WebKitWebView*  webview  = nullptr;

    // P2P networking
    std::unique_ptr<asio::io_context> ioc;
    std::thread ioc_thread;
    std::unique_ptr<UdpDiscovery> discovery;
    std::unique_ptr<PeerNetwork> network;
    LocalStorage* storage = nullptr;

    // User state
    std::string username;
    std::string config_dir;
    int tcp_port = 0;

    // Known peers: name → {ip, port, online}
    struct PeerInfo {
        std::string ip;
        int tcp_port = 0;
        bool online = false;
    };
    std::unordered_map<std::string, PeerInfo> peers;
    std::mutex peers_mutex;

    bool logged_in = false;
};

static AppContext* g_ctx = nullptr;

// ============================================================
// GTK main-thread JS helpers
// ============================================================
struct JsTask {
    std::string code;
};

static gboolean run_js_idle(gpointer data) {
    auto* t = static_cast<JsTask*>(data);
    if (g_ctx && g_ctx->webview) {
        webkit_web_view_evaluate_javascript(
            g_ctx->webview, t->code.c_str(), -1,
            nullptr, nullptr, nullptr, nullptr, nullptr);
    }
    delete t;
    return G_SOURCE_REMOVE;
}

static void call_js_async(const std::string& js) {
    g_idle_add(run_js_idle, new JsTask{js});
}

static void call_js(const std::string& js) {
    if (g_ctx && g_ctx->webview) {
        webkit_web_view_evaluate_javascript(
            g_ctx->webview, js.c_str(), -1,
            nullptr, nullptr, nullptr, nullptr, nullptr);
    }
}

static std::string escape_js(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 20);
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '"') o += "\\\"";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else o += c;
    }
    return o;
}

// ============================================================
// Notifications
// ============================================================
static void notify(const std::string& title, const std::string& body) {
    if (!notify_is_initted()) return;
    auto* n = notify_notification_new(title.c_str(), body.c_str(), "dialog-information");
    notify_notification_set_timeout(n, 5000);
    notify_notification_show(n, nullptr);
    g_object_unref(G_OBJECT(n));
}

// ============================================================
// P2P event handlers (called from io_context thread)
// ============================================================

// UDP discovered a new peer → connect to their TCP port
static void on_peer_discovered(const std::string& name, const std::string& ip, int port) {
    if (name == g_ctx->username) return;

    {
        std::lock_guard<std::mutex> lk(g_ctx->peers_mutex);
        auto& info = g_ctx->peers[name];
        info.ip = ip;
        info.tcp_port = port;
        info.online = true;
    }

    // Connect via TCP
    g_ctx->network->connect_to_peer(name, ip, port);

    call_js_async("window.__onPeerOnline('" + escape_js(name) + "');");
}

// UDP goodbye from a peer
static void on_peer_goodbye(const std::string& name) {
    {
        std::lock_guard<std::mutex> lk(g_ctx->peers_mutex);
        if (g_ctx->peers.count(name)) {
            g_ctx->peers[name].online = false;
        }
    }
    call_js_async("window.__onPeerOffline('" + escape_js(name) + "');");
}

// Message received from a peer (over TCP)
static void on_message_from_peer(const lan_chat::Message& msg) {
    if (!g_ctx || !g_ctx->logged_in) return;

    // Save to local storage
    if (msg.type == lan_chat::MsgType::ChatMessage && g_ctx->storage) {
        StoredMessage sm;
        sm.msg_id    = msg.id;
        sm.sender    = msg.from;
        sm.recipient = msg.to;
        sm.content   = msg.content;
        sm.timestamp = msg.timestamp;
        sm.is_read   = 0;
        g_ctx->storage->save_message(sm);

        // Notify if app not focused
        notify(msg.from, msg.content);
    }

    // Forward to JS UI
    auto json = lan_chat::serialize(msg);
    call_js_async("window.__onMessage('" + escape_js(json) + "');");
}

// Peer connection status changed
static void on_peer_status(const std::string& name, bool online) {
    if (!online) {
        call_js_async("window.__onPeerOffline('" + escape_js(name) + "');");
    }
}

// ============================================================
// JS → C++ action handler (GTK main thread)
// ============================================================
static void on_js_msg(WebKitUserContentManager* mgr,
                      WebKitJavascriptResult* result,
                      gpointer user_data) {
    auto* ctx = static_cast<AppContext*>(user_data);
    JSCValue* v = webkit_javascript_result_get_js_value(result);
    if (!v) return;
    char* s = jsc_value_to_string(v);
    if (!s) return;
    std::string json(s);
    g_free(s);

    try {
        auto j = nlohmann::json::parse(json);
        std::string action = j.value("action", "");

        if (action == "login") {
            std::string name = j.value("username", "");

            if (name.empty()) {
                call_js("window.__onLoginResult(false, '用户名不能为空');");
                return;
            }

            ctx->username = name;
            ctx->config_dir = std::string(getenv("HOME")) + "/.config/lan-chat";

            // Init storage
            std::string db_path = ctx->config_dir + "/" + name + ".db";
            ctx->storage = new LocalStorage(db_path);

            // Start IO context
            ctx->ioc = std::make_unique<asio::io_context>();

            // Start TCP peer network
            ctx->network = std::make_unique<PeerNetwork>(
                *ctx->ioc, name, on_message_from_peer, on_peer_status);
            ctx->tcp_port = ctx->network->start_listen();

            // Start UDP discovery
            ctx->discovery = std::make_unique<UdpDiscovery>();
            ctx->discovery->set_on_peer(on_peer_discovered);
            ctx->discovery->set_on_bye(on_peer_goodbye);
            ctx->discovery->start(name, ctx->tcp_port);

            // Run io_context in background thread
            ctx->ioc_thread = std::thread([ctx]() {
                try {
                    auto work = asio::make_work_guard(*ctx->ioc);
                    ctx->ioc->run();
                } catch (const std::exception& e) {
                    std::cerr << "[io] " << e.what() << "\n";
                }
            });

            ctx->logged_in = true;
            call_js("window.__onLoginResult(true, '');");
            std::cout << "[app] Logged in as '" << name
                      << "', TCP port " << ctx->tcp_port << "\n";

        } else if (action == "send_message") {
            std::string to = j.value("to", "");
            std::string content = j.value("content", "");
            if (!to.empty() && !content.empty()) {
                auto msg = lan_chat::make_chat(ctx->username, to, content);

                // Save locally
                if (ctx->storage) {
                    StoredMessage sm;
                    sm.msg_id    = msg.id;
                    sm.sender    = ctx->username;
                    sm.recipient = to;
                    sm.content   = content;
                    sm.timestamp = msg.timestamp;
                    sm.is_read   = 0;
                    ctx->storage->save_message(sm);
                }

                // Send over TCP to peer
                ctx->network->send_to(to, msg);
            }

        } else if (action == "send_broadcast") {
            std::string content = j.value("content", "");
            if (!content.empty()) {
                auto msg = lan_chat::make_chat(ctx->username, "", content);
                ctx->network->broadcast(msg);
            }

        } else if (action == "mark_read") {
            std::string sender = j.value("sender", "");
            if (ctx->storage && !sender.empty()) {
                ctx->storage->mark_read(sender);
            }

        } else if (action == "notify") {
            std::string title = j.value("title", "");
            std::string body = j.value("body", "");
            notify(title, body);
        }
    } catch (const std::exception& e) {
        std::cerr << "[js] " << e.what() << "\n";
    }
}

// ============================================================
// GTK activate
// ============================================================
static void on_activate(GtkApplication* app, gpointer data) {
    auto* ctx = static_cast<AppContext*>(data);

    GtkWidget* win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "LAN Chat");
    gtk_window_set_default_size(GTK_WINDOW(win), 960, 640);
    ctx->window = GTK_WINDOW(win);

    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, ".p2p-window { background: #1a1a2e; }", -1, nullptr);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    WebKitWebView* wv = WEBKIT_WEB_VIEW(webkit_web_view_new());
    ctx->webview = wv;

    WebKitSettings* ws = webkit_web_view_get_settings(wv);
    webkit_settings_set_enable_developer_extras(ws, TRUE);
    webkit_settings_set_javascript_can_access_clipboard(ws, TRUE);

    WebKitUserContentManager* mgr = webkit_web_view_get_user_content_manager(wv);
    webkit_user_content_manager_register_script_message_handler(mgr, "nativeAPI");
    g_signal_connect(mgr, "script-message-received::nativeAPI",
                     G_CALLBACK(on_js_msg), ctx);

    std::string html(reinterpret_cast<const char*>(embedded_html), embedded_html_len);
    webkit_web_view_load_html(wv, html.c_str(), nullptr);
    gtk_container_add(GTK_CONTAINER(win), GTK_WIDGET(wv));
    gtk_widget_show_all(win);
}

// ============================================================
// Main
// ============================================================
int main(int argc, char* argv[]) {
    std::string config_dir = std::string(getenv("HOME")) + "/.config/lan-chat";
    std::filesystem::create_directories(config_dir);

    notify_init("lan-chat");

    GtkApplication* app = gtk_application_new("com.lanchat.p2p", G_APPLICATION_FLAGS_NONE);

    AppContext ctx;
    ctx.config_dir = config_dir;
    g_ctx = &ctx;
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &ctx);

    int ret = g_application_run(G_APPLICATION(app), argc, argv);

    // Cleanup
    if (ctx.discovery) ctx.discovery->stop();
    if (ctx.network) ctx.network->stop();
    if (ctx.ioc) { ctx.ioc->stop(); }
    if (ctx.ioc_thread.joinable()) ctx.ioc_thread.join();
    delete ctx.storage;
    notify_uninit();
    g_object_unref(app);

    return ret;
}
