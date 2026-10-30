#!/usr/bin/env bash
# Run ChmLink's Windows runtime CI on the test VM from this machine.
# The VM never needs to reach the internet or GitHub: this script pushes the
# source over SSH, starts the run detached on the VM, follows its log, and
# fetches the results. See tools/ci/README.md.
#
#   vm-run.sh init  --baseline-dir <dir on the VM with signed chmlink.inf/.sys/.cat>
#   vm-run.sh doctor
#   vm-run.sh run <smoke|full> [--commit REV | --worktree] [--no-wait]
#   vm-run.sh attach <run-id>          # follow a run and fetch its results
#   vm-run.sh abort <run-id>
#   vm-run.sh restore                  # put the baseline driver back
#   vm-run.sh clean
#
# Exit status: 0 passed, 1 tests or build failed, 2 benchmark gate failed,
# 3 VM or infrastructure problem, 64 usage error.
#
# Written for bash 3.2 (macOS) as well as newer versions.
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$SCRIPT_DIR/../.." && pwd)
CONFIG=${CHMLINK_CI_ENV:-$HOME/.chmlink-ci.env}
# shellcheck source=/dev/null
[ -f "$CONFIG" ] && . "$CONFIG"

VM_HOST=${CHMLINK_VM_HOST:-}
VM_USER=${CHMLINK_VM_USER:-}
VM_ROOT=${CHMLINK_VM_ROOT:-C:\\ci}
RESULTS=${CHMLINK_CI_RESULTS:-$REPO/build/ci-results}
POLL_SECONDS=${CHMLINK_CI_POLL:-10}
SSH_BASE="-o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=4"
SSH_OPTS="$SSH_BASE ${CHMLINK_VM_SSH_OPTS:-}"

die() { echo "vm-run: $*" >&2; exit "${2:-64}"; }

[ -n "$VM_HOST" ] && [ -n "$VM_USER" ] || die "set CHMLINK_VM_HOST and CHMLINK_VM_USER (e.g. in $CONFIG)"
TARGET="$VM_USER@$VM_HOST"
# shellcheck disable=SC1003  # the quoted backslash is intended
VM_ROOT_FWD=$(printf '%s' "$VM_ROOT" | tr '\\' '/')

# The command string is expanded here on purpose; the VM runs it through cmd.exe.
# shellcheck disable=SC2086,SC2029
vm() { ssh $SSH_OPTS "$TARGET" "$@"; }
# shellcheck disable=SC2086
push() { scp -q $SSH_OPTS "$1" "$TARGET:$2"; }
# shellcheck disable=SC2086
pull() { scp -q -r $SSH_OPTS "$TARGET:$1" "$2"; }
# Run an action of the VM script. $1 is the script path on the VM, the rest its arguments.
vmps() {
    local script=$1; shift
    vm "powershell -NoProfile -ExecutionPolicy Bypass -File $script $*"
}

install_bin() {
    vm "if not exist $VM_ROOT\\bin mkdir $VM_ROOT\\bin" >/dev/null || return 1
    push "$SCRIPT_DIR/vm-ci.ps1" "$VM_ROOT_FWD/bin/vm-ci.ps1"
}
BIN="$VM_ROOT\\bin\\vm-ci.ps1"

# ------------------------------------------------------------------ commands
cmd_init() {
    local baseline=""
    while [ $# -gt 0 ]; do
        case $1 in --baseline-dir) baseline=$2; shift 2 ;; --cert) CERT=$2; shift 2 ;; *) die "unknown option $1" ;; esac
    done
    [ -n "$baseline" ] || die "init needs --baseline-dir"
    vm "if not exist $VM_ROOT mkdir $VM_ROOT" || die "cannot reach the VM" 3
    install_bin || die "cannot upload vm-ci.ps1" 3
    vmps "$BIN" -Action Init -BaselineFrom "\"$baseline\"" ${CERT:+-CertThumbprint $CERT}
}

cmd_doctor() {
    install_bin || die "cannot reach the VM" 3
    vmps "$BIN" -Action Doctor
    local code=$?
    [ $code -eq 0 ] || exit 3
}

cmd_restore() {
    install_bin || die "cannot reach the VM" 3
    vmps "$BIN" -Action Restore
}

