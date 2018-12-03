#include "main.h"

#include "addrman.h"
#include "arith_uint256.h"
#include "buip055fork.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "checkqueue.h"
#include "clientversion.h"
#include "connmgr.h"
#include "consensus/consensus.h"
#include "consensus/merkle.h"
#include "consensus/validation.h"
#include "dosman.h"
#include "expedited.h"
#include "hash.h"
#include "init.h"
#include "merkleblock.h"
#include "net.h"
#include "nodestate.h"
#include "parallel.h"
#include "policy/policy.h"
#include "pow.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "requestManager.h"
#include "script/script.h"
#include "script/sigcache.h"
#include "script/standard.h"
#include "thinblock.h"
#include "tinyformat.h"
#include "txdb.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "undo.h"
#include "util.h"
#include "utilmoneystr.h"
#include "utilstrencodings.h"
#include "validationinterface.h"
#include "versionbits.h"

#include <algorithm>
#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/math/distributions/poisson.hpp>
#include <boost/scope_exit.hpp>
#include <boost/thread.hpp>
#include <sstream>

#if defined(NDEBUG)
#error "Bitcoin cannot be compiled without assertions."
#endif

/**
 * Global state
 */

// BU variables moved to globals.cpp
// BU moved CCriticalSection cs_main;

// BU moved BlockMap mapBlockIndex;
// BU movedCChain chainActive;
CBlockIndex *pindexBestHeader = NULL;

CCoinsViewDB *pcoinsdbview = nullptr;

int64_t nTimeBestReceived = 0;
// BU moved CWaitableCriticalSection csBestBlock;
// BU moved CConditionVariable cvBlockChange;
bool fImporting = false;
bool fReindex = false;
bool fTxIndex = false;
bool fHavePruned = false;
bool fPruneMode = false;
bool fIsBareMultisigStd = DEFAULT_PERMIT_BAREMULTISIG;
bool fRequireStandard = true;
unsigned int nBytesPerSigOp = DEFAULT_BYTES_PER_SIGOP;
bool fCheckBlockIndex = false;
bool fCheckpointsEnabled = DEFAULT_CHECKPOINTS_ENABLED;
size_t nCoinCacheUsage = 5000 * 300;
uint64_t nPruneTarget = 0;
uint32_t nXthinBloomFilterSize = MAX_BLOOM_FILTER_SIZE;

CFeeRate minRelayTxFee = CFeeRate(DEFAULT_MIN_RELAY_TX_FEE);

// BU: Move global objects to a single file
extern CTxMemPool mempool;


// BU: start block download at low numbers in case our peers are slow when we start
/** Number of blocks that can be requested at any given time from a single peer. */
static unsigned int MAX_BLOCKS_IN_TRANSIT_PER_PEER = 1;
/** Size of the "block download window": how far ahead of our current height do we fetch?
 *  Larger windows tolerate larger download speed differences between peer, but increase the potential
 *  degree of disordering of blocks on disk (which make reindexing and in the future perhaps pruning
 *  harder). We'll probably want to make this a per-peer adaptive value at some point. */
static unsigned int BLOCK_DOWNLOAD_WINDOW = 256;

extern CTweak<unsigned int> maxBlocksInTransitPerPeer; // override the above
extern CTweak<unsigned int> blockDownloadWindow;
extern CTweak<uint64_t> reindexTypicalBlockSize;

extern std::map<CNetAddr, ConnectionHistory> mapInboundConnectionTracker;
extern CCriticalSection cs_mapInboundConnectionTracker;

/** A cache to store headers that have arrived but can not yet be connected **/
std::map<uint256, std::pair<CBlockHeader, int64_t> > mapUnConnectedHeaders;

/**
 * Returns true if there are nRequired or more blocks of minVersion or above
 * in the last Consensus::Params::nMajorityWindow blocks, starting at pstart and going backwards.
 */
static bool IsSuperMajority(int minVersion,
    const CBlockIndex *pstart,
    unsigned nRequired,
    const Consensus::Params &consensusParams);
static void CheckBlockIndex(const Consensus::Params &consensusParams);

/** Constant stuff for coinbase transactions we create: */
CScript COINBASE_FLAGS;

const std::string strMessageMagic = "Bitcoin Signed Message:\n";

