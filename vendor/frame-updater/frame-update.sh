#!/bin/sh
# SPDX-License-Identifier: MIT — part of frame-updater by sasaken1102r, shipped under the host app's MIT license
# Checks GitHub for a newer release of a Steam Frame app and installs it with the release's own
# install.sh. Shared by the apps: the original lives in the frame-updater repository and each app
# carries a copy (vendor/frame-updater/, see UPSTREAM there). POSIX sh; needs curl, tar, sha256sum
# and python3 (only to read the GitHub API's JSON). Writes nothing outside the user's home.
#
#   frame-update.sh --app A --repo O/R --current V [--asset P] [--force] check
#   frame-update.sh --app A --repo O/R --current V --asset P [--install-arg X]... [--detach] install
#   frame-update.sh --app A status
#   frame-update.sh --version
#
# --asset is the release file name with {version} in place of the version, e.g.
# "frameeyeosc-{version}-steamframe-aarch64.tar.gz". Every command prints one line of JSON on
# stdout (see README.md). Files, with C = ${XDG_CACHE_HOME:-~/.cache}/A:
#   C/update-check.json  last answer from GitHub, reused for 55 minutes (errors for 1 hour)
#   C/update-state.json  progress of the last install: running (with step) / done / failed
#                        (a "done" older than 24 hours is dropped at the next check or install)
#   C/update.log         log of the last install, including install.sh's output
#   C/update/            work folder (the copy of this script, downloads, the extracted release)
# install needs a SHA256SUMS file attached to the release and runs the release's install.sh with
# the options in ${XDG_CONFIG_HOME:-~/.config}/A/install-args (one per line), or with the
# --install-arg ones when that file is missing. --detach hands the install to a transient systemd
# user unit (A-update), so it keeps going when install.sh restarts the caller (the panel).
#
# Test overrides (environment): FRAME_UPDATE_API_URL (default https://api.github.com),
# FRAME_UPDATE_ALLOW_INSECURE=1 (allow http:// and any host), FRAME_UPDATE_CHECK_TTL (3300),
# FRAME_UPDATE_ERROR_TTL (3600).

FRAME_UPDATE_VERSION=0.3.0

api_base=${FRAME_UPDATE_API_URL:-https://api.github.com}
insecure=${FRAME_UPDATE_ALLOW_INSECURE:-0}
# A bit under the panels' hourly tick, so each tick really asks GitHub: about once an hour per app.
check_ttl=${FRAME_UPDATE_CHECK_TTL:-3300}
error_ttl=${FRAME_UPDATE_ERROR_TTL:-3600}
# A detached install that hasn't written its PID after this many seconds never started
start_grace=60
# A finished install's "done" is kept this long. The panels decide themselves whether it is news to them (it
# is if it came after they started); this only keeps the file from lying around for good
done_ttl=86400
nl='
'
cr=$(printf '\r')

usage() {
    cat >&2 <<'EOF'
Usage:
  frame-update.sh --app A --repo O/R --current V [--asset P] [--force] check
  frame-update.sh --app A --repo O/R --current V --asset P [--install-arg X]... [--detach] install
  frame-update.sh --app A status
  frame-update.sh --version
EOF
}

# Make a value safe to put between double quotes in our JSON: control characters are dropped and
# " \ become ' /, so the files never contain escapes and the readers stay simple.
json_safe() {
    printf '%s' "$1" | tr -d '\000-\037' | tr '"\\' "'/"
}

# Print the string value of KEY from one of our one-line JSON files.
json_str() {
    sed -n "s/.*\"$2\":\"\([^\"]*\)\".*/\1/p" "$1" 2>/dev/null | head -n 1
}

# Print the number or boolean value of KEY from one of our one-line JSON files.
json_raw() {
    sed -n "s/.*\"$2\":\([0-9a-z-]*\)[,}].*/\1/p" "$1" 2>/dev/null | head -n 1
}

now() {
    date +%s
}

# This boot's ID (empty where the kernel has none). The lock and the state file keep it next to a PID:
# after a power loss the PID may belong to another process, so a PID from another boot means nothing.
this_boot=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null | tr -cd '0-9a-f-')

# True if PID, recorded in boot BOOT (may be empty), is a live process of this user in this boot.
# kill -0 fails with EPERM for another user's process, which can't be ours either.
pid_alive() { # pid boot
    case $1 in '' | *[!0-9]*) return 1 ;; esac
    if [ -n "$2" ] && [ -n "$this_boot" ] && [ "$2" != "$this_boot" ]; then
        return 1
    fi
    kill -0 "$1" 2>/dev/null
}

