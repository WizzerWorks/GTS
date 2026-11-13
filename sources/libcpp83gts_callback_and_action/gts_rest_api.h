#ifndef gts_rest_api_h
#define gts_rest_api_h

#include <string>
#include <memory>

/*
	REST API server for GTS.

	The server runs on its own thread. Every request that touches GUI or
	GTS state is marshalled onto the FLTK main thread (Fl::awake), so the
	existing, non-thread-safe GTS code is only ever used from one thread.

	Endpoints (all under /api/v1, JSON unless noted):
	  GET  /version    application name, version, date
	  GET  /status     "idle" or "busy"
	  GET  /scanners   scanners found by SANE (empty on Windows)
	  GET  /config     current settings (text/plain, GTS config file format)
	  PUT  /config     load settings (text/plain, GTS config file format)
	  POST /scan       scan with the current settings, save to file
	                   body: {"path":"/full/path/out.tif"}
	  POST /shutdown   quit GTS
*/
class gts_rest_api {
public:
	gts_rest_api();
	~gts_rest_api();

	/* Start listening. token is optional: when not empty, requests
	need the header "Authorization: Bearer <token>".
	Call from the FLTK main thread, after Fl::lock(). */
	bool start(const std::string& host ,const int port
		,const std::string& token ,std::string& error);

	/* Stop listening. Call from the FLTK main thread.
	Keeps servicing queued main-thread work until the server is down. */
	void stop();

	/* true after POST /shutdown */
	bool quit_requested() const;

private:
	struct impl;
	std::unique_ptr<impl> impl_;
};

#endif /* !gts_rest_api_h */
