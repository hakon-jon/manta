// -*- mode: c++; indent-tabs-mode: nil; -*-
//
// Manta
// Copyright (c) 2013-2014 Illumina, Inc.
//
// This software is provided under the terms and conditions of the
// Illumina Open Source Software License 1.
//
// You should have received a copy of the Illumina Open Source
// Software License 1 along with this program. If not, see
// <https://github.com/sequencing/licenses/>
//

///
/// \author Bret Barnes, Xiaoyu Chen
///

#include "manta/ReadGroupStatsUtil.hh"

#include "blt_util/align_path_bam_util.hh"
#include "blt_util/bam_record_util.hh"
#include "blt_util/bam_streamer.hh"
#include "blt_util/log.hh"
#include "blt_util/ReadKey.hh"
#include "common/Exceptions.hh"
#include "manta/SVLocusScanner.hh"

#include "boost/foreach.hpp"

#include <iostream>
#include <set>
#include <sstream>
#include <vector>



/// compare distributions to determine stats convergence
static
bool
isStatSetMatch(
    const SizeDistribution& pss1,
    const SizeDistribution& pss2)
{
    static const float cdfPrecision(0.001);

    for (float prob(0.05); prob < 1; prob += 0.1)
    {
        // check if percentile values equal
        if (std::abs(pss1.quantile(prob) - pss2.quantile(prob)) >= 1)
        {
            return false;
        }

        // check the convergence of fragsize cdf
        const int fragSize(pss2.quantile(prob));
        if (std::abs(pss1.cdf(fragSize) - pss2.cdf(fragSize)) >= cdfPrecision)
        {
            return false;
        }
    }

    return true;
}



/// This produces a useful result only when both reads align to the same
/// chromosome.
static
PAIR_ORIENT::index_t
getRelOrient(
    const bam_record& br)
{
    pos_t pos1 = br.pos();
    bool is_fwd_strand1 = br.is_fwd_strand();
    pos_t pos2 = br.mate_pos();
    bool is_fwd_strand2 = br.is_mate_fwd_strand();

    if (! br.is_first())
    {
        std::swap(pos1,pos2);
        std::swap(is_fwd_strand1,is_fwd_strand2);
    }

    return PAIR_ORIENT::get_index(pos1,is_fwd_strand1,pos2,is_fwd_strand2);
}



/// get insert size from bam record but limit the precision to 4 digits
static
unsigned
getSimplifiedFragSize(
    const bam_record& bamRead)
{
    unsigned fragSize(std::abs(bamRead.template_size()));

    // reduce fragsize resolution for very large sizes:
    // (large sizes are uncommon -- this doesn't need to be efficient, and it's not)
    unsigned steps(0);
    while (fragSize>1000)
    {
        fragSize /= 10;
        steps++;
    }
    for (unsigned stepIndex(0); stepIndex<steps; ++stepIndex) fragSize *= 10;

    return fragSize;
}



struct ReadGroupLabel
{
    explicit
    ReadGroupLabel(
        const char* bamLabelPtr = NULL,
        const char* rgLabelPtr = NULL) :
        bamLabel((NULL==bamLabelPtr) ? "" : bamLabelPtr),
        rgLabel((NULL==rgLabelPtr) ? "" : rgLabelPtr)
    {}

    bool
    operator<(
        const ReadGroupLabel& rhs) const
    {
        if (bamLabel < rhs.bamLabel) return true;
        if (bamLabel == rhs.bamLabel)
        {
            return (rgLabel < rhs.rgLabel);
        }
        return false;
    }

    const std::string bamLabel;
    const std::string rgLabel;
};



std::ostream&
operator<<(
    std::ostream& os,
    const ReadGroupLabel& rgl)
{
    os << "read group '" << rgl.rgLabel << "' in bam file '" << rgl.bamLabel;
    return os;
}



/// track pair orientation so that a consensus can be found for a read group
///
struct ReadGroupOrientTracker
{
    ReadGroupOrientTracker(
        const char* bamLabel,
        const char* rgLabel) :
        _isFinalized(false),
        _totalOrientCount(0),
        _rgLabel(bamLabel,rgLabel)
    {
        std::fill(_orientCount.begin(),_orientCount.end(),0);
    }

