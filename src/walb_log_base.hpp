#pragma once
/**
 * @file
 * @brief Walb log utilities.
 * @author HOSHINO Takashi
 *
 * (C) 2012 Cybozu Labs, Inc.
 */
#include <cassert>
#include <memory>
#include <cstdlib>
#include <functional>
#include <type_traits>

#include "util.hpp"
#include "checksum.hpp"
#include "fileio.hpp"
#include "memory_buffer.hpp"
#include "walb_logger.hpp"
#include "walb/super.h"
#include "walb/log_device.h"
#include "walb/log_record.h"
#include "walb_log.h"
#include "backtrace.hpp"

namespace walb {

struct LogRecord : public walb_log_record
{
    uint64_t packLsid() const { return lsid - lsid_local; }
    bool isExist() const {
        return ::test_bit_u32(LOG_RECORD_EXIST, &flags);
    }
    bool isPadding() const {
        return ::test_bit_u32(LOG_RECORD_PADDING, &flags);
    }
    bool isDiscard() const {
        return ::test_bit_u32(LOG_RECORD_DISCARD, &flags);
    }
    bool hasData() const {
        return isExist() && !isDiscard();
    }
    bool hasDataForChecksum() const {
        return isExist() && !isDiscard() && !isPadding();
    }
    void setExist() {
        ::set_bit_u32(LOG_RECORD_EXIST, &flags);
    }
    void setPadding() {
        ::set_bit_u32(LOG_RECORD_PADDING, &flags);
    }
    void setDiscard() {
        ::set_bit_u32(LOG_RECORD_DISCARD, &flags);
    }
    void clearExist() {
        ::clear_bit_u32(LOG_RECORD_EXIST, &flags);
    }
    void clearPadding() {
        ::clear_bit_u32(LOG_RECORD_PADDING, &flags);
    }
    void clearDiscard() {
        ::clear_bit_u32(LOG_RECORD_DISCARD, &flags);
    }
    bool isValid() const { return ::is_valid_log_record_const(this); }
    void verify() const {
        if (!isValid()) throw cybozu::Exception("LogRecord:verify");
    }
    uint32_t ioSizePb(uint32_t pbs) const {
        return ::capacity_pb(pbs, io_size);
    }
    uint32_t ioSizeLb() const { return io_size; }
};

using LogBlock = std::shared_ptr<uint8_t>;
inline LogBlock createLogBlock(uint32_t pbs)
{
    return cybozu::util::allocateBlocks<uint8_t>(LOGICAL_BLOCK_SIZE, pbs);
}

namespace log {

class InvalidIo : public std::exception
{
public:
    const char *what() const noexcept override { return "invalid logpack IO."; }
};

inline void printRecord(::FILE *fp, size_t idx, const LogRecord &rec)
{
    ::fprintf(fp,
              "record %zu\n"
              "  checksum: %08x(%u)\n"
              "  lsid: %" PRIu64 "\n"
              "  lsid_local: %u\n"
              "  is_exist: %u\n"
              "  is_padding: %u\n"
              "  is_discard: %u\n"
              "  offset: %" PRIu64 "\n"
              "  io_size: %u\n"
              , idx
              , rec.checksum, rec.checksum
              , rec.lsid, rec.lsid_local
              , rec.isExist()
              , rec.isPadding()
              , rec.isDiscard()
              , rec.offset, rec.io_size);
}

inline void printRecordOneline(::FILE *fp, size_t idx, const LogRecord &rec)
{
    ::fprintf(fp,
              "wlog_rec %2zu:\t"
              "lsid %" PRIu64 " %u\tio %10" PRIu64 " %4u\t"
              "flags %u%u%u\tcsum %08x %u\n"
              , idx
              , rec.lsid, rec.lsid_local
              , rec.offset, rec.io_size
              , rec.isExist()
              , rec.isPadding()
              , rec.isDiscard()
              , rec.checksum, rec.checksum);
}

} // log

/**
 * log pack header
 */
class LogPackHeader
{
    LogBlock block_;
    walb_logpack_header *header_;
    uint32_t pbs_;
    uint32_t salt_;
public:
    static constexpr const char *NAME() { return "LogPackHeader"; }
    LogPackHeader(const void *header = 0, uint32_t pbs = 0, uint32_t salt = 0)
        : header_((walb_logpack_header*)header), pbs_(pbs), salt_(salt) {
    }
    LogPackHeader(const LogBlock &block, uint32_t pbs, uint32_t salt)
        : header_(nullptr), pbs_(pbs), salt_(salt) {
        setBlock(block);
    }
    LogPackHeader(uint32_t pbs, uint32_t salt)
        : header_(nullptr), pbs_(pbs), salt_(salt) {
        setBlock(createLogBlock(pbs));
    }
    const walb_logpack_header &header() const { checkBlock(); return *header_; }
    walb_logpack_header &header() { checkBlock(); return *header_; }
    uint32_t pbs() const { return pbs_; }
    uint32_t salt() const { return salt_; }
    void setPbs(uint32_t pbs) { pbs_ = pbs; }
    void setSalt(uint32_t salt) { salt_ = salt; }
    void setBlock(const LogBlock &block) {
        block_ = block;
        header_ = (walb_logpack_header*)block_.get();
    }
    void init(uint32_t pbs, uint32_t salt, LogBlock block = nullptr) {
        setPbs(pbs);
        setSalt(salt);
        if (!block) block = createLogBlock(pbs);
        setBlock(block);
    }

