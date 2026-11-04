#!/usr/bin/env bash
# Check and install the tools needed for Saturn 4 Ultra firmware work (WSL / Ubuntu / Debian).
#
# Usage:
#   tools/setup_env.sh              check + install what is missing
#   tools/setup_env.sh --check      only report, install nothing
#   tools/setup_env.sh --all        also install optional tools (ARM cross compiler, Java for Ghidra)
#
# Python tools (ubi_reader) go into a project virtualenv: <repo>/.venv
# Activate it with:  source .venv/bin/activate

set -u

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV_DIR="$REPO_DIR/.venv"
CHECK_ONLY=0
WITH_OPTIONAL=0

for arg in "$@"; do
    case "$arg" in
        --check) CHECK_ONLY=1 ;;
        --all)   WITH_OPTIONAL=1 ;;
        -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
        *) echo "Unknown option: $arg"; exit 2 ;;
    esac
done

if [ -t 1 ]; then
    GREEN=$'\e[32m'; RED=$'\e[31m'; YELLOW=$'\e[33m'; BOLD=$'\e[1m'; RESET=$'\e[0m'
else
    GREEN=""; RED=""; YELLOW=""; BOLD=""; RESET=""
fi

# command -> apt package (required)
REQUIRED=(
    "python3:python3"
    "pip3:python3-pip"
    "git:git"
    "xxd:xxd"
    "file:file"
    "xz:xz-utils"
    "unsquashfs:squashfs-tools"
    "mkfs.ubifs:mtd-utils"
    "ubinize:mtd-utils"
    "mkimage:u-boot-tools"
    "dumpimage:u-boot-tools"
    "fakeroot:fakeroot"
    "binwalk:binwalk"
    "nmap:nmap"
    "strings:binutils"
    "md5sum:coreutils"
)

# command -> apt package (optional, only with --all)
OPTIONAL=(
    "arm-linux-gnueabihf-gcc:gcc-arm-linux-gnueabihf"
    "java:openjdk-21-jdk"
)

# venv command (or "import.<module>") -> pip package (installed in the venv)
PY_TOOLS=(
    "ubireader_extract_files:ubi_reader"
    "import.capstone:capstone"
)

# Build deps for pip packages with C extensions (python-lzo fallback etc.)
BUILD_DEPS=(python3-venv python3-dev build-essential liblzo2-dev)

have() {
    # mkfs.ubifs & co live in /usr/sbin, which is not always in a normal user's PATH
    command -v "$1" >/dev/null 2>&1 || [ -x "/usr/sbin/$1" ] || [ -x "/sbin/$1" ]
}

declare -a MISSING_PKGS=()

check_list() {
    local title="$1"; shift
    echo "${BOLD}$title${RESET}"
    local entry cmd pkg
    for entry in "$@"; do
        cmd="${entry%%:*}"; pkg="${entry#*:}"
        if have "$cmd"; then
            printf "  %-28s %sOK%s\n" "$cmd" "$GREEN" "$RESET"
        else
            printf "  %-28s %sMISSING%s  (apt: %s)\n" "$cmd" "$RED" "$RESET" "$pkg"
            [[ " ${MISSING_PKGS[*]-} " == *" $pkg "* ]] || MISSING_PKGS+=("$pkg")
        fi
    done
}

check_py_tools() {
    echo "${BOLD}Python tools (venv: $VENV_DIR)${RESET}"
    local entry cmd pkg ok
    PY_MISSING=0
    for entry in "${PY_TOOLS[@]}"; do
        cmd="${entry%%:*}"; pkg="${entry#*:}"
        if [[ "$cmd" == import.* ]]; then
            "$VENV_DIR/bin/python" -c "import ${cmd#import.}" >/dev/null 2>&1 && ok=1 || ok=0
        else
            [ -x "$VENV_DIR/bin/$cmd" ] && ok=1 || ok=0
        fi
        if [ "$ok" -eq 1 ]; then
            printf "  %-28s %sOK%s\n" "$cmd" "$GREEN" "$RESET"
        else
            printf "  %-28s %sMISSING%s  (pip: %s)\n" "$cmd" "$RED" "$RESET" "$pkg"
            PY_MISSING=1
        fi
    done
}