    void
    addOrient(
        const bam_record& bamRead)
    {
        if (bamRead.pos() == bamRead.mate_pos()) return;

        static const unsigned maxOrientCount(100000);
        if (_totalOrientCount < maxOrientCount)
        {
            addOrient(getRelOrient(bamRead));
        }
    }

    const ReadPairOrient&
    getConsensusOrient()
    {
        finalize();
        return _finalOrient;
    }

private:

    void
    addOrient(
        const PAIR_ORIENT::index_t id)
    {
        assert(! _isFinalized);
        assert((id>=0) && (id<PAIR_ORIENT::SIZE));

        _orientCount[id]++;
        _totalOrientCount++;
    }

    void
    finalize()
    {
        if (_isFinalized) return;

        bool isMaxIndex(false);
        unsigned maxIndex(0);
        for (unsigned i(0); i<_orientCount.size(); ++i)
        {
            if ((! isMaxIndex) || (_orientCount[i] > _orientCount[maxIndex]))
            {
                isMaxIndex=true;
                maxIndex=i;
            }
        }

        assert(isMaxIndex);

        _finalOrient.setVal(maxIndex);

        {
            // make sure there's a dominant consensus orientation and that we have a minimum number of samples:
            static const unsigned minCount(100);
            static const float minMaxFrac(0.9);

            using namespace illumina::common;

            if (_totalOrientCount < minCount)
            {
                std::ostringstream oss;
                oss << "ERROR: Too few observations (" << _totalOrientCount << ") to determine pair orientation for " << _rgLabel << "'\n";
                BOOST_THROW_EXCEPTION(LogicException(oss.str()));
            }

            const unsigned minMaxCount(minMaxFrac*_totalOrientCount);
            if (_orientCount[maxIndex] < minMaxCount)
            {
                const unsigned maxPercent((_orientCount[maxIndex]*100)/_totalOrientCount);
                std::ostringstream oss;
                oss << "ERROR: Can't determine consensus pair orientation of " << _rgLabel << ".\n"
                    << "\tMost frequent orientation is '" << _finalOrient << "' (" << maxPercent << "% of " << _totalOrientCount << " total observations)\n";
                BOOST_THROW_EXCEPTION(LogicException(oss.str()));
            }
        }

        _isFinalized=true;
    }

    bool _isFinalized;
    unsigned _totalOrientCount;
    const ReadGroupLabel _rgLabel;
    boost::array<unsigned,PAIR_ORIENT::SIZE> _orientCount;

    ReadPairOrient _finalOrient;
};




/// all data required to build ReadGroupStats during estimation from the bam file
///
/// ultimately the only information we want to keep is the ReadGroupStats object itself,
/// which can be exported from this object
///
struct ReadGroupTracker
{
    explicit
    ReadGroupTracker(
        const char* bamLabel = NULL,
        const char* rgLabel = NULL) :
        _isFinalized(false),
        _rgLabel(bamLabel, rgLabel),
        _orientInfo(bamLabel, rgLabel),
        _isChecked(false),
        _isInsertSizeConverged(false)
    {}

    void
    addOrient(
        const bam_record& bamRead)
    {
        assert(! _isFinalized);

        _orientInfo.addOrient(bamRead);
    }

    void
    addInsertSize(const int size)
    {
        assert(! _isFinalized);

        _stats.fragStats.addObservation(size);
    }

    unsigned
    insertSizeObservations() const
    {
        return _stats.fragStats.totalObservations();
    }

    bool
    isInsertSizeCountCheck()
    {
        static const unsigned statsCheckCnt(100000);
        const bool isCheck((insertSizeObservations() % statsCheckCnt) == 0);
        if (isCheck) _isChecked=true;
        return isCheck;
    }

    bool
    isChecked() const
    {
        return _isChecked;
    }

    void
    clearChecked()
    {
        _isChecked=false;
    }

    bool
    isInsertSizeConverged() const
    {
        return _isInsertSizeConverged;
    }

    void
    updateInsertSizeConvergenceTest()
    {
        // check convergence
        if (_oldInsertSize.totalObservations() > 0)
        {
            _isInsertSizeConverged=isStatSetMatch(_oldInsertSize, _stats.fragStats);
        }
        _oldInsertSize = _stats.fragStats;
    }

    /// getting a const ref of the stats forces finalization steps:
    const ReadGroupStats&
    getStats() const
    {
        assert(_isFinalized);
        return _stats;
    }

