#!/bin/sh

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
WORK_TREE=$(CDPATH= cd "$SCRIPT_DIR/.." && pwd)
RELEASE_GIT_DIR=

git_cmd() {
  if [ -n "$RELEASE_GIT_DIR" ]; then
    GIT_DIR="$RELEASE_GIT_DIR" GIT_WORK_TREE="$WORK_TREE" git "$@"
  else
    git -C "$WORK_TREE" "$@"
  fi
}

# A linked worktree created from WSL stores a WSL absolute path in its .git
# file (for example a /mnt/<drive>/... spelling). MSYS Git cannot resolve that
# spelling, even though the same drive is available as /<drive>/.... Fall back to the translated
# gitdir while keeping the linked worktree itself as GIT_WORK_TREE.
if ! git_cmd rev-parse --git-dir >/dev/null 2>&1 && [ -f "$WORK_TREE/.git" ]; then
  RELEASE_GIT_DIR=$(sed -n 's/^gitdir:[[:space:]]*//p' "$WORK_TREE/.git" | head -n 1 | tr -d '\r')
  case "$RELEASE_GIT_DIR" in
    /mnt/[A-Za-z]/*)
      DRIVE_AND_PATH=${RELEASE_GIT_DIR#/mnt/}
      DRIVE=${DRIVE_AND_PATH%%/*}
      DRIVE=$(printf '%s' "$DRIVE" | tr '[:upper:]' '[:lower:]')
      RELEASE_GIT_DIR="/$DRIVE/${DRIVE_AND_PATH#*/}"
      ;;
    /* | [A-Za-z]:*)
      ;;
    *)
      RELEASE_GIT_DIR="$WORK_TREE/$RELEASE_GIT_DIR"
      ;;
  esac

  if ! git_cmd rev-parse --git-dir >/dev/null 2>&1; then
    RELEASE_GIT_DIR=
  fi
fi

GIT_SHA1=$(git_cmd rev-parse --verify --short=8 HEAD 2>/dev/null || echo 00000000)
GIT_DIRTY=$(git_cmd diff --no-ext-diff -- "$WORK_TREE/src" "$WORK_TREE/deps" 2>/dev/null | wc -l)
BUILD_ID=`uname -n`"-"`date +%s`
if [ -n "$SOURCE_DATE_EPOCH" ]; then
  BUILD_ID=$(date -u -d "@$SOURCE_DATE_EPOCH" +%s 2>/dev/null || date -u -r "$SOURCE_DATE_EPOCH" +%s 2>/dev/null || date -u +%s)
fi
test -f release.h || touch release.h
(cat release.h | grep SHA1 | grep $GIT_SHA1) && \
(cat release.h | grep DIRTY | grep $GIT_DIRTY) && exit 0 # Already up-to-date
echo "#define REDIS_GIT_SHA1 \"$GIT_SHA1\"" > release.h
echo "#define REDIS_GIT_DIRTY \"$GIT_DIRTY\"" >> release.h
echo "#define REDIS_BUILD_ID \"$BUILD_ID\"" >> release.h
echo "#include \"version.h\"" >> release.h
echo "#define REDIS_BUILD_ID_RAW REDIS_VERSION REDIS_BUILD_ID REDIS_GIT_DIRTY REDIS_GIT_SHA1" >> release.h
touch release.c # Force recompile of release.c