extern CStatHistory<uint64_t> nBlockValidationTime;
extern CCriticalSection cs_blockvalidationtime;

extern CCriticalSection cs_LastBlockFile;
extern CCriticalSection cs_nBlockSequenceId;


// Internal stuff
namespace
{
struct CBlockIndexWorkComparator
{
    bool operator()(CBlockIndex *pa, CBlockIndex *pb) const
    {
        // First sort by most total work, ...
        if (pa->nChainWork > pb->nChainWork)
            return false;
        if (pa->nChainWork < pb->nChainWork)
            return true;

        // ... then by earliest time received, ...
        if (pa->nSequenceId < pb->nSequenceId)
            return false;
        if (pa->nSequenceId > pb->nSequenceId)
            return true;

        // Use pointer address as tie breaker (should only happen with blocks
        // loaded from disk, as those all have id 0).
        if (pa < pb)
            return false;
        if (pa > pb)
            return true;

        // Identical blocks.
        return false;
    }
};

CBlockIndex *pindexBestInvalid;

/**
 * The set of all CBlockIndex entries with BLOCK_VALID_TRANSACTIONS (for itself and all ancestors) and
 * as good as our current tip or better. Entries may be failed, though, and pruning nodes may be
 * missing the data for the block.
 */
std::set<CBlockIndex *, CBlockIndexWorkComparator> setBlockIndexCandidates;
/** Number of nodes with fSyncStarted. */
int nSyncStarted = 0;
/** All pairs A->B, where A (or one of its ancestors) misses transactions, but B has transactions.
 * Pruned nodes may have entries where B is missing data.
 */
std::multimap<CBlockIndex *, CBlockIndex *> mapBlocksUnlinked;

std::vector<CBlockFileInfo> vinfoBlockFile;
int nLastBlockFile = 0;
/** Global flag to indicate we should check to see if there are
 *  block/undo files that should be deleted.  Set on startup
 *  or if we allocate more file space when we're in prune mode
 */
bool fCheckForPruning = false;

/**
 * Every received block is assigned a unique and increasing identifier, so we
 * know which one to give priority in case of a fork.
 */
/** Blocks loaded from disk are assigned id 0, so start the counter at 1. */
uint32_t nBlockSequenceId = 1;

/**
 * Sources of received blocks, saved to be able to send them reject
 * messages or ban them when processing happens afterwards. Protected by
 * cs_main.
 */
std::map<uint256, NodeId> mapBlockSource;


uint256 hashRecentRejectsChainTip;

/** Number of preferable block download peers. */
int nPreferredDownload = 0;

/** Dirty block file entries. */
std::set<int> setDirtyFileInfo;

/** Number of peers from which we're downloading blocks. */
int nPeersWithValidatedDownloads = 0;
} // anon namespace

/** Dirty block index entries. */
std::set<CBlockIndex *> setDirtyBlockIndex;

//////////////////////////////////////////////////////////////////////////////
//
// Registration of network node signals.
//

