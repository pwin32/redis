#!/usr/bin/env bash
# Generate sanitized release evidence and an SPDX 2.3 SBOM from a package that
# has already passed the complete Windows qualification workflow.

set -euo pipefail
export LC_ALL=C

usage() {
    echo "usage: $0 ARCHIVE EXTRACTED_DIR BENCHMARK_DIR OUTPUT_DIR SOURCE_BRANCH WINDOWS_REVISION BASELINE_REFERENCE GATE_JSON TOOLCHAIN_FILE IDENTITY_FILE"
}

die() {
    echo "error: $*" >&2
    exit 1
}

if (( $# != 10 )); then
    usage >&2
    exit 2
fi

archive=$1
extracted_dir=$2
benchmark_dir=$3
output_dir=$4
source_branch=$5
windows_revision=$6
baseline_reference=$7
gate_json=$8
toolchain_file=$9
identity_file=${10}

for tool in awk date find git grep install jq sed sha1sum sha256sum sort tr wc; do
    command -v "$tool" >/dev/null 2>&1 || die "required evidence tool not found: $tool"
done
[[ -f "$archive" ]] || die "archive not found: $archive"
[[ -f "$archive.sha256" ]] || die "archive checksum not found: $archive.sha256"
[[ -d "$extracted_dir" ]] || die "extracted package not found: $extracted_dir"
[[ -f "$benchmark_dir/summary.tsv" ]] || die "benchmark summary not found"
[[ -f "$benchmark_dir/assessment.tsv" ]] || die "benchmark assessment not found"
[[ -f "$gate_json" ]] || die "qualification gate JSON not found: $gate_json"
[[ -f "$toolchain_file" ]] || die "toolchain inventory not found: $toolchain_file"
[[ -f "$identity_file" ]] || die "package binary identity report not found: $identity_file"
[[ "$windows_revision" =~ ^[1-9][0-9]*$ ]] || die "invalid Windows revision"
[[ -n "$baseline_reference" ]] || die "benchmark baseline reference is empty"
jq -e 'type == "object" and (.policy | type == "string")' "$gate_json" >/dev/null ||
    die "qualification gate JSON is invalid"

(
    cd "$(dirname "$archive")"
    sha256sum -c "$(basename "$archive").sha256"
) >/dev/null || die "archive checksum verification failed"

mkdir -p "$output_dir"
if find "$output_dir" -mindepth 1 -print -quit | grep -q .; then
    die "evidence output directory must be empty: $output_dir"
fi

buildinfo="$extracted_dir/BUILDINFO.txt"
[[ -f "$buildinfo" ]] || die "BUILDINFO.txt is missing from extracted package"
version="$(sed -n 's/^Redis version: //p' "$buildinfo")"
source_commit="$(sed -n 's/^Source commit: //p' "$buildinfo")"
source_tree="$(sed -n 's/^Source tree: //p' "$buildinfo")"
buildinfo_revision="$(sed -n 's/^Windows package revision: //p' "$buildinfo")"
buildinfo_tag="$(sed -n 's/^Release tag: //p' "$buildinfo")"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "invalid BUILDINFO version"
[[ "$source_commit" =~ ^[0-9a-f]{40}$ ]] || die "invalid BUILDINFO source commit"
[[ "$source_tree" =~ ^[0-9a-f]{40}$ ]] || die "invalid BUILDINFO source tree"
[[ "$buildinfo_revision" == "$windows_revision" ]] || die "BUILDINFO Windows revision mismatch"
[[ -n "$source_branch" ]] || die "source branch is empty"

release_tag="v${version}-windows.${windows_revision}"
[[ "$buildinfo_tag" == "$release_tag" ]] || die "BUILDINFO release tag mismatch"
[[ "$(git rev-parse HEAD)" == "$source_commit" ]] || die "BUILDINFO source commit does not match checkout"
[[ "$(git rev-parse 'HEAD^{tree}')" == "$source_tree" ]] || die "BUILDINFO source tree does not match checkout"
package_name="$(basename "$archive")"
archive_sha="$(sha256sum "$archive" | sed 's/[[:space:]].*$//')"
created_utc="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
repository="${GITHUB_REPOSITORY:-pwin32/redis}"
workflow_run_id="${GITHUB_RUN_ID:-unknown}"
workflow_run_attempt="${GITHUB_RUN_ATTEMPT:-unknown}"
workflow_url="https://github.com/${repository}/actions/runs/${workflow_run_id}"

install -m 0644 "$archive" "$output_dir/$package_name"
install -m 0644 "$archive.sha256" "$output_dir/SHA256SUMS.txt"
install -m 0644 "$buildinfo" "$output_dir/BUILDINFO.txt"
install -m 0644 "$extracted_dir/PACKAGE-MANIFEST.txt" "$output_dir/package-manifest.txt"
install -m 0644 "$toolchain_file" "$output_dir/toolchain.txt"
install -m 0644 "$identity_file" "$output_dir/package-binary-identity.tsv"

awk -F '\t' '
    function csv(value) {
        gsub(/"/, "\"\"", value)
        return "\"" value "\""
    }
    {
        for (column = 1; column <= NF; column++) {
            printf "%s%s", column == 1 ? "" : ",", csv($column)
        }
        printf "\n"
    }
' "$benchmark_dir/summary.tsv" > "$output_dir/benchmark.csv"

awk -F '\t' '
    NR == 1 {
        if ($1 != "matrix_id" || $2 != "test") invalid = 1
        next
    }
    { rows++ }
    END { if (invalid || rows == 0) exit 1 }
' "$benchmark_dir/summary.tsv" || die "benchmark summary is malformed or empty"
benchmark_status="$(awk -F '\t' '
    NR == 1 {
        if ($1 != "metric" || $2 != "value" || $3 != "criterion" || $4 != "pass") invalid = 1
        next
    }
    $1 == "regression_advisory" {
        matches++
        status = $4
    }
    END {
        if (invalid || matches != 1) exit 1
        print status
    }
' "$benchmark_dir/assessment.tsv")" || die "benchmark assessment is malformed"
case "$benchmark_status" in
    pass|warn) ;;
    *) die "invalid benchmark advisory status: $benchmark_status" ;;