report() {
    MISSING_PKGS=()
    check_list "Required tools" "${REQUIRED[@]}"
    if [ "$WITH_OPTIONAL" -eq 1 ]; then
        check_list "Optional tools" "${OPTIONAL[@]}"
    fi
    check_py_tools
}

# ---------------------------------------------------------------------------

echo "${BOLD}== Saturn 4 Ultra firmware tooling check ==${RESET}"
grep -qi microsoft /proc/version 2>/dev/null && echo "Running under WSL" || echo "${YELLOW}Not WSL (fine on any Debian/Ubuntu)${RESET}"
echo

report

if [ "$CHECK_ONLY" -eq 1 ]; then
    echo
    if [ "${#MISSING_PKGS[@]}" -eq 0 ] && [ "$PY_MISSING" -eq 0 ]; then
        echo "${GREEN}Everything is installed.${RESET}"; exit 0
    fi
    echo "${YELLOW}Missing items found. Run without --check to install them.${RESET}"; exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
    echo "${RED}apt-get not found: this script supports Debian/Ubuntu only.${RESET}"; exit 1
fi

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

# --- apt packages ---------------------------------------------------------
if [ "${#MISSING_PKGS[@]}" -gt 0 ] || [ "$PY_MISSING" -eq 1 ]; then
    APT_PKGS=("${MISSING_PKGS[@]}")
    if [ "$PY_MISSING" -eq 1 ]; then
        for p in "${BUILD_DEPS[@]}"; do
            dpkg -s "$p" >/dev/null 2>&1 || APT_PKGS+=("$p")
        done
    fi
    if [ "${#APT_PKGS[@]}" -gt 0 ]; then
        echo
        echo "${BOLD}Installing apt packages:${RESET} ${APT_PKGS[*]}"
        $SUDO apt-get update || { echo "${RED}apt-get update failed${RESET}"; exit 1; }
        $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y "${APT_PKGS[@]}" \
            || { echo "${RED}apt-get install failed${RESET}"; exit 1; }
    fi
fi

# --- python venv ----------------------------------------------------------
if [ "$PY_MISSING" -eq 1 ]; then
    echo
    echo "${BOLD}Setting up Python venv in $VENV_DIR${RESET}"
    if [ ! -x "$VENV_DIR/bin/python" ]; then
        python3 -m venv "$VENV_DIR" || { echo "${RED}venv creation failed${RESET}"; exit 1; }
    fi
    "$VENV_DIR/bin/pip" install --upgrade pip >/dev/null
    for entry in "${PY_TOOLS[@]}"; do
        pkg="${entry#*:}"
        "$VENV_DIR/bin/pip" install "$pkg" || { echo "${RED}pip install $pkg failed${RESET}"; exit 1; }
    done
fi

# --- final verification ---------------------------------------------------
echo
echo "${BOLD}== Final check ==${RESET}"
report
echo
if [ "${#MISSING_PKGS[@]}" -eq 0 ] && [ "$PY_MISSING" -eq 0 ]; then
    echo "${GREEN}All tools installed.${RESET}"
    echo "Activate the Python tools with:  source $VENV_DIR/bin/activate"
    if [ "$WITH_OPTIONAL" -eq 1 ]; then
        echo "Ghidra is not in apt: download the release zip from"
        echo "  https://github.com/NationalSecurityAgency/ghidra/releases"
        echo "unzip it (e.g. to ~/tools/ghidra) and run ./ghidraRun (Java 21 is now installed)."
    fi
    exit 0
fi
echo "${RED}Some tools are still missing (see above).${RESET}"
exit 1
