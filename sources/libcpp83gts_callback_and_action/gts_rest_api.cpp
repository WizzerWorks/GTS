#ifdef _WIN32
/* before anything includes <windows.h>: winsock2 only, no min/max macros */
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# ifndef NOMINMAX
#  define NOMINMAX
# endif
#endif
#include "httplib.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>
#include <FL/Fl.H>
#ifdef _WIN32
# include <io.h>
#else
# include <unistd.h>
#endif
#include "ptbl_returncode.h"
#include "pri.h"
#include "gts_gui.h"
#include "gts_master.h"
#include "gts_rest_api.h"

namespace {

/*---------- JSON helpers (small, only what the API needs) ----------*/

std::string json_escape_(const std::string& src)
{
	std::string out;
	for (const unsigned char cc : src) {
		switch (cc) {
		case '"':  out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n";  break;
		case '\r': out += "\\r";  break;
		case '\t': out += "\\t";  break;
		default:
			if (cc < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof buf, "\\u%04x", cc);
				out += buf;
			} else {
				out += static_cast<char>(cc);
			}
		}
	}
	return out;
}
std::string json_str_(const std::string& src)
{
	return "\"" + json_escape_(src) + "\"";
}
std::string json_error_(const std::string& msg)
{
	return "{\"error\":" + json_str_(msg) + "}";
}

/* Get a string member of a flat JSON object: {"key":"value"} */
bool json_get_string_(
	const std::string& json ,const std::string& key ,std::string& val
)
{
	const std::string pat = "\"" + key + "\"";
	std::size_t pos = json.find(pat);
	if (pos == std::string::npos) { return false; }
	pos = json.find(':' ,pos + pat.size());
	if (pos == std::string::npos) { return false; }
	pos = json.find_first_not_of(" \t\r\n" ,pos + 1);
	if (pos == std::string::npos || json[pos] != '"') { return false; }
	val.clear();
	for (++pos; pos < json.size(); ++pos) {
		const char cc = json[pos];
		if (cc == '"') { return true; }
		if (cc != '\\') { val += cc; continue; }
		if (++pos >= json.size()) { return false; }
		switch (json[pos]) {
		case 'n': val += '\n'; break;
		case 't': val += '\t'; break;
		case 'r': val += '\r'; break;
		case 'u': return false; /* not supported */
		default:  val += json[pos]; /* \" \\ \/ */
		}
	}
	return false; /* unterminated */
}

/*---------- temporary file for the config load/save API ----------*/

std::string make_temp_path_()
{
#ifdef _WIN32
	const char* dir = std::getenv("TEMP");
	std::string path = std::string(dir ? dir : ".") + "\\gts_cfg_XXXXXX";
	if (_mktemp_s(&path[0] ,path.size() + 1) != 0) { return std::string(); }
	return path;
#else
	const char* dir = std::getenv("TMPDIR");
	std::string path = std::string(dir ? dir : "/tmp") + "/gts_cfg_XXXXXX";
	const int fd = mkstemp(&path[0]);
	if (fd < 0) { return std::string(); }
	close(fd);
	return path;
#endif
}

/*---------- run a function on the FLTK main thread ----------*/

struct main_job {
	std::function<void()> func;
	std::promise<void> done;
};
void main_job_run_(void* vp)
{
	std::unique_ptr<main_job> job(static_cast<main_job*>(vp));
	try { job->func(); } catch (...) { }
	job->done.set_value();
}

} // namespace

/*==================== server ====================*/

struct gts_rest_api::impl {
	httplib::Server server;
	std::thread thread;
	std::atomic<bool> listening{false};   /* listen() is running */
	std::atomic<bool> stopping{false};
	std::atomic<bool> busy{false};
	std::atomic<bool> quit{false};
	std::string token;

	/* false when the server is going down */
	bool on_main(const std::function<void()>& func)
	{
		if (this->stopping) { return false; }
		main_job* job = new main_job{func ,{}};
		std::future<void> fut = job->done.get_future();
		if (Fl::awake(main_job_run_ ,job) != 0) {
			delete job;
			return false;
		}
		fut.wait();
		return true;
	}

	void setup_routes();
};

