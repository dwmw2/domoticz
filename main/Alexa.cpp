#include "stdafx.h"
#include "WebServer.h"
#include "SQLHelper.h"
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

static void CreateErrorResponse(Json::Value& root, const Json::Value& request_json, const std::string& error_type, const std::string& error_message)
{
	root["event"]["header"]["namespace"] = "Alexa";
	root["event"]["header"]["name"] = "ErrorResponse";
	root["event"]["header"]["payloadVersion"] = "3";
	root["event"]["header"]["messageId"] = request_json["directive"]["header"]["messageId"].asString();
	root["event"]["payload"]["type"] = error_type;
	root["event"]["payload"]["message"] = error_message;
}

static void Alexa_HandleControl_ReportState(WebEmSession& session, const Json::Value& request_json, Json::Value& root, uint64_t device_idx)
{
	// TODO: Query device state and return StateReport
	CreateErrorResponse(root, request_json, "INTERNAL_ERROR", "ReportState not yet implemented");
}

static bool CheckDeviceAccess(CWebServer* server, const WebEmSession& session, uint64_t device_idx, bool& bControlPermitted)
{
	bControlPermitted = true;

	switch (session.rights)
	{
	case URIGHTS_ADMIN:
		// Admin can access anything
		return true;

	case URIGHTS_VIEWER:
		bControlPermitted = false;
		[[fallthrough]];

	case URIGHTS_SWITCHER:
	{
		// Find user and check device permissions
		int iUser = server->FindUser(session.username.c_str());
		if ((iUser < 0) || (iUser >= (int)server->m_users.size()))
			return false;

		// If TotSensors is 0, user has access to all devices
		if (server->m_users[iUser].TotSensors == 0)
			return true;

		// Check if device is in user's shared devices
		std::vector<std::vector<std::string>> result =
			m_sql.safe_query("SELECT COUNT(*) FROM SharedDevices WHERE (SharedUserID == '%d') AND (DeviceRowID == '%llu')", server->m_users[iUser].ID, device_idx);
		return (!result.empty() && atoi(result[0][0].c_str()) > 0);
	}

	default:
		// Invalid user rights
		return false;
	}
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

	std::string directive_namespace = request_json["directive"]["header"]["namespace"].asString();
	std::string directive_name = request_json["directive"]["header"]["name"].asString();
	std::string endpoint_id = request_json["directive"]["endpoint"]["endpointId"].asString();
	Json::Value cookie = request_json["directive"]["endpoint"]["cookie"];

	// Parse endpoint ID to determine device type and index
	bool is_scene = false;
	std::string device_idx_str = endpoint_id;

	// Check for scene_ prefix
	if (endpoint_id.find("scene_") == 0)
	{
		is_scene = true;
		device_idx_str = endpoint_id.substr(6); // Remove "scene_" prefix
	}
	else
	{
		// Remove any other xxx_ prefix (e.g., selector_)
		size_t underscore_pos = endpoint_id.find('_');
		if (underscore_pos != std::string::npos)
		{
			device_idx_str = endpoint_id.substr(underscore_pos + 1);
		}
	}

	uint64_t device_idx = std::stoull(device_idx_str);

	// Check access control for devices (scenes don't have per-user access control)
	if (!is_scene)
	{
		bool bControlPermitted;
		if (!CheckDeviceAccess(this, session, device_idx, bControlPermitted))
		{
			CreateErrorResponse(root, request_json, "NO_SUCH_ENDPOINT", "Device not found or access denied");
			return;
		}

		// ReportState is allowed for read-only users
		if (directive_namespace == "Alexa" && directive_name == "ReportState")
		{
			Alexa_HandleControl_ReportState(session, request_json, root, device_idx);
			return;
		}

		// Control operations require write permission
		if (!bControlPermitted)
		{
			CreateErrorResponse(root, request_json, "NO_SUCH_ENDPOINT", "Device control not permitted");
			return;
		}
	}

	// Check if this is a ReportState request
	if (directive_namespace == "Alexa" && directive_name == "ReportState")
	{
		// TODO: Query device state and return StateReport
		CreateErrorResponse(root, request_json, "INTERNAL_ERROR", "ReportState not yet implemented");
		return;
	}

	// Handle control directives (PowerController, BrightnessController, etc.)
	// TODO: Implement control handlers
	root["event"]["header"]["namespace"] = "Alexa";
	root["event"]["header"]["name"] = "ErrorResponse";
	root["event"]["header"]["payloadVersion"] = "3";
	root["event"]["header"]["messageId"] = request_json["directive"]["header"]["messageId"].asString();
	root["event"]["payload"]["type"] = "INTERNAL_ERROR";
	root["event"]["payload"]["message"] = "Control not yet implemented: " + directive_namespace + "/" + directive_name;
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
