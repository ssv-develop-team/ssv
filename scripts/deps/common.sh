#!/usr/bin/env bash

# Shared helpers for dependency providers. This file is intentionally side
# effect free when sourced: it only defines functions and constants.

: "${SSV_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

if ! declare -F ssv_info >/dev/null 2>&1; then
    ssv_info() { printf '[SSV] %s\n' "$*"; }
fi
if ! declare -F ssv_warn >/dev/null 2>&1; then
    ssv_warn() { printf '[SSV] %s\n' "$*" >&2; }
fi
if ! declare -F ssv_error >/dev/null 2>&1; then
    ssv_error() { printf '[SSV] %s\n' "$*" >&2; }
fi

ssv_deps_die() {
    ssv_error "$*"
    return 1
}

ssv_deps_have_command() {
    command -v "$1" >/dev/null 2>&1
}

ssv_deps_require_command() {
    local command_name="$1"
    if ! ssv_deps_have_command "$command_name"; then
        ssv_deps_die "required command not found: $command_name"
        return 1
    fi
}

ssv_deps_abs_path() {
    local path="$1"
    case "$path" in
        /*) printf '%s\n' "$path" ;;
        *) printf '%s/%s\n' "$SSV_ROOT" "$path" ;;
    esac
}

ssv_deps_normalize_path() {
    local path="$1"
    if ssv_deps_have_command realpath; then
        realpath -m -- "$path"
        return
    fi
    local suffix=""
    local cursor="$path"
    while [ ! -d "$cursor" ]; do
        local name
        name="$(basename -- "$cursor")"
        suffix="${name}${suffix:+/$suffix}"
        local parent
        parent="$(dirname -- "$cursor")"
        [ "$parent" != "$cursor" ] || break
        cursor="$parent"
    done
    local prefix
    prefix="$(cd -- "$cursor" && pwd -P)" || return 1
    printf '%s%s%s\n' "$prefix" "${suffix:+/}" "$suffix"
}

ssv_deps_validate_scalar() {
    local name="$1"
    local value="$2"
    case "$value" in
        *$'\n'*|*$'\r'*) ssv_deps_die "$name must not contain a newline"; return 1 ;;
        *:* ) ssv_deps_die "$name must not contain ':'"; return 1 ;;
    esac
}

ssv_deps_validate_root() {
    local name="$1"
    local value="$2"
    ssv_deps_validate_scalar "$name" "$value" || return 1
    [ -n "$value" ] || { ssv_deps_die "$name must not be empty"; return 1; }

    local abs
    abs="$(ssv_deps_abs_path "$value")"
    abs="$(ssv_deps_normalize_path "$abs")"
    local root_abs
    root_abs="$(cd -- "$SSV_ROOT" && pwd -P)"
    local deps_abs="$root_abs/.deps"
    local user_abs="${HOME:-}"
    case "$abs" in
        /|"$root_abs"|"$deps_abs"|"$user_abs")
            ssv_deps_die "$name points at a broad or unsafe directory: $value"
            return 1
            ;;
    esac
    printf '%s\n' "$abs"
}

ssv_deps_join_unique() {
    local result=""
    local item existing
    local input="$1"
    local IFS=:
    read -r -a items <<< "$input"
    for item in "${items[@]}"; do
        [ -n "$item" ] || continue
        existing=false
        local current
        local result_items=()
        IFS=: read -r -a result_items <<< "$result"
        for current in "${result_items[@]}"; do
            if [ "$current" = "$item" ]; then
                existing=true
                break
            fi
        done
        [ "$existing" = false ] || continue
        if [ -n "$result" ]; then result="$result:$item"; else result="$item"; fi
    done
    printf '%s\n' "$result"
}

ssv_deps_is_system_dir() {
    local dir="$1"
    case "$dir" in
        /lib|/lib/*|/usr/lib|/usr/lib/*|/usr/lib64|/usr/lib64/*) return 0 ;;
        *) return 1 ;;
    esac
}

ssv_deps_runtime_dirs() {
    # Arguments are directories, one per argument. Keep only existing,
    # non-system directories and preserve first-seen order.
    local result="" dir
    for dir in "$@"; do
        [ -n "$dir" ] || continue
        [ -d "$dir" ] || continue
        dir="$(cd -- "$dir" && pwd -P)"
        ssv_deps_is_system_dir "$dir" && continue
        result="$(ssv_deps_join_unique "$result${result:+:}$dir")"
    done
    printf '%s\n' "$result"
}

ssv_deps_validate_path_list() {
    local name="$1"
    local value="$2"
    case "$value" in *$'\n'*|*$'\r'*) ssv_deps_die "$name must not contain a newline"; return 1 ;; esac
    local item
    local IFS=:
    read -r -a items <<< "$value"
    for item in "${items[@]}"; do
        [ -n "$item" ] || continue
        case "$item" in *:*) ssv_deps_die "$name contains an invalid path: $item"; return 1 ;; esac
    done
}

ssv_deps_pkgconfig_dir() {
    local package_name="$1"
    local dir
    dir="$(pkg-config --variable=pcfiledir "$package_name" 2>/dev/null)" || {
        ssv_deps_die "pkg-config package not found: $package_name"
        return 1
    }
    [ -n "$dir" ] || { ssv_deps_die "pkg-config returned an empty pcfiledir for $package_name"; return 1; }
    dir="$(cd -- "$dir" 2>/dev/null && pwd -P)" || {
        ssv_deps_die "pkg-config pcfiledir does not exist for $package_name: $dir"
        return 1
    }
    printf '%s\n' "$dir"
}

ssv_deps_pkgconfig_version_at_least() {
    local package_name="$1"
    local minimum="$2"
    pkg-config --exists "$package_name" || return 1
    pkg-config --atleast-version="$minimum" "$package_name"
}

ssv_deps_download() {
    local url="$1"
    local destination="$2"
    local temporary="${destination}.tmp.$$"
    mkdir -p "$(dirname -- "$destination")"
    rm -f -- "$temporary"
    if ssv_deps_have_command curl; then
        curl -fL --retry 3 "$url" -o "$temporary" || { rm -f -- "$temporary"; return 1; }
    elif ssv_deps_have_command wget; then
        wget -O "$temporary" "$url" || { rm -f -- "$temporary"; return 1; }
    else
        ssv_deps_die "curl or wget is required to download $url"
        return 1
    fi
    [ -s "$temporary" ] || { rm -f -- "$temporary"; ssv_deps_die "downloaded file is empty: $url"; return 1; }
    mv -f -- "$temporary" "$destination"
}

ssv_deps_cached_download() {
    local url="$1"
    local destination="$2"
    if [ -s "$destination" ]; then
        printf '%s\n' "$destination"
        return 0
    fi
    ssv_info "downloading $(basename -- "$destination")" >&2
    ssv_deps_download "$url" "$destination" || return 1
    printf '%s\n' "$destination"
}

ssv_deps_with_cache_retry() {
    local cache_file="$1"
    local url="$2"
    shift 2
    if "$@"; then return 0; fi
    ssv_warn "cached dependency archive is invalid; downloading it once more: $(basename -- "$cache_file")"
    rm -f -- "$cache_file"
    ssv_deps_cached_download "$url" "$cache_file" >/dev/null || return 1
    "$@"
}

ssv_deps_extract_archive() {
    local archive="$1"
    local destination="$2"
    rm -rf -- "$destination"
    mkdir -p -- "$destination"
    case "$archive" in
        *.tar.zst|*.tzst)
            if tar --help 2>/dev/null | grep -q -- '--zstd'; then
                tar --zstd -xf "$archive" -C "$destination"
            elif ssv_deps_have_command zstd; then
                zstd -dc "$archive" | tar -xf - -C "$destination"
            else
                ssv_deps_die "tar cannot read zstd archive and zstd is unavailable: $archive"
                return 1
            fi
            ;;
        *.tar.gz|*.tgz) tar -xzf "$archive" -C "$destination" ;;
        *.tar.xz|*.txz) tar -xJf "$archive" -C "$destination" ;;
        *.tar.bz2|*.tbz2) tar -xjf "$archive" -C "$destination" ;;
        *.tar) tar -xf "$archive" -C "$destination" ;;
        *.zip)
            ssv_deps_require_command unzip || return 1
            unzip -q "$archive" -d "$destination"
            ;;
        *) ssv_deps_die "unsupported archive format: $archive"; return 1 ;;
    esac
}

ssv_deps_atomic_replace_dir() {
    local candidate="$1"
    local destination="$2"
    mkdir -p -- "$(dirname -- "$destination")"
    local backup="${destination}.old.$$"
    rm -rf -- "$backup"
    if [ -e "$destination" ]; then
        mv -- "$destination" "$backup"
    fi
    if mv -- "$candidate" "$destination"; then
        rm -rf -- "$backup"
        return 0
    fi
    if [ -e "$backup" ]; then mv -- "$backup" "$destination"; fi
    return 1
}

ssv_deps_make_candidate_dir() {
    local root="$1"
    local label="$2"
    local parent
    parent="$(dirname -- "$root")"
    mkdir -p -- "$parent"
    mktemp -d "$parent/.${label}.XXXXXX"
}

ssv_deps_compile_probe() {
    local source="$1"
    local package_name="$2"
    local runtime_dirs="$3"
    local workdir
    workdir="$(mktemp -d "${TMPDIR:-/tmp}/ssv-deps-probe.XXXXXX")"
    local source_file="$workdir/probe.cpp"
    local binary="$workdir/probe"
    printf '%s\n' "$source" > "$source_file"
    local cxx="${CXX:-c++}"
    local flags
    flags="$(pkg-config --cflags --libs --static "$package_name")" || { rm -rf -- "$workdir"; return 1; }
    local flag_args=()
    if [ -n "$flags" ]; then
        mapfile -t flag_args < <(xargs -n1 printf '%s\n' <<< "$flags")
    fi
    "$cxx" -std=c++17 "$source_file" -o "$binary" "${flag_args[@]}" || { rm -rf -- "$workdir"; return 1; }
    if [ -n "$runtime_dirs" ]; then
        LD_LIBRARY_PATH="$runtime_dirs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$binary" || { rm -rf -- "$workdir"; return 1; }
    else
        "$binary" || { rm -rf -- "$workdir"; return 1; }
    fi
    rm -rf -- "$workdir"
}

ssv_deps_write_pc() {
    local path="$1"
    shift
    mkdir -p -- "$(dirname -- "$path")"
    printf '%s\n' "$@" > "$path"
}

ssv_deps_is_empty_dir() {
    local dir="$1"
    [ -d "$dir" ] || return 0
    ! find "$dir" -mindepth 1 -print -quit 2>/dev/null | grep -q .
}

ssv_deps_require_replaceable_root() {
    local root="$1"
    local validator="$2"
    shift 2
    if [ ! -e "$root" ] || ssv_deps_is_empty_dir "$root"; then
        return 0
    fi
    if "$validator" "$root" "$@" >/dev/null 2>&1; then
        return 0
    fi
    # A recognized dependency can be upgraded in place. Unknown non-empty
    # data is never overwritten.
    case "$validator" in
        ssv_onnxruntime_managed_validate)
            [ -f "$root/include/onnxruntime_cxx_api.h" ] && return 0
            ;;
        ssv_opencv_managed_validate)
            find "$root" -type f -path '*/opencv4/opencv2/core.hpp' -print -quit 2>/dev/null | grep -q . && return 0
            ;;
        ssv_tensorrt_managed_validate)
            find "$root" -type f -name NvInfer.h -print -quit 2>/dev/null | grep -q . && return 0
            ;;
    esac
    ssv_deps_die "refusing to replace non-empty unrecognized dependency root: $root"
}
