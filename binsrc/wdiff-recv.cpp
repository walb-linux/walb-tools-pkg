/**
 * @file
 * @brief To send wdiff to a server.
 * @author MITSUNARI Shigeo
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include "cybozu/option.hpp"
#include "cybozu/socket.hpp"
#include "walb_log_file.hpp"
#include "walb_log_net.hpp"
#include "walb_logger.hpp"
#include "server_util.hpp"
#include "file_path.hpp"
#include "thread_util.hpp"
#include "net_util.hpp"
#include "protocol.hpp"

namespace walb {

class RequestWorker : public cybozu::thread::Runnable
{
private:
    cybozu::Socket sock_;
    std::string serverId_;
    cybozu::FilePath baseDir_;
    const std::atomic<bool> &forceQuit_;
    std::atomic<cybozu::server::ControlFlag> &ctrlFlag_;
public:
    RequestWorker(cybozu::Socket &&sock, const std::string &serverId,
                  const cybozu::FilePath &baseDir,
                  const std::atomic<bool> &forceQuit,
                  std::atomic<cybozu::server::ControlFlag> &ctrlFlag)
        : sock_(std::move(sock))
        , serverId_(serverId)
        , baseDir_(baseDir)
        , forceQuit_(forceQuit)
        , ctrlFlag_(ctrlFlag) {}
    void operator()() noexcept override try {
        run();
        sock_.close();
        done();
    } catch (...) {
        throwErrorLater();
        sock_.close();
    }
    void run() {
        std::string clientId;
        protocol::Protocol *protocol;
        if (protocol::run1stNegotiateAsServer(sock_, serverId_, clientId, &protocol, ctrlFlag_)) {
            return;
        }
        const auto pName = protocol::ProtocolName::WDIFF_SEND;
        const std::string pStr = protocol::PROTOCOL_TYPE_MAP.at(pName);
        assert(protocol == protocol::ProtocolFactory::getInstance().findServer(pStr));

        /* Original behavior for wdiff-recv command. */
        ProtocolLogger logger(serverId_, clientId);

        packet::Packet packet(sock_);

        std::string name; // not used
        cybozu::Uuid uuid;
        walb::MetaDiff diff;
        uint16_t maxIoBlocks;
        packet.read(name);
        packet.read(uuid);
        packet.read(maxIoBlocks);
        packet.read(diff);

        logger.debug("name %s", name.c_str());
        logger.debug("uuid %s", uuid.str().c_str());
        logger.debug("maxIoBlocks %u", maxIoBlocks);
        logger.debug("diff %s", diff.str().c_str());

        packet::Answer ans(sock_);
        if (!checkParams(logger, name, diff)) {
            ans.ng(1, "error for test.");
            return;
        }
        ans.ok();
        logger.debug("send ans ok.");

        const std::string fName = createDiffFileName(diff);
        cybozu::TmpFile tmpFile(baseDir_.str());
        cybozu::FilePath fPath = baseDir_ + fName;
        diff::Writer writer(tmpFile.fd());
        diff::FileHeaderRaw fileH;
        fileH.setMaxIoBlocksIfNecessary(maxIoBlocks);
        fileH.setUuid(uuid.rawData());
        writer.writeHeader(fileH);
fileH.print();
        logger.debug("write header.");
        recvAndWriteDiffs(sock_, writer, logger);
        logger.debug("close.");
        writer.close();
        tmpFile.save(fPath.str());

        packet::Ack ack(sock_);
        ack.send();
    }
private:
    bool checkParams(Logger &logger,
        const std::string &name,
        const walb::MetaDiff &diff) const {

        if (name.empty()) {
            logger.error("name is empty.");
            return false;
        }
        if (!diff.isValid()) {
            logger.error("invalid diff.");
            return false;
        }
        return true;
    }
    void recvAndWriteDiffs(cybozu::Socket &sock, diff::Writer &writer, Logger &logger) {
        walb::packet::StreamControl ctrl(sock);
		walb::diff::RecordRaw recRaw;
        while (ctrl.isNext()) {
			sock.read(recRaw.rawData(), recRaw.rawSize());
            if (!recRaw.isValid()) {
                logAndThrow(logger, "recvAndWriteDiffs:bad recRaw");
            }
            walb::diff::IoData io;
            io.set(recRaw.record());
            if (recRaw.dataSize() == 0) {
                writer.writeDiff(recRaw.record(), {});
            } else {
                sock.read(io.rawData(), recRaw.dataSize());
                if (!io.isValid()) {
                    logAndThrow(logger, "recvAndWriteDiffs:bad io");
                }
                if (io.calcChecksum() != recRaw.checksum()) {
                    logAndThrow(logger, "recvAndWriteDiffs:bad io checksum");
                }
//                writer.writeDiff(recRaw.record(), io.forMove());
                writer.compressAndWriteDiff(recRaw.record(), io.rawData());
            }
            ctrl.reset();
        }
        if (!ctrl.isEnd()) {
            throw cybozu::Exception("recvAndWriteDiffs:bad ctrl not end");
        }
    }
private:
    void logAndThrow(Logger& logger, const std::string& msg)
    {
        logger.error(msg);
        throw cybozu::Exception(msg);
    }
};

} // namespace walb

struct Option : cybozu::Option
{
    uint16_t port;
    std::string serverId;
    std::string baseDirStr;

    Option() {
        appendMust(&port, "p", "port to listen");
        std::string hostName = cybozu::net::getHostName();
        appendOpt(&serverId, hostName, "id", "host identifier");
        cybozu::FilePath curDir = cybozu::getCurrentDir();
        appendOpt(&baseDirStr, curDir.str(), "b", "base directory.");
        appendHelp("h");
    }
};

namespace walb {

namespace protocol {

void registerProtocolsForWdiffRecvCommand()
{
    ProtocolFactory &factory = ProtocolFactory::getInstance();
    factory.registerServer<Protocol>(ProtocolName::WDIFF_SEND);
}

}} // namespace walb::protocol

int main(int argc, char *argv[]) try
{
    cybozu::SetLogFILE(::stderr);
	cybozu::SetLogPriority(cybozu::LogDebug);

    Option opt;
    if (!opt.parse(argc, argv)) {
        opt.usage();
        throw RT_ERR("option error.");
    }
    cybozu::FilePath baseDir(opt.baseDirStr);
    if (!baseDir.stat().isDirectory()) {
        throw RT_ERR("%s is not directory.", baseDir.cStr());
    }
    walb::protocol::registerProtocolsForWdiffRecvCommand();

    auto createReqWorker = [&](
        cybozu::Socket &&sock, const std::atomic<bool> &forceQuit, std::atomic<cybozu::server::ControlFlag> &flag) {
        return std::make_shared<walb::RequestWorker>(
            std::move(sock), opt.serverId, baseDir, forceQuit, flag);
    };
    cybozu::server::MultiThreadedServer server(1);
    server.run(opt.port, createReqWorker);
    return 0;
} catch (std::exception &e) {
    LOGe("caught error: %s", e.what());
    return 1;
} catch (...) {
    LOGe("caught other error.");
    return 1;
}
