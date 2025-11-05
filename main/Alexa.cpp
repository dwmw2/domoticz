#include "stdafx.h"
#include "WebServer.h"
#include "Logger.h"
#include "../webserver/cWebem.h"
#include "../webserver/request.hpp"
#include "../webserver/reply.hpp"
#include <json/json.h>
#include <string>

extern CLogger _log;

namespace http {
namespace server {

void CWebServer::Alexa_HandleDiscovery(WebEmSession& session, const request& req, Json::Value& root)
{
	Json::Value request_json;
	Json::Reader reader;
	if (!reader.parse(req.content, request_json))
	{
		root["event"]["header"]["namespace"] = "Alexa";
		root["event"]["header"]["name"] = "ErrorResponse";
		root["event"]["payload"]["type"] = "INVALID_DIRECTIVE";
		root["event"]["payload"]["message"] = "Invalid JSON";
		return;
	}

	root["event"]["header"]["namespace"] = "Alexa";
	root["event"]["header"]["name"] = "ErrorResponse";
	root["event"]["header"]["payloadVersion"] = "3";
	root["event"]["header"]["messageId"] = request_json["directive"]["header"]["messageId"].asString();
	root["event"]["payload"]["type"] = "INTERNAL_ERROR";
	root["event"]["payload"]["message"] = "Discovery not yet implemented";
}

void CWebServer::Alexa_HandleAcceptGrant(WebEmSession& session, const request& req, Json::Value& root)
{
	Json::Value request_json;
	Json::Reader reader;
	if (!reader.parse(req.content, request_json))
	{
		root["event"]["header"]["namespace"] = "Alexa";
		root["event"]["header"]["name"] = "ErrorResponse";
		root["event"]["payload"]["type"] = "INVALID_DIRECTIVE";
		root["event"]["payload"]["message"] = "Invalid JSON";
		return;
	}

	root["event"]["header"]["namespace"] = "Alexa";
	root["event"]["header"]["name"] = "ErrorResponse";
	root["event"]["header"]["payloadVersion"] = "3";
	root["event"]["header"]["messageId"] = request_json["directive"]["header"]["messageId"].asString();
	root["event"]["payload"]["type"] = "INTERNAL_ERROR";
	root["event"]["payload"]["message"] = "AcceptGrant not yet implemented";
}

void CWebServer::Alexa_HandleControl(WebEmSession& session, const request& req, Json::Value& root)
{
	Json::Value request_json;
	Json::Reader reader;
	if (!reader.parse(req.content, request_json))
	{
		root["event"]["header"]["namespace"] = "Alexa";
		root["event"]["header"]["name"] = "ErrorResponse";
		root["event"]["payload"]["type"] = "INVALID_DIRECTIVE";
		root["event"]["payload"]["message"] = "Invalid JSON";
		return;
	}

	root["event"]["header"]["namespace"] = "Alexa";
	root["event"]["header"]["name"] = "ErrorResponse";
	root["event"]["header"]["payloadVersion"] = "3";
	root["event"]["header"]["messageId"] = request_json["directive"]["header"]["messageId"].asString();
	root["event"]["payload"]["type"] = "INTERNAL_ERROR";
	root["event"]["payload"]["message"] = "Control not yet implemented";
}

void CWebServer::GetAlexaPage(WebEmSession& session, const request& req, reply& rep)
{
	Json::Value root;

	// Parse request body for Alexa directive
	Json::Value request_json;
	Json::Reader reader;
	if (!reader.parse(req.content, request_json))
	{
		rep.status = http::server::reply::bad_request;
		return;
	}

	// Get directive namespace
	std::string directive_namespace = request_json["directive"]["header"]["namespace"].asString();

	// Route to appropriate handler
	if (directive_namespace == "Alexa.Discovery")
	{
		Alexa_HandleDiscovery(session, req, root);
	}
	else if (directive_namespace == "Alexa.Authorization")
	{
		Alexa_HandleAcceptGrant(session, req, root);
	}
	else if (directive_namespace.find("Alexa") == 0)
	{
		Alexa_HandleControl(session, req, root);
	}
	else
	{
		rep.status = http::server::reply::not_found;
		return;
	}

	reply::set_content(&rep, root.toStyledString());
	rep.status = http::server::reply::ok;
}

} // namespace server
} // namespace http