esac

IFS= read -r -d '' report_template <<'EOF' || true
# Redis __VERSION__ Windows qualification

- Release candidate: `__RELEASE_TAG__`
- Canonical branch: `__SOURCE_BRANCH__`
- Source commit: `__SOURCE_COMMIT__`
- Source tree: `__SOURCE_TREE__`
- Workflow run: __WORKFLOW_URL__ (attempt __WORKFLOW_ATTEMPT__)
- Completed: __CREATED_UTC__

| Qualification gate | Result |
| --- | --- |
| Public-source hygiene over full Git history | Passed |
| MinGW64 core, interop, launcher, and package build | Passed |
| Redis root test suite | Passed |
| Cluster test suite | Passed |
| Sentinel test suite | Passed |
| Module API test suite | Passed |
| Windows legacy and modern interop tests | Passed |
| Isolated Windows service and Event Log test | Passed |
| ZIP checksum, extraction, manifest, licenses, PE and import audit | Passed |
| Release binaries identical to the bytes used by source-tree suites | Passed |
| Packaged master/replica full and incremental synchronization | Passed |
| Packaged QFork RDB/AOF persistence and restart soak (30 minutes minimum) | Passed |
| Short loopback package benchmark completed | Passed |

## Benchmark

The benchmark uses the packaged client and server on loopback with persistence
disabled, one benchmark thread, 50 clients, pipeline 1, a discarded 10,000
request warmup, and three 50,000 request rounds for PING, SET, and GET. The
baseline reference is `__BASELINE_REFERENCE__`. A throughput decrease or p95
latency increase greater than 15% is advisory and does not block publication.
The advisory result for this run is `__BENCHMARK_STATUS__`; exact medians are in
`benchmark.csv`.

## Toolchain