namespace
{
int GetHeight()
{
    LOCK(cs_main);
    return chainActive.Height();
}

void UpdatePreferredDownload(CNode *node, CNodeState *state)
{
    nPreferredDownload -= state->fPreferredDownload;

    // Whether this node should be marked as a preferred download node.
    state->fPreferredDownload = !node->fOneShot && !node->fClient;
    // BU allow downloads from inbound nodes; this may have been limited to stop attackers from connecting
    // and offering a bad chain.  However, we are connecting to multiple nodes and so can choose the most work
    // chain on that basis.
    // state->fPreferredDownload = (!node->fInbound || node->fWhitelisted) && !node->fOneShot && !node->fClient;
    // LogPrint("net", "node %s preferred DL: %d because (%d || %d) && %d && %d\n", node->GetLogName(),
    //   state->fPreferredDownload, !node->fInbound, node->fWhitelisted, !node->fOneShot, !node->fClient);

    nPreferredDownload += state->fPreferredDownload;
}

void InitializeNode(NodeId nodeid, const CNode *pnode)
{
    LOCK(cs_main);
    CNodeState &state = mapNodeState.insert(std::make_pair(nodeid, CNodeState())).first->second;
    state.name = pnode->addrName;
    state.address = pnode->addr;
}

void FinalizeNode(NodeId nodeid)
{
    LOCK(cs_main);
    CNodeState *state = State(nodeid);

    if (state->fSyncStarted)
        nSyncStarted--;

    BOOST_FOREACH (const QueuedBlock &entry, state->vBlocksInFlight)
    {
        mapBlocksInFlight.erase(entry.hash);
    }
    nPreferredDownload -= state->fPreferredDownload;
    nPeersWithValidatedDownloads -= (state->nBlocksInFlightValidHeaders != 0);
    DbgAssert(nPeersWithValidatedDownloads >= 0, nPeersWithValidatedDownloads = 0);

    mapNodeState.erase(nodeid);

    if (mapNodeState.empty())
    {
        // Do a consistency check after the last peer is removed.  Force consistent state if production code
        DbgAssert(mapBlocksInFlight.empty(), mapBlocksInFlight.clear());
        DbgAssert(nPreferredDownload == 0, nPreferredDownload = 0);
        DbgAssert(nPeersWithValidatedDownloads == 0, nPeersWithValidatedDownloads = 0);
    }
}

// Requires cs_main.
// Returns a bool indicating whether we requested this block.
bool MarkBlockAsReceived(const uint256 &hash)
{
    std::map<uint256, std::pair<NodeId, std::list<QueuedBlock>::iterator> >::iterator itInFlight =
        mapBlocksInFlight.find(hash);
    if (itInFlight != mapBlocksInFlight.end())
    {
        // BUIP010 Xtreme Thinblocks: begin section
        int64_t getdataTime = itInFlight->second.second->nTime;
        int64_t now = GetTimeMicros();
        double nResponseTime = (double)(now - getdataTime) / 1000000.0;

        // BU:  calculate avg block response time over last 20 blocks to be used for IBD tuning
        // start at a higher number so that we don't start jamming IBD when we restart a node sync
        static double avgResponseTime = 5;
        static uint8_t blockRange = 20;
        if (avgResponseTime > 0)
            avgResponseTime -= (avgResponseTime / blockRange);
        avgResponseTime += nResponseTime / blockRange;
        if (avgResponseTime < 0.2)
        {
            MAX_BLOCKS_IN_TRANSIT_PER_PEER = 32;
        }
        else if (avgResponseTime < 0.5)
        {
            MAX_BLOCKS_IN_TRANSIT_PER_PEER = 16;
        }
        else if (avgResponseTime < 0.9)
        {
            MAX_BLOCKS_IN_TRANSIT_PER_PEER = 8;
        }
        else if (avgResponseTime < 1.4)
        {
            MAX_BLOCKS_IN_TRANSIT_PER_PEER = 4;
        }
        else if (avgResponseTime < 2.0)
        {
            MAX_BLOCKS_IN_TRANSIT_PER_PEER = 2;
        }
        else
        {
            MAX_BLOCKS_IN_TRANSIT_PER_PEER = 1;
        }

        LogPrint("thin", "Received block %s in %.2f seconds\n", hash.ToString(), nResponseTime);
        LogPrint("thin", "Average block response time is %.2f seconds\n", avgResponseTime);
        if (maxBlocksInTransitPerPeer.value != 0)
        {
            MAX_BLOCKS_IN_TRANSIT_PER_PEER = maxBlocksInTransitPerPeer.value;
        }
        if (blockDownloadWindow.value != 0)
        {
            BLOCK_DOWNLOAD_WINDOW = blockDownloadWindow.value;
        }
        LogPrint("thin", "BLOCK_DOWNLOAD_WINDOW is %d MAX_BLOCKS_IN_TRANSIT_PER_PEER is %d\n", BLOCK_DOWNLOAD_WINDOW,
            MAX_BLOCKS_IN_TRANSIT_PER_PEER);

        {
            LOCK(cs_vNodes);
            BOOST_FOREACH (CNode *pnode, vNodes)
            {
                if (pnode->mapThinBlocksInFlight.size() > 0)
                {
                    LOCK(pnode->cs_mapthinblocksinflight);
                    if (pnode->mapThinBlocksInFlight.count(hash))
                    {
                        // Only update thinstats if this is actually a thinblock and not a regular block.
                        // Sometimes we request a thinblock but then revert to requesting a regular block
                        // as can happen when the thinblock preferential timer is exceeded.
                        thindata.UpdateResponseTime(nResponseTime);
                        break;
                    }
                }
            }
        }
        // BUIP010 Xtreme Thinblocks: end section
        CNodeState *state = State(itInFlight->second.first);
        state->nBlocksInFlightValidHeaders -= itInFlight->second.second->fValidatedHeaders;
        if (state->nBlocksInFlightValidHeaders == 0 && itInFlight->second.second->fValidatedHeaders)
        {
            // Last validated block on the queue was received.
            nPeersWithValidatedDownloads--;
        }
        if (state->vBlocksInFlight.begin() == itInFlight->second.second)
        {
            // First block on the queue was received, update the start download time for the next one
            state->nDownloadingSince = std::max(state->nDownloadingSince, GetTimeMicros());
        }
        state->vBlocksInFlight.erase(itInFlight->second.second);
        state->nBlocksInFlight--;
        mapBlocksInFlight.erase(itInFlight);
        return true;
    }
    return false;
}

// BU MarkBlockAsInFlight moved out of anonymous namespace

/** Check whether the last unknown block a peer advertised is not yet known. */
void ProcessBlockAvailability(NodeId nodeid)
{
    CNodeState *state = State(nodeid);
    DbgAssert(state != NULL, return ); // node already destructed, nothing to do in production mode
    // AssertLockHeld(csMapBlockIndex);

    if (!state->hashLastUnknownBlock.IsNull())
    {
        BlockMap::iterator itOld = mapBlockIndex.find(state->hashLastUnknownBlock);
        if (itOld != mapBlockIndex.end() && itOld->second->nChainWork > 0)
        {
            if (state->pindexBestKnownBlock == NULL ||
                itOld->second->nChainWork >= state->pindexBestKnownBlock->nChainWork)
                state->pindexBestKnownBlock = itOld->second;
            state->hashLastUnknownBlock.SetNull();
        }
    }
}


// Requires cs_main
bool PeerHasHeader(CNodeState *state, CBlockIndex *pindex)
{
    if (state->pindexBestKnownBlock && pindex == state->pindexBestKnownBlock->GetAncestor(pindex->nHeight))
        return true;
    if (state->pindexBestHeaderSent && pindex == state->pindexBestHeaderSent->GetAncestor(pindex->nHeight))
        return true;
    return false;
}

/** Find the last common ancestor two blocks have.
 *  Both pa and pb must be non-NULL. */
CBlockIndex *LastCommonAncestor(CBlockIndex *pa, CBlockIndex *pb)
{
    if (pa->nHeight > pb->nHeight)
    {
        pa = pa->GetAncestor(pb->nHeight);
    }
    else if (pb->nHeight > pa->nHeight)
    {
        pb = pb->GetAncestor(pa->nHeight);
    }

    while (pa != pb && pa && pb)
    {
        pa = pa->pprev;
        pb = pb->pprev;
    }

    // Eventually all chain branches meet at the genesis block.
    assert(pa == pb);
    return pa;
}

/** Update pindexLastCommonBlock and add not-in-flight missing successors to vBlocks, until it has
 *  at most count entries. */
static unsigned int FindNextBlocksToDownload(NodeId nodeid, unsigned int count, std::vector<CBlockIndex *> &vBlocks)
{
    unsigned int amtFound = 0;
    if (count == 0)
        return 0;

    vBlocks.reserve(vBlocks.size() + count);
    CNodeState *state = State(nodeid);
    DbgAssert(state != NULL, return 0);

    // Make sure pindexBestKnownBlock is up to date, we'll need it.
    ProcessBlockAvailability(nodeid);

    if (state->pindexBestKnownBlock == NULL || state->pindexBestKnownBlock->nChainWork < chainActive.Tip()->nChainWork)
    {
        // This peer has nothing interesting.
        return 0;
    }

    if (state->pindexLastCommonBlock == NULL)
    {
        // Bootstrap quickly by guessing a parent of our best tip is the forking point.
        // Guessing wrong in either direction is not a problem.
        state->pindexLastCommonBlock =
            chainActive[std::min(state->pindexBestKnownBlock->nHeight, chainActive.Height())];
    }

    // If the peer reorganized, our previous pindexLastCommonBlock may not be an ancestor
    // of its current tip anymore. Go back enough to fix that.
    state->pindexLastCommonBlock = LastCommonAncestor(state->pindexLastCommonBlock, state->pindexBestKnownBlock);
    if (state->pindexLastCommonBlock == state->pindexBestKnownBlock)
        return 0;

    std::vector<CBlockIndex *> vToFetch;
    CBlockIndex *pindexWalk = state->pindexLastCommonBlock;
    // Never fetch further than the current chain tip + the block download window.  We need to ensure
    // the if running in pruning mode we don't download too many blocks ahead and as a result use to
    // much disk space to store unconnected blocks.
    int nWindowEnd = chainActive.Height() + BLOCK_DOWNLOAD_WINDOW;

    int nMaxHeight = std::min<int>(state->pindexBestKnownBlock->nHeight, nWindowEnd + 1);
    while (pindexWalk->nHeight < nMaxHeight)
    {
        // Read up to 128 (or more, if more blocks than that are needed) successors of pindexWalk (towards
        // pindexBestKnownBlock) into vToFetch. We fetch 128, because CBlockIndex::GetAncestor may be as expensive
        // as iterating over ~100 CBlockIndex* entries anyway.
        int nToFetch = std::min(nMaxHeight - pindexWalk->nHeight, std::max<int>(count - vBlocks.size(), 128));
        vToFetch.resize(nToFetch);
        pindexWalk = state->pindexBestKnownBlock->GetAncestor(pindexWalk->nHeight + nToFetch);
        vToFetch[nToFetch - 1] = pindexWalk;
        for (unsigned int i = nToFetch - 1; i > 0; i--)
        {
            vToFetch[i - 1] = vToFetch[i]->pprev;
        }

        // Iterate over those blocks in vToFetch (in forward direction), adding the ones that
        // are not yet downloaded and not in flight to vBlocks. In the mean time, update
        // pindexLastCommonBlock as long as all ancestors are already downloaded, or if it's
        // already part of our chain (and therefore don't need it even if pruned).
        BOOST_FOREACH (CBlockIndex *pindex, vToFetch)
        {
            if (!pindex->IsValid(BLOCK_VALID_TREE))
            {
                // We consider the chain that this peer is on invalid.
                return amtFound;
            }
            if (pindex->nStatus & BLOCK_HAVE_DATA || chainActive.Contains(pindex))
            {
                if (pindex->nChainTx)
                    state->pindexLastCommonBlock = pindex;
            }
            else
            {
                // Return if we've reached the end of the download window.
                if (pindex->nHeight > nWindowEnd)
                {
                    return amtFound;
                }

                // Return if we've reached the end of the number of blocks we can download for this peer.
                vBlocks.push_back(pindex);
                amtFound += 1;
                if (vBlocks.size() == count)
                {
                    return amtFound;
                }
            }
        }
    }
    return amtFound;
}

} // anon namespace

