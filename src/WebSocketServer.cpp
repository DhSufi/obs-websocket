#include <chrono>
#include <thread>
#include <QtConcurrent>
#include <QDateTime>
#include <obs-module.h>
#include <obs-frontend-api.h>

#include "WebSocketServer.h"
#include "eventhandler/EventHandler.h"
#include "obs-websocket.h"
#include "Config.h"
#include "utils/Crypto.h"
#include "utils/Platform.h"
#include "plugin-macros.generated.h"

WebSocketServer::WebSocketServer() :
	QObject(nullptr),
	_sessions()
{
	_server.get_alog().clear_channels(websocketpp::log::alevel::all);
	_server.get_elog().clear_channels(websocketpp::log::elevel::all);
	_server.init_asio();

#ifndef _WIN32
	_server.set_reuse_addr(true);
#endif

	_server.set_validate_handler(
		websocketpp::lib::bind(
			&WebSocketServer::onValidate, this, websocketpp::lib::placeholders::_1
		)
	);
	_server.set_open_handler(
		websocketpp::lib::bind(
			&WebSocketServer::onOpen, this, websocketpp::lib::placeholders::_1
		)
	);
	_server.set_close_handler(
		websocketpp::lib::bind(
			&WebSocketServer::onClose, this, websocketpp::lib::placeholders::_1
		)
	);
	_server.set_message_handler(
		websocketpp::lib::bind(
			&WebSocketServer::onMessage, this, websocketpp::lib::placeholders::_1, websocketpp::lib::placeholders::_2
		)
	);

	auto eventHandler = GetEventHandler();
	eventHandler->SetBroadcastCallback(
		std::bind(
			&WebSocketServer::BroadcastEvent, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4
		)
	);
}

WebSocketServer::~WebSocketServer()
{
	if (_server.is_listening())
		Stop();
}

void WebSocketServer::ServerRunner()
{
	blog(LOG_INFO, "[WebSocketServer::ServerRunner] IO thread started.");
	try {
		_server.run();
	} catch (websocketpp::exception const & e) {
		blog(LOG_ERROR, "[WebSocketServer::ServerRunner] websocketpp instance returned an error: %s", e.what());
	} catch (const std::exception & e) {
		blog(LOG_ERROR, "[WebSocketServer::ServerRunner] websocketpp instance returned an error: %s", e.what());
	} catch (...) {
		blog(LOG_ERROR, "[WebSocketServer::ServerRunner] websocketpp instance returned an error");
	}
	blog(LOG_INFO, "[WebSocketServer::ServerRunner] IO thread exited.");
}

void WebSocketServer::Start()
{
	if (_server.is_listening()) {
		blog(LOG_WARNING, "[WebSocketServer::Start] Call to Start() but the server is already listening.");
		return;
	}

	auto conf = GetConfig();
	if (!conf) {
		blog(LOG_ERROR, "[WebSocketServer::Start] Unable to retreive config!");
		return;
	}

	_serverPort = conf->ServerPort;
	_serverPassword = conf->ServerPassword;
	_debugEnabled = conf->DebugEnabled;
	AuthenticationRequired = conf->AuthRequired;
	AuthenticationSalt = Utils::Crypto::GenerateSalt();
	AuthenticationSecret = Utils::Crypto::GenerateSecret(conf->ServerPassword.toStdString(), AuthenticationSalt);

	// Set log levels if debug is enabled
	if (_debugEnabled) {
		_server.get_alog().set_channels(websocketpp::log::alevel::all);
		_server.get_alog().clear_channels(websocketpp::log::alevel::frame_header | websocketpp::log::alevel::frame_payload | websocketpp::log::alevel::control);
		_server.get_elog().set_channels(websocketpp::log::elevel::all);
		_server.get_alog().clear_channels(websocketpp::log::elevel::devel | websocketpp::log::elevel::library);
	} else {
		_server.get_alog().clear_channels(websocketpp::log::alevel::all);
		_server.get_elog().clear_channels(websocketpp::log::elevel::all);
	}

	_server.reset();

	websocketpp::lib::error_code errorCode;
	_server.listen(websocketpp::lib::asio::ip::tcp::v4(), _serverPort, errorCode);

	if (errorCode) {
		std::string errorCodeMessage = errorCode.message();
		blog(LOG_INFO, "[WebSocketServer::Start] Listen failed: %s", errorCodeMessage.c_str());
		return;
	}

	_server.start_accept();

	_serverThread = std::thread(&WebSocketServer::ServerRunner, this);

	blog(LOG_INFO, "[WebSocketServer::Start] Server started successfully on port %d. Possible connect address: %s", _serverPort, Utils::Platform::GetLocalAddress().c_str());
}

