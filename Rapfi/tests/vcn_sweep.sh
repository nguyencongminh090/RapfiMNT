#!/usr/bin/env bash
# Run YXVCN LEVEL at MAX_DEPTH DEPTH on every position line and print
# "index eval nodes best time" per position (single thread). END is sent only after the move line,
# otherwise the engine stops the search early (see docs/instruction/VCN-02). Both info formats are
# parsed: the classical "MESSAGE Speed .. | Eval e | Node n | Time t" and the network one
# "MESSAGE depth d-s ev e n n n/ms r tm t". A position with no info line (the engine answered
# without searching, e.g. a mate at the root) prints "-" for eval/nodes/time; a position that
# does not answer within TIMEOUT seconds (default 300) prints "TIMEOUT".
# Usage: tests/vcn_sweep.sh ENGINE LEVEL DEPTH [RULE=0] < positions.txt
ENGINE=$1; LEVEL=$2; DEPTH=$3; RULE=${4:-0}; TIMEOUT=${TIMEOUT:-300}
idx=0
while IFS= read -r line; do
    read -ra mv <<<"$line"
    coproc ENG { "$ENGINE" 2>&1; }
    {
        echo "START 15"; echo "INFO RULE $RULE"; echo "INFO THREAD_NUM 1"
        echo "INFO MAX_DEPTH $DEPTH"; echo "INFO TIMEOUT_TURN 60000"
        echo "YXBOARD"
        for ((i = 0; i < ${#mv[@]}; i++)); do echo "${mv[$i]},$((i % 2 + 1))"; done
        echo "DONE"; echo "YXVCN $LEVEL"
    } >&"${ENG[1]}"
    out=""; timedout=0
    while IFS= read -r -t "$TIMEOUT" l <&"${ENG[0]}"; do
        out+="$l"$'\n'
        [[ $l =~ ^[0-9]+,[0-9]+$ ]] && break
    done || timedout=1
    [[ $l =~ ^[0-9]+,[0-9]+$ ]] || timedout=1
    echo "END" >&"${ENG[1]}" 2>/dev/null
    if ((timedout)); then kill "$ENG_PID" 2>/dev/null; fi
    wait "$ENG_PID" 2>/dev/null
    if ((timedout)); then echo "$idx TIMEOUT - - -"; idx=$((idx + 1)); continue; fi
    s=$(grep -E 'MESSAGE (Speed|depth )' <<<"$out" | tail -1)
    if [[ $s == *Speed* ]]; then
        ev=$(sed -n 's/.*Eval \([^ ]*\).*/\1/p' <<<"$s")
        nd=$(sed -n 's/.*Node \([^ ]*\).*/\1/p' <<<"$s")
        tm=$(sed -n 's/.*Time \([^ ]*\).*/\1/p' <<<"$s")
    else
        ev=$(sed -n 's/.* ev \([^ ]*\) n .*/\1/p' <<<"$s")
        nd=$(sed -n 's/.* n \([^ ]*\) n\/ms.*/\1/p' <<<"$s")
        tm=$(sed -n 's/.* tm \([^ ]*\) pv.*/\1ms/p' <<<"$s")
    fi
    best=$(grep -E '^[0-9]+,[0-9]+$' <<<"$out" | tail -1)
    echo "$idx ${ev:--} ${nd:--} $best ${tm:--}"
    idx=$((idx + 1))
done