# Log a line to stderr and, during an install, to the log file.
log() {
    _line="$(date '+%Y-%m-%d %H:%M:%S') $*"
    printf '%s\n' "$_line" >&2
    if [ -n "${log_file:-}" ]; then
        printf '%s\n' "$_line" >>"$log_file"
    fi
}

# Print a usage error as JSON and stop.
usage_error() {
    printf '{"status":"error","error":"usage","message":"%s"}\n' "$(json_safe "$1")"
    usage
    exit 2
}

# Print the numeric x.y.z core of a version ("v0.4.0-rc1" -> "0.4.0"), or fail if there is none.
version_core() {
    _v=${1#[vV]}
    _v=${_v%%[-+]*}
    case $_v in
        '' | *[!0-9.]* | .* | *. | *..* | *.*.*.* | *[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]*) return 1 ;;
    esac
    printf '%s\n' "$_v"
}

# Compare two version cores (from version_core); print -1, 0 or 1. Missing parts count as 0.
vercmp() {
    _a=$1
    _b=$2
    for _i in 1 2 3; do
        _x=${_a%%.*}
        _y=${_b%%.*}
        case $_a in *.*) _a=${_a#*.} ;; *) _a= ;; esac
        case $_b in *.*) _b=${_b#*.} ;; *) _b= ;; esac
        [ -n "$_x" ] || _x=0
        [ -n "$_y" ] || _y=0
        if [ "$_x" -gt "$_y" ]; then echo 1; return; fi
        if [ "$_x" -lt "$_y" ]; then echo -1; return; fi
    done
    echo 0
}