namespace {

const char* const k_json = "application/json";

/* One main-thread job at a time; reject other long jobs with 409 */
struct busy_guard {
	std::atomic<bool>& flag;
	bool ok;
	explicit busy_guard(std::atomic<bool>& ff)
		:flag(ff) ,ok(!ff.exchange(true)) {}
	~busy_guard() { if (ok) { flag = false; } }
};

void reply_(httplib::Response& res ,const int status ,const std::string& body
	,const char* type = k_json)
{
	res.status = status;
	res.set_content(body ,type);
}

/* The scan: same processing as GUI "Scan and Save", but no display, no
dialogs, and the file path comes from the request. Main thread only. */
std::string scan_to_file_(const std::string& path)
{
	int return_code = NG;
	iip_canvas* canvas = cl_gts_master.iipg_scan(return_code);
	if (return_code == CANCEL) { return "Scan canceled"; }
	if (return_code != OK || canvas == nullptr) { return "Scan failed"; }

	if (cl_gts_master.rot_and_trace_and_enoise(
		cl_gts_master.cl_iip_scan.get_clp_canvas()
		,cl_gts_gui.choice_rot90->value()
	) != OK) {
		return "Image processing failed";
	}

	if (OK != cl_gts_master.iipg_save(
		&(cl_gts_master.cl_iip_edot)
		,const_cast<char*>(path.c_str())
		,cl_gts_gui.valinp_area_reso->value()
	)) {
		return "Can not save file";
	}
	return std::string();
}

} // namespace

