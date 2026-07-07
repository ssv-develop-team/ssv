#!/bin/bash

append_ssv_cuda_wheel_libs() {
    local site_packages
    for site_packages in "$SSV_ROOT"/.venv/lib/python*/site-packages; do
        [ -d "$site_packages/nvidia" ] || continue
        local lib_dir
        for lib_dir in "$site_packages"/nvidia/*/lib; do
            append_ld_path_if_dir "$lib_dir"
        done
    done
}