/** Update tracking information about which blocks a peer is assumed to have. */
void UpdateBlockAvailability(NodeId nodeid, const uint256 &hash)
{
    CNodeState *state = State(nodeid);
    DbgAssert(state != NULL, return ); // node already destructed, nothing to do in production mode

    ProcessBlockAvailability(nodeid);

    BlockMap::iterator it = mapBlockIndex.find(hash);
    if (it != mapBlockIndex.end() && it->second->nChainWork > 0)
    {
        // An actually better block was announced.
        if (state->pindexBestKnownBlock == NULL || it->second->nChainWork >= state->pindexBestKnownBlock->nChainWork)
            state->pindexBestKnownBlock = it->second;
    }
    else
    {
        // An unknown block was announced; just assume that the latest one is the best one.
        state->hashLastUnknownBlock = hash;
    }
}

void MarkBlockAsInFlight(NodeId nodeid,
    const uint256 &hash,
    const Consensus::Params &consensusParams,
    CBlockIndex *pindex = NULL)
{
    LOCK(cs_main);
    CNodeState *state = State(nodeid);
    DbgAssert(state != NULL, return );

    // If started then clear the thinblock timer used for preferential downloading
    thindata.ClearThinBlockTimer(hash);

    // BU why mark as received? because this erases it from the inflight list.  Instead we'll check for it
    // BU removed: MarkBlockAsReceived(hash);
    std::map<uint256, std::pair<NodeId, std::list<QueuedBlock>::iterator> >::iterator itInFlight =
        mapBlocksInFlight.find(hash);
    if (itInFlight == mapBlocksInFlight.end()) // If it hasn't already been marked inflight...
    {
        int64_t nNow = GetTimeMicros();
        QueuedBlock newentry = {hash, pindex, nNow, pindex != NULL};
        std::list<QueuedBlock>::iterator it = state->vBlocksInFlight.insert(state->vBlocksInFlight.end(), newentry);
        state->nBlocksInFlight++;
        state->nBlocksInFlightValidHeaders += newentry.fValidatedHeaders;
        if (state->nBlocksInFlight == 1)
        {
            // We're starting a block download (batch) from this peer.
            state->nDownloadingSince = GetTimeMicros();
        }
        if (state->nBlocksInFlightValidHeaders == 1 && pindex != NULL)
        {
            nPeersWithValidatedDownloads++;
        }
        mapBlocksInFlight[hash] = std::make_pair(nodeid, it);
    }
}

