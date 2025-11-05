#include "stdafx.h"
#include "WebServer.h"
#include "Logger.h"
#include "../webserver/cWebem.h"
#include "../webserver/request.hpp"
#include "../webserver/reply.hpp"
#include "../httpclient/HTTPClient.h"
#include <json/json.h>
#include <sstream>
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

	// Build discovery response structure
	root["event"]["header"]["namespace"] = "Alexa.Discovery";
	root["event"]["header"]["name"] = "Discover.Response";
	root["event"]["header"]["payloadVersion"] = "3";
	root["event"]["header"]["messageId"] = request_json["directive"]["header"]["messageId"].asString();
	root["event"]["payload"]["endpoints"] = Json::Value(Json::arrayValue);

	// TODO: Query devices and scenes directly from database
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

	// Get authenticated username from session
	std::string username = session.username;
	_log.Log(LOG_STATUS, "Alexa: AcceptGrant for user '%s'", username.c_str());

	// Extract authorization code
	std::string auth_code = request_json["directive"]["payload"]["grant"]["code"].asString();

	if (!auth_code.empty())
	{
		// TODO: Get CLIENT_ID and CLIENT_SECRET from configuration
		std::string client_id = "";
		std::string client_secret = "";

		if (!client_id.empty() && !client_secret.empty())
		{
			// Exchange authorization code for tokens
			std::stringstream post_data;
			post_data << "grant_type=authorization_code"
				<< "&code=" << auth_code
				<< "&client_id=" << client_id
				<< "&client_secret=" << client_secret;

			std::vector<std::string> headers;
			headers.push_back("Content-Type: application/x-www-form-urlencoded");

			std::string response_data;
			std::vector<std::string> response_headers;

			// Make HTTPS POST to Amazon LWA
			if (HTTPClient::POST("https://api.amazon.com/auth/o2/token",
				post_data.str(), headers, response_data, response_headers))
			{
				// Parse token response
				Json::Value token_response;
				if (reader.parse(response_data, token_response))
				{
					std::string access_token = token_response["access_token"].asString();
					std::string refresh_token = token_response["refresh_token"].asString();
					int expires_in = token_response["expires_in"].asInt();

					_log.Log(LOG_STATUS, "Alexa: Received access token for user '%s' (expires in %d seconds)", username.c_str(), expires_in);
					_log.Log(LOG_STATUS, "Alexa: Received refresh token for user '%s'", username.c_str());

					// TODO: Store tokens in database for user '%s'
				}
			}
		}
	}

	// Always return success response (even if token exchange fails)
	root["event"]["header"]["namespace"] = "Alexa.Authorization";
	root["event"]["header"]["name"] = "AcceptGrant.Response";
	root["event"]["header"]["payloadVersion"] = "3";
	root["event"]["header"]["messageId"] = request_json["directive"]["header"]["messageId"].asString();
	root["event"]["payload"] = Json::Value(Json::objectValue);
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
