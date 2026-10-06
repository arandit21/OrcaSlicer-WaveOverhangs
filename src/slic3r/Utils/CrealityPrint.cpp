#include "CrealityPrint.hpp"

#include <algorithm>
#include <sstream>
#include <exception>
#include <boost/format.hpp>
#include <boost/foreach.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/nowide/convert.hpp>

#include <curl/curl.h>
#include <wx/progdlg.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/format.hpp"
#include "Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include "Bonjour.hpp"
#include "slic3r/GUI/BonjourDialog.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

#include <fstream>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
using std::to_string;

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace {

// A Sonic Pad web UI is commonly configured as http://host/#/ . URL fragments
// are browser-side only; libcurl therefore sends a request for '/' when an API
// path is appended after '#'.  Strip the fragment before composing REST URLs.
std::string normalize_api_base(std::string host)
{
    const auto hash = host.find('#');
    if (hash != std::string::npos)
        host.erase(hash);

    while (!host.empty() && (host.back() == '/' || host.back() == ' ' || host.back() == '\t'))
        host.pop_back();

    if (host.find("http://") != 0 && host.find("https://") != 0)
        host = "http://" + host;

    return host;
}

std::string socket_host_from_config(std::string host)
{
    host = normalize_api_base(std::move(host));
    if (boost::istarts_with(host, "https://"))
        host.erase(0, 8);
    else if (boost::istarts_with(host, "http://"))
        host.erase(0, 7);

    const auto slash = host.find('/');
    if (slash != std::string::npos)
        host.erase(slash);

    // Legacy Creality websocket always uses port 9999. Remove an explicitly
    // configured HTTP port for the resolver while keeping bracketed IPv6 intact.
    if (!host.empty() && host.front() != '[') {
        const auto colon = host.rfind(':');
        if (colon != std::string::npos)
            host.erase(colon);
    }
    return host;
}

bool should_try_moonraker(unsigned status)
{
    // 405 is the observed Sonic Pad failure when the legacy Creality upload
    // endpoint is unavailable. 404 covers Sonic Pad firmware variants that
    // expose Moonraker but not /upload/<name>.
    return status == 404 || status == 405;
}

std::string path_utf8(const fs::path& path)
{
#ifdef _WIN32
    return boost::nowide::narrow(path.wstring());
#else
    return path.string();
#endif
}

std::string remote_dir_utf8(const fs::path& path)
{
    std::string out = path_utf8(path);
    std::replace(out.begin(), out.end(), '\\', '/');
    while (!out.empty() && out.front() == '/')
        out.erase(out.begin());
    while (!out.empty() && out.back() == '/')
        out.pop_back();
    if (out == ".")
        out.clear();
    return out;
}

bool moonraker_json_ok(const std::string& body, bool require_upload_shape, std::string& reason)
{
    json parsed = json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        reason = "Moonraker returned malformed JSON";
        return false;
    }

    if (parsed.contains("error")) {
        try {
            reason = "Moonraker error: " + parsed.at("error").dump();
        } catch (...) {
            reason = "Moonraker returned an error response";
        }
        return false;
    }

    if (!require_upload_shape)
        return true;

    if (parsed.contains("result")) {
        const auto& result = parsed.at("result");
        if (result.is_object()) {
            if (result.contains("item") || result.contains("path") || result.contains("action") ||
                result.contains("print_started") || result.contains("print_queued"))
                return true;
        } else if (result.is_string()) {
            const std::string s = result.get<std::string>();
            if (s == "ok" || s == "success")
                return true;
        }
    }

    // Some Creality Moonraker proxies unwrap result and return the upload item.
    if (parsed.contains("item") || parsed.contains("path") || parsed.contains("action"))
        return true;

    reason = "Moonraker returned an unexpected upload response";
    return false;
}

} // anonymous namespace