The resolved MSYS2/MinGW64 package versions used by this workflow are recorded
in `toolchain.txt` and in the archive's `BUILDINFO.txt`. This release does
not claim bit-for-bit reproducibility across changing hosted-runner toolchains.
EOF
report="${report_template//__VERSION__/$version}"
report="${report//__RELEASE_TAG__/$release_tag}"
report="${report//__SOURCE_BRANCH__/$source_branch}"
report="${report//__SOURCE_COMMIT__/$source_commit}"
report="${report//__SOURCE_TREE__/$source_tree}"
report="${report//__WORKFLOW_URL__/$workflow_url}"
report="${report//__WORKFLOW_ATTEMPT__/$workflow_run_attempt}"
report="${report//__CREATED_UTC__/$created_utc}"
report="${report//__BASELINE_REFERENCE__/$baseline_reference}"
report="${report//__BENCHMARK_STATUS__/$benchmark_status}"
report_file="$output_dir/TEST-REPORT.md"
printf '%s\n' "$report" > "$report_file"
[[ "$report" != *__* ]] || die "release report contains an unresolved template placeholder"
grep -Fq -- "- Release candidate: \`$release_tag\`" "$report_file" ||
    die "release report is missing the release tag"
grep -Fq -- "- Canonical branch: \`$source_branch\`" "$report_file" ||
    die "release report is missing the canonical branch"
grep -Fq -- "- Source commit: \`$source_commit\`" "$report_file" ||
    die "release report is missing the source commit"
grep -Fq -- "baseline reference is \`$baseline_reference\`." "$report_file" ||
    die "release report is missing the benchmark baseline"
grep -Fq -- "The advisory result for this run is \`$benchmark_status\`;" "$report_file" ||
    die "release report is missing the benchmark assessment"

files_json="$output_dir/.spdx-files.ndjson"
relationships_json="$output_dir/.spdx-relationships.ndjson"
verification_sha1s="$output_dir/.spdx-file-sha1s"
: > "$files_json"
: > "$relationships_json"
: > "$verification_sha1s"
file_count=0
while IFS= read -r package_file; do
    file_name="$(basename "$package_file")"
    file_sha1="$(sha1sum "$package_file" | sed 's/[[:space:]].*$//')"
    file_sha="$(sha256sum "$package_file" | sed 's/[[:space:]].*$//')"
    # File content is not a unique identifier: several Redis executables can
    # intentionally be byte-identical. Include the deterministic package file
    # name in the SPDX identifier so every file and relationship has a unique
    # target while remaining stable across CI runs.
    file_name_sha="$(printf '%s' "$file_name" | sha256sum | sed 's/[[:space:]].*$//')"
    file_id="SPDXRef-File-${file_name_sha:0:16}-${file_sha:0:16}"
    case "$file_name" in
        *.exe|*.dll) file_type=BINARY ;;
        *) file_type=TEXT ;;
    esac
    jq -cn \
        --arg id "$file_id" \
        --arg name "./$file_name" \
        --arg sha1 "$file_sha1" \
        --arg sha "$file_sha" \
        --arg type "$file_type" \
        '{SPDXID:$id,fileName:$name,checksums:[{algorithm:"SHA1",checksumValue:$sha1},{algorithm:"SHA256",checksumValue:$sha}],fileTypes:[$type],licenseConcluded:"NOASSERTION",copyrightText:"NOASSERTION"}' \
        >> "$files_json"
    jq -cn \
        --arg id "$file_id" \
        '{spdxElementId:"SPDXRef-Package",relationshipType:"CONTAINS",relatedSpdxElement:$id}' \
        >> "$relationships_json"
    printf '%s\n' "$file_sha1" >> "$verification_sha1s"
    file_count=$((file_count + 1))
done < <(find "$extracted_dir" -mindepth 1 -maxdepth 1 -type f | LC_ALL=C sort)
(( file_count > 0 )) || die "extracted package contains no files"
package_verification_code="$(LC_ALL=C sort "$verification_sha1s" | tr -d '\n' | sha1sum | sed 's/[[:space:]].*$//')"

