#pragma once
/**
 * @file
 * @brief walb diff merger.
 * @author HOSHINO Takashi
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include <memory>
#include <string>
#include <vector>
#include <queue>
#include <list>
#include <cassert>
#include <cstring>

#include "walb_diff_base.hpp"
#include "walb_diff_file.hpp"
#include "walb_diff_mem.hpp"
#include "walb_diff_stat.hpp"
#include "fileio.hpp"

namespace walb {

/**
 * To merge walb diff files.
 *
 * Usage:
 *   (1) call setMaxIoBlocks() and setShouldValidateUuid() if necessary.
 *   (2) add wdiffs by calling addWdiff() or addWdiffs().
 *   (3a) call mergeToFd() to write out the merged diff data.
 *   (3b) call prepare(), then call header() and getAndRemove() multiple times for other purpose.
 */
class DiffMerger /* final */
{
private:
    class Wdiff {
    private:
        const std::string wdiffPath_;
        mutable DiffReader reader_;
        DiffFileHeader header_;
        mutable DiffRecord rec_; // checksum field may not be calculated.
        mutable DiffIo io_;
        mutable bool isFilled_;
        mutable bool isEnd_;
    public:
        explicit Wdiff(const std::string &wdiffPath)
            : wdiffPath_(wdiffPath)
            , reader_(cybozu::util::File(wdiffPath, O_RDONLY))
            , rec_()
            , io_()
            , isFilled_(false)
            , isEnd_(false) {
            reader_.readHeader(header_);
        }
        /**
         * You must open the file before calling this constructor.
         */
        explicit Wdiff(cybozu::util::File &&file)
            : wdiffPath_()
            , reader_(std::move(file))
            , rec_()
            , io_()
            , isFilled_(false)
            , isEnd_(false) {
            reader_.readHeader(header_);
        }
        const std::string &path() const { return wdiffPath_; }
        const DiffFileHeader &header() const { return header_; }
        DiffRecord getFrontRec() const {
            verifyNotEnd(__func__);
            fill();
            return rec_;
        }
        void getAndRemoveIo(DiffIo &io);
        bool isEnd() const {
            fill();
            return isEnd_;
        }
        /**
         * RETURN:
         *   if the iterator has not reached the end,
         *   address of the current diff record.
         *   otherwise, UINT64_MAX.
         */
        uint64_t currentAddress() const {
            if (isEnd()) return UINT64_MAX;
            verifyFilled(__func__);
            return rec_.io_address;
        }
        const DiffStatistics& getStat() const {
            return reader_.getStat();
        }
    private:
        void fill() const;
#ifdef DEBUG
        void verifyNotEnd(const char *msg) const {
            if (isEnd()) throw cybozu::Exception(msg) << "reached to end";
        }
        void verifyFilled(const char *msg) const {
            if (!isFilled_) throw cybozu::Exception(msg) << "not filled";
        }
#else
        void verifyNotEnd(const char *) const {
        }
        void verifyFilled(const char *) const {
        }
#endif
    };
    bool shouldValidateUuid_;
    uint32_t maxIoBlocks_;

    DiffFileHeader wdiffH_;
    bool isHeaderPrepared_;

    using WdiffPtr = std::unique_ptr<Wdiff>;
    using WdiffPtrList = std::list<WdiffPtr>;
    WdiffPtrList wdiffs_;
    DiffMemory diffMem_;
    std::queue<DiffRecIo> mergedQ_;
    uint64_t doneAddr_;
    size_t searchLen_;
    /**
     * Diff recIos will be read from wdiffs_,
     * then added to diffMem_ (and merged inside it),
     * then pushed to mergedQ_ finally.
     *
     * The point of the algorithm is which wdiff will be chosen to get recIos.
     * See moveToDiffMemory() for detail.
     *
     * doneAddr_ is the minimum address in all the input wdiff streams.
     * There is no overlapped IOs which endAddr is <= doneAddr in all the streams.
     * so such IOs in diffMem_ can be put out safely.
     *
     * searchLen_ is required length [block size] as a buffer to merge wdiffs.
     * Ex. with the following diffs, we need to merge them at once.
     *
     * diff2     XXXXX
     * diff1        XXXXX
     * diff0          XXXXX
     * required  <-------->
     */

