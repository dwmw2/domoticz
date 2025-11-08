#include "stdafx.h"
#include "WebServer.h"
#include "mainworker.h"
#include "SQLHelper.h"
#include "Logger.h"
#include "Helper.h"
#include "RFXNames.h"
#include "../webserver/cWebem.h"
#include "../webserver/request.hpp"
#include "../webserver/reply.hpp"
#include "../httpclient/HTTPClient.h"
#include "../hardware/hardwaretypes.h"
#include <json/json.h>
#include <sstream>
#include <string>

extern CLogger _log;

namespace http {
namespace server {

static Json::Value CreateCapability(const std::string& intf)
{
	Json::Value capability;
	capability["type"] = "AlexaInterface";
	capability["interface"] = intf;
	capability["version"] = "3";
	return capability;
}

static Json::Value CreateCapabilityWithProperties(const std::string& intf, const std::string& property_name, bool proactivelyReported, bool retrievable)
{
	Json::Value capability = CreateCapability(intf);
	capability["properties"]["supported"] = Json::Value(Json::arrayValue);
	Json::Value prop;
	prop["name"] = property_name;
	capability["properties"]["supported"].append(prop);
	capability["properties"]["proactivelyReported"] = proactivelyReported;
	capability["properties"]["retrievable"] = retrievable;
	return capability;
}

static Json::Value CreateEndpoint(const std::string& endpoint_id, const std::string& friendly_name, const std::string& description, const std::string& display_category)
{
	Json::Value endpoint;
	endpoint["endpointId"] = endpoint_id;
	endpoint["manufacturerName"] = "Domoticz";
	endpoint["friendlyName"] = friendly_name;
	endpoint["description"] = description;
	endpoint["displayCategories"] = Json::Value(Json::arrayValue);
	endpoint["displayCategories"].append(display_category);
	return endpoint;
}

static std::string GetISO8601Timestamp()
{
	time_t now = mytime(nullptr);
	struct tm tm1;
#ifdef WIN32
	gmtime_s(&tm1, &now);
#else
	gmtime_r(&now, &tm1);
#endif
	char szTmp[80];
	strftime(szTmp, sizeof(szTmp), "%Y-%m-%dT%H:%M:%SZ", &tm1);
	return std::string(szTmp);
}

static Json::Value CreateProperty(const std::string& ns, const std::string& name, const Json::Value& value, const std::string& instance = "")
{
	Json::Value prop;
	prop["namespace"] = ns;
	if (!instance.empty())
		prop["instance"] = instance;
	prop["name"] = name;
	prop["value"] = value;
	prop["timeOfSample"] = GetISO8601Timestamp();
	prop["uncertaintyInMilliseconds"] = 500;
	return prop;
}

static Json::Value CreateEndpointHealthProperty()
{
	Json::Value health_value;
	health_value["value"] = "OK";
	Json::Value health = CreateProperty("Alexa.EndpointHealth", "connectivity", health_value);
	health["uncertaintyInMilliseconds"] = 0;
	return health;
}

static void CreateErrorResponse(Json::Value& root, const Json::Value& request_json, const std::string& error_type, const std::string& error_message)
{
	root["event"]["header"]["namespace"] = "Alexa";
	root["event"]["header"]["name"] = "ErrorResponse";
	root["event"]["header"]["payloadVersion"] = "3";
	if (request_json.isMember("directive") && request_json["directive"].isMember("header") && request_json["directive"]["header"].isMember("messageId"))
		root["event"]["header"]["messageId"] = request_json["directive"]["header"]["messageId"].asString();
	root["event"]["payload"]["type"] = error_type;
	root["event"]["payload"]["message"] = error_message;
}

// Build Alexa instance name from device name in cookie (e.g., "DeviceName.Mode")
static std::string BuildInstanceName(const Json::Value& cookie, const std::string& endpoint_id, const std::string& suffix)
{
	if (cookie.isMember("deviceName"))
	{
		std::string device_name = cookie["deviceName"].asString();
		device_name.erase(std::remove_if(device_name.begin(), device_name.end(),
			[](char c) { return !isalnum(c); }), device_name.end());
		return device_name + suffix;
	}
	return endpoint_id + suffix;
}

static Json::Value CreateFriendlyNames(const std::string& text)
{
	Json::Value names = Json::Value(Json::arrayValue);
	Json::Value name_us;
	name_us["@type"] = "text";
	name_us["value"]["locale"] = "en-US";
	name_us["value"]["text"] = text;
	names.append(name_us);
	Json::Value name_gb;
	name_gb["@type"] = "text";
	name_gb["value"]["locale"] = "en-GB";
	name_gb["value"]["text"] = text;
	names.append(name_gb);
	return names;
}

void CWebServer::Alexa_HandleDiscovery(WebEmSession& session, const request& req, Json::Value& root)
{
	Json::Value request_json;
	Json::Reader reader;
	if (!reader.parse(req.content, request_json))
	{
		root["event"]["header"]["namespace"] = "Alexa";
		CreateErrorResponse(root, request_json, "INVALID_DIRECTIVE", "Invalid JSON");
		return;
	}

	// Build discovery response structure
	root["event"]["header"]["namespace"] = "Alexa.Discovery";
	root["event"]["header"]["name"] = "Discover.Response";
	root["event"]["header"]["payloadVersion"] = "3";
	root["event"]["header"]["messageId"] = request_json["directive"]["header"]["messageId"].asString();
	root["event"]["payload"]["endpoints"] = Json::Value(Json::arrayValue);

	// Get user ID for access control
	int iUser = FindUser(session.username.c_str());
	if (iUser == -1)
	{
		_log.Log(LOG_ERROR, "Alexa Discovery: User '%s' not found", session.username.c_str());
		return;
	}

	// Get devices - all devices for admin, shared devices for regular users
	std::vector<std::vector<std::string>> devices_result;
	if (m_users[iUser].userrights == URIGHTS_ADMIN)
	{
		devices_result = m_sql.safe_query(
			"SELECT DISTINCT d.ID, d.Name, d.Type, d.SubType, d.SwitchType, d.Options "
			"FROM DeviceStatus d "
			"INNER JOIN DeviceToPlansMap p ON d.ID = p.DeviceRowID AND p.DevSceneType = 0 "
			"WHERE d.Used = 1 "
			"AND d.ID NOT IN (SELECT DeviceRowID FROM DeviceToPlansMap WHERE PlanID IN (SELECT ID FROM Plans WHERE Name = '$Hidden Devices') AND DevSceneType = 0) "
			"ORDER BY d.ID");
	}
	else
	{
		unsigned long userID = m_users[iUser].ID;
		devices_result = m_sql.safe_query(
			"SELECT DISTINCT d.ID, d.Name, d.Type, d.SubType, d.SwitchType, d.Options "
			"FROM DeviceStatus d "
			"INNER JOIN SharedDevices s ON d.ID = s.DeviceRowID "
			"INNER JOIN DeviceToPlansMap p ON d.ID = p.DeviceRowID AND p.DevSceneType = 0 "
			"WHERE s.SharedUserID = %lu AND d.Used = 1 "
			"AND d.ID NOT IN (SELECT DeviceRowID FROM DeviceToPlansMap WHERE PlanID IN (SELECT ID FROM Plans WHERE Name = '$Hidden Devices') AND DevSceneType = 0) "
			"ORDER BY d.ID",
			userID);
	}

	for (const auto& device_row : devices_result)
	{
		std::string device_idx = device_row[0];
		std::string device_name = device_row[1];
		int device_type = atoi(device_row[2].c_str());
		int device_subtype = atoi(device_row[3].c_str());
		int switch_type = atoi(device_row[4].c_str());
		std::string options_str = device_row[5];

		// Parse options using BuildDeviceOptions (handles base64 decoding)
		std::map<std::string, std::string> options = m_sql.BuildDeviceOptions(options_str);

		if (IsLightOrSwitch(device_type, device_subtype)
		    || ((device_type == pTypeRego6XXValue) && (device_subtype == sTypeRego6XXStatus)))
		{
			// Handle selector switches
			if (switch_type == STYPE_Selector)
			{
				Json::Value endpoint = CreateEndpoint("selector_" + device_idx, device_name, "Selector Switch", "OTHER");

				// Create instance name from device name (remove spaces)
				std::string instance_name = device_name;
				instance_name.erase(std::remove(instance_name.begin(), instance_name.end(), ' '), instance_name.end());
				instance_name += ".Mode";

				// Add ModeController capability
				Json::Value mode_capability = CreateCapabilityWithProperties("Alexa.ModeController", "mode", false, true);
				mode_capability["instance"] = instance_name;

				// Add capability resources (friendly names for the capability itself)
				mode_capability["capabilityResources"]["friendlyNames"] = CreateFriendlyNames(device_name);

				// Parse LevelNames from options
				std::map<std::string, std::string> selector_statuses;
				GetSelectorSwitchStatuses(options, selector_statuses);

				if (selector_statuses.empty())
				{
					_log.Log(LOG_ERROR, "(Alexa) Selector switch '%s' (idx %s) has no level names configured", device_name.c_str(), device_idx.c_str());
					continue;
				}

				// Check if Off level should be hidden
				bool hide_off_level = false;
				auto it_hide = options.find("LevelOffHidden");
				if (it_hide != options.end() && it_hide->second == "true")
				{
					hide_off_level = true;
				}

				mode_capability["configuration"]["ordered"] = false;
				mode_capability["configuration"]["supportedModes"] = Json::Value(Json::arrayValue);

				for (const auto& status : selector_statuses)
				{
					int level_value = atoi(status.first.c_str());

					// Skip level 0 if LevelOffHidden is true
					if (level_value == 0 && hide_off_level)
					{
						continue;
					}

					Json::Value mode;
					mode["value"] = "Level." + std::to_string(level_value);
					mode["modeResources"]["friendlyNames"] = CreateFriendlyNames(status.second);
					mode_capability["configuration"]["supportedModes"].append(mode);
				}
				endpoint["capabilities"] = Json::Value(Json::arrayValue);
				endpoint["capabilities"].append(mode_capability);
				endpoint["capabilities"].append(CreateCapability("Alexa"));

				// Add cookie with metadata
				endpoint["cookie"]["WhatAmI"] = "selector";
				endpoint["cookie"]["switchtype"] = switch_type;
				endpoint["cookie"]["deviceName"] = device_name;

				root["event"]["payload"]["endpoints"].append(endpoint);
			}
		}
	}

	// Get scenes/groups that are in room plans and not protected
	std::vector<std::vector<std::string>> scenes_result;
	scenes_result = m_sql.safe_query(
		"SELECT s.ID, s.Name, s.SceneType, s.Protected "
		"FROM Scenes s "
		"INNER JOIN DeviceToPlansMap d ON s.ID = d.DeviceRowID AND d.DevSceneType = 1 "
		"WHERE s.Protected = 0 "
		"ORDER BY s.Name");

	for (const auto& scene_row : scenes_result)
	{
		std::string scene_idx = scene_row[0];
		int scene_type = atoi(scene_row[2].c_str());

		Json::Value endpoint = CreateEndpoint("scene_" + scene_idx, scene_row[1], (scene_type == 0) ? "Scene" : "Group", (scene_type == 0) ? "SCENE_TRIGGER" : "SWITCH");

		// Scenes use SceneController, Groups use PowerController
		if (scene_type == 0) // Scene
		{
			// Add SceneController capability
			Json::Value scene_capability = CreateCapability("Alexa.SceneController");
			scene_capability["supportsDeactivation"] = false;

			endpoint["capabilities"] = Json::Value(Json::arrayValue);
			endpoint["capabilities"].append(scene_capability);
		}
		else // Group
		{
			// Add PowerController capability
			Json::Value power_capability = CreateCapabilityWithProperties("Alexa.PowerController", "powerState", false, true);

			endpoint["capabilities"] = Json::Value(Json::arrayValue);
			endpoint["capabilities"].append(power_capability);
		}

		// Add Alexa capability
		endpoint["capabilities"].append(CreateCapability("Alexa"));

		// Add cookie with metadata
		endpoint["cookie"]["WhatAmI"] = "scene";
		endpoint["cookie"]["SceneType"] = scene_type;

		root["event"]["payload"]["endpoints"].append(endpoint);
	}
}

void CWebServer::Alexa_HandleAcceptGrant(WebEmSession& session, const request& req, Json::Value& root)
{
	Json::Value request_json;
	Json::Reader reader;
	if (!reader.parse(req.content, request_json))
	{
		root["event"]["header"]["namespace"] = "Alexa";
		CreateErrorResponse(root, request_json, "INVALID_DIRECTIVE", "Invalid JSON");
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

static void Alexa_HandleControl_scene(WebEmSession& session, const Json::Value& request_json, Json::Value& root, uint64_t device_idx, const std::string& directive_namespace, const std::string& directive_name)
{
	// Check if scene is protected
	std::vector<std::vector<std::string>> result;
	result = m_sql.safe_query("SELECT Protected FROM Scenes WHERE (ID = %llu)", device_idx);
	if (result.empty() || atoi(result[0][0].c_str()) != 0)
	{
		CreateErrorResponse(root, request_json, "NO_SUCH_ENDPOINT", "Scene not found or protected");
		return;
	}

	// Check if this is a ReportState request
	if (directive_namespace == "Alexa" && directive_name == "ReportState")
	{
		// Query the scene/group info
		result = m_sql.safe_query("SELECT nValue, SceneType FROM Scenes WHERE (ID = %llu)", device_idx);
		if (result.empty())
		{
			CreateErrorResponse(root, request_json, "NO_SUCH_ENDPOINT", "Scene/Group not found");
			return;
		}

		// Only groups (not scenes) support state reporting
		int scene_type = atoi(result[0][1].c_str());
		if (scene_type == 0) // Scene
		{
			CreateErrorResponse(root, request_json, "INVALID_DIRECTIVE", "ReportState not supported for scenes");
			return;
		}

		int nValue = atoi(result[0][0].c_str());
		std::string power_state = (nValue != 0) ? "ON" : "OFF";

		// Return StateReport
		root["event"]["header"]["name"] = "StateReport";

		// Add powerState to context
		root["context"]["properties"].append(CreateProperty("Alexa.PowerController", "powerState", power_state));
		root["context"]["properties"].append(CreateEndpointHealthProperty());
		return;
	}

	// Handle control directives
	if (directive_namespace == "Alexa.SceneController")
	{
		// SceneController: Activate (for Scenes only)
		if (directive_name == "Activate")
		{
			// Switch the scene
			std::string idx_str = std::to_string(device_idx);
			_log.Log(LOG_STATUS, "User: %s initiated a scene command via Alexa", session.username.c_str());
			if (!m_mainworker.SwitchScene(idx_str, "On", session.username))
			{
				root["event"]["header"]["namespace"] = "Alexa";
				CreateErrorResponse(root, request_json, "ENDPOINT_UNREACHABLE", "Scene activation failed");
				return;
			}

			// Return ActivationStarted response
			root["event"]["header"]["namespace"] = "Alexa.SceneController";
			root["event"]["header"]["name"] = "ActivationStarted";
			root["event"]["payload"]["cause"]["type"] = "VOICE_INTERACTION";
			root["event"]["payload"]["timestamp"] = GetISO8601Timestamp();
			return;
		}
		else
		{
			CreateErrorResponse(root, request_json, "INVALID_DIRECTIVE", "SceneController only supports Activate");
			return;
		}
	}
	else if (directive_namespace == "Alexa.PowerController")
	{
		// PowerController: TurnOn/TurnOff (for Groups)
		if (directive_name == "TurnOn" || directive_name == "TurnOff")
		{
			// Switch the group
			std::string idx_str = std::to_string(device_idx);
			std::string switchcmd = (directive_name == "TurnOn") ? "On" : "Off";
			_log.Log(LOG_STATUS, "User: %s initiated a group command via Alexa", session.username.c_str());
			if (!m_mainworker.SwitchScene(idx_str, switchcmd, session.username))
			{
				CreateErrorResponse(root, request_json, "ENDPOINT_UNREACHABLE", "Group switch failed");
				return;
			}

			// Add powerState to context
			root["context"]["properties"].append(CreateProperty("Alexa.PowerController", "powerState", (directive_name == "TurnOn") ? "ON" : "OFF"));
			root["context"]["properties"].append(CreateEndpointHealthProperty());
			return;
		}
		else
		{
			CreateErrorResponse(root, request_json, "INVALID_DIRECTIVE", "PowerController only supports TurnOn/TurnOff");
			return;
		}
	}

	// Unsupported directive for scenes
	CreateErrorResponse(root, request_json, "INVALID_DIRECTIVE", "Unsupported directive for scene/group");
}

static void Alexa_HandleControl_ModeController(WebEmSession& session, const Json::Value& request_json, Json::Value& root, uint64_t device_idx, const std::string& directive_name)
{
	if (directive_name == "SetMode")
	{
		Json::Value cookie = request_json["directive"]["endpoint"]["cookie"];
		std::string endpoint_id = request_json["directive"]["endpoint"]["endpointId"].asString();
		std::string mode_value = request_json["directive"]["payload"]["mode"].asString();

		// Parse level from "Level.X" format
		int level = 0;
		if (mode_value.find("Level.") == 0)
		{
			level = atoi(mode_value.substr(6).c_str());
		}
		else
		{
			level = atoi(mode_value.c_str());
		}

		// Switch the selector to the specified level
		_log.Log(LOG_STATUS, "User: %s set selector switch %llu to level %d via Alexa", session.username.c_str(), device_idx, level);

		if (m_mainworker.SwitchLight(device_idx, "Set Level", level, NoColor, false, 0, session.username) == MainWorker::SL_ERROR)
		{
			CreateErrorResponse(root, request_json, "ENDPOINT_UNREACHABLE", "Unable to control device - hardware communication error");
			return;
		}

		// Build instance name from device name in cookie
		std::string instance_name = BuildInstanceName(cookie, endpoint_id, ".Mode");

		// Add mode to context
		root["context"]["properties"].append(CreateProperty("Alexa.ModeController", "mode", mode_value, instance_name));
		root["context"]["properties"].append(CreateEndpointHealthProperty());
		return;
	}

	// Unsupported directive for ModeController
	CreateErrorResponse(root, request_json, "INVALID_DIRECTIVE", "ModeController only supports SetMode");
}

static void Alexa_HandleControl_ReportState(WebEmSession& session, const Json::Value& request_json, Json::Value& root, uint64_t device_idx)
{
	Json::Value cookie = request_json["directive"]["endpoint"]["cookie"];
	std::string endpoint_id = request_json["directive"]["endpoint"]["endpointId"].asString();

	// Query device state
	std::vector<std::vector<std::string>> result;
	result = m_sql.safe_query("SELECT Type, SubType, SwitchType, nValue, sValue FROM DeviceStatus WHERE (ID = %llu)", device_idx);
	if (result.empty())
	{
		CreateErrorResponse(root, request_json, "NO_SUCH_ENDPOINT", "Device not found");
		return;
	}

	unsigned char dType = atoi(result[0][0].c_str());
	unsigned char dSubType = atoi(result[0][1].c_str());
	_eSwitchType switchtype = (_eSwitchType)atoi(result[0][2].c_str());

	// Begin creating a StateReport
	root["event"]["header"]["name"] = "StateReport";

	// First, switches...
	if (IsLightOrSwitch(dType, dSubType)
	    || ((dType == pTypeRego6XXValue) && (dSubType == sTypeRego6XXStatus)))
	{
		unsigned char nValue = atoi(result[0][3].c_str());
		std::string sValue = result[0][4];

		std::string lstatus;
		int level;
		bool bHaveDimmer, bHaveGroupCmd;
		int maxDimLevel;
		GetLightStatus(dType, dSubType, switchtype, nValue, sValue, lstatus, level, bHaveDimmer, maxDimLevel, bHaveGroupCmd);

		if (switchtype == STYPE_Selector)
		{
			// Build instance name from device name in cookie
			std::string instance_name = BuildInstanceName(cookie, endpoint_id, ".Mode");

			// Add mode to context with Level.X format
			root["context"]["properties"].append(CreateProperty("Alexa.ModeController", "mode", "Level." + std::to_string(level), instance_name));
			root["context"]["properties"].append(CreateEndpointHealthProperty());
			return;
		}
	}

	// Unsupported device type for ReportState
	CreateErrorResponse(root, request_json, "INVALID_DIRECTIVE", "ReportState not supported for this device type");
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
		CreateErrorResponse(root, request_json, "INVALID_DIRECTIVE", "Invalid JSON");
		return;
	}

	std::string directive_namespace = request_json["directive"]["header"]["namespace"].asString();
	std::string directive_name = request_json["directive"]["header"]["name"].asString();
	std::string endpoint_id = request_json["directive"]["endpoint"]["endpointId"].asString();
	Json::Value cookie = request_json["directive"]["endpoint"]["cookie"];

	// Build base response structure (can be modified by handlers)
	root["event"]["header"]["namespace"] = "Alexa";
	root["event"]["header"]["name"] = "Response";
	root["event"]["header"]["payloadVersion"] = "3";
	root["event"]["header"]["messageId"] = request_json["directive"]["header"]["messageId"].asString();
	root["event"]["endpoint"]["endpointId"] = endpoint_id;
	root["event"]["payload"] = Json::Value(Json::objectValue);
	root["context"]["properties"] = Json::Value(Json::arrayValue);

	// Parse endpoint ID into prefix and index
	std::string prefix;
	std::string device_idx_str;
	size_t underscore_pos = endpoint_id.rfind('_');
	if (underscore_pos != std::string::npos)
	{
		prefix = endpoint_id.substr(0, underscore_pos + 1);
		device_idx_str = endpoint_id.substr(underscore_pos + 1);
	}
	else
	{
		device_idx_str = endpoint_id;
	}

	uint64_t device_idx = std::stoull(device_idx_str);

	// Handle scenes/groups
	if (prefix == "scene_")
	{
		Alexa_HandleControl_scene(session, request_json, root, device_idx, directive_namespace, directive_name);
		return;
	}

	// Check access control for devices
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

	// Dispatch to controller-specific handlers
	typedef void (*ControllerHandler)(WebEmSession&, const Json::Value&, Json::Value&, uint64_t, const std::string&);

	static const std::map<std::string, ControllerHandler> controller_handlers = {
		{"Alexa.ModeController", Alexa_HandleControl_ModeController}
	};

	// Look up handler in dispatch map
	auto it = controller_handlers.find(directive_namespace);
	if (it != controller_handlers.end())
	{
		it->second(session, request_json, root, device_idx, directive_name);
		return;
	}

	// TODO: Implement other control handlers
	_log.Log(LOG_ERROR, "Alexa: Unimplemented control: %s/%s", directive_namespace.c_str(), directive_name.c_str());
	CreateErrorResponse(root, request_json, "INTERNAL_ERROR", "Control not yet implemented: " + directive_namespace + "/" + directive_name);
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