    void
    finalize()
    {
        if (_isFinalized) return;

        // finalize pair orientation:
        _stats.relOrients = _orientInfo.getConsensusOrient();

        if (_stats.relOrients.val() != PAIR_ORIENT::Rp)
        {
            using namespace illumina::common;

            std::ostringstream oss;
            oss << "ERROR: Unexpected consensus read orientation (" << _stats.relOrients << ") for " << _rgLabel << "\n"
                << "\tManta currently handles paired-end (FR) reads only.\n";
            BOOST_THROW_EXCEPTION(LogicException(oss.str()));
        }

        // finalize insert size distro:
        if (! isInsertSizeConverged())
        {
            if (_stats.fragStats.totalObservations() <1000)
            {
                using namespace illumina::common;

                std::ostringstream oss;
                oss << "ERROR: Can't generate pair statistics for " << _rgLabel << "\n"
                    << "\tTotal observed read pairs: " << insertSizeObservations() << "\n";
                BOOST_THROW_EXCEPTION(LogicException(oss.str()));
            }
            else if (! isInsertSizeCountCheck())
            {
                updateInsertSizeConvergenceTest();
            }

            if (! isInsertSizeConverged())
            {
                log_os << "WARNING: read pair statistics did not converge for " << _rgLabel << "\n";
            }
        }

        // final step before saving is to cut-off the extreme end of the fragment size distribution, this
        // is similar the some aligner's proper-pair bit definition of (3x the standard mean, etc.)
        static const float filterQuant(0.9995);
        _stats.fragStats.filterObservationsOverQuantile(filterQuant);

        _isFinalized=true;
    }

private:

    bool _isFinalized;
    const ReadGroupLabel _rgLabel;
    ReadGroupOrientTracker _orientInfo;

    bool _isChecked;
    bool _isInsertSizeConverged;
    SizeDistribution _oldInsertSize; // previous fragment distribution is stored to determine convergence

    ReadGroupStats _stats;
};



struct ReadAlignFilter
{
    /// use only the most conservative alignments to generate fragment stats --
    /// filter reads containing any cigar types besides MATCH:
    bool
    isFilterRead(
        const bam_record& bamRead)
    {
        using namespace ALIGNPATH;

        bam_cigar_to_apath(bamRead.raw_cigar(), bamRead.n_cigar(), _apath);

        BOOST_FOREACH(const path_segment& ps, _apath)
        {
            if (! is_segment_align_match(ps.type)) return true;
        }
        return false;
    }

private:
    ALIGNPATH::path_t _apath;
};



/// this object handles filtration of reads which are:
///
/// 1. not downstream or (if both reads start at same position) not the second in order in the bam file
/// 2. part of a high depth pileup region
///
struct ReadPairDepthFilter
{
    ReadPairDepthFilter() :
        _posCount(0),
        _lastTargetId(0),
        _lastPos(0)
    {}

    bool
    isFilterRead(
        const bam_record& bamRead)
    {
        static const unsigned maxPosCount(1);

        if (bamRead.target_id() != _lastTargetId)
        {
            _goodMates.clear();
            _lastTargetId=bamRead.target_id();
            _posCount=0;
            _lastPos = bamRead.pos();
        }
        else if (bamRead.pos() != _lastPos)
        {
            _posCount=0;
            _lastPos = bamRead.pos();
        }

        // Assert only two reads per fragment
        const unsigned readNum(bamRead.is_first() ? 1 : 2);
        assert(bamRead.is_second() == (readNum == 2));

        // sample each read pair once by sampling stats from
        // downstream read only, or whichever read is encountered
        // second if the read and its mate start at the same position:
        const bool isDownstream(bamRead.pos() > bamRead.mate_pos());
        const bool isSamePos(bamRead.pos() == bamRead.mate_pos());

        if (isDownstream || isSamePos)
        {
            const int mateReadNo( bamRead.is_first() ? 2 : 1);
            const ReadKey mateKey(bamRead.qname(), mateReadNo);

            mateMap_t::iterator i(_goodMates.find(mateKey));

            if (i == _goodMates.end())
            {
                if (isDownstream) return true;
            }
            else
            {
                _goodMates.erase(i);
                return false;
            }
        }

        // to prevent high-depth pileups from overly biasing the
        // read stats, we only take maxPosCount read pairs from each start
        // pos. by not inserting a key in goodMates, we also filter
        // the downstream mate:
        if (_posCount>=maxPosCount) return true;
        ++_posCount;

        /// crude mechanism to manage total set memory
        static const unsigned maxMateSetSize(100000);
        if (_goodMates.size() > maxMateSetSize) _goodMates.clear();

        _goodMates.insert(ReadKey(bamRead));

        return true;
    }

private:
    typedef std::set<ReadKey> mateMap_t;

