#!/usr/bin/env bash
# VCN-02 measurement set: run each attack position through `YXVCN 2|3|4` and print one row per
# (position, level) with the final search line. Not a unit test: results are recorded by hand in
# docs/todo/VCN-02-attack-search-quality.md and compared per slice.
#
# Usage: tests/vcn_measure.sh [path/to/pbrain-rapfi] [max_depth]
# Single thread, fixed MAX_DEPTH, so node counts are reproducible.

set -uo pipefail
ENGINE=${1:-build/pbrain-rapfi}
DEPTH=${2:-10}

# Each position: name|rule|black moves|white moves. Moves are "x,y" separated by spaces and are
# played alternately black, white, black, ... The side to move after the last stone is the
# attacker. Rule numbers follow INFO RULE: 0 freestyle, 1 standard, 4 renju.
POSITIONS=(
    # P1 item 1. White (defender) threatens five at 12,9; black must block there, which makes a
    # black block four that white blocks at 13,9. Black's VCF is then far away: 3,6 (four + block
    # three), white 2,6, then 3,7 is a double four. Expected: attacker win at every level.
    "P1-farVcfAfterForcedBlock|0|3,8 3,9 4,6 5,6 6,6 5,5 6,4 9,9 10,9 11,9 12,14|3,10 7,6 7,3 8,9 12,10 12,11 12,12 12,13 0,0 2,9 2,10"
    # P2 VCN-01 fixture: black open three on row 7. Expected: win at every level.
    "P2-openThree|0|7,7 8,7 9,7|3,3 3,12 12,12"
    # P3 VCN-01 fixture: lone black stone. Expected: loss at VC4/VC3, eval at VC2.
    "P3-quiet|0|7,7|3,3"
    # P4 Defender counter-four: white threatens five at 12,9 and black's block makes nothing;
    # black's open three (row 7) is far away. Expected: VC4 loss (block is not a four).
    "P4-counterFourBlockWithoutFour|0|3,7 4,7 5,7 12,14 0,0|12,10 12,11 12,12 12,13 14,0"
    # P5 Defender counter open three: white open three on column 11, black only has a flex two.
    # Expected: no attacker win at VC4/VC3.
    "P5-counterOpenThree|0|3,7 4,7 0,0|11,5 11,6 11,7"
    # P6 Defender faster VCF: white has a double four at 12,8 (fours on column 12 and row 8,
    # each blocked once); black has an open two. Expected: no attacker win.
    "P6-defenderFasterVcf|0|3,7 4,7 12,4 8,8 0,0 14,14|12,5 12,6 12,7 9,8 10,8 11,8"
    # P7 Review finding: after the P1 forced pair (12,9)/(13,9) black's only threat is a double
    # open three at 5,5 (no four). quickWinCheck() counts it as a win inside vcfsearch.
    # Expected: VC4 loss (a defender pass refutes the double three), VC3 win.
    "P7-doubleThreeAfterForcedBlock|0|3,5 4,5 5,3 5,4 9,9 10,9 11,9 12,14|8,9 12,10 12,11 12,12 12,13 0,0 14,0 0,14"
    # P8 P1 in renju with colours swapped: white attacks (no forbidden points for white).
    "P8-farVcfAfterForcedBlockRenju|4|3,10 7,6 7,3 8,9 12,10 12,11 12,12 12,13 0,0 2,9 2,10 14,14|3,8 3,9 4,6 5,6 6,6 5,5 6,4 9,9 10,9 11,9 12,14"
)

interleave() {
    local -a b=($1) w=($2)
    local i
    for ((i = 0; i < ${#b[@]} || i < ${#w[@]}; i++)); do
        [[ $i -lt ${#b[@]} ]] && echo "${b[$i]},1"
        [[ $i -lt ${#w[@]} ]] && echo "${w[$i]},2"
    done
}

printf '%-34s %-5s %-10s %-8s %-10s %-8s %s\n' position level eval depth nodes time best
for entry in "${POSITIONS[@]}"; do
    IFS='|' read -r name rule black white <<<"$entry"
    moves=$(interleave "$black" "$white")
    for level in 4 3 2; do
        # END must only be sent after the search has printed its move: the engine reads stdin
        # while thinking and would stop the search early, making node counts nondeterministic.
        coproc ENG { "$ENGINE" 2>&1; }
        {
            echo "START 15"
            echo "INFO RULE $rule"
            echo "INFO THREAD_NUM 1"
            echo "INFO MAX_DEPTH $DEPTH"
            echo "INFO TIMEOUT_TURN 60000"
            echo "YXBOARD"
            echo "$moves"  # colour 1 = black (first mover), 2 = white
            echo "DONE"
            echo "YXVCN $level"
        } >&"${ENG[1]}"
        out=""
        while IFS= read -r l <&"${ENG[0]}"; do
            out+="$l"$'\n'
            [[ $l =~ ^[0-9]+,[0-9]+$ ]] && break
        done
        echo "END" >&"${ENG[1]}"
        wait "$ENG_PID" 2>/dev/null
        line=$(grep 'MESSAGE Speed' <<<"$out" | tail -1)
        best=$(grep -E '^[0-9]+,[0-9]+$' <<<"$out" | tail -1)
        eval_=$(sed -n 's/.*Eval \([^ ]*\).*/\1/p' <<<"$line")
        depth=$(sed -n 's/.*Depth \([^ ]*\).*/\1/p' <<<"$line")
        nodes=$(sed -n 's/.*Node \([^ ]*\).*/\1/p' <<<"$line")
        time_=$(sed -n 's/.*Time \([^ ]*\).*/\1/p' <<<"$line")
        printf '%-34s VC%-3s %-10s %-8s %-10s %-8s %s\n' "$name" "$level" "$eval_" "$depth" "$nodes" "$time_" "$best"
    done
done
