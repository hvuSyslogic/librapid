//---------------------------------------------------------------------------------------------------------------------
// Copyright (c) 2015-2016 librapid project. All rights reserved.
// More license information, please see LICENSE file in module root folder.
//---------------------------------------------------------------------------------------------------------------------

#include <iostream>

#include <rapid/logging/logging.h>
#include <rapid/logging/utilis.h>

#include <rapid/platform/applicationsingleton.h>
#include <rapid/utils/singleton.h>
#include <rapid/utils/scopeguard.h>

#include "httpserverconfigfacade.h"
#include "fakehttpserver.h"
#include "httpserver.h"

//#define ENABLE_FAKE_HTTP_SERVER

#ifdef ENABLE_FAKE_HTTP_SERVER
std::weak_ptr<FakeHttpServer> httpServer;
#else
std::weak_ptr<HttpServer> httpServer;
#endif

BOOL WINAPI consoleHandler(DWORD consoleEvent) {
    switch (consoleEvent) {
    case CTRL_LOGOFF_EVENT:
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
		if (auto server = httpServer.lock()) {
			server->stop();
		}
        return TRUE;
        break;
    }
    return FALSE;
}

void startLogging() {
#ifdef _DEBUG
	rapid::logging::startLogging(rapid::logging::Info);
#else
	rapid::logging::startLogging(rapid::logging::Info);
#endif

	auto pFileLogAppender = std::make_shared<rapid::logging::FileLogAppender>();
	pFileLogAppender->setLogDirectory(L"./log/");
	rapid::logging::addLogAppender(pFileLogAppender);
	
	auto pConsoleOuputAppender = std::make_shared<rapid::logging::ConsoleOutputLogAppender>();
	pConsoleOuputAppender->setConsoleFont(L"Lucida Console", 12);
	pConsoleOuputAppender->setWindowSize(80, 50);
	rapid::logging::addLogAppender(pConsoleOuputAppender);
}

void startHttpServer() {
	startLogging();

	HttpServerConfigFacade::getInstance().loadXmlConfigFile("librapid.config.xml");

#ifdef ENABLE_FAKE_HTTP_SERVER
	// IPV4 binding
	auto pServerTemp = std::shared_ptr<FakeHttpServer>(FakeHttpServer::createHttpServer(
		"0.0.0.0",
		HttpServerConfigFacade::getInstance().getListenPort()
	));
#else
#if 1
	// IPV4 binding
	auto pServerTemp = std::shared_ptr<HttpServer>(HttpServer::createHttpServer(
		"0.0.0.0",
		HttpServerConfigFacade::getInstance().getListenPort()
		));
#else
	// IPV6 binding
	auto pServerTemp = std::shared_ptr<HttpServer>(HttpServer::createHttpServer("::1", 9999));
#endif
#endif

	httpServer = pServerTemp;
	pServerTemp->start();
}

static char const s_serverTitle[] = {
	"\t _____ _____ _____ _____    _____                      \r\n"
	"\t|  |  |_   _|_   _|  _  |  |   __|___ ___ _ _ ___ ___  \r\n"
	"\t|     | | |   | | |   __|  |__   | -_|  _| | | -_|  _| \r\n"
	"\t|__|__| |_|   |_| |__|     |_____|___|_|  \\_/|___|_|  \r\n"
	"\t                                  Powered by librapid. \r\n"
	"\r\n"
};

int main(int argc, char *argv[]) {
	std::cout << s_serverTitle;

	SCOPE_EXIT() {
		rapid::logging::stopLogging();
	};

	// Setup console handler, we want to 'Ctrl + C' stopping server.
	::SetConsoleCtrlHandler(consoleHandler, TRUE);

	/*
	// Test application is singleetion.
	rapid::platform::ApplicationSingleton app;
	if (app.hasInstanceRunning()) {
		RAPID_LOG_FATAL() << "Server already running!" << std::endl;
		std::cin.get();
		return EXIT_FAILURE;
	}
	*/

	try {
		startHttpServer();
	} catch (std::exception const &e) {
		RAPID_LOG_FATAL() << e.what();
		std::cin.get();
	}
}