// Requires cs_main
bool CanDirectFetch(const Consensus::Params &consensusParams)
{
    return !IsInitialBlockDownload();
    // chainActive.Tip()->GetBlockTime() > GetAdjustedTime() - consensusParams.nPowTargetSpacing * 20;
}

bool GetNodeStateStats(NodeId nodeid, CNodeStateStats &stats)
{
    CNodeRef node(connmgr->FindNodeFromId(nodeid));
    if (!node)
        return false;

    LOCK(cs_main);
    CNodeState *state = State(nodeid);
    if (state == NULL)
        return false;
    stats.nMisbehavior = node->nMisbehavior;
    stats.nSyncHeight = state->pindexBestKnownBlock ? state->pindexBestKnownBlock->nHeight : -1;
    stats.nCommonHeight = state->pindexLastCommonBlock ? state->pindexLastCommonBlock->nHeight : -1;
    BOOST_FOREACH (const QueuedBlock &queue, state->vBlocksInFlight)
    {
        if (queue.pindex)
            stats.vHeightInFlight.push_back(queue.pindex->nHeight);
    }
    return true;
}

void RegisterNodeSignals(CNodeSignals &nodeSignals)
{
    nodeSignals.GetHeight.connect(&GetHeight);
    nodeSignals.ProcessMessages.connect(&ProcessMessages);
    nodeSignals.SendMessages.connect(&SendMessages);
    nodeSignals.InitializeNode.connect(&InitializeNode);
    nodeSignals.FinalizeNode.connect(&FinalizeNode);
}