    unsigned _posCount;
    mateMap_t _goodMates;

    int _lastTargetId;
    int _lastPos;
};



struct CoreInsertStatsReadFilter
{
    bool
    isFilterRead(
        const bam_record& bamRead)
    {
        // filter common categories of undesirable reads:
        if (SVLocusScanner::isReadFilteredCore(bamRead)) return true;

        if (! is_mapped_chrom_pair(bamRead)) return true;
        if (bamRead.map_qual()==0) return true;

        // filter any split reads with an SA tag:
        static const char SAtag[] = {'S','A'};
        if (NULL != bamRead.get_string_tag(SAtag)) return true;

        // remove reads without perfect alignments
        if (alignFilter.isFilterRead(bamRead)) return true;

        /// filter out upstream reads and high depth regions:
        if (pairFilter.isFilterRead(bamRead)) return true;

        return false;
    }

    ReadAlignFilter alignFilter;
    ReadPairDepthFilter pairFilter;
};



#if 0
/// samples around various short segments of the genome so
/// that stats generation isn't biased towards a single region
struct GenomeSampler
{
    GenomeSampler(
        const std::string& bamFile) :
        _readStream(bamFile.c_str()),
        _chromCount(0),
        _isInitRegion(false),
        _isInitRecord(false),
        _isActiveChrom(true),
        _currentChrom(0)
    {
        const bam_header_t& header(*_readStream.get_header());

        _chromCount = (header.n_targets);

        _isActiveChrom = (_chromCount > 0);

        _chromSize.resize(_chromCount,0);
        _chromHighestPos.resize(_chromCount,-1);

        for (int32_t i(0); i<_chromCount; ++i)
        {
            _chromSize[i] = (header.target_len[i]);
        }

        init();
    }

    /// advance to next region of the genome, this must be called before using nextRecord():
    bool
    nextRegion()
    {
        if (! _isActiveChrom) return false;

        _isInitRegion=true;
        _isInitRecord=false;
        return true;
    }

    /// advance to next record in the current region, this must be called before using getBamRecord():
    bool
    nextRecord()
    {
        assert(_isInitRegion);

        if (! _isActiveChrom) return false;

        _isInitRecord=true;
        return true;
    }

    /// access current bam record
    const bam_record&
    getBamRecord()
    {
        assert(_isInitRegion);
        assert(_isInitRecord);

        return *(_readStream.get_record_ptr());
    }

private:
    void
    init()
    {
        if (! _isActiveChrom) return;

        const int32_t startPos(_chromHighestPos[chromIndex]+1);

        _currentChrom=0;
    }

    bam_streamer _readStream;
    int32_t _chromCount;
    std::vector<int32_t> _chromSize;

    std::vector<int32_t> _chromHighestPos;
    bool _isInitRegion;
    bool _isInitRecord;
    bool _isActiveChrom; ///< this is used to track whether we've reached the end of all chromosomes
    int32_t _currentChrom;
};
#endif



/// manage the info structs for each RG
struct ReadGroupManager
{
    typedef std::map<ReadGroupLabel,ReadGroupTracker> RGMapType;

    ReadGroupManager(
        const std::string& statsBamFile) :
        _isFinalized(false),
        _statsBamFile(statsBamFile)
    {}

    ReadGroupTracker&
    getTracker(
        const bam_record& bamRead)
    {
        const char* readGroup(getReadGroup(bamRead));
        ReadGroupLabel rgKey(_statsBamFile.c_str(), readGroup);

        RGMapType::iterator rgIter(_rgTracker.find(rgKey));
        if (rgIter == _rgTracker.end())
        {
            std::pair<RGMapType::iterator,bool> retval;
            retval = _rgTracker.insert(std::make_pair(rgKey,ReadGroupTracker(_statsBamFile.c_str(),readGroup)));

            assert(retval.second);
            rgIter = retval.first;
        }

        return rgIter->second;
    }

