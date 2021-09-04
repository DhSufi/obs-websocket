#pragma once

#include "RequestStatus.h"
#include "../../utils/Json.h"

enum ObsWebSocketSceneFilter {
	OBS_WEBSOCKET_SCENE_FILTER_SCENE_ONLY,
	OBS_WEBSOCKET_SCENE_FILTER_GROUP_ONLY,
	OBS_WEBSOCKET_SCENE_FILTER_SCENE_OR_GROUP,
};

struct Request
{
	Request(const std::string requestType, const json requestData = nullptr);

	// Contains the key and is not null
	const bool Contains(const std::string keyName) const;

	const bool ValidateBasic(const std::string keyName, RequestStatus::RequestStatus &statusCode, std::string &comment) const;
	const bool ValidateOptionalNumber(const std::string keyName, RequestStatus::RequestStatus &statusCode, std::string &comment, const double minValue = -INFINITY, const double maxValue = INFINITY) const;
	const bool ValidateNumber(const std::string keyName, RequestStatus::RequestStatus &statusCode, std::string &comment, const double minValue = -INFINITY, const double maxValue = INFINITY) const;
	const bool ValidateOptionalString(const std::string keyName, RequestStatus::RequestStatus &statusCode, std::string &comment, const bool allowEmpty = false) const;
	const bool ValidateString(const std::string keyName, RequestStatus::RequestStatus &statusCode, std::string &comment, const bool allowEmpty = false) const;
	const bool ValidateOptionalBoolean(const std::string keyName, RequestStatus::RequestStatus &statusCode, std::string &comment) const;
	const bool ValidateBoolean(const std::string keyName, RequestStatus::RequestStatus &statusCode, std::string &comment) const;
	const bool ValidateOptionalObject(const std::string keyName, RequestStatus::RequestStatus &statusCode, std::string &comment, const bool allowEmpty = false) const;
	const bool ValidateObject(const std::string keyName, RequestStatus::RequestStatus &statusCode, std::string &comment, const bool allowEmpty = false) const;
	const bool ValidateOptionalArray(const std::string keyName, RequestStatus::RequestStatus &statusCode, std::string &comment, const bool allowEmpty = false) const;
	const bool ValidateArray(const std::string keyName, RequestStatus::RequestStatus &statusCode, std::string &comment, const bool allowEmpty = false) const;

	// All return values have incremented refcounts
	obs_source_t *ValidateSource(const std::string keyName, RequestStatus::RequestStatus &statusCode, std::string &comment) const;
	obs_source_t *ValidateScene(const std::string keyName, RequestStatus::RequestStatus &statusCode, std::string &comment, const ObsWebSocketSceneFilter filter = OBS_WEBSOCKET_SCENE_FILTER_SCENE_ONLY) const;
	obs_source_t *ValidateInput(const std::string keyName, RequestStatus::RequestStatus &statusCode, std::string &comment) const;
	obs_sceneitem_t *ValidateSceneItem(const std::string sceneKeyName, const std::string sceneItemIdKeyName, RequestStatus::RequestStatus &statusCode, std::string &comment, const ObsWebSocketSceneFilter filter = OBS_WEBSOCKET_SCENE_FILTER_SCENE_ONLY) const;

	const bool HasRequestData;
	const std::string RequestType;
	const json RequestData;
};