cmd_clean() {
    install_bin || die "cannot reach the VM" 3
    vmps "$BIN" -Action Clean
}

cmd_abort() {
    [ $# -eq 1 ] || die "usage: abort <run-id>"
    install_bin || die "cannot reach the VM" 3
    vmps "$BIN" -Action Abort -RunId "$1"
    echo "Now run: vm-run.sh restore"
}

make_run_id() {
    local label=$1
    printf '%s-%s' "$(date +%Y%m%d-%H%M%S)" "$label"
}

export_tree() {   # $1 = commit | worktree, $2 = output tar
    if [ "$1" = worktree ]; then
        (cd "$REPO" && { git ls-files; git ls-files --others --exclude-standard; } | grep -v -E '^(docs|build|target)/' | sort -u > "$2.list" &&
            tar -cf "$2" -T "$2.list") || return 1
        rm -f "$2.list"
    else
        git -C "$REPO" archive --format=tar -o "$2" "$1"
    fi
}

cmd_run() {
    local lane=${1:-}; shift || true
    case $lane in smoke|full) ;; *) die "usage: run <smoke|full> [--commit REV | --worktree] [--no-wait]" ;; esac
    local source=HEAD wait=1 label
    while [ $# -gt 0 ]; do
        case $1 in
            --commit) source=$2; shift 2 ;;
            --worktree) source=worktree; shift ;;
            --no-wait) wait=0; shift ;;
            *) die "unknown option $1" ;;
        esac
    done
    if [ "$source" = worktree ]; then
        label=wt
    else
        git -C "$REPO" rev-parse --verify --quiet "$source^{commit}" >/dev/null || die "unknown revision $source"
        label=$(git -C "$REPO" rev-parse --short=8 "$source^{commit}")
        source=$(git -C "$REPO" rev-parse "$source^{commit}")
        if [ -n "$(git -C "$REPO" status --porcelain --untracked-files=no)" ]; then
            echo "note: the working tree has uncommitted changes; testing commit $label only (use --worktree to include them)" >&2
        fi
    fi
    local run_id tmp
    run_id=$(make_run_id "$label")
    tmp=$(mktemp -d) || die "mktemp failed"
    trap 'rm -rf "$tmp"' EXIT
    export_tree "$source" "$tmp/src.tar" || die "cannot export the source tree"

    echo "run $run_id: lane=$lane source=$label"
    install_bin || die "cannot reach the VM" 3
    if ! vmps "$BIN" -Action Doctor >"$tmp/doctor.txt" 2>&1; then
        cat "$tmp/doctor.txt" >&2
        die "VM preflight failed; see above (try: vm-run.sh doctor)" 3
    fi
    grep -E '^(WARN|BUSY|HEALABLE)' "$tmp/doctor.txt"
    vm "mkdir $VM_ROOT\\runs\\$run_id\\src" >/dev/null || die "cannot create the run directory" 3
    push "$tmp/src.tar" "$VM_ROOT_FWD/runs/$run_id/src.tar" || die "upload failed" 3
    vm "tar -xf $VM_ROOT\\runs\\$run_id\\src.tar -C $VM_ROOT\\runs\\$run_id\\src && del $VM_ROOT\\runs\\$run_id\\src.tar" || die "cannot unpack on the VM" 3
    local launched
    # The CI script is always this checkout's copy, so any commit can be tested, including
    # ones that predate tools/ci. The commit under test supplies only the code to build and run.
    launched=$(vmps "$BIN" -Action Launch -RunId "$run_id" -Lane "$lane" 2>&1)
    case $launched in LAUNCHED*) ;; *) echo "$launched" >&2; die "the VM did not start the run" 3 ;; esac
    echo "started on the VM ($launched); follow later with: vm-run.sh attach $run_id"
    [ $wait -eq 1 ] || exit 0
    follow "$run_id" "$lane"
}