void UnregisterNodeSignals(CNodeSignals &nodeSignals)
{
    nodeSignals.GetHeight.disconnect(&GetHeight);
    nodeSignals.ProcessMessages.disconnect(&ProcessMessages);
    nodeSignals.SendMessages.disconnect(&SendMessages);
    nodeSignals.InitializeNode.disconnect(&InitializeNode);
    nodeSignals.FinalizeNode.disconnect(&FinalizeNode);
}

CBlockIndex *FindForkInGlobalIndex(const CChain &chain, const CBlockLocator &locator)
{
    // Find the first block the caller has in the main chain
    BOOST_FOREACH (const uint256 &hash, locator.vHave)
    {
        BlockMap::iterator mi = mapBlockIndex.find(hash);
        if (mi != mapBlockIndex.end())
        {
            CBlockIndex *pindex = (*mi).second;
            if (chain.Contains(pindex))
                return pindex;
        }
    }
    return chain.Genesis();
}

CCoinsViewCache *pcoinsTip = NULL;
CBlockTreeDB *pblocktree = NULL;

bool IsFinalTx(const CTransaction &tx, int nBlockHeight, int64_t nBlockTime)
{
    if (tx.nLockTime == 0)
        return true;
    if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
        return true;
    BOOST_FOREACH (const CTxIn &txin, tx.vin)
    {
        if (!(txin.nSequence == CTxIn::SEQUENCE_FINAL))
            return false;
    }
    return true;
}