    /**
     * statIn: input wdiffs statistics.
     * statOut: output wdiff statistics.
     *     This is meaningful only when you use mergeToFd().
     */
    mutable DiffStatistics statIn_, statOut_;

public:
    explicit DiffMerger(size_t initSearchLen = DEFAULT_MERGE_BUFFER_LB)
        : shouldValidateUuid_(false)
        , maxIoBlocks_(0)
        , wdiffH_()
        , isHeaderPrepared_(false)
        , wdiffs_()
        , diffMem_()
        , mergedQ_()
        , doneAddr_(0)
        , searchLen_(initSearchLen)
        , statIn_(), statOut_() {
    }
    /**
     * @maxIoBlocks Max io blocks in the output wdiff [logical block].
     *     0 means no limitation.
     */
    void setMaxIoBlocks(uint32_t maxIoBlocks) {
        maxIoBlocks_ = maxIoBlocks;
    }
    /**
     * @shouldValidateUuid validate that all wdiff's uuid are the same if true,
     */
    void setShouldValidateUuid(bool shouldValidateUuid) {
        shouldValidateUuid_ = shouldValidateUuid;
    }
    /**
     * Add a diff file.
     * Newer wdiff file must be added later.
     */
    void addWdiff(const std::string& wdiffPath) {
        wdiffs_.emplace_back(new Wdiff(wdiffPath));
    }
    /**
     * Add diff files.
     */
    void addWdiffs(const StrVec &wdiffPaths) {
        for (const std::string &s : wdiffPaths) {
            addWdiff(s);
        }
    }
    void addWdiffs(std::vector<cybozu::util::File> &&fileV) {
        for (cybozu::util::File &file : fileV) {
            wdiffs_.emplace_back(new Wdiff(std::move(file)));
        }
        fileV.clear();
    }
    /**
     * Merge input wdiff files and put them into output fd.
     * The last wdiff's uuid will be used for output wdiff.
     *
     * @outFd file descriptor for output wdiff.
     */
    void mergeToFd(int outFd);
    /**
     * Prepare wdiff header and variables.
     */
    void prepare();
    /**
     * Get header.
     */
    const DiffFileHeader &header() const {
        assert(isHeaderPrepared_);
        return wdiffH_;
    }
    /**
     * Get a DiffRecIo and remove it from the merger.
     * RETURN:
     *   false if there is no diffIo anymore.
     */
    bool getAndRemove(DiffRecIo &recIo);

    const DiffStatistics& statIn() const {
        assert(wdiffs_.empty());
        return statIn_;
    }
    /**
     * Use this only if you used mergeToFd().
     */
    const DiffStatistics& statOut() const {
        assert(wdiffs_.empty());
        return statOut_;
    }
    std::string memUsageStr() const {
        return cybozu::itoa(searchLen_ * LBS / KIBI) + "KiB";
    }
private:
    uint64_t getMinimumAddr() const;
    void moveToDiffMemory();

    /**
     * Try to get Ios from wdiffs and add to wdiffMem_.
     *
     * minimum current address among wdiffs will be set to doneAddr_.
     * UINT64_MAX if there is no wdiffs.
     *
     * RETURN:
     *   the number of merged IOs.
     */
    size_t tryMoveToDiffMemory();

    bool shouldMerge(const DiffRecord& rec, uint64_t minAddr) const {
        return (rec.io_address < doneAddr_ + searchLen_)
            && (rec.endIoAddress() <= minAddr);
    }
    /**
     * Move all IOs which ioAddress + ioBlocks <= doneAddr
     * from diffMem_ to the mergedQ_.
     *
     * RETURN:
     *   false if there is no Io to move.
     */
    bool moveToMergedQueue();
    void removeEndedWdiffs();

    void mergeIo(const DiffRecord &rec, DiffIo &&io) {
        assert(!rec.isCompressed());
        diffMem_.add(rec, std::move(io), maxIoBlocks_);
    }

    void verifyUuid(const cybozu::Uuid &uuid) const;
    uint32_t getMaxIoBlocks() const;
};

} //namespace walb