    // check if all read groups have been sufficiently sampled in this region:
    bool
    isFinishedRegion()
    {
        BOOST_FOREACH(RGMapType::value_type& val, _rgTracker)
        {
            if(! val.second.isChecked()) return false;
        }

        BOOST_FOREACH(RGMapType::value_type& val, _rgTracker)
        {
            val.second.clearChecked();
        }

        return true;
    }

    // test if all read groups have converged or hit other stopping conditions
    bool
    isStopEstimation()
    {
        static const unsigned maxRecordCount(5000000);
        BOOST_FOREACH(RGMapType::value_type& val, _rgTracker)
        {
            if(! (val.second.isInsertSizeConverged() || (val.second.insertSizeObservations()>maxRecordCount))) return false;
        }
        return true;
    }

    /// must call this before getting the final map:
    void
    finalize()
    {
        if (_isFinalized) return;
        BOOST_FOREACH(RGMapType::value_type& val, _rgTracker)
        {
            val.second.finalize();
        }
    }

    const RGMapType&
    getMap() const
    {
        assert(_isFinalized);
        return _rgTracker;
    }

private:
    bool _isFinalized;
    std::string _statsBamFile;
    RGMapType _rgTracker;
};



void
extractReadGroupStatsFromBam(
    const std::string& statsBamFile,
    ReadGroupStatsSet& rstats)
{
    bam_streamer read_stream(statsBamFile.c_str());

    const bam_header_t& header(* read_stream.get_header());
    const int32_t chromCount(header.n_targets);
    std::vector<int32_t> chromSize(chromCount,0);
    std::vector<int32_t> chromHighestPos(chromCount,-1);
    for (int32_t i(0); i<chromCount; ++i)
    {
        chromSize[i] = (header.target_len[i]);
    }

    bool isStopEstimation(false);
    bool isActiveChrom(true);

    CoreInsertStatsReadFilter coreFilter;
    ReadGroupManager rgManager(statsBamFile.c_str());

    while (isActiveChrom && (!isStopEstimation))
    {
        isActiveChrom=false;
        for (int32_t chromIndex(0); chromIndex<chromCount; ++chromIndex)
        {
            if (isStopEstimation) break;

            const int32_t startPos(chromHighestPos[chromIndex]+1);
#ifdef DEBUG_RPS
            std::cerr << "INFO: Stats requesting bam region starting from: chrid: " << chromIndex << " start: " << startPos << "\n";
#endif
            read_stream.set_new_region(chromIndex,startPos,chromSize[chromIndex]);
            while (read_stream.next())
            {
                const bam_record& bamRead(*(read_stream.get_record_ptr()));
                if (bamRead.pos()<startPos) continue;

                chromHighestPos[chromIndex]=bamRead.pos();
                isActiveChrom=true;

                if (coreFilter.isFilterRead(bamRead)) continue;

                ReadGroupTracker& rgInfo(rgManager.getTracker(bamRead));

                // get orientation stats before final filter for innie reads below:
                //
                // we won't use anything but innie reads for insert size stats, but sampling
                // orientation beforehand allows us to detect, ie. a mate-pair library
                // so that we can blow-up with an informative error msg
                //
                rgInfo.addOrient(bamRead);

                // filter mapped innies on the same chrom
                //
                // note we don't rely on the proper pair bit because this already contains an
                // arbitrary length filter and  subjects the method to aligner specific variation
                //
                // TODO: ..note this locks-in standard ilmn orientation -- okay for now but this function needs major
                // re-arrangement for mate-pair support, we could still keep independence from each aligner's proper
                // pair decisions by estimating a fragment distro for each orientation and only keeping the one with
                // the most samples
                //
                if (! is_innie_pair(bamRead)) continue;

                rgInfo.addInsertSize(getSimplifiedFragSize(bamRead));

                if (! rgInfo.isInsertSizeCountCheck()) continue;

                // check convergence
                rgInfo.updateInsertSizeConvergenceTest();

                if (! rgManager.isFinishedRegion()) continue;

                isStopEstimation = rgManager.isStopEstimation();

                // break from reading the current chromosome
                break;
            }
        }
    }

    rgManager.finalize();

    BOOST_FOREACH(const ReadGroupManager::RGMapType::value_type& val, rgManager.getMap())
    {
        rstats.setStats(val.first.bamLabel, val.first.rgLabel, val.second.getStats());
    }
}