void WebSocketServer::Stop()
{
	if (!_server.is_listening()) {
		blog(LOG_WARNING, "[WebSocketServer::Stop] Call to Stop() but the server is not listening.");
		return;
	}

	_server.stop_listening();

	std::unique_lock<std::mutex> lock(_sessionMutex);
	for (auto const& [hdl, session] : _sessions) {
		websocketpp::lib::error_code errorCode;
		_server.pause_reading(hdl, errorCode);
		if (errorCode) {
			blog(LOG_INFO, "[WebSocketServer::Stop] Error: %s", errorCode.message().c_str());
			continue;
		}

		_server.close(hdl, websocketpp::close::status::going_away, "Server stopping.", errorCode);
		if (errorCode) {
			blog(LOG_INFO, "[WebSocketServer::Stop] Error: %s", errorCode.message().c_str());
			continue;
		}
	}
	lock.unlock();

	_threadPool.waitForDone();

	// This can delay the thread that it is running on. Bad but kinda required.
	while (_sessions.size() > 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	_serverThread.join();

	blog(LOG_INFO, "[WebSocketServer::Stop] Server stopped successfully");
}

void WebSocketServer::InvalidateSession(websocketpp::connection_hdl hdl)
{
	blog(LOG_INFO, "[WebSocketServer::InvalidateSession] Invalidating a session.");

	websocketpp::lib::error_code errorCode;
	_server.pause_reading(hdl, errorCode);
	if (errorCode) {
		blog(LOG_INFO, "[WebSocketServer::InvalidateSession] Error: %s", errorCode.message().c_str());
		return;
	}

	_server.close(hdl, WebSocketCloseCode::SessionInvalidated, "Your session has been invalidated.", errorCode);
	if (errorCode) {
		blog(LOG_INFO, "[WebSocketServer::InvalidateSession] Error: %s", errorCode.message().c_str());
		return;
	}
}

std::vector<WebSocketServer::WebSocketSessionState> WebSocketServer::GetWebSocketSessions()
{
	std::vector<WebSocketServer::WebSocketSessionState> webSocketSessions;

	std::unique_lock<std::mutex> lock(_sessionMutex);
	for (auto & [hdl, session] : _sessions) {
		uint64_t connectedAt = session->ConnectedAt();
		uint64_t incomingMessages = session->IncomingMessages();
		uint64_t outgoingMessages = session->OutgoingMessages();
		std::string remoteAddress = session->RemoteAddress();
		bool isIdentified = session->IsIdentified();
		
		webSocketSessions.emplace_back(WebSocketSessionState{hdl, remoteAddress, connectedAt, incomingMessages, outgoingMessages, isIdentified});
	}
	lock.unlock();

	return webSocketSessions;
}

// It isn't consistent to directly call the WebSocketServer from the events system, but it would also be dumb to make it unnecessarily complicated.

bool WebSocketServer::onValidate(websocketpp::connection_hdl hdl)
{
	auto conn = _server.get_con_from_hdl(hdl);

	std::vector<std::string> requestedSubprotocols = conn->get_requested_subprotocols();
	for (auto subprotocol : requestedSubprotocols) {
		if (subprotocol == "obswebsocket.json" || subprotocol == "obswebsocket.msgpack") {
			conn->select_subprotocol(subprotocol);
			break;
		}
	}

	return true;
}

void WebSocketServer::onOpen(websocketpp::connection_hdl hdl)
{
	auto conn = _server.get_con_from_hdl(hdl);

	// Build new session
	std::unique_lock<std::mutex> lock(_sessionMutex);
	SessionPtr session = _sessions[hdl] = std::make_shared<WebSocketSession>();
	std::unique_lock<std::mutex> sessionLock(session->OperationMutex);
	lock.unlock();

	// Configure session details
	session->SetRemoteAddress(conn->get_remote_endpoint());
	session->SetConnectedAt(QDateTime::currentSecsSinceEpoch());
	session->SetAuthenticationRequired(AuthenticationRequired);
	std::string selectedSubprotocol = conn->get_subprotocol();
	if (!selectedSubprotocol.empty()) {
		if (selectedSubprotocol == "obswebsocket.json")
			session->SetEncoding(WebSocketEncoding::Json);
		else if (selectedSubprotocol == "obswebsocket.msgpack")
			session->SetEncoding(WebSocketEncoding::MsgPack);
	}

	// Build `Hello`
	json helloMessageData;
	helloMessageData["obsWebSocketVersion"] = OBS_WEBSOCKET_VERSION;
	helloMessageData["rpcVersion"] = OBS_WEBSOCKET_RPC_VERSION;
	if (AuthenticationRequired) {
		session->SetSecret(AuthenticationSecret);
		std::string sessionChallenge = Utils::Crypto::GenerateSalt();
		session->SetChallenge(sessionChallenge);
		helloMessageData["authentication"] = json::object();
		helloMessageData["authentication"]["challenge"] = sessionChallenge;
		helloMessageData["authentication"]["salt"] = AuthenticationSalt;
	}
	json helloMessage;
	helloMessage["op"] = 0;
	helloMessage["d"] = helloMessageData;

	sessionLock.unlock();

	// Build SessionState object for signal
	WebSocketSessionState state;
	state.remoteAddress = session->RemoteAddress();
	state.connectedAt = session->ConnectedAt();
	state.incomingMessages = session->IncomingMessages();
	state.outgoingMessages = session->OutgoingMessages();
	state.isIdentified = session->IsIdentified();

	// Emit signals
	emit ClientConnected(state);

	// Log connection
	blog(LOG_INFO, "[WebSocketServer::onOpen] New WebSocket client has connected from %s", session->RemoteAddress().c_str());

	// Send object to client
	websocketpp::lib::error_code errorCode;
	auto sessionEncoding = session->Encoding();
	if (sessionEncoding == WebSocketEncoding::Json) {
		std::string helloMessageJson = helloMessage.dump();
		_server.send(hdl, helloMessageJson, websocketpp::frame::opcode::text, errorCode);
	} else if (sessionEncoding == WebSocketEncoding::MsgPack) {
		auto msgPackData = json::to_msgpack(helloMessage);
		std::string messageMsgPack(msgPackData.begin(), msgPackData.end());
		_server.send(hdl, messageMsgPack, websocketpp::frame::opcode::binary, errorCode);
	}
	session->IncrementOutgoingMessages();
}

void WebSocketServer::onClose(websocketpp::connection_hdl hdl)
{
	auto conn = _server.get_con_from_hdl(hdl);

	// Get info from the session and then delete it
	std::unique_lock<std::mutex> lock(_sessionMutex);
	SessionPtr session = _sessions[hdl];
	uint64_t eventSubscriptions = session->EventSubscriptions();
	bool isIdentified = session->IsIdentified();
	uint64_t connectedAt = session->ConnectedAt();
	uint64_t incomingMessages = session->IncomingMessages();
	uint64_t outgoingMessages = session->OutgoingMessages();
	std::string remoteAddress = session->RemoteAddress();
	_sessions.erase(hdl);
	lock.unlock();

	// If client was identified, decrement appropriate refs in eventhandler.
	if (isIdentified) {
		auto eventHandler = GetEventHandler();
		eventHandler->ProcessUnsubscription(eventSubscriptions);
	}

	// Build SessionState object for signal
	WebSocketSessionState state;
	state.remoteAddress = remoteAddress;
	state.connectedAt = connectedAt;
	state.incomingMessages = incomingMessages;
	state.outgoingMessages = outgoingMessages;
	state.isIdentified = isIdentified;

	// Emit signals
	emit ClientDisconnected(state, conn->get_local_close_code());

	// Log disconnection
	blog(LOG_INFO, "[WebSocketServer::onClose] WebSocket client %s has disconnected", remoteAddress.c_str());

	// Get config for tray notification
	auto conf = GetConfig();
	if (!conf) {
		blog(LOG_ERROR, "[WebSocketServer::onClose] Unable to retreive config!");
		return;
	}

	// If previously identified, not going away, and notifications enabled, send a tray notification
	if (isIdentified && (conn->get_local_close_code() != websocketpp::close::status::going_away) && conf->AlertsEnabled) {
		QString title = obs_module_text("OBSWebSocket.TrayNotification.Disconnected.Title");
		QString body = QString(obs_module_text("OBSWebSocket.TrayNotification.Disconnected.Body")).arg(QString::fromStdString(remoteAddress));
		Utils::Platform::SendTrayNotification(QSystemTrayIcon::Information, title, body);
	}
}

void WebSocketServer::onMessage(websocketpp::connection_hdl hdl, websocketpp::server<websocketpp::config::asio>::message_ptr message)
{
	auto opcode = message->get_opcode();
	std::string payload = message->get_payload();
	QtConcurrent::run(&_threadPool, [=]() {
		std::unique_lock<std::mutex> lock(_sessionMutex);
		SessionPtr session;
		try {
			session = _sessions.at(hdl);
		} catch (const std::out_of_range& oor) {
			return;
		}
		lock.unlock();

		session->IncrementIncomingMessages();

		json incomingMessage;

		// Check for invalid opcode and decode
		websocketpp::lib::error_code errorCode;
		uint8_t sessionEncoding = session->Encoding();
		if (sessionEncoding == WebSocketEncoding::Json) {
			if (opcode != websocketpp::frame::opcode::text) {
				if (!session->IgnoreInvalidMessages())
					_server.close(hdl, WebSocketCloseCode::MessageDecodeError, "Your session encoding is set to Json, but a binary message was received.", errorCode);
				return;
			}

			try {
				incomingMessage = json::parse(payload);
			} catch (json::parse_error& e) {
				if (!session->IgnoreInvalidMessages())
					_server.close(hdl, WebSocketCloseCode::MessageDecodeError, std::string("Unable to decode Json: ") + e.what(), errorCode);
				return;
			}
		} else if (sessionEncoding == WebSocketEncoding::MsgPack) {
			if (opcode != websocketpp::frame::opcode::binary) {
				if (!session->IgnoreInvalidMessages())
					_server.close(hdl, WebSocketCloseCode::MessageDecodeError, "Your session encoding is set to MsgPack, but a text message was received.", errorCode);
				return;
			}

			try {
				incomingMessage = json::from_msgpack(payload);
			} catch (json::parse_error& e) {
				if (!session->IgnoreInvalidMessages())
					_server.close(hdl, WebSocketCloseCode::MessageDecodeError, std::string("Unable to decode MsgPack: ") + e.what(), errorCode);
				return;
			}
		}

		if (_debugEnabled)
			blog(LOG_INFO, "[WebSocketServer::onMessage] Incoming message (decoded):\n%s", incomingMessage.dump(2).c_str());

		ProcessResult ret;

		// Verify incoming message is an object
		if (!incomingMessage.is_object()) {
			if (!session->IgnoreInvalidMessages()) {
				ret.closeCode = WebSocketCloseCode::MessageDecodeError;
				ret.closeReason = "You sent a non-object payload.";
				goto skipProcessing;
			}
			return;
		}

		// Disconnect client if 4.x protocol is detected
		if (!session->IsIdentified() && incomingMessage.contains("request-type")) {
			blog(LOG_WARNING, "[WebSocketServer::onMessage] Client %s appears to be running a pre-5.0.0 protocol.", session->RemoteAddress().c_str());
			ret.closeCode = WebSocketCloseCode::UnsupportedRpcVersion;
			ret.closeReason = "You appear to be attempting to connect with the pre-5.0.0 plugin protocol. Check to make sure your client is updated.";
			goto skipProcessing;
		}

		// Validate op code
		if (!incomingMessage.contains("op")) {
			if (!session->IgnoreInvalidMessages()) {
				ret.closeCode = WebSocketCloseCode::UnknownOpCode;
				ret.closeReason = "Your request is missing an `op`.";
				goto skipProcessing;
			}
			return;
		}

		ProcessMessage(session, ret, incomingMessage["op"], incomingMessage["d"]);

skipProcessing:
		if (ret.closeCode != WebSocketCloseCode::DontClose) {
			websocketpp::lib::error_code errorCode;
			_server.close(hdl, ret.closeCode, ret.closeReason, errorCode);
			return;
		}

		if (!ret.result.is_null()) {
			websocketpp::lib::error_code errorCode;
			if (sessionEncoding == WebSocketEncoding::Json) {
				std::string helloMessageJson = ret.result.dump();
				_server.send(hdl, helloMessageJson, websocketpp::frame::opcode::text, errorCode);
			} else if (sessionEncoding == WebSocketEncoding::MsgPack) {
				auto msgPackData = json::to_msgpack(ret.result);
				std::string messageMsgPack(msgPackData.begin(), msgPackData.end());
				_server.send(hdl, messageMsgPack, websocketpp::frame::opcode::binary, errorCode);
			}
			session->IncrementOutgoingMessages();

			if (_debugEnabled)
				blog(LOG_INFO, "[WebSocketServer::onMessage] Outgoing message:\n%s", ret.result.dump(2).c_str());

			if (errorCode)
				blog(LOG_WARNING, "[WebSocketServer::onMessage] Sending message to client failed: %s", errorCode.message().c_str());
		}
	});
}
