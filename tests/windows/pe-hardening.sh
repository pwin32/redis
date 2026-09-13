#!/usr/bin/env bash
# Check executable hardening in a local build or an extracted Windows package.
set -euo pipefail
export LC_ALL=C

die() {
    echo "error: $*" >&2
    exit 1
}

if (( $# != 1 )); then
    echo "usage: $0 BINARY_DIR" >&2
    exit 2
fi
[[ -d "$1" ]] || die "binary directory not found: $1"
binary_dir="$(cd "$1" && pwd -P)"
command -v objdump >/dev/null 2>&1 || die "objdump is required"

for required in redis-server.exe redis-cli.exe redis-benchmark.exe \
                redis-check-aof.exe redis-check-rdb.exe; do
    [[ -s "$binary_dir/$required" ]] || die "missing executable: $required"
done

for pe_file in "$binary_dir"/*.exe; do
    pe_name="$(basename "$pe_file")"
    case "$pe_name" in
        redis-server.exe|redis-sentinel.exe|redis-check-aof.exe|redis-check-rdb.exe)
            aslr=0
            ;;
        redis-cli.exe|redis-benchmark.exe|hiredis-test.exe|interop-test.exe|\
        redis-test-launcher.exe|redis-affinity-test.exe)
            aslr=1
            ;;
        *)
            die "no executable hardening policy for $pe_name"
            ;;
    esac

    format="$(objdump -f "$pe_file")"
    [[ "$format" == *"file format pei-x86-64"* ]] ||
        die "$pe_name is not a 64-bit PE image"
    fields="$(objdump -p "$pe_file" | awk '
        $1 == "Characteristics" || $1 == "DllCharacteristics" ||
        ($1 == "Entry" && $2 == "5") {print}
    ')"
    dll_characteristics="$(awk '$1 == "DllCharacteristics" {print $2}' <<< "$fields")"
    [[ "$dll_characteristics" =~ ^[0-9A-Fa-f]+$ ]] ||
        die "$pe_name has no readable PE DllCharacteristics field"
    dll_flags=$((16#$dll_characteristics))
    (( (dll_flags & 0x0100) != 0 )) || die "$pe_name does not retain NX compatibility"

    relocations=not-required
    if (( aslr )); then
        (( (dll_flags & 0x0040) != 0 )) || die "$pe_name does not enable dynamic-base ASLR"
        (( (dll_flags & 0x0020) != 0 )) || die "$pe_name does not enable high-entropy ASLR"
        characteristics="$(awk '$1 == "Characteristics" {sub(/^0x/, "", $2); print $2}' <<< "$fields")"
        [[ "$characteristics" =~ ^[0-9A-Fa-f]+$ ]] ||
            die "$pe_name has no readable PE Characteristics field"
        (( (16#$characteristics & 0x0001) == 0 )) ||
            die "$pe_name marks base relocations as stripped"
        relocation_rva="$(awk '$1 == "Entry" && $2 == "5" {print $3}' <<< "$fields")"
        relocation_size="$(awk '$1 == "Entry" && $2 == "5" {print $4}' <<< "$fields")"
        [[ "$relocation_rva" =~ ^[0-9A-Fa-f]+$ && "$relocation_size" =~ ^[0-9A-Fa-f]+$ ]] ||
            die "$pe_name has no readable base relocation directory"
        (( 16#$relocation_rva != 0 && 16#$relocation_size != 0 )) ||
            die "$pe_name has an empty base relocation directory"
        sections="$(objdump -h "$pe_file")"
        relocation_section_size="$(awk '$2 == ".reloc" {print $3}' <<< "$sections")"
        [[ "$relocation_section_size" =~ ^[0-9A-Fa-f]+$ ]] ||
            die "$pe_name has no .reloc section"
        (( 16#$relocation_section_size >= 16#$relocation_size )) ||
            die "$pe_name has a truncated .reloc section"
        relocations=present
    else
        (( (dll_flags & 0x0040) == 0 )) ||
            die "$pe_name unexpectedly enables dynamic-base ASLR incompatible with QFork"
        (( (dll_flags & 0x0020) == 0 )) ||
            die "$pe_name unexpectedly enables high-entropy ASLR incompatible with QFork"
    fi
    printf '%s\tDllCharacteristics=0x%08x\tNX_COMPAT=1\tDYNAMIC_BASE=%d\tHIGH_ENTROPY_VA=%d\tRELOCATIONS=%s\n' \
        "$pe_name" "$dll_flags" "$aslr" "$aslr" "$relocations"
done
