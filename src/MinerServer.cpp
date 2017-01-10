﻿#include "MinerServer.hpp"
#include <memory>
#include <Poco/Net/HTTPServer.h>
#include "MinerUtil.hpp"
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include "RequestHandler.hpp"
#include <Poco/JSON/Object.h>
#include <Poco/File.h>
#include "MinerLogger.hpp"
#include <Poco/URI.h>
#include "Miner.hpp"
#include "MinerConfig.hpp"
#include <Poco/Path.h>

using namespace Poco;
using namespace Net;

Burst::MinerServer::MinerServer()
	: minerData_(nullptr), port_{0}
{
	auto ip = MinerConfig::getConfig().getServerUrl().getCanonical();
	auto port = std::to_string(MinerConfig::getConfig().getServerUrl().getPort());

	variables_.variables.emplace(std::make_pair("title", []() { return "Burst miner " + versionToString(); }));
	variables_.variables.emplace(std::make_pair("ip", [ip]() { return ip; }));
	variables_.variables.emplace(std::make_pair("port", [port]() { return port; }));
	variables_.variables.emplace(std::make_pair("nullDeadline", []() { return deadlineFormat(0); }));
}

Burst::MinerServer::~MinerServer()
{}

void Burst::MinerServer::run(uint16_t port)
{
	port_ = port;

	ServerSocket socket{port_};

	auto params = new HTTPServerParams;

	params->setMaxQueued(100);
	params->setMaxThreads(16);
	params->setServerName("Burst miner");
	params->setSoftwareVersion("Burst miner " + versionToString());

	if (server_ != nullptr)
		server_->stopAll(true);

	server_ = std::make_unique<HTTPServer>(this, socket, params);

	if (server_ != nullptr)
	{
		try
		{
			server_->start();
			variables_.variables["port"] = [port]() { return std::to_string(port); };
		}
		catch (std::exception& exc)
		{
			server_.release();
			MinerLogger::write(std::string("could not start local server: ") + exc.what(), TextType::Error);
		}
	}
}

void Burst::MinerServer::stop() const
{
	if (server_ != nullptr)
		server_->stopAll(true);
}

Poco::Net::HTTPRequestHandler* Burst::MinerServer::createRequestHandler(const Poco::Net::HTTPServerRequest& request)
{
	if (request.find("Upgrade") != request.end() &&
		icompare(request["Upgrade"], "websocket") == 0)
		return new WebSocketHandler{this};

	MinerLogger::write("request: " + request.getURI(), TextType::Debug);

	try
	{
		URI uri{request.getURI()};

		if (uri.getPath() == "/")
			return new RootHandler{variables_};
		
		Path path{"public"};
		path.append(uri.getPath());

		if (Poco::File{ path }.exists())
			return new AssetHandler{variables_};

		return new NotFoundHandler;
	}
	catch (...)
	{
		return new BadRequestHandler;
	}
}

void Burst::MinerServer::connectToMinerData(MinerData& minerData)
{
	minerData_ = &minerData;
	minerData.addObserverBlockDataChanged(*this, &MinerServer::blockDataChanged);
}

void Burst::MinerServer::addWebsocket(std::unique_ptr<Poco::Net::WebSocket> websocket)
{
	ScopedLock<FastMutex> lock{mutex_};
	auto blockData = minerData_->getBlockData();
	bool error;

	{
		std::stringstream ss;
		createJsonConfig().stringify(ss);
		error = !sendToWebsocket(*websocket, ss.str());
	}

	if (blockData != nullptr)
	{
		for (auto data = blockData->entries.begin(); data != blockData->entries.end() && !error; ++data)
		{
			std::stringstream ss;
			data->stringify(ss);

			if (!sendToWebsocket(*websocket, ss.str()))
				error = true;
		}
	}

	if (error)
		websocket.release();
	else
		websockets_.emplace_back(move(websocket));		
}

void Burst::MinerServer::sendToWebsockets(const std::string& data)
{
	ScopedLock<FastMutex> lock{mutex_};
	
	auto ws = websockets_.begin();

	while (ws != websockets_.end())
	{
		if (sendToWebsocket(**ws, data))
			++ws;
		else
			ws = websockets_.erase(ws);
	}
}

void Burst::MinerServer::sendToWebsockets(const JSON::Object& json)
{
	std::stringstream ss;
	json.stringify(ss);
	sendToWebsockets(ss.str());
}

void Burst::MinerServer::blockDataChanged(BlockDataChangedNotification* notification)
{
	sendToWebsockets(*notification->blockData);
	notification->release();
}

bool Burst::MinerServer::sendToWebsocket(WebSocket& websocket, const std::string& data) const
{
	try
	{
		auto n = websocket.sendFrame(data.data(), static_cast<int>(data.size()));
		if (n != data.size())
			MinerLogger::write("could not fully send: " + data, TextType::Error);
		return true;
	}
	catch (std::exception& exc)
	{
		MinerLogger::write(std::string("could not send the data to the websocket!: ") + exc.what(),
			TextType::Debug);
		return false;
	}
}
