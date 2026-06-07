#!/bin/bash
#
# Unit tests for xrdp-cups-env.sh
# Tests the profile.d script logic for setting CUPS_SERVER
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENV_SCRIPT="${SCRIPT_DIR}/../src/cups/xrdp-cups-env.sh"

tests_run=0
tests_passed=0

assert_eq() {
    local expected="$1" actual="$2" msg="$3"
    tests_run=$((tests_run + 1))
    if [ "$expected" = "$actual" ]; then
        tests_passed=$((tests_passed + 1))
    else
        echo "FAIL: $msg" >&2
        echo "  expected: '$expected'" >&2
        echo "  actual:   '$actual'" >&2
    fi
}

# Run the env script in a subshell and capture CUPS_SERVER
run_env_script() {
    local display="$1" ini_file="$2" uid="$3"
    (
        export DISPLAY="$display"
        # Override the sesman.ini path and id -u call
        # We source a modified version that uses our test ini
        SESMAN_INI="$ini_file"
        # Can't easily override id -u, but we know our UID
        . "$ENV_SCRIPT"
        echo "${CUPS_SERVER:-UNSET}"
    )
}

# Create temp dir for test files
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# --- Test: DISPLAY unset -> CUPS_SERVER not set ---
test_no_display() {
    local result
    result=$(unset DISPLAY; . "$ENV_SCRIPT"; echo "${CUPS_SERVER:-UNSET}")
    assert_eq "UNSET" "$result" "No DISPLAY -> CUPS_SERVER unset"
}

# --- Test: Default (no sesman.ini) -> enabled ---
test_missing_ini() {
    local result
    result=$(
        export DISPLAY=":10"
        # Point at non-existent file
        sed "s|/etc/xrdp/sesman.ini|${TMPDIR}/nonexistent.ini|" "$ENV_SCRIPT" > "$TMPDIR/env_test.sh"
        . "$TMPDIR/env_test.sh"
        echo "${CUPS_SERVER:-UNSET}"
    )
    local expected="/tmp/xrdp-cups-$(id -u)-10/sock"
    assert_eq "$expected" "$result" "Missing ini -> defaults to enabled"
}

# --- Test: EnablePerSessionCupsd=true -> sets CUPS_SERVER ---
test_enabled_explicit() {
    cat > "$TMPDIR/sesman_true.ini" << 'EOF'
[Chansrv]
EnablePerSessionCupsd=true
EOF
    local result
    result=$(
        export DISPLAY=":10"
        sed "s|/etc/xrdp/sesman.ini|${TMPDIR}/sesman_true.ini|" "$ENV_SCRIPT" > "$TMPDIR/env_test.sh"
        . "$TMPDIR/env_test.sh"
        echo "${CUPS_SERVER:-UNSET}"
    )
    local expected="/tmp/xrdp-cups-$(id -u)-10/sock"
    assert_eq "$expected" "$result" "EnablePerSessionCupsd=true -> sets CUPS_SERVER"
}

# --- Test: EnablePerSessionCupsd=false -> unset ---
test_disabled() {
    cat > "$TMPDIR/sesman_false.ini" << 'EOF'
[Chansrv]
EnablePerSessionCupsd=false
EOF
    local result
    result=$(
        export DISPLAY=":10"
        sed "s|/etc/xrdp/sesman.ini|${TMPDIR}/sesman_false.ini|" "$ENV_SCRIPT" > "$TMPDIR/env_test.sh"
        . "$TMPDIR/env_test.sh"
        echo "${CUPS_SERVER:-UNSET}"
    )
    assert_eq "UNSET" "$result" "EnablePerSessionCupsd=false -> CUPS_SERVER unset"
}

# --- Test: Case insensitive (FALSE, No, 0) ---
test_disabled_case_insensitive() {
    for val in "FALSE" "False" "no" "NO" "0"; do
        cat > "$TMPDIR/sesman_ci.ini" << EOF
[Chansrv]
EnablePerSessionCupsd=$val
EOF
        local result
        result=$(
            export DISPLAY=":10"
            sed "s|/etc/xrdp/sesman.ini|${TMPDIR}/sesman_ci.ini|" "$ENV_SCRIPT" > "$TMPDIR/env_test.sh"
            . "$TMPDIR/env_test.sh"
            echo "${CUPS_SERVER:-UNSET}"
        )
        assert_eq "UNSET" "$result" "EnablePerSessionCupsd=$val -> CUPS_SERVER unset"
    done
}

# --- Test: Last occurrence wins ---
test_last_wins() {
    cat > "$TMPDIR/sesman_multi.ini" << 'EOF'
[Chansrv]
EnablePerSessionCupsd=false
EnablePerSessionCupsd=true
EOF
    local result
    result=$(
        export DISPLAY=":10"
        sed "s|/etc/xrdp/sesman.ini|${TMPDIR}/sesman_multi.ini|" "$ENV_SCRIPT" > "$TMPDIR/env_test.sh"
        . "$TMPDIR/env_test.sh"
        echo "${CUPS_SERVER:-UNSET}"
    )
    local expected="/tmp/xrdp-cups-$(id -u)-10/sock"
    assert_eq "$expected" "$result" "Last EnablePerSessionCupsd line wins (true)"
}

# --- Test: DISPLAY=:10.0 strips .0 ---
test_display_with_screen() {
    cat > "$TMPDIR/sesman_screen.ini" << 'EOF'
[Chansrv]
EnablePerSessionCupsd=true
EOF
    local result
    result=$(
        export DISPLAY=":10.0"
        sed "s|/etc/xrdp/sesman.ini|${TMPDIR}/sesman_screen.ini|" "$ENV_SCRIPT" > "$TMPDIR/env_test.sh"
        . "$TMPDIR/env_test.sh"
        echo "${CUPS_SERVER:-UNSET}"
    )
    local expected="/tmp/xrdp-cups-$(id -u)-10/sock"
    assert_eq "$expected" "$result" "DISPLAY=:10.0 strips screen number"
}

# --- Test: Commented-out setting ignored ---
test_commented_ignored() {
    cat > "$TMPDIR/sesman_comment.ini" << 'EOF'
[Chansrv]
#EnablePerSessionCupsd=false
; EnablePerSessionCupsd=false
EOF
    local result
    result=$(
        export DISPLAY=":10"
        sed "s|/etc/xrdp/sesman.ini|${TMPDIR}/sesman_comment.ini|" "$ENV_SCRIPT" > "$TMPDIR/env_test.sh"
        . "$TMPDIR/env_test.sh"
        echo "${CUPS_SERVER:-UNSET}"
    )
    local expected="/tmp/xrdp-cups-$(id -u)-10/sock"
    assert_eq "$expected" "$result" "Commented lines ignored -> defaults to enabled"
}

# Run all tests
test_no_display
test_missing_ini
test_enabled_explicit
test_disabled
test_disabled_case_insensitive
test_last_wins
test_display_with_screen
test_commented_ignored

echo "test_cups_env: ${tests_passed}/${tests_run} passed"
[ "$tests_passed" -eq "$tests_run" ]