    /*
     * Fields.
     */
    uint32_t checksum() const { return header_->checksum; }
    uint16_t sectorType() const { return header_->sector_type; }
    uint16_t totalIoSize() const { return header_->total_io_size; }
    uint64_t logpackLsid() const { return header_->logpack_lsid; }
    uint16_t nRecords() const { return header_->n_records; }
    uint16_t nPadding() const { return header_->n_padding; }

    /*
     * Utilities.
     */
    const uint8_t *rawData() const { return (const uint8_t*)header_; }
    const LogRecord &record(size_t pos) const {
        checkIndexRange(pos);
        return static_cast<const LogRecord &>(header_->record[pos]);
    }
    struct LogRecord &record(size_t pos) {
        checkIndexRange(pos);
        return static_cast<LogRecord &>(header_->record[pos]);
    }
    uint64_t nextLogpackLsid() const {
        if (nRecords() > 0) {
            return logpackLsid() + 1 + totalIoSize();
        } else {
            return logpackLsid();
        }
    }
    uint64_t totalPaddingPb() const {
        if (nPadding() == 0) {
            return 0;
        }
        uint64_t t = 0;
        for (size_t i = 0; i < nRecords(); i++) {
            const LogRecord &rec = record(i);
            if (rec.isPadding()) {
                t += ::capacity_pb(pbs(), rec.io_size);
            }
        }
        return t;
    }
    bool isEnd() const {
        return nRecords() == 0 && logpackLsid() == uint64_t(-1);
    }
    bool isValid(bool isChecksum = true) const {
        if (isChecksum) {
            return ::is_valid_logpack_header_and_records_with_checksum(header_, pbs(), salt()) != 0;
        } else {
            return ::is_valid_logpack_header_and_records(header_) != 0;
        }
    }
    void verify() const {
        if (!isValid(true)) throw cybozu::Exception(NAME()) << "invalid";
    }
    void copyFrom(const void *data, size_t size)
    {
        if (pbs_ != size) {
            throw cybozu::Exception(NAME()) << "invalid size" << size << pbs_;
        }
        ::memcpy(header_, data, size);
        verify();
    }

    /*
     * Print.
     */
    void printRecord(size_t pos, ::FILE *fp = ::stdout) const {
        log::printRecord(fp, pos, record(pos));
    }
    void printRecordOneline(size_t pos, ::FILE *fp = ::stdout) const {
        log::printRecordOneline(fp, pos, record(pos));
    }
    void printHeader(::FILE *fp = ::stdout) const {
        ::fprintf(fp,
                  "*****logpack header*****\n"
                  "checksum: %08x(%u)\n"
                  "n_records: %u\n"
                  "n_padding: %u\n"
                  "total_io_size: %u\n"
                  "logpack_lsid: %" PRIu64 "\n",
                  header_->checksum, header_->checksum,
                  header_->n_records,
                  header_->n_padding,
                  header_->total_io_size,
                  header_->logpack_lsid);
    }
    void print(::FILE *fp = ::stdout) const {
        printHeader(fp);
        for (size_t i = 0; i < nRecords(); i++) {
            printRecord(i, fp);
        }
    }
    /**
     * Print each IO oneline.
     * logpack_lsid, mode(W, D, or P), offset[lb], io_size[lb].
     */
    void printShort(::FILE *fp = ::stdout) const {
        for (size_t i = 0; i < nRecords(); i++) {
            const struct walb_log_record &rec = record(i);
            assert(::test_bit_u32(LOG_RECORD_EXIST, &rec.flags));
            char mode = 'W';
            if (::test_bit_u32(LOG_RECORD_DISCARD, &rec.flags)) { mode = 'D'; }
            if (::test_bit_u32(LOG_RECORD_PADDING, &rec.flags)) { mode = 'P'; }
            ::fprintf(fp,
                      "%" PRIu64 "\t%c\t%" PRIu64 "\t%u\n",
                      header_->logpack_lsid,
                      mode, rec.offset, rec.io_size);
        }
    }
    /**
     * Update checksum field.
     */
    void updateChecksum() {
        header_->checksum = 0;
        header_->checksum = ::checksum((const uint8_t*)header_, pbs(), salt());
    }

