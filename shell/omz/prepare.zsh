#!/usr/bin/env zsh
# BB10's bundled tar mishandles deferred symlinks; recreate the pinned map.
emulate -LR zsh
setopt ERR_EXIT NO_UNSET
bundle=${1:?}
[[ -d $bundle && -r $bundle/symlinks.tsv ]] || exit 2
while IFS=$'\t' read -r name target; do
  [[ $name != /* && $name != ../* && $name != */../* && $target != /* ]] || exit 3
  [[ ! -d $bundle/$name || -L $bundle/$name ]] || exit 3
  rm -f "$bundle/$name"
  ln -s "$target" "$bundle/$name"
done < "$bundle/symlinks.tsv"
print RUNTIME_SYMLINKS_READY