namespace Slic3r {

CrealityPrint::CrealityPrint(DynamicPrintConfig* config) : 
    m_host(config->opt_string("print_host")), 
    m_web_ui(config->opt_string("print_host_webui")),
    m_cafile(config->opt_string("printhost_cafile")),
    m_port(config->opt_string("printhost_port")),
    m_apikey(config->opt_string("printhost_apikey")),
    m_ssl_revoke_best_effort(config->opt_bool("printhost_ssl_ignore_revoke"))
{}

const char* CrealityPrint::get_name() const { return "Creality Print"; }

std::string CrealityPrint::get_host() const {
    return m_host;
}
void  CrealityPrint::set_auth(Http& http) const
{
    http.header("Authorization", "Bearer " + m_apikey);
    if (!m_cafile.empty()) {
        http.ca_file(m_cafile);
    }
}

wxString CrealityPrint::get_test_ok_msg() const { return _(L("Connected to CrealityPrint successfully!")); }

wxString CrealityPrint::get_test_failed_msg(wxString& msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to CrealityPrint"), msg.Truncate(256));
}

bool CrealityPrint::test_moonraker(wxString& msg) const
{
    bool res = false;
    std::string parse_error;
    auto url = make_url("server/info");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Sonic Pad/Moonraker probe at: %2%") % get_name() % url;

    auto http = Http::get(std::move(url));
    if (!m_apikey.empty())
        http.header("X-Api-Key", m_apikey);
    if (!m_cafile.empty())
        http.ca_file(m_cafile);

    http.timeout_connect(5)
        .timeout_max(10)
        .on_error([&](std::string body, std::string error, unsigned status) {
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&](std::string body, unsigned) {
            if (moonraker_json_ok(body, false, parse_error)) {
                res = true;
            } else {
                res = false;
                msg = wxString::FromUTF8(parse_error.c_str());
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    return res;
}

bool CrealityPrint::test(wxString& msg) const
{ 
    bool res = true;
    bool try_moonraker = false;
    wxString legacy_error;
    const char* name = get_name();
    auto url = make_url("info");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get version at: %2%") % name % url;
    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
            res = false;
            legacy_error = format_error(body, error, status);
            if (should_try_moonraker(status))
                try_moonraker = true;
            else
                msg = legacy_error;
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got version: %2%") % name % body;
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .on_ip_resolve([&](std::string address) {
            msg = GUI::from_u8(address);
        })
#endif // WIN32
        .perform_sync();

    if (res)
        return true;

    if (try_moonraker) {
        wxString moonraker_msg;
        if (test_moonraker(moonraker_msg)) {
            BOOST_LOG_TRIVIAL(info) << name << ": using Sonic Pad/Moonraker compatibility path";
            msg.clear();
            return true;
        }
        msg = moonraker_msg.empty() ? legacy_error : moonraker_msg;
    }

    return false;
}

PrintHostPostUploadActions CrealityPrint::get_post_upload_actions() const {
    return PrintHostPostUploadAction::StartPrint; 
}

bool CrealityPrint::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{   
    const char* name = get_name();
    const auto upload_filename = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();
    wxString test_msg;
    if (!test(test_msg)) {
        error_fn(std::move(test_msg));
        return false;
    }

    bool res = true;
    bool try_moonraker = false;
    bool cancelled = false;
    wxString legacy_error;
    auto url = make_url("upload/" + safe_filename(upload_filename.string()));

    auto  http = Http::post(url);
    set_auth(http);
    http.form_add("path", upload_parent_path.string())
        .form_add_file("file", upload_data.source_path.string(), upload_filename.string())

        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;

            if (upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
                start_print(safe_filename(upload_filename.string()));
            }
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file to %2%: %3%, HTTP %4%, body: `%5%`") % name % url % error % status % body;
            legacy_error = format_error(body, error, status);
            try_moonraker = should_try_moonraker(status);
            res = false;
            // Do not emit the legacy 404/405 yet. On Sonic Pad it is expected;
            // emit only if the compatibility path also fails.
            if (!try_moonraker)
                error_fn(legacy_error);
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                cancelled = true;
                res = false;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    if (res || cancelled || !try_moonraker)
        return res;

    BOOST_LOG_TRIVIAL(info) << name << ": legacy upload endpoint returned 404/405; retrying through Sonic Pad/Moonraker";
    if (upload_moonraker(upload_data, prorgess_fn, error_fn, info_fn))
        return true;

    // upload_moonraker reports its own precise error. Keep the original error
    // in the debug log for diagnosis without showing two dialogs to the user.
    BOOST_LOG_TRIVIAL(debug) << name << ": original legacy upload error: " << legacy_error.ToStdString();
    return false;
}

bool CrealityPrint::upload_moonraker(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    (void)info_fn;

    const auto upload_filename = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();
    const std::string wire_filename = path_utf8(upload_filename);
    const std::string remote_dir = remote_dir_utf8(upload_parent_path);

    bool res = false;
    bool cancelled = false;
    wxString upload_error;
    std::string parse_error;
    auto url = make_url("server/files/upload");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Moonraker upload %2% -> %3%") % get_name() % wire_filename % url;

    auto http = Http::post(url);
    if (!m_apikey.empty())
        http.header("X-Api-Key", m_apikey);
    if (!m_cafile.empty())
        http.ca_file(m_cafile);

    http.form_add("root", "gcodes");
    http.form_add("print", "false");
    if (!remote_dir.empty())
        http.form_add("path", remote_dir);
    http.form_add_file("file", upload_data.source_path.string(), wire_filename)
        .timeout_connect(5)
        .timeout_max(300)
        .on_complete([&](std::string body, unsigned status) {
            if (moonraker_json_ok(body, true, parse_error)) {
                res = true;
                BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Moonraker file uploaded: HTTP %2%: %3%") % get_name() % status % body;
            } else {
                res = false;
                upload_error = wxString::FromUTF8(parse_error.c_str());
            }
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            res = false;
            upload_error = format_error(body, error, status);
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Moonraker upload error: %2%, HTTP %3%, body: `%4%`") % get_name() % error % status % body;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            progress_fn(std::move(progress), cancel);
            if (cancel) {
                cancelled = true;
                res = false;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    if (!res) {
        if (!cancelled) {
            if (upload_error.empty())
                upload_error = _L("Sonic Pad / Moonraker upload failed.");
            error_fn(upload_error);
        }
        return false;
    }

    if (upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
        std::string remote_filename = wire_filename;
        if (!remote_dir.empty())
            remote_filename = remote_dir + "/" + wire_filename;
        return start_print_moonraker(remote_filename, error_fn);
    }

    return true;
}

bool CrealityPrint::start_print_moonraker(const std::string& remote_filename, ErrorFn error_fn) const
{
    bool res = false;
    wxString request_error;
    std::string parse_error;
    const std::string url = make_url("printer/print/start?filename=" + Http::url_encode(remote_filename));

    auto http = Http::post(url);
    if (!m_apikey.empty())
        http.header("X-Api-Key", m_apikey);
    if (!m_cafile.empty())
        http.ca_file(m_cafile);

    http.timeout_connect(5)
        .timeout_max(15)
        .on_complete([&](std::string body, unsigned) {
            if (moonraker_json_ok(body, false, parse_error)) {
                res = true;
            } else {
                request_error = wxString::FromUTF8(parse_error.c_str());
            }
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            request_error = format_error(body, error, status);
            res = false;
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    if (!res) {
        if (request_error.empty())
            request_error = _L("G-code was uploaded, but Sonic Pad could not start the print.");
        error_fn(request_error);
    }
    return res;
}

std::string CrealityPrint::make_url(const std::string &path) const
{
    std::string base = normalize_api_base(m_host);
    std::string rel = path;
    while (!rel.empty() && rel.front() == '/')
        rel.erase(rel.begin());
    return base + "/" + rel;
}

std::string CrealityPrint::safe_filename(const std::string &filename) const
{
    std::string safe_filename = filename;
    std::replace(safe_filename.begin(), safe_filename.end(), ' ', '_');

    return safe_filename;
}

void CrealityPrint::start_print(const std::string &filename) const
{
    try {
        std::string host = socket_host_from_config(m_host);
        auto const port = "9999";

        json j2 = {
            { "method", "set" },
            {
                "params", {
                    { "opGcodeFile", "printprt:/usr/data/printer_data/gcodes/" + filename }
                }    
            }
        };

        net::io_context ioc;

        tcp::resolver resolver{ioc};
        websocket::stream<tcp::socket> ws{ioc};

        auto const results = resolver.resolve(host, port);

        auto ep = net::connect(ws.next_layer(), results);

        host += ':' + std::to_string(ep.port());

        ws.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req)
            {
                req.set(http::field::user_agent,
                    std::string(BOOST_BEAST_VERSION_STRING) +
                        " websocket-client-coro");
            }));

        ws.handshake(host, "/");
        
        ws.write(net::buffer(to_string(j2)));

        beast::flat_buffer buffer;

        ws.read(buffer);

        ws.close(websocket::close_code::normal);
    } catch(std::exception const& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    
}

}