jq -n \
    --arg created "$created_utc" \
    --arg namespace "https://github.com/${repository}/sbom/${source_commit}/${workflow_run_id}/attempt-${workflow_run_attempt}" \
    --arg name "Redis-x64-${version}-mingw-r${windows_revision}" \
    --arg version "$version" \
    --arg package_file_name "$package_name" \
    --arg archive_sha "$archive_sha" \
    --arg package_verification_code "$package_verification_code" \
    --slurpfile files "$files_json" \
    --slurpfile contains "$relationships_json" \
    '{
        spdxVersion:"SPDX-2.3",
        dataLicense:"CC0-1.0",
        SPDXID:"SPDXRef-DOCUMENT",
        name:($name + " SBOM"),
        documentNamespace:$namespace,
        creationInfo:{created:$created,creators:["Tool: pwin32-release-evidence/1"]},
        packages:[{
            SPDXID:"SPDXRef-Package",
            name:$name,
            versionInfo:$version,
            packageFileName:$package_file_name,
            downloadLocation:"NOASSERTION",
            filesAnalyzed:true,
            packageVerificationCode:{packageVerificationCodeValue:$package_verification_code},
            checksums:[{algorithm:"SHA256",checksumValue:$archive_sha}],
            licenseConcluded:"NOASSERTION",
            licenseDeclared:"NOASSERTION",
            copyrightText:"NOASSERTION"
        }],
        files:$files,
        relationships:([{
            spdxElementId:"SPDXRef-DOCUMENT",
            relationshipType:"DESCRIBES",
            relatedSpdxElement:"SPDXRef-Package"
        }] + $contains)
    }' > "$output_dir/sbom.spdx.json"
rm -f "$files_json" "$relationships_json" "$verification_sha1s"

jq -e --arg package_verification_code "$package_verification_code" '
    ([.SPDXID] + [.packages[]?.SPDXID] + [.files[]?.SPDXID]) as $ids |
    ($ids | length == (unique | length)) and
    (.packages | length == 1) and
    (.packages[0].filesAnalyzed == true) and
    (.packages[0].packageVerificationCode.packageVerificationCodeValue == $package_verification_code) and
    (all(.files[];
        ([.checksums[] | select(.algorithm == "SHA1")] | length == 1) and
        ([.checksums[] | select(.algorithm == "SHA256")] | length == 1))) and
    ([.relationships[]?.spdxElementId, .relationships[]?.relatedSpdxElement]
        | all(. as $id | ($ids | index($id) != null)))
' "$output_dir/sbom.spdx.json" >/dev/null ||
    die "generated SPDX SBOM has duplicate or dangling identifiers"

jq -n \
    --arg schema_version "1" \
    --arg version "$version" \
    --arg windows_revision "$windows_revision" \
    --arg tag "$release_tag" \
    --arg branch "$source_branch" \
    --arg source_commit "$source_commit" \
    --arg source_tree "$source_tree" \
    --arg archive "$package_name" \
    --arg archive_sha256 "$archive_sha" \
    --arg baseline "$baseline_reference" \
    --arg benchmark_status "$benchmark_status" \
    --arg workflow_url "$workflow_url" \
    --arg completed_utc "$created_utc" \
    --slurpfile maintained_lines "$gate_json" \
    '{
        schema_version:$schema_version,
        release:{version:$version,windows_revision:($windows_revision|tonumber),tag:$tag,branch:$branch},
        source:{commit:$source_commit,tree:$source_tree},
        package:{archive:$archive,sha256:$archive_sha256,scope:"Redis core only"},
        qualification:{workflow_url:$workflow_url,completed_utc:$completed_utc,all_required_tests:"passed",release_binary_identity:"passed",maintained_lines:$maintained_lines[0]},
        benchmark:{baseline_reference:$baseline,regression_advisory:$benchmark_status,blocking:false},
        attestations:{required_before_publication:{build_provenance:true,sbom:true},created_by:"Publish Windows release workflow"}
    }' > "$output_dir/release-evidence.json"

printf 'RELEASE_EVIDENCE_OK tag=%s archive_sha256=%s\n' "$release_tag" "$archive_sha"