# True if the URL may be fetched: https to GitHub only (unless FRAME_UPDATE_ALLOW_INSECURE=1).
url_allowed() {
    if [ "$insecure" = 1 ]; then
        case $1 in http://* | https://*) return 0 ;; *) return 1 ;; esac
    fi
    case $1 in https://*) ;; *) return 1 ;; esac
    _host=${1#https://}
    _host=${_host%%/*}
    _host=${_host%%\?*}
    _host=${_host%%#*}
    case $_host in
        *[!a-z0-9.-]*) return 1 ;;
        github.com | api.github.com | codeload.github.com | *.githubusercontent.com) return 0 ;;
    esac
    return 1
}

# curl with the options every request uses. Prints "<http code> <final url>".
fetch() { # url output max-seconds [extra curl options...]
    _url=$1
    _out=$2
    _max=$3
    shift 3
    if [ "$insecure" = 1 ]; then
        curl --silent --show-error --fail --location --max-redirs 5 --connect-timeout 15 \
            --max-time "$_max" --max-filesize 268435456 -o "$_out" -w '%{http_code} %{url_effective}' "$@" "$_url"
    else
        curl --silent --show-error --fail --location --max-redirs 5 --connect-timeout 15 \
            --proto =https --proto-redir =https \
            --max-time "$_max" --max-filesize 268435456 -o "$_out" -w '%{http_code} %{url_effective}' "$@" "$_url"
    fi
}

# Download URL to FILE. On failure sets err_code/err_msg and returns 1.
download() { # url file max-seconds
    if ! url_allowed "$1"; then
        err_code=bad-url
        err_msg="refusing to download $1"
        return 1
    fi
    _res=$(fetch "$1" "$2" "$3" 2>"$2.err")
    _rc=$?
    _code=${_res%% *}
    _eff=${_res#* }
    if [ "$_rc" -ne 0 ]; then
        err_code=network
        err_msg="download failed (HTTP ${_code:-000}): $(head -n 1 "$2.err" 2>/dev/null) $1"
        rm -f "$2.err"
        return 1
    fi
    rm -f "$2.err"
    if ! url_allowed "$_eff"; then
        err_code=bad-url
        err_msg="redirected to $_eff"
        return 1
    fi
    return 0
}

# Ask GitHub for the latest release. Sets rel_tag rel_page rel_draft rel_pre rel_asset_url
# rel_sums_url rel_asset_name, and rel_notes / rel_notes_ja (its summary and the Japanese one, from
# the release text; may be empty). On failure sets err_code/err_msg and returns 1.
fetch_release() {
    rel_url="$api_base/repos/$repo/releases/latest"
    if ! url_allowed "$rel_url"; then
        err_code=bad-url
        err_msg="refusing to contact $rel_url"
        return 1
    fi
    mkdir -p "$work_dir" || { err_code=io; err_msg="cannot create $work_dir"; return 1; }
    _json="$work_dir/release.$$.json"
    _res=$(fetch "$rel_url" "$_json" 30 -H 'Accept: application/vnd.github+json' \
        -H 'X-GitHub-Api-Version: 2022-11-28' 2>"$_json.err")
    _rc=$?
    _code=${_res%% *}
    _eff=${_res#* }
    _curl_err=$(head -n 1 "$_json.err" 2>/dev/null)
    rm -f "$_json.err"
    if [ "$_rc" -ne 0 ]; then
        rm -f "$_json"
        case $_code in
            404) err_code=not-found; err_msg="no published release in $repo" ;;
            403 | 429) err_code=rate-limited; err_msg="GitHub refused the request (HTTP $_code), probably the hourly limit" ;;
            *) err_code=network; err_msg="cannot reach GitHub (HTTP ${_code:-000}): $_curl_err" ;;
        esac
        return 1
    fi
    if ! url_allowed "$_eff"; then
        rm -f "$_json"
        err_code=bad-url
        err_msg="redirected to $_eff"
        return 1
    fi
    if ! command -v python3 >/dev/null 2>&1; then
        rm -f "$_json"
        err_code=missing-tool
        err_msg="python3 is needed to read GitHub's answer"
        return 1
    fi
    _fields=$(python3 - "$_json" "$asset_pattern" <<'EOF'
import json
import re
import sys

NOTES_MAX = 300
JA_PREFIX = re.compile(r"(?:日本語|japanese)\s*[:：]\s*", re.IGNORECASE)
SKIPPED = re.compile(r"([-*+]|\d+[.)])(\s|$)|#|```|~~~|\||<!--|>")


def plain(text):
    """Markdown to plain text on one line: code marks, links, bold and italics go, the words stay."""
    # Escaped marks (\*) are kept aside as private-use characters until the end
    text = re.sub(r"\\([\\`*_{}\[\]()#+\-.!|<>~])", lambda m: chr(0xE000 + ord(m.group(1))), text)
    parts = re.split(r"`+", text)
    for i in range(0, len(parts), 2):  # outside code spans
        part = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", parts[i])
        part = re.sub(r"<((?:https?://|mailto:)[^>]*)>", r"\1", part)
        part = re.sub(r"(\*\*|__|~~)(?=\S)(.+?)(?<=\S)\1", r"\2", part)
        parts[i] = re.sub(r"(?<![\w*])\*(?=\S)(.+?)(?<=\S)\*(?![\w*])", r"\1", part)
    text = re.sub("[-]", lambda m: chr(ord(m.group(0)) - 0xE000), "".join(parts))
    return " ".join("".join(c if c >= " " and c != "\x7f" else " " for c in text).split())


def cap(text):
    """At most NOTES_MAX characters: cut at a space if there is one near the end (else at a character) and add …"""
    if len(text) <= NOTES_MAX:
        return text
    cut = text[:NOTES_MAX - 1]
    space = cut.rfind(" ")
    if space >= NOTES_MAX * 2 // 3:
        cut = cut[:space]
    return cut.rstrip(" ,.;:、。，．") + "…"


def notes_of(body):
    """The summary (the first paragraph that isn't a list, a heading or the Japanese one) and the Japanese one."""
    notes = notes_ja = ""
    if not isinstance(body, str):
        return notes, notes_ja
    for paragraph in re.split(r"\n[ \t]*\n", body.replace("\r\n", "\n").replace("\r", "\n")):
        lines = [line.strip() for line in paragraph.strip().split("\n")]
        if not lines[0] or SKIPPED.match(lines[0]):
            continue
        # A list that starts right under the text, without a blank line, isn't part of it
        for i, line in enumerate(lines):
            if i > 0 and SKIPPED.match(line):
                lines = lines[:i]
                break
        text = plain(" ".join(lines))
        prefix = JA_PREFIX.match(text)
        if prefix:
            if not notes_ja:
                notes_ja = cap(text[prefix.end():].strip())
        elif not notes:
            notes = cap(text)
        if notes and notes_ja:
            break
    return notes, notes_ja


try:
    with open(sys.argv[1], encoding="utf-8") as f:
        release = json.load(f)
    tag = release["tag_name"]
    version = tag[1:] if tag[:1] in ("v", "V") else tag
    name = sys.argv[2].replace("{version}", version) if sys.argv[2] else ""
    asset = sums = ""
    for item in release.get("assets") or []:
        url = item.get("browser_download_url") or ""
        if name and item.get("name") == name:
            asset = url
        if item.get("name") == "SHA256SUMS":
            sums = url
    fields = [tag, release.get("html_url") or "", "1" if release.get("draft") else "0",
              "1" if release.get("prerelease") else "0", asset, sums, name]
    for value in fields:
        if not isinstance(value, str) or any(c in value for c in "\r\n\"\\"):
            raise ValueError(value)
    # The notes only add to the answer: a body that can't be read leaves them empty
    try:
        fields += notes_of(release.get("body"))
    except Exception:
        fields += ["", ""]
    # UTF-8 whatever the locale (the notes may be Japanese)
    sys.stdout.buffer.write(("\n".join(fields) + "\n").encode("utf-8"))
except Exception as error:  # anything unexpected in the answer
    print(error, file=sys.stderr)
    sys.exit(1)
EOF
)
    _rc=$?
    rm -f "$_json"
    if [ "$_rc" -ne 0 ]; then
        err_code=bad-response
        err_msg="cannot read GitHub's answer"
        return 1
    fi
    # One value per line (the parser made sure none contains a newline); some may be empty
    rel_tag=$(printf '%s\n' "$_fields" | sed -n 1p)
    rel_page=$(printf '%s\n' "$_fields" | sed -n 2p)
    rel_draft=$(printf '%s\n' "$_fields" | sed -n 3p)
    rel_pre=$(printf '%s\n' "$_fields" | sed -n 4p)
    rel_asset_url=$(printf '%s\n' "$_fields" | sed -n 5p)
    rel_sums_url=$(printf '%s\n' "$_fields" | sed -n 6p)
    rel_asset_name=$(printf '%s\n' "$_fields" | sed -n 7p)
    rel_notes=$(printf '%s\n' "$_fields" | sed -n 8p)
    rel_notes_ja=$(printf '%s\n' "$_fields" | sed -n 9p)
    return 0
}

# True if the state file is a "done" older than done_ttl (or dated in the future).
done_is_stale() {
    [ "$(json_str "$state_file" state)" = done ] || return 1
    _updated=$(json_raw "$state_file" updated_at)
    _age=$(($(now) - ${_updated:-0}))
    [ "$_age" -lt 0 ] || [ "$_age" -gt "$done_ttl" ]
}

# Remove a stale "done", so a later hand install of an older version isn't shown as "installed".
clear_stale_done() {
    if done_is_stale; then
        rm -f "$state_file"
    fi
}

# ---------------------------------------------------------------------------------------------
# check

# Print the check result for the cache file (fresh or reused) and exit.
report_check() { # cached(true/false)
    _checked=$(json_raw "$check_file" checked_at)
    _err=$(json_str "$check_file" error)
    if [ -n "$_err" ]; then
        printf '{"status":"error","error":"%s","message":"%s","current":"%s","cached":%s,"checked_at":%s}\n' \
            "$_err" "$(json_str "$check_file" message)" "$(json_safe "$current")" "$1" "${_checked:-0}"
        exit 1
    fi
    _latest=$(json_str "$check_file" latest)
    _page=$(json_str "$check_file" url)
    _extra=
    _status=up-to-date
    if [ -n "$_latest" ]; then
        if ! _latest_core=$(version_core "$_latest"); then
            printf '{"status":"error","error":"bad-version","message":"%s","current":"%s","cached":%s,"checked_at":%s}\n' \
                "release tag $(json_safe "$_latest") is not a version" "$(json_safe "$current")" "$1" "${_checked:-0}"
            exit 1
        fi
        if [ "$(vercmp "$_latest_core" "$current_core")" = 1 ]; then
            _status=update-available
            if [ -n "$asset_pattern" ]; then
                if [ "$(json_raw "$check_file" has_asset)" != true ]; then
                    _extra=',"installable":false,"reason":"no-asset"'
                elif [ "$(json_raw "$check_file" has_sums)" != true ]; then
                    _extra=',"installable":false,"reason":"no-checksums"'
                else
                    _extra=',"installable":true'
                fi
            fi
            # The new release's summary, and the Japanese one if its text has one ("" if not)
            _extra="$_extra,\"notes\":\"$(json_str "$check_file" notes)\",\"notes_ja\":\"$(json_str "$check_file" notes_ja)\""
        fi
    fi
    printf '{"status":"%s","current":"%s","latest":"%s","url":"%s"%s,"cached":%s,"checked_at":%s}\n' \
        "$_status" "$(json_safe "$current")" "$_latest" "$_page" "$_extra" "$1" "${_checked:-0}"
    exit 0
}

cmd_check() {
    mkdir -p "$cache_dir" || usage_error "cannot create $cache_dir"
    clear_stale_done
    _source="$(json_safe "$api_base/repos/$repo/releases/latest") $(json_safe "$asset_pattern")"
    # An answer kept by frame-update 0.1.0 has no notes: ask GitHub again once
    if [ "$force" != 1 ] && [ -f "$check_file" ] && [ "$(json_str "$check_file" source)" = "$_source" ] &&
        { [ -n "$(json_str "$check_file" error)" ] || grep -q '"notes":' "$check_file"; }; then
        _checked=$(json_raw "$check_file" checked_at)
        _age=$(($(now) - ${_checked:-0}))
        _ttl=$check_ttl
        [ -z "$(json_str "$check_file" error)" ] || _ttl=$error_ttl
        if [ "$_age" -ge 0 ] && [ "$_age" -lt "$_ttl" ]; then
            report_check true
        fi
    fi
    _tmp="$check_file.tmp.$$"
    if fetch_release; then
        _latest=$(json_safe "${rel_tag#[vV]}")
        # /releases/latest skips drafts and prereleases already; this is only a safety net
        [ "$rel_draft$rel_pre" = 00 ] || _latest=
        _has_asset=false
        _has_sums=false
        [ -z "$rel_asset_url" ] || _has_asset=true
        [ -z "$rel_sums_url" ] || _has_sums=true
        printf '{"source":"%s","latest":"%s","url":"%s","has_asset":%s,"has_sums":%s,"notes":"%s","notes_ja":"%s","checked_at":%s}\n' \
            "$_source" "$_latest" "$(json_safe "$rel_page")" "$_has_asset" "$_has_sums" \
            "$(json_safe "$rel_notes")" "$(json_safe "$rel_notes_ja")" "$(now)" >"$_tmp"
    else
        log "check failed: $err_msg"
        printf '{"source":"%s","error":"%s","message":"%s","checked_at":%s}\n' \
            "$_source" "$err_code" "$(json_safe "$err_msg")" "$(now)" >"$_tmp"
    fi
    mv -f "$_tmp" "$check_file"
    report_check false
}

# ---------------------------------------------------------------------------------------------
# install

# Write the state file (atomically).
write_state() { # state step version error message
    _tmp="$state_file.tmp.$$"
    {
        printf '{"state":"%s"' "$1"
        [ -z "$2" ] || printf ',"step":"%s"' "$2"
        [ -z "$3" ] || printf ',"version":"%s"' "$(json_safe "$3")"
        [ -z "$4" ] || printf ',"error":"%s"' "$4"
        [ -z "$5" ] || printf ',"message":"%s"' "$(json_safe "$5")"
        if [ "$1" = running ] && [ "$detach" != 1 ]; then
            printf ',"pid":%s' "$$"
        fi
        if [ "$1" = running ] && [ -n "$this_boot" ]; then
            printf ',"boot_id":"%s"' "$this_boot"
        fi
        printf ',"updated_at":%s}\n' "$(now)"
    } >"$_tmp" && mv -f "$_tmp" "$state_file"
}

# Stop the install with an error: record it, print the state and exit 1.
fail() { # error message
    log "failed ($1): $2"
    write_state failed "" "${target_version:-}" "$1" "$2"
    finished=1
    cat "$state_file"
    exit 1
}

# Stop before touching the state file (another install owns it) and exit 1.
refuse() { # error message
    log "$2"
    printf '{"state":"failed","error":"%s","message":"%s","updated_at":%s}\n' "$1" "$(json_safe "$2")" "$(now)"
    exit 1
}

# True if the install lock is held by a live process of this boot.
lock_busy() {
    [ -d "$lock_dir" ] || return 1
    pid_alive "$(cat "$lock_dir/pid" 2>/dev/null)" "$(cat "$lock_dir/boot_id" 2>/dev/null)"
}

# Write our PID and boot into the lock we just made.
mark_lock() {
    echo "$this_boot" >"$lock_dir/boot_id"
    echo "$$" >"$lock_dir/pid"
}

acquire_lock() {
    if mkdir "$lock_dir" 2>/dev/null; then
        mark_lock
        return 0
    fi
    lock_busy && return 1
    # Left behind by an install that was killed, or by one from before a reboot
    rm -rf "$lock_dir"
    mkdir "$lock_dir" 2>/dev/null || return 1
    mark_lock
}

on_exit() {
    if [ "$finished" != 1 ]; then
        log "interrupted"
        write_state failed "" "${target_version:-}" interrupted "the update was interrupted"
    fi
    # The download and the extracted release, whether the install worked, failed or was stopped
    # (the log and the state file stay)
    case $ver_dir in
        "$update_dir"/?*) rm -rf "$ver_dir" ;;
    esac
    if [ "$(cat "$lock_dir/pid" 2>/dev/null)" = "$$" ]; then
        rm -rf "$lock_dir"
    fi
}

# True if ARG may be passed on to install.sh.
valid_install_arg() {
    case $1 in
        --uninstall | --uninstall=* | --purge | --purge=*) return 1 ;;
        --[A-Za-z0-9]*) ;;
        *) return 1 ;;
    esac
    case $1 in *[!A-Za-z0-9._=/:,@+-]*) return 1 ;; esac
    return 0
}

# Check that the archive only holds plain files and folders below the extraction folder. Python
# reads the raw entries: some tar programs (busybox) quietly rewrite "/" and ".." when listing.
check_archive() { # archive
    _why=$(python3 - "$1" 2>>"$log_file" <<'EOF'
import sys
import tarfile

try:
    with tarfile.open(sys.argv[1], "r:gz") as tar:
        members = tar.getmembers()
except Exception as error:
    print(f"cannot read it ({error})")
    sys.exit(1)
if not members:
    print("it is empty")
    sys.exit(1)
for member in members:
    name = member.name
    if name.startswith("/") or ".." in name.split("/") or "\\" in name or any(ord(c) < 32 for c in name):
        print(f"unsafe path {name!r}")
        sys.exit(1)
    if not (member.isfile() or member.isdir()):
        print(f"link or special file {name!r}")
        sys.exit(1)
EOF
)
    [ $? -eq 0 ] || fail unsafe-archive "$asset_name: ${_why:-cannot check it}"
}

# Run the extracted install.sh with the stored options.
run_installer() { # folder
    set --
    if [ -f "$args_file" ]; then
        log "options from $args_file"
        while IFS= read -r _arg || [ -n "$_arg" ]; do
            _arg=${_arg%"$cr"}
            case $_arg in '' | '#'*) continue ;; esac
            valid_install_arg "$_arg" || fail bad-args "not allowed in $args_file: $_arg"
            set -- "$@" "$_arg"
        done <"$args_file"
    else
        _old_ifs=$IFS
        IFS=$nl
        for _arg in $install_args; do
            set -- "$@" "$_arg"
        done
        IFS=$_old_ifs
    fi
    log "running install.sh $*"
    [ -x "$installer_dir/install.sh" ] || chmod u+x "$installer_dir/install.sh"
    (cd "$installer_dir" && ./install.sh "$@") </dev/null >>"$log_file" 2>&1
}

# Hand the install to a transient systemd user unit and return at once.
cmd_detach() {
    command -v systemd-run >/dev/null 2>&1 || refuse detach-failed "systemd-run not found"
    lock_busy && refuse busy "another update of $app is running"
    case $0 in
        */*) _self=$0 ;;
        *) _self=$(command -v "$0") ;;
    esac
    [ -f "$_self" ] || refuse detach-failed "cannot find this script ($0)"
    mkdir -p "$update_dir" || refuse detach-failed "cannot create $update_dir"
    # Run a copy: the install replaces the installed script while it is being read
    _copy="$update_dir/frame-update.sh"
    cp "$_self" "$_copy.tmp.$$" && mv -f "$_copy.tmp.$$" "$_copy" || refuse detach-failed "cannot copy $_self"
    : >"$log_file"
    log "frame-update $FRAME_UPDATE_VERSION: starting the unit $app-update"
    write_state running start "" "" ""
    set -- systemd-run --user --unit="$app-update" --description="Update $app" --collect --quiet \
        --setenv=FRAME_UPDATE_DETACHED=1
    for _var in HOME XDG_CACHE_HOME XDG_CONFIG_HOME XDG_DATA_HOME XDG_RUNTIME_DIR \
        FRAME_UPDATE_API_URL FRAME_UPDATE_ALLOW_INSECURE FRAME_UPDATE_CHECK_TTL FRAME_UPDATE_ERROR_TTL; do
        eval "_isset=\${$_var+1}"
        if [ -n "$_isset" ]; then
            eval "_val=\$$_var"
            set -- "$@" "--setenv=$_var=$_val"
        fi
    done
    set -- "$@" -- /bin/sh "$_copy" --app "$app" --repo "$repo" --current "$current" --asset "$asset_pattern"
    _old_ifs=$IFS
    IFS=$nl
    for _arg in $install_args; do
        set -- "$@" --install-arg "$_arg"
    done
    IFS=$_old_ifs
    set -- "$@" install
    if ! "$@" >>"$log_file" 2>&1; then
        log "systemd-run failed"
        write_state failed "" "" detach-failed "could not start the unit $app-update (see $log_file)"
        cat "$state_file"
        exit 1
    fi
    cat "$state_file"
    exit 0
}