    template <typename Reader>
    DEPRECATED bool read(Reader &reader) {
        return readFrom(reader);
    }
    template <typename Writer>
    DEPRECATED void write(Writer &writer) {
        writeTo(writer);
    }
    /**
     * RETURN:
     *   false if read failed or invalid.
     */
    template <typename Reader>
    bool readFrom(Reader &reader) {
        try {
            reader.read(header_, pbs_);
        } catch (std::exception &e) {
            LOGs.debug() << NAME() << e.what();
            return false;
        }
        return isValid(true);
    }
    /**
     * Write the logpack header block.
     */
    template <typename Writer>
    void writeTo(Writer &writer) {
        updateChecksum();
        if (!isValid(true)) {
            throw cybozu::Exception(NAME()) << "write:invalid";
        }
        writer.write(header_, pbs());
    }
    /**
     * Initialize logpack header block.
     */
    void init(uint64_t lsid) {
        ::memset(header_, 0, pbs());
        header_->logpack_lsid = lsid;
        header_->sector_type = SECTOR_TYPE_LOGPACK;
        /*
          header_->total_io_size = 0;
          header_->n_records = 0;
          header_->n_padding = 0;
          header_->checksum = 0;
        */
    }
    /**
     * Make the header block terminator.
     * Do not forget to call updateChecksum() before write it.
     */
    void setEnd() {
        init(uint64_t(-1));
    }
    /**
     * Add a normal IO.
     *
     * @offset [logical block]
     * @size [logical block]
     *   can not be 0.
     * RETURN:
     *   true in success, or false (you must create a new header).
     */
    bool addNormalIo(uint64_t offset, uint16_t size) {
        if (::max_n_log_record_in_sector(pbs()) <= nRecords()) {
            return false;
        }
        if (MAX_TOTAL_IO_SIZE_IN_LOGPACK_HEADER <
            totalIoSize() + ::capacity_pb(pbs(), size)) {
            return false;
        }
        if (size == 0) {
            throw cybozu::Exception("Normal IO can not be zero-sized.");
        }
        size_t pos = nRecords();
        struct walb_log_record &rec = header_->record[pos];
        rec.flags = 0;
        ::set_bit_u32(LOG_RECORD_EXIST, &rec.flags);
        rec.offset = offset;
        rec.io_size = size;
        rec.lsid_local = header_->total_io_size + 1;
        rec.lsid = header_->logpack_lsid + rec.lsid_local;
        rec.checksum = 0; /* You must set this lator. */

        header_->n_records++;
        header_->total_io_size += capacity_pb(pbs(), size);
        assert(::is_valid_logpack_header_and_records(header_));
        return true;
    }
    /**
     * Add a discard IO.
     *
     * @offset [logical block]
     * @size [logical block]
     *   can not be 0.
     * RETURN:
     *   true in success, or false (you must create a new header).
     */
    bool addDiscardIo(uint64_t offset, uint16_t size) {
        if (::max_n_log_record_in_sector(pbs()) <= nRecords()) {
            return false;
        }
        if (size == 0) {
            throw cybozu::Exception("Discard IO can not be zero-sized.");
        }
        size_t pos = nRecords();
        struct walb_log_record &rec = header_->record[pos];
        rec.flags = 0;
        ::set_bit_u32(LOG_RECORD_EXIST, &rec.flags);
        ::set_bit_u32(LOG_RECORD_DISCARD, &rec.flags);
        rec.offset = offset;
        rec.io_size = size;
        rec.lsid_local = header_->total_io_size + 1;
        rec.lsid = header_->logpack_lsid + rec.lsid_local;
        rec.checksum = 0; /* Not be used. */

        header_->n_records++;
        /* You must not update total_io_size. */
        assert(::is_valid_logpack_header_and_records(header_));
        return true;
    }
    /**
     * Add a padding.
     *
     * @size [logical block]
     *   can be 0.
     * RETURN:
     *   true in success, or false (you must create a new header).
     */
    bool addPadding(uint16_t size) {
        if (::max_n_log_record_in_sector(pbs()) <= nRecords()) {
            return false;
        }
        if (MAX_TOTAL_IO_SIZE_IN_LOGPACK_HEADER <
            totalIoSize() + ::capacity_pb(pbs(), size)) {
            return false;
        }
        if (0 < nPadding()) {
            return false;
        }
        if (size % ::n_lb_in_pb(pbs()) != 0) {
            throw cybozu::Exception("Padding size must be pbs-aligned.");
        }

        size_t pos = nRecords();
        struct walb_log_record &rec = header_->record[pos];
        rec.flags = 0;
        ::set_bit_u32(LOG_RECORD_EXIST, &rec.flags);
        ::set_bit_u32(LOG_RECORD_PADDING, &rec.flags);
        rec.offset = 0; /* will not be used. */
        rec.io_size = size;
        rec.lsid_local = header_->total_io_size + 1;
        rec.lsid = header_->logpack_lsid + rec.lsid_local;
        rec.checksum = 0;  /* will not be used. */

        header_->n_records++;
        header_->total_io_size += ::capacity_pb(pbs(), size);
        header_->n_padding++;
        assert(::is_valid_logpack_header_and_records(header_));
        return true;
    }
    /**
     * Update all lsid entries in the logpack header.
     *
     * @newLsid new logpack lsid.
     *   If -1, nothing will be changed.
     *
     * RETURN:
     *   true in success.
     *   false if lsid overlap ocurred.
     */
    bool updateLsid(uint64_t newLsid) {
        assert(isValid(false));
        if (newLsid == uint64_t(-1)) {
            return true;
        }
        if (header_->logpack_lsid == newLsid) {
            return true;
        }

        header_->logpack_lsid = newLsid;
        for (size_t i = 0; i < header_->n_records; i++) {
            struct walb_log_record &rec = record(i);
            rec.lsid = newLsid + rec.lsid_local;
        }
        return isValid(false);
    }
    /**
     * Shrink.
     * Delete records from rec[invalidIdx] to the last.
     */
    void shrink(size_t invalidIdx) {
        assert(invalidIdx < nRecords());

        /* Invalidate records. */
        for (size_t i = invalidIdx; i < nRecords(); i++) {
            ::log_record_init(&record(i));
        }

        /* Set n_records and total_io_size. */
        header_->n_records = invalidIdx;
        header_->total_io_size = 0;
        header_->n_padding = 0;
        for (size_t i = 0; i < nRecords(); i++) {
            struct walb_log_record &rec = record(i);
            if (!::test_bit_u32(LOG_RECORD_DISCARD, &rec.flags)) {
                header_->total_io_size += ::capacity_pb(pbs(), rec.io_size);
            }
            if (::test_bit_u32(LOG_RECORD_PADDING, &rec.flags)) {
                header_->n_padding++;
            }
        }

        updateChecksum();
        assert(isValid());
    }
protected:
    void checkIndexRange(size_t pos) const {
        if (pos >= nRecords()) {
            throw cybozu::Exception("LogPackHeader:index out of range.") << pos;
        }
    }
    void checkBlock() const {
        if (header_ == nullptr) {
            throw cybozu::Exception("LogPackHeader:header is null.");
        }
    }
};

/**
 * Helper class to manage multiple IO blocks.
 * This is copyable and movable.
 */
class LogBlockShared
{
private:
    static constexpr const char *NAME() { return "LogBlockShared"; }
    uint32_t pbs_; /* physical block size. */
    std::vector<LogBlock> data_; /* Each block's size must be pbs. */
public:
    explicit LogBlockShared(uint32_t pbs = 0) : pbs_(pbs) {}
    void init(size_t pbs) {
        pbs_ = pbs;
        checkPbs();
        resize(0);
    }
    size_t nBlocks() const { return data_.size(); }
    const uint8_t *get(size_t idx) const {
        checkRange(idx);
        return data_[idx].get();
    }
    uint8_t *get(size_t idx) {
        checkRange(idx);
        return data_[idx].get();
    }
    uint32_t pbs() const {
        checkPbs();
        return pbs_;
    }
    void resize(size_t nBlocks0) {
        if (nBlocks0 < data_.size()) {
            data_.resize(nBlocks0);
        }
        while (data_.size() < nBlocks0) {
            data_.push_back(allocPb());
        }
    }
    void addBlock(const LogBlock &block) {
        assert(block);
        data_.push_back(block);
    }
    LogBlock getBlock(size_t idx) const { return data_[idx]; }
    template <typename Reader>
    void read(Reader &reader, uint32_t nBlocks0) {
        checkPbs();
        resize(nBlocks0);
        for (size_t i = 0; i < nBlocks0; i++) {
            reader.read(get(i), pbs_);
        }
    }
    template <typename Writer>
    void write(Writer &writer) const {
        checkPbs();
        for (size_t i = 0; i < nBlocks(); i++) {
            writer.write(get(i), pbs_);
        }
    }
    bool calcIsAllZero(size_t ioSizeLb) const {
        checkPbs();
        checkSizeLb(ioSizeLb);
        size_t remaining = ioSizeLb * LOGICAL_BLOCK_SIZE;
        size_t i = 0;
        while (0 < remaining) {
            const size_t s = std::min<size_t>(pbs_, remaining);
            if (!cybozu::util::calcIsAllZero(get(i), s)) return false;
            remaining -= s;
            i++;
        }
        return true;
    }
    uint32_t calcChecksum(size_t ioSizeLb, uint32_t salt) const {
        checkPbs();
        checkSizeLb(ioSizeLb);
        uint32_t csum = salt;
        size_t remaining = ioSizeLb * LOGICAL_BLOCK_SIZE;
        size_t i = 0;
        while (0 < remaining) {
            assert(i < nBlocks());
            const size_t s = std::min<size_t>(pbs_, remaining);
            csum = cybozu::util::checksumPartial(get(i), s, csum);
            remaining -= s;
            i++;
        }
        return cybozu::util::checksumFinish(csum);
    }
private:
    LogBlock allocPb() const {
        checkPbs();
        return createLogBlock(pbs_);
    }
    void checkPbs() const {
        if (pbs_ == 0) throw cybozu::Exception(NAME()) << "pbs is zero";
    }
    void checkSizeLb(size_t ioSizeLb) const {
        if (nBlocks() * pbs_ < ioSizeLb * LOGICAL_BLOCK_SIZE) {
            throw cybozu::Exception(NAME())
                << "checkSizeLb:ioSizeLb too large"
                << ioSizeLb << ::capacity_lb(pbs_, nBlocks());
        }
    }
    void checkRange(size_t idx) const {
        checkPbs();
        if (nBlocks() <= idx) {
            throw cybozu::Exception(NAME()) << "index out of range" << idx;
        }
    }
};

namespace log {

inline void verifyLogChecksum(const LogRecord& rec, const LogBlockShared& blockS, uint32_t salt)
{
    if (!rec.hasDataForChecksum()) return;
    const uint32_t checksum = blockS.calcChecksum(rec.io_size, salt);
    if (checksum != rec.checksum) {
        throw cybozu::Exception(__func__)
            << "invalid checksum" << checksum << rec.checksum;
    }
}

inline bool isLogChecksumValid(const LogRecord& rec, const LogBlockShared& blockS, uint32_t salt)
{
    try {
        verifyLogChecksum(rec, blockS, salt);
        return true;
    } catch (std::exception &) {
        return false;
    }
}

} // walb::log

/**
 * Logpack record and IO data.
 */
struct LogPackIo
{
private:
    uint32_t salt;
public:
    LogRecord rec;
    LogBlockShared blockS;

    void set(const LogPackHeader &logh, size_t pos) {
        rec = logh.record(pos);
        salt = logh.salt();
        blockS.init(logh.pbs());
    }

    bool isValid(bool isChecksum = true) const {
        if (!rec.isValid()) return false;
        if (!isChecksum) return true;
        return log::isLogChecksumValid(rec, blockS, salt);
    }
    uint32_t calcIoChecksumWithZeroSalt() const {
        return blockS.calcChecksum(rec.ioSizeLb(), 0);
    }
    uint32_t calcIoChecksum() const {
        return blockS.calcChecksum(rec.ioSizeLb(), salt);
    }
};

} // walb
