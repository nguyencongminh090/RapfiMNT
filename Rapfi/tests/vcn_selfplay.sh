#!/usr/bin/env bash
# Generate mid-game positions by engine self-play (freestyle 15x15) for the VCN sweeps.
# Usage: tests/vcn_selfplay.sh ENGINE GAMES SEED > positions.txt
# One line per position, "x,y x,y ...", the first move is black's. Positions are taken every
# 4 plies from ply 12 to ply 44. Depth-limited (not time-limited) so output is reproducible.
ENGINE=$1; GAMES=$2; SEED=$3
RANDOM=$SEED
for ((g = 0; g < GAMES; g++)); do
    moves=()
    while ((${#moves[@]} < 3)); do  # random 3-stone opening in the central 5x5
        m="$((5 + RANDOM % 5)),$((5 + RANDOM % 5))"
        [[ " ${moves[*]} " == *" $m "* ]] || moves+=("$m")
    done
    coproc ENG { "$ENGINE" 2>&1; }
    {
        echo "START 15"; echo "INFO RULE 0"; echo "INFO THREAD_NUM 1"
        echo "INFO TIMEOUT_TURN 5000"; echo "INFO MAX_DEPTH 4"
    } >&"${ENG[1]}"
    while ((${#moves[@]} < 44)); do
        n=${#moves[@]}
        {
            echo "BOARD"
            for ((i = 0; i < n; i++)); do  # colour 1 = the side to move's stones
                if (((n - i) % 2 == 0)); then echo "${moves[$i]},1"; else echo "${moves[$i]},2"; fi
            done
            echo "DONE"
        } >&"${ENG[1]}"
        while IFS= read -r l <&"${ENG[0]}"; do
            [[ $l =~ ^[0-9]+,[0-9]+$ ]] && break
        done
        moves+=("$l")
        if ((${#moves[@]} >= 12 && ${#moves[@]} % 4 == 0)); then echo "${moves[*]}"; fi
    done
    echo "END" >&"${ENG[1]}"
    wait "$ENG_PID" 2>/dev/null
done
