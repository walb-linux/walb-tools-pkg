/**
 * @file
 * @brief To send wlog to a proxy.
 * @author HOSHINO Takashi
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include <stdexcept>
#include <cstdio>
#include <time.h>
#include "cybozu/option.hpp"
#include "cybozu/socket.hpp"
#include "cybozu/atoi.hpp"
#include "cybozu/log.hpp"
#include "protocol.hpp"
#include "meta.hpp"
#include "file_path.hpp"
#include "time.hpp"
#include "net_util.hpp"
#include "meta.hpp"
#include "walb_log_file.hpp"
#include "walb_log_net.hpp"

/**
 * parse "HOST:PORT" format.
 */
std::pair<std::string, uint16_t> parseHostPortStr(const std::string &s)
{
    auto throwError = [&]() {
        std::string msg = cybozu::util::formatString(
            "parseHostColonPortStr: invalid format '%s'", s.c_str());
        throw std::runtime_error(msg);
    };
    size_t n = s.find(":");
    if (n == std::string::npos) throwError();
    std::string hostStr = s.substr(0, n);
    if (hostStr.empty()) throwError();
    std::string portStr = s.substr(n + 1);
    if (portStr.empty()) throwError();
    uint16_t port = cybozu::atoi(portStr);
    return std::make_pair(hostStr, port);
}

struct Option : cybozu::Option
{
    std::string proxyHostPort;
    std::string name;
    uint64_t gid;
    std::vector<std::string> wlogPathV;
    std::string clientId;
    bool canNotMerge;
    std::string timeStampStr;

    Option() {
        appendMust(&proxyHostPort, "proxy", "proxy host:port");
        appendMust(&name, "name", "volume identifier");
        appendOpt(&gid, 0, "begin gid.");
        appendParamVec(&wlogPathV, "wlog_path_list", "wlog path list");
        std::string hostName = cybozu::net::getHostName();
        appendOpt(&clientId, hostName, "id", "client identifier");
        appendBoolOpt(&canNotMerge, "m", "clear canMerge flag.");
        appendOpt(&timeStampStr, "", "t", "timestamp in YYYYmmddHHMMSS format.");
        appendHelp("h");
    }
};

void sendWlog(cybozu::Socket &sock, const std::string &clientId,
              const std::string &name, int wlogFd, walb::MetaDiff &diff)
{
    std::string diffFileName = createDiffFileName(diff);
    LOGi("try to send %s...", diffFileName.c_str());

    walb::log::Reader reader(wlogFd);

    std::string serverId = walb::run1stNegotiateAsClient(
        sock, clientId, "wlog-send");
    walb::ProtocolLogger logger(clientId, serverId);
    std::atomic<bool> forceQuit(false);


    walb::log::FileHeader fileH;
    reader.readHeader(fileH);
    uint64_t sizePb = -1; // unknown.

    /* wlog-send negotiation */
    walb::LogSendProtocol::Client client(sock, logger, forceQuit, {});
    /*
     * TODO:
     *   This uuid should not be wlog header.
     *   This is used to detect hash-sync/full-sync occurrence
     *   in order to delete pending wlog files in proxies.
     */
    client.setParams(name, fileH.uuid(), diff, fileH.pbs(), fileH.salt(), sizePb);
    client.prepare();

    /* Send log packs. */
    try {
        while (!reader.isEnd()) {
            assert(reader.isFirstInPack());
            walb::log::PackHeader &h = reader.packHeader();
            client.pushHeader(h);
            for (size_t i = 0; i < h.nRecords(); i++) {
                walb::log::RecordRaw rec;
                walb::log::BlockDataVec blockD;
                reader.readLog(rec, blockD);
                client.pushIo(h, i, blockD);
            }
        }
        client.sync();
    } catch (...) {
        client.fail();
        throw;
    }

    /* The wlog-send protocol has finished.
       You can close the socket. */
};

int main(int argc, char *argv[])
try {
    cybozu::SetLogFILE(::stderr);

    Option opt;
    if (!opt.parse(argc, argv)) {
        opt.usage();
        throw std::runtime_error("option error.");
    }
    std::string host;
    uint16_t port;
    std::tie(host, port) = parseHostPortStr(opt.proxyHostPort);

    uint64_t ts = ::time(0);
    if (!opt.timeStampStr.empty()) {
        ts = cybozu::strToUnixTime(opt.timeStampStr);
    }

    uint64_t gid = opt.gid;
    for (const std::string &wlogPath : opt.wlogPathV) {
        cybozu::util::FileOpener fo(wlogPath, O_RDONLY);
        walb::MetaDiff diff;
        diff.init();
        diff.setSnap0(gid);
        diff.setSnap1(gid + 1);
        diff.setTimestamp(ts);
        diff.setCanMerge(!opt.canNotMerge);
        cybozu::Socket sock;
        sock.connect(host, port);
        sendWlog(sock, opt.clientId, opt.name, fo.fd(), diff);
        gid++;
    }
    return 0;
} catch (std::exception &e) {
    ::fprintf(::stderr, "exception: %s\n", e.what());
    return 1;
} catch (...) {
    ::fprintf(::stderr, "caught an other error.\n");
    return 1;
}