void gts_rest_api::impl::setup_routes()
{
	httplib::Server& svr = this->server;

	if (!this->token.empty()) {
		const std::string expect = "Bearer " + this->token;
		svr.set_pre_routing_handler(
		[expect](const httplib::Request& req ,httplib::Response& res) {
			if (req.get_header_value("Authorization") == expect) {
				return httplib::Server::HandlerResponse::Unhandled;
			}
			reply_(res ,401 ,json_error_("unauthorized"));
			return httplib::Server::HandlerResponse::Handled;
		});
	}

	svr.set_error_handler(
	[](const httplib::Request&, httplib::Response& res) {
		if (res.body.empty()) {
			reply_(res ,res.status
				,json_error_(httplib::status_message(res.status)));
		}
	});

	svr.set_exception_handler(
	[](const httplib::Request&, httplib::Response& res
		,std::exception_ptr) {
		reply_(res ,500 ,json_error_("internal error"));
	});

	svr.Get("/api/v1/version"
	,[](const httplib::Request&, httplib::Response& res) {
		reply_(res ,200 ,
			"{\"name\":" + json_str_(cl_gts_master.cp_release_name())
			+ ",\"version\":"
			+ json_str_(cl_gts_master.cp_release_number())
			+ ",\"date\":" + json_str_(cl_gts_master.cp_release_date())
			+ ",\"api\":\"v1\"}");
	});

	svr.Get("/api/v1/status"
	,[this](const httplib::Request&, httplib::Response& res) {
		reply_(res ,200 ,std::string("{\"state\":")
			+ (this->busy ? "\"busy\"" : "\"idle\"") + "}");
	});

	svr.Get("/api/v1/scanners"
	,[this](const httplib::Request&, httplib::Response& res) {
		std::string body = "{\"scanners\":[";
		const bool ok = this->on_main([&body]() {
#ifndef _WIN32
			const SANE_Device** dev = cl_gts_master.cl_iip_scan.sane_devlist;
			for (int ii = 0; dev != nullptr && dev[ii] != nullptr; ++ii) {
				if (ii > 0) { body += ","; }
				body += "{\"name\":" + json_str_(dev[ii]->name)
					+ ",\"vendor\":" + json_str_(dev[ii]->vendor)
					+ ",\"model\":" + json_str_(dev[ii]->model)
					+ ",\"type\":" + json_str_(dev[ii]->type) + "}";
			}
#else
			/* TWAIN: only the driver name is known */
			std::vector<std::string> names;
			(void)cl_gts_master.cl_iip_scan.get_source_names(names);
			for (std::size_t ii = 0; ii < names.size(); ++ii) {
				if (ii > 0) { body += ","; }
				body += "{\"name\":" + json_str_(names[ii]) + "}";
			}
#endif
		});
		if (!ok) { reply_(res ,503 ,json_error_("shutting down")); return; }
		reply_(res ,200 ,body + "]}");
	});

	svr.Get("/api/v1/config"
	,[this](const httplib::Request&, httplib::Response& res) {
		const std::string tmp = make_temp_path_();
		if (tmp.empty()) {
			reply_(res ,500 ,json_error_("no temporary file")); return;
		}
		int ret = NG;
		const bool ok = this->on_main([&]() {
			ret = cl_gts_master.cl_memo_config.save(tmp);
		});
		std::ifstream ifs(tmp.c_str() ,std::ios::binary);
		std::ostringstream ost;
		ost << ifs.rdbuf();
		ifs.close();
		std::remove(tmp.c_str());
		if (!ok) { reply_(res ,503 ,json_error_("shutting down")); return; }
		if (ret != OK) {
			reply_(res ,500 ,json_error_("can not save config")); return;
		}
		reply_(res ,200 ,ost.str() ,"text/plain; charset=utf-8");
	});

	svr.Put("/api/v1/config"
	,[this](const httplib::Request& req ,httplib::Response& res) {
		busy_guard guard(this->busy);
		if (!guard.ok) {
			reply_(res ,409 ,json_error_("busy")); return;
		}
		const std::string tmp = make_temp_path_();
		if (tmp.empty()) {
			reply_(res ,500 ,json_error_("no temporary file")); return;
		}
		{
		std::ofstream ofs(tmp.c_str() ,std::ios::binary);
		ofs << req.body;
		}
		int ret = NG;
		const bool ok = this->on_main([&]() {
			ret = cl_gts_master.cl_memo_config.load(tmp);
		});
		std::remove(tmp.c_str());
		if (!ok) { reply_(res ,503 ,json_error_("shutting down")); return; }
		if (ret != OK) {
			reply_(res ,400 ,json_error_("can not load config")); return;
		}
		reply_(res ,200 ,"{\"result\":\"ok\"}");
	});

	svr.Post("/api/v1/scan"
	,[this](const httplib::Request& req ,httplib::Response& res) {
		std::string path;
		if (!json_get_string_(req.body ,"path" ,path) || path.empty()) {
			reply_(res ,400 ,json_error_(
				"body must be {\"path\":\"/full/path/out.tif\"}"));
			return;
		}
		busy_guard guard(this->busy);
		if (!guard.ok) {
			reply_(res ,409 ,json_error_("busy")); return;
		}
		std::string err;
		const bool ok = this->on_main([&]() { err = scan_to_file_(path); });
		if (!ok) { reply_(res ,503 ,json_error_("shutting down")); return; }
		if (!err.empty()) { reply_(res ,500 ,json_error_(err)); return; }
		reply_(res ,200 ,"{\"result\":\"ok\",\"path\":"
			+ json_str_(path) + "}");
	});

	svr.Post("/api/v1/shutdown"
	,[this](const httplib::Request&, httplib::Response& res) {
		reply_(res ,200 ,"{\"result\":\"shutting down\"}");
		this->quit = true;
	});
}

/*==================== public ====================*/

gts_rest_api::gts_rest_api() : impl_(new impl) {}
gts_rest_api::~gts_rest_api()
{
	/* stop() must have been called, otherwise the thread terminates */
	if (this->impl_->thread.joinable()) {
		this->impl_->stopping = true;
		this->impl_->server.stop();
		this->impl_->thread.join();
	}
}

bool gts_rest_api::start(
	const std::string& host ,const int port ,const std::string& token
	,std::string& error
)
{
	impl& im = *this->impl_;
	im.token = token;
	im.setup_routes();

	if (!im.server.bind_to_port(host ,port)) {
		error = "can not listen on " + host + ":" + std::to_string(port);
		return false;
	}
	im.listening = true;
	im.thread = std::thread([&im]() {
		im.server.listen_after_bind();
		im.listening = false;
	});
	im.server.wait_until_ready();
	return true;
}

void gts_rest_api::stop()
{
	impl& im = *this->impl_;
	if (!im.thread.joinable()) { return; }
	im.stopping = true;
	im.server.stop();
	/* in-flight requests may wait for the main thread: keep serving */
	while (im.listening) { Fl::wait(0.02); }
	im.thread.join();
}

bool gts_rest_api::quit_requested() const
{
	return this->impl_->quit;
}