bool CheckFinalTx(const CTransaction &tx, int flags)
{
    // AssertLockHeld(cs_main); no longer needed, although caller may want to take to ensure chain does not advance

    // By convention a negative value for flags indicates that the
    // current network-enforced consensus rules should be used. In
    // a future soft-fork scenario that would mean checking which
    // rules would be enforced for the next block and setting the
    // appropriate flags. At the present time no soft-forks are
    // scheduled, so no flags are set.
    flags = std::max(flags, 0);

    CBlockIndex *tip = chainActive.Tip();
    // CheckFinalTx() uses chainActive.Height()+1 to evaluate
    // nLockTime because when IsFinalTx() is called within
    // CBlock::AcceptBlock(), the height of the block *being*
    // evaluated is what is used. Thus if we want to know if a
    // transaction can be part of the *next* block, we need to call
    // IsFinalTx() with one more than chainActive.Height().
    const int nBlockHeight = tip ? tip->nHeight + 1 : 0;

    // BIP113 will require that time-locked transactions have nLockTime set to
    // less than the median time of the previous block they're contained in.
    // When the next block is created its previous block will be the current
    // chain tip, so we use that to calculate the median time passed to
    // IsFinalTx() if LOCKTIME_MEDIAN_TIME_PAST is set.
    const int64_t nBlockTime = (flags & LOCKTIME_MEDIAN_TIME_PAST) ? tip->GetMedianTimePast() : GetAdjustedTime();

    return IsFinalTx(tx, nBlockHeight, nBlockTime);
}

/**
 * Calculates the block height and previous block's median time past at
 * which the transaction will be considered final in the context of BIP 68.
 * Also removes from the vector of input heights any entries which did not
 * correspond to sequence locked inputs as they do not affect the calculation.
 */
std::pair<int, int64_t> CalculateSequenceLocks(const CTransaction &tx,
    int flags,
    std::vector<int> *prevHeights,
    const CBlockIndex &block)
{
    assert(prevHeights->size() == tx.vin.size());

    // Will be set to the equivalent height- and time-based nLockTime
    // values that would be necessary to satisfy all relative lock-
    // time constraints given our view of block chain history.
    // The semantics of nLockTime are the last invalid height/time, so
    // use -1 to have the effect of any height or time being valid.
    int nMinHeight = -1;
    int64_t nMinTime = -1;

    // tx.nVersion is signed integer so requires cast to unsigned otherwise
    // we would be doing a signed comparison and half the range of nVersion
    // wouldn't support BIP 68.
    bool fEnforceBIP68 = static_cast<uint32_t>(tx.nVersion) >= 2 && flags & LOCKTIME_VERIFY_SEQUENCE;

    // Do not enforce sequence numbers as a relative lock time
    // unless we have been instructed to
    if (!fEnforceBIP68)
    {
        return std::make_pair(nMinHeight, nMinTime);
    }

    for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++)
    {
        const CTxIn &txin = tx.vin[txinIndex];

        // Sequence numbers with the most significant bit set are not
        // treated as relative lock-times, nor are they given any
        // consensus-enforced meaning at this point.
        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG)
        {
            // The height of this input is not relevant for sequence locks
            (*prevHeights)[txinIndex] = 0;
            continue;
        }

        int nCoinHeight = (*prevHeights)[txinIndex];

        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG)
        {
            int64_t nCoinTime = block.GetAncestor(std::max(nCoinHeight - 1, 0))->GetMedianTimePast();
            // NOTE: Subtract 1 to maintain nLockTime semantics
            // BIP 68 relative lock times have the semantics of calculating
            // the first block or time at which the transaction would be
            // valid. When calculating the effective block time or height
            // for the entire transaction, we switch to using the
            // semantics of nLockTime which is the last invalid block
            // time or height.  Thus we subtract 1 from the calculated
            // time or height.

            // Time-based relative lock-times are measured from the
            // smallest allowed timestamp of the block containing the
            // txout being spent, which is the median time past of the
            // block prior.
            nMinTime = std::max(nMinTime, nCoinTime + (int64_t)((txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK)
                                                                << CTxIn::SEQUENCE_LOCKTIME_GRANULARITY) -
                                              1);
        }
        else
        {
            nMinHeight = std::max(nMinHeight, nCoinHeight + (int)(txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK) - 1);
        }
    }

    return std::make_pair(nMinHeight, nMinTime);
}