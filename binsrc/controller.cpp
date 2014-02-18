/**
 * @file
 * @brief WalB controller tool.
 * @author HOSHINO Takashi
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include <cstdio>
#include <string>
#include "cybozu/socket.hpp"
#include "cybozu/option.hpp"
#include "net_util.hpp"
#include "controller.hpp"
#include "walb_util.hpp"

struct Option : public cybozu::Option
{
    std::string addr;
    uint16_t port;
    std::string cmd;
    std::vector<std::string> params;
    std::string ctrlId;
    bool isDebug;
    Option() {
        appendMust(&addr, "a", "host name or address");
        appendMust(&port, "p", "port number");
        appendParam(&cmd, "command", "command name");
        appendParamVec(&params, "parameters", "command parameters");
        appendBoolOpt(&isDebug, "debug", "put debug message.");

        std::string hostName = cybozu::net::getHostName();
        appendOpt(&ctrlId, hostName, "id", "controller identfier");

        appendHelp("h");
    }
};

namespace walb {

void runClient(Option &opt)
{
    cybozu::Socket sock;
    sock.connect(opt.addr, opt.port);
    std::atomic<bool> forceQuit(false);
    std::string serverId = protocol::run1stNegotiateAsClient(
        sock, opt.ctrlId, opt.cmd);
    ProtocolLogger logger(opt.ctrlId, serverId);

    const std::map<std::string, protocol::ClientHandler> h = {
        { "status", c2xGetStrVecClient },
        { "storage-init-vol", c2sInitVolClient },
        { "archive-init-vol", c2aInitVolClient },
        { "full-bkp", c2sFullSyncClient },
    };
    protocol::clientDispatch(opt.cmd, sock, logger, forceQuit, opt.params, h);
}

} // namespace walb

int main(int argc, char *argv[])
try {
    Option opt;
    if (!opt.parse(argc, argv)) {
        opt.usage();
        return 1;
    }
    walb::util::setLogSetting("-", opt.isDebug);
    walb::runClient(opt);

} catch (std::exception &e) {
    LOGe("Controller: error: %s", e.what());
    return 1;
} catch (...) {
    LOGe("Controller: caught other error.");
    return 1;
}

/* end of file */