cmd_install() {
    [ -n "$asset_pattern" ] || usage_error "install needs --asset"
    mkdir -p "$cache_dir" || usage_error "cannot create $cache_dir"
    log_file="$cache_dir/update.log"
    if [ "$detach" = 1 ]; then
        cmd_detach
    fi
    finished=0
    target_version=
    ver_dir=
    acquire_lock || refuse busy "another update of $app is running"
    trap on_exit EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    clear_stale_done
    # Work folders left by an install that was killed (the running copy of this script is a file, kept)
    for _old in "$update_dir"/*/; do
        [ -d "$_old" ] && rm -rf "$_old"
    done
    # A detached run keeps the log started by cmd_detach
    [ "${FRAME_UPDATE_DETACHED:-}" = 1 ] || : >"$log_file"
    log "frame-update $FRAME_UPDATE_VERSION: updating $app from $current (PID $$)"
    write_state running start "" "" ""

    fetch_release || fail "$err_code" "$err_msg"
    [ "$rel_draft$rel_pre" = 00 ] || fail not-found "the latest release is a draft or prerelease"
    latest_core=$(version_core "$rel_tag") || fail bad-version "release tag $rel_tag is not a version"
    target_version=${rel_tag#[vV]}
    if [ "$(vercmp "$latest_core" "$current_core")" != 1 ]; then
        fail not-newer "the latest release $target_version is not newer than $current"
    fi
    asset_name=$rel_asset_name
    [ -n "$rel_asset_url" ] || fail no-asset "release $rel_tag has no $asset_name"
    [ -n "$rel_sums_url" ] || fail no-checksums "release $rel_tag has no SHA256SUMS; update by hand"
    log "latest release: $rel_tag ($rel_page)"

    ver_dir="$update_dir/$latest_core"
    rm -rf "$ver_dir"
    mkdir -p "$ver_dir/files" || fail io "cannot create $ver_dir"
    write_state running download "$target_version" "" ""
    log "downloading $rel_asset_url"
    download "$rel_asset_url" "$ver_dir/$asset_name" 600 || fail "$err_code" "$err_msg"
    download "$rel_sums_url" "$ver_dir/SHA256SUMS" 60 || fail "$err_code" "$err_msg"

    write_state running verify "$target_version" "" ""
    _expected=$(awk -v n="$asset_name" '$2 == n || $2 == "*" n { print tolower($1); exit }' "$ver_dir/SHA256SUMS")
    case $_expected in
        '' | *[!0-9a-f]*) fail no-checksums "SHA256SUMS has no entry for $asset_name; update by hand" ;;
    esac
    [ "${#_expected}" -eq 64 ] || fail no-checksums "SHA256SUMS has a malformed entry for $asset_name"
    _actual=$(sha256sum "$ver_dir/$asset_name" | cut -c1-64)
    [ "$_actual" = "$_expected" ] || fail checksum-mismatch "$asset_name has SHA-256 $_actual, SHA256SUMS says $_expected"
    log "SHA-256 ok: $_actual"

    write_state running extract "$target_version" "" ""
    check_archive "$ver_dir/$asset_name"
    tar -xzf "$ver_dir/$asset_name" -C "$ver_dir/files" 2>>"$log_file" || fail unsafe-archive "cannot extract $asset_name"
    installer_dir=
    if [ -f "$ver_dir/files/install.sh" ]; then
        installer_dir="$ver_dir/files"
    else
        _count=0
        for _entry in "$ver_dir/files"/*; do
            [ -e "$_entry" ] || continue
            _count=$((_count + 1))
            _top=$_entry
        done
        if [ "$_count" -eq 1 ] && [ -f "$_top/install.sh" ]; then
            installer_dir=$_top
        fi
    fi
    [ -n "$installer_dir" ] || fail no-installer "$asset_name has no install.sh"

    write_state running install "$target_version" "" ""
    run_installer
    _rc=$?
    [ "$_rc" -eq 0 ] || fail install-failed "install.sh exited with $_rc (see $log_file)"

    log "updated $app to $target_version"
    write_state done "" "$target_version" "" ""
    finished=1
    cat "$state_file"
    exit 0
}

# ---------------------------------------------------------------------------------------------
# status

cmd_status() {
    if [ ! -f "$state_file" ] || done_is_stale; then
        echo '{"state":"idle"}'
        exit 0
    fi
    if [ "$(json_str "$state_file" state)" = running ]; then
        _pid=$(json_raw "$state_file" pid)
        _boot=$(json_str "$state_file" boot_id)
        _updated=$(json_raw "$state_file" updated_at)
        _age=$(($(now) - ${_updated:-0}))
        # Gone: written in another boot, its PID is not ours and alive, or no PID long after the start
        if { [ -n "$_boot" ] && [ -n "$this_boot" ] && [ "$_boot" != "$this_boot" ]; } ||
            { [ -n "$_pid" ] && ! pid_alive "$_pid" "$_boot"; } ||
            { [ -z "$_pid" ] && [ "$_age" -gt "$start_grace" ]; }; then
            printf '{"state":"failed","version":"%s","error":"interrupted","message":"the update was interrupted","updated_at":%s}\n' \
                "$(json_str "$state_file" version)" "$(json_raw "$state_file" updated_at)"
            exit 0
        fi
    fi
    cat "$state_file"
    exit 0
}

# ---------------------------------------------------------------------------------------------

main() {
    app=
    repo=
    current=
    asset_pattern=
    force=0
    detach=0
    install_args=
    cmd=
    while [ $# -gt 0 ]; do
        case $1 in
            --app | --repo | --current | --asset | --install-arg)
                [ $# -ge 2 ] || usage_error "$1 needs a value"
                case $1 in
                    --app) app=$2 ;;
                    --repo) repo=$2 ;;
                    --current) current=$2 ;;
                    --asset) asset_pattern=$2 ;;
                    --install-arg)
                        valid_install_arg "$2" || usage_error "not allowed: --install-arg $2"
                        install_args="$install_args$nl$2"
                        ;;
                esac
                shift 2
                ;;
            --force) force=1; shift ;;
            --detach) detach=1; shift ;;
            --version) echo "frame-update $FRAME_UPDATE_VERSION"; exit 0 ;;
            -h | --help) usage; exit 0 ;;
            check | install | status)
                [ -z "$cmd" ] || usage_error "more than one command"
                cmd=$1
                shift
                ;;
            vercmp)
                # For tests: frame-update.sh vercmp A B
                [ $# -eq 3 ] || usage_error "vercmp needs two versions"
                _a=$(version_core "$2") && _b=$(version_core "$3") || usage_error "not a version"
                vercmp "$_a" "$_b"
                exit 0
                ;;
            *) usage_error "unknown argument: $1" ;;
        esac
    done
    [ -n "$cmd" ] || usage_error "no command (check, install or status)"
    case $app in '' | .* | *[!a-z0-9._-]*) usage_error "--app must be a name like frameeyeosc" ;; esac
    case $asset_pattern in */* | *\\*) usage_error "--asset must be a file name" ;; esac
    for _ttl in "$check_ttl" "$error_ttl"; do
        case $_ttl in '' | *[!0-9]*) usage_error "FRAME_UPDATE_*_TTL must be a number of seconds" ;; esac
    done

    cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/$app"
    config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/$app"
    check_file="$cache_dir/update-check.json"
    state_file="$cache_dir/update-state.json"
    update_dir="$cache_dir/update"
    work_dir="$update_dir"
    lock_dir="$cache_dir/update.lock"
    args_file="$config_dir/install-args"

    if [ "$cmd" = status ]; then
        cmd_status
    fi
    case $repo in
        */*/* | /* | */ | *[!A-Za-z0-9._/-]*) usage_error "--repo must look like owner/name" ;;
        */*) ;;
        *) usage_error "--repo must look like owner/name" ;;
    esac
    current_core=$(version_core "$current") || usage_error "--current is not a version: $current"
    case $cmd in
        check) cmd_check ;;
        install) cmd_install ;;
    esac
}

# The whole script is read before main runs, so replacing the file during an install is harmless
main "$@"
exit $?