# Follow a run's log until it finishes, then fetch results and apply the gates.
follow() {
    local run_id=$1 lane=$2 offset=0 failures=0 out state stage next last_stage="" poll_err
    poll_err=$(mktemp) || die "mktemp failed" 3
    while :; do
        if out=$(vmps "$BIN" -Action Poll -RunId "$run_id" -Offset "$offset" 2>"$poll_err"); then
            failures=0
            out=$(printf '%s\n' "$out" | tr -d '\r')
            printf '%s\n' "$out" | grep -v -e '^@@STATE' -e '^$'
            state=$(printf '%s\n' "$out" | grep '^@@STATE' | tail -1)
            [ -n "$state" ] || { sleep "$POLL_SECONDS"; continue; }
            # shellcheck disable=SC2086
            set -- $state; state=$2; stage=$3; next=$4
            offset=$next
            if [ "$stage" != "$last_stage" ] && [ "$stage" != "-" ]; then echo "--- stage: $stage"; last_stage=$stage; fi
            case $state in
                passed|failed|infra-failed|aborted|crashed|missing) break ;;
            esac
        else
            failures=$((failures + 1))
            if [ $failures -eq 1 ] || [ $failures -gt 30 ]; then
                echo "poll failed (attempt $failures): $(tr -d '\r' < "$poll_err" | grep -v '^$' | head -3 | cut -c1-200)" >&2
            fi
            [ $failures -le 30 ] || die "cannot read the run's state; it continues on the VM. Resume: vm-run.sh attach $run_id" 3
        fi
        sleep "$POLL_SECONDS"
    done
    rm -f "$poll_err"
    finish "$run_id" "$lane" "$state"
}

cmd_attach() {
    [ $# -eq 1 ] || die "usage: attach <run-id>"
    install_bin || die "cannot reach the VM" 3
    follow "$1" "$(lane_of "$1")"
}

lane_of() {   # best effort: the summary has the lane once the run is done
    local lane
    lane=$(vm "powershell -NoProfile -Command \"(Get-Content $VM_ROOT\\runs\\$1\\out\\summary.json -Raw -ErrorAction SilentlyContinue | ConvertFrom-Json).Lane\"" 2>/dev/null | tr -d '\r')
    echo "${lane:-smoke}"
}

finish() {
    local run_id=$1 lane=$2 state=$3 dest
    dest="$RESULTS/$run_id"
    mkdir -p "$RESULTS" || die "cannot create $RESULTS" 3
    rm -rf "$dest"
    if ! pull "$VM_ROOT_FWD/runs/$run_id/out" "$dest" 2>/dev/null; then
        mkdir -p "$dest"
    fi
    echo
    echo "results: $dest"
    case $state in
        passed) ;;
        failed) echo "RESULT: FAILED (see log above and $dest/summary.json)"; exit 1 ;;
        *) echo "RESULT: $state"
           echo "the VM may need attention: vm-run.sh doctor / restore"; exit 3 ;;
    esac
    if [ "$lane" = full ] && ls "$dest"/bench-*/bench.jsonl >/dev/null 2>&1; then
        local env_file="" candidate
        for candidate in "$dest"/bench-*/environment.json; do env_file=$candidate; break; done
        echo "comparing benchmark with the baseline..."
        local code=0
        python3 "$SCRIPT_DIR/compare_bench.py" "$dest"/bench-*/bench.jsonl --env "$env_file" \
            --baseline "$SCRIPT_DIR/bench-baseline.json" --markdown "$dest/bench-summary.md" >"$dest/bench-compare.txt" 2>&1 || code=$?
        grep -E '^## |FAIL|Warning|No baseline|error' "$dest/bench-compare.txt"
        if [ "$code" -eq 1 ]; then echo "RESULT: BENCHMARK REGRESSION (details: $dest/bench-summary.md)"; exit 2; fi
        [ "$code" -eq 0 ] || { echo "RESULT: benchmark comparison error ($code)"; exit 3; }
    fi
    echo "RESULT: PASSED"
    exit 0
}

# ---------------------------------------------------------------------- main
[ $# -ge 1 ] || die "usage: vm-run.sh <init|doctor|run|attach|abort|restore|clean> ..."
command=$1; shift
case $command in
    init) cmd_init "$@" ;;
    doctor) cmd_doctor ;;
    run) cmd_run "$@" ;;
    attach) cmd_attach "$@" ;;
    abort) cmd_abort "$@" ;;
    restore) cmd_restore ;;
    clean) cmd_clean ;;
    *) die "unknown command $command" ;;
esac
