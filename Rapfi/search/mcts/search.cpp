/*
 *  Rapfi, a Gomoku/Renju playing engine supporting piskvork protocol.
 *  Copyright (C) 2024  Rapfi developers
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "../../core/iohelper.h"
#include "../../eval/eval.h"
#include "../../eval/evaluator.h"
#include "../../game/movegen.h"
#include "../../game/wincheck.h"
#include "../hashtable.h"
#include "../opening.h"
#include "../searchcommon.h"
#include "../searchconfig.h"
#include "parameter.h"
#include "searcher.h"
#include "searchtesting.h"

#include <algorithm>
#include <atomic>
#include <limits>

using namespace Search;
using namespace Search::MCTS;

namespace SimpleVCF {

struct SearchStack
{
    Pattern4 moveP4[2];
};

/// VCN state of one VCF chain (VCN-03 slice 4). No pass is played inside a VCF, so the level is
/// constant along the chain and is computed once at the entry.
struct VcnContext
{
    VCNLevel level;     ///< The VCN level of every node of the chain
    Color    attacker;  ///< The VCN attacker
    HashKey  keyXor;    ///< Vcn::hashXor(options, level): VCF TT entries of this level only
    /// When set, every attack-side node of the chain adds one here (the work measure of the
    /// VCN pass-test solver, VCN-04). The search itself does not read it.
    uint32_t *nodeCounter = nullptr;
};

// forward declaration
template <Rule Rule, bool VcnOn>
Value vcfsearch(Board            &board,
                SearchStack      *ss,
                int               ply,
                Value             alpha,
                Value             beta,
                Depth             depth,
                const VcnContext &vcn);
template <Rule Rule, bool VcnOn>
Value vcfdefend(Board            &board,
                SearchStack      *ss,
                int               ply,
                Value             alpha,
                Value             beta,
                Depth             depth,
                const VcnContext &vcn);

/// Shared entry of the plain (VcnOn = false) and the VCN (VcnOn = true) VCF search.
template <bool VcnOn>
Value vcfEntry(Rule rule, Board &board, int ply, const VcnContext &vcn)
{
    Value alpha = -VALUE_EVAL_MAX;
    Value beta  = VALUE_EVAL_MAX;
    Depth depth = 0;

    SearchStack  stack[MAX_MOVES + 4];
    SearchStack *ss = stack - ply + 4;
    for (int i : {1, 2, 3, 4}) {
        ss[ply - i].moveP4[BLACK] = NONE;
        ss[ply - i].moveP4[WHITE] = NONE;
    }

    // The VC4 attacker always runs the attack side, as AB's search() does: when the defender has
    // a five, the QVCF picker yields the block, which must itself continue the VCF.
    bool vc4Attacker = false;
    if constexpr (VcnOn)
        vc4Attacker = vcn.level == VCNLevel::VC4 && board.sideToMove() == vcn.attacker;

    if (!vc4Attacker && board.p4Count(~board.sideToMove(), A_FIVE)) {
        switch (rule) {
        case FREESTYLE: return vcfdefend<FREESTYLE, VcnOn>(board, ss, ply, alpha, beta, depth, vcn);
        case STANDARD: return vcfdefend<STANDARD, VcnOn>(board, ss, ply, alpha, beta, depth, vcn);
        case RENJU: return vcfdefend<RENJU, VcnOn>(board, ss, ply, alpha, beta, depth, vcn);
        default: break;
        }
    }
    else {
        switch (rule) {
        case FREESTYLE: return vcfsearch<FREESTYLE, VcnOn>(board, ss, ply, alpha, beta, depth, vcn);
        case STANDARD: return vcfsearch<STANDARD, VcnOn>(board, ss, ply, alpha, beta, depth, vcn);
        case RENJU: return vcfsearch<RENJU, VcnOn>(board, ss, ply, alpha, beta, depth, vcn);
        default: break;
        }
    }

    return VALUE_ZERO;
}

/// The plain VCF of the side to move (ordinary search, VCN off).
Value vcf(Rule rule, Board &board, int ply)
{
    return vcfEntry<false>(rule, board, ply, VcnContext {VCNLevel::None, BLACK, 0});
}

/// The VCF under VCN rules (VCN-03 slice 4), with the same flags and values as AB's
/// vcfsearch/vcfdefend: the VC4 attacker plays fours only (quickWinCheck vcfOnly, whole-board
/// QVCF picker with plain block fours) and has no stand-pat, so a missing VCF returns
/// mated_in(ply + 2); a VC4 defender facing no five wins by passing; a VC5 attacker without a
/// five loses. TT entries live under zobristKey() ^ vcn.keyXor.
Value vcf(Rule rule, Board &board, int ply, const VcnContext &vcn)
{
    return vcfEntry<true>(rule, board, ply, vcn);
}

template <Rule Rule, bool VcnOn>
Value vcfsearch(Board            &board,
                SearchStack      *ss,
                int               ply,
                Value             alpha,
                Value             beta,
                Depth             depth,
                const VcnContext &vcn)
{
    Color                self = board.sideToMove();
    SearchThread        *thisThread = board.thisThread();
    const SearchOptions &options    = thisThread->options();
    Value                bestValue  = -VALUE_INFINITE;
    Value                oldAlpha   = alpha;  // Flag BOUND_EXACT when value above alpha in PVNode
    Pos                  bestMove   = Pos::NONE;
    if (ply > thisThread->selDepth)
        thisThread->selDepth = ply;

    // VCN: decided per node from the side to move (the level is constant along the chain)
    bool vcfOnly = false;
    if constexpr (VcnOn) {
        if (vcn.nodeCounter)
            ++*vcn.nodeCounter;

        const bool isAttacker = self == vcn.attacker;
        vcfOnly               = isAttacker && vcn.level == VCNLevel::VC4;

        // VC5: the attacker must already have a five, otherwise they lose at once (before the
        // draw check, as AB)
        if (isAttacker && vcn.level == VCNLevel::VC5 && Vcn::vc5AttackerLoses(board, self))
            return mated_in(board.ply());
    }

    // Check if the board has been filled or we have reached the max game ply.
    if (board.movesLeft() == 0 || board.nonPassMoveCount() >= options.maxMoves)
        return getDrawValue(board, options, board.ply());

    // Check for immediate winning (the VC4 attacker counts only wins made of fours)
    if (Value value = quickWinCheck(options.rule, board, board.ply(), VALUE_MATE_IN_MAX_PLY, vcfOnly);
        value != VALUE_ZERO) {
        // Do not return mate that longer than maxMoves option
        if (mate_step(value, 0) > options.maxMoves)
            value = getDrawValue(board, options, board.ply());

        return value;
    }

    // Mate distance pruning
    alpha = std::max(mated_in(board.ply()), alpha);
    beta  = std::min(mate_in(board.ply() + 1), beta);
    if (alpha >= beta)
        return alpha;

    // Step 4. Transposition table lookup (VCN results live under their own keys)
    HashKey posKey = board.zobristKey();
    if constexpr (VcnOn)
        posKey ^= vcn.keyXor;
    Value ttValue = VALUE_NONE;
    Value ttEval  = VALUE_NONE;
    bool  ttIsPv  = false;
    Bound ttBound = BOUND_NONE;
    Pos   ttMove  = Pos::NONE;
    int   ttDepth = (int)DEPTH_LOWER_BOUND;
    bool  ttHit   = TT.probe(posKey, ttValue, ttEval, ttIsPv, ttBound, ttMove, ttDepth, 0);

    // Check for an early TT cutoff (for all types of nodes)
    if (ttHit && ttDepth >= depth) {
        if (ttBound & BOUND_LOWER)
            alpha = std::max(alpha, ttValue);
        if (ttBound & BOUND_UPPER)
            beta = std::min(beta, ttValue);
        if (alpha >= beta)
            return ttValue;
    }

    if (vcfOnly) {
        // The VC4 attacker cannot stand pat: without a four they are refuted by a defender pass.
        // Start from that worst case so a missing VCF returns it.
        bestValue = mated_in(board.ply() + 2);
    }
    else {
        // Stand pat for the situation that we do not find a mate
        bestValue = VALUE_ZERO;
        if (bestValue > alpha)
            alpha = bestValue;
    }

    MovePicker mp(Rule,
                  board,
                  MovePicker::ExtraArgs<MovePicker::QVCF> {
                      Pos::NONE,
                      depth,
                      {ss[ply - 2].moveP4[self], ss[ply - 4].moveP4[self]},
                      vcfOnly,
                      vcfOnly});

    while (Pos move = mp()) {
        assert(board.isLegal(move));
        ss[ply].moveP4[BLACK] = board.pattern4(move, BLACK);
        ss[ply].moveP4[WHITE] = board.pattern4(move, WHITE);

        // Make and search the move
        board.move<Rule, Board::MoveType::NO_EVAL>(move);

        // Call defence-side vcf search
        Value value = -vcfdefend<Rule, VcnOn>(board, ss, ply + 1, -beta, -alpha, depth - 1, vcn);

        board.undo<Rule, Board::MoveType::NO_EVAL>();

        // Check for a new best move
        if (value > bestValue) {
            bestValue = value;

            if (value > alpha) {
                bestMove = move;

                if (value < beta)  // Update alpha
                    alpha = value;
                else  // Fail high
                    break;
            }
        }
    }

    // Save TT entry for this position
    TT.store(posKey,
             bestValue,
             VALUE_NONE,
             true,
             bestValue >= beta      ? BOUND_LOWER
             : bestValue > oldAlpha ? BOUND_EXACT
                                    : BOUND_UPPER,
             bestMove,
             (int)std::max(depth, DEPTH_QVCF),
             0);

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);
    return bestValue;
}

template <Rule Rule, bool VcnOn>
Value vcfdefend(Board            &board,
                SearchStack      *ss,
                int               ply,
                Value             alpha,
                Value             beta,
                Depth             depth,
                const VcnContext &vcn)
{
    Color                self = board.sideToMove(), oppo = ~self;
    SearchThread        *thisThread = board.thisThread();
    const SearchOptions &options    = thisThread->options();
    if (ply > thisThread->selDepth)
        thisThread->selDepth = ply;

    // Return evaluation immediately if there is no vcf threat
    if (!board.p4Count(oppo, A_FIVE)) {
        // VCN: the VC4 attacker's last move created no five threat, so the defender wins by
        // passing (the attacker would then be at VC5 without a five). AB's vcfdefend value.
        if constexpr (VcnOn) {
            if (self != vcn.attacker && vcn.level == VCNLevel::VC4)
                return mate_in(board.ply() + 1);
        }
        return VALUE_ZERO;
    }

    // Check if the board has been filled or we have reached the max game ply.
    if (board.movesLeft() == 0 || board.nonPassMoveCount() >= options.maxMoves)
        return getDrawValue(board, options, board.ply());

    // Search the only defence move
    Pos move = board.stateInfo().lastPattern4(oppo, A_FIVE);
    assert(board.pattern4(move, oppo) == A_FIVE);

    Value value;
    if (options.rule == Rule::RENJU && self == BLACK && board.checkForbiddenPoint(move)) {
        // For renju, if black's defence move is a forbidden point, black loses in two steps.
        value = mated_in(board.ply() + 2);
    }
    else {
        ss[ply].moveP4[BLACK] = board.pattern4(move, BLACK);
        ss[ply].moveP4[WHITE] = board.pattern4(move, WHITE);

        HashKey nextKey = board.zobristKeyAfter(move);
        if constexpr (VcnOn)
            nextKey ^= vcn.keyXor;
        TT.prefetch(nextKey);  // pre-make, overlaps the board update
        board.move<Rule, Board::MoveType::NO_EVAL>(move);

        // Call attack-side vcf search
        // Note that we do not reduce depth for vcf defence move.
        value = -vcfsearch<Rule, VcnOn>(board, ss, ply + 1, -beta, -alpha, depth, vcn);

        board.undo<Rule, Board::MoveType::NO_EVAL>();
    }

    assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);
    return value;
}

}  // namespace SimpleVCF

/// The VCN pass test (VCN-04, option G). Under MCTS the VCN defender has no pass edge: a pass is
/// the statement "the attacker's last move carries no threat one level up", so it is answered
/// here, when a defender leaf is first evaluated, by a solver on a position with a null move
/// instead of by the network on a position where the attacker has moved twice.
///
/// Root level VC3: the question is the exact fours-only VCF (SimpleVCF at VC4).
/// Root level VC2: the question is a VC3 (VCT) win of the attacker. Three answers; only NoThreat
/// ends a node (in the defender's favour). Threat and Unknown leave an ordinary node, so a wrong
/// or missing Threat can never create an attacker win.
///
/// No static or shared mutable state except the TT: evaluateNode runs on several threads.
namespace VcnPassTest {

using Search::MCTS::Testing::PassTestResult;

/// Recursion limit of the solver (plies below its root); a line that reaches it is undecided.
constexpr int MaxSolverDepth = 48;

/// Last iteration of the solver: the largest number of quiet (non-four) attacker moves in a line.
constexpr int MaxQuietMoves = 8;

/// TT signature of a cached solver answer. The key is the VC3 key of the position XORed with a
/// salt, so an entry of the VCF search (same TT) lives under another key; the fixed bound, depth
/// and eval tag make an aliased entry of another kind fail the check instead of reading as a
/// proof. Only proven answers are stored, never Unknown.
constexpr HashKey CacheSalt     = 0x6A09E667F3BCC909ULL;
constexpr Value   CacheTag      = Value(0x5A3C);
constexpr int     CacheDepth    = 8;
constexpr Value   CacheThreat   = Value(1);
constexpr Value   CacheNoThreat = Value(-1);

/// Number of cells where `side` makes a four or a five.
inline int fourCellCount(const Board &board, Color side)
{
    return board.p4Count(side, E_BLOCK4) + board.p4Count(side, D_BLOCK4_PLUS)
           + board.p4Count(side, C_BLOCK4_FLEX3) + board.p4Count(side, B_FLEX4)
           + board.p4Count(side, A_FIVE);
}

/// Whether a stone on a cell of class `p4` makes a three (open or blocked) or a four on some
/// line. Classes from K_BLOCK3 up hold a three or a four, except J_FLEX2_2X (two open twos, no
/// three), which sits between them in the enum.
constexpr bool makesThreeOrFour(Pattern4 p4)
{
    return p4 >= K_BLOCK3 && p4 != J_FLEX2_2X;
}

/// Number of cells where `side` makes a three (open or blocked) or a four.
inline int threeOrFourCellCount(const Board &board, Color side)
{
    return board.p4Count(side, K_BLOCK3) + board.p4Count(side, I_BLOCK3_PLUS)
           + board.p4Count(side, H_FLEX3) + board.p4Count(side, G_FLEX3_PLUS)
           + board.p4Count(side, F_FLEX3_2X) + fourCellCount(board, side);
}

/// Renju black: a cell the pattern table marks FORBID may be a legal move that makes a three or
/// a four (a false forbidden point), and whether it is legal can depend on stones elsewhere. The
/// pattern-count shortcuts below are therefore not used while such a cell exists.
inline bool hasPatternForbidCell(const Board &board, Rule rule, Color side)
{
    return rule == Rule::RENJU && side == BLACK && board.p4Count(BLACK, FORBID) > 0;
}

/// Stage 1, the static necessary condition. A VC3 win starts with a move after which the attacker
/// has a four available (a four, or the first move of the VCF the move threatens). That four cell
/// either exists now (class >= E_BLOCK4) or is made by the move, which then puts a third stone
/// into a five-window of that line: a three (makesThreeOrFour). No such cell: no VC3 win.
PassTestResult staticRule(const Board &board, Rule rule, Color attacker)
{
    if (threeOrFourCellCount(board, attacker) > 0 || hasPatternForbidCell(board, rule, attacker))
        return PassTestResult::Unknown;
    return PassTestResult::NoThreat;
}

struct Context
{
    const SearchOptions  &options;
    Color                 attacker;
    SimpleVCF::VcnContext vc3;       ///< VCF of either side inside the solver (level VC3)
    SimpleVCF::VcnContext vc4;       ///< The exact fours-only VCF after a null move
    HashKey               cacheXor;  ///< XORed into the zobrist key of a cached attacker node
    int                   ply;       ///< Tree ply of the leaf (only indexes the VCF stack)
    uint32_t              budget;    ///< Work the solver may do: stones placed plus VCF nodes
    uint32_t              nodes    = 0;
    uint32_t              vcfNodes = 0;      ///< Written by the VCF searches (nodeCounter)
    bool                  aborted  = false;  ///< The budget ran out: the answer is Unknown
    bool                  depthCut = false;  ///< This iteration left a line undecided

    Context(const SearchOptions &options, int ply, uint32_t budget)
        : options(options)
        , attacker(options.vcnMode.attacker)
        , vc3 {VCNLevel::VC3, attacker, Vcn::hashXor(options, VCNLevel::VC3), &vcfNodes}
        , vc4 {VCNLevel::VC4, attacker, Vcn::hashXor(options, VCNLevel::VC4), &vcfNodes}
        , cacheXor(vc3.keyXor ^ CacheSalt)
        , ply(ply)
        , budget(budget)
    {}
    Context(const Context &)            = delete;  // vc3 / vc4 point into this object
    Context &operator=(const Context &) = delete;

    /// Accounts for one stone; false (and aborted) when the budget is used up.
    bool takeNode()
    {
        if (nodes >= budget) {
            aborted = true;
            return false;
        }
        nodes++;
        return true;
    }

    /// Moves the nodes of the VCF searches run since the last call into the work count. A VCF
    /// is never cut short (its result is exact and goes to the TT), so the count can pass the
    /// budget by one VCF search; the next takeNode() then ends the solver.
    void chargeVcf()
    {
        nodes += vcfNodes;
        vcfNodes = 0;
    }
};

inline bool cacheProbe(HashKey key, bool &attackerWins)
{
    Value value, eval;
    bool  isPv;
    Bound bound;
    Pos   move;
    int   depth;
    if (!TT.probe(key, value, eval, isPv, bound, move, depth, 0))
        return false;
    if (bound != BOUND_EXACT || eval != CacheTag || depth != CacheDepth)
        return false;
    if (value != CacheThreat && value != CacheNoThreat)
        return false;
    attackerWins = value == CacheThreat;
    return true;
}

inline void cacheStore(HashKey key, bool attackerWins)
{
    TT.store(key,
             attackerWins ? CacheThreat : CacheNoThreat,
             CacheTag,
             false,
             BOUND_EXACT,
             Pos::NONE,
             CacheDepth,
             0);
}

/// Whether the board is at a point where the game is drawn (no win can be claimed either way).
inline bool drawReached(const Board &board, const SearchOptions &options)
{
    return board.movesLeft() == 0 || board.nonPassMoveCount() >= options.maxMoves;
}

/// The exact answer to "after a null move of the defender (to move), does the attacker have a
/// VCF?": the fours-only, whole-board VCF of the VC4 attacker, which has no stand-pat.
/// @return The VCF value from the attacker's view on the board after the null move (absolute
///     ply): a mate_in() value when the VCF exists, a mated_in() value when it does not.
template <Rule R>
Value vcfAfterNull(Board &board, Context &ctx)
{
    assert(board.sideToMove() != ctx.attacker);
    board.move<R, Board::MoveType::NO_EVAL>(Pos::PASS);
    Value value = SimpleVCF::vcf(R, board, ctx.ply + 1, ctx.vc4);
    board.undo<R, Board::MoveType::NO_EVAL>();
    ctx.chargeVcf();
    return value;
}

/// The attacker moves that have to be looked at, written to `out` (room for MAX_MOVES): a
/// superset of the admissible moves, cut only by conditions that are necessary for admissibility.
///  - The defender threatens a five: only its block (any other move loses to the five).
///  - The attacker has no four cell: a move that threatens a VCF must make the first four of
///    that VCF possible, so it makes a three (makesThreeOrFour).
///  - Otherwise every legal move: a quiet move can make an existing four work.
/// Cells of the whole board are scanned, not only the candidate range.
template <Rule R>
uint32_t collectAttackerMoves(const Board &board, const Context &ctx, Pos *out)
{
    const Color attacker = ctx.attacker, defender = ~attacker;
    const bool  renjuBlack    = R == Rule::RENJU && attacker == BLACK;
    const bool  mustBlockFive = board.p4Count(defender, A_FIVE) > 0;
    const bool  anyMove =
        fourCellCount(board, attacker) > 0 || hasPatternForbidCell(board, R, attacker);

    uint32_t n = 0;
    FOR_EVERY_EMPTY_POS(&board, pos)
    {
        const Pattern4 p4 = board.pattern4(pos, attacker);
        if (mustBlockFive) {
            if (board.pattern4(pos, defender) != A_FIVE)
                continue;
        }
        else if (!anyMove && !makesThreeOrFour(p4))
            continue;
        if (renjuBlack && p4 == FORBID && board.checkForbiddenPoint(pos))
            continue;
        out[n++] = pos;
    }
    return n;
}

/// Whether the attacker's last move (already on the board, defender to move) is admissible at
/// VC3: it is a four, or the attacker has a VCF once the defender passes. Exact.
template <Rule R>
bool lastMoveAdmissible(Board &board, Context &ctx)
{
    const Color attacker = ctx.attacker, defender = ~attacker;
    if (board.p4Count(attacker, A_FIVE))
        return true;  // a four: the defender has to block
    if (board.p4Count(defender, A_FIVE))
        return false;  // the defender's five was not blocked
    // A VCF starts with a four (cheap filter; not usable next to a pattern-forbidden cell)
    if (fourCellCount(board, attacker) == 0 && !hasPatternForbidCell(board, R, attacker))
        return false;
    return vcfAfterNull<R>(board, ctx) >= VALUE_MATE_IN_MAX_PLY;
}

/// Result of a solver node, from the attacker's view.
enum class Outcome : uint8_t {
    NoWin,    ///< Proven: the attacker has no forced win here
    Win,      ///< Proven: the attacker has a forced win here
    Unknown,  ///< Not decided at this depth (ctx.depthCut) or the budget ran out (ctx.aborted)
};

template <Rule R>
Outcome defenderLoses(Board &board, Context &ctx, int quietLeft, int depth);

/// Attacker to move at VC3: is there a forced win? Each attacker move must be admissible, and
/// every defender reply to it must lose. NoWin is a proof, so the move loop looks at every move
/// of collectAttackerMoves().
/// @param quietLeft How many more non-four attacker moves this line may hold (the iteration
///     depth). Fours are always followed: their reply is forced.
/// @param depth Plies below the solver root (bounds the recursion).
template <Rule R>
Outcome attackerWins(Board &board, Context &ctx, int quietLeft, int depth)
{
    assert(board.sideToMove() == ctx.attacker);

    if (depth >= MaxSolverDepth || drawReached(board, ctx.options)) {
        ctx.depthCut = true;
        return Outcome::Unknown;
    }

    const HashKey key = board.zobristKey() ^ ctx.cacheXor;
    if (bool cached; cacheProbe(key, cached))
        return cached ? Outcome::Win : Outcome::NoWin;

    // Immediate wins and losses. A double three is a win at VC3 (the defender's pass leaves a
    // straight four one level up), so the full check is used, with the beta that admits the
    // four-three and double-three rules under Renju.
    if (Value value = quickWinCheck<R>(board, board.ply(), VALUE_MATE_IN_MAX_PLY, false);
        value != VALUE_ZERO) {
        const bool wins = value > VALUE_ZERO;
        cacheStore(key, wins);
        return wins ? Outcome::Win : Outcome::NoWin;
    }

    // A VCF is a win at every level. When the defender has a five this runs the forced block and
    // the defender's own VCF after it: negative when the block is forbidden or that VCF wins.
    const Value vcfValue = SimpleVCF::vcf(R, board, ctx.ply, ctx.vc3);
    ctx.chargeVcf();
    if (vcfValue >= VALUE_MATE_IN_MAX_PLY || vcfValue <= VALUE_MATED_IN_MAX_PLY) {
        const bool wins = vcfValue > VALUE_ZERO;
        cacheStore(key, wins);
        return wins ? Outcome::Win : Outcome::NoWin;
    }

    Pos            moves[MAX_MOVES];
    const uint32_t numMoves = collectAttackerMoves<R>(board, ctx, moves);
    bool           unknown  = false;
    for (uint32_t i = 0; i < numMoves; i++) {
        if (!ctx.takeNode())
            return Outcome::Unknown;

        board.move<R, Board::MoveType::NO_EVAL>(moves[i]);
        Outcome outcome = Outcome::NoWin;
        if (lastMoveAdmissible<R>(board, ctx)) {
            const bool isFour = board.p4Count(ctx.attacker, A_FIVE) > 0;
            if (isFour)
                outcome = defenderLoses<R>(board, ctx, quietLeft, depth + 1);
            else if (quietLeft > 0)
                outcome = defenderLoses<R>(board, ctx, quietLeft - 1, depth + 1);
            else {
                // An admissible quiet move beyond the iteration depth: not decided here
                ctx.depthCut = true;
                outcome      = Outcome::Unknown;
            }
        }
        board.undo<R, Board::MoveType::NO_EVAL>();

        if (ctx.aborted)
            return Outcome::Unknown;
        if (outcome == Outcome::Win) {
            cacheStore(key, true);
            return Outcome::Win;
        }
        unknown |= outcome == Outcome::Unknown;
    }

    if (unknown)
        return Outcome::Unknown;
    cacheStore(key, false);
    return Outcome::NoWin;
}

/// Defender to move after an admissible attacker move: does every reply lose (Win), or does one
/// reply leave the attacker without a win (NoWin)? A pass loses by admissibility, so only stones
/// are tried. The reply lists are the ones the MCTS graph uses for this position (the only block
/// of a five; the defences of an open three plus the defender's own fours; every candidate
/// otherwise), generated without the picker, which would ask the evaluator for a policy on a
/// board it was not told about. A reply list that misses a defence can only turn the answer into
/// Win, which ends no node.
template <Rule R>
Outcome defenderLoses(Board &board, Context &ctx, int quietLeft, int depth)
{
    const Color attacker = ctx.attacker, defender = ~attacker;
    assert(board.sideToMove() == defender);

    if (depth >= MaxSolverDepth || drawReached(board, ctx.options)) {
        ctx.depthCut = true;
        return Outcome::Unknown;
    }

    // The defender's own immediate win refutes the attack; two attacker fives end it
    if (Value value = quickWinCheck<R>(board, board.ply(), VALUE_MATE_IN_MAX_PLY, false);
        value != VALUE_ZERO)
        return value < VALUE_ZERO ? Outcome::Win : Outcome::NoWin;

    ScoredMove  replies[MAX_MOVES];
    ScoredMove *end;
    if (board.p4Count(attacker, A_FIVE)) {
        // One five to block (two or more were handled above)
        end = generate<DEFEND_FIVE>(board, replies);
    }
    else {
        // The defender's VCF wins the game
        const Value vcfValue = SimpleVCF::vcf(R, board, ctx.ply, ctx.vc3);
        ctx.chargeVcf();
        if (vcfValue >= VALUE_MATE_IN_MAX_PLY)
            return Outcome::NoWin;

        if (board.p4Count(attacker, B_FLEX4)) {
            end = generate<DEFEND_FOUR>(board, replies);
            end = (R == Rule::RENJU ? generate<VCF | RULE_RENJU> : generate<VCF>)(board, end);
        }
        else {
            // Blocks of the attacker's strongest cells first: a refutation is found sooner
            end = generate<ALL>(board, replies);
            for (ScoredMove *m = replies; m != end; m++)
                m->score = Score(int(board.pattern4(m->pos, attacker)) * PATTERN4_NB
                                 + int(board.pattern4(m->pos, defender)));
            std::sort(replies, end, ScoredMove::ScoreComparator {});
        }
    }

    bool unknown = false;
    for (ScoredMove *m = replies; m != end; m++) {
        if (R == Rule::RENJU && defender == BLACK && board.checkForbiddenPoint(m->pos))
            continue;
        if (!ctx.takeNode())
            return Outcome::Unknown;

        board.move<R, Board::MoveType::NO_EVAL>(m->pos);
        const Outcome outcome = attackerWins<R>(board, ctx, quietLeft, depth + 1);
        board.undo<R, Board::MoveType::NO_EVAL>();

        if (ctx.aborted)
            return Outcome::Unknown;
        if (outcome == Outcome::NoWin)
            return Outcome::NoWin;  // this reply holds
        unknown |= outcome == Outcome::Unknown;
    }

    // Every reply loses (or there is no legal reply), unless one of them is not decided
    return unknown ? Outcome::Unknown : Outcome::Win;
}

/// Stage 2, the bounded solver: the attacker is to move on `board` at VC3. Iterative deepening
/// on the number of quiet attacker moves per line, so the budget goes to the short refutations
/// first; proven nodes of one iteration are read from the cache by the next.
template <Rule R>
PassTestResult solve(Board &board, Context &ctx)
{
    if (ctx.budget == 0)
        return PassTestResult::Unknown;

    for (int quietMoves = 0; quietMoves <= MaxQuietMoves; quietMoves++) {
        ctx.depthCut          = false;
        const Outcome outcome = attackerWins<R>(board, ctx, quietMoves, 0);
        if (ctx.aborted)
            break;
        if (outcome == Outcome::Win)
            return PassTestResult::Threat;
        if (outcome == Outcome::NoWin)
            return PassTestResult::NoThreat;
        if (!ctx.depthCut)
            break;  // undecided for a reason more depth does not remove
    }
    return PassTestResult::Unknown;
}

PassTestResult
solve(Board &board, const SearchOptions &options, int ply, uint32_t budget, uint32_t *nodesUsed)
{
    Context        ctx(options, ply, budget);
    PassTestResult result = PassTestResult::Unknown;
    switch (options.rule) {
    case FREESTYLE: result = solve<FREESTYLE>(board, ctx); break;
    case STANDARD: result = solve<STANDARD>(board, ctx); break;
    case RENJU: result = solve<RENJU>(board, ctx); break;
    default: break;
    }
    if (nodesUsed)
        *nodesUsed = ctx.nodes;
    return result;
}

#ifdef VCN_PASS_TEST_STATS
/// Diagnostic build only (measurement of the stage 2 budget, VCN-04): outcome counters of the
/// pass test, printed at the end of each search. Not compiled into a normal build.
struct Stats
{
    std::atomic<uint64_t> vc3Tests {0}, vc3NoThreat {0};
    std::atomic<uint64_t> vc2Tests {0}, vc2StaticNoThreat {0};
    std::atomic<uint64_t> solverRuns {0}, solverNoThreat {0}, solverThreat {0}, solverNodes {0};

    void print()
    {
        MESSAGEL("PassTest VC3 tests " << vc3Tests << " noThreat " << vc3NoThreat << " | VC2 tests "
                                       << vc2Tests << " staticNoThreat " << vc2StaticNoThreat
                                       << " | solver runs " << solverRuns << " noThreat "
                                       << solverNoThreat << " threat " << solverThreat
                                       << " unknown " << (solverRuns - solverNoThreat - solverThreat)
                                       << " nodes " << solverNodes);
        vc3Tests = vc3NoThreat = vc2Tests = vc2StaticNoThreat = 0;
        solverRuns = solverNoThreat = solverThreat = solverNodes = 0;
    }
} stats;
    #define PASS_TEST_STAT(statement) statement
#else
    #define PASS_TEST_STAT(statement)
#endif

/// The pass test of a defender leaf: defender to move, no attacker five, `level` the node level.
/// @return The terminal value of the node (the defender, to move, wins by passing) when the
///     attacker has no win one level up after a null move; VALUE_NONE when the node stays an
///     ordinary node (a pass loses, or the answer is unknown).
template <Rule R>
Value defenderPassValue(Board &board, const SearchOptions &options, int ply, VCNLevel level)
{
    assert(board.sideToMove() != options.vcnMode.attacker);
    assert(!board.p4Count(options.vcnMode.attacker, A_FIVE));

    if (level == VCNLevel::VC3) {
        // Exact: the value the pass child had as a VC4 attacker node, negated
        Context ctx(options, ply, 0);
        Value   value = vcfAfterNull<R>(board, ctx);
        PASS_TEST_STAT(stats.vc3Tests++; stats.vc3NoThreat += value <= VALUE_MATED_IN_MAX_PLY);
        return value <= VALUE_MATED_IN_MAX_PLY ? -value : VALUE_NONE;
    }

    if (level == VCNLevel::VC2) {
        PassTestResult result = staticRule(board, R, options.vcnMode.attacker);
        PASS_TEST_STAT(stats.vc2Tests++;
                       stats.vc2StaticNoThreat += result == PassTestResult::NoThreat);
        if (result == PassTestResult::Unknown && SearchCfg.mctsVcnPassTestNodes > 0) {
            Context ctx(options, ply + 1, uint32_t(SearchCfg.mctsVcnPassTestNodes));
            board.move<R, Board::MoveType::NO_EVAL>(Pos::PASS);
            result = solve<R>(board, ctx);
            board.undo<R, Board::MoveType::NO_EVAL>();
            PASS_TEST_STAT(stats.solverRuns++; stats.solverNodes += ctx.nodes;
                           stats.solverNoThreat += result == PassTestResult::NoThreat;
                           stats.solverThreat += result == PassTestResult::Threat);
        }
        // The shortest line of a pass refutation at VC2: pass, a move without a threat, pass,
        // and the VC4 attacker without a four is lost two plies later.
        return result == PassTestResult::NoThreat ? mate_in(board.ply() + 5) : VALUE_NONE;
    }

    return VALUE_NONE;
}

Value defenderPassValue(Board &board, const SearchOptions &options, int ply, VCNLevel level)
{
    switch (options.rule) {
    case FREESTYLE: return defenderPassValue<FREESTYLE>(board, options, ply, level);
    case STANDARD: return defenderPassValue<STANDARD>(board, options, ply, level);
    case RENJU: return defenderPassValue<RENJU>(board, options, ply, level);
    default: return VALUE_NONE;
    }
}

}  // namespace VcnPassTest

namespace {

/// Compute the Cpuct exploration factor for the given parent node visits.
inline float cpuctExplorationFactor(uint64_t parentVisits)
{
    float cpuct = CpuctExploration;
    if (CpuctExplorationLog != 0)
        cpuct += CpuctExplorationLog * std::log(1.0f + parentVisits / CpuctExplorationBase);
    return cpuct * std::sqrt(parentVisits + 1e-2f);
}

/// Compute the initial utility value for unexplored children, considering first play urgency.
template <bool Root>
inline float fpuValue(float parentAvgUtility, float parentRawUtility, float exploredPolicySum)
{
    const float fpuReductionMax = Root ? RootFpuReductionMax : FpuReductionMax;
    const float fpuLossRatio    = Root ? RootFpuLossProp : FpuLossProp;

    float blendWeight      = std::min(1.0f, std::pow(exploredPolicySum, FpuUtilityBlendPow));
    float parentUtilityFPU = blendWeight * parentAvgUtility + (1 - blendWeight) * parentRawUtility;
    float fpu              = parentUtilityFPU - fpuReductionMax * std::sqrt(exploredPolicySum);
    fpu -= (1 + fpu) * fpuLossRatio;
    return fpu;
}

/// Compute PUCT selection value with the given child statistics.
inline float puctSelectionValue(float    childUtility,
                                float    childDraw,
                                float    parentDraw,
                                float    childPolicy,
                                uint64_t childVisits,
                                uint32_t childVirtualVisits,
                                float    cpuctExploration)
{
    float U = cpuctExploration * childPolicy / (1 + childVisits);
    float Q = childUtility;

    // Reduce utility value for drawish child nodes for PUCT selection
    // Encourage exploration for less drawish child nodes
    if (SearchCfg.drawUtilityPenalty != 0)
        Q -= SearchCfg.drawUtilityPenalty * childDraw * (1 - parentDraw);

    // Account for virtual losses
    if (childVirtualVisits > 0)
        Q = (Q * childVisits - childVirtualVisits) / (childVisits + childVirtualVisits);

    return Q + U;
}

/// allocateOrFindNode: allocate a new node if it does not exist in the node table
/// @param searcher The searcher owning the node table and the memory counter
/// @param hash The hash key of the node
/// @return A pair of (the node pointer, whether the node is inserted by myself)
std::pair<Node *, bool> allocateOrFindNode(MCTSSearcher &searcher, HashKey hash)
{
    // Try to find a transposition node with the board's zobrist hash
    Node *node         = searcher.nodeTable->findNode(hash);
    bool  didInsertion = false;

    // Allocate and insert a new child node if we do not find a transposition
    if (!node) {
        std::tie(node, didInsertion) =
            searcher.nodeTable->tryEmplaceNode(hash, searcher.graph.globalNodeAge);
        if (didInsertion)
            searcher.graphMemoryUsed.fetch_add(nodeFootprint(), std::memory_order_relaxed);
    }

    return {node, didInsertion};
}

/// select: select the best child node according to the selection value
/// @param node The node to select child from, must be already expanded
/// @return A pair of (the best child edge pointer, the child node pointer)
///   The child node pointer is nullptr if the edge is unexplored (has zero visit).
///   At root, the edge pointer is nullptr if no edge is in the root move list.
template <bool Root>
std::pair<Edge *, Node *> selectChild(Node &node, const Board &board, bool graphClosed)
{
    assert(!node.isLeaf());
    SearchThread *thisThread = board.thisThread();

    uint64_t parentVisits     = node.getVisits();
    float    parentDraw       = node.getD();
    float    cpuctExploration = cpuctExplorationFactor(parentVisits);

    // Apply dynamic cpuct scaling based on parent utility variance if needed
    if (CpuctUtilityStdevScale > 0) {
        float parentUtilityVar = node.getQVar(CpuctUtilityVarPrior, CpuctUtilityVarPriorWeight);
        float parentUtilityStdevProp = std::sqrt(parentUtilityVar / CpuctUtilityVarPrior);
        float parentUtilityStdevFactor =
            1.0f + CpuctUtilityStdevScale * (parentUtilityStdevProp - 1.0f);
        cpuctExploration *= parentUtilityStdevFactor;
    }

    float bestSelectionValue = -std::numeric_limits<float>::infinity();
    Edge *bestEdge           = nullptr;
    Node *bestNode           = nullptr;
    float exploredPolicySum  = 0.0f;

    // Iterate through all expanded children to find the best selection value
    EdgeArray &edges = *node.getEdges();
    uint32_t   edgeIndex;
    for (edgeIndex = 0; edgeIndex < edges.numEdges; edgeIndex++) {
        Edge &childEdge = edges[edgeIndex];
        Pos   move      = childEdge.getMove();

        // Skip the edge if this move is not in the root move list
        if constexpr (Root) {
            auto rm = std::find(thisThread->rootMoves.begin(), thisThread->rootMoves.end(), move);
            if (rm == thisThread->rootMoves.end())
                continue;
        }

        // Get the child node of this edge
        Node *childNode = childEdge.getChild();
        // If this edge is not expanded, then the following edges must be unexpanded as well
        if (!childNode)
            break;

        // Accumulated explored policy sum
        float childPolicy = childEdge.getP();
        exploredPolicySum += childPolicy;

        // Compute selection value and update the best selection value
        uint64_t childVisits        = childEdge.getVisits();
        uint32_t childVirtualVisits = childNode->getVirtualVisits();
        float    childUtility       = -childNode->getQ();
        float    childDraw          = childNode->getD();
        float    selectionValue     = puctSelectionValue(childUtility,
                                                  childDraw,
                                                  parentDraw,
                                                  childPolicy,
                                                  childVisits,
                                                  childVirtualVisits,
                                                  cpuctExploration);
        if (selectionValue > bestSelectionValue) {
            bestSelectionValue = selectionValue;
            bestEdge           = &childEdge;
            bestNode           = childNode;
        }
    }

    // Compute selection value of the first unexplored child (which will have the highest
    // policy among the rest unexplored children)
    // A closed graph (SEARCH-03) offers no unexplored edge: the playout stays in explored nodes.
    if (edgeIndex < edges.numEdges && !graphClosed) {
        float fpuUtility = fpuValue<Root>(node.getQ(), node.getEvalUtility(), exploredPolicySum);

        Edge    &childEdge          = edges[edgeIndex];
        float    childPolicy        = childEdge.getP();
        uint64_t childVisits        = 0;  // Unexplored edge must has zero edge visit
        uint32_t childVirtualVisits = 0;  // Unexplored edge must has zero virtual visit
        float    selectionValue     = puctSelectionValue(fpuUtility,
                                                  parentDraw,
                                                  parentDraw,
                                                  childPolicy,
                                                  childVisits,
                                                  childVirtualVisits,
                                                  cpuctExploration);
        if (selectionValue > bestSelectionValue) {
            bestSelectionValue = selectionValue;
            bestEdge           = &childEdge;
            bestNode           = nullptr;  // No child node
        }
    }

    // At root, every edge may be excluded from the root move list (e.g. when
    // all generated moves are blocked), in which case no edge is selectable
    // and a null edge is returned. For non-root nodes the edge is non-null.
    // A closed graph (SEARCH-03) may leave a non-root node without any explored edge.
    assert(Root || graphClosed || bestEdge);
    return {bestEdge, bestNode};
}

/// Generates the moves of a non-root node with their normalised policies, before the edge cut.
/// Shared by expandNode<false> and widenNode, so a widen regenerates every kept edge (SEARCH-10).
/// The list is the picker's in every mode. In VCN mode the defender has no pass edge (VCN-04):
/// its pass is a test made when the node is first evaluated (VcnPassTest), so every node of the
/// graph is reached by stones only and no leaf is valued with an extra attacker move.
/// @return The number of moves written to moveList / policyList.
uint32_t
generateNodeMoves(const Board &board, const SearchOptions &options, Pos *moveList, float *policyList)
{
    MovePicker mp(options.rule,
                  board,
                  MovePicker::ExtraArgs<MovePicker::MAIN> {
                      Pos::NONE,
                      {},
                      true,
                      PolicyTemperature,
                  });

    uint32_t numMoves = 0;
    while (Pos move = mp()) {
        assert(move != Pos::PASS);
        assert(mp.hasPolicyScore());
        assert(mp.hasNormalizedPolicy());
        moveList[numMoves]   = move;
        policyList[numMoves] = mp.curMoveNormalizePolicy();
        numMoves++;
    }
    return numMoves;
}

/// expand: generate edges and evaluate the policy of this node
/// @return Whether this node has no valid move, which means this node is a terminal node.
template <bool Root = false>
bool expandNode(Node &node, const SearchOptions &options, const Board &board, int ply)
{
    MCTSSearcher &searcher = static_cast<MCTSSearcher &>(*board.thisThread()->engine.searcher());

    if constexpr (Root) {
        MovePicker mp(options.rule,
                      board,
                      MovePicker::ExtraArgs<MovePicker::ROOT> {
                          true,
                          RootPolicyTemperature,
                      });
        bool       noValidMove = node.createEdges(mp, &searcher.graphMemoryUsed);
        assert(!node.isLeaf());
        assert(!noValidMove);
        return false;
    }
    else {
        Pos      moveList[MAX_MOVES];
        float    policyList[MAX_MOVES];
        uint32_t numMoves = generateNodeMoves(board, options, moveList, policyList);

        EdgeLimits limits {SearchCfg.mctsEdgePolicyMass,
                           static_cast<uint32_t>(std::max(SearchCfg.mctsMinEdges, 1))};
        bool       noValidMove =
            node.createEdgesFrom(moveList, policyList, numMoves, limits, &searcher.graphMemoryUsed);
        if (noValidMove) {
            Value terminalValue = board.p4Count(~board.sideToMove(), A_FIVE)
                                      ? mated_in(board.ply() + 2)
                                      : mated_in(board.ply() + 4);

            // Do not return mate that longer than maxMoves option
            if (std::abs(terminalValue) >= VALUE_MATE_IN_MAX_PLY) {
                if (mate_step(terminalValue, 0) > options.maxMoves)
                    terminalValue = getDrawValue(board, options, board.ply());
            }

            // Multiple threads may concurrently discover that this leaf has
            // no valid move; the transition to a terminal node is one-shot
            // and preserves the visits the node has already accumulated.
            node.tryMarkTerminal(terminalValue);
        }
        else {
            assert(!node.isLeaf());
        }
        return noValidMove;
    }
}

/// evaluate: evaluate the value of this node and make the first visit
template <bool Root = false>
void evaluateNode(Node &node, const SearchOptions &options, Board &board, int ply)
{
    SearchThread *thisThread = board.thisThread();

    if (!Root) {
        if (ply > thisThread->selDepth)
            thisThread->selDepth = ply;

        // VCN mode (VCN-03 slice 4): the leaf rules of AB, in AB's order. The level comes from the
        // defender passes since the root (slice 2); the graph holds no pass (VCN-04), so it is
        // the root level at every node. VCN off reads nothing here.
        const bool vcnOn             = options.vcnMode.enabled();
        const Color vcnAttacker      = options.vcnMode.attacker;
        VCNLevel   vcnLevel          = VCNLevel::None;
        bool       vcnAttackerToMove = false;
        if (vcnOn) {
            MCTSSearcher &searcher =
                static_cast<MCTSSearcher &>(*thisThread->engine.searcher());
            vcnLevel = mctsNodeVcnLevel(board, options, searcher.scratch.rootDefenderPasses);
            vcnAttackerToMove = board.sideToMove() == vcnAttacker;

            // 1. VC5: the attacker must already have a five, otherwise they lose at once. Before
            // the draw check, so a full board does not turn a lost attacker into a draw.
            if (vcnAttackerToMove && vcnLevel == VCNLevel::VC5
                && Vcn::vc5AttackerLoses(board, vcnAttacker)) {
                node.setTerminal(mated_in(board.ply()));
                return;
            }
        }

        // 2. Check if the board has been filled or we have reached the max game ply.
        if (board.movesLeft() == 0 || board.nonPassMoveCount() >= options.maxMoves) {
            Value value = getDrawValue(board, options, board.ply());
            node.setTerminal(value);
            return;
        }

        // 3. Check for immediate winning (the VC4 attacker counts only wins made of fours)
        const bool vcfOnly = vcnAttackerToMove && vcnLevel == VCNLevel::VC4;
        if (Value value =
                quickWinCheck(options.rule, board, board.ply(), VALUE_MATE_IN_MAX_PLY, vcfOnly);
            value != VALUE_ZERO) {
            // Do not return mate that longer than maxMoves option
            if (mate_step(value, 0) > options.maxMoves)
                value = getDrawValue(board, options, board.ply());

            node.setTerminal(value);
            return;
        }

        if (vcnOn) {
            const SimpleVCF::VcnContext vcn {vcnLevel,
                                             vcnAttacker,
                                             Vcn::hashXor(options, vcnLevel)};

            if (vcnLevel == VCNLevel::VC4) {
                // 4. VC4 defender and the attacker made no five threat: the defender wins by
                // passing (the attacker would be at VC5 without a five).
                if (!vcnAttackerToMove && !board.p4Count(vcnAttacker, A_FIVE)) {
                    node.setTerminal(mate_in(board.ply() + 1));
                    return;
                }

                // 5. VC4 attacker: always solved here. The fours-only VCF has no stand-pat and
                // returns mated_in(ply + 2) when there is no VCF; never the network value.
                if (vcnAttackerToMove) {
                    node.setTerminal(SimpleVCF::vcf(options.rule, board, ply, vcn));
                    return;
                }
            }

            // 6. Otherwise (VC2/VC3, a VC4 defender facing a five, VC5 defender): the VCF under
            // VCN rules, then the network value.
            if (Value value = SimpleVCF::vcf(options.rule, board, ply, vcn); value != VALUE_ZERO) {
                node.setTerminal(value);
                return;
            }

            // 7. The pass test (VCN-04): a VC2 / VC3 defender who faces no five and has no win
            // of its own. There is no pass edge; instead, after a null move, does the attacker
            // win one level up? If not, the defender wins by passing and the node ends here.
            // Otherwise (a pass loses, or the VC2 answer is unknown) it is an ordinary node.
            if (!vcnAttackerToMove && !board.p4Count(vcnAttacker, A_FIVE)
                && (vcnLevel == VCNLevel::VC2 || vcnLevel == VCNLevel::VC3)) {
                if (Value value = VcnPassTest::defenderPassValue(board, options, ply, vcnLevel);
                    value != VALUE_NONE) {
                    // Do not return mate that longer than maxMoves option
                    if (mate_step(value, 0) > options.maxMoves)
                        value = getDrawValue(board, options, board.ply());

                    node.setTerminal(value);
                    return;
                }
            }
        }
        // Search VCF
        else if (Value value = SimpleVCF::vcf(options.rule, board, ply); value != VALUE_ZERO) {
            node.setTerminal(value);
            return;
        }
    }

    // Evaluate value for new node that has not been visited
    Evaluation::ValueType v = Evaluation::computeEvaluatorValue(board);
    node.setNonTerminal(v.winLossRate(), v.draw());

    // If ExpandWhenFirstEvaluate mode is enabled, we expand the node immediately
    if (SearchCfg.expandWhenFirstEvaluate)
        expandNode<Root>(node, options, board, ply);
}

/// widen: replace the cut edge array of a node whose every kept reply is a proven loss with one
/// holding every generated move, so that the node can be proven lost (SEARCH-10).
/// @param node A non-root node whose updateStats() reported that a widen is needed.
/// @param board The board state of this node.
void widenNode(Node &node, const SearchOptions &options, const Board &board)
{
    MCTSSearcher &searcher = static_cast<MCTSSearcher &>(*board.thisThread()->engine.searcher());

    // Another thread may have widened the node already; a closed graph takes no new edge array.
    EdgeArray *oldEdges = node.getEdges();
    if (!oldEdges || !oldEdges->edgesCut() || searcher.graphClosed())
        return;

    // The same move generation as expandNode<false>, without the cut
    Pos      moveList[MAX_MOVES];
    float    policyList[MAX_MOVES];
    uint32_t numMoves = generateNodeMoves(board, options, moveList, policyList);

#ifndef NDEBUG
    // The board is at this node's position, so every kept move must be regenerated.
    for (uint32_t i = 0; i < oldEdges->numEdges; i++)
        assert(std::find(moveList, moveList + numMoves, (*oldEdges)[i].getMove())
               != moveList + numMoves);
#endif

    node.replaceEdgesWithFull(oldEdges,
                              moveList,
                              policyList,
                              numMoves,
                              &searcher.graphMemoryUsed,
                              searcher.retiredEdgeArrays);
}

/// select and backpropagate: select the best child node and backpropagate the statistics
/// @param node The node to search, must been already allocated.
/// @param board The board state of this node. The board's hash must be equal to the node's.
/// @param ply The current search ply. Root node is zero.
/// @param visits The number of new visits for this playout.
/// @return The number of actual new visits added to this node.
template <bool Root = false>
uint32_t searchNode(Node &node, Board &board, int ply, uint32_t newVisits)
{
    SearchThread  *thisThread = board.thisThread();
    SearchOptions &options    = thisThread->options();
    MCTSSearcher  &searcher   = static_cast<MCTSSearcher &>(*thisThread->engine.searcher());
    assert(node.getHash() == mctsNodeKey(board, options, searcher.scratch.rootDefenderPasses));

    // Discard visits in this node if it is unevaluated
    uint64_t parentVisits = node.getVisits();
    if (parentVisits == 0)
        return 0;

    if (Root)
        thisThread->selDepth = 0;

    // Cap new visits so that we dont do too much at one time
    // The float-to-integer conversion only happens below newVisits, so it never overflows.
    if (float visitsCap = parentVisits * MaxNewVisitsProp; visitsCap < float(newVisits))
        newVisits = std::min(newVisits, uint32_t(visitsCap) + 1);

    // Return directly if this node is a terminal node and not at root
    if (!Root && node.isTerminal()) {
        node.incrementVisits(newVisits);
        return newVisits;
    }

    // A closed graph (SEARCH-03) takes no new nodes or edge arrays. Sampled once per call.
    const bool graphClosed = searcher.graphClosed();

    // Delay expansion of rarely visited leaves: credit the visit with the static value, the
    // same way a terminal node is visited (SEARCH-02). A closed graph expands no leaf either.
    if (!Root && node.isLeaf()
        && (graphClosed || !readyToExpand(parentVisits, SearchCfg.mctsExpandMinVisits))) {
        node.incrementVisits(newVisits);
        return newVisits;
    }

    // Make sure the parent node is expanded before we select a child
    if (node.isLeaf()) {
        bool noValidMove = expandNode<Root>(node, options, board, ply);

        // If we found that there is no valid move, we mark this node as terminal
        // node the finish this visit.
        if (noValidMove) {
            node.incrementVisits(newVisits);
            return newVisits;
        }
    }

    bool     stopThisPlayout = false;
    uint32_t actualNewVisits = 0;
    while (!stopThisPlayout && newVisits > 0) {
        // Set when updateStats() finds every kept reply proven lost (SEARCH-10)
        bool widenNeeded = false;

        // Select the best edge to explore
        auto [childEdge, childNode] = selectChild<Root>(node, board, graphClosed);

        // When no root edge is in the root move list, no playout can make
        // any progress, so stop the search gracefully and let the bestmove
        // selection fall back to the root move list.
        if constexpr (Root) {
            if (!childEdge) {
                thisThread->engine.stopThinking();
                break;
            }
        }
        else if (!childEdge) {
            // Closed graph and no explored child here (SEARCH-03): end the playout. Nothing
            // was visited below this node yet, so credit the visit with its static value.
            assert(graphClosed);
            if (actualNewVisits == 0) {
                node.incrementVisits(newVisits);
                return newVisits;
            }
            break;
        }

        // Make the move to reach the child node
        Pos move = childEdge->getMove();
        board.move(options.rule, move);

        // Reaching a leaf node, expand it
        bool allocatedNode = false;
        if (!childNode) {
            HashKey hash = mctsNodeKey(board, options, searcher.scratch.rootDefenderPasses);
            std::tie(childNode, allocatedNode) = allocateOrFindNode(searcher, hash);

            // Remember this child node in the edge
            childEdge->setChild(childNode);
        }

        // Evaluate the new child node if we are the one who really allocated the node
        if (allocatedNode) {
            // Mark that we are now starting to visit this node
            node.beginVisit(1);
            evaluateNode(*childNode, options, board, ply + 1);

            // Increment child edge visit count
            childEdge->addVisits(1);
            widenNeeded = node.updateStats();
            node.finishVisit(1, 1);
            actualNewVisits++;
            newVisits--;
        }
        else {
            // When transposition happens, we stop the playout if the child node has been
            // visited more times than the parent node. Only continue the playout if the
            // child node has been visited less times than the edge visits, or the absolute
            // child node visits is less than the given threshold.
            uint64_t childEdgeVisits = childEdge->getVisits();
            uint64_t childNodeVisits = childNode->getVisits();
            if (childEdgeVisits >= childNodeVisits
                || childNodeVisits < MinTranspositionSkipVisits) {
                node.beginVisit(newVisits);
                uint32_t actualChildNewVisits = searchNode(*childNode, board, ply + 1, newVisits);
                assert(actualChildNewVisits <= newVisits);

                if (actualChildNewVisits > 0) {
                    childEdge->addVisits(actualChildNewVisits);
                    widenNeeded = node.updateStats();
                    actualNewVisits += actualChildNewVisits;
                }
                // Discard this playout if we can not make new visits to the best child,
                // since some other thread is evaluating it
                else
                    stopThisPlayout = true;

                node.finishVisit(newVisits, actualChildNewVisits);
                newVisits -= actualChildNewVisits;
            }
            else {
                // Increment edge visits without search the node
                childEdge->addVisits(1);
                widenNeeded = node.updateStats();
                node.incrementVisits(1);
                actualNewVisits++;
                newVisits--;
            }
        }

        // Undo the move
        board.undo(options.rule);

        // Add the cut moves back once the board is at this node again (SEARCH-10). The root is
        // never cut, so it never widens.
        if constexpr (!Root) {
            if (widenNeeded)
                widenNode(node, options, board);
        }

        // Record root move's seldepth
        if constexpr (Root) {
            auto rmIt = std::find(thisThread->rootMoves.begin(), thisThread->rootMoves.end(), move);
            if (rmIt != thisThread->rootMoves.end()) {
                RootMove &rm = *rmIt;
                rm.selDepth  = std::max(rm.selDepth, thisThread->selDepth);
            }
        }
    }

    return actualNewVisits;
}

/// Bestmove-selection pass 1: fill edgeIndices/selectionValues from all
/// expanded children of the node, accumulating their bound stats.
/// @return Tuple of (best index, best selection value, accumulated max bound).
std::tuple<int, float, ValueBound> computeSelectionValues(const Node            &node,
                                                          std::vector<uint32_t> &edgeIndices,
                                                          std::vector<float>    &selectionValues)
{
    int        bestmoveIndex          = -1;
    float      bestmoveSelectionValue = std::numeric_limits<float>::lowest();
    ValueBound maxBound {-VALUE_INFINITE};

    const EdgeArray &edges = *node.getEdges();
    for (uint32_t edgeIndex = 0; edgeIndex < edges.numEdges; edgeIndex++) {
        const Edge &childEdge = edges[edgeIndex];
        // Only select from expanded children in the first pass
        Node *childNode = childEdge.getChild();
        if (!childNode)
            continue;

        float childPolicy    = childEdge.getP();
        float selectionValue = 2.0f * childPolicy;

        // A linked child may still be mid-evaluation on another thread (the
        // child pointer is published before its first visit completes), so
        // zero node visits is a legal transient state here; the edge-visit
        // guard below keeps such children out of the visit weighting.
        uint64_t childVisits = childEdge.getVisits();
        // Skip zero visits children
        if (childVisits > 0) {
            // Discount the child visits by 1 and add small weight on raw policy
            float visitWeight = childVisits * float(childVisits - 1) / float(childVisits);
            selectionValue += visitWeight;
        }

        // Find the best edge with the highest selection value
        if (selectionValue > bestmoveSelectionValue) {
            bestmoveIndex          = edgeIndices.size();
            bestmoveSelectionValue = selectionValue;
        }
        edgeIndices.push_back(edgeIndex);
        selectionValues.push_back(selectionValue);

        // Update bound stats
        maxBound |= childNode->getBound();
    }

    return {bestmoveIndex, bestmoveSelectionValue, maxBound};
}

/// Bestmove-selection pass 2 (UseLCBForBestmoveSelection): fill lcbValues and
/// give the best-LCB child a selection value bonus.
/// @return The updated best index.
int applyLCBSelection(const Node                  &node,
                      const std::vector<uint32_t> &edgeIndices,
                      std::vector<float>          &selectionValues,
                      std::vector<float>          &lcbValues,
                      int                          bestmoveIndex,
                      float                        bestmoveSelectionValue)
{
    const EdgeArray &edges = *node.getEdges();

    int   bestLCBIndex = -1;
    float bestLCBValue = std::numeric_limits<float>::lowest();

    std::vector<float> lcbRadius;

    // Compute LCB values for all selectable children and find highest LCB value
    for (size_t i = 0; i < edgeIndices.size(); i++) {
        uint32_t    edgeIndex = edgeIndices[i];
        const Edge &childEdge = edges[edgeIndex];
        Node       *childNode = childEdge.getChild();
        assert(childNode);

        float utilityMean = -childNode->getQ();

        // Only compute LCB for children with enough visits
        uint64_t childVisits = childNode->getVisits();
        if (childVisits < 2) {
            lcbValues.push_back(utilityMean - 1e5f);
            lcbRadius.push_back(1e4f);
        }
        else {
            float utilityVar = childNode->getQVar();
            float radius     = LCBStdevs * std::sqrt(utilityVar / childVisits);
            lcbValues.push_back(utilityMean - radius);
            lcbRadius.push_back(radius);

            if (selectionValues[i] > 0
                && selectionValues[i] >= LCBMinVisitProp * bestmoveSelectionValue
                && lcbValues[i] > bestLCBValue) {
                bestLCBIndex = i;
                bestLCBValue = lcbValues[i];
            }
        }
    }

    // Best LCB child gets a bonus on selection value
    if (bestLCBIndex >= 0) {
        float bestLCBSelectionValue = selectionValues[bestLCBIndex];
        for (size_t i = 0; i < edgeIndices.size(); i++) {
            if (i == bestLCBIndex)
                continue;

            // Compute how much the best LCB value is better than the current child
            float lcbBonus = bestLCBValue - lcbValues[i];
            if (lcbBonus <= 0)
                continue;

            // Compute how many times larger the radius can be before this LCB value is better
            float gain   = std::min(lcbBonus / lcbRadius[i] + 1.0f, 5.0f);
            float lbound = gain * gain * selectionValues[i];
            if (lbound > bestLCBSelectionValue)
                bestLCBSelectionValue = lbound;
        }
        selectionValues[bestLCBIndex] = bestLCBSelectionValue;
        bestmoveIndex                 = bestLCBIndex;
    }

    return bestmoveIndex;
}

/// Bestmove-selection pass 3 (a proven mate exists below): reweight children to
/// prefer the shortest proven mate.
/// @param maxBound The bound accumulated by computeSelectionValues.
/// @return The updated best index.
int overrideBestByMateBounds(const Node                  &node,
                             ValueBound                   maxBound,
                             const std::vector<uint32_t> &edgeIndices,
                             std::vector<float>          &selectionValues)
{
    const EdgeArray &edges = *node.getEdges();

    int   bestmoveIndex          = -1;
    float bestmoveSelectionValue = std::numeric_limits<float>::lowest();

    // Make best move the one with the maximum lower bound
    for (size_t i = 0; i < edgeIndices.size(); i++) {
        uint32_t    edgeIndex = edgeIndices[i];
        const Edge &childEdge = edges[edgeIndex];
        Node       *childNode = childEdge.getChild();
        assert(childNode);

        Value childLowerBound = static_cast<Value>(-childNode->getBound().upper);
        if (childLowerBound < VALUE_MATE_IN_MAX_PLY)  // Downweight non-proven mate moves
            selectionValues[i] *= 1e-9f;
        else if (childLowerBound < maxBound.lower)  // Downweight non-shorted mate moves
            selectionValues[i] *= 1e-3f * (1 + maxBound.lower - childLowerBound);

        // Find the best edge with the highest selection value
        if (selectionValues[i] > bestmoveSelectionValue) {
            bestmoveIndex          = i;
            bestmoveSelectionValue = selectionValues[i];
        }
    }

    return bestmoveIndex;
}

/// Bestmove-selection pass 4 (no expanded children): select from unexplored
/// children by their raw policy prior.
/// @return The updated best index.
int selectByRawPolicy(const Node            &node,
                      std::vector<uint32_t> &edgeIndices,
                      std::vector<float>    &selectionValues)
{
    const EdgeArray &edges = *node.getEdges();

    int   bestmoveIndex          = -1;
    float bestmoveSelectionValue = std::numeric_limits<float>::lowest();

    for (uint32_t edgeIndex = 0; edgeIndex < edges.numEdges; edgeIndex++) {
        const Edge &childEdge   = edges[edgeIndex];
        float       childPolicy = childEdge.getP();
        if (childPolicy > bestmoveSelectionValue) {
            bestmoveIndex          = edgeIndices.size();
            bestmoveSelectionValue = childPolicy;
        }
        edgeIndices.push_back(edgeIndex);
        selectionValues.push_back(childPolicy);
    }
    assert(!edgeIndices.empty());

    return bestmoveIndex;
}

/// Select best move to play for the given node.
/// @param node The node to compute selection value. Must be expanded.
/// @param edgeIndices[out] The edge indices of selectable children.
/// @param selectionValues[out] The selection values of selectable children.
/// @param lcbValues[out] The lower confidence bound values of selectable children.
///     Only filled when we are using LCB for selection.
/// @param allowDirectPolicyMove If true and the node has no explored children,
///     allow to select from those unexplored children by policy prior directly.
/// @return The index of the best move (with highest selection value) to select.
///     Returns -1 if there is no selectable children.
int selectBestmoveOfChildNode(const Node            &node,
                              std::vector<uint32_t> &edgeIndices,
                              std::vector<float>    &selectionValues,
                              std::vector<float>    &lcbValues,
                              bool                   allowDirectPolicyMove)
{
    assert(!node.isLeaf());
    edgeIndices.clear();
    selectionValues.clear();
    lcbValues.clear();

    auto [bestmoveIndex, bestmoveSelectionValue, maxBound] =
        computeSelectionValues(node, edgeIndices, selectionValues);

    // Compute lower confidence bound values if needed
    if (UseLCBForBestmoveSelection && !edgeIndices.empty())
        bestmoveIndex = applyLCBSelection(node,
                                          edgeIndices,
                                          selectionValues,
                                          lcbValues,
                                          bestmoveIndex,
                                          bestmoveSelectionValue);

    // Select best check mate move if possible
    if (maxBound.lower >= VALUE_MATE_IN_MAX_PLY)
        bestmoveIndex = overrideBestByMateBounds(node, maxBound, edgeIndices, selectionValues);

    // If we have no expanded children for selection, try select by raw policy if allowed
    if (edgeIndices.empty() && allowDirectPolicyMove)
        bestmoveIndex = selectByRawPolicy(node, edgeIndices, selectionValues);

    return bestmoveIndex;
}

/// Extract PV of the given node recursively.
/// @param node The node to extract PV.
/// @param pv[out] The extracted PV will be appended to this array.
/// @param maxDepth Only extract PV within this depth.
void extractPVOfChildNode(const Node &node, std::vector<Pos> &pv, int maxDepth = 100)
{
    const Node           *curNode = &node;
    std::vector<uint32_t> tempEdgeIndices;
    std::vector<float>    tempSelectionValues, tempLCBValues;
    for (int depth = 0; depth < maxDepth; depth++) {
        if (curNode->isLeaf())
            break;

        int bestmoveIndex = selectBestmoveOfChildNode(*curNode,
                                                      tempEdgeIndices,
                                                      tempSelectionValues,
                                                      tempLCBValues,
                                                      true);
        if (bestmoveIndex < 0)
            break;

        uint32_t         bestEdgeIndex = tempEdgeIndices[bestmoveIndex];
        const EdgeArray &edges         = *curNode->getEdges();
        const Edge      &bestEdge      = edges[bestEdgeIndex];
        Pos              bestmove      = bestEdge.getMove();
        pv.push_back(bestmove);

        curNode = bestEdge.getChild();
        if (!curNode)
            break;
    }
}

/// Process all node-table shards cooperatively: each calling thread repeatedly
/// grabs the next unprocessed shard index until none remain. Must be called
/// from a task running on every thread, sharing one counter across threads.
/// The shard's lock is held while f runs.
void processAllShards(NodeTable                                     &nodeTable,
                      std::atomic<size_t>                           &nextShardIndex,
                      const std::function<void(NodeTable::Shard &)> &f)
{
    for (;;) {
        size_t shardIdx = nextShardIndex.fetch_add(1, std::memory_order_relaxed);
        if (shardIdx >= nodeTable.getNumShards())
            return;

        NodeTable::Shard shard = nodeTable.getShardByShardIndex(shardIdx);
        std::unique_lock lock(shard.mutex);
        f(shard);
    }
}

/// Apply a custom function to the node and its children recursively.
/// @param node The node to traverse recursively.
/// @param f The function to apply to the node and its children.
/// @param prng The PRNG to use for permuting the children for multi-thread visit.
/// @param globalNodeAge The global node age to synchronize the visits.
void recursiveApply(Node &node, std::function<void(Node &)> *f, PRNG *prng, uint32_t globalNodeAge)
{
    std::atomic<uint32_t> &nodeAge = node.getAgeRef();
    // If the node's age has been updated, then the node's traversal is done
    if (nodeAge.load(std::memory_order_acquire) == globalNodeAge)
        return;

    if (!node.isLeaf()) {
        EdgeArray &edges = *node.getEdges();

        // Get the edge indices of the children to visit
        std::vector<uint32_t> edgeIndices;
        for (uint32_t edgeIndex = 0; edgeIndex < edges.numEdges; edgeIndex++) {
            Edge &childEdge = edges[edgeIndex];
            Node *childNode = childEdge.getChild();
            if (childNode)
                edgeIndices.push_back(edgeIndex);
        }

        // Shuffle the indices for better multi-thread visit
        if (prng)
            std::shuffle(edgeIndices.begin(), edgeIndices.end(), *prng);

        // Recursively apply the function to the children
        for (uint32_t edgeIndex : edgeIndices) {
            Edge &childEdge = edges[edgeIndex];
            Node *childNode = childEdge.getChild();
            recursiveApply(*childNode, f, prng, globalNodeAge);
        }
    }

    // If we are the one who first set the node age, we call the function
    uint32_t oldAge = nodeAge.exchange(globalNodeAge, std::memory_order_acq_rel);
    if (f && oldAge != globalNodeAge)
        (*f)(node);
}

}  // namespace

float Search::MCTS::Testing::cpuctExplorationFactor(uint64_t parentVisits)
{
    return ::cpuctExplorationFactor(parentVisits);
}

float Search::MCTS::Testing::puctSelectionValue(float    childUtility,
                                                float    childDraw,
                                                float    parentDraw,
                                                float    childPolicy,
                                                uint64_t childVisits,
                                                uint32_t childVirtualVisits,
                                                float    cpuctExploration)
{
    return ::puctSelectionValue(childUtility,
                                childDraw,
                                parentDraw,
                                childPolicy,
                                childVisits,
                                childVirtualVisits,
                                cpuctExploration);
}

MCTSSearcher::MCTSSearcher() : Searcher(false)
{
    graph.root          = nullptr;
    graph.globalNodeAge = 0;
    nodeTable           = std::make_unique<NodeTable>(SearchCfg.numNodeTableShardsPowerOfTwo);
}

void MCTSSearcher::setMemoryLimit(size_t memorySizeKB)
{
    // The argument is the total MCTS search-memory budget: a small capped
    // slice goes to the (shared) VCF TT, the rest becomes the graph budget.
    size_t vcfTTMaxKB   = static_cast<size_t>(std::max(SearchCfg.mctsVCFTTMaxSizeKB, 1));
    size_t vcfTTDivisor = static_cast<size_t>(std::max(SearchCfg.mctsVCFTTBudgetDivisor, 1));

    memoryLimitKB      = memorySizeKB;
    memoryLimitApplied = true;
    size_t vcfTTSizeKB;
    if (memorySizeKB == 0) {
        // No budget set: graph unbounded (keyed off memoryLimitKB == 0),
        // VCF TT at its default maximum.
        vcfTTSizeKB      = vcfTTMaxKB;
        graphMemoryLimit = 0;
        graphBounded     = false;  // searchMain derives a default budget from the free RAM
    }
    else {
        graphBounded   = true;
        vcfTTSizeKB    = std::clamp<size_t>(memorySizeKB / vcfTTDivisor, 1, vcfTTMaxKB);
        vcfTTSizeKB    = std::min(vcfTTSizeKB, memorySizeKB);
        size_t graphKB = memorySizeKB - vcfTTSizeKB;
        // Saturate instead of wrapping on absurd budgets (> 2^54 KiB)
        graphMemoryLimit = graphKB <= std::numeric_limits<size_t>::max() / 1024
                               ? graphKB * 1024
                               : std::numeric_limits<size_t>::max();
    }
    TT.resize(vcfTTSizeKB);
}

void MCTSSearcher::freeRetiredEdgeArrays()
{
    size_t freed = retiredEdgeArrays.freeAll();
    if (freed)
        graphMemoryUsed.fetch_sub(freed, std::memory_order_relaxed);
}

size_t MCTSSearcher::getMemoryLimit() const
{
    return memoryLimitKB;
}

void MCTSSearcher::clear(SearchEngine &pool, bool clearAllMemory)
{
    // Note: previousPosition is deliberately NOT reset here (preserved
    // semantics; see GraphState's lifecycle comment).
    graph.root = nullptr;
    if (!clearAllMemory)
        return;

    graph.globalNodeAge = 0;

    // Clear the node table using all threads, and wait for finish
    pool.main()->runTask([this](SearchThread &th) {
        std::atomic<size_t> numShardsProcessed = 0;
        th.engine.runOnAllThreads(
            [this, &numShardsProcessed](SearchThread &t) {
                processAllShards(*this->nodeTable,
                                 numShardsProcessed,
                                 [this](NodeTable::Shard &shard) {
                                     this->nodeTable->clearShard(shard);
                                 });
            },
            true);
    });
    pool.waitForIdle();
    freeRetiredEdgeArrays();
    graphMemoryUsed.store(0, std::memory_order_relaxed);

    // The shared VCF TT is also search state: clear it so YXHASHCLEAR and
    // YXSHOWHASHUSAGE (which reports TT) agree. Threads are idle here.
    TT.clear();

    // Reset node table num shards if needed
    if (nodeTable->getNumShards() != SearchCfg.numNodeTableShardsPowerOfTwo)
        nodeTable = std::make_unique<NodeTable>(SearchCfg.numNodeTableShardsPowerOfTwo);
}

const RootMove *MCTSSearcher::searchMain(SearchThread &th)
{
    SearchContext &ctx   = th.engine.ctx;
    Board         &board = *th.board;

    // Evaluator must be enabled for MCTS search
    if (!board.evaluator()) {
        FOR_EVERY_EMPTY_POS(th.board, pos)
        {
            ctx.bestMove     = pos;
            ctx.resultAction = ActionType::Move;
            ERRORL("Evaluator is not enabled, cannot use mcts search.");
            ctx.printer.printBestmoveWithoutSearch(th, pos, mated_in(0), 0, nullptr);
            return nullptr;
        }
    }

    // No explicit memory limit: bound the graph by a share of the RAM that is free now
    // (SEARCH-03). Written here, before any worker thread starts.
    if (memoryLimitKB == 0) {
        // No limit was ever set: the VCF TT is still unsized, give it its default size.
        if (!memoryLimitApplied)
            setMemoryLimit(0);
        graphMemoryLimit = deriveDefaultGraphBudget(availablePhysicalMemoryKB(),
                                                    graphMemoryUsed.load(std::memory_order_relaxed),
                                                    SearchCfg.mctsDefaultMemoryPercent);
        graphBounded     = graphMemoryLimit > 0;
        if (graphBounded)
            MESSAGEL("MCTS default graph budget: " << (graphMemoryLimit >> 20) << " MB");
    }

    // Initialize per-search scratch state
    scratch.numSelectableRootMoves = 0;
    scratch.lastOutputNodes        = 0;
    scratch.lastOutputTime         = now();
    // Base of the per-node VCN level (VCN-03 slice 2). Set before setupRootNode, which keys the
    // root through mctsNodeKey, and before the workers start (same happens-before as graph.root).
    scratch.rootDefenderPasses = board.passMoveCountOfSide(~th.options().vcnMode.attacker);

    // Starts worker threads, then starts main thread.
    // Root setup happens-before the workers read graph.root: runOnAllThreads
    // dispatches the tasks only after setupRootNode returns (on this thread),
    // and the task hand-off synchronizes through the thread mutex/condvar.
    ctx.printer.printSearchStarts(th, ctx.timectl);
    setupRootNode(th);  // Setup root node and other stuffs
    th.engine.runOnAllThreads([this](SearchThread &t) { search(t); }, true);

    // Every search thread has returned (runOnAllThreads waits for the workers), so no thread
    // holds a pointer into a replaced edge array any more (SEARCH-10).
    freeRetiredEdgeArrays();

#ifdef VCN_PASS_TEST_STATS
    if (th.options().vcnMode.enabled())
        VcnPassTest::stats.print();
#endif

    // Rank root moves and record best move
    updateRootMovesData(th);
    ctx.printer.printRootMoves(th, ctx.timectl, scratch.numSelectableRootMoves);
    ctx.printer.printMCTSSearchEnds(th, ctx.timectl);  // BRIEF result line (SEARCH-14)

    return &th.rootMoves[0];
}

void MCTSSearcher::search(SearchThread &th)
{
    SearchOptions &options = th.options();
    Board         &board   = *th.board;
    assert(!graph.root->isLeaf());

    // Main search loop
    std::vector<Node *> selectedPath;
    while (!th.engine.isTerminating()) {
        // Stop when the graph memory budget is exhausted
        if (graphBounded && graphMemoryUsed.load(std::memory_order_relaxed) >= graphMemoryLimit)
            break;

        // Stop far below 2^48, where visit counts would carry into the packed policy and
        // terminal-value bits (SEARCH-08). Other counters are bounded by the playouts made since
        // the graph was cleared; reaching 2^48 that way takes over a year at 6M visits/s.
        if (graph.root->getVisits() >= SearchVisitGuard)
            break;

        uint32_t newNumPlayouts = SearchCfg.maxNumVisitsPerPlayout;

        // Cap new number of playouts to the maximum num nodes to visit
        if (options.maxNodes) {
            uint64_t nodesSearched = th.engine.nodesSearched();
            if (nodesSearched >= options.maxNodes)
                break;

            uint64_t maxNodesToVisit = options.maxNodes - nodesSearched;
            if (maxNodesToVisit < newNumPlayouts)
                newNumPlayouts = maxNodesToVisit;
        }

        uint32_t newNumNodes = searchNode<true>(*graph.root, board, 0, newNumPlayouts);
        th.numNodes.fetch_add(newNumNodes, std::memory_order_relaxed);

        if (th.isMainThread()) {
            th.engine.ctx.checkExit(std::max(newNumNodes, 64u));

            bool printRootMoves = false;
            if (SearchCfg.nodesToPrintMCTSRootmoves > 0) {
                uint64_t currentNumNodes = th.engine.nodesSearched();
                uint64_t numElapsedNodes = currentNumNodes - scratch.lastOutputNodes;

                if (numElapsedNodes >= SearchCfg.nodesToPrintMCTSRootmoves) {
                    scratch.lastOutputNodes = currentNumNodes;
                    printRootMoves          = true;
                }
            }
            if (SearchCfg.timeToPrintMCTSRootmoves > 0) {
                Time currentTime = now();
                Time elapsedTime = currentTime - scratch.lastOutputTime;
                if (elapsedTime >= SearchCfg.timeToPrintMCTSRootmoves) {
                    scratch.lastOutputTime = currentTime;
                    printRootMoves         = true;
                }
            }

            if (printRootMoves) {
                updateRootMovesData(th);
                th.engine.ctx.printer.printRootMoves(th,
                                                      th.engine.ctx.timectl,
                                                      scratch.numSelectableRootMoves);
            }

            // A forced root says nothing about the attack under VCN (same exemption as AB's
            // singular-root rule in search/ab/search.cpp; SEARCH-13).
            if (th.rootMoves.size() == 1 && !options.vcnMode.enabled()
                && th.engine.nodesSearched() >= SearchCfg.numNodesAfterSingularRoot)
                th.engine.stopThinking();
        }
    }
}

bool MCTSSearcher::checkTimeupCondition(const TimeControl &timectl)
{
    if (timectl.elapsed() >= timectl.maximum())
        return true;
    if (timectl.checkStop())
        return true;
    return false;
}

void MCTSSearcher::setupRootNode(SearchThread &th)
{
    // Clear the searcher if we have not initialized yet
    if (!nodeTable)
        clear(th.engine, true);

    // Get the current root position
    std::vector<Pos> rootPosition;
    for (int moveIndex = 0; moveIndex < th.board->ply(); moveIndex++) {
        Pos move = th.board->getHistoryMove(moveIndex);
        rootPosition.push_back(move);
    }

    // Recycling must run whenever the graph memory budget is exceeded -
    // clear(false) keeps the table and the counter, so a new/backward/
    // unchanged position must still be able to reclaim the budget below.
    bool overBudget = graphClosed();

    SearchOptions &opts = th.options();

    // The root key also carries the VCN mode (off / level / attacker), VCN-03 slice 2: the same
    // moves searched in another mode must not reuse the previous root.
    const HashKey rootKey = mctsNodeKey(*th.board, opts, scratch.rootDefenderPasses);

    // If the root position has not changed, we do not need to update the root node
    if (graph.root && rootPosition == graph.previousPosition && rootKey == graph.previousRootKey
        && !overBudget)
        return;

    // Initialize the root node to expanded state
    std::tie(graph.root, std::ignore) = allocateOrFindNode(*this, rootKey);
    if (graph.root->getVisits() == 0)
        evaluateNode<true>(*graph.root, opts, *th.board, 0);
    if (graph.root->isLeaf())
        expandNode<true>(*graph.root, opts, *th.board, 0);
    assert(graph.root->getEdges()->numEdges > 0);

    // Garbage collect old nodes (only when we go forward, and not with
    // singular root), or whenever the graph memory budget is exceeded.
    // Re-sample the budget here: root setup above may have allocated the
    // new root and its edges, crossing the limit after the first check.
    overBudget = overBudget || graphClosed();
    if ((rootPosition.size() >= graph.previousPosition.size() && th.rootMoves.size() > 1)
        || overBudget)
        recycleOldNodes(th);

    // Update previous Position and root key
    graph.previousPosition = std::move(rootPosition);
    graph.previousRootKey  = rootKey;
}

void MCTSSearcher::recycleOldNodes(SearchThread &th)
{
    // Retired edge arrays still hold child pointers; they must be gone before a recycle can free
    // the slabs of their children (PERF-04). searchMain frees them after every search.
    assert(retiredEdgeArrays.count() == 0);

    // Increment global node age
    graph.globalNodeAge += 1;

    std::atomic<uint32_t> numReachableNodes = 0;
    std::atomic<uint32_t> numRecycledNodes  = 0;

    std::function<void(Node &)> f = [&](Node &node) {
        numReachableNodes.fetch_add(1, std::memory_order_relaxed);
    };

    // Mark all reachable nodes from the root node
    th.engine.runOnAllThreads(
        [this, &f](SearchThread &t) {
            PRNG prng(Hash::LCHash(t.id));
            recursiveApply(*this->graph.root,
                           &f,
                           t.id ? &prng : nullptr,
                           this->graph.globalNodeAge);
        },
        true);

    // Remove all unreachable nodes
    std::atomic<size_t> numShardsProcessed = 0;

    th.engine.runOnAllThreads(
        [this, &numRecycledNodes, &numShardsProcessed](SearchThread &t) {
            processAllShards(
                *this->nodeTable,
                numShardsProcessed,
                [this, &numRecycledNodes](NodeTable::Shard &shard) {
                    // The predicate runs before the node is destroyed, so its edge array
                    // is still readable for the footprint.
                    size_t numErased = this->nodeTable->eraseNodesIf(shard, [this](Node &node) {
                        if (node.getAgeRef().load(std::memory_order_relaxed)
                            == this->graph.globalNodeAge)
                            return false;
                        size_t footprint = nodeFootprint();
                        if (const EdgeArray *edges = node.getEdges())
                            footprint += edgeArrayFootprint(edges->numEdges);
                        this->graphMemoryUsed.fetch_sub(footprint, std::memory_order_relaxed);
                        return true;
                    });
                    numRecycledNodes.fetch_add(static_cast<uint32_t>(numErased),
                                               std::memory_order_relaxed);
                });
        },
        true);

    MESSAGEL("Reachable nodes: " << numReachableNodes.load()
                                 << ", Recycled nodes: " << numRecycledNodes.load()
                                 << ", Root visit: " << graph.root->getVisits());
}

void MCTSSearcher::updateRootMovesData(SearchThread &th)
{
    assert(graph.root != nullptr);
    assert(!graph.root->isLeaf());

    std::vector<uint32_t> edgeIndices;
    std::vector<float>    selectionValues, lcbValues;
    selectBestmoveOfChildNode(*graph.root, edgeIndices, selectionValues, lcbValues, true);
    uint32_t maxNumRootMovesToPrint =
        std::max<uint32_t>(th.options().multiPV, SearchCfg.maxNonPVRootmovesToPrint);

    for (RootMove &rm : th.rootMoves) {
        rm.selectionValue = std::numeric_limits<float>::lowest();
        rm.previousValue  = rm.value;
        rm.previousPv     = rm.pv;
        rm.pv.resize(1);
    }

    EdgeArray &edges               = *graph.root->getEdges();
    scratch.numSelectableRootMoves = 0;
    for (size_t i = 0; i < edgeIndices.size(); i++) {
        uint32_t edgeIndex = edgeIndices[i];
        Edge    &childEdge = edges[edgeIndex];
        Pos      move      = childEdge.getMove();

        // Skip edges that are not in the root move list
        auto rm = std::find(th.rootMoves.begin(), th.rootMoves.end(), move);
        if (rm == th.rootMoves.end())
            continue;

        // Record the root move's value for explored children
        Node *childNode = childEdge.getChild();
        if (childNode) {
            ValueBound childBound = childNode->getBound();
            if (childNode->getVisits() > 0) {
                float childUtility = -childNode->getQ();

                rm->winRate  = childUtility * 0.5f + 0.5f;
                rm->drawRate = childNode->getD();
                if (Value lo = childBound.childLowerBound(); lo >= VALUE_MATE_IN_MAX_PLY)
                    rm->value = mate_in(std::max(mate_step(lo, 0) - th.board->ply(), 0));
                else if (Value up = childBound.childUpperBound(); up <= VALUE_MATED_IN_MAX_PLY)
                    rm->value = mated_in(std::max(mate_step(up, 0) - th.board->ply(), 0));
                else
                    rm->value = Evaluation::winRateToValue(rm->winRate);
                rm->utilityStdev = std::sqrt(childNode->getQVar());
                scratch.numSelectableRootMoves++;
            }
            rm->numNodes = childEdge.getVisits();
            extractPVOfChildNode(*childNode, rm->pv);
        }
        else {
            rm->numNodes = 0;
        }
        rm->policyPrior = childEdge.getP();
        rm->lcbValue =
            i < lcbValues.size() ? lcbValues[i] : std::numeric_limits<float>::quiet_NaN();
        rm->selectionValue = selectionValues[i];
    }

    // If we do not have any visited children, display all of them to show policy
    if (scratch.numSelectableRootMoves == 0)
        scratch.numSelectableRootMoves = th.rootMoves.size();
    scratch.numSelectableRootMoves =
        std::min(scratch.numSelectableRootMoves, maxNumRootMovesToPrint);

    // Sort the root moves in descending order by selection value
    std::stable_sort(th.rootMoves.begin(),
                     th.rootMoves.end(),
                     [](const RootMove &m1, const RootMove &m2) {
                         return m1.selectionValue > m2.selectionValue;
                     });
}

uint32_t Search::MCTS::Testing::generateNodeMoves(const Board         &board,
                                                  const SearchOptions &options,
                                                  Pos                 *moveList,
                                                  float               *policyList)
{
    return ::generateNodeMoves(board, options, moveList, policyList);
}

void Search::MCTS::Testing::evaluateNode(Node                &node,
                                         const SearchOptions &options,
                                         Board               &board,
                                         int                  ply)
{
    ::evaluateNode<false>(node, options, board, ply);
}

Value Search::MCTS::Testing::simpleVcf(Board               &board,
                                       const SearchOptions &options,
                                       VCNLevel             level,
                                       int                  ply)
{
    if (!options.vcnMode.enabled() || level == VCNLevel::None)
        return SimpleVCF::vcf(options.rule, board, ply);
    return SimpleVCF::vcf(
        options.rule,
        board,
        ply,
        SimpleVCF::VcnContext {level, options.vcnMode.attacker, Vcn::hashXor(options, level)});
}

Search::MCTS::Testing::PassTestResult
Search::MCTS::Testing::vcnPassTestStatic(const Board &board, const SearchOptions &options)
{
    return VcnPassTest::staticRule(board, options.rule, options.vcnMode.attacker);
}

Search::MCTS::Testing::PassTestResult Search::MCTS::Testing::vctSolve(Board               &board,
                                                                      const SearchOptions &options,
                                                                      uint32_t  nodeBudget,
                                                                      uint32_t *nodesUsed)
{
    return VcnPassTest::solve(board, options, 1, nodeBudget, nodesUsed);
}

namespace {

template <Rule R>
uint32_t vctAdmissibleMovesOf(Board &board, const SearchOptions &options, Pos *moveList)
{
    VcnPassTest::Context ctx(options, 1, 0);
    Pos                  moves[MAX_MOVES];
    const uint32_t       numMoves = VcnPassTest::collectAttackerMoves<R>(board, ctx, moves);
    uint32_t             n        = 0;
    for (uint32_t i = 0; i < numMoves; i++) {
        board.move<R, Board::MoveType::NO_EVAL>(moves[i]);
        if (VcnPassTest::lastMoveAdmissible<R>(board, ctx))
            moveList[n++] = moves[i];
        board.undo<R, Board::MoveType::NO_EVAL>();
    }
    return n;
}

}  // namespace

uint32_t Search::MCTS::Testing::vctAdmissibleMoves(Board               &board,
                                                   const SearchOptions &options,
                                                   Pos                 *moveList)
{
    switch (options.rule) {
    case FREESTYLE: return vctAdmissibleMovesOf<FREESTYLE>(board, options, moveList);
    case STANDARD: return vctAdmissibleMovesOf<STANDARD>(board, options, moveList);
    case RENJU: return vctAdmissibleMovesOf<RENJU>(board, options, moveList);
    default: return 0;
    }
}

HashKey Search::MCTS::Testing::vctCacheKey(const Board &board, const SearchOptions &options)
{
    return board.zobristKey() ^ VcnPassTest::Context(options, 0, 0).cacheXor;
}
